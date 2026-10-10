#include "Animation/HumanoidImport.h"

#include "Animation/KHRHumanoidImporter.h"
#include "Animation/VRM1Importer.h"
#include "Logger/Logger.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine
{
namespace Animation
{

const char* HumanoidImportPathToString(HumanoidImportPath path)
{
    switch (path)
    {
        case HumanoidImportPath::VRM1:        return "VRM1";
        case HumanoidImportPath::KHRHumanoid: return "KHRHumanoid";
        case HumanoidImportPath::Heuristic:   return "Heuristic";
        case HumanoidImportPath::None:        return "None";
    }
    return "None";
}

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;

constexpr float kAPoseAngleThresholdDeg = 30.0f;     // shoulder->wrist tilt past horizontal
constexpr float kAPoseMinArmLength      = 0.1f;      // sanity check on shoulder->wrist length

// Name coverage can pass on a four-legged rest pose when front legs reuse
// arm bone names. Hands at foot height distinguishes that from a biped T/A-pose.
constexpr float kFourLeggedHandFootYFraction = 0.25f;
constexpr float kMinStanceBodyHeight = 0.05f;

// Compose world-space rest-position of the source bone by walking the
// parent chain and concatenating local rest TRS matrices.
glm::vec4 ComputeWorldRestPosition(const SkeletonData& skel, uint32 boneIdx)
{
    if (boneIdx >= skel.BoneCount) return glm::vec4(0.0f);

    glm::mat4 m(1.0f);
    int32 cur = static_cast<int32>(boneIdx);
    // Walk leaf-to-root composing parent * local at each step. Bound the
    // loop defensively in case of malformed parent links.
    for (int hops = 0; hops < 1024 && cur >= 0; ++hops)
    {
        glm::vec3 t(0.0f), s(1.0f);
        glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
        if (skel.RestTranslation.size() >= static_cast<size_t>(cur + 1) * 3)
        {
            const float* tp = &skel.RestTranslation[cur * 3];
            t = glm::vec3(tp[0], tp[1], tp[2]);
        }
        if (skel.RestRotation.size() >= static_cast<size_t>(cur + 1) * 4)
        {
            const float* rp = &skel.RestRotation[cur * 4];
            r = glm::quat(rp[3], rp[0], rp[1], rp[2]); // (w,x,y,z) <- (x,y,z,w)
        }
        if (skel.RestScale.size() >= static_cast<size_t>(cur + 1) * 3)
        {
            const float* sp = &skel.RestScale[cur * 3];
            s = glm::vec3(sp[0], sp[1], sp[2]);
        }
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), t)
                              * glm::mat4_cast(r)
                              * glm::scale(glm::mat4(1.0f), s);
        m = local * m;

        if (static_cast<size_t>(cur) >= skel.Parent.size()) break;
        cur = skel.Parent[cur];
    }

    return m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
}

// Read a bone's local rest rotation as a glm quaternion. Defaults to
// identity if storage is missing.
glm::quat ReadBoneRestRotation(const SkeletonData& skel, uint32 boneIdx)
{
    if (skel.RestRotation.size() >= static_cast<size_t>(boneIdx + 1) * 4)
    {
        const float* rp = &skel.RestRotation[boneIdx * 4];
        return glm::quat(rp[3], rp[0], rp[1], rp[2]);
    }
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

// Lookup the cached source-bone index for a canonical bone, or ~0u if
// unmapped. Uses HumanoidNameMatcher::Result::ByCanonical for O(1) access.
uint32 SourceBoneFor(const HumanoidNameMatcher::Result& mapping, HumanBone bone)
{
    const auto* m = mapping.Find(bone);
    if (!m) return ~0u;
    return m->CachedSourceIndex;
}

bool LooksLikeFourLeggedRest(const SkeletonData& skel, const HumanoidNameMatcher::Result& mapping)
{
    auto worldY = [&](HumanBone bone) -> float {
        const uint32 i = SourceBoneFor(mapping, bone);
        if (i == ~0u)
            return std::numeric_limits<float>::quiet_NaN();
        return ComputeWorldRestPosition(skel, i).y;
    };
    const float hipY = worldY(HumanBone::Hips);
    const float leftHandY = worldY(HumanBone::LeftHand);
    const float rightHandY = worldY(HumanBone::RightHand);
    const float leftFootY = worldY(HumanBone::LeftFoot);
    const float rightFootY = worldY(HumanBone::RightFoot);
    if (!std::isfinite(hipY) || !std::isfinite(leftHandY) || !std::isfinite(rightHandY)
        || !std::isfinite(leftFootY) || !std::isfinite(rightFootY))
        return false;

    const float handY = 0.5f * (leftHandY + rightHandY);
    const float footY = 0.5f * (leftFootY + rightFootY);
    const float bodyHeight = std::abs(hipY - footY);
    if (bodyHeight < kMinStanceBodyHeight)
        return false;
    return std::abs(handY - footY) < kFourLeggedHandFootYFraction * bodyHeight;
}

// Returns the canonical bones that participate in each ChainKind. The first
// entry is the chain Start, the last is the End; all the entries in between
// (inclusive) are the IncludeBones list.
const std::vector<HumanBone>& CanonicalChainBones(ChainKind kind)
{
    static const std::vector<HumanBone> kEmpty;
    static const std::vector<HumanBone> kLeftArm  = {HumanBone::LeftShoulder, HumanBone::LeftUpperArm, HumanBone::LeftLowerArm, HumanBone::LeftHand};
    static const std::vector<HumanBone> kRightArm = {HumanBone::RightShoulder, HumanBone::RightUpperArm, HumanBone::RightLowerArm, HumanBone::RightHand};
    static const std::vector<HumanBone> kLeftLeg  = {HumanBone::LeftUpperLeg, HumanBone::LeftLowerLeg, HumanBone::LeftFoot, HumanBone::LeftToes};
    static const std::vector<HumanBone> kRightLeg = {HumanBone::RightUpperLeg, HumanBone::RightLowerLeg, HumanBone::RightFoot, HumanBone::RightToes};
    static const std::vector<HumanBone> kSpine    = {HumanBone::Hips, HumanBone::Spine, HumanBone::Chest, HumanBone::UpperChest, HumanBone::Neck, HumanBone::Head};
    static const std::vector<HumanBone> kHead     = {HumanBone::Neck, HumanBone::Head};

    switch (kind)
    {
        case ChainKind::LeftArm:  return kLeftArm;
        case ChainKind::RightArm: return kRightArm;
        case ChainKind::LeftLeg:  return kLeftLeg;
        case ChainKind::RightLeg: return kRightLeg;
        case ChainKind::Spine:    return kSpine;
        case ChainKind::Head:     return kHead;
        default:                  return kEmpty;
    }
}

} // namespace

APoseDetectionResult DetectAPose(const SkeletonData& skel,
                                 const HumanoidNameMatcher::Result& mapping)
{
    APoseDetectionResult result;

    auto sideAngle = [&](HumanBone shoulder, HumanBone wrist) -> std::pair<bool, float>
    {
        const uint32 s = SourceBoneFor(mapping, shoulder);
        const uint32 w = SourceBoneFor(mapping, wrist);
        if (s == ~0u || w == ~0u) return {false, 0.0f};

        const glm::vec4 ps = ComputeWorldRestPosition(skel, s);
        const glm::vec4 pw = ComputeWorldRestPosition(skel, w);
        const glm::vec3 vec = glm::vec3(pw - ps);
        const float len = glm::length(vec);
        if (len < kAPoseMinArmLength) return {false, 0.0f};

        // Angle from world horizontal. We measure the absolute Y deflection
        // (downward A-pose vs horizontal T-pose); the sign is irrelevant.
        const float horizLen = std::sqrt(vec.x * vec.x + vec.z * vec.z);
        const float angle = std::atan2(std::abs(vec.y), std::max(horizLen, 1e-6f)) * kRadToDeg;
        return {true, angle};
    };

    const auto leftPair  = sideAngle(HumanBone::LeftUpperArm,  HumanBone::LeftHand);
    const auto rightPair = sideAngle(HumanBone::RightUpperArm, HumanBone::RightHand);

    result.LeftSidePresent  = leftPair.first;
    result.RightSidePresent = rightPair.first;
    result.ShoulderToWristAngleDegLeft  = leftPair.second;
    result.ShoulderToWristAngleDegRight = rightPair.second;

    // EITHER side past 30deg counts as A-pose; some rigs pose only one arm
    // sample, but Synty / mannequin rigs are symmetric — both fire.
    const bool leftAPose  = result.LeftSidePresent  && result.ShoulderToWristAngleDegLeft  > kAPoseAngleThresholdDeg;
    const bool rightAPose = result.RightSidePresent && result.ShoulderToWristAngleDegRight > kAPoseAngleThresholdDeg;
    result.IsAPose = leftAPose || rightAPose;
    return result;
}

// Quaternion that rotates unit vector `from` to unit vector `to`. Returns
// identity when the inputs are already aligned; uses an arbitrary
// perpendicular axis for the 180° (antipodal) case to avoid the degenerate
// shortest-arc formula.
glm::quat QuatFromTo(const glm::vec3& from, const glm::vec3& to)
{
    const float d = glm::dot(from, to);
    if (d > 0.99999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.99999f)
    {
        // 180° flip — pick an axis orthogonal to `from`.
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), from);
        if (glm::length(axis) < 1e-4f)
            axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), from);
        axis = glm::normalize(axis);
        return glm::angleAxis(kPi, axis);
    }
    const glm::vec3 cross = glm::cross(from, to);
    glm::quat q(1.0f + d, cross.x, cross.y, cross.z);
    return glm::normalize(q);
}

// Walk the canonical chain definitions to find the next bone after `bone`
// in its chain. Returns HumanBone::None if the bone is at the end of its
// chain (or not part of any chain).
HumanBone FindCanonicalChild(HumanBone bone)
{
    constexpr ChainKind kKinds[] = {
        ChainKind::Spine,
        ChainKind::LeftArm,
        ChainKind::RightArm,
        ChainKind::LeftLeg,
        ChainKind::RightLeg,
        ChainKind::Head,
    };
    for (ChainKind k : kKinds)
    {
        const auto& chain = CanonicalChainBones(k);
        for (size_t i = 0; i + 1 < chain.size(); ++i)
            if (chain[i] == bone) return chain[i + 1];
    }
    return HumanBone::None;
}

// Phase 25.1 — Per-bone retarget-pose Q derivation (chain-aware).
//
// For each canonical bone in the rig, compute Q (a bone-local pre-rotation)
// such that, when applied as `R_c_local = Q * R_a_local`, every bone in the
// canonical chain has its effective world rotation aligned to the canonical
// T-pose direction defined by SkeletonProfile.LocalForward. Combined with
// the world-space transport in RetargetNode, this brings every rig —
// regardless of authored bind orientation — into a shared canonical T-pose at
// retarget bind. The clip's per-frame motion is then transported in canonical
// frame, where source's and target's bind world rotations agree by
// construction.
//
// Why "chain-aware" and what changed from the earlier per-bone formulation:
//   The naive bake `Q_local = inv(parent_authored) * qWorld * parent_authored`
//   is a similarity transform that preserves qWorld's effect *in the
//   authored parent frame*. But when we compose canonical locals along the
//   chain, each child sees its parent's CANONICAL world (which differs from
//   the authored world by the parent's own qWorld). The similarity-transform
//   Q computed in the authored frame stops aligning correctly two bones into
//   the chain, because the parent's frame has been rotated.
//
//   The correct construction picks Q so that
//       parent_canonical * Q * R_a_local = qWorld * authored_world[bone]
//   which gives
//       Q = inv(parent_canonical) * qWorld * parent_authored
//   Note this is NOT a similarity transform — the LHS uses canonical, the
//   RHS uses authored. Walking bones in topological order lets us fill
//   parent_canonical for each child from its parent's already-computed Q.
//
// Method:
//   1. Walk all bones in topological (parent-before-child) order.
//   2. For every bone, maintain authored_world[b] = parent_authored * R_a_local[b]
//      and canonical_world[b]:
//        - canonical_world[b] = parent_canonical * Q * R_a_local[b] for mapped
//          bones (where Q comes from step 3),
//        - canonical_world[b] = parent_canonical * R_a_local[b] for unmapped
//          bones (Q = identity).
//   3. For each canonical-mapped bone with a canonical child:
//        a. authoredDirection = (child_world_pos - own_world_pos), normalized.
//        b. canonicalDirection = profile.LocalForward (profile rest is identity,
//           so bone-local axes coincide with world axes at canonical T-pose).
//        c. qWorld = QuatFromTo(authoredDirection, canonicalDirection).
//        d. Q_local = inv(canonical_world[parent]) * qWorld * authored_world[parent].
//
// Bones with no canonical child (Hand, Foot, Toes, Head, Eyes, Jaw, terminal
// fingers) get Q = identity — they have no skeleton-derived direction to
// align. Hips also gets Q = identity: it has multiple children with no single
// segment direction, and downstream world-space transport handles the body
// root via the canonical T-pose match anyway.
void BakeRetargetPoseFromAPose(const SkeletonData& skel,
                               const SkeletonProfile& profile,
                               HumanoidRig& outRig)
{
    auto& boneMap = outRig.BoneMapMutable();

    // Build a bone-index → BoneMap pointer table so the topological walk can
    // do an O(1) "is this bone canonical-mapped" lookup. The reverse table
    // also resolves canonical-child bone indices without a second pass over
    // the bone map (used by the segment-direction step).
    std::vector<HumanoidBoneMapping*> boneToMapping(skel.BoneCount, nullptr);
    std::vector<uint32> canonicalToBone(static_cast<size_t>(HumanBone::Count), ~0u);
    for (auto& m : boneMap)
    {
        if (m.Canonical == HumanBone::None) continue;
        const uint32 idx = m.CachedSourceIndex;
        if (idx == ~0u || idx >= skel.BoneCount) continue;
        boneToMapping[idx] = &m;
        canonicalToBone[static_cast<size_t>(m.Canonical)] = idx;
        // Default Q to identity; canonical bones with a derivable direction
        // overwrite below.
        m.RetargetPoseRotation = Mathematics::Quaternion::Identity();
    }

    // Cache authored world rotations and pre-cache authored world positions
    // — we need the authored world for the bind frame regardless of Q.
    std::vector<glm::quat> authoredWorld(skel.BoneCount, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    std::vector<glm::quat> canonicalWorld(skel.BoneCount, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

    // Topological walk. FBX-imported skeletons have parent index < child
    // index, so 0..N-1 is parent-before-child. Synthetic test fixtures
    // honour the same convention. SkeletonData::TopologicalOrder is the
    // authoritative source if populated.
    const bool useTopo = !skel.TopologicalOrder.empty()
                       && skel.TopologicalOrder.size() == skel.BoneCount;

    for (uint32 i = 0; i < skel.BoneCount; ++i)
    {
        const uint32 b = useTopo ? skel.TopologicalOrder[i] : i;

        const glm::quat localBind = ReadBoneRestRotation(skel, b);
        const int32 parentIdx = (b < skel.Parent.size()) ? skel.Parent[b] : -1;
        const glm::quat parentAuthored = (parentIdx >= 0)
            ? authoredWorld[static_cast<size_t>(parentIdx)]
            : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::quat parentCanonical = (parentIdx >= 0)
            ? canonicalWorld[static_cast<size_t>(parentIdx)]
            : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        authoredWorld[b] = glm::normalize(parentAuthored * localBind);

        HumanoidBoneMapping* mapping = boneToMapping[b];
        if (!mapping || mapping->Canonical == HumanBone::Hips)
        {
            // Non-canonical or Hips: identity Q, canonical local = authored local.
            canonicalWorld[b] = glm::normalize(parentCanonical * localBind);
            continue;
        }

        const ProfileBone* profileBone = profile.FindBone(mapping->Canonical);
        const HumanBone childCanonical = FindCanonicalChild(mapping->Canonical);
        const uint32 childIdx = (childCanonical != HumanBone::None)
            ? canonicalToBone[static_cast<size_t>(childCanonical)]
            : ~0u;

        if (!profileBone || childIdx == ~0u)
        {
            // Terminal mapped bone, or profile lookup missing. Identity Q.
            canonicalWorld[b] = glm::normalize(parentCanonical * localBind);
            continue;
        }

        // Authored segment direction in world.
        const glm::vec4 ownPos   = ComputeWorldRestPosition(skel, b);
        const glm::vec4 childPos = ComputeWorldRestPosition(skel, childIdx);
        const glm::vec3 dir      = glm::vec3(childPos - ownPos);
        const float dirLen       = glm::length(dir);
        if (dirLen < 1e-4f)
        {
            canonicalWorld[b] = glm::normalize(parentCanonical * localBind);
            continue;
        }
        const glm::vec3 authoredDirection = dir / dirLen;

        // Canonical direction in world (profile rest is identity).
        const glm::vec3 canonicalDirection(
            profileBone->LocalForward.x,
            profileBone->LocalForward.y,
            profileBone->LocalForward.z);

        const glm::quat qWorld = QuatFromTo(authoredDirection, canonicalDirection);

        // Chain-aware Q: parent CANONICAL on the left, parent AUTHORED on the
        // right. This is NOT a similarity transform — the asymmetry is what
        // makes the canonical chain compose correctly.
        const glm::quat qLocal = glm::inverse(parentCanonical) * qWorld * parentAuthored;
        const glm::quat qLocalNorm = glm::normalize(qLocal);

        mapping->RetargetPoseRotation = Mathematics::Quaternion(qLocalNorm);

        // Canonical world for downstream children: canonical_local = Q * bind.
        canonicalWorld[b] = glm::normalize(parentCanonical * qLocalNorm * localBind);
    }
}

bool AutoImportHumanoidRig(const SkeletonData& skel,
                           const SkeletonProfile& profile,
                           HumanoidRig& outRig)
{
    HumanoidNameMatcher matcher;
    const auto matchResult = matcher.MatchSkeleton(skel.BoneNames);

    if (matchResult.Coverage < kAutoImportCoverageThreshold)
    {
        Logger::Log::Warning("AutoImportHumanoidRig: coverage {:.0f}% below threshold {:.0f}%",
                             matchResult.Coverage * 100.0f,
                             kAutoImportCoverageThreshold * 100.0f);
        return false;
    }

    if (LooksLikeFourLeggedRest(skel, matchResult))
    {
        Logger::Log::Warning(
            "AutoImportHumanoidRig: four-legged rest pose (mapped hands at foot height)");
        return false;
    }

    // 1. Bone map
    auto& boneMap = outRig.BoneMapMutable();
    boneMap.clear();
    boneMap.reserve(matchResult.Mappings.size());
    for (const auto& m : matchResult.Mappings)
    {
        HumanoidBoneMapping out;
        out.Canonical = m.Canonical;
        out.SourceBoneName = m.SourceBoneName;
        out.CachedSourceIndex = m.CachedSourceIndex;
        out.RetargetPoseRotation = Mathematics::Quaternion::Identity();
        boneMap.push_back(out);
    }

    // 2. Body proportions from world rest positions.
    auto& prop = outRig.ProportionsMutable();
    prop = BodyProportions{};

    auto worldOf = [&](HumanBone b) -> std::pair<bool, glm::vec3>
    {
        const uint32 idx = SourceBoneFor(matchResult, b);
        if (idx == ~0u) return {false, glm::vec3(0.0f)};
        const glm::vec4 p = ComputeWorldRestPosition(skel, idx);
        return {true, glm::vec3(p)};
    };

    const auto hipsPos      = worldOf(HumanBone::Hips);
    const auto leftFootPos  = worldOf(HumanBone::LeftFoot);
    const auto rightFootPos = worldOf(HumanBone::RightFoot);
    const auto leftShPos    = worldOf(HumanBone::LeftShoulder);
    const auto rightShPos   = worldOf(HumanBone::RightShoulder);
    const auto leftHandPos  = worldOf(HumanBone::LeftHand);
    const auto leftUpLegPos = worldOf(HumanBone::LeftUpperLeg);

    if (hipsPos.first)
        prop.HipHeight = std::abs(hipsPos.second.y);

    if (leftShPos.first && rightShPos.first)
        prop.ShoulderWidth = glm::length(leftShPos.second - rightShPos.second);

    if (leftUpLegPos.first && leftFootPos.first)
        prop.LegLength = glm::length(leftUpLegPos.second - leftFootPos.second);
    else if (hipsPos.first && leftFootPos.first)
        prop.LegLength = glm::length(hipsPos.second - leftFootPos.second);

    if (leftShPos.first && leftHandPos.first)
        prop.ArmLength = glm::length(leftShPos.second - leftHandPos.second);

    // Per-chain length: sum world segment lengths along each chain. Indexed
    // by ChainKind enum.
    auto chainLength = [&](ChainKind kind) -> float
    {
        const auto& bones = CanonicalChainBones(kind);
        float total = 0.0f;
        std::pair<bool, glm::vec3> prev{false, {}};
        for (HumanBone b : bones)
        {
            auto cur = worldOf(b);
            if (prev.first && cur.first)
                total += glm::length(cur.second - prev.second);
            prev = cur;
        }
        return total;
    };

    for (size_t i = 0; i < prop.PerChainLengths.size(); ++i)
        prop.PerChainLengths[i] = chainLength(static_cast<ChainKind>(i));

    // 3. Chain definitions. Only emit a chain when its Start AND End bones
    // are mapped; partial chains would confuse downstream IK setup.
    auto& chains = outRig.ChainsMutable();
    chains.clear();

    auto emitChain = [&](ChainKind kind)
    {
        const auto& bones = CanonicalChainBones(kind);
        if (bones.size() < 2) return;

        // Find first + last MAPPED bone in the canonical chain. Optional
        // anatomy (e.g., toes) is allowed to be absent; we emit the chain
        // as long as we have at least 2 mapped bones in order.
        std::vector<HumanBone> mapped;
        mapped.reserve(bones.size());
        for (HumanBone b : bones)
            if (matchResult.Find(b)) mapped.push_back(b);
        if (mapped.size() < 2) return;

        HumanoidChain c;
        c.Kind = kind;
        c.Start = mapped.front();
        c.End = mapped.back();
        c.IncludeBones = std::move(mapped);
        chains.push_back(std::move(c));
    };

    emitChain(ChainKind::Spine);
    emitChain(ChainKind::LeftArm);
    emitChain(ChainKind::RightArm);
    emitChain(ChainKind::LeftLeg);
    emitChain(ChainKind::RightLeg);
    emitChain(ChainKind::Head);

    // 4. Translation bones default to {Hips}; nothing custom for v1.
    auto& tb = outRig.TranslationBonesMutable();
    tb.clear();
    if (matchResult.Find(HumanBone::Hips))
        tb.push_back(HumanBone::Hips);

    // 5. Detect A-pose + bake retarget pose.
    BakeRetargetPoseFromAPose(skel, profile, outRig);

    // 6. Profile reference (caller still has the option to override).
    outRig.SetProfileRef(profile.GetGUID());

    return true;
}

void AutoCreateRetargetMap(const HumanoidRig& src,
                           const HumanoidRig& tgt,
                           RetargetMap& outMap)
{
    outMap.SetSourceRigRef(src.GetGUID());
    outMap.SetTargetRigRef(tgt.GetGUID());

    auto& chainMap = outMap.ChainMapMutable();
    chainMap.clear();

    auto findChain = [](const HumanoidRig& rig, ChainKind kind) -> const HumanoidChain*
    {
        for (const auto& c : rig.Chains())
            if (c.Kind == kind) return &c;
        return nullptr;
    };

    constexpr ChainKind kKinds[] = {
        ChainKind::Spine,
        ChainKind::LeftArm,
        ChainKind::RightArm,
        ChainKind::LeftLeg,
        ChainKind::RightLeg,
        ChainKind::Head,
    };
    for (ChainKind k : kKinds)
    {
        if (!findChain(src, k) || !findChain(tgt, k)) continue;
        ChainPairing p;
        p.Kind = k;
        p.FK.RotationMode = FKRotationMode::OneToOne;
        p.FK.RotationAlpha = 1.0f;
        p.FK.TranslationMode = FKTranslationMode::None;
        p.IK.Enabled = false;
        p.IK.BlendToSource = 1.0f;
        p.IK.Extension = 0.0f;
        p.FootLock.Enabled = false;
        p.FootLock.SpeedThreshold = 0.05f;
        p.FootLock.LockBlend = 1.0f;
        chainMap.push_back(p);
    }

    // Op stack is empty by default; users (or Phase 5+ defaults) extend it.
    outMap.OpStackMutable().clear();
}

bool AutoImportHumanoidRigDispatch(const cgltf_data* gltf,
                                   const SkeletonData& skel,
                                   const SkeletonProfile& profile,
                                   HumanoidRig& outRig,
                                   HumanoidImportPath& outPath)
{
    outPath = HumanoidImportPath::None;

    // Stage 1: VRM 1.0 / VRM 0.x. Most specific — when present, the
    // bone-list is authoritative and a heuristic regex would only add
    // noise.
    if (gltf && HasVRMExtension(gltf))
    {
        VRM1Metadata meta; // captured-but-currently-unused; future face/lookAt consumers read it.
        if (ImportFromVRM1(gltf, skel, profile, outRig, meta))
        {
            outPath = HumanoidImportPath::VRM1;
            Logger::Log::Info("HumanoidRig auto-imported via VRM 1.0 (specVersion={}, legacyVRM0={})",
                               meta.SpecVersion, meta.IsLegacyVRM0 ? 1 : 0);
            return true;
        }
        Logger::Log::Warning("HumanoidRig: VRM extension present but import failed; falling back");
    }

    // Stage 2: KHR_humanoid (Khronos draft).
    if (gltf && HasKHRHumanoidExtension(gltf))
    {
        if (ImportFromKHRHumanoid(gltf, skel, profile, outRig))
        {
            outPath = HumanoidImportPath::KHRHumanoid;
            Logger::Log::Info("HumanoidRig auto-imported via KHR_humanoid");
            return true;
        }
        Logger::Log::Warning("HumanoidRig: KHR_humanoid extension present but import failed; falling back");
    }

    // Stage 3: Phase 2a heuristic name matcher.
    if (AutoImportHumanoidRig(skel, profile, outRig))
    {
        outPath = HumanoidImportPath::Heuristic;
        Logger::Log::Info("HumanoidRig auto-imported via heuristic name matcher");
        return true;
    }

    Logger::Log::Warning("HumanoidRig: all three import paths failed");
    return false;
}

} // namespace Animation
} // namespace GameEngine
