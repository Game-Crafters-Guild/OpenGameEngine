#include "UI/UITextureRegistry.h"
#include "Core/Application.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Text/FontAtlas.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <set>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
void SetEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

struct UITextureRegistryFixture : public ::testing::Test
{
    std::unique_ptr<IDevice> device;

    void SetUp() override
    {
        SetEnv("GE_HEADLESS_TEST", "1");

        DeviceDesc dd{};
        dd.preferredAPI = GraphicsAPI::Vulkan;
        dd.enableDynamicRendering = true;
        device = DeviceFactory::CreateDevice(dd);
        if (!device || !device->Initialize(dd))
        {
            GTEST_SKIP() << "Vulkan device init failed (no Vulkan runtime / CI machine)";
        }
    }
};

std::vector<TextureHandle> CreatePixelTextures(IDevice* device, size_t count)
{
    std::vector<TextureHandle> textures;
    textures.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        TextureDesc td{};
        td.width = 1;
        td.height = 1;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource)
                 | static_cast<uint32_t>(TextureUsage::TransferDst);
        td.debugName = "UITexRegTest_Pixel";
        auto tex = device->CreateTexture(td);
        if (tex.IsValid())
            textures.push_back(tex);
    }
    return textures;
}

void DestroyTextures(IDevice* device, std::vector<TextureHandle>& textures)
{
    for (auto& t : textures)
    {
        if (t.IsValid())
            device->DestroyTexture(t);
    }
    textures.clear();
}
} // namespace

// Slot-recycle invariant (post-Phase 2.12): Register → Unregister → Register for
// the same texture handle must immediately return a valid index. The previous
// implementation parked freed slots until the frame drained, which would have
// made this return 0 on the second Register.
TEST_F(UITextureRegistryFixture, SlotRecycleIsImmediate)
{
    UI::UITextureRegistry registry(device.get());

    auto textures = CreatePixelTextures(device.get(), 1);
    ASSERT_EQ(textures.size(), 1u);

    uint32_t idx1 = registry.Register(textures[0]);
    ASSERT_NE(idx1, 0u);

    registry.Unregister(idx1);
    uint32_t idx2 = registry.Register(textures[0]);
    EXPECT_NE(idx2, 0u);

    // A second Unregister → Register with a different texture must also succeed
    // without starving the free pool.
    registry.Unregister(idx2);
    auto more = CreatePixelTextures(device.get(), 1);
    ASSERT_EQ(more.size(), 1u);
    uint32_t idx3 = registry.Register(more[0]);
    EXPECT_NE(idx3, 0u);

    registry.Shutdown();
    DestroyTextures(device.get(), textures);
    DestroyTextures(device.get(), more);
}

// The white-scene-view regression class (cddaf12f2): UnregisterByHandle's
// reverse lookup compared a truncated map key against the FULL 64-bit
// generational id and silently never matched, so the slot was never freed.
// Pin the observable contract: unregister-by-handle recycles the slot
// (LIFO, same semantic the SlotRecycleIsImmediate pin uses).
TEST_F(UITextureRegistryFixture, UnregisterByHandleFreesTheSlot)
{
    UI::UITextureRegistry registry(device.get());

    auto textures = CreatePixelTextures(device.get(), 2);
    ASSERT_EQ(textures.size(), 2u);

    const uint32_t slotA = registry.Register(textures[0]);
    ASSERT_NE(slotA, 0u);

    registry.UnregisterByHandle(textures[0]);

    // The freed slot must be recycled for the next registration.
    const uint32_t slotB = registry.Register(textures[1]);
    EXPECT_EQ(slotB, slotA) << "UnregisterByHandle must actually free the slot";

    registry.Shutdown();
    DestroyTextures(device.get(), textures);
}

// The other half of the same regression: the dedup key truncated the
// generational handle's high bits, so a pool-recycled texture (same index,
// bumped generation) dedup-hit its DEAD predecessor's slot and the UI
// sampled a destroyed texture. A generation-bumped handle must never reuse
// the dead handle's registration.
TEST_F(UITextureRegistryFixture, GenerationRecycledHandleDoesNotAliasDeadPredecessor)
{
    UI::UITextureRegistry registry(device.get());

    auto first = CreatePixelTextures(device.get(), 1);
    ASSERT_EQ(first.size(), 1u);
    const TextureHandle dead = first[0];
    const uint32_t slotDead = registry.Register(dead);
    ASSERT_NE(slotDead, 0u);

    // Destroy WITHOUT unregistering — the real-world shape (the pool aged the
    // texture out; nobody told the registry).
    DestroyTextures(device.get(), first);

    // Re-create until the device recycles the dead handle's index with a new
    // generation. The destroy is timeline-deferred, so tick device frames to
    // retire it before each probe.
    TextureHandle recycled{};
    std::vector<TextureHandle> probes;
    for (int i = 0; i < 8 && !recycled.IsValid(); ++i)
    {
        device->WaitForIdle();
        device->BeginFrame();
        auto next = CreatePixelTextures(device.get(), 1);
        ASSERT_EQ(next.size(), 1u);
        probes.push_back(next[0]);
        if (static_cast<uint32_t>(next[0].id) == static_cast<uint32_t>(dead.id) &&
            next[0].id != dead.id)
            recycled = next[0];
    }
    if (!recycled.IsValid())
    {
        registry.Shutdown();
        DestroyTextures(device.get(), probes);
        GTEST_SKIP() << "device did not recycle the texture index; aliasing shape not reachable";
    }

    // The truncated key would dedup-hit slotDead here (still bound to the
    // destroyed texture); the full-id key must mint a fresh registration.
    const uint32_t slotRecycled = registry.Register(recycled);
    ASSERT_NE(slotRecycled, 0u);
    EXPECT_NE(slotRecycled, slotDead)
        << "a generation-bumped handle dedup-hit its dead predecessor's slot";

    registry.Shutdown();
    DestroyTextures(device.get(), probes);
}

// Threaded stress: many workers call Register/Unregister/UpdateSlot concurrently
// while the main thread materialises transient frame sets. Purpose is to exercise
// the m_Mutex guarding the state tables and descriptor writes. Any crash or
// invalid handle return indicates a correctness regression.
TEST_F(UITextureRegistryFixture, ThreadedRegisterUnregisterStress)
{
    UI::UITextureRegistry registry(device.get());

    constexpr size_t kTexturePoolSize = 64;
    constexpr int kWorkerThreads = 6;
    constexpr int kOpsPerWorker = 200;

    auto textures = CreatePixelTextures(device.get(), kTexturePoolSize);
    ASSERT_EQ(textures.size(), kTexturePoolSize);

    std::atomic<bool> stop{false};
    std::atomic<int> crashes{0};
    std::atomic<int> invalidHandles{0};

    std::vector<std::thread> workers;
    workers.reserve(kWorkerThreads);
    for (int w = 0; w < kWorkerThreads; ++w)
    {
        workers.emplace_back([&, w]() {
            std::mt19937 rng(static_cast<uint32_t>(0xC0FFEEu ^ w));
            std::uniform_int_distribution<int> opDist(0, 2);
            std::uniform_int_distribution<int> texDist(0, static_cast<int>(kTexturePoolSize) - 1);

            std::vector<uint32_t> ownedSlots;
            ownedSlots.reserve(32);

            for (int i = 0; i < kOpsPerWorker; ++i)
            {
                int op = opDist(rng);
                try
                {
                    if (op == 0)
                    {
                        uint32_t slot = registry.Register(textures[texDist(rng)]);
                        if (slot == 0)
                            ++invalidHandles;
                        else
                            ownedSlots.push_back(slot);
                    }
                    else if (op == 1 && !ownedSlots.empty())
                    {
                        size_t idx = rng() % ownedSlots.size();
                        registry.Unregister(ownedSlots[idx]);
                        ownedSlots.erase(ownedSlots.begin() + idx);
                    }
                    else if (op == 2 && !ownedSlots.empty())
                    {
                        size_t idx = rng() % ownedSlots.size();
                        registry.UpdateSlot(ownedSlots[idx], textures[texDist(rng)]);
                    }
                }
                catch (...)
                {
                    ++crashes;
                }
            }

            for (uint32_t slot : ownedSlots)
                registry.Unregister(slot);
        });
    }

    // Main thread: materialise transient frame sets concurrently with workers.
    // Mirrors the real per-draw pattern and exercises the map-iteration path
    // that must not race with writers.
    int framesCreated = 0;
    while (framesCreated < 100)
    {
        auto set = registry.CreateTransientFrameSet();
        // Set may be empty if the device's transient-pool is not populated in the
        // headless path; what we care about is that the call did not crash and
        // didn't observe a torn map.
        (void)set;
        ++framesCreated;
        std::this_thread::yield();
    }

    for (auto& t : workers)
        t.join();
    stop.store(true);

    EXPECT_EQ(crashes.load(), 0);
    EXPECT_EQ(invalidHandles.load(), 0)
        << "Register returned 0 — either pool exhausted or concurrency bug";

    registry.Shutdown();
    DestroyTextures(device.get(), textures);
}

// Repeat the threaded stress to catch flakes that surface only every N runs.
// 50 iterations is enough to make a 2%-failure race almost certain to fire.
TEST_F(UITextureRegistryFixture, ThreadedStressRepeated)
{
    constexpr int kIterations = 50;
    constexpr size_t kTexturePoolSize = 32;
    constexpr int kWorkerThreads = 4;
    constexpr int kOpsPerWorker = 100;

    for (int iter = 0; iter < kIterations; ++iter)
    {
        UI::UITextureRegistry registry(device.get());
        auto textures = CreatePixelTextures(device.get(), kTexturePoolSize);
        // A shortfall is device-resource exhaustion (shared machine-wide Vulkan
        // ceiling), not a registry defect — skip loudly, same policy as SetUp's
        // device-init skip.
        if (textures.size() < kTexturePoolSize)
            GTEST_SKIP() << "device supplied " << textures.size() << "/"
                         << kTexturePoolSize << " textures at iter " << iter;

        std::atomic<int> crashes{0};
        std::vector<std::thread> workers;
        workers.reserve(kWorkerThreads);

        for (int w = 0; w < kWorkerThreads; ++w)
        {
            workers.emplace_back([&, w, iter]() {
                std::mt19937 rng(static_cast<uint32_t>(0xBADC0DEu ^ (iter << 8) ^ w));
                std::uniform_int_distribution<int> opDist(0, 2);
                std::uniform_int_distribution<int> texDist(0, static_cast<int>(kTexturePoolSize) - 1);

                std::vector<uint32_t> ownedSlots;
                for (int i = 0; i < kOpsPerWorker; ++i)
                {
                    int op = opDist(rng);
                    try
                    {
                        if (op == 0)
                        {
                            uint32_t s = registry.Register(textures[texDist(rng)]);
                            if (s != 0) ownedSlots.push_back(s);
                        }
                        else if (op == 1 && !ownedSlots.empty())
                        {
                            size_t idx = rng() % ownedSlots.size();
                            registry.Unregister(ownedSlots[idx]);
                            ownedSlots.erase(ownedSlots.begin() + idx);
                        }
                        else if (op == 2 && !ownedSlots.empty())
                        {
                            size_t idx = rng() % ownedSlots.size();
                            registry.UpdateSlot(ownedSlots[idx], textures[texDist(rng)]);
                        }
                    }
                    catch (...)
                    {
                        ++crashes;
                    }
                }

                for (uint32_t slot : ownedSlots)
                    registry.Unregister(slot);
            });
        }

        for (int k = 0; k < 20; ++k)
        {
            (void)registry.CreateTransientFrameSet();
            std::this_thread::yield();
        }

        for (auto& t : workers)
            t.join();

        EXPECT_EQ(crashes.load(), 0) << "iter " << iter;

        registry.Shutdown();
        DestroyTextures(device.get(), textures);
    }
}

// --- Binding 1 (Slug band textures) slot discipline ---
//
// Binding 1 is a PARTIALLY_BOUND utexture2D array materialised into transient
// descriptor memory that is bump-allocated and never zeroed, and the fragment
// shader turns a band record's .x into a loop trip count. A slot the shader can
// index but the registry never wrote therefore does not read as "empty" — it
// reads as whatever descriptor last occupied those bytes, and an arbitrary
// integer becomes an unbounded fragment loop. Slot 0 is reserved for a
// zero-filled integer dummy so every indexable slot resolves to a live,
// correctly-typed image whose band records decode to zero curves.

namespace
{
// Load the Roboto staged next to the test executable (see UITests' POST_BUILD
// copy) and shape a string through it, which is what drives Slug glyph packing
// and page creation. Returns false when the font is not staged.
bool LoadShapedSlugAtlas(Rendering::Text::FontAtlas& atlas, unsigned pixelSize)
{
    const std::filesystem::path fontPath =
        PathUtils::GetExecutableDirectory() / "Assets" / "Fonts" / "Roboto-Regular.ttf";
    std::error_code ec;
    if (!std::filesystem::exists(fontPath, ec))
        return false;

    std::vector<unsigned char> bytes;
    {
        FILE* f = std::fopen(fontPath.string().c_str(), "rb");
        if (!f)
            return false;
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0)
        {
            bytes.resize(static_cast<size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
                bytes.clear();
        }
        std::fclose(f);
    }
    if (bytes.empty())
        return false;

    if (!atlas.LoadFontBytes(bytes.data(), bytes.size(), pixelSize))
        return false;

    Rendering::Text::FontAtlas::ShapeResult shaped;
    atlas.ShapeText("Slug band slots", static_cast<float>(pixelSize), shaped);
    return atlas.GetSlugPageCount() > 0;
}
} // namespace

// Band slot 0 belongs to the dummy, so a real font page must never be handed
// it: a page parked on slot 0 would be silently overwritten by the fallback and
// render blank. Distinct pages must also never share a slot.
TEST_F(UITextureRegistryFixture, BandSlotZeroIsReservedForTheDummy)
{
    UI::UITextureRegistry registry(device.get());

    Rendering::Text::FontAtlas atlas;
    if (!LoadShapedSlugAtlas(atlas, 32))
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    const auto& indices = registry.RegisterSlugTextures(atlas);
    ASSERT_FALSE(indices.empty());

    std::set<uint32_t> seen;
    for (const auto& idx : indices)
    {
        EXPECT_NE(idx.BandTexIdx, 0u)
            << "a real Slug page was assigned the reserved dummy band slot";
        EXPECT_LT(idx.BandTexIdx, UI::UITextureRegistry::kDefaultMaxBandTextures);
        EXPECT_TRUE(seen.insert(idx.BandTexIdx).second)
            << "two Slug pages share band slot " << idx.BandTexIdx;
    }

    registry.Shutdown();
}

// Exhausting the band pool must degrade to the dummy slot, not to an index one
// past the end of the descriptor array. The old allocator returned
// m_MaxBandTextures on overflow and RegisterSlugTextures published it straight
// into the indices the shader dereferences.
TEST_F(UITextureRegistryFixture, ExhaustedBandPoolFallsBackToTheDummySlot)
{
    UI::UITextureRegistry registry(device.get());

    // Atlas identity is derived from the font bytes plus the pixel size, so the
    // same face at distinct sizes yields distinct atlases and distinct pages.
    // A few past kDefaultMaxBandTextures is enough to overflow: the device's
    // resolved ceiling is at most the policy one, never above it. Each atlas
    // costs a 4096x64 curve + band texture pair, so overshooting is not free.
    constexpr unsigned kAtlasCount = UI::UITextureRegistry::kDefaultMaxBandTextures + 4;
    std::vector<std::unique_ptr<Rendering::Text::FontAtlas>> atlases;
    bool sawFallback = false;
    bool anyLoaded = false;

    for (unsigned i = 0; i < kAtlasCount; ++i)
    {
        auto atlas = std::make_unique<Rendering::Text::FontAtlas>();
        if (!LoadShapedSlugAtlas(*atlas, 12 + i))
            break;
        anyLoaded = true;

        const auto& indices = registry.RegisterSlugTextures(*atlas);
        for (const auto& idx : indices)
        {
            // The invariant that actually protects the shader: never an index
            // outside the array.
            ASSERT_LT(idx.BandTexIdx, UI::UITextureRegistry::kDefaultMaxBandTextures)
                << "band slot index escaped the descriptor array at atlas " << i;
            if (idx.BandTexIdx == 0u)
                sawFallback = true;
        }
        atlases.push_back(std::move(atlas));
    }

    if (!anyLoaded)
        GTEST_SKIP() << "Staged Roboto-Regular.ttf not found next to the test executable";

    EXPECT_TRUE(sawFallback)
        << "registered " << atlases.size() << " atlases without exhausting the band pool; "
        << "the overflow path never ran";

    registry.Shutdown();
}

// The transient set writes every band slot, including those no page occupies.
// Materialising a set straight after construction is the minimal exercise of
// that loop: it must stay valid when only the dummy is populated.
TEST_F(UITextureRegistryFixture, TransientSetMaterialisesWithOnlyTheBandDummyPopulated)
{
    UI::UITextureRegistry registry(device.get());

    DescriptorSetHandle set = registry.CreateTransientFrameSet();
    EXPECT_TRUE(set.IsValid());

    registry.Shutdown();
}
