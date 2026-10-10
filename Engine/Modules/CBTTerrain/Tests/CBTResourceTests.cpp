// Resource creation on a real device: all persistent buffers allocate and the
// descriptor set writes succeed. CBTResources uses only the static kernel-set
// layout, so it does not need a compiled shader.

#include <gtest/gtest.h>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTResources.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTestHarness.h"

#include <array>

using namespace GameEngine::CBTTerrain;
using namespace GameEngine::CBTTerrain::Test;
using namespace GameEngine::Rendering;

TEST(CBTResources, AllocatesAllBuffersAndWritesDescriptorSet)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device) << "failed to create headless Vulkan device";

    CBTKernelSet kernels; // not initialized: Resources only needs the static layout
    CBTResources resources;
    ASSERT_TRUE(resources.Initialize(*device, kernels));
    EXPECT_TRUE(resources.IsReady());
    EXPECT_EQ(resources.GetPoolSize(), kDefaultBisectorPoolSize);

    for (uint32_t i = 0; i < kCBTBindingCount; ++i)
        EXPECT_TRUE(resources.GetBuffer(static_cast<CBTBinding>(i)).IsValid())
            << "binding " << i << " buffer invalid";

    EXPECT_TRUE(resources.GetDescriptorSet().IsValid());
    EXPECT_TRUE(resources.GetReadbackBuffer().IsValid());
    EXPECT_TRUE(resources.GetIdentityIndexBuffer().IsValid());
    EXPECT_TRUE(resources.GetDrawCountBuffer().IsValid());
    // C4 params UBO + default height source (bindings 14/15).
    EXPECT_TRUE(resources.GetFrameParamsBuffer().IsValid());
    EXPECT_TRUE(resources.GetDefaultHeightTexture().IsValid());
    EXPECT_TRUE(resources.GetHeightSampler().IsValid());
    EXPECT_GT(resources.GetPersistentByteSize(), 0u);

    // Slot-diet round 2: the work-queue buffer is THREE aliased payload lanes (16 + 3*P words),
    // not six — the −12 B/slot diet. Pin the sizing so a regression that un-aliases a lane is caught.
    EXPECT_EQ(WQElementCount(kDefaultBisectorPoolSize),
              kWQCounterSlots + 3u * kDefaultBisectorPoolSize);
    EXPECT_EQ(resources.GetBufferByteSize(CBTBinding::WorkQueue),
              static_cast<uint64_t>(WQElementCount(kDefaultBisectorPoolSize)) * 4u);
    // Surface the measured persistent footprint (the production "CBTResources: N buffers, X KiB
    // persistent" line) so the before/after diet is visible in the test log at the 1M pool.
    GTEST_LOG_(INFO) << "CBTResources persistent bytes: " << resources.GetPersistentByteSize() << " ("
                     << (resources.GetPersistentByteSize() / 1024u) << " KiB) at pool "
                     << resources.GetPoolSize();

    resources.Shutdown();
    EXPECT_FALSE(resources.IsReady());
    device->Shutdown();
}

// The sculpt page rings are read only by a spherical tree, so a planar one holds 256 B placeholders
// per ring slot and the full rings (64 MiB host-visible at the default page budget) exist only while
// the tree is seeded for a sphere.
TEST(CBTInstance, SculptRingsExistOnlyForASphericalTree)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device) << "failed to create headless Vulkan device";

    CBTKernelSet kernels; // root seeding dispatches the vertex refresh, so the kernels must load
    ASSERT_TRUE(kernels.Initialize(*device, ShaderOutputDir())) << "CBT kernels missing from the build output";
    CBTInstance instance;
    ASSERT_TRUE(instance.Initialize(*device, kernels));
    const CBTResources& res = instance.GetResources();
    const uint64_t placeholderRingBytes =
        static_cast<uint64_t>(kCBTFrameParamsRing) * 2u * kSculptPlaceholderSlotWords * 4u;
    const uint64_t fullRingBytes =
        static_cast<uint64_t>(kCBTFrameParamsRing) *
        (static_cast<uint64_t>(ResolveSculptPagePoolCount()) * kSculptPageTexels + kSculptPageTableEntries) * 4u;

    ASSERT_TRUE(instance.InitializeRoots(kDomainPlanar));
    EXPECT_FALSE(res.HasSphericalSculptRings());
    EXPECT_EQ(res.GetSculptPagePoolCount(), 0u);
    EXPECT_EQ(res.GetSculptPoolSlotBytes(), kSculptPlaceholderSlotWords * 4u);
    EXPECT_EQ(res.GetSculptTableSlotBytes(), kSculptPlaceholderSlotWords * 4u);
    EXPECT_TRUE(res.GetSphereSculptBuffer().IsValid()) << "binding 16 must stay valid on a planar tree";
    EXPECT_TRUE(res.GetSphereSculptPageTableBuffer().IsValid()) << "binding 20 must stay valid on a planar tree";
    const uint64_t planarBytes = res.GetPersistentByteSize();

    ASSERT_TRUE(instance.InitializeRoots(kDomainSpherical));
    EXPECT_TRUE(res.HasSphericalSculptRings());
    EXPECT_EQ(res.GetSculptPagePoolCount(), ResolveSculptPagePoolCount());
    EXPECT_EQ(res.GetSculptPoolSlotBytes(), static_cast<uint64_t>(ResolveSculptPagePoolCount()) * kSculptPageTexels * 4u);
    EXPECT_EQ(res.GetSculptTableSlotBytes(), static_cast<uint64_t>(kSculptPageTableEntries) * 4u);
    EXPECT_EQ(res.GetPersistentByteSize() - planarBytes, fullRingBytes - placeholderRingBytes);
    GTEST_LOG_(INFO) << "sculpt rings: planar " << placeholderRingBytes << " B, spherical " << fullRingBytes
                     << " B (" << fullRingBytes / (1024u * 1024u) << " MiB)";

    ASSERT_TRUE(instance.InitializeRoots(kDomainPlanar));
    EXPECT_FALSE(res.HasSphericalSculptRings());
    EXPECT_EQ(res.GetPersistentByteSize(), planarBytes) << "returning to planar must release the full rings";

    instance.Shutdown();
    kernels.Shutdown();
    device->Shutdown();
}

// A sculpt ring allocation the device refuses must leave the rings the descriptor sets point at
// alive: the new pair is created before the old one is destroyed, so a refused 512 GiB host-visible
// request (2^21 pages) returns false with the placeholders still bound, and a later request that fits
// still provisions.
TEST(CBTResources, ARefusedSculptRingAllocationKeepsTheBoundRings)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device) << "failed to create headless Vulkan device";

    CBTKernelSet kernels; // not initialized: Resources only needs the static layout
    CBTResources resources;
    ASSERT_TRUE(resources.Initialize(*device, kernels));
    const BufferHandle boundPool = resources.GetSphereSculptBuffer();
    const BufferHandle boundTable = resources.GetSphereSculptPageTableBuffer();
    const uint64_t boundBytes = resources.GetPersistentByteSize();
    ASSERT_TRUE(boundPool.IsValid());
    ASSERT_TRUE(boundTable.IsValid());

    constexpr uint32_t kRefusedPages = 1u << 21;
    EXPECT_FALSE(resources.ProvisionSculptRings(true, kRefusedPages)) << "the device backed a 512 GiB ring";
    EXPECT_FALSE(resources.HasSphericalSculptRings());
    EXPECT_EQ(resources.GetSphereSculptBuffer(), boundPool) << "the sets must keep pointing at the bound pool";
    EXPECT_EQ(resources.GetSphereSculptPageTableBuffer(), boundTable);
    EXPECT_EQ(resources.GetPersistentByteSize(), boundBytes);
    // Still live: a destroyed buffer does not map.
    void* pool = device->MapBuffer(boundPool);
    EXPECT_NE(pool, nullptr) << "the bound pool was destroyed";
    if (pool)
        device->UnmapBuffer(boundPool);
    void* table = device->MapBuffer(boundTable);
    EXPECT_NE(table, nullptr) << "the bound page table was destroyed";
    if (table)
        device->UnmapBuffer(boundTable);

    EXPECT_TRUE(resources.ProvisionSculptRings(true, ResolveSculptPagePoolCount()));
    EXPECT_TRUE(resources.HasSphericalSculptRings());

    resources.Shutdown();
    device->Shutdown();
}

TEST(CBTResources, RejectsNonDefaultPoolInC1)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device);

    CBTKernelSet kernels;
    CBTResources resources;
    EXPECT_FALSE(resources.Initialize(*device, kernels, kDefaultBisectorPoolSize * 2u));
    device->Shutdown();
}

// Device-rebuild forget path, DEVICE-FREE (deliberately no MakeHeadlessDevice). The load-bearing
// property is that ReprovisionAfterDeviceRebuild touches no device at all: the rebuild teardown has
// already destroyed every VkObject behind these handles, so calling Destroy* over them is exactly
// what must not happen. Running it with m_Device still null proves no Destroy* path is reachable —
// Shutdown() by contrast guards its whole body on `if (m_Device)` and would be a silent no-op here,
// which is why this exercises the new entry point rather than Shutdown.
TEST(CBTResources, ReprovisionAfterDeviceRebuildIsDeviceFreeAndLeavesNotReady)
{
    CBTResources resources;
    resources.ReprovisionAfterDeviceRebuild();

    EXPECT_FALSE(resources.IsReady());
    EXPECT_EQ(resources.GetPoolSize(), 0u);
    EXPECT_EQ(resources.GetPersistentByteSize(), 0u);
    for (uint32_t i = 0; i < kCBTBindingCount; ++i)
        EXPECT_FALSE(resources.GetBuffer(static_cast<CBTBinding>(i)).IsValid()) << "binding " << i;
    EXPECT_FALSE(resources.GetDescriptorSet().IsValid());
    EXPECT_FALSE(resources.GetIdentityIndexBuffer().IsValid());
    EXPECT_FALSE(resources.GetDrawCountBuffer().IsValid());
    EXPECT_FALSE(resources.GetFrameParamsBuffer().IsValid());
    EXPECT_FALSE(resources.GetSphereSculptBuffer().IsValid());
    EXPECT_FALSE(resources.GetSphereSculptPageTableBuffer().IsValid());
    EXPECT_FALSE(resources.GetSurfaceParamsBuffer().IsValid());
    EXPECT_FALSE(resources.GetAtlasRowsBuffer().IsValid());
    EXPECT_FALSE(resources.GetDefaultHeightTexture().IsValid());
    EXPECT_FALSE(resources.GetDefaultAtlasHeightTexture().IsValid());
    EXPECT_FALSE(resources.GetDefaultAtlasCoarseTexture().IsValid());
    EXPECT_FALSE(resources.GetHeightSampler().IsValid());
    EXPECT_FALSE(resources.GetReadbackBuffer().IsValid());

    // Idempotent: the hook can fire again (a second loss during recovery) without effect.
    resources.ReprovisionAfterDeviceRebuild();
    EXPECT_FALSE(resources.IsReady());
}

// The instance-level forget must leave IsReady() false so a later Initialize re-allocates rather
// than an IsReady()-gated caller reusing dead state, and must not reset the terrain-source
// generation (a monotonic version readers compare for change).
TEST(CBTInstance, ReprovisionAfterDeviceRebuildIsDeviceFreeAndLeavesNotReady)
{
    CBTInstance instance;
    const uint64_t genBefore = instance.GetResources().GetTerrainSourceGeneration();

    instance.ReprovisionAfterDeviceRebuild();

    EXPECT_FALSE(instance.IsReady());
    EXPECT_FALSE(instance.GetResources().IsReady());
    EXPECT_EQ(instance.GetResources().GetPoolSize(), 0u);
    EXPECT_EQ(instance.GetResources().GetTerrainSourceGeneration(), genBefore)
        << "rewinding the monotonic terrain-source generation could mask a rebind";
}

namespace
{
TextureHandle MakeHeightTexture(IDevice& device, const char* name)
{
    TextureDesc td{};
    td.width = 4;
    td.height = 4;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.debugName = name;
    return device.CreateTexture(td);
}
} // namespace

// In-place re-provision safety oracle. A retired terrain heightmap must be dropped from EVERY
// height/atlas/coarse ring element in one shot (RebindTerrainSourcesToDefault), and the terrain
// source generation must advance — the deterministic replacement for the one-frame-margin per-slot
// self-heal that a SamplesPerMeter edit could lose (the device-lost this fix closes).
TEST(CBTResources, ReprovisionDropEvictsRetiredTextureFromWholeRing)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device);

    CBTKernelSet kernels;
    CBTResources resources;
    ASSERT_TRUE(resources.Initialize(*device, kernels));

    const TextureHandle retired = MakeHeightTexture(*device, "ReproHeightmap");
    ASSERT_TRUE(retired.IsValid());

    // Bind the (soon-to-be-retired) heightmap into EVERY height ring element, as steady-state
    // rendering does over kCBTFrameParamsRing frames.
    const uint64_t genBefore = resources.GetTerrainSourceGeneration();
    for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
        resources.SetHeightSource(slot, retired);
    EXPECT_EQ(resources.CountTerrainSourceSlots(retired), kCBTFrameParamsRing);
    EXPECT_GT(resources.GetTerrainSourceGeneration(), genBefore) << "each bind advances the version";

    // The re-provision drop: every element holding the retired view is rebound to the default in the
    // same call, and the generation advances — no ring element outlives the retired texture.
    const uint64_t genBound = resources.GetTerrainSourceGeneration();
    EXPECT_TRUE(resources.RebindTerrainSourcesToDefault());
    EXPECT_EQ(resources.CountTerrainSourceSlots(retired), 0u)
        << "a retired texture must not remain in any ring element after the drop";
    EXPECT_GT(resources.GetTerrainSourceGeneration(), genBound)
        << "the drop must advance the terrain source generation";
    EXPECT_EQ(resources.CountTerrainSourceSlots(resources.GetDefaultHeightTexture()), kCBTFrameParamsRing)
        << "every height element reverts to the flat default";

    // Idempotent: an already-clean ring reports no change and does not churn the version.
    const uint64_t genClean = resources.GetTerrainSourceGeneration();
    EXPECT_FALSE(resources.RebindTerrainSourcesToDefault());
    EXPECT_EQ(resources.GetTerrainSourceGeneration(), genClean);

    // Path equivalence: the re-provision recreates the heightmap at the new resolution. Binding the
    // NEW texture converges the ring onto it — the same end state the recreate path (leak old, lazily
    // bind new) reaches, so both paths end with CBT sampling the new texture, never the retired one.
    const TextureHandle recreated = MakeHeightTexture(*device, "ReproHeightmapRecreated");
    ASSERT_TRUE(recreated.IsValid());
    EXPECT_NE(recreated, retired);
    for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
        resources.SetHeightSource(slot, recreated);
    EXPECT_EQ(resources.CountTerrainSourceSlots(recreated), kCBTFrameParamsRing);
    EXPECT_EQ(resources.CountTerrainSourceSlots(retired), 0u);

    device->DestroyTexture(recreated);
    device->DestroyTexture(retired);
    resources.Shutdown();
    device->Shutdown();
}

// The atlas (18) and coarse (19) rings ride the same drop — an atlas-backed terrain's re-provision
// must not leave a stale atlas view either.
TEST(CBTResources, ReprovisionDropEvictsAtlasAndCoarseRings)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device);

    CBTKernelSet kernels;
    CBTResources resources;
    ASSERT_TRUE(resources.Initialize(*device, kernels));

    const TextureHandle atlas = MakeHeightTexture(*device, "ReproAtlas");
    const TextureHandle coarse = MakeHeightTexture(*device, "ReproCoarse");
    ASSERT_TRUE(atlas.IsValid());
    ASSERT_TRUE(coarse.IsValid());

    for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
    {
        resources.SetAtlasSource(slot, atlas);
        resources.SetCoarseSource(slot, coarse);
    }
    EXPECT_EQ(resources.CountTerrainSourceSlots(atlas), kCBTFrameParamsRing);
    EXPECT_EQ(resources.CountTerrainSourceSlots(coarse), kCBTFrameParamsRing);

    EXPECT_TRUE(resources.RebindTerrainSourcesToDefault());
    EXPECT_EQ(resources.CountTerrainSourceSlots(atlas), 0u);
    EXPECT_EQ(resources.CountTerrainSourceSlots(coarse), 0u);

    device->DestroyTexture(atlas);
    device->DestroyTexture(coarse);
    resources.Shutdown();
    device->Shutdown();
}

// ---------------------------------------------------------------------------
// Frame-ring rotation: the write-after-fence margin the 4th element is bought for.
//
// The rings are kCBTFrameParamsRing deep against a device that paces fewer frames,
// so the write to an element lands at least one RETIRED frame after that element's
// last reader. That margin exists only if the rotation actually visits every
// element. IDevice::GetFrameIndex() reports a slot in [0, framesInFlight), so its
// VALUE can never name the last element; AdvanceFrameCounter unwraps it into the
// monotonic counter CBTFrameRingSlot reduces. These drive the resources with the
// domain the device really produces (f % framesInFlight), which is what the
// production caller passes.
// ---------------------------------------------------------------------------

// The premise the ring depth is chosen against. If a backend ever paces as many
// frames as the ring has elements, the margin is gone and kCBTFrameParamsRing must
// grow with it — this is where that shows up.
TEST(CBTResources, DevicePacingIsShallowerThanTheFrameRing)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device);

    const uint32_t pacing = device->GetFramesInFlight();
    EXPECT_LT(pacing, kCBTFrameParamsRing)
        << "the frame ring (" << kCBTFrameParamsRing << ") must stay deeper than the device's "
           "pacing (" << pacing << ") or a ring write lands while the element's last reader is "
           "still in flight";

    device->Shutdown();
}

// Every ring element must be reached over a full cycle of device frames. Reducing the
// device's wrapped index directly leaves the last element on its Initialize-time
// default forever, which is the whole margin.
TEST(CBTResources, FrameRingsRotateThroughEveryElementUnderTheDeviceIndexDomain)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device);

    const uint32_t pacing = device->GetFramesInFlight();
    ASSERT_GT(pacing, 1u) << "a paced device is the premise of this test";
    ASSERT_LT(pacing, kCBTFrameParamsRing);

    CBTKernelSet kernels;
    CBTResources resources;
    ASSERT_TRUE(resources.Initialize(*device, kernels));

    const TextureHandle height = MakeHeightTexture(*device, "RingRotationHeight");
    const TextureHandle atlas = MakeHeightTexture(*device, "RingRotationAtlas");
    const TextureHandle coarse = MakeHeightTexture(*device, "RingRotationCoarse");
    ASSERT_TRUE(height.IsValid() && atlas.IsValid() && coarse.IsValid());

    // Exactly what CBTRenderNode does: one AdvanceFrameCounter per device frame, and
    // every per-frame bind that frame takes the value it returned.
    for (uint32_t f = 0; f < kCBTFrameParamsRing; ++f)
    {
        const uint32_t frame = resources.AdvanceFrameCounter(f % pacing);
        resources.SetHeightSource(frame, height);
        resources.SetAtlasSource(frame, atlas);
        resources.SetCoarseSource(frame, coarse);
    }

    EXPECT_EQ(resources.CountTerrainSourceSlots(height), kCBTFrameParamsRing)
        << "the height ring did not reach every element over " << kCBTFrameParamsRing
        << " device frames: the rotation collapsed onto the device's pacing (" << pacing
        << "), so the element beyond it still holds its Initialize-time default and the "
           "write-after-fence margin does not exist";
    EXPECT_EQ(resources.CountTerrainSourceSlots(atlas), kCBTFrameParamsRing)
        << "the atlas ring did not reach every element";
    EXPECT_EQ(resources.CountTerrainSourceSlots(coarse), kCBTFrameParamsRing)
        << "the coarse ring did not reach every element";

    device->DestroyTexture(height);
    device->DestroyTexture(atlas);
    device->DestroyTexture(coarse);
    resources.Shutdown();
    device->Shutdown();
}

// The reuse distance stated directly: over one cycle the elements are pairwise
// distinct, and the first repeat is kCBTFrameParamsRing frames later — not the
// device's pacing, which is what a wrapped selector would give.
TEST(CBTResources, FrameRingReuseDistanceIsRingDepthNotDevicePacing)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device);

    const uint32_t pacing = device->GetFramesInFlight();
    ASSERT_GT(pacing, 1u);

    CBTKernelSet kernels;
    CBTResources resources;
    ASSERT_TRUE(resources.Initialize(*device, kernels));

    std::array<uint32_t, kCBTFrameParamsRing> seen{};
    for (uint32_t f = 0; f < kCBTFrameParamsRing; ++f)
        seen[f] = CBTFrameRingSlot(resources.AdvanceFrameCounter(f % pacing));

    for (uint32_t a = 0; a < kCBTFrameParamsRing; ++a)
        for (uint32_t b = a + 1; b < kCBTFrameParamsRing; ++b)
            EXPECT_NE(seen[a], seen[b])
                << "frames " << a << " and " << b << " selected the same ring element: the "
                   "rotation collapsed to fewer than " << kCBTFrameParamsRing << " elements";

    // Frame kCBTFrameParamsRing is the FIRST that may reuse the frame-0 element: by then
    // the device has fenced through frame kCBTFrameParamsRing - pacing, so frame 0's read
    // of it has long retired.
    const uint32_t wrapped =
        CBTFrameRingSlot(resources.AdvanceFrameCounter(kCBTFrameParamsRing % pacing));
    EXPECT_EQ(wrapped, seen[0]) << "the counter must cycle the ring, not run past it";

    resources.Shutdown();
    device->Shutdown();
}

// Several views declare against ONE device frame. The counter must hold: rotating
// inside a frame would spend the margin AND break agreement with the push constant,
// which the LAST view to declare would have overwritten.
TEST(CBTResources, RepeatedDeviceIndexHoldsTheRingElement)
{
    auto device = MakeHeadlessDevice();
    ASSERT_TRUE(device);

    CBTKernelSet kernels;
    CBTResources resources;
    ASSERT_TRUE(resources.Initialize(*device, kernels));

    resources.AdvanceFrameCounter(0u);
    const uint32_t gameView = resources.AdvanceFrameCounter(1u);
    const uint32_t editorView = resources.AdvanceFrameCounter(1u);

    EXPECT_EQ(gameView, editorView)
        << "a repeated device index is the same frame, not the next one";
    EXPECT_EQ(CBTFrameRingSlot(gameView), CBTFrameRingSlot(editorView));

    resources.Shutdown();
    device->Shutdown();
}
