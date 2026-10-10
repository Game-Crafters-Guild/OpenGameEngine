#include <gtest/gtest.h>

#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Animation/Nodes/RetargetNode.h"
#include "Animation/PoseStack.h"
#include "Animation/QuaternionMath.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

#include "AssetCore/GUID.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Memory/AllocationCountScope.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <filesystem>

using namespace GameEngine;
using namespace GameEngine::Animation;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

namespace
{

constexpr float kSameRigToleranceDeg = 1e-3f; // 1 milli-degree per bone
constexpr float kCrossRigToleranceDeg = 0.01f;
constexpr float kCrossValidationToleranceDeg = 1e-3f; // 1 milli-degree

float DegBetween(const Quaternion& a, const Quaternion& b)
{
    return AngleBetweenDegrees(a, b);
}

Quaternion AxisAngleDeg(const Vector3& axis, float deg)
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    return Quaternion::FromAxisAngle(axis, deg * kDegToRad);
}

// Trivial source-pose provider: copies a stored pose into outPose on
// Evaluate.
class StaticPoseNode : public AnimGraphNode
{
public:
    AnimationPose Pose;
    void Evaluate(EvaluationContext& /*ctx*/, AnimationPose& outPose) override
    {
        outPose = Pose;
    }
};

// Construct a tiny SkeletonData with N bones, parents 0,1,2... (each
// bone parented to the previous), rest local rotation per bone provided
// in restRot, rest local translation each (1,0,0) so the chain extends
// along X.
SkeletonData MakeChain(const std::vector<Quaternion>& restRot)
{
    const uint32_t bones = static_cast<uint32_t>(restRot.size());
    SkeletonData s;
    s.BoneCount = bones;
    s.Parent.resize(bones);
    s.RestTranslation.resize(bones * 3, 0.0f);
    s.RestRotation.resize(bones * 4, 0.0f);
    s.RestScale.resize(bones * 3, 1.0f);
    s.RestLocalMatrix.resize(bones * 16, 0.0f);
    s.BoneNames.resize(bones);
    for (uint32_t i = 0; i < bones; ++i)
    {
        s.Parent[i] = (i == 0) ? -1 : static_cast<int32>(i - 1);
        // Each bone offset by (1,0,0) along its parent's X.
        if (i > 0)
        {
            s.RestTranslation[i * 3 + 0] = 1.0f;
        }
        const glm::quat& q = restRot[i].GetGLM();
        s.RestRotation[i * 4 + 0] = q.x;
        s.RestRotation[i * 4 + 1] = q.y;
        s.RestRotation[i * 4 + 2] = q.z;
        s.RestRotation[i * 4 + 3] = q.w;
        s.RestScale[i * 3 + 0] = 1.0f;
        s.RestScale[i * 3 + 1] = 1.0f;
        s.RestScale[i * 3 + 2] = 1.0f;
        // identity rest local matrix is fine for these tests
        s.RestLocalMatrix[i * 16 + 0]  = 1.0f;
        s.RestLocalMatrix[i * 16 + 5]  = 1.0f;
        s.RestLocalMatrix[i * 16 + 10] = 1.0f;
        s.RestLocalMatrix[i * 16 + 15] = 1.0f;
        s.BoneNames[i] = "bone" + std::to_string(i);
    }
    s.BuildBoneNameLookup();
    return s;
}

// Populate an existing rig in place. Returns nothing — caller owns the rig
// (HumanoidRig inherits Asset which is non-copyable / non-movable).
void PopulateRig(HumanoidRig& rig,
                 const std::vector<HumanBone>& canonical,
                 const std::vector<Quaternion>& retargetQ,
                 const std::vector<HumanoidChain>& chains,
                 HumanBone translationBone = HumanBone::Hips,
                 float hipHeight = 1.0f)
{
    auto& bm = rig.BoneMapMutable();
    bm.clear();
    for (size_t i = 0; i < canonical.size(); ++i)
    {
        if (canonical[i] == HumanBone::None) continue;
        HumanoidBoneMapping m;
        m.Canonical = canonical[i];
        m.SourceBoneName = "bone" + std::to_string(i);
        m.CachedSourceIndex = static_cast<uint32_t>(i);
        m.RetargetPoseRotation = retargetQ[i];
        bm.push_back(m);
    }
    rig.ChainsMutable() = chains;
    auto& tb = rig.TranslationBonesMutable();
    tb.clear();
    tb.push_back(translationBone);
    rig.ProportionsMutable().HipHeight = hipHeight;
}

EvaluationContext MakeCtx(PoseStack& stack)
{
    EvaluationContext ctx;
    ctx.ScratchStack = &stack;
    ctx.DeltaTime = 0.0f;
    return ctx;
}

// Reference: Formulation B runtime decode from Appendix B (Phase 3 form).
// qSrcDelta[b] = inverse(srcRetargetPose[b]) * srcLocalPose[b]
// where srcRetargetPose[b] = RetargetPoseRotation[b] * bind_src[b].
// Parent-shift is identity in Phase 3 / Phase 11 (see RetargetNode::Build
// comment); the simple form is correct when the runtime always writes parents
// at their retarget pose, which the retarget pipeline guarantees.
Quaternion FormulationB_qSrcDelta(const SkeletonData& skel,
                                  const HumanoidRig& rig,
                                  const AnimationPose& clipKeys,
                                  uint32_t boneIdx)
{
    auto getCanonical = [&](uint32_t b) -> HumanBone
    {
        for (const auto& m : rig.BoneMap())
            if (m.CachedSourceIndex == b) return m.Canonical;
        return HumanBone::None;
    };
    auto getRetargetQ = [&](uint32_t b) -> Quaternion
    {
        for (const auto& m : rig.BoneMap())
            if (m.CachedSourceIndex == b) return m.RetargetPoseRotation;
        return Quaternion::Identity();
    };
    auto restRot = [&](uint32_t b) -> Quaternion
    {
        if (skel.RestRotation.size() < (b + 1) * 4) return Quaternion::Identity();
        const float* rp = &skel.RestRotation[b * 4];
        return Quaternion(glm::quat(rp[3], rp[0], rp[1], rp[2]));
    };

    if (getCanonical(boneIdx) == HumanBone::None)
        return clipKeys.Rotations[boneIdx];

    const Quaternion bind_src = restRot(boneIdx);
    const Quaternion srcRetarget = (getRetargetQ(boneIdx) * bind_src).Normalized();

    return (Inverse(srcRetarget) * clipKeys.Rotations[boneIdx]).Normalized();
}

} // namespace

// ===== Tests =====

// Same-rig identity: source rig == target rig, retarget pose identity, clip
// plays its rest pose -> output rotations match the rest pose.
TEST(RetargetNodeTests, SameRig_RestPose_IdentityWithinTolerance)
{
    // 3-bone chain: hips, spine, head. All identity rest.
    std::vector<Quaternion> rest = {
        Quaternion::Identity(),
        Quaternion::Identity(),
        Quaternion::Identity()
    };
    SkeletonData skel = MakeChain(rest);
    HumanoidChain spineChain;
    spineChain.Kind = ChainKind::Spine;
    spineChain.Start = HumanBone::Hips;
    spineChain.End = HumanBone::Head;
    spineChain.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};

    std::vector<HumanBone> canon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};
    std::vector<Quaternion> retargetQ(3, Quaternion::Identity());
    HumanoidRig rig(GUID(), std::filesystem::path("test://rig.humanoidrig.json"));
    PopulateRig(rig, canon, retargetQ, {spineChain});

    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    {
        ChainPairing p;
        p.Kind = ChainKind::Spine;
        p.FK.RotationMode = FKRotationMode::OneToOne;
        p.FK.RotationAlpha = 1.0f;
        map.ChainMapMutable().push_back(p);
    }

    StaticPoseNode source;
    source.Pose.Resize(3);
    for (uint32_t b = 0; b < 3; ++b) source.Pose.Rotations[b] = Quaternion::Identity();

    RetargetNode node;
    node.Configure(&source, &rig, &rig, &map);
    ASSERT_TRUE(node.Build(skel, skel));

    PoseStack stack;
    stack.Reserve(8);
    EvaluationContext ctx = MakeCtx(stack);

    AnimationPose out;
    node.Evaluate(ctx, out);

    ASSERT_EQ(out.BoneCount, 3u);
    for (uint32_t b = 0; b < 3; ++b)
        EXPECT_LT(DegBetween(out.Rotations[b], Quaternion::Identity()), kSameRigToleranceDeg);
}

// Same-rig clip plays a non-identity pose: target reproduces it within
// 1e-4° per bone.
TEST(RetargetNodeTests, SameRig_PerturbedClip_ReproducesSource)
{
    std::vector<Quaternion> rest = {
        Quaternion::Identity(),
        Quaternion::Identity(),
        Quaternion::Identity()
    };
    SkeletonData skel = MakeChain(rest);
    HumanoidChain spineChain;
    spineChain.Kind = ChainKind::Spine;
    spineChain.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};

    std::vector<HumanBone> canon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};
    std::vector<Quaternion> retargetQ(3, Quaternion::Identity());
    HumanoidRig rig(GUID(), std::filesystem::path("test://rig.humanoidrig.json"));
    PopulateRig(rig, canon, retargetQ, {spineChain});

    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    ChainPairing p;
    p.Kind = ChainKind::Spine;
    p.FK.RotationMode = FKRotationMode::OneToOne;
    p.FK.RotationAlpha = 1.0f;
    map.ChainMapMutable().push_back(p);

    // Clip: each bone rotated 10°, 20°, 30° on Y.
    StaticPoseNode source;
    source.Pose.Resize(3);
    source.Pose.Rotations[0] = AxisAngleDeg({0,1,0}, 10.0f);
    source.Pose.Rotations[1] = AxisAngleDeg({0,1,0}, 20.0f);
    source.Pose.Rotations[2] = AxisAngleDeg({0,1,0}, 30.0f);

    RetargetNode node;
    node.Configure(&source, &rig, &rig, &map);
    ASSERT_TRUE(node.Build(skel, skel));

    PoseStack stack;
    stack.Reserve(8);
    EvaluationContext ctx = MakeCtx(stack);
    AnimationPose out;
    node.Evaluate(ctx, out);

    ASSERT_EQ(out.BoneCount, 3u);
    for (uint32_t b = 0; b < 3; ++b)
        EXPECT_LT(DegBetween(out.Rotations[b], source.Pose.Rotations[b]), kSameRigToleranceDeg);
}

// Cross-rig 5° X-tilt source bind, identity-bind target. Source plays its
// authored bind (clip = tilt5). Phase 25.2 semantics: target preserves the
// SOURCE'S authored visual, not target's bind visual. So target's resulting
// world rotation matches source's authored world (= tilt5 at bone 0, tilt10
// at bone 1, tilt15 at bone 2 — chained 5° tilts), and target's local
// rotation per bone is tilt5 (the per-bone delta that builds those world
// rotations on a chain whose bind is identity).
//
// The bake Q sets the rig's CANONICAL T-pose to identity world (Q = inv(bind)
// at every bone). At runtime, source clip is read in source's AUTHORED frame
// (no Q applied to clip locals); the world-space delta from source's
// canonical bind to source's authored clip carries the bind offset onto
// target. With matching canonical Q (here both rigs reduce to canonical
// T-pose at bind), target's pose is source's authored visual.
TEST(RetargetNodeTests, CrossRig_5DegXTilt_TargetPreservesSourceAuthoredVisual)
{
    const Quaternion tilt5 = AxisAngleDeg({1,0,0}, 5.0f);

    // Source rig: bones tilted 5° on X.
    std::vector<Quaternion> srcRest = {tilt5, tilt5, tilt5};
    SkeletonData srcSkel = MakeChain(srcRest);

    // Target rig: identity bind.
    std::vector<Quaternion> tgtRest = {
        Quaternion::Identity(),
        Quaternion::Identity(),
        Quaternion::Identity()
    };
    SkeletonData tgtSkel = MakeChain(tgtRest);

    HumanoidChain spineChain;
    spineChain.Kind = ChainKind::Spine;
    spineChain.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};

    std::vector<HumanBone> canon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};
    // Source RetargetPoseRotation: Q s.t. Q * bind_src = identity (canonical
    // T-pose at bind). Under the chain-aware bake, parent-canonical is
    // identity at every bone, so per-bone Q = inv(bind_src) suffices.
    std::vector<Quaternion> srcRetargetQ(3, Inverse(tilt5));
    // Target RetargetPoseRotation: identity (already canonical T-pose at bind).
    std::vector<Quaternion> tgtRetargetQ(3, Quaternion::Identity());

    HumanoidRig srcRig(GUID(), std::filesystem::path("test://srcRig.humanoidrig.json"));
    PopulateRig(srcRig, canon, srcRetargetQ, {spineChain});
    HumanoidRig tgtRig(GUID(), std::filesystem::path("test://tgtRig.humanoidrig.json"));
    PopulateRig(tgtRig, canon, tgtRetargetQ, {spineChain});

    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    ChainPairing p;
    p.Kind = ChainKind::Spine;
    p.FK.RotationMode = FKRotationMode::OneToOne;
    p.FK.RotationAlpha = 1.0f;
    map.ChainMapMutable().push_back(p);

    // Source plays its authored bind: clip locals = tilt5 (= source bind).
    StaticPoseNode source;
    source.Pose.Resize(3);
    for (uint32_t b = 0; b < 3; ++b)
        source.Pose.Rotations[b] = tilt5;

    RetargetNode node;
    node.Configure(&source, &srcRig, &tgtRig, &map);
    ASSERT_TRUE(node.Build(srcSkel, tgtSkel));

    PoseStack stack;
    stack.Reserve(8);
    EvaluationContext ctx = MakeCtx(stack);
    AnimationPose out;
    node.Evaluate(ctx, out);

    ASSERT_EQ(out.BoneCount, 3u);
    // Target locals match source's authored bind (tilt5 per bone): the
    // per-bone delta that produces source's authored world chain (5°, 10°,
    // 15°) on a target rig with identity bind. This is the cross-rig
    // "preserve source visual" guarantee at the bind frame.
    for (uint32_t b = 0; b < 3; ++b)
    {
        const float err = DegBetween(out.Rotations[b], tilt5);
        EXPECT_LT(err, kCrossRigToleranceDeg)
            << "bone " << b << " err=" << err << " deg";
    }
}

// Determinism: two evaluations on identical inputs produce bit-exact poses.
TEST(RetargetNodeTests, Deterministic_BitExactAcrossRuns)
{
    std::vector<Quaternion> rest = {
        Quaternion::Identity(),
        Quaternion::Identity(),
        Quaternion::Identity()
    };
    SkeletonData skel = MakeChain(rest);
    HumanoidChain spineChain;
    spineChain.Kind = ChainKind::Spine;
    spineChain.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};

    std::vector<HumanBone> canon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};
    std::vector<Quaternion> retargetQ(3, Quaternion::Identity());
    HumanoidRig rig(GUID(), std::filesystem::path("test://rig.humanoidrig.json"));
    PopulateRig(rig, canon, retargetQ, {spineChain});
    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    ChainPairing p; p.Kind = ChainKind::Spine; p.FK.RotationMode = FKRotationMode::OneToOne;
    map.ChainMapMutable().push_back(p);

    StaticPoseNode source;
    source.Pose.Resize(3);
    source.Pose.Rotations[0] = AxisAngleDeg({0,1,0}, 17.0f);
    source.Pose.Rotations[1] = AxisAngleDeg({1,0,0}, 23.0f);
    source.Pose.Rotations[2] = AxisAngleDeg({0,0,1}, 41.0f);

    RetargetNode node;
    node.Configure(&source, &rig, &rig, &map);
    ASSERT_TRUE(node.Build(skel, skel));

    PoseStack stack;
    stack.Reserve(8);
    EvaluationContext ctx = MakeCtx(stack);

    AnimationPose first, second;
    node.Evaluate(ctx, first);
    node.Evaluate(ctx, second);

    ASSERT_EQ(first.BoneCount, second.BoneCount);
    for (uint32_t b = 0; b < first.BoneCount; ++b)
    {
        const glm::quat& a = first.Rotations[b].GetGLM();
        const glm::quat& c = second.Rotations[b].GetGLM();
        EXPECT_EQ(a.w, c.w);
        EXPECT_EQ(a.x, c.x);
        EXPECT_EQ(a.y, c.y);
        EXPECT_EQ(a.z, c.z);
    }
}

// Allocation gate: zero allocs per Evaluate after warm-up.
TEST(RetargetNodeTests, ZeroAllocationsAfterWarmup)
{
    std::vector<Quaternion> rest = {
        Quaternion::Identity(),
        Quaternion::Identity(),
        Quaternion::Identity()
    };
    SkeletonData skel = MakeChain(rest);
    HumanoidChain spineChain;
    spineChain.Kind = ChainKind::Spine;
    spineChain.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};

    std::vector<HumanBone> canon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Head};
    std::vector<Quaternion> retargetQ(3, Quaternion::Identity());
    HumanoidRig rig(GUID(), std::filesystem::path("test://rig.humanoidrig.json"));
    PopulateRig(rig, canon, retargetQ, {spineChain});
    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    ChainPairing p; p.Kind = ChainKind::Spine; p.FK.RotationMode = FKRotationMode::OneToOne;
    map.ChainMapMutable().push_back(p);

    StaticPoseNode source;
    source.Pose.Resize(3);
    source.Pose.Rotations[0] = AxisAngleDeg({0,1,0}, 5.0f);
    source.Pose.Rotations[1] = AxisAngleDeg({0,1,0}, 10.0f);
    source.Pose.Rotations[2] = AxisAngleDeg({0,1,0}, 15.0f);

    RetargetNode node;
    node.Configure(&source, &rig, &rig, &map);
    ASSERT_TRUE(node.Build(skel, skel));

    PoseStack stack;
    stack.Reserve(8);
    EvaluationContext ctx = MakeCtx(stack);

    AnimationPose out;
    out.Resize(3); // warm-up
    node.Evaluate(ctx, out); // warm-up

    GameEngine::Memory::AllocationCountScope allocations(GameEngine::Memory::CountWindow::ThisThread);
    node.Evaluate(ctx, out);
    EXPECT_EQ(allocations.Count(), 0u);
}

// Cross-rig chain count mismatch: 3-bone source spine, 5-bone target spine.
// World-space retarget transports each canonically-mapped bone independently;
// target bones 3+ that have no canonical match on the source side stay at
// their bind local rotation. This supersedes the legacy Mode B (arc-length
// resample) and Mode D (world-arc slerp) semantics, which produced
// artificially-smoothed rotations along the target chain regardless of
// which bones actually animated on the source side.
TEST(RetargetNodeTests, CrossRig_3SrcTo5TgtSpine_WorldSpaceCanonicalMap)
{
    std::vector<Quaternion> srcRest(3, Quaternion::Identity());
    SkeletonData srcSkel = MakeChain(srcRest);
    std::vector<Quaternion> tgtRest(5, Quaternion::Identity());
    SkeletonData tgtSkel = MakeChain(tgtRest);

    HumanoidChain srcSpine;
    srcSpine.Kind = ChainKind::Spine;
    srcSpine.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest};
    HumanoidChain tgtSpine;
    tgtSpine.Kind = ChainKind::Spine;
    tgtSpine.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest,
                              HumanBone::UpperChest, HumanBone::Neck};

    std::vector<HumanBone> srcCanon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest};
    std::vector<HumanBone> tgtCanon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest,
                                       HumanBone::UpperChest, HumanBone::Neck};
    HumanoidRig srcRig(GUID(), std::filesystem::path("test://srcRig.humanoidrig.json"));
    PopulateRig(srcRig, srcCanon, std::vector<Quaternion>(3, Quaternion::Identity()), {srcSpine});
    HumanoidRig tgtRig(GUID(), std::filesystem::path("test://tgtRig.humanoidrig.json"));
    PopulateRig(tgtRig, tgtCanon, std::vector<Quaternion>(5, Quaternion::Identity()), {tgtSpine});

    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    ChainPairing p;
    p.Kind = ChainKind::Spine;
    p.FK.RotationMode = FKRotationMode::OneToOne;
    p.FK.RotationAlpha = 1.0f;
    map.ChainMapMutable().push_back(p);

    StaticPoseNode source;
    source.Pose.Resize(3);
    source.Pose.Rotations[0] = AxisAngleDeg({0,1,0},  0.0f);
    source.Pose.Rotations[1] = AxisAngleDeg({0,1,0}, 30.0f);
    source.Pose.Rotations[2] = AxisAngleDeg({0,1,0}, 60.0f);

    RetargetNode node;
    node.Configure(&source, &srcRig, &tgtRig, &map);
    ASSERT_TRUE(node.Build(srcSkel, tgtSkel));

    PoseStack stack;
    stack.Reserve(8);
    EvaluationContext ctx = MakeCtx(stack);
    AnimationPose out;
    node.Evaluate(ctx, out);

    // World-space retarget on identity-bind rigs reproduces source local
    // rotations one-to-one for matched canonicals: target_world == source_world,
    // and parent walks match because both rigs have identity bind world.
    EXPECT_LT(DegBetween(out.Rotations[0], source.Pose.Rotations[0]), 0.1f) << "Hips";
    EXPECT_LT(DegBetween(out.Rotations[1], source.Pose.Rotations[1]), 0.1f) << "Spine";
    EXPECT_LT(DegBetween(out.Rotations[2], source.Pose.Rotations[2]), 0.1f) << "Chest";
    // Unmapped canonicals stay at bind (identity) — the source side has no
    // bone to drive them.
    EXPECT_LT(DegBetween(out.Rotations[3], Quaternion::Identity()), 0.1f) << "UpperChest unmapped";
    EXPECT_LT(DegBetween(out.Rotations[4], Quaternion::Identity()), 0.1f) << "Neck unmapped";
}

// Antipode test: feed a "spinning" clip with quaternions that cross
// hemispheres on consecutive frames; consecutive output deltas stay <= 1.0
// dot magnitude (no antipode pop).
TEST(RetargetNodeTests, Antipode_NoPopOnHemisphereCross)
{
    std::vector<Quaternion> rest = {
        Quaternion::Identity(),
        Quaternion::Identity(),
        Quaternion::Identity()
    };
    SkeletonData skel = MakeChain(rest);
    HumanoidChain spineChain;
    spineChain.Kind = ChainKind::Spine;
    spineChain.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest};
    std::vector<HumanBone> canon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest};
    HumanoidRig rig(GUID(), std::filesystem::path("test://rig.humanoidrig.json"));
    PopulateRig(rig, canon, std::vector<Quaternion>(3, Quaternion::Identity()),
                              {spineChain});
    // Mode B forces a slerp at every Evaluate; antipode flips would surface
    // on hemisphere-crossing source rotations.
    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    ChainPairing p;
    p.Kind = ChainKind::Spine;
    p.FK.RotationMode = FKRotationMode::Transport;
    p.FK.RotationAlpha = 1.0f;
    map.ChainMapMutable().push_back(p);

    StaticPoseNode source;
    source.Pose.Resize(3);

    RetargetNode node;
    node.Configure(&source, &rig, &rig, &map);
    ASSERT_TRUE(node.Build(skel, skel));

    PoseStack stack;
    stack.Reserve(8);
    EvaluationContext ctx = MakeCtx(stack);
    AnimationPose out;

    Quaternion prev = Quaternion::Identity();
    bool first = true;
    // Walk a 360° spin in 32 steps: angles 0, 22.5, 45, ..., 337.5. The
    // quaternion crosses hemispheres at 180°.
    for (int frame = 0; frame < 32; ++frame)
    {
        const float angle = static_cast<float>(frame) * 11.25f;
        for (uint32_t b = 0; b < 3; ++b)
            source.Pose.Rotations[b] = AxisAngleDeg({0,1,0}, angle);
        node.Evaluate(ctx, out);
        if (!first)
        {
            // Per-frame world-rotation delta should be small (~11.25°).
            for (uint32_t b = 0; b < 3; ++b)
            {
                const float deg = AngleBetweenDegrees(out.Rotations[b], prev);
                EXPECT_LT(deg, 25.0f) << "antipode pop at frame " << frame << " bone " << b;
            }
        }
        prev = out.Rotations[0];
        first = false;
    }
}

// Cross-profile placeholder test: Phase 3 treats axis frames as identity.
// When source and target reference the SAME profile (the dominant case),
// the "cross-profile" math reduces to same-rig. Documented limitation: a
// Phase 4+ ticket will wire CanonicalAxisFrame into SkeletonProfile.
TEST(RetargetNodeTests, CrossProfile_SameProfileFallback_Identity)
{
    std::vector<Quaternion> rest(3, Quaternion::Identity());
    SkeletonData skel = MakeChain(rest);
    HumanoidChain chain;
    chain.Kind = ChainKind::Spine;
    chain.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest};
    std::vector<HumanBone> canon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest};
    HumanoidRig rig(GUID(), std::filesystem::path("test://rig.humanoidrig.json"));
    PopulateRig(rig, canon, std::vector<Quaternion>(3, Quaternion::Identity()),
                              {chain});

    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    ChainPairing p; p.Kind = ChainKind::Spine; p.FK.RotationMode = FKRotationMode::OneToOne;
    map.ChainMapMutable().push_back(p);

    StaticPoseNode source;
    source.Pose.Resize(3);
    source.Pose.Rotations[0] = AxisAngleDeg({0,1,0}, 13.0f);
    source.Pose.Rotations[1] = AxisAngleDeg({1,0,0}, 7.0f);
    source.Pose.Rotations[2] = AxisAngleDeg({0,0,1}, -19.0f);

    RetargetNode node;
    node.Configure(&source, &rig, &rig, &map);
    ASSERT_TRUE(node.Build(skel, skel));

    PoseStack stack;
    stack.Reserve(8);
    EvaluationContext ctx = MakeCtx(stack);
    AnimationPose out;
    node.Evaluate(ctx, out);

    for (uint32_t b = 0; b < 3; ++b)
        EXPECT_LT(DegBetween(out.Rotations[b], source.Pose.Rotations[b]),
                  kSameRigToleranceDeg);
}

// Stage 3 translation: hip translation scales by hip-height ratio; non-
// translation bones lock to target retarget pose's position.
TEST(RetargetNodeTests, Stage3_TranslationScales_HipHeightRatio)
{
    std::vector<Quaternion> rest(3, Quaternion::Identity());
    SkeletonData srcSkel = MakeChain(rest);
    SkeletonData tgtSkel = MakeChain(rest);
    HumanoidChain chain;
    chain.Kind = ChainKind::Spine;
    chain.IncludeBones = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest};
    std::vector<HumanBone> canon = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest};

    // Source hip height = 1.0; target hip height = 1.5.
    HumanoidRig srcRig(GUID(), std::filesystem::path("test://srcRig.humanoidrig.json"));
    PopulateRig(srcRig, canon, std::vector<Quaternion>(3, Quaternion::Identity()),
                                  {chain}, HumanBone::Hips, 1.0f);
    HumanoidRig tgtRig(GUID(), std::filesystem::path("test://tgtRig.humanoidrig.json"));
    PopulateRig(tgtRig, canon, std::vector<Quaternion>(3, Quaternion::Identity()),
                                  {chain}, HumanBone::Hips, 1.5f);
    RetargetMap map(GUID(), std::filesystem::path("test://map.retargetmap.json"));
    ChainPairing p; p.Kind = ChainKind::Spine; p.FK.RotationMode = FKRotationMode::OneToOne;
    map.ChainMapMutable().push_back(p);

    StaticPoseNode source;
    source.Pose.Resize(3);
    source.Pose.Positions[0] = Vector3(0.2f, 1.1f, 0.3f); // hip world translation
    source.Pose.Positions[1] = Vector3(0.0f, 0.5f, 0.0f); // spine offset (not translated)
    source.Pose.Positions[2] = Vector3(0.0f, 0.5f, 0.0f);

    RetargetNode node;
    node.Configure(&source, &srcRig, &tgtRig, &map);
    ASSERT_TRUE(node.Build(srcSkel, tgtSkel));

    PoseStack stack; stack.Reserve(8);
    EvaluationContext ctx = MakeCtx(stack);
    AnimationPose out;
    node.Evaluate(ctx, out);

    ASSERT_EQ(out.BoneCount, 3u);
    // Hip: scaled by 1.5.
    EXPECT_NEAR(out.Positions[0].x, 0.2f * 1.5f, 1e-5f);
    EXPECT_NEAR(out.Positions[0].y, 1.1f * 1.5f, 1e-5f);
    EXPECT_NEAR(out.Positions[0].z, 0.3f * 1.5f, 1e-5f);
    // Spine: forced to target rest translation (1,0,0) per MakeChain layout.
    EXPECT_NEAR(out.Positions[1].x, 1.0f, 1e-5f);
    EXPECT_NEAR(out.Positions[1].y, 0.0f, 1e-5f);
    EXPECT_NEAR(out.Positions[1].z, 0.0f, 1e-5f);
}
