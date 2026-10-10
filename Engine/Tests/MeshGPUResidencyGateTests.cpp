// Tests for the residency gate as MeshGPURegistry and MeshPoolGroupPlan
// enforce it: the two chokepoints an entry's bytes can escape through.
//
// Two properties, and they pull in opposite directions:
//   1. The gate REFUSES a non-resident entry at both chokepoints (red-armed
//      below: the forcing is applied, the refusal asserted, the forcing
//      removed, and the admission asserted).
//   2. The gate refuses NOTHING in normal operation. Every mesh pool is
//      host-visible today, so an upload is readable the moment UploadMesh
//      returns; a gate that fired here would be a regression, not a guard.

#include <gtest/gtest.h>

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/MeshPoolGroupPlan.h"
#include "Assets/ModelAsset.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"

#include "TestDeviceHelper.h"

#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

class MeshGPUResidencyGateTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Scene = std::make_unique<GPUScene>(m_Device.get());
        ASSERT_TRUE(m_Scene->Initialize(64u, 16u));
        m_Registry.Initialize(m_Device.get());
        m_Registry.SetGPUScene(m_Scene.get());
    }

    void TearDown() override
    {
        m_Registry.Shutdown();
        if (m_Scene)
            m_Scene->Shutdown();
        m_Scene.reset();
        if (m_Device)
            m_Device->Shutdown();
    }

    static Mesh MakeTriangle(float xOffset = 0.0f)
    {
        Mesh m{};
        m.Name = "GateTriangle";
        Vertex v0{}, v1{}, v2{};
        v0.Position[0] = xOffset;
        v0.Position[1] = 1.0f;
        v1.Position[0] = xOffset - 1.0f;
        v1.Position[1] = -1.0f;
        v2.Position[0] = xOffset + 1.0f;
        v2.Position[1] = -1.0f;
        v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
        m.Vertices = {v0, v1, v2};
        m.Indices  = {0u, 1u, 2u};
        m.MinBounds[0] = xOffset - 1.0f; m.MinBounds[1] = -1.0f; m.MinBounds[2] = 0.0f;
        m.MaxBounds[0] = xOffset + 1.0f; m.MaxBounds[1] = 1.0f;  m.MaxBounds[2] = 0.0f;
        return m;
    }

    MeshGPUHandle Register(uint32_t submesh, const Mesh& mesh)
    {
        return m_Registry.RegisterSubmesh(MeshGPUKey{m_Guid, submesh}, mesh);
    }

    uint32_t MeshTableCount() const
    {
        return static_cast<uint32_t>(m_Scene->GetMeshes().size());
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<GPUScene> m_Scene;
    MeshGPURegistry m_Registry;
    GUID m_Guid = GUID::Generate();
};

} // namespace

// PROPERTY 2, the one that makes this commit a no-op in production: on
// host-visible pools every registered entry is resident the moment it is
// published, so neither chokepoint refuses anything.
TEST_F(MeshGPUResidencyGateTest, GateRefusesNothingInNormalOperation)
{
    constexpr uint32_t kMeshCount = 8u;
    std::vector<MeshGPUHandle> handles;
    for (uint32_t i = 0; i < kMeshCount; ++i)
    {
        const MeshGPUHandle handle = Register(i, MakeTriangle(static_cast<float>(i)));
        ASSERT_TRUE(handle.IsValid());
        handles.push_back(handle);
    }

    m_Registry.BeginFrame(1u);

    for (MeshGPUHandle handle : handles)
    {
        const MeshGPUEntry* entry = m_Registry.Find(handle);
        ASSERT_NE(entry, nullptr);
        EXPECT_NE(entry->uploadSeq, 0u) << "a completed upload must carry a stamp";
        EXPECT_TRUE(m_Registry.IsFlushResident(*entry));

        MeshGPUEntryBindings bindings{};
        EXPECT_TRUE(m_Registry.TryGetDrawableBindings(*entry, bindings));
        EXPECT_TRUE(bindings.coreVB.IsValid());
        EXPECT_TRUE(bindings.indexBuffer.IsValid());
    }

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());
    EXPECT_EQ(plan.ResidencyStats().SkippedNonResident, 0u) << "Route A must leave nothing absent";
    EXPECT_EQ(plan.ResidencyStats().SkippedNonResidentTotal, 0u);
    EXPECT_EQ(plan.ResidencyStats().Refreshes, 1u) << "a zero is only evidence if Route A ran";
    for (MeshGPUHandle handle : handles)
    {
        const MeshGPUEntry* entry = m_Registry.Find(handle);
        ASSERT_NE(entry, nullptr);
        EXPECT_NE(plan.GroupOf(entry->gpuMeshIndex), MeshPoolGroupPlan::kAbsentGroup);
    }

    const MeshGPUResidencyStats stats = m_Registry.GetResidencyStats();
    EXPECT_EQ(stats.RouteBRefusedNonResident, 0u);
    EXPECT_EQ(stats.RouteBRefusedBucketMissing, 0u);
    EXPECT_EQ(stats.RouteBRefusedNonResidentTotal, 0u);
    EXPECT_EQ(stats.RouteBRefusedBucketMissingTotal, 0u);
    EXPECT_EQ(stats.WindowExhausted, 0u);
}

// PROPERTY 1 at the Route B chokepoint, red-armed. The forcing is the stamp
// itself: an entry whose uploadSeq is the never-uploaded sentinel is exactly
// what an entry mid-upload, mid-re-register or post-rebuild carries.
TEST_F(MeshGPUResidencyGateTest, RouteBRefusesANonResidentEntryAndAdmitsItOnceResident)
{
    const MeshGPUHandle handle = Register(0u, MakeTriangle());
    ASSERT_TRUE(handle.IsValid());
    const MeshGPUEntry* live = m_Registry.Find(handle);
    ASSERT_NE(live, nullptr);
    ASSERT_NE(live->uploadSeq, 0u);

    MeshGPUEntry probe = *live;

    // Forcing applied.
    probe.uploadSeq = 0u;
    EXPECT_FALSE(m_Registry.IsFlushResident(probe));
    MeshGPUEntryBindings refused{};
    EXPECT_FALSE(m_Registry.TryGetDrawableBindings(probe, refused));

    // Forcing removed: the same entry, same bucket, same pool indices.
    probe.uploadSeq = live->uploadSeq;
    EXPECT_TRUE(m_Registry.IsFlushResident(probe));
    MeshGPUEntryBindings admitted{};
    EXPECT_TRUE(m_Registry.TryGetDrawableBindings(probe, admitted));
    EXPECT_TRUE(admitted.coreVB.IsValid());
    EXPECT_TRUE(admitted.indexBuffer.IsValid());

    const MeshGPUResidencyStats stats = m_Registry.GetResidencyStats();
    EXPECT_EQ(stats.RouteBRefusedNonResident, 1u);
    EXPECT_EQ(stats.RouteBRefusedBucketMissing, 0u);
}

// The bytes escape through the out-parameter, which [[nodiscard]] does not
// cover. On refusal it must be value-initialised, with no pool handle written.
TEST_F(MeshGPUResidencyGateTest, RefusalLeavesTheOutParameterValueInitialised)
{
    const MeshGPUHandle handle = Register(0u, MakeTriangle());
    ASSERT_TRUE(handle.IsValid());
    const MeshGPUEntry* live = m_Registry.Find(handle);
    ASSERT_NE(live, nullptr);

    MeshGPUEntryBindings dirty{};
    ASSERT_TRUE(m_Registry.TryGetDrawableBindings(*live, dirty));
    ASSERT_TRUE(dirty.coreVB.IsValid());
    ASSERT_TRUE(dirty.indexBuffer.IsValid());

    MeshGPUEntry probe = *live;
    probe.uploadSeq = 0u;
    EXPECT_FALSE(m_Registry.TryGetDrawableBindings(probe, dirty));

    EXPECT_FALSE(dirty.coreVB.IsValid());
    EXPECT_FALSE(dirty.indexBuffer.IsValid());
    EXPECT_FALSE(dirty.tangentVB.IsValid());
    EXPECT_FALSE(dirty.colorVB.IsValid());
    EXPECT_FALSE(dirty.uv1VB.IsValid());
    EXPECT_FALSE(dirty.jointsVB.IsValid());
    EXPECT_FALSE(dirty.weightsVB.IsValid());
    EXPECT_FALSE(dirty.joints1VB.IsValid());
    EXPECT_FALSE(dirty.weights1VB.IsValid());
    for (const BufferHandle& extra : dirty.extraUvVB)
        EXPECT_FALSE(extra.IsValid());
    EXPECT_EQ(dirty.indexType, 0u);
    EXPECT_EQ(dirty, MeshGPUEntryBindings{});
}

// The reason split has to be right or the "must read zero" number is useless.
// An entry with no bucket owns no storage, so its refusal is a designed answer
// that steady-state operation reaches on purpose (tombstones, failed
// allocations, geometry that has not been built). It must NOT land in the
// non-resident counter, which is the one a gate criterion can be written on.
TEST_F(MeshGPUResidencyGateTest, ARefusalWithNoBucketIsCountedAsBucketMissing)
{
    const Mesh empty{}; // no vertices: no bucket, no stamp
    const MeshGPUHandle handle = Register(0u, empty);
    ASSERT_TRUE(handle.IsValid());
    const MeshGPUEntry* entry = m_Registry.Find(handle);
    ASSERT_NE(entry, nullptr);
    ASSERT_EQ(entry->bucketKey, VertexAttributeFlags::None);
    ASSERT_EQ(entry->uploadSeq, 0u);

    MeshGPUEntryBindings bindings{};
    EXPECT_FALSE(m_Registry.TryGetDrawableBindings(*entry, bindings));

    const MeshGPUResidencyStats stats = m_Registry.GetResidencyStats();
    EXPECT_EQ(stats.RouteBRefusedBucketMissing, 1u);
    EXPECT_EQ(stats.RouteBRefusedNonResident, 0u)
        << "a designed non-drawable state must not read as a residency defect";
}

// BeginFrame owns the per-frame counters' reset. The process totals and the
// window-exhaustion count must survive it: they are the only numbers a poll
// that misses the frame can be read against, and a criterion written on a
// counter that clears every frame cannot distinguish "never refused" from
// "refused while nobody was sampling".
TEST_F(MeshGPUResidencyGateTest, BeginFrameResetsThePerFrameRefusalCountersOnly)
{
    const MeshGPUHandle handle = Register(0u, MakeTriangle());
    ASSERT_TRUE(handle.IsValid());
    MeshGPUEntry probe = *m_Registry.Find(handle);
    probe.uploadSeq = 0u;

    MeshGPUEntryBindings bindings{};
    EXPECT_FALSE(m_Registry.TryGetDrawableBindings(probe, bindings));
    ASSERT_EQ(m_Registry.GetResidencyStats().RouteBRefusedNonResident, 1u);

    m_Registry.BeginFrame(1u);
    const MeshGPUResidencyStats stats = m_Registry.GetResidencyStats();
    EXPECT_EQ(stats.RouteBRefusedNonResident, 0u);
    EXPECT_EQ(stats.RouteBRefusedBucketMissing, 0u);
    EXPECT_EQ(stats.RouteBRefusedNonResidentTotal, 1u)
        << "the refusal must still be visible to a poll after the frame ends";
    EXPECT_EQ(stats.RouteBRefusedBucketMissingTotal, 0u);
    EXPECT_EQ(stats.WindowExhausted, 0u);
}

// PROPERTY 1 at the Route A chokepoint, red-armed against a REACHABLE state
// rather than a synthesised one.
//
// Mid-reprovision an entry that has not been re-uploaded yet still carries its
// pre-rebuild bucketKey, indexCount and gpuMeshIndex — every field Route A's
// pre-existing filters test — while its storage is gone and its stamp cleared.
// The sourceLookup callback runs inside that loop, which is the observation
// point. Without the residency check Refresh would key group ids off those
// stale pool identities and publish them as drawable.
TEST_F(MeshGPUResidencyGateTest, RouteALeavesAnEntryAbsentWhileItsStorageIsGone)
{
    constexpr uint32_t kMeshCount = 3u;
    std::vector<MeshGPUHandle> handles;
    for (uint32_t i = 0; i < kMeshCount; ++i)
        handles.push_back(Register(i, MakeTriangle(static_cast<float>(i))));

    MeshPoolGroupPlan before;
    before.Refresh(m_Registry, MeshTableCount());
    ASSERT_EQ(before.ResidencyStats().SkippedNonResident, 0u);
    ASSERT_EQ(before.LiveGroupCount(), 1u);

    uint32_t skippedMidRebuild = 0u;
    uint32_t liveGroupsMidRebuild = 1u;
    bool observed = false;

    // Nothing is restorable (no asset, no retained CPU mirror), so every entry
    // tombstones — but the FIRST callback fires before any of them has been
    // touched, which is the state under test. The pool VkBuffers this drops
    // are reclaimed by the device teardown in TearDown.
    m_Registry.ReprovisionAfterDeviceRebuild(
        [&](const MeshGPUKey&) -> MeshGPUCpuSource
        {
            if (!observed)
            {
                observed = true;
                MeshPoolGroupPlan midRebuild;
                midRebuild.Refresh(m_Registry, MeshTableCount());
                skippedMidRebuild     = midRebuild.ResidencyStats().SkippedNonResident;
                liveGroupsMidRebuild  = midRebuild.LiveGroupCount();
            }
            return {};
        });

    ASSERT_TRUE(observed) << "the reprovision walk never reached an entry";
    EXPECT_EQ(skippedMidRebuild, kMeshCount)
        << "Route A must leave every entry whose storage is gone absent";
    EXPECT_EQ(liveGroupsMidRebuild, 0u)
        << "an all-absent map must publish no live groups";
}

// Route A's census answers "what is absent from the CURRENT map" and every
// Refresh rebuilds it — and Refresh runs several times per frame. A skip is
// therefore erased microseconds after it happens, so the census alone cannot
// carry a steady-state claim. The process total can: it survives the rebuild
// that clears the census, and the Refresh count says the gate ran at all
// (Route A does not run with draw consolidation off).
TEST_F(MeshGPUResidencyGateTest, RouteASkipSurvivesInTheTotalAfterTheCensusIsRebuilt)
{
    constexpr uint32_t kMeshCount = 3u;
    for (uint32_t i = 0; i < kMeshCount; ++i)
        ASSERT_TRUE(Register(i, MakeTriangle(static_cast<float>(i))).IsValid());

    MeshPoolGroupPlan plan;
    plan.Refresh(m_Registry, MeshTableCount());
    ASSERT_EQ(plan.ResidencyStats().SkippedNonResident, 0u);
    ASSERT_EQ(plan.ResidencyStats().SkippedNonResidentTotal, 0u);

    // Same observation point as RouteALeavesAnEntryAbsentWhileItsStorageIsGone:
    // the first reprovision callback fires with every stamp cleared and every
    // pre-rebuild pool identity still in place.
    bool observed = false;
    m_Registry.ReprovisionAfterDeviceRebuild(
        [&](const MeshGPUKey&) -> MeshGPUCpuSource
        {
            if (!observed)
            {
                observed = true;
                plan.Refresh(m_Registry, MeshTableCount());
            }
            return {};
        });
    ASSERT_TRUE(observed) << "the reprovision walk never reached an entry";

    const MeshPoolGroupResidencyStats mid = plan.ResidencyStats();
    ASSERT_EQ(mid.SkippedNonResident, kMeshCount);
    ASSERT_EQ(mid.SkippedNonResidentTotal, kMeshCount);

    // Nothing was restorable, so every entry is now tombstoned (no bucket) and
    // is filtered ahead of the residency arm: this Refresh skips nothing and
    // rebuilds the census over the evidence.
    plan.Refresh(m_Registry, MeshTableCount());
    const MeshPoolGroupResidencyStats after = plan.ResidencyStats();
    EXPECT_EQ(after.SkippedNonResident, 0u) << "every Refresh rebuilds the census";
    EXPECT_EQ(after.SkippedNonResidentTotal, kMeshCount)
        << "the total is what a single poll can be read against";
    EXPECT_EQ(after.Refreshes, 3u);
}

// A completed upload's stamp is retired and replaced, not reused, when the same
// key is registered with different content. The old stamp must not strand the
// ring and the new entry must be drawable.
TEST_F(MeshGPUResidencyGateTest, InPlaceReRegisterMintsAFreshStamp)
{
    const MeshGPUHandle handle = Register(0u, MakeTriangle(0.0f));
    ASSERT_TRUE(handle.IsValid());
    const uint64_t firstSeq = m_Registry.Find(handle)->uploadSeq;
    ASSERT_NE(firstSeq, 0u);

    const MeshGPUHandle again = Register(0u, MakeTriangle(5.0f));
    EXPECT_EQ(again, handle) << "an in-place re-register keeps the handle";

    const MeshGPUEntry* entry = m_Registry.Find(handle);
    ASSERT_NE(entry, nullptr);
    EXPECT_NE(entry->uploadSeq, firstSeq);
    EXPECT_NE(entry->uploadSeq, 0u);

    MeshGPUEntryBindings bindings{};
    EXPECT_TRUE(m_Registry.TryGetDrawableBindings(*entry, bindings));
}

// RED-ARMED on the content hash. An upload that did not complete must NOT
// store the real hash: the next byte-identical registration would hit the dedup
// fast path and hand back the entry that can never draw. The 0 sentinel forces
// the re-upload path instead, so a later registration heals the key.
TEST_F(MeshGPUResidencyGateTest, IncompleteUploadStoresTheHealingContentHash)
{
    const Mesh empty{}; // no vertices: UploadMesh returns before any allocation
    const MeshGPUHandle handle = Register(0u, empty);
    ASSERT_TRUE(handle.IsValid());

    const MeshGPUEntry* entry = m_Registry.Find(handle);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->uploadSeq, 0u);
    EXPECT_EQ(entry->contentHash, 0u)
        << "an entry that cannot draw must not be dedup-reachable by its own content";

    MeshGPUEntryBindings bindings{};
    EXPECT_FALSE(m_Registry.TryGetDrawableBindings(*entry, bindings));

    // The key heals: the dedup fast path cannot fire on the 0 sentinel, so this
    // takes the re-upload path and the entry becomes drawable.
    const MeshGPUHandle healed = Register(0u, MakeTriangle());
    EXPECT_EQ(healed, handle);
    const MeshGPUEntry* healedEntry = m_Registry.Find(handle);
    ASSERT_NE(healedEntry, nullptr);
    EXPECT_NE(healedEntry->uploadSeq, 0u);
    EXPECT_NE(healedEntry->contentHash, 0u);
    EXPECT_TRUE(m_Registry.TryGetDrawableBindings(*healedEntry, bindings));
}

// The healing sentinel must not cost the dedup fast path: a completed upload
// keeps its real hash, so a byte-identical re-registration still dedups.
TEST_F(MeshGPUResidencyGateTest, CompletedUploadKeepsItsContentHashSoDedupStillFires)
{
    const Mesh mesh = MakeTriangle();
    const MeshGPUHandle handle = Register(0u, mesh);
    ASSERT_TRUE(handle.IsValid());

    const MeshGPUEntry* entry = m_Registry.Find(handle);
    ASSERT_NE(entry, nullptr);
    const uint64_t seq  = entry->uploadSeq;
    const uint64_t hash = entry->contentHash;
    ASSERT_NE(hash, 0u);

    EXPECT_EQ(Register(0u, mesh), handle);
    EXPECT_EQ(m_Registry.Find(handle)->uploadSeq, seq)
        << "a dedup hit must not re-upload, so the stamp is unchanged";
    EXPECT_EQ(m_Registry.Find(handle)->contentHash, hash);
}

// Cycling far more registrations than the ring holds must never exhaust it:
// each upload retires its own stamp, so the base keeps moving. Red-armed by
// the tiny window — with kDefaultPendingWindow this test could not fail.
TEST(MeshGPUResidencyGateTinyWindowTest, RegistrationCyclesFarMoreUploadsThanTheWindow)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";

    MeshGPURegistry registry(/*pendingUploadWindow=*/4u);
    registry.Initialize(device.get());

    Mesh mesh{};
    Vertex v0{}, v1{}, v2{};
    v0.Position[1] = 1.0f;
    v1.Position[0] = -1.0f;
    v1.Position[1] = -1.0f;
    v2.Position[0] = 1.0f;
    v2.Position[1] = -1.0f;
    v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
    mesh.Vertices = {v0, v1, v2};
    mesh.Indices  = {0u, 1u, 2u};
    mesh.MinBounds[0] = -1.0f; mesh.MinBounds[1] = -1.0f;
    mesh.MaxBounds[0] = 1.0f;  mesh.MaxBounds[1] = 1.0f;

    const GUID guid = GUID::Generate();
    constexpr uint32_t kRegistrations = 64u;
    for (uint32_t i = 0; i < kRegistrations; ++i)
    {
        const MeshGPUHandle handle = registry.RegisterSubmesh(MeshGPUKey{guid, i}, mesh);
        ASSERT_TRUE(handle.IsValid());
        const MeshGPUEntry* entry = registry.Find(handle);
        ASSERT_NE(entry, nullptr);
        ASSERT_NE(entry->uploadSeq, 0u) << "registration " << i << " was refused a stamp";
        MeshGPUEntryBindings bindings{};
        ASSERT_TRUE(registry.TryGetDrawableBindings(*entry, bindings));
    }

    EXPECT_EQ(registry.GetResidencyStats().WindowExhausted, 0u);

    registry.Shutdown();
    device->Shutdown();
}

// The observer must not corrupt the measurement. get_render_stats previews up
// to eight batch keys per view and asks each one whether its mesh is drawable;
// routed through the counting chokepoint, one poll banks up to eight refusal
// events per view into RouteBRefused*Total — which has NO reset, and is exactly
// the number the mesh-pool gate's acceptance criterion is read from. So the
// uncounted read has to give the same answer on every arm and move nothing.
TEST_F(MeshGPUResidencyGateTest, ADrawabilityReadAgreesWithTheChokepointAndCountsNothing)
{
    // Register everything BEFORE resolving any entry pointer: entries live in a
    // GenerationalVector, so a later registration can move them.
    const MeshGPUHandle liveHandle = Register(0u, MakeTriangle());
    ASSERT_TRUE(liveHandle.IsValid());
    const Mesh empty{}; // no vertices: no bucket, no stamp
    const MeshGPUHandle bucketlessHandle = Register(1u, empty);
    ASSERT_TRUE(bucketlessHandle.IsValid());

    const MeshGPUEntry* live = m_Registry.Find(liveHandle);
    ASSERT_NE(live, nullptr);
    ASSERT_NE(live->uploadSeq, 0u);
    const MeshGPUEntry* bucketless = m_Registry.Find(bucketlessHandle);
    ASSERT_NE(bucketless, nullptr);
    ASSERT_EQ(bucketless->bucketKey, VertexAttributeFlags::None);

    MeshGPUEntry nonResident = *live;
    nonResident.uploadSeq = 0u;

    // One pass through the COUNTING chokepoint, which is the arm that is
    // SUPPOSED to move the numbers. It also arms them: a zero delta below is
    // only evidence if the counters were non-zero to begin with.
    MeshGPUEntryBindings bindings{};
    ASSERT_TRUE(m_Registry.TryGetDrawableBindings(*live, bindings));
    ASSERT_FALSE(m_Registry.TryGetDrawableBindings(*bucketless, bindings));
    ASSERT_FALSE(m_Registry.TryGetDrawableBindings(nonResident, bindings));

    const MeshGPUResidencyStats before = m_Registry.GetResidencyStats();
    ASSERT_EQ(before.RouteBRefusedNonResidentTotal, 1u);
    ASSERT_EQ(before.RouteBRefusedBucketMissingTotal, 1u);

    // Now poll the way a stats reader does: every previewed key, every frame
    // the panel is open. The verdicts must match the chokepoint's on every arm
    // — a predicate that answered differently would be a second, drifting copy
    // of the tests — and nothing may move.
    constexpr int kPolls = 8;
    for (int poll = 0; poll < kPolls; ++poll)
    {
        EXPECT_TRUE(m_Registry.IsEntryDrawable(*live)) << "poll " << poll;
        EXPECT_FALSE(m_Registry.IsEntryDrawable(*bucketless)) << "poll " << poll;
        EXPECT_FALSE(m_Registry.IsEntryDrawable(nonResident)) << "poll " << poll;
    }

    const MeshGPUResidencyStats after = m_Registry.GetResidencyStats();
    EXPECT_EQ(after.RouteBRefusedNonResident, before.RouteBRefusedNonResident);
    EXPECT_EQ(after.RouteBRefusedBucketMissing, before.RouteBRefusedBucketMissing);
    EXPECT_EQ(after.RouteBRefusedNonResidentTotal, before.RouteBRefusedNonResidentTotal)
        << "the process total never resets — a read that moves it corrupts the gate's criterion";
    EXPECT_EQ(after.RouteBRefusedBucketMissingTotal, before.RouteBRefusedBucketMissingTotal)
        << "the process total never resets — a read that moves it corrupts the gate's criterion";
}
