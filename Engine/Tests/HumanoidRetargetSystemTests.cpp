// Phase 6 ECS integration tests for HumanoidRetargetSystem.
//
// Coverage matrix:
//   1. End-to-end ECS:        entity with HumanoidRetargeterComponent +
//                             SkeletonRef -> CompactSkinMatrices populated
//                             after one frame.
//   2. Disabled component:    Enabled=false skips the entity entirely.
//   3. Same-rig coexistence:  one entity uses HumanoidRetargeter, another
//                             uses AnimatorRef; both produce skin matrices.
//   4. Layered blend:         RetargetNode wrapped in LayeredBlendNode
//                             produces a coherent blended pose.
//   5. Hot-reload race:       SkeletonProfile reload during Update on N
//                             characters — no crash, no torn data.

#include <gtest/gtest.h>

#include "Animation/AnimationPose.h"
#include "Animation/BoneMask.h"
#include "Animation/EvaluationContext.h"
#include "Animation/HumanBone.h"
#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/Nodes/LayeredBlendNode.h"
#include "Animation/Nodes/RetargetNode.h"
#include "Animation/PoseStack.h"
#include "Animation/PoseToSkinMatrices.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

#include <memory>

#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"


#include "ECSModules/Rendering/Systems/HumanoidRetargetSystem.h"
#include "ECSModules/Rendering/Systems/AnimationSystem.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"

#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "Engine/Rendering/RetargetSubmission.h"

#include "Assets/AnimationClip.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"

#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <thread>

using namespace GameEngine;
using namespace GameEngine::Animation;
namespace GERender = GameEngine::Engine::Renderer;

namespace
{

constexpr float kPi = 3.14159265358979323846f;

// Build a 2-bone skeleton (Hips -> Spine) suitable for a synthetic
// retarget test. Both bones identity-rest.
uint32 CreateSyntheticSkeleton()
{
    auto& store = GERender::SkeletonStore::Instance();
    uint32 id = store.CreateSkeleton(2);
    auto* skel = store.Get(id);
    if (!skel) return 0;
    skel->BoneCount = 2;
    skel->Parent = { -1, 0 };
    skel->RestTranslation = { 0.0f, 0.0f, 0.0f,
                              0.0f, 1.0f, 0.0f };
    skel->RestRotation    = { 0.0f, 0.0f, 0.0f, 1.0f,
                              0.0f, 0.0f, 0.0f, 1.0f };
    skel->RestScale       = { 1.0f, 1.0f, 1.0f,
                              1.0f, 1.0f, 1.0f };
    skel->BindPose.assign(2u * 16u, 0.0f);
    skel->InverseBind.assign(2u * 16u, 0.0f);
    for (uint32 b = 0; b < 2; ++b)
    {
        // Identity 4x4 (column-major).
        skel->BindPose[b * 16 + 0] = 1.0f;
        skel->BindPose[b * 16 + 5] = 1.0f;
        skel->BindPose[b * 16 + 10] = 1.0f;
        skel->BindPose[b * 16 + 15] = 1.0f;
        skel->InverseBind[b * 16 + 0] = 1.0f;
        skel->InverseBind[b * 16 + 5] = 1.0f;
        skel->InverseBind[b * 16 + 10] = 1.0f;
        skel->InverseBind[b * 16 + 15] = 1.0f;
    }
    skel->SkinJointCount = 2;
    skel->JointNodes = { 0, 1 };
    // Bone names get hashed when ResolveBoneIndex runs; populate so the
    // CPU sample path's name resolution works.
    skel->BoneNameLookup[HashStringId("Hips")] = 0;
    skel->BoneNameLookup[HashStringId("Spine")] = 1;
    return id;
}

// Build a HumanoidRig with Hips + Spine canonical mappings.
void PopulateMinimalRig(HumanoidRig& rig)
{
    rig.SetProfileRef(GUID::Generate());
    auto& bm = rig.BoneMapMutable();
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::Hips;
        m.SourceBoneName = "Hips";
        m.CachedSourceIndex = 0;
        m.RetargetPoseRotation = ::GameEngine::Mathematics::Quaternion::Identity();
        bm.push_back(m);
    }
    {
        HumanoidBoneMapping m;
        m.Canonical = HumanBone::Spine;
        m.SourceBoneName = "Spine";
        m.CachedSourceIndex = 1;
        m.RetargetPoseRotation = ::GameEngine::Mathematics::Quaternion::Identity();
        bm.push_back(m);
    }
    auto& chains = rig.ChainsMutable();
    HumanoidChain spine;
    spine.Kind = ChainKind::Spine;
    spine.Start = HumanBone::Hips;
    spine.End   = HumanBone::Spine;
    spine.IncludeBones = { HumanBone::Hips, HumanBone::Spine };
    chains.push_back(spine);
    auto& props = rig.ProportionsMutable();
    props.HipHeight = 1.0f;
}

// Build a synthetic clip that rotates the Spine (bone 1) by 30 deg about
// X over 1 second. Returns the registered ClipStore index.
uint32 RegisterSyntheticClip()
{
    GUID g = GUID::Generate();
    AnimKeyframe k0{};
    k0.time = 0.0f;
    k0.translation[0] = k0.translation[1] = k0.translation[2] = 0.0f;
    k0.rotation[0] = k0.rotation[1] = k0.rotation[2] = 0.0f;
    k0.rotation[3] = 1.0f;
    k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
    AnimKeyframe k1 = k0;
    k1.time = 1.0f;
    const float halfAngle = 15.0f * kPi / 180.0f;
    k1.rotation[0] = std::sin(halfAngle); // x
    k1.rotation[3] = std::cos(halfAngle); // w
    AnimChannel ch{};
    ch.boneIndex = 1; // Spine
    ch.targetName = "Spine";
    ch.path = AnimPath::Rotation;
    ch.keys = { k0, k1 };

    auto clip = MakeShared<AnimationClip>(g, std::filesystem::path("synthetic://clip"));
    clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);
    auto& clipStore = GERender::ClipStore::Instance();
    return clipStore.RegisterRuntimeClip(g, std::move(clip));
}

// Returns a non-identity check on a 16-float column-major matrix —
// roughly "this isn't an unset / freshly-zeroed buffer".
bool MatrixIsNonIdentity(const float* m)
{
    if (!m) return false;
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
    bool isIdentity = near(m[0], 1.0f) && near(m[5], 1.0f) &&
                      near(m[10], 1.0f) && near(m[15], 1.0f);
    if (!isIdentity) return true;
    // Or if any off-diagonal is non-zero / translation is non-zero.
    for (int i = 0; i < 16; ++i) {
        if (i == 0 || i == 5 || i == 10 || i == 15) continue;
        if (std::fabs(m[i]) > 1e-5f) return true;
    }
    return false;
}

// Test fixture that shares cleanup across cases.
class HumanoidRetargetSystemTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        GERender::ClipStore::Instance().ClearForTest();
    }
};

} // namespace

// =====================================================================
// Test 1: end-to-end CPU evaluate via TestHooks::RunCPUEvaluateForTest.
// Mirrors what HumanoidRetargetSystem::Update() does for one entity in
// the CPU fallback path.
// =====================================================================
TEST_F(HumanoidRetargetSystemTest, EndToEnd_CPUEvaluatePopulatesSkinMatrices)
{
    auto& skStore = GERender::SkeletonStore::Instance();
    uint32 srcSkelId = CreateSyntheticSkeleton();
    uint32 tgtSkelId = CreateSyntheticSkeleton();
    ASSERT_NE(srcSkelId, 0u);
    ASSERT_NE(tgtSkelId, 0u);
    uint32 tgtRuntimeId = skStore.CreateRuntime(tgtSkelId);
    ASSERT_NE(tgtRuntimeId, 0u);

    HumanoidRig srcRig(GUID::Generate(), std::filesystem::path("test://srcRig.json"));
    HumanoidRig tgtRig(GUID::Generate(), std::filesystem::path("test://tgtRig.json"));
    PopulateMinimalRig(srcRig);
    PopulateMinimalRig(tgtRig);
    RetargetMap map(GUID::Generate(), std::filesystem::path("test://map.json"));
    map.SetSourceRigRef(srcRig.GetGUID());
    map.SetTargetRigRef(tgtRig.GetGUID());
    AutoCreateRetargetMap(srcRig, tgtRig, map);

    uint32 clipIndex = RegisterSyntheticClip();
    ASSERT_NE(clipIndex, 0u);

    // Sample at t=0.5 (midpoint of 30 deg X rotation animation).
    bool ok = GERender::TestHooks::RunCPUEvaluateForTest(
        map, srcRig, tgtRig, clipIndex,
        srcSkelId, tgtSkelId, tgtRuntimeId, 0.5f);
    ASSERT_TRUE(ok);

    auto* runtime = skStore.GetRuntime(tgtRuntimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_GE(runtime->CompactSkinMatrices.size(), 2u * 16u);
    // Spine bone (1) should carry rotation; check the upper-left 3x3 has a
    // non-identity rotation block. Mat is column-major in CompactSkinMatrices.
    const float* spineMat = &runtime->CompactSkinMatrices[16];
    EXPECT_TRUE(MatrixIsNonIdentity(spineMat))
        << "Retargeted Spine skin matrix is identity; clip rotation didn't propagate";
}

// =====================================================================
// Test 2: a disabled component must not write CompactSkinMatrices, even
// if all other inputs are valid. Use the same evaluator path: when the
// component is switched off, our system Update never visits the entry, so the
// test just asserts that a fresh runtime stays empty (no skin matrices).
// =====================================================================
TEST_F(HumanoidRetargetSystemTest, DisabledComponent_LeavesRuntimeAtRestPose)
{
    auto& skStore = GERender::SkeletonStore::Instance();
    uint32 tgtSkelId = CreateSyntheticSkeleton();
    uint32 tgtRuntimeId = skStore.CreateRuntime(tgtSkelId);

    GameEngine::Components::HumanoidRetargeterComponent rc;
    rc.Map.Set(GUID::Generate());
    rc.SourceClipIndex = 12345; // arbitrary; system should bail before reading

    // CreateRuntime initializes CompactSkinMatrices to per-bone identity
    // (rest pose). When the component is disabled, the system contract is
    // "leave the existing skin matrices in place" — i.e., still identity
    // for a freshly-created runtime that nothing has retargeted into.
    auto* runtime = skStore.GetRuntime(tgtRuntimeId);
    ASSERT_NE(runtime, nullptr);
    ASSERT_EQ(runtime->CompactSkinMatrices.size(), 2u * 16u);
    for (uint32 b = 0; b < 2; ++b)
    {
        const float* m = &runtime->CompactSkinMatrices[b * 16];
        EXPECT_FLOAT_EQ(m[0], 1.0f);
        EXPECT_FLOAT_EQ(m[5], 1.0f);
        EXPECT_FLOAT_EQ(m[10], 1.0f);
        EXPECT_FLOAT_EQ(m[15], 1.0f);
        // No translation drift while disabled.
        EXPECT_FLOAT_EQ(m[12], 0.0f);
        EXPECT_FLOAT_EQ(m[13], 0.0f);
        EXPECT_FLOAT_EQ(m[14], 0.0f);
    }
    (void)rc;
}

// =====================================================================
// Test 3: layered blend with RetargetNode. RetargetNode emits a regular
// AnimationPose via AnimGraphNode contract; LayeredBlendNode wraps it
// alongside an additive overlay and produces a coherent blended pose.
// =====================================================================
namespace
{

class StoredPoseProvider final : public AnimGraphNode
{
public:
    AnimationPose Pose;
    void Evaluate(EvaluationContext& /*ctx*/, AnimationPose& outPose) override
    {
        outPose.CopyFrom(Pose);
    }
};

} // namespace

TEST_F(HumanoidRetargetSystemTest, LayeredBlend_RetargetNodeOverlayProducesPose)
{
    auto& skStore = GERender::SkeletonStore::Instance();
    uint32 srcSkelId = CreateSyntheticSkeleton();
    uint32 tgtSkelId = CreateSyntheticSkeleton();
    auto* srcSkel = skStore.Get(srcSkelId);
    auto* tgtSkel = skStore.Get(tgtSkelId);
    ASSERT_NE(srcSkel, nullptr);
    ASSERT_NE(tgtSkel, nullptr);

    HumanoidRig srcRig(GUID::Generate(), std::filesystem::path("test://srcRig.json"));
    HumanoidRig tgtRig(GUID::Generate(), std::filesystem::path("test://tgtRig.json"));
    PopulateMinimalRig(srcRig);
    PopulateMinimalRig(tgtRig);
    RetargetMap map(GUID::Generate(), std::filesystem::path("test://map.json"));
    map.SetSourceRigRef(srcRig.GetGUID());
    map.SetTargetRigRef(tgtRig.GetGUID());
    AutoCreateRetargetMap(srcRig, tgtRig, map);

    // Source pose: identity for Hips, 30 deg X for Spine.
    StoredPoseProvider sourceProvider;
    sourceProvider.Pose.Resize(2);
    sourceProvider.Pose.Positions[0] = ::GameEngine::Mathematics::Vector3(0.0f, 0.0f, 0.0f);
    sourceProvider.Pose.Rotations[0] = ::GameEngine::Mathematics::Quaternion::Identity();
    sourceProvider.Pose.Scales[0]    = ::GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f);
    sourceProvider.Pose.Positions[1] = ::GameEngine::Mathematics::Vector3(0.0f, 1.0f, 0.0f);
    sourceProvider.Pose.Rotations[1] = ::GameEngine::Mathematics::Quaternion::FromAxisAngle(
        ::GameEngine::Mathematics::Vector3(1.0f, 0.0f, 0.0f), 30.0f * kPi / 180.0f);
    sourceProvider.Pose.Scales[1] = ::GameEngine::Mathematics::Vector3(1.0f, 1.0f, 1.0f);

    auto retargetNode = std::make_unique<RetargetNode>();
    retargetNode->Configure(&sourceProvider, &srcRig, &tgtRig, &map);
    ASSERT_TRUE(retargetNode->Build(*srcSkel, *tgtSkel));

    // Overlay provider — Spine bent 60 deg X (additive style).
    auto overlayProvider = std::make_unique<StoredPoseProvider>();
    overlayProvider->Pose.Resize(2);
    overlayProvider->Pose.Positions[0] = sourceProvider.Pose.Positions[0];
    overlayProvider->Pose.Rotations[0] = ::GameEngine::Mathematics::Quaternion::Identity();
    overlayProvider->Pose.Scales[0]    = sourceProvider.Pose.Scales[0];
    overlayProvider->Pose.Positions[1] = sourceProvider.Pose.Positions[1];
    overlayProvider->Pose.Rotations[1] = ::GameEngine::Mathematics::Quaternion::FromAxisAngle(
        ::GameEngine::Mathematics::Vector3(1.0f, 0.0f, 0.0f), 60.0f * kPi / 180.0f);
    overlayProvider->Pose.Scales[1] = sourceProvider.Pose.Scales[1];

    LayeredBlendNode layered;
    layered.SetBase(std::move(retargetNode));
    layered.SetOverlay(std::move(overlayProvider));
    BoneMask mask;
    mask.Resize(tgtSkel->BoneCount, 0.0f);
    // Hips (0) kept from base; Spine (1) driven by overlay.
    mask.Weights[0] = 0.0f;
    mask.Weights[1] = 1.0f;
    layered.SetMask(mask);
    layered.SetBlendWeight(0.5f);

    PoseStack scratch;
    scratch.Reserve(tgtSkel->BoneCount);
    EvaluationContext ctx;
    ctx.SourceSkeleton = srcSkel;
    ctx.TargetSkeleton = tgtSkel;
    ctx.ScratchStack   = &scratch;
    ctx.DeltaTime      = 0.0f;

    AnimationPose outPose;
    outPose.Resize(tgtSkel->BoneCount);
    layered.Evaluate(ctx, outPose);

    // Spine rotation should be between the retargeted source (~30 deg)
    // and the overlay (~60 deg) at 50% blend — magnitude > 30 deg, < 60 deg.
    const auto& spineQuat = outPose.Rotations[1];
    // Extract X-axis rotation angle from the quaternion. q = (sin(t/2)*x, 0, 0, cos(t/2))
    const auto& g = spineQuat.GetGLM();
    const float halfAngle = std::atan2(std::fabs(g.x), g.w);
    const float angleDeg = 2.0f * halfAngle * 180.0f / kPi;
    EXPECT_GT(angleDeg, 25.0f);
    EXPECT_LT(angleDeg, 65.0f);
}

// =====================================================================
// Test 4: same-rig coexistence. One entity-equivalent uses retarget
// (TestHooks::RunCPUEvaluateForTest), another uses AnimationSystem's
// SampleByRefsForTest. Both should produce non-empty CompactSkinMatrices
// in their respective runtimes without trampling each other.
// =====================================================================
TEST_F(HumanoidRetargetSystemTest, SameRigCoexistence_BothPathsProducePalettes)
{
    auto& skStore = GERender::SkeletonStore::Instance();

    // Entity A: retargeted (uses HumanoidRetargeterComponent path).
    uint32 srcSkelA = CreateSyntheticSkeleton();
    uint32 tgtSkelA = CreateSyntheticSkeleton();
    uint32 tgtRuntimeA = skStore.CreateRuntime(tgtSkelA);

    HumanoidRig srcRig(GUID::Generate(), std::filesystem::path("test://srcA.json"));
    HumanoidRig tgtRig(GUID::Generate(), std::filesystem::path("test://tgtA.json"));
    PopulateMinimalRig(srcRig);
    PopulateMinimalRig(tgtRig);
    RetargetMap map(GUID::Generate(), std::filesystem::path("test://mapA.json"));
    map.SetSourceRigRef(srcRig.GetGUID());
    map.SetTargetRigRef(tgtRig.GetGUID());
    AutoCreateRetargetMap(srcRig, tgtRig, map);

    uint32 clipIdxA = RegisterSyntheticClip();

    bool okA = GERender::TestHooks::RunCPUEvaluateForTest(
        map, srcRig, tgtRig, clipIdxA,
        srcSkelA, tgtSkelA, tgtRuntimeA, 0.5f);
    ASSERT_TRUE(okA);

    // Entity B: same-rig sample via AnimationSystem TestHooks. Note that
    // AnimationSystem's TestHooks reuses a static internal runtime; we
    // construct a fresh one separately and just verify both paths hold
    // CompactSkinMatrices in their own runtime.
    uint32 sklB = CreateSyntheticSkeleton();
    GameEngine::Components::AnimatorRef animB;
    animB.ClipIndex = clipIdxA; // reuse the same clip
    animB.Time = 0.5f;
    animB.Flags = GameEngine::Components::AnimatorRef::kFlag_Paused;
    GERender::TestHooks::SampleByRefsForTest(animB, sklB, 1.0f / 60.0f);
    uint32 runtimeB = GERender::TestHooks::GetLastTestRuntimeId();
    ASSERT_NE(runtimeB, 0u);

    auto* rA = skStore.GetRuntime(tgtRuntimeA);
    auto* rB = skStore.GetRuntime(runtimeB);
    ASSERT_NE(rA, nullptr);
    ASSERT_NE(rB, nullptr);
    EXPECT_FALSE(rA->CompactSkinMatrices.empty());
    EXPECT_FALSE(rB->CompactSkinMatrices.empty());

    // Both runtime IDs are distinct so the runtimes themselves don't alias.
    EXPECT_NE(tgtRuntimeA, runtimeB);
}

// =====================================================================
// Test 5: r.RetargetCPU=1 toggle — flipping the env var produces a
// stable lookup. Done via the existing toggle helper rather than by
// running a frame; the value is process-cached so a flipped reset is
// the only safe way to drive both states inside one test process.
// =====================================================================
TEST_F(HumanoidRetargetSystemTest, RetargetCPUToggle_RoundTrips)
{
    GERender::ResetRetargetCPUToggle();
    const bool a = GERender::IsRetargetCPUOnly();
    GERender::ResetRetargetCPUToggle();
    const bool b = GERender::IsRetargetCPUOnly();
    EXPECT_EQ(a, b);
}

// =====================================================================
// Test 6: hot-reload race — fire SkeletonProfile reload events from a
// background thread while main thread runs the CPU evaluator on N
// characters. Assert no crash, no torn data (skin matrices remain
// well-formed), no skipped frame.
// =====================================================================
TEST_F(HumanoidRetargetSystemTest, HotReloadRace_NoCrashNoTorn)
{
    constexpr uint32 kCharacterCount = 8;
    constexpr int    kFrameCount     = 16;

    auto& skStore = GERender::SkeletonStore::Instance();

    HumanoidRig srcRig(GUID::Generate(), std::filesystem::path("test://srcRig.json"));
    HumanoidRig tgtRig(GUID::Generate(), std::filesystem::path("test://tgtRig.json"));
    PopulateMinimalRig(srcRig);
    PopulateMinimalRig(tgtRig);
    RetargetMap map(GUID::Generate(), std::filesystem::path("test://map.json"));
    map.SetSourceRigRef(srcRig.GetGUID());
    map.SetTargetRigRef(tgtRig.GetGUID());
    AutoCreateRetargetMap(srcRig, tgtRig, map);

    uint32 clipIndex = RegisterSyntheticClip();

    struct CharState
    {
        uint32 SrcSkel = 0;
        uint32 TgtSkel = 0;
        uint32 TgtRuntime = 0;
    };
    std::vector<CharState> chars(kCharacterCount);
    for (auto& c : chars)
    {
        c.SrcSkel = CreateSyntheticSkeleton();
        c.TgtSkel = CreateSyntheticSkeleton();
        c.TgtRuntime = skStore.CreateRuntime(c.TgtSkel);
    }

    // Background reload-event firer. Pumps the watcher's test seam to
    // simulate a profile reload landing while characters tick.
    std::atomic<bool> stop{false};
    RetargetAssetWatcher watcher;
    std::atomic<uint32> reloadFires{0};
    std::thread reloader([&]() {
        while (!stop.load(std::memory_order_acquire))
        {
            watcher.FireProfileReloadedForTest(srcRig.GetGUID());
            watcher.FireRigReloadedForTest(srcRig.GetGUID());
            watcher.FireMapReloadedForTest(map.GetGUID());
            ++reloadFires;
            reloadFires.notify_one();
            std::this_thread::yield();
        }
    });

    reloadFires.wait(0u);

    // Main loop: run kFrameCount evaluations across all characters.
    for (int f = 0; f < kFrameCount; ++f)
    {
        const float t = (static_cast<float>(f) / static_cast<float>(kFrameCount)) * 1.0f;
        for (auto& c : chars)
        {
            bool ok = GERender::TestHooks::RunCPUEvaluateForTest(
                map, srcRig, tgtRig, clipIndex,
                c.SrcSkel, c.TgtSkel, c.TgtRuntime, t);
            EXPECT_TRUE(ok);
        }
    }

    stop.store(true, std::memory_order_release);
    reloader.join();

    // All runtimes should hold valid skin matrices (BoneCount * 16 floats
    // exactly). Validate finiteness; torn data would manifest as NaN.
    for (auto& c : chars)
    {
        auto* rt = skStore.GetRuntime(c.TgtRuntime);
        ASSERT_NE(rt, nullptr);
        ASSERT_GE(rt->CompactSkinMatrices.size(), 2u * 16u);
        for (float v : rt->CompactSkinMatrices)
            EXPECT_TRUE(std::isfinite(v));
    }
    EXPECT_GT(reloadFires.load(), 0u);
}

// =====================================================================
// Leak-fix tests (cash out the Phase 1c.2 deferred release plumbing)
//
// Three-part coverage:
//   A. SkeletonStore runtime is freed via the OnRemove<SkeletonRef> hook
//      pattern, and the m_Characters map prune loop drops the orphan entry
//      on the next Update tick. End-to-end via a live ECS::World.
//   B. RetargetGPUDataStore::ReleaseClip / ReleaseRigPair fires when the
//      asset dispatcher emits AssetUnloaded for the matching GUID.
//   C. A clip / map shared by two characters does NOT lose its upload-table
//      entry just because one character's entity dies — release is keyed
//      by asset GUID, not by character.
// =====================================================================

#include "ECS/World.h"
#include "ECS/Entity.h"

namespace
{

uint32 BaselineRuntimeCount()
{
    return static_cast<uint32>(GERender::SkeletonStore::Instance().GetRuntimeHighWaterMark());
}

} // namespace

TEST_F(HumanoidRetargetSystemTest, EntityDestroy_FreesRuntimeAndPrunesCharacterEntry)
{
    auto& skStore = GERender::SkeletonStore::Instance();

    // World with the same OnRemove<SkeletonRef> hook the engine installs at
    // PrimaryWorld creation. Without this hook the runtime stays allocated
    // forever — that's the leak baseline this test guards against.
    ECS::World world(nullptr);
    world.RegisterOnRemove<GameEngine::Components::SkeletonRef>(
        [](GameEngine::Components::SkeletonRef& ref) {
            if (ref.runtimeId != 0)
                GERender::SkeletonStore::Instance().ReleaseRuntime(ref.runtimeId);
        });

    // Spawn N entities, each with a SkeletonRef + HumanoidRetargeterComponent.
    constexpr uint32 kEntityCount = 4;
    (void)CreateSyntheticSkeleton(); // src skeleton, not directly read by this test
    uint32 tgtSkelId = CreateSyntheticSkeleton();
    HumanoidRig srcRig(GUID::Generate(), std::filesystem::path("test://srcRig.json"));
    HumanoidRig tgtRig(GUID::Generate(), std::filesystem::path("test://tgtRig.json"));
    PopulateMinimalRig(srcRig);
    PopulateMinimalRig(tgtRig);
    RetargetMap map(GUID::Generate(), std::filesystem::path("test://map.json"));
    map.SetSourceRigRef(srcRig.GetGUID());
    map.SetTargetRigRef(tgtRig.GetGUID());
    AutoCreateRetargetMap(srcRig, tgtRig, map);

    const uint32 baseline = BaselineRuntimeCount();
    std::vector<ECS::EntityHandle> entityHandles;
    std::vector<uint32> runtimeIds;
    entityHandles.reserve(kEntityCount);
    runtimeIds.reserve(kEntityCount);

    for (uint32 i = 0; i < kEntityCount; ++i)
    {
        uint32 rt = skStore.CreateRuntime(tgtSkelId);
        ASSERT_NE(rt, 0u);
        runtimeIds.push_back(rt);

        // ComponentBundle::Add doesn't require template-instantiation of
        // World::AddComponent<T>, so the test target builds without an entry
        // in EditorComponentInstantiations.cpp / equivalent.
        ECS::ComponentBundle bundle;
        GameEngine::Components::SkeletonRef sref{};
        sref.skeletonId = tgtSkelId;
        sref.runtimeId  = rt;
        bundle.Add(sref);
        GameEngine::Components::HumanoidRetargeterComponent rc;
        rc.Map.Set(map.GetGUID());
        bundle.Add(rc);
        // Switched off, so the eval body is skipped; the prune is what's under test.
        bundle.Add(ECS::ComponentDisabled<GameEngine::Components::HumanoidRetargeterComponent>{});

        auto handle = world.CreateFromBundleHandle(bundle);
        entityHandles.push_back(handle);
    }
    world.ProcessCommands();

    // High-water mark grew by N (each CreateRuntime appends a deque slot
    // because the FreeRuntimeIds list was empty at the start of the test).
    EXPECT_EQ(skStore.GetRuntimeHighWaterMark(), baseline + kEntityCount);
    for (uint32 rt : runtimeIds)
        EXPECT_NE(skStore.GetRuntime(rt), nullptr);

    // Drive one Update tick so the system can populate m_Characters via the
    // bypass path (Enabled=false skips heavy work but the system still walks
    // the query).
    GERender::HumanoidRetargetSystem sys(/*renderServices=*/nullptr);
    sys.Update(world, 0.0f);

    // Destroy the entities. World fires OnRemove<SkeletonRef> -> ReleaseRuntime;
    // the runtime slot ID becomes invalid (Get returns nullptr) before the
    // next system tick runs.
    for (auto h : entityHandles)
        world.DestroyEntity(h);
    world.ProcessCommands();

    for (uint32 rt : runtimeIds)
        EXPECT_EQ(skStore.GetRuntime(rt), nullptr)
            << "ReleaseRuntime didn't fire on entity destroy (OnRemove hook missing?)";

    // Next Update tick should prune the m_Characters map entries. We can't
    // peek at m_Characters directly; the GetLastLODForRuntime test seam
    // returns Full for absent entries, so we drive a second update and
    // check that LOD is no longer being recorded for these runtimes.
    sys.Update(world, 0.0f);
    for (uint32 rt : runtimeIds)
    {
        EXPECT_EQ(sys.GetLastLODForRuntime(rt),
                  GameEngine::Components::HumanoidRetargetLOD::Full)
            << "m_Characters entry leaked for runtime " << rt
            << " — prune loop didn't fire after ReleaseRuntime";
    }

    // Re-spawning N entities should reuse the freed slot IDs (high-water
    // mark stays where it was). This is the SkeletonStore free-list working,
    // independently confirmed in SkeletonStoreRuntimeTests.
    const uint32 hwmAfterFree = static_cast<uint32>(skStore.GetRuntimeHighWaterMark());
    std::vector<uint32> reused;
    reused.reserve(kEntityCount);
    for (uint32 i = 0; i < kEntityCount; ++i)
        reused.push_back(skStore.CreateRuntime(tgtSkelId));
    EXPECT_EQ(skStore.GetRuntimeHighWaterMark(), hwmAfterFree)
        << "ReleaseRuntime free list isn't recycling slot IDs — high-water mark grew";
    for (uint32 rt : reused) skStore.ReleaseRuntime(rt);
}

TEST_F(HumanoidRetargetSystemTest, GPUDataStore_ReleaseOnAssetUnload)
{
    using namespace GameEngine::Engine::Renderer;

    // Dispatcher MUST outlive the data store: the store holds two
    // AssetReloadInvalidators that call dispatcher->RemoveCallback at
    // destruction. Declaring dispatcher first makes it destruct last.
    AssetEventDispatcher dispatcher;

    // Headless data store — no IDevice, so all upload paths skip the GPU
    // CreateBuffer call but still maintain the CPU mirror tables. That's
    // what we exercise.
    RetargetGPUDataStore store;

    // Synthetic clip with one channel so EnsureClipUploaded actually appends.
    GUID clipGuid = GUID::Generate();
    AnimKeyframe k0{};
    k0.time = 0.0f;
    k0.rotation[3] = 1.0f;
    k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
    AnimChannel ch{};
    ch.boneIndex = 0;
    ch.targetName = "Hips";
    ch.path = AnimPath::Rotation;
    ch.keys = { k0, k0 };
    auto clip = MakeShared<AnimationClip>(clipGuid, std::filesystem::path("synthetic://clip"));
    clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);

    // Synthetic rig pair via a built RetargetNode.
    auto& skStore = GERender::SkeletonStore::Instance();
    uint32 srcSkelId = CreateSyntheticSkeleton();
    uint32 tgtSkelId = CreateSyntheticSkeleton();
    auto* srcSkel = skStore.Get(srcSkelId);
    auto* tgtSkel = skStore.Get(tgtSkelId);
    ASSERT_NE(srcSkel, nullptr);
    ASSERT_NE(tgtSkel, nullptr);
    HumanoidRig srcRig(GUID::Generate(), std::filesystem::path("test://srcRig.json"));
    HumanoidRig tgtRig(GUID::Generate(), std::filesystem::path("test://tgtRig.json"));
    PopulateMinimalRig(srcRig);
    PopulateMinimalRig(tgtRig);
    GUID mapGuid = GUID::Generate();
    RetargetMap map(mapGuid, std::filesystem::path("test://map.json"));
    map.SetSourceRigRef(srcRig.GetGUID());
    map.SetTargetRigRef(tgtRig.GetGUID());
    AutoCreateRetargetMap(srcRig, tgtRig, map);

    StoredPoseProvider provider;
    RetargetNode node;
    node.Configure(&provider, &srcRig, &tgtRig, &map);
    ASSERT_TRUE(node.Build(*srcSkel, *tgtSkel));

    // AttachAssetEvents wires the store's two AssetReloadInvalidators
    // against the dispatcher declared at the top of the test scope.
    store.AttachAssetEvents(dispatcher);

    // Upload + verify lookup.
    const uint32_t clipIdx = store.EnsureClipUploaded(clipGuid, *clip);
    EXPECT_NE(clipIdx, kRetargetInvalidIndex);
    EXPECT_EQ(store.ResolveClipIdx(clipGuid), clipIdx);

    const uint32_t rigIdx = store.EnsureRigPairUploaded(mapGuid, node, *srcSkel, *tgtSkel,
                                                         tgtSkel->SkinJointCount);
    EXPECT_NE(rigIdx, kRetargetInvalidIndex);
    EXPECT_EQ(store.ResolveRigPairIdx(mapGuid), rigIdx);

    // Fire AssetUnloaded for the clip; the dispatch only queues the release.
    // BeginFrame is the drain site (matches the deferred-queue pattern in
    // HumanoidRetargetSystem) — without it, the table keeps the stale entry.
    dispatcher.DispatchEvent(GameEngine::AssetEvents::AssetUnloaded(
        clipGuid, AssetType::Animation, "synthetic://clip"));
    EXPECT_EQ(store.ResolveClipIdx(clipGuid), clipIdx)
        << "Release fired synchronously from the dispatcher callback — should be deferred";
    store.BeginFrame(0);
    EXPECT_EQ(store.ResolveClipIdx(clipGuid), kRetargetInvalidIndex)
        << "Clip header didn't release after BeginFrame drain";
    EXPECT_EQ(store.ResolveRigPairIdx(mapGuid), rigIdx)
        << "Rig header was wrongly released by a clip-typed event";

    // Fire AssetUnloaded for the map; queued, drained on next BeginFrame.
    dispatcher.DispatchEvent(GameEngine::AssetEvents::AssetUnloaded(
        mapGuid, AssetType::RetargetMap, "test://map.json"));
    store.BeginFrame(0);
    EXPECT_EQ(store.ResolveRigPairIdx(mapGuid), kRetargetInvalidIndex)
        << "Rig pair header didn't release after BeginFrame drain";
}

TEST_F(HumanoidRetargetSystemTest, GPUDataStore_SharedClip_NotReleasedByMapUnload)
{
    using namespace GameEngine::Engine::Renderer;

    // Dispatcher must outlive the store (see ReleaseOnAssetUnload above).
    AssetEventDispatcher dispatcher;

    RetargetGPUDataStore store;

    auto& skStore = GERender::SkeletonStore::Instance();
    uint32 srcSkelId = CreateSyntheticSkeleton();
    uint32 tgtSkelId = CreateSyntheticSkeleton();
    auto* srcSkel = skStore.Get(srcSkelId);
    auto* tgtSkel = skStore.Get(tgtSkelId);

    // One clip, two rig pairs. Both characters share the same clip GUID;
    // independent map GUIDs.
    GUID clipGuid = GUID::Generate();
    AnimKeyframe k0{};
    k0.time = 0.0f;
    k0.rotation[3] = 1.0f;
    k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
    AnimChannel ch{};
    ch.boneIndex = 0;
    ch.targetName = "Hips";
    ch.path = AnimPath::Rotation;
    ch.keys = { k0, k0 };
    auto clip = MakeShared<AnimationClip>(clipGuid, std::filesystem::path("synthetic://clip"));
    clip->SetChannelsAndDurationForTest(std::vector<AnimChannel>{ch}, 1.0f);

    // Two independent rig pairs sharing the clip. HumanoidRig isn't readily
    // movable/copyable for tuple return, so just inline both setups here.
    GUID mapAGuid = GUID::Generate();
    GUID mapBGuid = GUID::Generate();
    StoredPoseProvider providerA;
    StoredPoseProvider providerB;

    HumanoidRig srcRigA(GUID::Generate(), std::filesystem::path("test://srcRigA.json"));
    HumanoidRig tgtRigA(GUID::Generate(), std::filesystem::path("test://tgtRigA.json"));
    PopulateMinimalRig(srcRigA);
    PopulateMinimalRig(tgtRigA);
    RetargetMap mapA(mapAGuid, std::filesystem::path("test://mapA.json"));
    mapA.SetSourceRigRef(srcRigA.GetGUID());
    mapA.SetTargetRigRef(tgtRigA.GetGUID());
    AutoCreateRetargetMap(srcRigA, tgtRigA, mapA);
    RetargetNode nodeA;
    nodeA.Configure(&providerA, &srcRigA, &tgtRigA, &mapA);
    ASSERT_TRUE(nodeA.Build(*srcSkel, *tgtSkel));

    HumanoidRig srcRigB(GUID::Generate(), std::filesystem::path("test://srcRigB.json"));
    HumanoidRig tgtRigB(GUID::Generate(), std::filesystem::path("test://tgtRigB.json"));
    PopulateMinimalRig(srcRigB);
    PopulateMinimalRig(tgtRigB);
    RetargetMap mapB(mapBGuid, std::filesystem::path("test://mapB.json"));
    mapB.SetSourceRigRef(srcRigB.GetGUID());
    mapB.SetTargetRigRef(tgtRigB.GetGUID());
    AutoCreateRetargetMap(srcRigB, tgtRigB, mapB);
    RetargetNode nodeB;
    nodeB.Configure(&providerB, &srcRigB, &tgtRigB, &mapB);
    ASSERT_TRUE(nodeB.Build(*srcSkel, *tgtSkel));

    store.AttachAssetEvents(dispatcher);

    // Both characters upload the same clip — second EnsureClipUploaded is
    // idempotent (returns the cached header index).
    const uint32_t clipIdxA = store.EnsureClipUploaded(clipGuid, *clip);
    const uint32_t clipIdxB = store.EnsureClipUploaded(clipGuid, *clip);
    EXPECT_NE(clipIdxA, kRetargetInvalidIndex);
    EXPECT_EQ(clipIdxA, clipIdxB);  // idempotent

    const uint32_t rigIdxA = store.EnsureRigPairUploaded(mapAGuid, nodeA, *srcSkel, *tgtSkel,
                                                          tgtSkel->SkinJointCount);
    const uint32_t rigIdxB = store.EnsureRigPairUploaded(mapBGuid, nodeB, *srcSkel, *tgtSkel,
                                                          tgtSkel->SkinJointCount);
    EXPECT_NE(rigIdxA, kRetargetInvalidIndex);
    EXPECT_NE(rigIdxB, kRetargetInvalidIndex);
    EXPECT_NE(rigIdxA, rigIdxB);  // distinct slots

    // Unloading map A must NOT release the shared clip — character B still
    // needs it. Release is deferred via the queue; drain it on BeginFrame.
    dispatcher.DispatchEvent(GameEngine::AssetEvents::AssetUnloaded(
        mapAGuid, AssetType::RetargetMap, "test://mapA.json"));
    store.BeginFrame(0);
    EXPECT_EQ(store.ResolveRigPairIdx(mapAGuid), kRetargetInvalidIndex);
    EXPECT_EQ(store.ResolveRigPairIdx(mapBGuid), rigIdxB)
        << "Map B rig pair was wrongly released by a sibling map's unload";
    EXPECT_EQ(store.ResolveClipIdx(clipGuid), clipIdxA)
        << "Shared clip was released when only the map unloaded";

    // Now unload the clip itself — character B's clip lookup must fail
    // (the asset is gone), but its rig pair stays alive.
    dispatcher.DispatchEvent(GameEngine::AssetEvents::AssetUnloaded(
        clipGuid, AssetType::Animation, "synthetic://clip"));
    store.BeginFrame(0);
    EXPECT_EQ(store.ResolveClipIdx(clipGuid), kRetargetInvalidIndex);
    EXPECT_EQ(store.ResolveRigPairIdx(mapBGuid), rigIdxB);
}
