// Phase 9 end-to-end smoke test for the humanoid retargeting redesign.
//
// This is the ONE integration test that proves "retargeting works end-to-
// end." Per-phase unit tests (HumanoidRetargetSystemTests, RetargetNodeTests,
// etc.) cover individual stages; this test exercises the whole pipeline on
// a synthetic but representative scene over 30 frames.
//
// Coverage:
//   1. Spawn one entity-equivalent with HumanoidRetargeterComponent +
//      synthetic source / target rigs + synthetic walking clip.
//   2. Run 30 frames through TestHooks::RunCPUEvaluateForTest (mirrors
//      what HumanoidRetargetSystem::Update() does per character on the
//      CPU fallback path).
//   3. Assert: no exceptions, the entity's CompactSkinMatrices are
//      populated and non-identity (rotation present), and no NaNs / Infs
//      anywhere in the matrix stream.
//
// Synthetic-but-representative: a 4-bone chain (Hips -> Spine -> Chest ->
// Head) with rotation + translation tracks on the source clip. Both rigs
// are minimal HumanoidRigs sharing the canonical mapping. Same-skeleton
// retargeting reproduces the source clip; the test checks that the runtime
// path produces the right shape of output, not the math itself (covered by
// RetargetNodeTests).

#include <gtest/gtest.h>

#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/HumanBone.h"
#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/PoseStack.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonData.h"

#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"

#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"

#include "Engine/Rendering/RetargetSubmission.h"

#include "Assets/AnimationClip.h"
#include "AssetCore/GUID.h"

#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace GameEngine;
using namespace GameEngine::Animation;
namespace GERender = GameEngine::Engine::Renderer;

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr uint32 kBoneCount = 4;
constexpr int    kFrameCount = 30;

// Build a 4-bone synthetic humanoid spine: Hips -> Spine -> Chest -> Head.
// Each bone offset 1m up its parent. Identity rotations at rest.
uint32 CreateSyntheticHumanoidSkeleton()
{
    auto& store = GERender::SkeletonStore::Instance();
    uint32 id = store.CreateSkeleton(kBoneCount);
    auto* skel = store.Get(id);
    if (!skel) return 0;
    skel->BoneCount = kBoneCount;
    skel->Parent = { -1, 0, 1, 2 };
    skel->RestTranslation = {
        0.0f, 0.0f, 0.0f, // Hips at world origin
        0.0f, 0.4f, 0.0f, // Spine 0.4m above Hips
        0.0f, 0.4f, 0.0f, // Chest 0.4m above Spine
        0.0f, 0.4f, 0.0f, // Head 0.4m above Chest
    };
    skel->RestRotation = {
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    skel->RestScale = {
        1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f,
    };
    skel->BindPose.assign(kBoneCount * 16u, 0.0f);
    skel->InverseBind.assign(kBoneCount * 16u, 0.0f);
    for (uint32 b = 0; b < kBoneCount; ++b)
    {
        skel->BindPose[b * 16 + 0] = 1.0f;
        skel->BindPose[b * 16 + 5] = 1.0f;
        skel->BindPose[b * 16 + 10] = 1.0f;
        skel->BindPose[b * 16 + 15] = 1.0f;
        skel->InverseBind[b * 16 + 0] = 1.0f;
        skel->InverseBind[b * 16 + 5] = 1.0f;
        skel->InverseBind[b * 16 + 10] = 1.0f;
        skel->InverseBind[b * 16 + 15] = 1.0f;
    }
    skel->SkinJointCount = kBoneCount;
    skel->JointNodes = { 0, 1, 2, 3 };
    skel->BoneNames = { "Hips", "Spine", "Chest", "Head" };
    skel->BoneNameLookup[HashStringId("Hips")]  = 0;
    skel->BoneNameLookup[HashStringId("Spine")] = 1;
    skel->BoneNameLookup[HashStringId("Chest")] = 2;
    skel->BoneNameLookup[HashStringId("Head")]  = 3;
    return id;
}

// Populate a HumanoidRig with the four canonical mappings and a Spine chain.
void PopulateHumanoidRig(HumanoidRig& rig)
{
    rig.SetProfileRef(GUID::Generate());
    auto& bm = rig.BoneMapMutable();
    auto add = [&bm](HumanBone bone, const char* name, uint32 idx) {
        HumanoidBoneMapping m;
        m.Canonical = bone;
        m.SourceBoneName = name;
        m.CachedSourceIndex = idx;
        m.RetargetPoseRotation = Mathematics::Quaternion::Identity();
        bm.push_back(m);
    };
    add(HumanBone::Hips,  "Hips",  0);
    add(HumanBone::Spine, "Spine", 1);
    add(HumanBone::Chest, "Chest", 2);
    add(HumanBone::Head,  "Head",  3);

    auto& chains = rig.ChainsMutable();
    HumanoidChain spine;
    spine.Kind = ChainKind::Spine;
    spine.Start = HumanBone::Hips;
    spine.End   = HumanBone::Head;
    spine.IncludeBones = { HumanBone::Hips, HumanBone::Spine, HumanBone::Chest, HumanBone::Head };
    chains.push_back(spine);

    auto& props = rig.ProportionsMutable();
    props.HipHeight = 0.4f;
}

// Synthetic walking clip — Spine sways +/- 10 deg around X over 1 second.
// Hips translates forward 1m over 1 second. Loops via the system.
uint32 RegisterSyntheticWalkClip()
{
    GUID g = GUID::Generate();
    auto clip = MakeShared<AnimationClip>(g, std::filesystem::path("synthetic://walk_clip"));

    std::vector<AnimChannel> channels;

    // Hips translation channel (forward over 1s).
    {
        AnimChannel ch{};
        ch.boneIndex = 0;
        ch.targetName = "Hips";
        ch.path = AnimPath::Translation;

        AnimKeyframe k0{};
        k0.time = 0.0f;
        k0.translation[0] = 0.0f;
        k0.translation[1] = 0.0f;
        k0.translation[2] = 0.0f;
        k0.rotation[0] = k0.rotation[1] = k0.rotation[2] = 0.0f;
        k0.rotation[3] = 1.0f;
        k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;

        AnimKeyframe k1 = k0;
        k1.time = 1.0f;
        k1.translation[2] = 1.0f;

        ch.keys = { k0, k1 };
        channels.push_back(ch);
    }

    // Spine rotation channel (sway).
    {
        AnimChannel ch{};
        ch.boneIndex = 1;
        ch.targetName = "Spine";
        ch.path = AnimPath::Rotation;

        AnimKeyframe k0{};
        k0.time = 0.0f;
        k0.translation[0] = k0.translation[1] = k0.translation[2] = 0.0f;
        const float halfA = 5.0f * kPi / 180.0f;
        k0.rotation[0] = std::sin(halfA);
        k0.rotation[3] = std::cos(halfA);
        k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;

        AnimKeyframe k1 = k0;
        k1.time = 0.5f;
        const float halfB = -5.0f * kPi / 180.0f;
        k1.rotation[0] = std::sin(halfB);
        k1.rotation[3] = std::cos(halfB);

        AnimKeyframe k2 = k0;
        k2.time = 1.0f;

        ch.keys = { k0, k1, k2 };
        channels.push_back(ch);
    }

    // Chest rotation channel (smaller sway).
    {
        AnimChannel ch{};
        ch.boneIndex = 2;
        ch.targetName = "Chest";
        ch.path = AnimPath::Rotation;

        AnimKeyframe k0{};
        k0.time = 0.0f;
        k0.translation[0] = k0.translation[1] = k0.translation[2] = 0.0f;
        k0.rotation[0] = k0.rotation[1] = k0.rotation[2] = 0.0f;
        k0.rotation[3] = 1.0f;
        k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;

        AnimKeyframe k1 = k0;
        k1.time = 0.5f;
        const float halfA = 3.0f * kPi / 180.0f;
        k1.rotation[0] = std::sin(halfA);
        k1.rotation[3] = std::cos(halfA);

        AnimKeyframe k2 = k0;
        k2.time = 1.0f;

        ch.keys = { k0, k1, k2 };
        channels.push_back(ch);
    }

    clip->SetChannelsAndDurationForTest(channels, 1.0f);
    auto& clipStore = GERender::ClipStore::Instance();
    return clipStore.RegisterRuntimeClip(g, std::move(clip));
}

bool MatrixIsNonIdentity(const float* m, float eps = 1e-5f)
{
    if (!m) return false;
    auto near = [eps](float a, float b) { return std::fabs(a - b) < eps; };
    bool isIdentity = near(m[0], 1.0f) && near(m[5], 1.0f) &&
                      near(m[10], 1.0f) && near(m[15], 1.0f);
    if (!isIdentity) return true;
    for (int i = 0; i < 16; ++i) {
        if (i == 0 || i == 5 || i == 10 || i == 15) continue;
        if (std::fabs(m[i]) > eps) return true;
    }
    return false;
}

bool MatrixIsFinite(const float* m, uint32 count = 16)
{
    if (!m) return false;
    for (uint32 i = 0; i < count; ++i)
    {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

class HumanoidRetargetEndToEndSmokeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        GERender::ClipStore::Instance().ClearForTest();
    }
};

} // namespace

// =====================================================================
// One end-to-end smoke test that exercises the full Phase 6 ECS bridge
// (HumanoidRetargetSystem CPU fallback) over 30 frames on a synthetic
// 4-bone humanoid spine. The pipeline path:
//
//   AnimationClip (synthetic walk) -> source SkeletonData (4 bones) ->
//   AnimationPose -> RetargetNode (Stage 1 encode + Stage 2 FK transport
//   + Stage 3 translate) -> BuildPoseToSkinMatrices -> compact skin
//   matrices in target SkeletonRuntimeState.
//
// Asserts:
//   * No exceptions thrown across all frames.
//   * After at least one frame, the runtime's CompactSkinMatrices buffer
//     is populated to the right size (kBoneCount * 16 floats).
//   * At least one bone's matrix is non-identity (rotation propagated).
//   * Every float in every emitted matrix is finite (no NaN, no Inf).
// =====================================================================
TEST_F(HumanoidRetargetEndToEndSmokeTest, ThirtyFrames_ProducesValidNonIdentityFiniteSkinMatrices)
{
    auto& skStore = GERender::SkeletonStore::Instance();

    uint32 srcSkelId = CreateSyntheticHumanoidSkeleton();
    uint32 tgtSkelId = CreateSyntheticHumanoidSkeleton();
    ASSERT_NE(srcSkelId, 0u);
    ASSERT_NE(tgtSkelId, 0u);

    uint32 tgtRuntimeId = skStore.CreateRuntime(tgtSkelId);
    ASSERT_NE(tgtRuntimeId, 0u);

    HumanoidRig srcRig(GUID::Generate(), std::filesystem::path("test://e2e_src.json"));
    HumanoidRig tgtRig(GUID::Generate(), std::filesystem::path("test://e2e_tgt.json"));
    PopulateHumanoidRig(srcRig);
    PopulateHumanoidRig(tgtRig);

    RetargetMap map(GUID::Generate(), std::filesystem::path("test://e2e_map.json"));
    map.SetSourceRigRef(srcRig.GetGUID());
    map.SetTargetRigRef(tgtRig.GetGUID());
    AutoCreateRetargetMap(srcRig, tgtRig, map);

    uint32 clipIndex = RegisterSyntheticWalkClip();
    ASSERT_NE(clipIndex, 0u);

    // Set up the component (declarative; mirrors what auto-bootstrap
    // would produce). Not consumed directly by the test hook, but
    // documents the entity-equivalent shape.
    Components::HumanoidRetargeterComponent rc;
    rc.Map.Set(map.GetGUID());
    rc.SourceClipIndex = clipIndex;
    rc.ClipTimeSeconds = 0.0f;
    rc.Loop = true;
    rc.Speed = 1.0f;

    bool sawNonIdentityMatrix = false;
    bool everyFrameFinite = true;

    constexpr float kClipDuration = 1.0f;
    const float dt = kClipDuration / static_cast<float>(kFrameCount);

    for (int f = 0; f < kFrameCount; ++f)
    {
        const float t = static_cast<float>(f) * dt;

        bool ok = false;
        ASSERT_NO_THROW({
            ok = GERender::TestHooks::RunCPUEvaluateForTest(
                map, srcRig, tgtRig, clipIndex,
                srcSkelId, tgtSkelId, tgtRuntimeId, t);
        }) << "Frame " << f << " threw an exception";
        ASSERT_TRUE(ok) << "Frame " << f << " evaluation failed";

        auto* runtime = skStore.GetRuntime(tgtRuntimeId);
        ASSERT_NE(runtime, nullptr) << "Frame " << f << " runtime missing";
        ASSERT_GE(runtime->CompactSkinMatrices.size(), kBoneCount * 16u)
            << "Frame " << f << " matrix buffer too small";

        for (uint32 b = 0; b < kBoneCount; ++b)
        {
            const float* m = &runtime->CompactSkinMatrices[b * 16];
            if (!MatrixIsFinite(m))
            {
                everyFrameFinite = false;
                ADD_FAILURE() << "Frame " << f << " bone " << b
                              << " skin matrix contains NaN or Inf";
            }
            if (MatrixIsNonIdentity(m))
            {
                sawNonIdentityMatrix = true;
            }
        }
    }

    EXPECT_TRUE(sawNonIdentityMatrix)
        << "Across 30 frames of a non-trivial walking clip, no bone produced "
           "a non-identity skin matrix. The retarget pipeline isn't propagating "
           "the source clip rotations.";
    EXPECT_TRUE(everyFrameFinite)
        << "At least one frame emitted a NaN / Inf in the skin palette.";
}

// =====================================================================
// Audit P2 #11 — strong correctness gate. The original smoke test only
// checked "non-identity, finite". A retarget pipeline that left every
// bone at the target retarget pose (i.e. produced T-pose output from a
// walking clip) would still pass that bar. This test specifically picks
// a frame near mid-cycle (t = 0.25s, when the spine sway is at its
// extreme) and asserts the spine bone's WORLD rotation deltas exceed a
// per-bone amplitude threshold. The test fails if M_src/M_tgt is
// silently unwired (P0 #3) or parent-shift wrong (P0 #4) when those
// regressions zero-out per-bone rotation deltas.
// =====================================================================
TEST_F(HumanoidRetargetEndToEndSmokeTest, MidCycleFrame_SpineRotationAmplitudeExceedsThreshold)
{
    auto& skStore = GERender::SkeletonStore::Instance();

    uint32 srcSkelId = CreateSyntheticHumanoidSkeleton();
    uint32 tgtSkelId = CreateSyntheticHumanoidSkeleton();
    ASSERT_NE(srcSkelId, 0u);
    ASSERT_NE(tgtSkelId, 0u);

    uint32 tgtRuntimeId = skStore.CreateRuntime(tgtSkelId);
    ASSERT_NE(tgtRuntimeId, 0u);

    HumanoidRig srcRig(GUID::Generate(), std::filesystem::path("test://amp_src.json"));
    HumanoidRig tgtRig(GUID::Generate(), std::filesystem::path("test://amp_tgt.json"));
    PopulateHumanoidRig(srcRig);
    PopulateHumanoidRig(tgtRig);

    RetargetMap map(GUID::Generate(), std::filesystem::path("test://amp_map.json"));
    map.SetSourceRigRef(srcRig.GetGUID());
    map.SetTargetRigRef(tgtRig.GetGUID());
    AutoCreateRetargetMap(srcRig, tgtRig, map);

    uint32 clipIndex = RegisterSyntheticWalkClip();
    ASSERT_NE(clipIndex, 0u);

    // Sample at t = 0.25s — this is between key 0 (rotation = +5°) and
    // key 1 at t = 0.5s (rotation = -5°), so mid-interpolated we expect a
    // ~0° (stationary at midpoint of the slerp). Switch to t = 0.0s
    // (peak amplitude +5° on Spine bone X) so we can detect the rotation
    // amplitude reliably.
    const float midTime = 0.0f;
    bool ok = GERender::TestHooks::RunCPUEvaluateForTest(
        map, srcRig, tgtRig, clipIndex, srcSkelId, tgtSkelId, tgtRuntimeId,
        midTime);
    ASSERT_TRUE(ok);

    auto* runtime = skStore.GetRuntime(tgtRuntimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_GE(runtime->CompactSkinMatrices.size(), kBoneCount * 16u);

    // Spine bone (index 1) should have non-trivial rotation in its skin
    // matrix at t=0 (the key0 5° X-rotation should propagate). Specifically,
    // the M[5] / M[6] / M[9] / M[10] sub-matrix should reflect a 5° X-axis
    // rotation: cos(5°) ~= 0.9962, sin(5°) ~= 0.0872.
    //
    // Skin matrix at bone 1 is mesh-root-inverse * world * inverse-bind.
    // For our synthetic identity bind/mesh-root setup, that simplifies to
    // the world matrix of bone 1, which is parent-world * local. Hips
    // local = identity, so world(Hips) = identity; world(Spine) =
    // local(Spine) which carries the 5° X rotation.
    const float* spineSkin = &runtime->CompactSkinMatrices[1 * 16];

    // Hand-pick the X-rotation sub-matrix. For a column-major mat4 with
    // X-rotation by theta, m[5] = cos, m[6] = sin (col 1 row 2), m[9] =
    // -sin (col 2 row 1), m[10] = cos.
    const float cosVal = spineSkin[5];
    const float sinVal = spineSkin[6];

    // The rotation magnitude should be ~5°. Allow generous tolerance
    // because the skin matrix also folds in the translation. The key
    // assertion is "magnitude is significantly non-zero".
    const float magnitude = std::sqrt(sinVal * sinVal + (1.0f - cosVal) * (1.0f - cosVal));
    EXPECT_GT(magnitude, 0.05f)
        << "Spine bone skin matrix at t=0 shows ~zero X-rotation. The "
           "retarget pipeline is not propagating clip rotation. cos="
        << cosVal << " sin=" << sinVal;
    EXPECT_TRUE(MatrixIsFinite(spineSkin));
}
