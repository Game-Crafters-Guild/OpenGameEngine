// Texture-view creation is concurrent by contract.
//
// TextureService::RegisterTextureBindless calls IDevice::CreateTextureView from
// ECS extraction workers (the extraction wave levelizes TerrainExtraction and
// RenderExtraction together and SystemManager dispatches a multi-system wave onto
// the JobSystem), while the render thread resolves already-created views on the
// descriptor-write path. These tests pin that contract on the device's own
// view-tracking state through the public diagnostics: the per-view metadata maps
// must stay exactly in step with the live-view list, and handles handed out
// concurrently must be distinct.
//
// Oracles are checked after every worker has joined, i.e. while the device is
// quiescent, so a failure is a lost/duplicated container entry rather than a
// benign mid-flight observation.

#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{
void SetEnvVar(const char* key, const char* value)
{
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

std::unique_ptr<IDevice> MakeHeadlessDevice()
{
    SetEnvVar("GE_HEADLESS_TEST", "1");
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
    {
        return nullptr;
    }
    return dev;
}

TextureHandle MakeTexture(IDevice& dev, uint32_t mipLevels = 1, uint32_t arrayLayers = 1)
{
    TextureDesc td{};
    td.width = 4;
    td.height = 4;
    td.mipLevels = mipLevels;
    td.arrayLayers = arrayLayers;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    td.debugName = "ViewThreadingTex";
    return dev.CreateTexture(td);
}

// The three per-view metadata maps (aspect / format / owner) are written and
// erased together, so the device reports their combined entry count and it must
// always be three per live view.
void ExpectMetadataInStep(IDevice& dev, const char* where)
{
    const auto s = dev.GetResourcePoolStats();
    EXPECT_EQ(s.textureViewMetadataEntries, s.liveTextureViews * 3)
        << where << ": per-view metadata is out of step with the live-view list ("
        << s.textureViewMetadataEntries << " entries for " << s.liveTextureViews << " views)";
}

// Threads all block on `go` so the create calls genuinely overlap.
template <typename Fn>
void RunInParallel(int threadCount, Fn&& body)
{
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (int t = 0; t < threadCount; ++t)
    {
        threads.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            body(t);
        });
    }
    while (ready.load(std::memory_order_acquire) < threadCount)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& th : threads)
        th.join();
}

constexpr int kThreads = 8;
constexpr int kViewsPerThread = 48;

class TextureViewThreadingTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_dev = MakeHeadlessDevice();
        if (!m_dev)
            GTEST_SKIP() << "No Vulkan device available";
        m_baseline = m_dev->GetResourcePoolStats().liveTextureViews;
    }

    void TearDown() override
    {
        if (m_dev)
            m_dev->Shutdown();
    }

    std::unique_ptr<IDevice> m_dev;
    size_t m_baseline = 0;
};

// Distinct textures, one view each, created concurrently. A torn generational
// store hands the same index to two threads; a torn metadata map loses entries.
TEST_F(TextureViewThreadingTest, ConcurrentCreateOnDistinctTexturesKeepsHandlesAndMetadataIntact)
{
    std::vector<TextureHandle> textures(kThreads * kViewsPerThread);
    for (auto& t : textures)
    {
        t = MakeTexture(*m_dev);
        ASSERT_TRUE(t.IsValid());
    }

    std::vector<TextureViewHandle> views(textures.size());
    RunInParallel(kThreads, [&](int t) {
        for (int i = 0; i < kViewsPerThread; ++i)
        {
            const size_t idx = static_cast<size_t>(t) * kViewsPerThread + i;
            TextureViewDesc vd{};
            vd.viewType = TextureViewType::View2D;
            vd.debugName = "ConcurrentDistinct";
            views[idx] = m_dev->CreateTextureView(textures[idx], vd);
        }
    });

    std::set<uint64_t> unique;
    for (size_t i = 0; i < views.size(); ++i)
    {
        ASSERT_TRUE(views[i].IsValid()) << "view " << i << " failed to create";
        EXPECT_TRUE(unique.insert(views[i].id).second) << "view " << i << " duplicated handle id";
    }

    const auto s = m_dev->GetResourcePoolStats();
    EXPECT_EQ(s.liveTextureViews, m_baseline + views.size());
    ExpectMetadataInStep(*m_dev, "after concurrent create");

    for (auto v : views)
        m_dev->DestroyTextureView(v);
    for (auto t : textures)
        m_dev->DestroyTexture(t);
    m_dev->WaitForIdle();
    ExpectMetadataInStep(*m_dev, "after drain");
}

// Many views of ONE texture: every create hits the same VulkanTexture, so the
// owner map and the per-texture view refcount are contended on top of the
// generational store.
TEST_F(TextureViewThreadingTest, ConcurrentCreateOnSharedTextureTracksEveryView)
{
    TextureHandle tex = MakeTexture(*m_dev, /*mipLevels*/ 2, /*arrayLayers*/ 2);
    ASSERT_TRUE(tex.IsValid());

    std::vector<TextureViewHandle> views(static_cast<size_t>(kThreads) * kViewsPerThread);
    RunInParallel(kThreads, [&](int t) {
        for (int i = 0; i < kViewsPerThread; ++i)
        {
            TextureViewDesc vd{};
            vd.viewType = TextureViewType::View2D;
            vd.baseMip = static_cast<uint32_t>(i % 2);
            vd.levelCount = 1;
            vd.baseLayer = static_cast<uint32_t>(t % 2);
            vd.layerCount = 1;
            vd.debugName = "ConcurrentShared";
            views[static_cast<size_t>(t) * kViewsPerThread + i] = m_dev->CreateTextureView(tex, vd);
        }
    });

    std::set<uint64_t> unique;
    for (size_t i = 0; i < views.size(); ++i)
    {
        ASSERT_TRUE(views[i].IsValid()) << "view " << i << " failed to create";
        EXPECT_TRUE(unique.insert(views[i].id).second) << "view " << i << " duplicated handle id";
    }
    EXPECT_EQ(m_dev->GetResourcePoolStats().liveTextureViews, m_baseline + views.size());
    ExpectMetadataInStep(*m_dev, "after shared-texture create");

    for (auto v : views)
        m_dev->DestroyTextureView(v);
    m_dev->DestroyTexture(tex);
    m_dev->WaitForIdle();
    ExpectMetadataInStep(*m_dev, "after drain");
    EXPECT_EQ(m_dev->GetResourcePoolStats().liveTextureViews, m_baseline);
}

// Create/destroy churn from every thread. Destroys are deferred, so the drain at
// the end is what proves nothing was orphaned: every metadata entry must have
// found its way back out.
TEST_F(TextureViewThreadingTest, ConcurrentCreateDestroyChurnLeavesNothingOrphaned)
{
    TextureHandle tex = MakeTexture(*m_dev);
    ASSERT_TRUE(tex.IsValid());

    RunInParallel(kThreads, [&](int) {
        for (int i = 0; i < kViewsPerThread; ++i)
        {
            TextureViewDesc vd{};
            vd.viewType = TextureViewType::View2D;
            vd.debugName = "Churn";
            TextureViewHandle v = m_dev->CreateTextureView(tex, vd);
            if (v.IsValid())
                m_dev->DestroyTextureView(v);
        }
    });

    m_dev->WaitForIdle();
    EXPECT_EQ(m_dev->GetResourcePoolStats().liveTextureViews, m_baseline);
    ExpectMetadataInStep(*m_dev, "after churn drain");

    m_dev->DestroyTexture(tex);
    m_dev->WaitForIdle();
}

// Writers (CreateTextureView) racing readers (the descriptor path resolves the
// view handle, its aspect and its owner). This is the arm that catches a reader
// holding an interior pointer into the generational store while a writer grows
// it — the descriptor bytes are disjoint, the lookups around them are not.
TEST_F(TextureViewThreadingTest, ConcurrentCreateWhileDescriptorWritesResolveViews)
{
    TextureHandle tex = MakeTexture(*m_dev);
    ASSERT_TRUE(tex.IsValid());

    // Pre-create the views the readers resolve, and a set to write them into.
    constexpr int kReaderViews = 16;
    std::vector<TextureViewHandle> readable(kReaderViews);
    for (auto& v : readable)
    {
        TextureViewDesc vd{};
        vd.viewType = TextureViewType::View2D;
        vd.debugName = "ReaderView";
        v = m_dev->CreateTextureView(tex, vd);
        ASSERT_TRUE(v.IsValid());
    }

    DescriptorSetLayoutDesc layout{};
    DescriptorBinding binding{};
    binding.binding = 0;
    binding.type = DescriptorType::Texture;
    binding.count = kReaderViews;
    binding.shaderStages = kShaderStageFragment;
    binding.flags = kDescriptorBindingPartiallyBound | kDescriptorBindingUpdateAfterBind;
    layout.bindings.push_back(binding);
    layout.debugName = "ViewThreadingReaderLayout";

    DescriptorSetDesc dsd{};
    dsd.layout = layout;
    dsd.poolFlags = DescriptorPoolFlags::UpdateAfterBind;
    DescriptorSetHandle set = m_dev->CreateDescriptorSet(dsd);
    ASSERT_TRUE(set.IsValid());

    std::vector<TextureViewHandle> created(static_cast<size_t>(kThreads / 2) * kViewsPerThread);
    RunInParallel(kThreads, [&](int t) {
        if (t % 2 == 0)
        {
            // Writer: grows the generational store and the metadata maps.
            const int writerOrdinal = t / 2;
            for (int i = 0; i < kViewsPerThread; ++i)
            {
                TextureViewDesc vd{};
                vd.viewType = TextureViewType::View2D;
                vd.debugName = "WriterView";
                created[static_cast<size_t>(writerOrdinal) * kViewsPerThread + i] =
                    m_dev->CreateTextureView(tex, vd);
            }
        }
        else
        {
            // Reader: the descriptor write resolves view -> VkImageView, aspect and owner.
            for (int i = 0; i < kViewsPerThread; ++i)
                m_dev->UpdateImageBinding(set, 0, readable[i % kReaderViews],
                                          static_cast<uint32_t>(i % kReaderViews));
        }
    });

    for (size_t i = 0; i < created.size(); ++i)
        EXPECT_TRUE(created[i].IsValid()) << "writer view " << i << " failed to create";
    ExpectMetadataInStep(*m_dev, "after reader/writer overlap");

    m_dev->DestroyDescriptorSet(set);
    for (auto v : created)
        m_dev->DestroyTextureView(v);
    for (auto v : readable)
        m_dev->DestroyTextureView(v);
    m_dev->DestroyTexture(tex);
    m_dev->WaitForIdle();
    ExpectMetadataInStep(*m_dev, "after drain");
}

} // namespace
