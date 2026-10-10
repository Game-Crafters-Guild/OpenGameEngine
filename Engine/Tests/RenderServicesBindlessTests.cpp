#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/BindlessResourceManager.h"

#include "EngineLogCapture.h"
#include "ScopedStdStreamCapture.h"
#include "TestDeviceHelper.h"

#include "Logger/LogLevel.h"
#include "Logger/Logger.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;

class RenderServicesBindlessTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_device = CreateVulkanDeviceFast();
        if (!m_device)
            GTEST_SKIP() << "No Vulkan device available";
        ASSERT_TRUE(m_rs.Initialize(m_device.get()));
    }

    void TearDown() override
    {
        m_rs.Shutdown();
        if (m_device)
            m_device->Shutdown();
    }

    // Create a minimal 1x1 RGBA8 texture for testing.
    TextureHandle CreateTestTexture(const char* name = "TestTex")
    {
        TextureDesc td{};
        td.width = 1;
        td.height = 1;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
        td.debugName = name;
        return m_device->CreateTexture(td);
    }

    std::unique_ptr<IDevice> m_device;
    RenderServices m_rs;
};

// --- GetSampler tests ---

TEST_F(RenderServicesBindlessTest, GetSampler_AllPresets_ReturnValid)
{
    EXPECT_TRUE(m_rs.Textures().GetSampler(SamplerPreset::LinearRepeat).IsValid());
    EXPECT_TRUE(m_rs.Textures().GetSampler(SamplerPreset::LinearClamp).IsValid());
    EXPECT_TRUE(m_rs.Textures().GetSampler(SamplerPreset::PointClamp).IsValid());
    EXPECT_TRUE(m_rs.Textures().GetSampler(SamplerPreset::PointRepeat).IsValid());
}

TEST_F(RenderServicesBindlessTest, GetSampler_DistinctPresetsReturnDifferentHandles)
{
    auto repeat = m_rs.Textures().GetSampler(SamplerPreset::LinearRepeat);
    auto clamp = m_rs.Textures().GetSampler(SamplerPreset::LinearClamp);
    auto point = m_rs.Textures().GetSampler(SamplerPreset::PointClamp);
    EXPECT_NE(repeat, clamp);
    EXPECT_NE(clamp, point);
}

TEST_F(RenderServicesBindlessTest, BindlessTextureLayoutMatchesDeviceCapacityAndPoolPath)
{
    const uint32_t capacity = std::max<uint32_t>(1u, m_device->GetCapabilities().maxBindlessTextures);
    const auto layout = TextureService::GetBindlessTextureSetLayout(capacity);

    // Split layout: a SAMPLED_IMAGE texture array (binding 0) + a shared sampler array (binding 1).
    ASSERT_EQ(layout.bindings.size(), 2u);
    ASSERT_NE(layout.debugName, nullptr);
    EXPECT_STREQ(layout.debugName, "BindlessTextureSetLayout");

    const auto& textureArray = layout.bindings[0];
    EXPECT_EQ(textureArray.binding, 0u);
    EXPECT_EQ(textureArray.type, DescriptorType::Texture);
    EXPECT_EQ(textureArray.count, capacity);
    // Compute included since 50f9fb9e7: terrain-grass compute passes sample the
    // bindless array (heightmap/splat lookups) directly from CS.
    EXPECT_EQ(textureArray.shaderStages, kShaderStageVertex | kShaderStageFragment | kShaderStageCompute);
    // UPDATE_AFTER_BIND is what routes this layout onto the pool path inside
    // VulkanDevice::CreateVulkanDescriptorSetLayout, regardless of whether the
    // device is otherwise descriptor-buffer-enabled.
    EXPECT_NE(textureArray.flags & kDescriptorBindingUpdateAfterBind, 0u);
    EXPECT_NE(textureArray.flags & kDescriptorBindingPartiallyBound, 0u);

    const auto& samplerArray = layout.bindings[1];
    EXPECT_EQ(samplerArray.binding, 1u);
    EXPECT_EQ(samplerArray.type, DescriptorType::Sampler);
    EXPECT_EQ(samplerArray.count, static_cast<uint32_t>(SamplerPreset::kCount));
    EXPECT_EQ(samplerArray.shaderStages, kShaderStageVertex | kShaderStageFragment | kShaderStageCompute);
}

// --- GetBindlessIndex tests ---

TEST_F(RenderServicesBindlessTest, GetBindlessIndex_InvalidTexture_ReturnsZero)
{
    EXPECT_EQ(m_rs.Textures().GetBindlessIndex(TextureHandle{}), 0u);
}

// --- Advanced overload (BindlessTextureDesc) ---

// A single-level view and a full-mip-chain view of the SAME slice are different
// resources: texelFetch(tex, coord, lod) with lod > 0 reads nothing through the
// former. mipCount therefore has to reach both the created view and the cache
// key — a key that omitted it would alias the two onto whichever registered
// first, and the loser would silently sample the wrong shape.
TEST_F(RenderServicesBindlessTest, GetBindlessIndex_MipCountSelectsADistinctView)
{
    TextureDesc td{};
    td.width = 8;
    td.height = 8;
    td.mipLevels = 4;
    td.arrayLayers = 2;
    td.format = static_cast<uint32_t>(TextureFormat::R32G32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    td.debugName = "MippedArrayTex";
    auto tex = m_device->CreateTexture(td);
    ASSERT_TRUE(tex.IsValid());

    BindlessTextureDesc single{};
    single.textureHandle = tex;
    single.type = BindlessResourceType::Texture2D;
    single.arraySlice = 1;
    single.mipCount = 1;

    BindlessTextureDesc chain = single;
    chain.mipCount = 0; // every remaining level

    const uint32_t singleIdx = m_rs.Textures().GetBindlessIndex(single);
    const uint32_t chainIdx = m_rs.Textures().GetBindlessIndex(chain);
    EXPECT_NE(singleIdx, 0u);
    EXPECT_NE(chainIdx, 0u);
    EXPECT_NE(singleIdx, chainIdx) << "mipCount must be part of the bindless cache key";

    // Each request is still cached: re-asking returns the same slot rather than
    // burning a descriptor per frame.
    EXPECT_EQ(m_rs.Textures().GetBindlessIndex(single), singleIdx);
    EXPECT_EQ(m_rs.Textures().GetBindlessIndex(chain), chainIdx);

    m_device->DestroyTexture(tex);
}

// --- InvalidateBindlessTexture tests ---

TEST_F(RenderServicesBindlessTest, InvalidateBindlessTexture_DoesNotAffectOtherTextures)
{
    auto texA = CreateTestTexture("TexA");
    auto texB = CreateTestTexture("TexB");

    uint32_t idxA = m_rs.Textures().GetBindlessIndex(texA);
    uint32_t idxB = m_rs.Textures().GetBindlessIndex(texB);
    EXPECT_NE(idxA, 0u);
    EXPECT_NE(idxB, 0u);

    // Invalidating A should not affect B's cached index.
    m_rs.Textures().InvalidateBindless(texA);
    EXPECT_EQ(m_rs.Textures().GetBindlessIndex(texB), idxB);

    m_device->DestroyTexture(texA);
    m_device->DestroyTexture(texB);
}

// --- DiscardTextureDeferred (worker-safe retirement of caller-owned textures) ---

TEST_F(RenderServicesBindlessTest, DiscardTextureDeferred_EvictsCacheEntry)
{
    auto tex = CreateTestTexture("DiscardDeferred");
    const uint32_t idx = m_rs.Textures().GetBindlessIndex(tex);
    ASSERT_NE(idx, 0u);

    // The cache is keyed on the handle .id, which the device reissues: a stale
    // entry would serve a recycled id the WRONG descriptor. Discarding must
    // evict the entry, not merely park the texture for destruction.
    m_rs.Textures().DiscardTextureDeferred(tex);

    // Re-registering proves the entry was evicted: the discarded slot is only
    // released by the drain below, so a fresh registration cannot reuse it.
    const uint32_t reregistered = m_rs.Textures().GetBindlessIndex(tex);
    EXPECT_NE(reregistered, 0u);
    EXPECT_NE(reregistered, idx);

    // Drop the re-registration, then drain the parked slot + texture on the
    // render thread (this one). No DestroyTexture here: the drain owns it.
    m_rs.Textures().InvalidateBindless(tex);
    m_rs.Textures().FlushPendingUploads();
}

TEST_F(RenderServicesBindlessTest, DiscardTextureDeferred_DoesNotAffectOtherTextures)
{
    auto texA = CreateTestTexture("DiscardA");
    auto texB = CreateTestTexture("DiscardB");

    const uint32_t idxB = m_rs.Textures().GetBindlessIndex(texB);
    ASSERT_NE(m_rs.Textures().GetBindlessIndex(texA), 0u);
    ASSERT_NE(idxB, 0u);

    m_rs.Textures().DiscardTextureDeferred(texA);
    EXPECT_EQ(m_rs.Textures().GetBindlessIndex(texB), idxB);

    m_rs.Textures().FlushPendingUploads();
    m_device->DestroyTexture(texB);
}

// --- Concurrent registration ---
//
// GetBindlessIndex is reached from ECS extraction workers: the extraction wave
// levelizes TerrainExtraction and RenderExtraction together, and SystemManager
// dispatches a multi-system wave onto the JobSystem (ECS/Systems.h:314/322). Two
// workers therefore register concurrently, and the losing registration must be
// retired on the render thread rather than from the worker that lost.

// Every racer on one key must observe the single winning index — the loser's
// slot is never handed out.
TEST_F(RenderServicesBindlessTest, GetBindlessIndex_ConcurrentSameKey_AllRacersAgree)
{
    auto tex = CreateTestTexture("RaceSameKey");
    ASSERT_TRUE(tex.IsValid());

    constexpr int kThreads = 8;
    std::vector<uint32_t> results(kThreads, 0u);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            results[i] = m_rs.Textures().GetBindlessIndex(tex);
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& t : threads)
        t.join();

    EXPECT_NE(results[0], 0u);
    for (int i = 1; i < kThreads; ++i)
        EXPECT_EQ(results[i], results[0]) << "racer " << i << " saw a different slot";

    // Retiring the losers is render-thread work; it must not disturb the winner.
    m_rs.Textures().FlushPendingUploads();
    EXPECT_EQ(m_rs.Textures().GetBindlessIndex(tex), results[0]);

    m_device->DestroyTexture(tex);
}

// Distinct keys registered concurrently must each get their own slot: the race
// handling must not collapse unrelated registrations onto one index.
TEST_F(RenderServicesBindlessTest, GetBindlessIndex_ConcurrentDistinctKeys_AllUnique)
{
    constexpr int kThreads = 8;
    std::vector<TextureHandle> textures(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        textures[i] = CreateTestTexture("RaceDistinct");
        ASSERT_TRUE(textures[i].IsValid());
    }

    std::vector<uint32_t> results(kThreads, 0u);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            results[i] = m_rs.Textures().GetBindlessIndex(textures[i]);
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& t : threads)
        t.join();

    std::set<uint32_t> unique;
    for (int i = 0; i < kThreads; ++i)
    {
        EXPECT_NE(results[i], 0u) << "racer " << i << " failed to register";
        unique.insert(results[i]);
    }
    EXPECT_EQ(unique.size(), static_cast<size_t>(kThreads));

    m_rs.Textures().FlushPendingUploads();
    for (int i = 0; i < kThreads; ++i)
        m_device->DestroyTexture(textures[i]);
}

// A registration that needs a subresource view (non-zero array slice / mip, or a
// non-Color aspect) calls IDevice::CreateTextureView from whichever thread got
// there — so the device's view-tracking containers, not just the cache, are
// contended. The oracle is the device's own consistency invariant: the per-view
// metadata maps must stay exactly three entries per live view.
TEST_F(RenderServicesBindlessTest, GetBindlessIndex_ConcurrentSubresourceViews_TrackedConsistently)
{
    constexpr int kThreads = 8;
    constexpr int kPerThread = 12;

    // 2-layer textures so arraySlice 1 forces the subresource-view path.
    std::vector<TextureHandle> textures(kThreads);
    for (int i = 0; i < kThreads; ++i)
    {
        TextureDesc td{};
        td.width = 4;
        td.height = 4;
        td.arrayLayers = kPerThread;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
        td.debugName = "ConcurrentSubresourceTex";
        textures[i] = m_device->CreateTexture(td);
        ASSERT_TRUE(textures[i].IsValid());
    }

    const size_t viewsBefore = m_device->GetResourcePoolStats().liveTextureViews;

    std::vector<uint32_t> results(static_cast<size_t>(kThreads) * kPerThread, 0u);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kPerThread; ++i)
            {
                BindlessTextureDesc desc{};
                desc.textureHandle = textures[t];
                desc.type = BindlessResourceType::Texture2D;
                desc.arraySlice = static_cast<uint32_t>(i);
                results[static_cast<size_t>(t) * kPerThread + i] =
                    m_rs.Textures().GetBindlessIndex(desc);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& t : threads)
        t.join();

    std::set<uint32_t> unique;
    for (size_t i = 0; i < results.size(); ++i)
    {
        EXPECT_NE(results[i], 0u) << "registration " << i << " failed";
        unique.insert(results[i]);
    }
    EXPECT_EQ(unique.size(), results.size()) << "distinct subresources collapsed onto shared slots";

    // Every slice but slice 0 needed its own view; slice 0 takes the whole-texture
    // path unless the format forces a swizzle, so assert the invariant rather than
    // an exact count.
    const auto stats = m_device->GetResourcePoolStats();
    EXPECT_GT(stats.liveTextureViews, viewsBefore) << "no subresource view was created";
    EXPECT_EQ(stats.textureViewMetadataEntries, stats.liveTextureViews * 3)
        << "per-view metadata is out of step with the live-view list";

    // The cache must still resolve every key to the same slot after the race.
    for (int t = 0; t < kThreads; ++t)
    {
        for (int i = 0; i < kPerThread; ++i)
        {
            BindlessTextureDesc desc{};
            desc.textureHandle = textures[t];
            desc.type = BindlessResourceType::Texture2D;
            desc.arraySlice = static_cast<uint32_t>(i);
            EXPECT_EQ(m_rs.Textures().GetBindlessIndex(desc),
                      results[static_cast<size_t>(t) * kPerThread + i]);
        }
    }

    m_rs.Textures().FlushPendingUploads();
    for (auto t : textures)
        m_device->DestroyTexture(t);
}

// A manager is built per device and this suite builds a device per test, so the manager's
// lifecycle chatter is emitted once per test. It has to go through the logger: output
// written straight to stdout is a block of lines no log level can turn off. The
// provisioned capacity is clamped to the same device capability for every device in a
// process, so one manager reporting it at Info says everything the reader needs and the
// rest belong at Debug.
//
// The manager needs no device to construct, initialize or shut down, so both phases hold
// wherever this test runs in the binary and whatever else has already built one. Whether
// the one Info report is still unspent by then is not: that property has its own binary,
// in BindlessResourceManagerDiagnosticsTests.
TEST(BindlessResourceManagerLogging, ReportsCapacityThroughTheLoggerAndRepeatsAtDebug)
{
    constexpr int kManagers = 3;
    constexpr uint32_t kTextures = 64;
    constexpr uint32_t kBuffers = 64;
    constexpr const char* kCapacityLine = "BindlessResourceManager: ready with";

    auto buildManagers = [](int count) {
        for (int i = 0; i < count; ++i)
        {
            BindlessResourceManager manager(nullptr);
            EXPECT_TRUE(manager.Initialize(kTextures, kBuffers));
            manager.Shutdown();
        }
        Logger::Log::Flush();
    };

    // Every manager reports its capacity through the logger, and nothing reaches stdout.
    std::string consoleOutput;
    std::vector<std::string> debugLines;
    {
        GameEngine::TestLog::ScopedStdStreamCapture streams;
        {
            GameEngine::TestLog::ScopedEngineLogCapture capture(&debugLines, Logger::LogLevel::Debug);
            // A capture that sees nothing is indistinguishable from a line that was never
            // emitted: prove the sink live before reading a zero out of it.
            Logger::Log::Debug("RenderServicesBindlessTests: capture sink live");
            buildManagers(kManagers);
        }
        consoleOutput = streams.Text();
    }

    EXPECT_EQ(std::string::npos, consoleOutput.find("Bindless"))
        << "the manager wrote to stdout or stderr, where no log level reaches it:\n"
        << consoleOutput;
    EXPECT_EQ(static_cast<size_t>(kManagers),
              GameEngine::TestLog::CountLinesContaining(debugLines, kCapacityLine));

    // The once-per-process capacity report is spent by now, either by the phase above or
    // by an earlier device in this binary, so these reach Debug only and an Info reader
    // sees none of them.
    std::vector<std::string> infoLines;
    {
        GameEngine::TestLog::ScopedEngineLogCapture capture(&infoLines, Logger::LogLevel::Info);
        Logger::Log::Info("RenderServicesBindlessTests: capture sink live");
        buildManagers(kManagers);
    }

    EXPECT_EQ(0u, GameEngine::TestLog::CountLinesContaining(infoLines, kCapacityLine));
    EXPECT_EQ(1u, GameEngine::TestLog::CountLinesContaining(infoLines, "capture sink live"))
        << "the Info capture was not live, so the zero above proves nothing";
}
