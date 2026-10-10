// Stamp-audit lock (extraction-fusion T15, correctness A1c): an idle animated
// scene — one playing skinned entity plus one morph-carrying entity, with no
// actual changes — must not write-grant ("stamp") the four extraction probe
// columns (MeshRenderer, LocalBounds, WorldSectorCoord, LODGroup) frame over
// frame. The ECS stamps write columns at visit, BEFORE the callback, for
// every non-filtered chunk, so any per-frame system that binds a probed
// column mutably re-arms this failure regardless of what its body does.
// This is the tripwire that keeps a Changed<>-gated extraction fast path
// viable in real content against the next mutable-binding regression.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "ECSModules/Rendering/Systems/MorphTargetSystem.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Assets/AnimationClip.h"
#include "AssetCore/GUID.h"

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ChangeFilter.h"
#include "ECS/ECSTemplates.h"

#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/WorldSectorCoord.h"

#include <filesystem>

using namespace GameEngine;
using GameEngine::Engine::Renderer::RenderServices;

namespace
{

// Entities visited by a Changed<C>-gated scan since `gate`. Visiting form on
// purpose: Query::Count() ignores the Changed<> filter, and the const pointer
// keeps the probe itself read-only (the scan never stamps).
template <typename C>
size_t VisitedSince(ECS::World& world, uint64 gate)
{
    ECS::ChangeGate g;
    g.LastRunVersion = gate;
    size_t visited = 0;
    auto q = world.Query<ECS::Read<C>>();
    q.template Changed<C>(g);
    q.BatchEach([&](const C*, std::size_t count) { visited += count; });
    return visited;
}

void SetIdentity(float* m)
{
    for (int i = 0; i < 16; ++i)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

uint32 MakeSingleBoneSkeleton()
{
    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skeletonId = skStore.CreateSkeleton(1);
    auto* skel = skStore.Get(skeletonId);
    if (!skel)
        return 0;
    skel->Parent = {-1};
    skel->InverseBind.resize(16);
    SetIdentity(skel->InverseBind.data());
    skel->BindPose.resize(16);
    SetIdentity(skel->BindPose.data());
    skel->RestTranslation = {0.0f, 0.0f, 0.0f};
    skel->RestRotation = {0.0f, 0.0f, 0.0f, 1.0f};
    skel->RestScale = {1.0f, 1.0f, 1.0f};
    skel->SkinJointCount = 1;
    skel->JointNodes = {0};
    return skeletonId;
}

// Legacy channel (targetNameId = 0): trusted stored boneIndex, so the clip
// samples on either the GPU-safe or CPU path without name resolution and the
// cross-rig bootstrap sniff stays quiet.
uint32 RegisterLoopingClip()
{
    AnimKeyframe k0{};
    k0.time = 0.0f;
    k0.rotation[3] = 1.0f;
    k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
    AnimKeyframe k1 = k0;
    k1.time = 1.0f;
    k1.translation[0] = 1.0f;

    AnimChannel ch{};
    ch.boneIndex = 0;
    ch.path = AnimPath::Translation;
    ch.keys = {k0, k1};

    const GUID clipGuid = GUID::Generate();
    auto clip = MakeShared<AnimationClip>(clipGuid, std::filesystem::path("Synthetic://StampAudit"));
    clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);
    if (!Engine::Renderer::TestHooks::SetClipCacheForTest(clipGuid, clip))
        return 0;
    return Engine::Renderer::ClipStore::Instance().GetIndexIfPresent(clipGuid);
}

} // namespace

TEST(ExtractionStampAuditTests, IdleAnimatedSceneDoesNotStampProbedColumns)
{
    Engine::Renderer::TestHooks::ClearClipCacheForTest();

    // Un-initialized RenderServices is sufficient and keeps the audit
    // device-free: compute skinning reports not-ready (CPU sample path) and
    // MorphTargetSystem only takes the registry reference before its
    // null-model early return.
    RenderServices rs;
    Engine::Renderer::AnimationSystem animationSystem(&rs);
    Engine::Renderer::MorphTargetSystem morphSystem(&rs);

    ECS::World world(nullptr);

    auto& skStore = Engine::Renderer::SkeletonStore::Instance();
    const uint32 skeletonId = MakeSingleBoneSkeleton();
    ASSERT_NE(skeletonId, 0u);
    const uint32 runtimeId = skStore.CreateRuntime(skeletonId);
    ASSERT_NE(runtimeId, 0u);
    const uint32 clipIndex = RegisterLoopingClip();
    ASSERT_NE(clipIndex, 0u);

    // Playing skinned entity: the AnimationSystem playback query visits its
    // chunk every frame.
    Components::AnimatorRef animRef{};
    animRef.ClipIndex = clipIndex;
    animRef.Flags = Components::AnimatorRef::kFlag_Loop;

    Components::SkeletonRef skelRef{};
    skelRef.skeletonId = skeletonId;
    skelRef.runtimeId = runtimeId;

    ECS::Entity skinned = world.Create();
    skinned.Set(animRef);
    skinned.Set(skelRef);
    skinned.Set(Components::MeshRenderer{});
    skinned.Set(Components::LocalBounds{});
    skinned.Set(Components::LODGroup{});
    skinned.Set(Components::WorldSectorCoord{});

    // Morph-carrying entity: MorphTargetSystem visits its chunk every frame
    // (null model GUID -> early return, but stamp-at-visit fires before the
    // callback body, which is exactly the shape under audit).
    ECS::Entity morph = world.Create();
    morph.Set(Components::MeshRenderer{});
    morph.Set(Components::MorphTargetWeights{});
    morph.Set(Components::LocalBounds{});
    morph.Set(Components::LODGroup{});
    morph.Set(Components::WorldSectorCoord{});

    world.ProcessCommands();

    constexpr float32 kDt = 1.0f / 60.0f;
    constexpr int kIdleFrames = 8;

    // Settle frame: first-visit work (bootstrap memoization, runtime prime)
    // happens outside the audited window.
    animationSystem.Update(world, kDt);
    morphSystem.Update(world, kDt);

    const uint64 gate = world.GetGlobalSystemVersion();
    for (int frame = 0; frame < kIdleFrames; ++frame)
    {
        animationSystem.Update(world, kDt);
        morphSystem.Update(world, kDt);
    }

    // Liveness: the systems really visited their chunks. AnimatorRef and
    // MorphTargetWeights are genuine per-frame write columns, so their
    // stamps prove the dispatches ran (and playback advanced).
    EXPECT_GT(VisitedSince<Components::AnimatorRef>(world, gate), 0u)
        << "AnimationSystem did not visit the skinned entity - the audit is vacuous";
    EXPECT_GT(VisitedSince<Components::MorphTargetWeights>(world, gate), 0u)
        << "MorphTargetSystem did not visit the morph entity - the audit is vacuous";
    const auto* animAfter = world.GetComponent<Components::AnimatorRef>(skinned.GetHandle());
    ASSERT_NE(animAfter, nullptr);
    EXPECT_GT(animAfter->Time, 0.0f) << "clip is not actually playing";

    // The lock: all four probe columns stayed quiet across the idle frames.
    EXPECT_EQ(VisitedSince<Components::MeshRenderer>(world, gate), 0u)
        << "an idle frame write-granted MeshRenderer - a Changed<MeshRenderer> "
           "probe would fire every frame in any animated scene";
    EXPECT_EQ(VisitedSince<Components::LocalBounds>(world, gate), 0u)
        << "an idle frame write-granted LocalBounds";
    EXPECT_EQ(VisitedSince<Components::WorldSectorCoord>(world, gate), 0u)
        << "an idle frame write-granted WorldSectorCoord";
    EXPECT_EQ(VisitedSince<Components::LODGroup>(world, gate), 0u)
        << "an idle frame write-granted LODGroup";

    // Positive controls: a real edit fires each probe, so quiet-above cannot
    // be a probe that matches nothing.
    const uint64 gateMr = world.GetGlobalSystemVersion();
    if (auto* mr = world.GetComponentForWrite<Components::MeshRenderer>(skinned.GetHandle()))
        mr->castShadows = !mr->castShadows;
    EXPECT_GT(VisitedSince<Components::MeshRenderer>(world, gateMr), 0u);

    const uint64 gateLb = world.GetGlobalSystemVersion();
    if (auto* lb = world.GetComponentForWrite<Components::LocalBounds>(morph.GetHandle()))
        lb->Box.halfExtents.x += 1.0f;
    EXPECT_GT(VisitedSince<Components::LocalBounds>(world, gateLb), 0u);

    Engine::Renderer::TestHooks::ClearClipCacheForTest();
}
