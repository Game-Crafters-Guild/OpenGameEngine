// Extraction dirty-feed fast path locks (fusion S2b, GE_EXTRACTION_FEED).
// Lane selection uses the constructor bool seam — the env flag is a
// process-static latch, so per-fixture env pins cannot work (the flip arc's
// G1 lesson). Post-S3 the feed lane IS the process default; the whole-process
// GE_EXTRACTION_FEED=0 suite run is the rollback-coverage enforcement point
// for the full lane. Everything here is device-gated (GTEST_SKIP without
// Vulkan): the fast path patches a real GPUScene.
//
// Frame model: tests emulate the engine tick — ProcessCommands, hierarchy,
// extraction, then exactly one feed Swap per frame (extraction runs at most
// once per swap window in the real tick structure).

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "ECSModules/Rendering/Systems/MorphTargetSystem.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "ECSModules/Rendering/SkeletonStore.h"

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"

#include "Components/Transform.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Rendering/ParticleRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Rendering/WorldSectorCoord.h"

#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/ShadowCasterChanges.h"

#include "Assets/ModelAsset.h" // Mesh/Vertex for registry fixtures
#include "AssetCore/GUID.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <algorithm>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
using namespace GameEngine::ECS;
using namespace GameEngine::Components;

#include "ExtractionHarness.h"
#include "TestDeviceHelper.h"

using namespace GameEngine::Testing::ExtractionFastPath;

namespace
{

// Mask the worldKey (upper 16 bits of GPUInstance.flags) so instance bytes
// captured against two different Worlds compare equal — the key is a hash of
// the (necessarily distinct) worldIds and is the single legitimate cross-run
// difference in a scripted A/B scenario.
std::vector<GPUInstance> MaskedInstances(const std::vector<GPUInstance>& in)
{
    std::vector<GPUInstance> out = in;
    for (auto& inst : out)
        inst.flags &= 0x0000FFFFu;
    return out;
}

struct SubmissionSnapshot
{
    uint32 viewId = 0;
    uint64 meshHandle = 0;
    GUID materialGuid{};
    uint32 instanceIndex = 0;
    uint32 renderLayerMask = 0;
    uint32 flagsMasked = 0;

    bool operator==(const SubmissionSnapshot&) const = default;
};

std::vector<SubmissionSnapshot> SnapshotSubmissions(
    const std::vector<WorldSubmissionRecord>& subs)
{
    std::vector<SubmissionSnapshot> out;
    out.reserve(subs.size());
    for (const auto& s : subs)
    {
        SubmissionSnapshot snap;
        snap.viewId = static_cast<uint32>(s.viewId);
        snap.meshHandle = static_cast<uint64>(s.meshHandle);
        snap.materialGuid = s.material ? s.material->GetGuid() : GUID::Null();
        snap.instanceIndex = s.instanceIndex;
        snap.renderLayerMask = s.renderLayerMask;
        snap.flagsMasked = s.flags & 0x0000FFFFu;
        out.push_back(snap);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// T4 — lane parity (the headline lock): a scripted mover scene stepped N
// frames produces byte-identical GPUScene instances (worldKey masked — the
// hash of the necessarily-distinct worldIds is the one legitimate cross-run
// difference), identical submissions and identical digests, fast path OFF
// vs ON.
// ---------------------------------------------------------------------------
namespace
{
struct ParityCapture
{
    std::vector<std::vector<GPUInstance>> instancesPerFrame;
    std::vector<std::vector<SubmissionSnapshot>> submissionsPerFrame;
    std::vector<uint64> digestPerFrame;
    uint32 fastFrames = 0;
};

ParityCapture RunParityScenario(Rendering::IDevice* device, bool fastPath)
{
    ParityCapture cap;
    ExtractionHarness h;
    if (!h.Initialize(device, fastPath))
        return cap;

    constexpr int kRenderables = 12;
    constexpr int kMovers = 4;
    constexpr int kFrames = 8;
    std::vector<ECS::Entity> entities;
    for (int i = 0; i < kRenderables; ++i)
        entities.push_back(h.SpawnRenderable(static_cast<float>(i), 0.0f, 0.0f));

    for (int frame = 0; frame < kFrames; ++frame)
    {
        if (frame >= 2)
        {
            // Deterministic mover pattern once both lanes had a chance to
            // settle (frames 0-1 are structurally noisy on both lanes).
            for (int m = 0; m < kMovers; ++m)
                entities[static_cast<size_t>(m)].Set(
                    MakeTransformAt(static_cast<float>(m), 1.0f + static_cast<float>(frame), 0.0f));
        }
        h.StepFrame();
        cap.instancesPerFrame.push_back(MaskedInstances(h.rs.GetGPUScene()->GetInstances()));
        cap.submissionsPerFrame.push_back(
            SnapshotSubmissions(h.extraction->GetSubmissionsForTest()));
        cap.digestPerFrame.push_back(h.rs.GetWorldRenderContentDigest(h.worldId));
        if (h.Stats().FastFrame != 0)
            ++cap.fastFrames;
    }
    return cap;
}
} // namespace

TEST_F(ExtractionFastPathTest, LaneParityInstancesSubmissionsDigest)
{
    ParityCapture off = RunParityScenario(m_Device.get(), false);
    ParityCapture on = RunParityScenario(m_Device.get(), true);

    ASSERT_EQ(off.instancesPerFrame.size(), on.instancesPerFrame.size());
    ASSERT_FALSE(off.instancesPerFrame.empty());
    EXPECT_EQ(off.fastFrames, 0u) << "OFF lane must never take the fast path";
    EXPECT_GT(on.fastFrames, 0u) << "ON lane never engaged — the parity test is vacuous";

    for (size_t f = 0; f < off.instancesPerFrame.size(); ++f)
    {
        const auto& a = off.instancesPerFrame[f];
        const auto& b = on.instancesPerFrame[f];
        ASSERT_EQ(a.size(), b.size()) << "frame " << f;
        ASSERT_FALSE(a.empty()) << "frame " << f;
        EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(GPUInstance)))
            << "GPUScene instance bytes diverged at frame " << f;
        EXPECT_EQ(off.submissionsPerFrame[f], on.submissionsPerFrame[f])
            << "submissions diverged at frame " << f;
        EXPECT_EQ(off.digestPerFrame[f], on.digestPerFrame[f])
            << "content digest diverged at frame " << f;
    }
}

// ---------------------------------------------------------------------------
// T5 — escalation matrix: each enumerated trigger provably escalates, and the
// post-escalation state matches what an always-full run would produce.
// ---------------------------------------------------------------------------

// Spawning escalates; so does a world reset: Clear() + respawn recovers through
// E4, and the feed's swap generation is monotonic across the reset so the cadence
// guard cannot be faked.
TEST_F(ExtractionFastPathTest, SpawnEscalatesViaStructuralVersion)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    const uint32 before = h.rs.GetGPUScene()->GetInstanceCount();
    ECS::Entity spawned = h.SpawnRenderable(5.0f, 0.0f, 0.0f);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationStructuralChange,
              0u);
    EXPECT_GT(h.rs.GetGPUScene()->GetInstanceCount(), before)
        << "the escalated full pass must create the new instance";
    auto* mg = h.world->GetComponent<MeshGPUData>(spawned.GetHandle());
    ASSERT_NE(mg, nullptr);
    EXPECT_NE(mg->instanceIndex, 0xFFFFFFFFu);

    // One full frame consumed the trigger; fast path resumes.
    ASSERT_TRUE(h.PrimeToFastPath());

    const uint64 genBefore = h.world->GetComponentDirtyFeed().SwapGeneration();
    h.world->Clear();
    EXPECT_GE(h.world->GetComponentDirtyFeed().SwapGeneration(), genBefore)
        << "swap generation must be monotonic across World::Clear";

    ECS::Entity respawned = h.SpawnRenderable(2.0f, 0.0f, 0.0f);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationStructuralChange,
              0u);
    mg = h.world->GetComponent<MeshGPUData>(respawned.GetHandle());
    ASSERT_NE(mg, nullptr);
    EXPECT_NE(mg->instanceIndex, 0xFFFFFFFFu);
    ASSERT_TRUE(h.PrimeToFastPath());
}

TEST_F(ExtractionFastPathTest, DespawnEscalatesAndDropsSubmissions)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ECS::Entity victim = h.SpawnRenderable(1.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());
    const size_t subsBefore = h.extraction->GetSubmissionsForTest().size();
    ASSERT_EQ(subsBefore, 2u);

    h.world->DestroyEntity(victim.GetHandle());
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationStructuralChange,
              0u);
    EXPECT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);
    ASSERT_TRUE(h.PrimeToFastPath());
    EXPECT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u)
        << "fast frames must replay the rebuilt (shrunk) submission cache";
}

TEST_F(ExtractionFastPathTest, MeshRendererValueEditEscalatesViaProbe)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    auto* mgBefore = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mgBefore, nullptr);
    const uint32 flagsBefore =
        h.rs.GetGPUScene()->GetInstances()[mgBefore->instanceIndex].flags;
    ASSERT_NE(flagsBefore & 1u, 0u) << "castShadows expected on by default";

    if (auto* mr = h.world->GetComponentForWrite<MeshRenderer>(e.GetHandle()))
        mr->castShadows = false;
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationProbeMeshRenderer,
              0u);
    auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[mg->instanceIndex].flags & 1u, 0u)
        << "the full pass must fold the castShadows edit into instance flags";
    ASSERT_TRUE(h.PrimeToFastPath());
}

TEST_F(ExtractionFastPathTest, LayerOnlyChangesPublishPayloadAndInvalidateShadows)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    auto actor = h.SpawnRenderable(0, 0, 0);
    auto sibling = h.SpawnRenderable(1, 0, 0);
    ASSERT_TRUE(h.PrimeToFastPath());
    const auto slot = h.world->GetComponent<MeshGPUData>(actor.GetHandle())->instanceIndex;
    const auto siblingSlot = h.world->GetComponent<MeshGPUData>(sibling.GetHandle())->instanceIndex;
    const auto original = h.rs.GetGPUScene()->GetInstances()[slot];
    const auto epoch = h.rs.ShadowCasterContentVersion(h.worldId);
    h.world->GetComponentForWrite<MeshRenderer>(actor.GetHandle())->renderLayerMask = 0;
    h.StepFrame();
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[slot].renderLayerMask, 0u);
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[siblingSlot].renderLayerMask, 1u);
    EXPECT_GT(h.rs.ShadowCasterContentVersion(h.worldId), epoch);
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[slot].normalMatrixCol0.x, original.normalMatrixCol0.x);
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[slot].normalMatrixCol0.y, original.normalMatrixCol0.y);
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[slot].normalMatrixCol0.z, original.normalMatrixCol0.z);
    ASSERT_TRUE(h.PrimeToFastPath());
    actor.Set(MakeTransformAt(.5f, 0, 0));
    h.StepFrame();
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[slot].renderLayerMask, 0u)
        << "transform feed must retain the authored mask";
    h.world->GetComponentForWrite<MeshRenderer>(actor.GetHandle())->renderLayerMask = 0x80000000u;
    h.rs.Views().SetViewRenderLayerMask(h.viewId, 0x80000000u);
    h.StepFrame();
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[slot].renderLayerMask, 0x80000000u);
    ASSERT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);
    EXPECT_EQ(h.extraction->GetSubmissionsForTest()[0].instanceIndex, slot);
}

TEST_F(ExtractionFastPathTest, LayerPayloadRecoversAnOutOfRangeDerivedSlot)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    auto actor = h.SpawnRenderable(0, 0, 0);
    MeshGPUData stale{};
    stale.instanceIndex = 64;
    actor.Set(stale);
    h.world->GetComponentForWrite<MeshRenderer>(actor.GetHandle())->renderLayerMask = 0;
    h.StepFrame();
    const auto* gpu = h.world->GetComponent<MeshGPUData>(actor.GetHandle());
    ASSERT_NE(gpu, nullptr);
    ASSERT_LT(gpu->instanceIndex, h.rs.GetGPUScene()->GetInstances().size());
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[gpu->instanceIndex].renderLayerMask, 0u);
}

TEST_F(ExtractionFastPathTest, LocalBoundsEditEscalatesViaProbe)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    LocalBounds lb{};
    lb.Box.center = {0.0f, 0.0f, 0.0f};
    lb.Box.halfExtents = {1.0f, 1.0f, 1.0f};
    e.Set(lb);
    ASSERT_TRUE(h.PrimeToFastPath());

    auto* mgBefore = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mgBefore, nullptr);
    const float radiusBefore =
        h.rs.GetGPUScene()->GetInstances()[mgBefore->instanceIndex].boundingRadius;

    if (auto* b = h.world->GetComponentForWrite<LocalBounds>(e.GetHandle()))
        b->Box.halfExtents.x = 4.0f;
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationProbeLocalBounds,
              0u);
    auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    EXPECT_GT(h.rs.GetGPUScene()->GetInstances()[mg->instanceIndex].boundingRadius,
              radiusBefore);
    ASSERT_TRUE(h.PrimeToFastPath());
}

TEST_F(ExtractionFastPathTest, LodGroupBiasEditEscalatesViaProbe)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    e.Set(LODGroup{});
    ASSERT_TRUE(h.PrimeToFastPath());

    if (auto* lod = h.world->GetComponentForWrite<LODGroup>(e.GetHandle()))
        lod->Bias = 2.0f;
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationProbeLodGroup,
              0u);
    auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    EXPECT_FLOAT_EQ(h.rs.GetGPUScene()->GetInstances()[mg->instanceIndex].lodBias, 2.0f);
    ASSERT_TRUE(h.PrimeToFastPath());
}

TEST_F(ExtractionFastPathTest, WorldSectorCoordEditEscalatesViaProbe)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    e.Set(WorldSectorCoord{});
    ASSERT_TRUE(h.PrimeToFastPath());

    // Editing the sector changes the packed sector AND the full-world
    // boundingCenter (camera-relative rendering), so it must escalate.
    if (auto* sc = h.world->GetComponentForWrite<WorldSectorCoord>(e.GetHandle()))
        sc->x += 1;
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationProbeSectorCoord,
              0u);
    ASSERT_TRUE(h.PrimeToFastPath());
}

// Regression for the pick-vs-render invisibility: TAGGING a mesh (adding
// WorldSectorCoord to an entity that had none — the IPC set_component path) must
// repack the GPUInstance sector AND recompose boundingCenter to full world.
// Otherwise the vertex stage reconstructs the instance far out while culling sees
// the sector-local bounds → frustum-culled at the true spot, rasterised offscreen
// at the local spot → invisible everywhere. The add is a structural change that
// escalates the full lane; the no-op guard must then REBUILD (the sector is part
// of it now), not skip because the transform version is unchanged.
//
// The editor IPC protocol EXACTLY: tag first (frame N), let it process, THEN edit
// the Transform (frame N+k, a separate IPC call). The follow-up transform edit
// rides the fast path off the CACHED sector, so it must preserve the tag — if the
// cache were stale (0) the sector would silently drop on the transform edit.
TEST_F(ExtractionFastPathTest, WorldSectorCoordAddThenTransformEditKeepsSector)
{
    namespace Origin = GameEngine::Engine::Renderer;
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(5.0f, 5.0f, 5.0f);
    ASSERT_TRUE(h.PrimeToFastPath());
    auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    const uint32 slot = mg->instanceIndex;

    // Untagged baseline: sector 0.
    {
        const auto& inst = h.rs.GetGPUScene()->GetInstances()[slot];
        int32 bx, by, bz;
        Origin::UnpackSector(inst.sectorPacked[0], inst.sectorPacked[1], bx, by, bz);
        EXPECT_EQ(bx, 0);
        EXPECT_EQ(by, 0);
        EXPECT_EQ(bz, 0);
    }

    // Frame N: tag it far out, exactly as the DebugServer set_component IPC path does.
    const WorldSectorCoord sector{3595, 0, 3595};
    h.world->AddComponentImmediate<WorldSectorCoord>(e.GetHandle(), sector);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u) << "adding WorldSectorCoord must escalate the full lane";
    {
        const auto& inst = h.rs.GetGPUScene()->GetInstances()[slot];
        int32 sx, sy, sz;
        Origin::UnpackSector(inst.sectorPacked[0], inst.sectorPacked[1], sx, sy, sz);
        ASSERT_EQ(sx, 3595) << "frame N (tag add) must repack the sector";
        EXPECT_EQ(sy, 0);
        EXPECT_EQ(sz, 3595);

        // boundingCenter must be FULL world (== ComposeEffectiveWorldTransform), so
        // GPU culling agrees with the shader — the F1 bounds discriminator.
        const auto* wt = h.world->GetComponent<WorldTransform>(e.GetHandle());
        ASSERT_NE(wt, nullptr);
        const WorldTransform composed =
            ComposeEffectiveWorldTransform(*wt, &sector, Components::kWorldSectorSize);
        EXPECT_FLOAT_EQ(inst.boundingCenter.x, composed.matrix[12]);
        EXPECT_FLOAT_EQ(inst.boundingCenter.y, composed.matrix[13]);
        EXPECT_FLOAT_EQ(inst.boundingCenter.z, composed.matrix[14]);
    }

    // Frame N+k: edit the Transform in a SEPARATE step (rides the fast path).
    e.Set(Transform::FromTRS(Mathematics::Vector3{100.0f, 2.0f, 100.0f}, Mathematics::Quaternion{},
                             Mathematics::Vector3{15.0f, 15.0f, 15.0f}));
    h.StepFrame();

    const auto& inst = h.rs.GetGPUScene()->GetInstances()[slot];
    int32 sx, sy, sz;
    Origin::UnpackSector(inst.sectorPacked[0], inst.sectorPacked[1], sx, sy, sz);
    EXPECT_EQ(sx, 3595) << "the follow-up transform edit must PRESERVE the tag (cached sector)";
    EXPECT_EQ(sy, 0);
    EXPECT_EQ(sz, 3595);

    const auto* wt = h.world->GetComponent<WorldTransform>(e.GetHandle());
    ASSERT_NE(wt, nullptr);
    const WorldTransform composed =
        ComposeEffectiveWorldTransform(*wt, &sector, Components::kWorldSectorSize);
    EXPECT_FLOAT_EQ(inst.boundingCenter.x, composed.matrix[12]);
    EXPECT_FLOAT_EQ(inst.boundingCenter.z, composed.matrix[14]);
}

TEST_F(ExtractionFastPathTest, FirstFrameEscalatesViaUnprimedCaches)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationCachesUnprimed,
              0u)
        << "the very first frame must run the full lane to prime the caches (E11)";
}

TEST_F(ExtractionFastPathTest, DisabledTagRoundTripEscalatesAndTombstones)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());
    auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    const uint32 slot = mg->instanceIndex;

    h.world->AddComponentImmediate<ECS::Disabled>(e.GetHandle(), ECS::Disabled{});
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationStructuralChange,
              0u);
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[slot].meshIndex, 0xFFFFFFFFu)
        << "the escalated frame's CLEANUP must tombstone the disabled instance";

    h.world->RemoveComponentImmediate<ECS::Disabled>(e.GetHandle());
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.rs.GetGPUScene()->GetInstances()[slot].meshIndex, 0xFFFFFFFFu)
        << "re-enabling must repaint the kept slot";
    ASSERT_TRUE(h.PrimeToFastPath());
}

TEST_F(ExtractionFastPathTest, MeshReloadEventEscalatesAndSentinelHoldsFullLane)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    // In-place valid -> sentinel re-register (empty content) fires the reload
    // event (S2a A2 fix) -> E6 escalates this frame; the sentinel row then
    // holds the full lane via E8 until the mesh is GPU-resident again (T16
    // b/c wiring).
    h.rs.GetMeshGPURegistry().RegisterSubmesh({FixedGuid(0x11), 0}, Mesh{});
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationMeshReload,
              0u);
    EXPECT_GT(h.Stats().PendingSentinelIndex, 0u);

    // Pending pins the full lane (conservative-correct).
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationPendingTransient,
              0u);

    // Restore geometry: sentinel -> valid is the streaming-in shape (no
    // event); the retrying full lane renders it, pending drains, fast path
    // resumes.
    Mesh mesh{};
    Vertex v0{}, v1{}, v2{};
    v0.Position[1] = 1.0f;
    v1.Position[0] = -1.0f;
    v2.Position[0] = 1.0f;
    mesh.Vertices = {v0, v1, v2};
    mesh.Indices = {0, 1, 2};
    h.rs.GetMeshGPURegistry().RegisterSubmesh({FixedGuid(0x11), 0}, mesh);
    ASSERT_TRUE(h.PrimeToFastPath());
    EXPECT_EQ(h.Stats().PendingSentinelIndex, 0u);
}

TEST_F(ExtractionFastPathTest, UnregisterSubmeshEventEscalates)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    h.SpawnRenderable(0.0f, 0.0f, 0.0f);

    // Second mesh, second entity: unregistering it must escalate via the new
    // UnregisterSubmesh notification (E6) — without it, cached submissions
    // keep referencing a freed GPU row a later AddMesh can recycle.
    Mesh mesh{};
    Vertex v0{}, v1{}, v2{};
    v0.Position[1] = 2.0f;
    v1.Position[0] = -2.0f;
    v2.Position[0] = 2.0f;
    mesh.Vertices = {v0, v1, v2};
    mesh.Indices = {0, 1, 2};
    const MeshGPUKey key{FixedGuid(0x33), 0};
    const MeshGPUHandle second = h.rs.GetMeshGPURegistry().RegisterSubmesh(key, mesh);
    ASSERT_TRUE(second.IsValid());

    ECS::Entity e = h.world->Create();
    e.Set(MakeTransformAt(3.0f, 0.0f, 0.0f));
    MeshRenderer mr{};
    mr.meshGpuHandleId = static_cast<uint64>(second);
    mr.renderLayerMask = 0x1u;
    mr.materialAssetGuid.Set(h.materialGuid);
    e.Set(mr);
    h.world->ProcessCommands();

    ASSERT_TRUE(h.PrimeToFastPath());

    h.rs.GetMeshGPURegistry().UnregisterSubmesh(key);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationMeshReload,
              0u);
}

TEST_F(ExtractionFastPathTest, MaterialRegistrationEscalatesViaDigest)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    // Register through MaterialSystem so the SSBO index assignment bumps
    // m_MaterialSSBOGeneration and the depth-class authority array refreshes
    // — the two E7 digest inputs.
    MaterialDocument doc{};
    doc.materialName = "FastPathE7Poke";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "surfaces/standard_surface.glsl";
    h.rs.Materials().RegisterMaterialFromDocument(FixedGuid(0x44), doc);

    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationMaterialDigest,
              0u);
    ASSERT_TRUE(h.PrimeToFastPath());
}

TEST_F(ExtractionFastPathTest, ViewOpenCloseAndMaskEditEscalate)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());
    ASSERT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);

    // Open a second matching view.
    ViewId second = h.rs.Views().AllocateView("FastPathView2", h.cameraId);
    h.rs.Views().SetViewWorldId(second, h.worldId);
    h.rs.Views().SetViewRenderLayerMask(second, 0x1u);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits & RenderExtractionSystem::kEscalationViewSet,
              0u);
    EXPECT_EQ(h.extraction->GetSubmissionsForTest().size(), 2u)
        << "the rebuilt cache must emit per-view submissions for the new view";
    ASSERT_TRUE(h.PrimeToFastPath());

    // Layer-mask edit invalidates the cached per-view emission set.
    h.rs.Views().SetViewRenderLayerMask(second, 0x2u);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits & RenderExtractionSystem::kEscalationViewSet,
              0u);
    EXPECT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);
    ASSERT_TRUE(h.PrimeToFastPath());

    // Close it again.
    h.rs.Views().ReleaseView(second);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits & RenderExtractionSystem::kEscalationViewSet,
              0u);
    ASSERT_TRUE(h.PrimeToFastPath());
}

// A probe capture's OnDemand view starting and stopping still escalates (its
// own submissions appear and disappear) but changes nothing a persistent view
// draws: the content versions the TAA stationary certification and the shadow
// caches key on stay put. A mask edit on an Always view still advances them.
TEST_F(ExtractionFastPathTest, OnDemandViewChurnLeavesContentVersions)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ViewId capture = h.rs.Views().AllocateView("FastPathCapture", h.cameraId,
                                               Rendering::ViewPurpose::UtilityCapture,
                                               Rendering::ViewParticipation::OnDemand);
    h.rs.Views().SetViewWorldId(capture, h.worldId);
    h.rs.Views().SetViewRenderLayerMask(capture, 0x1u);
    ASSERT_TRUE(h.PrimeToFastPath());
    const uint64_t renderVersion = h.rs.RenderContentVersion(h.worldId);
    const uint64_t casterVersion = h.rs.ShadowCasterContentVersion(h.worldId);

    h.rs.Views().RequestViewFrame(capture);
    h.rs.Views().BeginFrame();
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_EQ(h.Stats().EscalationReasonBits, RenderExtractionSystem::kEscalationViewSet);
    EXPECT_EQ(h.extraction->GetSubmissionsForTest().size(), 2u)
        << "the capture view must be fed while its request is live";
    h.rs.Views().BeginFrame();
    h.StepFrame();
    EXPECT_EQ(h.Stats().EscalationReasonBits, RenderExtractionSystem::kEscalationViewSet)
        << "the expiry frame must escalate on E9 alone";
    EXPECT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);
    EXPECT_EQ(h.rs.RenderContentVersion(h.worldId), renderVersion);
    EXPECT_EQ(h.rs.ShadowCasterContentVersion(h.worldId), casterVersion);

    ASSERT_TRUE(h.PrimeToFastPath());
    h.rs.Views().SetViewRenderLayerMask(h.viewId, 0x2u);
    h.StepFrame();
    EXPECT_GT(h.rs.RenderContentVersion(h.worldId), renderVersion);
    EXPECT_GT(h.rs.ShadowCasterContentVersion(h.worldId), casterVersion);
}

// The churn exception never hides a real change: a renderable that moves in
// the frame an OnDemand view activates still advances both versions.
TEST_F(ExtractionFastPathTest, OnDemandActivationWithAMoverStillBumps)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity mover = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ViewId capture = h.rs.Views().AllocateView("FastPathCapture", h.cameraId,
                                               Rendering::ViewPurpose::UtilityCapture,
                                               Rendering::ViewParticipation::OnDemand);
    h.rs.Views().SetViewWorldId(capture, h.worldId);
    h.rs.Views().SetViewRenderLayerMask(capture, 0x1u);
    ASSERT_TRUE(h.PrimeToFastPath());
    const uint64_t renderVersion = h.rs.RenderContentVersion(h.worldId);
    const uint64_t casterVersion = h.rs.ShadowCasterContentVersion(h.worldId);

    mover.Set(MakeTransformAt(0.0f, 9.0f, 0.0f));
    h.rs.Views().RequestViewFrame(capture);
    h.rs.Views().BeginFrame();
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits & RenderExtractionSystem::kEscalationViewSet, 0u);
    EXPECT_GT(h.rs.RenderContentVersion(h.worldId), renderVersion);
    EXPECT_GT(h.rs.ShadowCasterContentVersion(h.worldId), casterVersion);
}

// ---------------------------------------------------------------------------
// T6 — cadence gap: swap windows discarded while extraction was not running
// force the full lane exactly once (the shared SwapGenerationGuard, E3).
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, MissedSwapWindowsEscalateOnce)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity mover = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    // Extraction disabled for 3 windows while a mover keeps moving: the
    // hierarchy emissions land in windows that are swapped out unseen.
    for (int i = 0; i < 3; ++i)
    {
        mover.Set(MakeTransformAt(0.0f, 10.0f + static_cast<float>(i), 0.0f));
        h.world->ProcessCommands();
        h.hierarchy.Update(*h.world, kDt);
        h.world->SwapComponentDirtyFeed();
    }

    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationMissedWindow,
              0u);
    auto* mg = h.world->GetComponent<MeshGPUData>(mover.GetHandle());
    ASSERT_NE(mg, nullptr);
    const auto& inst = h.rs.GetGPUScene()->GetInstances()[mg->instanceIndex];
    EXPECT_FLOAT_EQ(inst.transform.Data()[13], 12.0f)
        << "the recovery full pass must not leave the mover frozen";
    ASSERT_TRUE(h.PrimeToFastPath());
}

// ---------------------------------------------------------------------------
// T7 — overflow: an overflowed window cannot be trusted (Snapshot is
// incomplete) and must force the full lane.
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, FeedOverflowEscalates)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    auto& feed = h.world->GetComponentDirtyFeed();
    for (std::size_t i = 0; i <= ECS::ComponentDirtyFeed::kOverflowCap; ++i)
        feed.Append(e.GetHandle());
    ASSERT_TRUE(feed.Overflowed());

    e.Set(MakeTransformAt(0.0f, 42.0f, 0.0f));
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u);
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationFeedOverflow,
              0u);
    auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    EXPECT_FLOAT_EQ(h.rs.GetGPUScene()->GetInstances()[mg->instanceIndex].transform.Data()[13],
                    42.0f);
}

// ---------------------------------------------------------------------------
// T9 — undo byte-restore under motion: restoring an OLDER Version must
// repaint on the fast path (== compare, not <) via the feed emission the
// unified data-only set body produces.
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, UndoVersionBackwardRepaintsOnFastPath)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(1.0f, 2.0f, 3.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    const auto* wtOldPtr = h.world->GetComponent<WorldTransform>(e.GetHandle());
    ASSERT_NE(wtOldPtr, nullptr);
    const WorldTransform wtOld = *wtOldPtr; // bytes incl. Version — the undo payload

    // Move (render at the new spot on a fast frame).
    e.Set(MakeTransformAt(1.0f, 20.0f, 3.0f));
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 1u);
    auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    EXPECT_FLOAT_EQ(h.rs.GetGPUScene()->GetInstances()[mg->instanceIndex].transform.Data()[13],
                    20.0f);

    // Undo: byte-restore WorldTransform only (matrix AND the older Version) —
    // the editor's undo shape. The data-only set stamps + emits to the feed;
    // Transform is untouched so the hierarchy's change gate leaves the
    // restored bytes in place.
    e.Set(wtOld);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 1u)
        << "an undo byte-restore is a value write — it must not escalate";
    EXPECT_GT(h.Stats().FeedPatchedCount, 0u);
    EXPECT_FLOAT_EQ(h.rs.GetGPUScene()->GetInstances()[mg->instanceIndex].transform.Data()[13],
                    2.0f)
        << "the restored (older) Version must compare unequal and repaint";
}

// ---------------------------------------------------------------------------
// T10 — unsubscribed world (the thumbnail-world shape): a second extraction
// instance against a world with no WorldTransform feed subscription must
// never take the fast path.
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, UnsubscribedWorldStaysOnFullLane)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));

    auto thumbWorld = std::make_unique<World>(nullptr); // deliberately NOT feed-subscribed
    RenderExtractionSystem thumbExtraction(&h.rs, true);

    ViewId thumbView = h.rs.Views().AllocateView("ThumbView", h.cameraId);
    h.rs.Views().SetViewWorldId(thumbView, thumbWorld->GetWorldId());
    h.rs.Views().SetViewRenderLayerMask(thumbView, 0x1u);

    ECS::Entity e = thumbWorld->Create();
    e.Set(MakeTransformAt(0.0f, 0.0f, 0.0f));
    MeshRenderer mr{};
    mr.meshGpuHandleId = static_cast<uint64>(h.meshHandle);
    mr.renderLayerMask = 0x1u;
    mr.materialAssetGuid.Set(h.materialGuid);
    e.Set(mr);
    thumbWorld->ProcessCommands();

    TransformHierarchySystem thumbHierarchy;
    for (int i = 0; i < 5; ++i)
    {
        thumbWorld->ProcessCommands();
        thumbHierarchy.Update(*thumbWorld, kDt);
        thumbExtraction.Update(*thumbWorld, kDt);
        EXPECT_EQ(h.rs.GetRenderExtractionStats().FastFrame, 0u)
            << "an unsubscribed world must stay full-lane (E1), frame " << i;
        EXPECT_NE(h.rs.GetRenderExtractionStats().EscalationReasonBits &
                      RenderExtractionSystem::kEscalationFeedDisabled,
                  0u);
    }
    auto* mg = thumbWorld->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    EXPECT_NE(mg->instanceIndex, 0xFFFFFFFFu) << "thumbnails must still render";
}

// ---------------------------------------------------------------------------
// T11 — always-refresh subset (D5): per-frame-volatile skinning state is
// picked up on FAST frames with no escalation, while submission-visible
// edits on a morph carrier escalate via the E5 probes (morph carriers are
// probed like every other archetype — impl-review F1). Also the T15
// composition's fast-frames clause: a "playing" skinned entity + an idle
// morph carrier must sustain the fast path.
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, SubsetRefreshesSkinnedStateAndMorphEditsEscalate)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));

    auto& skStore = SkeletonStore::Instance();
    const uint32 skeletonId = skStore.CreateSkeleton(1);
    ASSERT_NE(skeletonId, 0u);
    if (auto* skel = skStore.Get(skeletonId))
    {
        skel->Parent = {-1};
        skel->InverseBind.resize(16);
        SetIdentityMatrix(skel->InverseBind.data());
        skel->BindPose.resize(16);
        SetIdentityMatrix(skel->BindPose.data());
        skel->RestTranslation = {0.0f, 0.0f, 0.0f};
        skel->RestRotation = {0.0f, 0.0f, 0.0f, 1.0f};
        skel->RestScale = {1.0f, 1.0f, 1.0f};
        skel->SkinJointCount = 1;
        skel->JointNodes = {0};
    }
    const uint32 runtimeId = skStore.CreateRuntime(skeletonId);
    ASSERT_NE(runtimeId, 0u);
    auto* runtime = skStore.GetRuntime(runtimeId);
    ASSERT_NE(runtime, nullptr);
    runtime->CompactSkinMatrices.resize(12, 0.0f); // non-empty => palette offset consumed
    runtime->AtlasPaletteOffsetBones = 64u;

    // "Playing" skinned entity: palette offset churns per frame (the
    // SkinningUpload shape) — never probed, refreshed by the subset.
    ECS::Entity skinned = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    SkinnedMeshRenderer smr{};
    smr.skeletonId = skeletonId;
    skinned.Set(smr);
    SkeletonRef skelRef{};
    skelRef.skeletonId = skeletonId;
    skelRef.runtimeId = runtimeId;
    skinned.Set(skelRef);
    AnimatorRef anim{};
    anim.ClipIndex = 1; // hasActiveAnimation
    skinned.Set(anim);

    // Morph carrier (idle): takes no grants while idle, so it must not cost
    // fast frames; its edits are probed like any other archetype's.
    ECS::Entity morph = h.SpawnRenderable(2.0f, 0.0f, 0.0f);
    morph.Set(MorphTargetWeights{});
    LocalBounds lb{};
    lb.Box.halfExtents = {1.0f, 1.0f, 1.0f};
    morph.Set(lb);

    h.world->ProcessCommands();
    ASSERT_TRUE(h.PrimeToFastPath());

    auto* mgSkinned = h.world->GetComponent<MeshGPUData>(skinned.GetHandle());
    ASSERT_NE(mgSkinned, nullptr);
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[mgSkinned->instanceIndex].skinPaletteOffset,
              64u);

    // Idle frames sustain the fast path (the T15-composition soak premise).
    for (int i = 0; i < 4; ++i)
    {
        h.StepFrame();
        EXPECT_EQ(h.Stats().FastFrame, 1u) << "idle animated scene escalated at frame " << i;
    }

    // Per-frame palette churn: subset picks it up on a fast frame.
    runtime->AtlasPaletteOffsetBones = 128u;
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 1u) << "palette churn must not escalate";
    EXPECT_GT(h.Stats().SubsetRefreshedCount, 0u);
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[mgSkinned->instanceIndex].skinPaletteOffset,
              128u);

    // Animation stop via AnimatorRef edit: not probed, subset recomputes.
    if (auto* a = h.world->GetComponentForWrite<AnimatorRef>(skinned.GetHandle()))
        a->ClipIndex = 0;
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 1u) << "AnimatorRef edits must not escalate";
    EXPECT_EQ(h.rs.GetGPUScene()->GetInstances()[mgSkinned->instanceIndex].skinPaletteOffset,
              0u)
        << "stopping playback must drop the palette offset to the bind-pose block";

    // Morph bounds edit: submission-adjacent record input on a morph carrier
    // — must escalate via the LocalBounds probe (the impl-review F1 class:
    // with a morph exclusion on the probes this write had NO trigger and the
    // replayed submissions went stale indefinitely).
    auto* mgMorph = h.world->GetComponent<MeshGPUData>(morph.GetHandle());
    ASSERT_NE(mgMorph, nullptr);
    const float radiusBefore =
        h.rs.GetGPUScene()->GetInstances()[mgMorph->instanceIndex].boundingRadius;
    if (auto* b = h.world->GetComponentForWrite<LocalBounds>(morph.GetHandle()))
        b->Box.halfExtents.x = 6.0f;
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u)
        << "a morph-carrier LocalBounds edit must escalate like any other archetype's";
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationProbeLocalBounds,
              0u);
    EXPECT_GT(h.rs.GetGPUScene()->GetInstances()[mgMorph->instanceIndex].boundingRadius,
              radiusBefore)
        << "the escalated full pass must fold the fresh bounds into the instance";
    ASSERT_TRUE(h.PrimeToFastPath());
}

// ---------------------------------------------------------------------------
// Impl-review F1(a): a material swap between two ALREADY-registered
// materials on a morph carrier moves no E7 digest and fires no E6 event —
// the MeshRenderer probe is the only trigger, and the replayed submission
// must retarget within one frame.
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, MorphCarrierMaterialSwapEscalatesAndRetargetsSubmission)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));

    // Second material registered registry-direct (bypasses MaterialSystem's
    // index assignment): the E7 digest inputs never move, isolating the
    // probe as the sole trigger.
    const GUID materialB = FixedGuid(0x55);
    {
        MaterialDocument doc{};
        doc.materialName = "FastPathMaterialB";
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "surfaces/standard_surface.glsl";
        Material* matB = h.rs.Materials().Registry().Register(materialB, doc);
        ASSERT_NE(matB, nullptr);
        Material::TestFactory::SetGraphicsPipelineId(*matB, Rendering::GraphicsPipelineId{1u});
        Material::TestFactory::SetGpuSceneMaterialIndex(*matB, 0u);
    }

    ECS::Entity morph = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    morph.Set(MorphTargetWeights{});
    h.world->ProcessCommands();
    ASSERT_TRUE(h.PrimeToFastPath());
    ASSERT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);
    ASSERT_EQ(h.extraction->GetSubmissionsForTest()[0].material->GetGuid(), h.materialGuid);

    if (auto* mr = h.world->GetComponentForWrite<MeshRenderer>(morph.GetHandle()))
        mr->materialAssetGuid.Set(materialB);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u)
        << "a submission-visible edit on a morph carrier must escalate";
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationProbeMeshRenderer,
              0u);
    ASSERT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);
    EXPECT_EQ(h.extraction->GetSubmissionsForTest()[0].material->GetGuid(), materialB)
        << "the rebuilt submission must carry the swapped material";
    ASSERT_TRUE(h.PrimeToFastPath());
}

// ---------------------------------------------------------------------------
// Impl-review F1(b): the FIRST zero->nonzero morph weight edit swaps
// meshGpuHandleId source->runtime with no freed row (fresh registration, no
// E6 event) — the MeshRenderer probe must escalate and the replayed
// submission must carry the runtime mesh handle, not keep drawing the
// unmorphed source indefinitely.
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, FirstMorphWeightEditSwapsSubmissionMeshHandle)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));

    const GUID modelGuid = FixedGuid(0x66);
    auto model = std::make_unique<ModelAsset>(modelGuid,
                                              std::filesystem::path("Synthetic://FastPathMorph"));
    {
        Mesh source{};
        source.Name = "FastPathMorphSource";
        Vertex v0{}, v1{}, v2{};
        v0.Position[1] = 1.0f;
        v1.Position[0] = -1.0f;
        v1.Position[1] = -1.0f;
        v2.Position[0] = 1.0f;
        v2.Position[1] = -1.0f;
        v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
        source.Vertices = {v0, v1, v2};
        source.Indices = {0, 1, 2};
        source.MinBounds[0] = -1.0f;
        source.MinBounds[1] = -1.0f;
        source.MaxBounds[0] = 1.0f;
        source.MaxBounds[1] = 1.0f;
        MorphTarget target{};
        target.Name = "raise";
        target.VertexIndices = {0u};
        target.PositionDeltas = {0.0f, 2.0f, 0.0f};
        source.MorphTargets.push_back(std::move(target));
        Vector<Mesh> meshes;
        meshes.push_back(std::move(source));
        model->SetMeshesForTest(std::move(meshes));
    }

    MorphTargetSystem morphSystem(&h.rs);
    morphSystem.SetModelResolverForTest(
        [&](const GUID& guid) { return guid == modelGuid ? model.get() : nullptr; });

    ECS::Entity morph = h.world->Create();
    morph.Set(MakeTransformAt(0.0f, 0.0f, 0.0f));
    {
        MeshRenderer mr{};
        mr.modelAssetGuid.Set(modelGuid);
        mr.meshId = 0;
        mr.renderLayerMask = 0x1u;
        mr.materialAssetGuid.Set(h.materialGuid);
        morph.Set(mr);
    }
    morph.Set(MorphTargetWeights{});
    morph.Set(LocalBounds{});
    h.world->ProcessCommands();

    // Frame model with the morph system in its production slot (Extraction/2,
    // before extraction at /6).
    const auto stepWithMorph = [&]()
    {
        h.world->ProcessCommands();
        h.hierarchy.Update(*h.world, kDt);
        morphSystem.Update(*h.world, kDt);
        h.extraction->Update(*h.world, kDt);
        h.world->SwapComponentDirtyFeed();
    };

    // Prime: zero weights -> the source mesh renders; idle morph frames take
    // no grants, so the fast path sustains.
    bool primed = false;
    for (int i = 0; i < 6 && !primed; ++i)
    {
        stepWithMorph();
        primed = h.Stats().FastFrame != 0;
    }
    ASSERT_TRUE(primed) << "idle morph scene never reached the fast path";
    const auto* mrRead = h.world->GetComponent<MeshRenderer>(morph.GetHandle());
    ASSERT_NE(mrRead, nullptr);
    const uint64 sourceHandle = mrRead->meshGpuHandleId;
    ASSERT_NE(sourceHandle, 0u);
    ASSERT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);
    ASSERT_EQ(static_cast<uint64>(h.extraction->GetSubmissionsForTest()[0].meshHandle),
              sourceHandle);

    // First weight edit: runtime mesh registered fresh (no row freed, no E6).
    if (auto* w = h.world->GetComponentForWrite<MorphTargetWeights>(morph.GetHandle()))
    {
        w->weights[0] = 1.0f;
        ++w->version;
    }
    stepWithMorph();
    EXPECT_EQ(h.Stats().FastFrame, 0u)
        << "the source->runtime handle swap must escalate via the MeshRenderer probe";
    EXPECT_NE(h.Stats().EscalationReasonBits &
                  RenderExtractionSystem::kEscalationProbeMeshRenderer,
              0u);
    mrRead = h.world->GetComponent<MeshRenderer>(morph.GetHandle());
    ASSERT_NE(mrRead, nullptr);
    ASSERT_NE(mrRead->meshGpuHandleId, sourceHandle) << "morph did not swap the handle";
    ASSERT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);
    EXPECT_EQ(static_cast<uint64>(h.extraction->GetSubmissionsForTest()[0].meshHandle),
              mrRead->meshGpuHandleId)
        << "the replayed submission must draw the morphed runtime mesh, not the source";
}

// ---------------------------------------------------------------------------
// T12 — GPUScene slot churn (the smoke-particle shape) while meshes ride
// cached submissions: foreign slot add/remove must never clobber a mesh slot
// or destabilize the replay.
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, ForeignSlotChurnLeavesCachedSubmissionsStable)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    const uint32 meshSlot = mg->instanceIndex;
    GPUInstance meshBytes = h.rs.GetGPUScene()->GetInstances()[meshSlot];

    // Churn foreign slots the way the smoke simulation does (per-particle
    // AddInstance/RemoveInstance, no ECS writes): frames must stay fast and
    // the mesh slot must stay byte-stable.
    auto* scene = h.rs.GetGPUScene();
    for (int i = 0; i < 6; ++i)
    {
        GPUInstance particle{};
        particle.meshIndex = 0u;
        particle.materialIndex = 0u;
        particle.boundingRadius = 0.1f;
        const uint32 slot = scene->AddInstance(particle);
        EXPECT_NE(slot, meshSlot) << "foreign churn must never claim the mesh slot";
        h.StepFrame();
        EXPECT_EQ(h.Stats().FastFrame, 1u) << "slot churn is not an escalation trigger";
        scene->RemoveInstance(slot);
        h.StepFrame();
        EXPECT_EQ(h.Stats().FastFrame, 1u);
        EXPECT_EQ(0, std::memcmp(&scene->GetInstances()[meshSlot], &meshBytes,
                                 sizeof(GPUInstance)))
            << "mesh instance bytes destabilized at churn round " << i;
        ASSERT_EQ(h.extraction->GetSubmissionsForTest().size(), 1u);
        EXPECT_EQ(h.extraction->GetSubmissionsForTest()[0].instanceIndex, meshSlot);
    }
}

// ---------------------------------------------------------------------------
// T13 — bulk motion stays on the patch lane: motion alone never escalates,
// whatever the mover count (the feed's own overflow cap is E2's).
// ---------------------------------------------------------------------------
namespace
{
std::vector<ECS::Entity> SpawnRenderableGrid(ExtractionHarness& h, int count)
{
    std::vector<ECS::Entity> entities;
    entities.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        entities.push_back(h.SpawnRenderable(static_cast<float>(i % 256), 0.0f,
                                             static_cast<float>(i / 256)));
    return entities;
}

void MoveAll(std::vector<ECS::Entity>& entities, float height)
{
    for (size_t i = 0; i < entities.size(); ++i)
        entities[i].Set(MakeTransformAt(static_cast<float>(i % 256), height,
                                        static_cast<float>(i / 256)));
}
} // namespace

TEST_F(ExtractionFastPathTest, BulkMotionStaysOnThePatchLane)
{
    // 30,000 movers a frame is ~60,000 feed entries (both windows), well past
    // the patch budget the lane used to escalate at, and under the feed's cap.
    constexpr int kMovers = 30000;
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    std::vector<ECS::Entity> entities = SpawnRenderableGrid(h, kMovers);
    ASSERT_TRUE(h.PrimeToFastPath());
    h.StepFrame();
    h.StepFrame();
    ASSERT_EQ(h.Stats().FastFrame, 1u);

    for (int frame = 0; frame < 3; ++frame)
    {
        MoveAll(entities, 2.0f + static_cast<float>(frame));
        h.StepFrame();
        EXPECT_EQ(h.Stats().FastFrame, 1u) << "frame " << frame;
        EXPECT_EQ(h.Stats().EscalationReasonBits, 0u) << "frame " << frame;
        EXPECT_EQ(h.Stats().FeedPatchedCount, static_cast<uint32>(kMovers)) << "frame " << frame;
    }
    const auto& rows = h.rs.GetGPUScene()->GetInstances();
    for (const ECS::Entity& e : entities)
    {
        const auto* meshGpu = h.world->GetComponent<MeshGPUData>(e.GetHandle());
        ASSERT_NE(meshGpu, nullptr);
        EXPECT_EQ(rows[meshGpu->instanceIndex].transform.Data()[13], 4.0f);
    }
}

// ---------------------------------------------------------------------------
// E10: an always-refresh subset past its ceiling pins the parallel full lane —
// heavy-animation scenes keep the tool that fits them (D5/D6).
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, SubsetOverCeilingPinsFullLane)
{
    constexpr int kCeiling = 4096; // kMaxAlwaysRefreshDefault
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    // Animation controllers without a renderable bridge must not count against
    // the ceiling. Their future rendering-component adds are structural changes
    // and will rebuild the subset normally.
    for (int i = 0; i < 8; ++i)
    {
        ECS::Entity controller = h.world->Create();
        controller.Set(AnimatorRef{});
        controller.Set(SkeletonRef{});
        controller.Set(SkinnedMeshRenderer{});
        controller.Set(MorphTargetWeights{});
    }
    h.world->ProcessCommands();
    for (int i = 0; i < kCeiling; ++i)
    {
        ECS::Entity e = h.world->Create();
        e.Set(MakeTransformAt(static_cast<float>(i % 64), 0.0f, static_cast<float>(i / 64)));
        MeshRenderer mr{};
        mr.meshGpuHandleId = static_cast<uint64>(h.meshHandle);
        mr.renderLayerMask = 0x1u;
        mr.materialAssetGuid.Set(h.materialGuid);
        e.Set(mr);
        e.Set(AnimatorRef{});
    }
    h.world->ProcessCommands();
    // Exactly AT the ceiling stays fast (the compare is deliberately >).
    ASSERT_TRUE(h.PrimeToFastPath(8)) << "a subset exactly at the ceiling must not pin the full lane";

    // One more carrier crosses it: pinned thereafter.
    ECS::Entity extra = h.SpawnRenderable(-1.0f, 0.0f, 0.0f);
    extra.Set(AnimatorRef{});
    h.world->ProcessCommands();
    for (int i = 0; i < 4; ++i)
    {
        h.StepFrame();
        EXPECT_EQ(h.Stats().FastFrame, 0u) << "frame " << i;
    }
    EXPECT_NE(h.Stats().EscalationReasonBits & RenderExtractionSystem::kEscalationSubsetOverBudget, 0u);
}

// ---------------------------------------------------------------------------
// T13b — the patch lane on a job system (I6). It forks its build and its row
// writes on the world's pool; with thousands of movers every output must equal
// the same lane run inline (no pool): rows, the frame-mover list in order and
// the caster-change signal. Its rows and movers must also equal the full lane's.
// ---------------------------------------------------------------------------
namespace
{
struct CasterChangeCopy
{
    uint64_t Version = 0;
    bool Unattributed = true;
    std::vector<ShadowCasterChangeSphere> Spheres;
};

struct PoolParityCapture
{
    std::vector<std::vector<GPUInstance>> instancesPerFrame;
    std::vector<std::vector<uint32>> moverIndicesPerFrame; // in list order
    std::vector<CasterChangeCopy> casterChangesPerFrame;
    uint32 fastFrames = 0;
};

// Who puts the movers into the component dirty feed, and in what order. The
// hierarchy appends them in chunk order when only Transform is written. A
// producer that also writes WorldTransform directly (a physics sync, say)
// appends them in its own order; the hierarchy then finds the matrix already
// current and appends nothing.
enum class MoverWriteOrder
{
    Hierarchy,
    DirectForward,
    DirectReverse,
};

void WriteMover(ECS::Entity& entity, const Transform& transform, MoverWriteOrder order)
{
    entity.Set(transform);
    if (order == MoverWriteOrder::Hierarchy)
        return;
    WorldTransform world = *entity.Get<WorldTransform>();
    std::memcpy(world.matrix, transform.matrix, sizeof(world.matrix));
    ++world.Version;
    entity.Set(world);
}

PoolParityCapture RunPoolParityScenario(Rendering::IDevice* device, bool fastPath,
                                        JobSystem::WorkStealingThreadPool* pool, MoverWriteOrder order)
{
    constexpr int kRenderables = 6000;
    constexpr size_t kMovers = 5000; // several fork-join chunks
    constexpr int kFrames = 9;
    PoolParityCapture cap;
    ExtractionHarness h;
    if (!h.Initialize(device, fastPath, pool))
        return cap;
    std::vector<ECS::Entity> entities = SpawnRenderableGrid(h, kRenderables);
    for (int frame = 0; frame < kFrames; ++frame)
    {
        // Move frames 2-6, then stop: frames 7-8 carry the settle rebuild.
        if (frame >= 2 && frame <= 6)
            for (size_t step = 0; step < kMovers; ++step)
            {
                const size_t m = order == MoverWriteOrder::DirectReverse ? kMovers - 1 - step : step;
                WriteMover(entities[m],
                           MakeTransformAt(static_cast<float>(m % 256), 1.0f + static_cast<float>(frame),
                                           static_cast<float>(m / 256)),
                           order);
            }
        const size_t moversBefore = h.rs.GetFrameMovers().size();
        h.StepFrame();
        cap.instancesPerFrame.push_back(MaskedInstances(h.rs.GetGPUScene()->GetInstances()));
        std::vector<uint32> movers;
        for (size_t i = moversBefore; i < h.rs.GetFrameMovers().size(); ++i)
            movers.push_back(h.rs.GetFrameMovers()[i].InstanceIndex);
        cap.moverIndicesPerFrame.push_back(std::move(movers));
        const ShadowCasterChangeSet changes = h.rs.GetShadowCasterChanges(h.worldId);
        cap.casterChangesPerFrame.push_back(
            {changes.Version, changes.Unattributed, {changes.Spheres.begin(), changes.Spheres.end()}});
        if (h.Stats().FastFrame != 0)
            ++cap.fastFrames;
    }
    return cap;
}

std::vector<uint32> Sorted(std::vector<uint32> values)
{
    std::sort(values.begin(), values.end());
    return values;
}
} // namespace

TEST_F(ExtractionFastPathTest, PatchLaneOnAPoolMatchesTheFullLane)
{
    JobSystem::WorkStealingThreadPool pool(4);
    PoolParityCapture full = RunPoolParityScenario(m_Device.get(), false, &pool, MoverWriteOrder::Hierarchy);
    PoolParityCapture inlinePatch = RunPoolParityScenario(m_Device.get(), true, nullptr, MoverWriteOrder::Hierarchy);
    PoolParityCapture poolPatch = RunPoolParityScenario(m_Device.get(), true, &pool, MoverWriteOrder::Hierarchy);
    ASSERT_EQ(full.instancesPerFrame.size(), poolPatch.instancesPerFrame.size());
    ASSERT_EQ(inlinePatch.instancesPerFrame.size(), poolPatch.instancesPerFrame.size());
    ASSERT_FALSE(full.instancesPerFrame.empty());
    EXPECT_EQ(full.fastFrames, 0u);
    EXPECT_GE(poolPatch.fastFrames, 5u) << "the patch lane never carried the motion; parity is vacuous";
    EXPECT_EQ(inlinePatch.fastFrames, poolPatch.fastFrames);
    for (size_t f = 0; f < poolPatch.instancesPerFrame.size(); ++f)
    {
        const auto& rows = poolPatch.instancesPerFrame[f];
        ASSERT_EQ(rows.size(), inlinePatch.instancesPerFrame[f].size()) << "frame " << f;
        ASSERT_EQ(rows.size(), full.instancesPerFrame[f].size()) << "frame " << f;
        EXPECT_EQ(0, std::memcmp(rows.data(), inlinePatch.instancesPerFrame[f].data(),
                                 rows.size() * sizeof(GPUInstance)))
            << "rows diverged from the inline patch lane at frame " << f;
        EXPECT_EQ(0, std::memcmp(rows.data(), full.instancesPerFrame[f].data(),
                                 rows.size() * sizeof(GPUInstance)))
            << "rows diverged from the full lane at frame " << f;
        EXPECT_EQ(poolPatch.moverIndicesPerFrame[f], inlinePatch.moverIndicesPerFrame[f])
            << "frame-mover list (in order) diverged from the inline patch lane at frame " << f;
        EXPECT_EQ(Sorted(poolPatch.moverIndicesPerFrame[f]), Sorted(full.moverIndicesPerFrame[f]))
            << "frame movers diverged from the full lane at frame " << f;
        const CasterChangeCopy& a = poolPatch.casterChangesPerFrame[f];
        const CasterChangeCopy& b = inlinePatch.casterChangesPerFrame[f];
        EXPECT_EQ(a.Version, b.Version) << "caster version diverged at frame " << f;
        EXPECT_EQ(a.Unattributed, b.Unattributed) << "caster attribution diverged at frame " << f;
        ASSERT_EQ(a.Spheres.size(), b.Spheres.size()) << "frame " << f;
        EXPECT_EQ(0, std::memcmp(a.Spheres.data(), b.Spheres.data(),
                                 a.Spheres.size() * sizeof(ShadowCasterChangeSphere)))
            << "caster spheres diverged at frame " << f;
    }
}

// The patch lane orders the dirty-feed snapshot by entity index, so its outputs
// do not depend on the order the producers appended in: movers fed in reverse
// give the same rows, the same frame-mover list in the same order and the same
// caster-change signal as movers fed forward.
TEST_F(ExtractionFastPathTest, PatchLaneOutputsDoNotDependOnTheFeedOrder)
{
    PoolParityCapture forward =
        RunPoolParityScenario(m_Device.get(), true, nullptr, MoverWriteOrder::DirectForward);
    PoolParityCapture reverse =
        RunPoolParityScenario(m_Device.get(), true, nullptr, MoverWriteOrder::DirectReverse);
    ASSERT_EQ(forward.instancesPerFrame.size(), reverse.instancesPerFrame.size());
    ASSERT_FALSE(forward.instancesPerFrame.empty());
    EXPECT_GE(reverse.fastFrames, 5u) << "the patch lane never carried the motion; the comparison is vacuous";
    EXPECT_EQ(forward.fastFrames, reverse.fastFrames);
    for (size_t f = 0; f < reverse.instancesPerFrame.size(); ++f)
    {
        const auto& rows = reverse.instancesPerFrame[f];
        ASSERT_EQ(rows.size(), forward.instancesPerFrame[f].size()) << "frame " << f;
        EXPECT_EQ(0, std::memcmp(rows.data(), forward.instancesPerFrame[f].data(), rows.size() * sizeof(GPUInstance)))
            << "rows depend on the feed order at frame " << f;
        EXPECT_EQ(reverse.moverIndicesPerFrame[f], forward.moverIndicesPerFrame[f])
            << "the frame-mover list (in order) depends on the feed order at frame " << f;
        const CasterChangeCopy& a = reverse.casterChangesPerFrame[f];
        const CasterChangeCopy& b = forward.casterChangesPerFrame[f];
        EXPECT_EQ(a.Unattributed, b.Unattributed) << "caster attribution depends on the feed order at frame " << f;
        ASSERT_EQ(a.Spheres.size(), b.Spheres.size()) << "frame " << f;
        EXPECT_EQ(0, std::memcmp(a.Spheres.data(), b.Spheres.data(),
                                 a.Spheres.size() * sizeof(ShadowCasterChangeSphere)))
            << "caster spheres depend on the feed order at frame " << f;
    }
}

// ---------------------------------------------------------------------------
// T13c — caster attribution on the patch lane (I6, under the sphere cap). With
// fewer moved casters than kMaxChangedCasterSpheres the frame is attributed,
// and each sphere must enclose the caster's previous and current bounds: the
// attribution reads the rows before the batched row write replaces them.
// ---------------------------------------------------------------------------
namespace
{
ShadowCasterChangeSphere EnclosingSphere(const GPUInstance& previous, const GPUInstance& current)
{
    const float dx = current.boundingCenter.x - previous.boundingCenter.x;
    const float dy = current.boundingCenter.y - previous.boundingCenter.y;
    const float dz = current.boundingCenter.z - previous.boundingCenter.z;
    ShadowCasterChangeSphere sphere{};
    sphere.X = 0.5f * (previous.boundingCenter.x + current.boundingCenter.x);
    sphere.Y = 0.5f * (previous.boundingCenter.y + current.boundingCenter.y);
    sphere.Z = 0.5f * (previous.boundingCenter.z + current.boundingCenter.z);
    sphere.Radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) +
                    std::max(previous.boundingRadius, current.boundingRadius);
    return sphere;
}

std::vector<ShadowCasterChangeSphere> SortedSpheres(std::vector<ShadowCasterChangeSphere> spheres)
{
    std::sort(spheres.begin(), spheres.end(),
              [](const ShadowCasterChangeSphere& a, const ShadowCasterChangeSphere& b)
              { return std::tie(a.X, a.Y, a.Z, a.Radius) < std::tie(b.X, b.Y, b.Z, b.Radius); });
    return spheres;
}
} // namespace

TEST_F(ExtractionFastPathTest, PatchLaneAttributesCastersAgainstTheirPreviousPose)
{
    constexpr int kRenderables = 60;
    constexpr size_t kMovers = 40; // under kMaxChangedCasterSpheres (64)
    JobSystem::WorkStealingThreadPool pool(4);
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true, &pool));
    std::vector<ECS::Entity> entities = SpawnRenderableGrid(h, kRenderables);
    ASSERT_TRUE(h.PrimeToFastPath());

    for (int frame = 0; frame < 3; ++frame)
    {
        const std::vector<GPUInstance> before = h.rs.GetGPUScene()->GetInstances();
        for (size_t m = 0; m < kMovers; ++m)
            entities[m].Set(MakeTransformAt(static_cast<float>(m), 2.0f + 3.0f * static_cast<float>(frame),
                                            4.0f * static_cast<float>(frame)));
        const size_t moversBefore = h.rs.GetFrameMovers().size();
        h.StepFrame();
        ASSERT_NE(h.Stats().FastFrame, 0u) << "frame " << frame << " left the patch lane";
        const std::vector<GPUInstance>& after = h.rs.GetGPUScene()->GetInstances();

        std::vector<ShadowCasterChangeSphere> expected;
        for (size_t i = moversBefore; i < h.rs.GetFrameMovers().size(); ++i)
        {
            const uint32 index = h.rs.GetFrameMovers()[i].InstanceIndex;
            expected.push_back(EnclosingSphere(before[index], after[index]));
        }
        ASSERT_EQ(expected.size(), kMovers) << "frame " << frame;

        const ShadowCasterChangeSet changes = h.rs.GetShadowCasterChanges(h.worldId);
        ASSERT_FALSE(changes.Unattributed) << "frame " << frame << " lost attribution under the cap";
        const std::vector<ShadowCasterChangeSphere> actual =
            SortedSpheres({changes.Spheres.begin(), changes.Spheres.end()});
        expected = SortedSpheres(std::move(expected));
        ASSERT_EQ(actual.size(), expected.size()) << "frame " << frame;
        for (size_t i = 0; i < actual.size(); ++i)
        {
            EXPECT_FLOAT_EQ(actual[i].X, expected[i].X) << "frame " << frame << " sphere " << i;
            EXPECT_FLOAT_EQ(actual[i].Y, expected[i].Y) << "frame " << frame << " sphere " << i;
            EXPECT_FLOAT_EQ(actual[i].Z, expected[i].Z) << "frame " << frame << " sphere " << i;
            EXPECT_FLOAT_EQ(actual[i].Radius, expected[i].Radius) << "frame " << frame << " sphere " << i;
        }
    }
}

// ---------------------------------------------------------------------------
// T14 — multi-step window (editor passive refresh): a second extraction run
// within one swap window is all no-ops and replays submissions per step.
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, MultiStepWindowIsIdempotent)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity e = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    e.Set(MakeTransformAt(0.0f, 5.0f, 0.0f));
    h.world->ProcessCommands();
    h.hierarchy.Update(*h.world, kDt);

    // Step 1 of the window: patches the mover.
    h.extraction->Update(*h.world, kDt);
    EXPECT_EQ(h.Stats().FastFrame, 1u);
    EXPECT_EQ(h.Stats().FeedPatchedCount, 1u);
    EXPECT_EQ(h.Stats().SubmissionCount, 1u);

    // Step 2, same window (no swap): re-delivery is a Version-equal no-op;
    // submissions are still submitted for the step.
    h.extraction->Update(*h.world, kDt);
    EXPECT_EQ(h.Stats().FastFrame, 1u);
    EXPECT_EQ(h.Stats().FeedPatchedCount, 0u);
    EXPECT_EQ(h.Stats().SubmissionCount, 1u);

    auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    ASSERT_NE(mg, nullptr);
    EXPECT_FLOAT_EQ(h.rs.GetGPUScene()->GetInstances()[mg->instanceIndex].transform.Data()[13],
                    5.0f);
    h.world->SwapComponentDirtyFeed();
    ASSERT_TRUE(h.PrimeToFastPath());
}

TEST_F(ExtractionFastPathTest, DirtyBatchDeduplicatesHandlesAndGrantsOnlyActualWrites)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    std::vector<ECS::Entity> entities;
    for (int i = 0; i < 12; ++i)
        entities.push_back(h.SpawnRenderable(static_cast<float>(i), 0.0f, 0.0f));
    // Keep an idle renderer in a separate archetype so another entity's
    // legitimate chunk-level stamp cannot hide an accidental idle write grant.
    entities.back().Set(LocalBounds{});
    ASSERT_TRUE(h.PrimeToFastPath());
    h.StepFrame(); // drain bootstrap feed entries

    const auto before = h.rs.GetGPUScene()->GetInstances();
    constexpr int moverIndices[] = {1, 5, 9};
    for (const int i : moverIndices)
        entities[i].Set(MakeTransformAt(static_cast<float>(i), 8.0f, 0.0f));
    h.world->ProcessCommands();
    h.hierarchy.Update(*h.world, kDt);

    // Interleave duplicates, stale generations of live indices, invalid handles,
    // and live static renderers. Candidate order must not affect motion history
    // or let a stale generation suppress the live handle sharing its index.
    const auto transformType = GetComponentTypeId<WorldTransform>();
    for (int repeat = 0; repeat < 4; ++repeat)
    {
        h.world->EmitComponentDirty(transformType, EntityHandle::Invalid());
        for (int i = static_cast<int>(entities.size()) - 1; i >= 0; --i)
        {
            const auto handle = entities[i].GetHandle();
            h.world->EmitComponentDirty(transformType,
                                        EntityHandle(handle.id ^ (1u << kEntityIndexBits)));
            h.world->EmitComponentDirty(transformType, handle);
        }
    }
    const auto bridgeType = GetComponentTypeId<MeshGPUData>();
    const auto idleHandle = entities.back().GetHandle();
    const uint64 idleStamp = h.world->GetEntityColumnVersion(idleHandle, bridgeType);
    const uint64 readStamp = h.world->GetEntityColumnVersion(idleHandle, transformType);
    const uint64 stampsBefore = h.world->GetColumnStampCount();
    h.extraction->Update(*h.world, kDt);
    ASSERT_EQ(h.Stats().FastFrame, 1u);
    EXPECT_EQ(h.Stats().FeedPatchedCount, 3u);
    // The three movers share one chunk, and the batch stamps a same-chunk run
    // once; the idle entity's versions below show no grant reached it.
    EXPECT_EQ(h.world->GetColumnStampCount() - stampsBefore, 1u);
    EXPECT_EQ(h.world->GetEntityColumnVersion(idleHandle, bridgeType), idleStamp);
    EXPECT_EQ(h.world->GetEntityColumnVersion(idleHandle, transformType), readStamp);

    for (int i = 0; i < static_cast<int>(entities.size()); ++i)
    {
        const auto* bridge = h.world->GetComponent<MeshGPUData>(entities[i].GetHandle());
        ASSERT_NE(bridge, nullptr);
        const uint32 slot = bridge->instanceIndex;
        const auto& actual = h.rs.GetGPUScene()->GetInstances()[slot];
        const bool moving = i == 1 || i == 5 || i == 9;
        if (moving)
        {
            EXPECT_FLOAT_EQ(actual.transform.Data()[13], 8.0f);
            EXPECT_EQ(std::memcmp(actual.prevTransform.Data(), before[slot].transform.Data(),
                                  sizeof(float) * 16), 0);
            EXPECT_TRUE(bridge->LastPrevDiffers);
        }
        else
            EXPECT_EQ(std::memcmp(&actual, &before[slot], sizeof(GPUInstance)), 0);
    }

    const uint64 stampsAfter = h.world->GetColumnStampCount();
    h.extraction->Update(*h.world, kDt); // same window: no early settle or grants
    EXPECT_EQ(h.Stats().FeedPatchedCount, 0u);
    EXPECT_EQ(h.world->GetColumnStampCount(), stampsAfter);

    h.world->SwapComponentDirtyFeed();
    h.StepFrame(); // next window: each stopped mover settles exactly once
    EXPECT_EQ(h.Stats().FeedPatchedCount, 3u);
    h.StepFrame();
    EXPECT_EQ(h.Stats().FeedPatchedCount, 0u);
}

// ---------------------------------------------------------------------------
// S0 composition assertion: the stats surface publishes the composition
// counts the bench post-processing asserts on ("residual ~= CLEANUP" only
// holds for particle/volume/ocean-free scenes), and they respond to content.
// ---------------------------------------------------------------------------
TEST_F(ExtractionFastPathTest, CompositionCountsPublishedForResidualAttribution)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    h.StepFrame();
    EXPECT_EQ(h.Stats().ParticleEmitterCount, 0u);
    EXPECT_EQ(h.Stats().VolumeCount, 0u);
    EXPECT_EQ(h.Stats().OceanCount, 0u);

    // Positive control: an enabled ocean flips its count (and the digest —
    // the ocean term is re-added per frame on both lanes).
    const uint64 digestBefore = h.rs.GetWorldRenderContentDigest(h.worldId);
    ECS::Entity ocean = h.world->Create();
    ocean.Set(OceanRenderer{});
    h.world->ProcessCommands();
    h.StepFrame();
    EXPECT_EQ(h.Stats().OceanCount, 1u);
    EXPECT_NE(h.rs.GetWorldRenderContentDigest(h.worldId), digestBefore);

    // Positive control: an emitter running the default stack (eight particles a second) is counted,
    // with the particles its simulation keeps alive and the simulation's own time.
    ECS::Entity emitter = h.world->Create();
    emitter.Set(MakeTransformAt(0.0f, 0.0f, 0.0f));
    emitter.Set(ParticleEmitter3D{});
    h.world->ProcessCommands();
    for (int frame = 0; frame < 30; ++frame)
        h.StepFrame();
    EXPECT_EQ(h.Stats().ParticleEmitterCount, 1u);
    EXPECT_GT(h.Stats().LiveParticles, 0u);
    EXPECT_GT(h.Stats().ParticleSimulationMs, 0.0f);
}

// Moving particles change a view's pixels without moving an instance or advancing a content
// epoch, so extraction reports unversioned motion for each view that draws them, and the TAA
// certification reads the report as a mover. A view whose frustum holds none of them, and a view
// of particles frozen by a paused emitter, report nothing.
TEST_F(ExtractionFastPathTest, MovingParticlesReportUnversionedMotionForTheViewsThatDrawThem)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    // A second view of the same world whose clip volume is 100 units to the side.
    CameraData away = MakeIdentityCamera();
    away.viewProj[12] = 100.0f;
    const CameraId awayCamera = h.rs.Views().AllocateCamera("AwayCamera");
    h.rs.Views().SetCameraData(awayCamera, away);
    const ViewId awayView = h.rs.Views().AllocateView("AwayView", awayCamera);
    h.rs.Views().SetViewWorldId(awayView, h.worldId);
    const auto step = [&h]
    {
        h.rs.BeginWorldDrawFrame();
        h.StepFrame();
    };

    step();
    EXPECT_FALSE(h.rs.HasUnversionedMotion(h.viewId)) << "no emitter: nothing moves unversioned";

    ECS::Entity emitter = h.world->Create();
    emitter.Set(MakeTransformAt(0.0f, -0.9f, 0.5f));
    emitter.Set(ParticleEmitter3D{});
    h.world->ProcessCommands();
    for (int frame = 0; frame < 30; ++frame)
        step();
    ASSERT_GT(h.Stats().LiveParticles, 0u) << "positive control: the emitter must be alive";
    // This harness compiles no shaders: give the particle material the stand-in pipeline and
    // GPUScene slot the harness's own material has, so the particles are drawn.
    h.rs.Materials().Registry().ForEachMutable(
        [](const GUID&, Material& material)
        {
            if (material.GetGraphicsPipelineId().IsValid())
                return;
            Material::TestFactory::SetGraphicsPipelineId(material, Rendering::GraphicsPipelineId{1u});
            Material::TestFactory::SetGpuSceneMaterialIndex(material, 1u);
        });
    step();
    EXPECT_TRUE(h.rs.HasUnversionedMotion(h.viewId)) << "the view draws moving particles";
    EXPECT_FALSE(h.rs.HasUnversionedMotion(awayView)) << "no particle survives this view's frustum";

    ParticlePlayback paused{};
    paused.Paused = true;
    emitter.Set(paused);
    h.world->ProcessCommands();
    step();
    step();
    ASSERT_GT(h.Stats().LiveParticles, 0u) << "a paused emitter keeps its particles";
    EXPECT_FALSE(h.rs.HasUnversionedMotion(h.viewId)) << "frozen particles do not move";

    emitter.Set(ParticlePlayback{});
    h.world->ProcessCommands();
    step();
    EXPECT_TRUE(h.rs.HasUnversionedMotion(h.viewId)) << "the first frame after the pause moves again";
}

// A renderer left at its defaults sorts every particle by its own depth, across emitters: two emitters
// at one origin, each with its own material, interleave in the view's draws instead of drawing one
// after the other.
TEST_F(ExtractionFastPathTest, DefaultParticleDrawOrderInterleavesEmittersByParticleDepth)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    for (uint32 index = 0; index < 2; ++index)
    {
        ECS::Entity emitter = h.world->Create();
        // Inside the harness camera's unit clip volume while the particles rise for a second.
        emitter.Set(MakeTransformAt(0.0f, -0.9f, 0.5f));
        ParticleEmitter3D settings{};
        settings.Seed = 11u + index;
        emitter.Set(settings);
        ParticleRenderer renderer{};
        renderer.Lighting = index == 0 ? ParticleLightingMode::Unlit : ParticleLightingMode::Lit;
        emitter.Set(renderer);
    }
    h.world->ProcessCommands();
    for (int frame = 0; frame < 60; ++frame)
        h.StepFrame();
    ASSERT_EQ(h.Stats().ParticleEmitterCount, 2u);
    ASSERT_GT(h.Stats().LiveParticles, 8u);
    // This harness compiles no shaders: give the particle materials the stand-in pipeline and GPUScene
    // slot the harness's own material has, so their particles reach the draw stream.
    uint32 materialIndex = 1;
    h.rs.Materials().Registry().ForEachMutable(
        [&materialIndex](const GUID&, Material& material)
        {
            if (material.GetGraphicsPipelineId().IsValid())
                return;
            Material::TestFactory::SetGraphicsPipelineId(material, Rendering::GraphicsPipelineId{1u});
            Material::TestFactory::SetGpuSceneMaterialIndex(material, materialIndex++);
        });
    ASSERT_EQ(materialIndex, 3u) << "one material per emitter";
    h.StepFrame();
    h.rs.EmitProducerForwardCommandsForView(h.viewId);
    const auto commands = h.rs.GetLateForwardCommands(h.viewId);
    ASSERT_FALSE(commands.empty()) << "positive control: the particles must reach the view";
    EXPECT_GT(commands.size(), 2u) << "one run per emitter means the particles were not sorted by their own depth";
}

// The view's particles reach the pass farthest first. Two emitters whose particles do not overlap in
// depth draw as one run each, the farther emitter's run first, whatever order they were created in
// (the nearer one first here). The identity camera makes view depth the world z.
TEST_F(ExtractionFastPathTest, TheFartherEmittersParticlesDrawFirst)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    const float depths[] = {0.2f, 0.8f};
    for (uint32 index = 0; index < 2; ++index)
    {
        ECS::Entity emitter = h.world->Create();
        emitter.Set(MakeTransformAt(0.0f, -0.9f, depths[index]));
        ParticleEmitter3D settings{};
        settings.Seed = 21u + index;
        emitter.Set(settings);
        ParticleRenderer renderer{};
        renderer.Lighting = index == 0 ? ParticleLightingMode::Lit : ParticleLightingMode::Unlit;
        emitter.Set(renderer);
    }
    h.world->ProcessCommands();
    for (int frame = 0; frame < 20; ++frame)
        h.StepFrame();
    uint32 materialIndex = 1;
    h.rs.Materials().Registry().ForEachMutable(
        [&materialIndex](const GUID&, Material& material)
        {
            if (material.GetGraphicsPipelineId().IsValid())
                return;
            Material::TestFactory::SetGraphicsPipelineId(material, Rendering::GraphicsPipelineId{1u});
            Material::TestFactory::SetGpuSceneMaterialIndex(material, materialIndex++);
        });
    ASSERT_EQ(materialIndex, 3u) << "one material per emitter";
    h.StepFrame();
    h.rs.EmitProducerForwardCommandsForView(h.viewId);
    const auto commands = h.rs.GetLateForwardCommands(h.viewId);
    ASSERT_EQ(commands.size(), 2u) << "positive control: one run per emitter, their particles apart in depth";
    ASSERT_NE(commands.front().Material, nullptr);
    ASSERT_NE(commands.back().Material, nullptr);
    EXPECT_EQ(commands.front().Material->GetLightingModel(), Rendering::LightingModel::kUnlit)
        << "the farther (unlit) emitter's particles must draw first";
    EXPECT_NE(commands.back().Material->GetLightingModel(), Rendering::LightingModel::kUnlit)
        << "the nearer (lit) emitter's particles must draw last";
}

// Emitters whose renderers match draw with one shared material, so their particles, sorted together
// by depth, merge into one run: one draw for both emitters.
TEST_F(ExtractionFastPathTest, EmittersSharingAMaterialDrawInOneRun)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    for (uint32 index = 0; index < 2; ++index)
    {
        ECS::Entity emitter = h.world->Create();
        emitter.Set(MakeTransformAt(0.0f, -0.9f, 0.5f));
        ParticleEmitter3D settings{};
        settings.Seed = 11u + index;
        emitter.Set(settings);
        emitter.Set(ParticleRenderer{});
    }
    h.world->ProcessCommands();
    for (int frame = 0; frame < 60; ++frame)
        h.StepFrame();
    ASSERT_EQ(h.Stats().ParticleEmitterCount, 2u);
    ASSERT_GT(h.Stats().LiveParticles, 8u);
    uint32 materialIndex = 1;
    h.rs.Materials().Registry().ForEachMutable(
        [&materialIndex](const GUID&, Material& material)
        {
            if (material.GetGraphicsPipelineId().IsValid())
                return;
            Material::TestFactory::SetGraphicsPipelineId(material, Rendering::GraphicsPipelineId{1u});
            Material::TestFactory::SetGpuSceneMaterialIndex(material, materialIndex++);
        });
    ASSERT_EQ(materialIndex, 2u) << "positive control: matching renderers share one material";
    h.StepFrame();
    h.rs.EmitProducerForwardCommandsForView(h.viewId);
    const auto commands = h.rs.GetLateForwardCommands(h.viewId);
    ASSERT_FALSE(commands.empty()) << "positive control: the particles must reach the view";
    EXPECT_EQ(commands.size(), 1u) << "particles of one material must merge into one draw across emitters";
}

// Every distinct renderer setting is a distinct particle material, and an emitter walks through many
// of them while a value is dragged in the inspector. A document no emitter draws with any more gives
// its registration back, so the registry does not grow with the edits.
TEST_F(ExtractionFastPathTest, ParticleMaterialEditsReuseTheirRegistration)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity emitter = h.world->Create();
    emitter.Set(MakeTransformAt(0.0f, 0.0f, 0.0f));
    emitter.Set(ParticleEmitter3D{});
    emitter.Set(ParticleRenderer{});
    h.world->ProcessCommands();
    for (int frame = 0; frame < 30; ++frame)
        h.StepFrame();
    ASSERT_GT(h.Stats().LiveParticles, 0u) << "positive control: the emitter must be drawing";
    const size_t registered = h.rs.Materials().Registry().GetMaterialCount();

    constexpr int kEdits = 20;
    for (int edit = 1; edit <= kEdits; ++edit)
    {
        ParticleRenderer renderer{};
        renderer.EmissionIntensity = static_cast<float>(edit);
        emitter.Set(renderer);
        h.StepFrame();
    }
    EXPECT_EQ(h.Stats().ParticleEmitterCount, 1u);
    EXPECT_LE(h.rs.Materials().Registry().GetMaterialCount(), registered + 1u);
}

// ---------------------------------------------------------------------------
// prevTransform motion-vector contract (TAA P0): while an instance moves, its
// GPUInstance carries {transform = T_f, prevTransform = T_{f-1}}; when it
// stops, the LastPrevDiffers latch forces exactly ONE settle rebuild that
// advances prev := current (else the frozen version gate would leave a
// permanent phantom motion vector — the TAA ghost-streak bug), after which
// the instance re-enters Skip and its bytes are stable. Covered on both the
// fast (feed-patch) lane and the full lane.
// ---------------------------------------------------------------------------
namespace
{

// The instance's two matrix columns, captured for cross-frame comparison.
struct MatrixPair
{
    float transform[16];
    float prev[16];
};

MatrixPair CaptureMatrices(const ExtractionHarness& h, uint32 slot)
{
    MatrixPair pair{};
    const auto& inst = h.rs.GetGPUScene()->GetInstances()[slot];
    std::memcpy(pair.transform, inst.transform.Data(), sizeof(pair.transform));
    std::memcpy(pair.prev, inst.prevTransform.Data(), sizeof(pair.prev));
    return pair;
}

bool SameMatrix(const float a[16], const float b[16])
{
    return std::memcmp(a, b, sizeof(float) * 16) == 0;
}

} // namespace

TEST_F(ExtractionFastPathTest, PrevTransformLagsWhileMovingAndSettlesOnStop)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity mover = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    h.SpawnRenderable(3.0f, 0.0f, 0.0f); // a static neighbor that must stay quiescent
    ASSERT_TRUE(h.PrimeToFastPath());

    auto* mg = h.world->GetComponent<MeshGPUData>(mover.GetHandle());
    ASSERT_NE(mg, nullptr);
    const uint32 slot = mg->instanceIndex;
    ASSERT_NE(slot, 0xFFFFFFFFu);

    // Spawn frames: prev == transform (no history).
    const MatrixPair rest = CaptureMatrices(h, slot);
    EXPECT_TRUE(SameMatrix(rest.prev, rest.transform));

    // Move for three frames: prev must lag transform by exactly one frame.
    MatrixPair before = rest;
    for (int f = 0; f < 3; ++f)
    {
        mover.Set(MakeTransformAt(0.0f, 1.0f + static_cast<float>(f), 0.0f));
        h.StepFrame();
        const MatrixPair now = CaptureMatrices(h, slot);
        EXPECT_FALSE(SameMatrix(now.transform, before.transform)) << "frame " << f;
        EXPECT_TRUE(SameMatrix(now.prev, before.transform))
            << "prevTransform must equal LAST frame's transform (frame " << f << ")";
        const auto* mgNow = h.world->GetComponent<MeshGPUData>(mover.GetHandle());
        ASSERT_NE(mgNow, nullptr);
        EXPECT_TRUE(mgNow->LastPrevDiffers) << "latch must be set while moving";
        before = now;
    }

    // Stop. The dirty feed re-delivers the last write for one more window;
    // that re-delivery is the settle rebuild: prev := current, latch clears.
    h.StepFrame();
    const MatrixPair settled = CaptureMatrices(h, slot);
    EXPECT_TRUE(SameMatrix(settled.transform, before.transform));
    EXPECT_TRUE(SameMatrix(settled.prev, settled.transform))
        << "one frame after the stop, prevTransform must have advanced to current";
    {
        const auto* mgNow = h.world->GetComponent<MeshGPUData>(mover.GetHandle());
        ASSERT_NE(mgNow, nullptr);
        EXPECT_FALSE(mgNow->LastPrevDiffers);
    }

    // Quiescence: the following frame patches nothing and the bytes hold.
    h.StepFrame();
    EXPECT_NE(h.Stats().FastFrame, 0u);
    EXPECT_EQ(h.Stats().FeedPatchedCount, 0u)
        << "a settled mover must not keep re-patching";
    const MatrixPair after = CaptureMatrices(h, slot);
    EXPECT_TRUE(SameMatrix(after.transform, settled.transform));
    EXPECT_TRUE(SameMatrix(after.prev, settled.prev));
}

TEST_F(ExtractionFastPathTest, PrevTransformSettlesOnFullLaneToo)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), false)); // full lane every frame
    ECS::Entity mover = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    h.StepFrame();
    h.StepFrame();

    auto* mg = h.world->GetComponent<MeshGPUData>(mover.GetHandle());
    ASSERT_NE(mg, nullptr);
    const uint32 slot = mg->instanceIndex;
    ASSERT_NE(slot, 0xFFFFFFFFu);

    MatrixPair before = CaptureMatrices(h, slot);
    mover.Set(MakeTransformAt(0.0f, 5.0f, 0.0f));
    h.StepFrame();
    const MatrixPair moved = CaptureMatrices(h, slot);
    EXPECT_TRUE(SameMatrix(moved.prev, before.transform));
    EXPECT_FALSE(SameMatrix(moved.prev, moved.transform));

    // Stop: the full lane's LastPrevDiffers gate forces one settle rebuild.
    h.StepFrame();
    const MatrixPair settled = CaptureMatrices(h, slot);
    EXPECT_TRUE(SameMatrix(settled.prev, settled.transform));
    EXPECT_TRUE(SameMatrix(settled.transform, moved.transform));

    // And exactly one: the next frame skips (no rebuild for this instance).
    const uint32 rebuildsBefore = h.Stats().RebuildCount;
    h.StepFrame();
    EXPECT_EQ(h.Stats().RebuildCount, 0u)
        << "post-settle frame must skip every instance (had " << rebuildsBefore
        << " rebuilds on the settle frame)";
    const MatrixPair after = CaptureMatrices(h, slot);
    EXPECT_TRUE(SameMatrix(after.prev, settled.prev));
    EXPECT_TRUE(SameMatrix(after.transform, settled.transform));
}

TEST_F(ExtractionFastPathTest, FrameMoversListCarriesMoversNotSettles)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity mover = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    auto* mg = h.world->GetComponent<MeshGPUData>(mover.GetHandle());
    ASSERT_NE(mg, nullptr);
    const uint32 slot = mg->instanceIndex;

    // The harness never calls BeginWorldDrawFrame (which clears the list), so
    // assert on the per-step DELTA of the accumulating list.
    const size_t countBefore = h.rs.GetFrameMovers().size();
    mover.Set(MakeTransformAt(0.0f, 9.0f, 0.0f));
    h.StepFrame();
    const auto moversAfterMove = h.rs.GetFrameMovers();
    ASSERT_GT(moversAfterMove.size(), countBefore) << "a moving instance must be listed";
    bool found = false;
    for (size_t i = countBefore; i < moversAfterMove.size(); ++i)
        found = found || (moversAfterMove[i].InstanceIndex == slot &&
                          static_cast<uint64>(moversAfterMove[i].MeshHandle) ==
                              static_cast<uint64>(h.meshHandle));
    EXPECT_TRUE(found);

    // The settle frame writes prev := current (zero object MV) — not a mover.
    const size_t countAfterMove = moversAfterMove.size();
    h.StepFrame();
    EXPECT_EQ(h.rs.GetFrameMovers().size(), countAfterMove)
        << "the settle rebuild must not enter the movers list";
}

// A skinned mover's PREVIOUS palette offset must distinguish "no pose last
// frame" from "same offset as last frame". Steady-state atlas offsets are
// frame-STABLE (the atlas bump-allocates in a stable order), so encoding "no
// history" as Previous := Current is indistinguishable from the normal case —
// and on a spawn frame it points the skinned MV stage at last frame's atlas at
// an offset that belonged to whatever runtime occupied that range.
TEST_F(ExtractionFastPathTest, SkinnedMoverCarriesNoPreviousPaletteSentinelOnSpawnFrame)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));

    auto& skStore = SkeletonStore::Instance();
    const uint32 skeletonId = skStore.CreateSkeleton(1);
    ASSERT_NE(skeletonId, 0u);
    if (auto* skel = skStore.Get(skeletonId))
    {
        skel->Parent = {-1};
        skel->InverseBind.resize(16);
        SetIdentityMatrix(skel->InverseBind.data());
        skel->BindPose.resize(16);
        SetIdentityMatrix(skel->BindPose.data());
        skel->RestTranslation = {0.0f, 0.0f, 0.0f};
        skel->RestRotation = {0.0f, 0.0f, 0.0f, 1.0f};
        skel->RestScale = {1.0f, 1.0f, 1.0f};
        skel->SkinJointCount = 1;
        skel->JointNodes = {0};
    }
    const uint32 runtimeId = skStore.CreateRuntime(skeletonId);
    ASSERT_NE(runtimeId, 0u);
    auto* runtime = skStore.GetRuntime(runtimeId);
    ASSERT_NE(runtime, nullptr);
    runtime->CompactSkinMatrices.resize(12, 0.0f);

    ECS::Entity skinned = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    SkinnedMeshRenderer smr{};
    smr.skeletonId = skeletonId;
    skinned.Set(smr);
    SkeletonRef skelRef{};
    skelRef.skeletonId = skeletonId;
    skelRef.runtimeId = runtimeId;
    skinned.Set(skelRef);
    AnimatorRef anim{};
    anim.ClipIndex = 1; // hasActiveAnimation
    skinned.Set(anim);

    // Spawn frame: a palette was published, but RollPaletteOffsets has never
    // aged one into the previous-frame field, so there is no previous pose.
    constexpr uint32 kFirstOffset = 64u;
    runtime->SetAtlasPaletteOffset(kFirstOffset);
    h.world->ProcessCommands();
    ASSERT_TRUE(h.PrimeToFastPath());

    auto* mgSkinned = h.world->GetComponent<MeshGPUData>(skinned.GetHandle());
    ASSERT_NE(mgSkinned, nullptr);
    const uint32 slot = mgSkinned->instanceIndex;

    auto findMover = [&](size_t from) -> const RenderServices::FrameMoverRecord* {
        const auto movers = h.rs.GetFrameMovers();
        for (size_t i = movers.size(); i > from; --i)
        {
            if (movers[i - 1].InstanceIndex == slot)
                return &movers[i - 1];
        }
        return nullptr;
    };

    size_t countBefore = h.rs.GetFrameMovers().size();
    runtime->SetAtlasPaletteOffset(kFirstOffset);
    h.StepFrame();
    const RenderServices::FrameMoverRecord* spawnMover = findMover(countBefore);
    ASSERT_NE(spawnMover, nullptr) << "a palette-active instance must be listed as a mover";
    EXPECT_EQ(spawnMover->SkinPaletteOffset, kFirstOffset);
    EXPECT_EQ(spawnMover->PrevSkinPaletteOffset, RenderServices::kNoPreviousSkinPalette)
        << "with no previous pose the consumer must be told to use the CURRENT atlas, "
           "not handed an offset into last frame's ring";

    // Now age the published offset into the previous-frame field and publish a
    // new one: the mover must carry the real previous offset, not the sentinel.
    skStore.RollPaletteOffsets();
    constexpr uint32 kSecondOffset = 128u;
    runtime->SetAtlasPaletteOffset(kSecondOffset);
    countBefore = h.rs.GetFrameMovers().size();
    h.StepFrame();
    const RenderServices::FrameMoverRecord* steadyMover = findMover(countBefore);
    ASSERT_NE(steadyMover, nullptr);
    EXPECT_EQ(steadyMover->SkinPaletteOffset, kSecondOffset);
    EXPECT_EQ(steadyMover->PrevSkinPaletteOffset, kFirstOffset)
        << "a runtime with pose history must pair its current offset with last frame's";

    skStore.ReleaseRuntime(runtimeId);
}

// ---------------------------------------------------------------------------
// T18 — continuity, not batch identity. The per-slot continuity stamp is the
// processor-side half of "may a consumer trust state it carried for this
// instance from the previous frame". It must move for events the batch key
// cannot see (a new tenant joining a settled batch, a recycled slot, a mesh
// reloaded behind a stable handle) and must NOT move for the two inputs whose
// history is exact — an idle frame and a transform-only move — or every mover
// would be permanently discontinuous.
// ---------------------------------------------------------------------------
namespace
{

struct BatchKeySnapshot
{
    const Material* material = nullptr;
    uint64 mesh = 0;
    uint32 materialIndex = 0;
    uint32 meshIndex = 0;
    uint32 colorClassId = 0;
    bool operator==(const BatchKeySnapshot&) const = default;
};

// The batch keys WorldDrawBuilder derives from this frame's submissions — the
// identity a key-presence check would have compared. Snapshotted by value so
// two frames can be compared after the builder's span is gone.
std::vector<BatchKeySnapshot> SnapshotBatchKeys(ExtractionHarness& h)
{
    h.rs.BeginWorldDrawFrame();
    h.rs.SubmitWorldSubmissions(h.extraction->GetSubmissionsForTest());
    h.rs.BuildWorldBatchKeys();
    std::vector<BatchKeySnapshot> out;
    for (const auto& key : h.rs.GetEntityBatchKeys(h.viewId))
        out.push_back({key.material, static_cast<uint64>(key.mesh), key.materialIndex,
                       key.meshIndex, key.colorClassId});
    return out;
}

uint32 SlotOf(const ExtractionHarness& h, ECS::Entity e)
{
    const auto* mg = h.world->GetComponent<MeshGPUData>(e.GetHandle());
    return mg ? mg->instanceIndex : 0xFFFFFFFFu;
}

uint32 StampOf(const ExtractionHarness& h, uint32 slot)
{
    return h.rs.GetGPUScene()->GetInstanceContinuityStamp(slot);
}

} // namespace

TEST_F(ExtractionFastPathTest, ContinuityStampHoldsAcrossIdleAndTransformOnlyFrames)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity mover = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ECS::Entity neighbor = h.SpawnRenderable(3.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());

    const uint32 moverSlot = SlotOf(h, mover);
    const uint32 neighborSlot = SlotOf(h, neighbor);
    ASSERT_NE(moverSlot, 0xFFFFFFFFu);
    const uint32 moverStamp = StampOf(h, moverSlot);
    const uint32 neighborStamp = StampOf(h, neighborSlot);
    EXPECT_NE(moverStamp, 0u);

    // Idle frames on the fast path: no census, no movement.
    for (int f = 0; f < 3; ++f)
    {
        h.StepFrame();
        EXPECT_NE(h.Stats().FastFrame, 0u) << "positive control: the idle path ran";
    }
    EXPECT_EQ(StampOf(h, moverSlot), moverStamp);

    // Transform-only motion is the input prevTransform reproduces exactly; it
    // must never break continuity, on the fast lane or after a settle.
    for (int f = 0; f < 3; ++f)
    {
        mover.Set(MakeTransformAt(0.0f, 1.0f + static_cast<float>(f), 0.0f));
        h.StepFrame();
        EXPECT_EQ(StampOf(h, moverSlot), moverStamp) << "moving frame " << f;
    }
    h.StepFrame(); // settle rebuild (prev := current)
    h.StepFrame();
    EXPECT_EQ(StampOf(h, moverSlot), moverStamp);
    EXPECT_EQ(StampOf(h, neighborSlot), neighborStamp);

    // The same holds when every frame takes the full lane.
    ExtractionHarness full;
    ASSERT_TRUE(full.Initialize(m_Device.get(), false));
    ECS::Entity fullMover = full.SpawnRenderable(0.0f, 0.0f, 0.0f);
    full.StepFrame();
    full.StepFrame();
    const uint32 fullSlot = SlotOf(full, fullMover);
    const uint32 fullStamp = StampOf(full, fullSlot);
    fullMover.Set(MakeTransformAt(0.0f, 5.0f, 0.0f));
    full.StepFrame();
    full.StepFrame();
    EXPECT_EQ(StampOf(full, fullSlot), fullStamp);
}

TEST_F(ExtractionFastPathTest, ContinuityStampMovesForNewAndRecycledTenantsWhileTheBatchKeyHolds)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    ECS::Entity anchor = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());
    const std::vector<BatchKeySnapshot> settledKeys = SnapshotBatchKeys(h);
    ASSERT_EQ(settledKeys.size(), 1u);
    const uint32 anchorSlot = SlotOf(h, anchor);
    const uint32 anchorStamp = StampOf(h, anchorSlot);

    // A brand-new instance joins the settled batch: the key set is identical,
    // the newcomer carries its own stamp and the anchor keeps its own.
    ECS::Entity joiner = h.SpawnRenderable(1.0f, 0.0f, 0.0f);
    h.StepFrame();
    EXPECT_EQ(SnapshotBatchKeys(h), settledKeys) << "the newcomer joins the existing key";
    const uint32 joinerSlot = SlotOf(h, joiner);
    ASSERT_NE(joinerSlot, 0xFFFFFFFFu);
    const uint32 joinerStamp = StampOf(h, joinerSlot);
    EXPECT_NE(joinerStamp, 0u);
    EXPECT_EQ(StampOf(h, anchorSlot), anchorStamp);

    // Destroy the joiner and let a new entity recycle its slot: same key set,
    // same slot index, different tenant, different stamp. The harness world
    // deliberately carries no RenderWorldHooks (they reach the scene through
    // EngineCore), so the slot is released here exactly as
    // ReleaseMeshGpuInstance releases it on entity destruction.
    h.rs.GetGPUScene()->RemoveInstance(joinerSlot);
    h.world->DestroyEntity(joiner.GetHandle());
    h.StepFrame();
    ECS::Entity tenant = h.SpawnRenderable(2.0f, 0.0f, 0.0f);
    h.StepFrame();
    ASSERT_EQ(SlotOf(h, tenant), joinerSlot) << "precondition: the freed slot was recycled";
    EXPECT_EQ(SnapshotBatchKeys(h), settledKeys) << "the recycled tenant joins the same key";
    EXPECT_NE(StampOf(h, joinerSlot), joinerStamp);
    EXPECT_EQ(StampOf(h, anchorSlot), anchorStamp);
}

namespace
{

// A one-submesh model whose triangle spans y in [-halfHeight, +halfHeight], so
// two heights hash differently and ReloadModelMeshes re-uploads in place.
std::unique_ptr<ModelAsset> MakeContinuityModel(const GUID& guid, float halfHeight)
{
    Mesh m{};
    m.Name = "ContinuityTriangle";
    Vertex v0{}, v1{}, v2{};
    v0.Position[1] = halfHeight;
    v1.Position[0] = -1.0f;
    v1.Position[1] = -halfHeight;
    v2.Position[0] = 1.0f;
    v2.Position[1] = -halfHeight;
    v0.Normal[2] = v1.Normal[2] = v2.Normal[2] = 1.0f;
    m.Vertices = {v0, v1, v2};
    m.Indices = {0, 1, 2};
    auto model = std::make_unique<ModelAsset>(guid, std::filesystem::path("Synthetic://Continuity"));
    Vector<Mesh> meshes;
    meshes.push_back(std::move(m));
    model->SetMeshesForTest(std::move(meshes));
    return model;
}

} // namespace

// The production hot-reload path: ReloadModelMeshes re-uploads the changed
// geometry into the SAME handle slot and the SAME GPUScene mesh row, so the
// batch key is identical before and after; only the upload sequence moves.
// The reload notification escalates extraction to the full lane, where the
// upload-sequence compare turns the reload into a rebuild that moves the
// instance's continuity stamp.
TEST_F(ExtractionFastPathTest, ContinuityStampMovesOnAStableHandleMeshReload)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));

    const GUID modelGuid = FixedGuid(0x77);
    auto model = MakeContinuityModel(modelGuid, 1.0f);
    const auto handles = h.rs.GetMeshGPURegistry().RegisterModelMeshes(modelGuid, *model);
    ASSERT_EQ(handles.size(), 1u);
    ECS::Entity e = h.world->Create();
    e.Set(MakeTransformAt(0.0f, 0.0f, 0.0f));
    MeshRenderer mr{};
    mr.meshGpuHandleId = static_cast<uint64>(handles[0]);
    mr.renderLayerMask = 0x1u;
    mr.materialAssetGuid.Set(h.materialGuid);
    e.Set(mr);
    h.world->ProcessCommands();
    ASSERT_TRUE(h.PrimeToFastPath());

    const std::vector<BatchKeySnapshot> keysBefore = SnapshotBatchKeys(h);
    ASSERT_EQ(keysBefore.size(), 1u);
    const uint32 slot = SlotOf(h, e);
    ASSERT_NE(slot, 0xFFFFFFFFu);
    const uint32 stampBefore = StampOf(h, slot);
    const auto* entryBefore = h.rs.GetMeshGPURegistry().Find(handles[0]);
    ASSERT_NE(entryBefore, nullptr);
    const uint64 uploadBefore = entryBefore->uploadSeq;
    const uint32 meshRowBefore = entryBefore->gpuMeshIndex;

    auto regrown = MakeContinuityModel(modelGuid, 2.5f);
    const auto report = h.rs.GetMeshGPURegistry().ReloadModelMeshes(modelGuid, *regrown);
    ASSERT_EQ(report.SubmeshesReuploaded, 1u) << "precondition: the reload re-uploaded";
    const auto* entryAfter = h.rs.GetMeshGPURegistry().Find(handles[0]);
    ASSERT_NE(entryAfter, nullptr) << "precondition: the handle is stable across the reload";
    ASSERT_NE(entryAfter->uploadSeq, uploadBefore) << "precondition: a new upload sequence";
    ASSERT_EQ(entryAfter->gpuMeshIndex, meshRowBefore) << "precondition: the mesh row is stable";

    h.StepFrame();
    EXPECT_EQ(h.Stats().FastFrame, 0u) << "the reload notification escalates to the full lane";
    EXPECT_EQ(SnapshotBatchKeys(h), keysBefore) << "a stable-handle reload leaves the key identical";
    EXPECT_NE(StampOf(h, slot), stampBefore) << "the reload must break continuity";

    // Recovery: the frames after the reload are continuous again.
    const uint32 stampAfterReload = StampOf(h, slot);
    ASSERT_TRUE(h.PrimeToFastPath());
    EXPECT_EQ(StampOf(h, slot), stampAfterReload);
}

TEST_F(ExtractionFastPathTest, ContinuityStampMovesOnMaterialRebind)
{
    ExtractionHarness h;
    ASSERT_TRUE(h.Initialize(m_Device.get(), true));
    const GUID materialB = FixedGuid(0x66);
    {
        MaterialDocument doc{};
        doc.materialName = "ContinuityMaterialB";
        doc.lightingModel = "StandardPBR";
        doc.surfaceShader = "surfaces/standard_surface.glsl";
        Material* matB = h.rs.Materials().Registry().Register(materialB, doc);
        ASSERT_NE(matB, nullptr);
        Material::TestFactory::SetGraphicsPipelineId(*matB, Rendering::GraphicsPipelineId{1u});
        Material::TestFactory::SetGpuSceneMaterialIndex(*matB, 1u);
    }
    ECS::Entity e = h.SpawnRenderable(0.0f, 0.0f, 0.0f);
    ASSERT_TRUE(h.PrimeToFastPath());
    const uint32 slot = SlotOf(h, e);
    const uint32 stampBefore = StampOf(h, slot);

    if (auto* mr = h.world->GetComponentForWrite<MeshRenderer>(e.GetHandle()))
        mr->materialAssetGuid.Set(materialB);
    h.StepFrame();
    EXPECT_NE(StampOf(h, slot), stampBefore) << "a material rebind changes what the slot renders";
}
