// Classic (non-bindless) material indexing: a device without descriptor
// indexing must initialize into the compatibility path — defaults, samplers and
// LUTs created, bindless skipped — say which mode it took, and hand out
// per-material descriptor sets built by MaterialBindingCache.

#include <gtest/gtest.h>

#include "EngineLogCapture.h"

#include "AssetCore/GUID.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBindingCache.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/TextureService.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Types/StringId.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::Rendering;

namespace
{
// forceDisableBindlessResources is retained specifically so the Classic path
// can be exercised on a dev machine (which always reports bindless-capable).
std::unique_ptr<IDevice> CreateForceNoBindlessDevice()
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDebugLayer = false;
    dd.enableDynamicRendering = true;
    dd.forceDisableBindlessResources = true;

    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

// Owns a force-no-bindless device plus an initialized TextureService, or
// reports that this machine cannot host the fixture.
struct ClassicFixture
{
    std::unique_ptr<IDevice> Device;
    MaterialRegistry Registry;
    TextureService Textures;
    bool Ready = false;
    const char* SkipReason = nullptr;

    ClassicFixture()
    {
        Device = CreateForceNoBindlessDevice();
        if (!Device)
        {
            SkipReason = "No Vulkan device available";
            return;
        }
        if (Device->GetCapabilities().supportsBindlessResources)
        {
            SkipReason = "forceDisableBindlessResources not honored on this device";
            return;
        }
        Ready = Textures.Initialize(Device.get(), Registry,
                                    RendererProfile::FromCapabilities(Device->GetCapabilities()));
        if (!Ready)
            SkipReason = "TextureService::Initialize failed on the Classic path";
    }

    ~ClassicFixture()
    {
        if (Device)
        {
            Textures.Shutdown();
            Device->Shutdown();
        }
    }
};

Material MakeMaterial()
{
    return Material::TestFactory::Create(GUID::Generate(), "ClassicIndexingTest", 64);
}
} // namespace

TEST(TextureServiceClassicIndexing, NoBindlessDeviceInitializesIntoClassicMode)
{
    ClassicFixture fx;
    if (!fx.Ready)
        GTEST_SKIP() << fx.SkipReason;

    EXPECT_EQ(fx.Textures.GetMaterialIndexingMode(),
              TextureService::MaterialIndexingMode::Classic);
    EXPECT_FALSE(fx.Textures.IsBindlessEnabled());
    EXPECT_FALSE(fx.Textures.BindlessTextureSet().IsValid());

    // Both modes need the fallbacks and the shared sampler cache; only the
    // bindless manager and its global set are skipped.
    EXPECT_TRUE(fx.Textures.GetDefaultWhiteTexture().IsValid());
    EXPECT_TRUE(fx.Textures.GetDefaultBlackTexture().IsValid());
    EXPECT_TRUE(fx.Textures.GetDefaultFlatNormalTexture().IsValid());
    EXPECT_TRUE(fx.Textures.GetSampler(SamplerPreset::LinearRepeat).IsValid());
}

TEST(TextureServiceClassicIndexing, ClassicModeProducesAUsableMaterialSet)
{
    ClassicFixture fx;
    if (!fx.Ready)
        GTEST_SKIP() << fx.SkipReason;

    EXPECT_TRUE(fx.Textures.DefaultMaterialTextureSet().IsValid());

    Material mat = MakeMaterial();
    const auto set = fx.Textures.MaterialTextureSet(mat);
    EXPECT_TRUE(set.IsValid());
    // Cached: a second lookup must not allocate.
    EXPECT_EQ(fx.Textures.MaterialTextureSet(mat), set);

    // MaterialRegistry::Register asserts its slot defaults were seeded, so
    // Classic must seed them too even though it never reads the index lane.
    MaterialDocument doc{};
    doc.schemaVersion = 2;
    doc.materialName = "ClassicBootMaterial";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    EXPECT_NE(fx.Registry.Register(GUID::Generate(), doc), nullptr);
}

// GetSampler creates an uncached preset outside its lock, so racers that all miss the
// cache each create one, and all but the first park theirs for the render thread to
// destroy. Only Classic reaches that path: Bindless creates every preset at
// initialization. Every racer must get the one cached sampler, and the drain must
// destroy the duplicates and nothing else.
TEST(TextureServiceClassicIndexing, ConcurrentFirstUseOfASamplerPresetYieldsOneSampler)
{
    ClassicFixture fx;
    if (!fx.Ready)
        GTEST_SKIP() << fx.SkipReason;

    const size_t samplersBefore = fx.Device->GetResourcePoolStats().liveSamplers;
    constexpr int kThreads = 8;
    std::vector<SamplerHandle> results(kThreads);
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
            results[i] = fx.Textures.GetSampler(SamplerPreset::BilinearRepeat);
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& t : threads)
        t.join();

    ASSERT_TRUE(results[0].IsValid());
    for (int i = 1; i < kThreads; ++i)
        EXPECT_EQ(results[i], results[0]) << "racer " << i << " saw a different sampler";

    // Drain the parked duplicates, then let the device retire its deferred destroys.
    fx.Textures.FlushPendingUploads();
    fx.Device->WaitForIdle();
    fx.Device->BeginFrame();
    EXPECT_EQ(fx.Textures.GetSampler(SamplerPreset::BilinearRepeat), results[0]);
    EXPECT_EQ(fx.Device->GetResourcePoolStats().liveSamplers, samplersBefore + 1)
        << "one sampler must survive the race: the drain destroyed the cached one or leaked a duplicate";
}

TEST(TextureServiceClassicIndexing, MaterialSetLayoutMatchesTheCompatShaderBindings)
{
    const auto layout = MaterialBindingCache::GetSetLayout();
    ASSERT_EQ(layout.bindings.size(), kTextureSlotArraySize * 2u);

    for (uint32_t slot = 0; slot < kTextureSlotArraySize; ++slot)
    {
        const auto& tex = layout.bindings[slot];
        EXPECT_EQ(tex.binding, slot);
        EXPECT_EQ(tex.type, DescriptorType::Texture);
        EXPECT_EQ(tex.count, 1u);

        const auto& smp = layout.bindings[kTextureSlotArraySize + slot];
        EXPECT_EQ(smp.binding, MaterialBindingCache::kSamplerBindingOffset + slot);
        EXPECT_EQ(smp.type, DescriptorType::Sampler);
        EXPECT_EQ(smp.count, 1u);
    }
    // MaterialBinder classifies the set by binding 0's name.
    EXPECT_STREQ(layout.bindings[0].debugName, "ge_MaterialTextures0");
}

TEST(MaterialBindingCacheTest, SubstitutesThePerSlotDefaultForEveryUnboundSlot)
{
    ClassicFixture fx;
    if (!fx.Ready)
        GTEST_SKIP() << fx.SkipReason;

    MaterialBindingCache cache;
    MaterialBindingCache::SlotDefaults defaults{};
    defaults.White = fx.Textures.GetDefaultWhiteTexture();
    defaults.Black = fx.Textures.GetDefaultBlackTexture();
    defaults.FlatNormal = fx.Textures.GetDefaultFlatNormalTexture();
    const auto sampler = fx.Textures.GetSampler(SamplerPreset::LinearRepeat);
    cache.Initialize(fx.Device.get(), defaults, sampler);

    // The fallback ladder must mirror Material::InitBindlessDefaults, or the two
    // profiles render an unassigned slot differently.
    EXPECT_EQ(cache.DefaultTextureForSlot(static_cast<uint32_t>(TextureSlot::kAlbedo)), defaults.White);
    EXPECT_EQ(cache.DefaultTextureForSlot(static_cast<uint32_t>(TextureSlot::kNormal)), defaults.FlatNormal);
    EXPECT_EQ(cache.DefaultTextureForSlot(static_cast<uint32_t>(TextureSlot::kMetalRough)), defaults.White);
    EXPECT_EQ(cache.DefaultTextureForSlot(static_cast<uint32_t>(TextureSlot::kEmissive)), defaults.White);
    EXPECT_EQ(cache.DefaultTextureForSlot(static_cast<uint32_t>(TextureSlot::kAO)), defaults.White);
    EXPECT_EQ(cache.DefaultTextureForSlot(static_cast<uint32_t>(TextureSlot::kCoatNormal)), defaults.FlatNormal);
    EXPECT_EQ(cache.DefaultTextureForSlot(static_cast<uint32_t>(TextureSlot::kRoughness)), defaults.White);
    EXPECT_EQ(cache.DefaultTextureForSlot(static_cast<uint32_t>(TextureSlot::kMetallic)), defaults.Black);

    // A material with no textures at all still yields a fully-written set: the
    // layout is not partially-bound, so every slot must carry a default.
    Material bare = MakeMaterial();
    EXPECT_TRUE(cache.GetOrBuild(bare, sampler).IsValid());

    // A material with one slot bound resolves that slot and defaults the rest.
    Material albedoOnly = MakeMaterial();
    albedoOnly.SetTexture("albedoMap"_sid, fx.Textures.GetDefaultBlackTexture());
    EXPECT_EQ(albedoOnly.GetTextureForSlot(static_cast<uint32_t>(TextureSlot::kAlbedo)),
              fx.Textures.GetDefaultBlackTexture());
    EXPECT_FALSE(albedoOnly.GetTextureForSlot(static_cast<uint32_t>(TextureSlot::kNormal)).IsValid());
    EXPECT_TRUE(cache.GetOrBuild(albedoOnly, sampler).IsValid());

    cache.Shutdown();
}

TEST(MaterialBindingCacheTest, InvalidateDropsTheCachedEntry)
{
    ClassicFixture fx;
    if (!fx.Ready)
        GTEST_SKIP() << fx.SkipReason;

    MaterialBindingCache cache;
    MaterialBindingCache::SlotDefaults defaults{};
    defaults.White = fx.Textures.GetDefaultWhiteTexture();
    defaults.Black = fx.Textures.GetDefaultBlackTexture();
    defaults.FlatNormal = fx.Textures.GetDefaultFlatNormalTexture();
    const auto sampler = fx.Textures.GetSampler(SamplerPreset::LinearRepeat);
    cache.Initialize(fx.Device.get(), defaults, sampler);

    Material mat = MakeMaterial();
    const auto first = cache.GetOrBuild(mat, sampler);
    ASSERT_TRUE(first.IsValid());
    ASSERT_EQ(cache.GetOrBuild(mat, sampler), first);

    cache.Invalidate(mat.GetGuid());
    const auto rebuilt = cache.GetOrBuild(mat, sampler);
    ASSERT_TRUE(rebuilt.IsValid());
    // Persistent-pool sets are not recycled by DestroyDescriptorSet, so a
    // rebuilt entry is observably a different set — which is what proves the
    // dropped entry was rebuilt rather than served from the cache.
    EXPECT_NE(rebuilt, first);

    // A sampler change rebuilds without an explicit invalidation.
    const auto clamped = fx.Textures.GetSampler(SamplerPreset::LinearClamp);
    if (clamped.IsValid() && clamped != sampler)
        EXPECT_NE(cache.GetOrBuild(mat, clamped), rebuilt);

    cache.Shutdown();
}

namespace
{
constexpr const char* kClassicNotice = "no bindless descriptor indexing";
} // namespace

// Classic and Bindless are different material binding paths, and which one a machine took
// decides what a materials bug there can even be. Taking it in silence means the answer is
// only reachable by reproducing on that machine, so the boot names the mode and the reason.
TEST(TextureServiceClassicIndexing, ClassicModeNamesItselfAtInit)
{
    auto device = CreateForceNoBindlessDevice();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    if (device->GetCapabilities().supportsBindlessResources)
    {
        device->Shutdown();
        GTEST_SKIP() << "forceDisableBindlessResources not honored on this device";
    }

    std::vector<std::string> lines;
    MaterialRegistry registry;
    TextureService textures;
    bool ready = false;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        ready = textures.Initialize(device.get(), registry,
                                    RendererProfile::FromCapabilities(device->GetCapabilities()));
        Logger::Log::Flush();
    }
    if (ready)
        textures.Shutdown();
    device->Shutdown();

    ASSERT_TRUE(ready) << "TextureService::Initialize failed on the Classic path";
    EXPECT_FALSE(TestLog::FirstLineContaining(lines, kClassicNotice).empty())
        << "a classic-indexing boot did not name the mode it took";
}

// The counterpart: a bindless device must not claim it fell back, or the notice is noise
// that trains people to ignore it.
TEST(TextureServiceClassicIndexing, BindlessModeDoesNotClaimClassic)
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDebugLayer = false;
    dd.enableDynamicRendering = true;

    auto device = DeviceFactory::CreateDevice(dd);
    if (!device || !device->Initialize(dd))
        GTEST_SKIP() << "No Vulkan device available";
    if (!device->GetCapabilities().supportsBindlessResources)
    {
        device->Shutdown();
        GTEST_SKIP() << "this device has no bindless indexing; the Bindless path is unreachable";
    }

    std::vector<std::string> lines;
    MaterialRegistry registry;
    TextureService textures;
    bool ready = false;
    {
        TestLog::ScopedEngineLogCapture capture(&lines);
        // The sink sees only what Engine.dll logs; one line of our own proves it is live,
        // so the absence below is a silence rather than a capture that saw nothing.
        Logger::Log::Warning("TextureServiceClassicIndexingTests: capture liveness probe");
        ready = textures.Initialize(device.get(), registry,
                                    RendererProfile::FromCapabilities(device->GetCapabilities()));
        Logger::Log::Flush();
    }
    if (ready)
        textures.Shutdown();
    device->Shutdown();

    ASSERT_TRUE(ready) << "TextureService::Initialize failed on the Bindless path";
    ASSERT_FALSE(TestLog::FirstLineContaining(lines, "capture liveness probe").empty())
        << "the log capture saw nothing at all; the check below would pass on any build";
    EXPECT_TRUE(TestLog::FirstLineContaining(lines, kClassicNotice).empty())
        << "a bindless boot reported the classic-indexing fallback";
}
