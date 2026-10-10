#include "Animation/Ops/FootLockOp.h"

#include "Animation/AnimationPose.h"
#include "Animation/HumanoidRig.h"
#include "Animation/QuaternionMath.h"
#include "Animation/SkeletonData.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace GameEngine
{
namespace Animation
{

namespace
{

constexpr float kFootLockEpsilon = 1.0e-6f;
constexpr float kFootLockMinDt = 1.0f / 240.0f; // safety floor — avoids div-by-zero when DeltaTime=0

// World-space TRS at boneIdx in `pose`, computed via parent walk.
struct BoneWorld
{
    Mathematics::Quaternion Rot;
    Mathematics::Vector3    Pos;
};

BoneWorld ComputeWorld(const AnimationPose& pose,
                       const SkeletonData& skel,
                       int boneIdx)
{
    using Mathematics::Quaternion;
    using Mathematics::Vector3;

    BoneWorld out;
    if (boneIdx < 0 || static_cast<uint32_t>(boneIdx) >= pose.BoneCount) {
        out.Rot = Quaternion::Identity();
        out.Pos = Vector3(0, 0, 0);
        return out;
    }
    int chain[64];
    int len = 0;
    int b = boneIdx;
    while (b >= 0 && len < 64) {
        chain[len++] = b;
        b = (static_cast<uint32_t>(b) < skel.Parent.size()) ? skel.Parent[b] : -1;
    }
    Quaternion accumRot = Quaternion::Identity();
    Vector3    accumPos(0, 0, 0);
    for (int i = len - 1; i >= 0; --i) {
        const int idx = chain[i];
        const Quaternion& r = pose.Rotations[idx];
        const Vector3&    t = pose.Positions[idx];
        accumPos = accumPos + accumRot.Rotate(t);
        accumRot = (accumRot * r).Normalized();
    }
    out.Rot = accumRot;
    out.Pos = accumPos;
    return out;
}

int FindBoneByCanonical(const HumanoidRig& rig,
                        const SkeletonData& skel,
                        HumanBone canonical)
{
    if (canonical == HumanBone::None) return -1;
    for (const auto& m : rig.BoneMap()) {
        if (m.Canonical != canonical) continue;
        if (m.CachedSourceIndex != ~0u && m.CachedSourceIndex < skel.BoneCount)
            return static_cast<int>(m.CachedSourceIndex);
        for (uint32 b = 0; b < skel.BoneCount; ++b) {
            if (b < skel.BoneNames.size() && skel.BoneNames[b] == m.SourceBoneName)
                return static_cast<int>(b);
        }
    }
    return -1;
}

// Closed-form 2-bone IK matching TwoBoneIKNode: solves for new mid + tip
// positions then converts back to local rotations. Pelvis (UpperLeg's parent)
// is NOT moved per plan §3.6 FootLockOp spec.
void SolveLegIK(AnimationPose& pose,
                const SkeletonData& skel,
                int upperLegIdx,
                int lowerLegIdx,
                int footIdx,
                const Mathematics::Vector3& worldFootGoal)
{
    using Mathematics::Quaternion;
    using Mathematics::Vector3;
    using Animation::SlerpShort;

    if (upperLegIdx < 0 || lowerLegIdx < 0 || footIdx < 0) return;

    BoneWorld root = ComputeWorld(pose, skel, upperLegIdx);
    BoneWorld mid  = ComputeWorld(pose, skel, lowerLegIdx);
    BoneWorld tip  = ComputeWorld(pose, skel, footIdx);

    Vector3 chainAxis = worldFootGoal - root.Pos;
    const float distToGoal = chainAxis.Length();
    if (distToGoal < kFootLockEpsilon) return;
    chainAxis = chainAxis * (1.0f / distToGoal);

    Vector3 upperVec = mid.Pos - root.Pos;
    Vector3 lowerVec = tip.Pos - mid.Pos;
    const float upperLen = std::max(upperVec.Length(), kFootLockEpsilon);
    const float lowerLen = std::max(lowerVec.Length(), kFootLockEpsilon);

    // Clamp goal distance into reachable range. Up to 5% leg stretch per plan
    // §3 Stage 5; FootLockOp follows the same clamp.
    const float maxReach = (upperLen + lowerLen) * 1.05f;
    const float minReach = std::abs(upperLen - lowerLen);
    const float clampedDist = std::clamp(distToGoal, minReach, maxReach);

    // Law of cosines: rootAngle is the angle between (root->goal) and (root->mid).
    const float cosRootAngle = std::clamp(
        (upperLen * upperLen + clampedDist * clampedDist - lowerLen * lowerLen) /
            (2.0f * upperLen * clampedDist),
        -1.0f, 1.0f);
    const float rootAngle = std::acos(cosRootAngle);

    // Bend direction: project current mid onto plane perpendicular to chainAxis.
    Vector3 midOffset = mid.Pos - root.Pos;
    Vector3 bendDir = midOffset - chainAxis * Vector3::Dot(midOffset, chainAxis);
    if (bendDir.Length() < 1e-3f) {
        // Degenerate: fall back to world-up cross.
        bendDir = Vector3::Cross(chainAxis, Vector3(0, 1, 0));
        if (bendDir.Length() < kFootLockEpsilon) bendDir = Vector3::Cross(chainAxis, Vector3(1, 0, 0));
    }
    bendDir = bendDir.Normalize();

    Vector3 newMid = root.Pos + chainAxis * (upperLen * std::cos(rootAngle))
                              + bendDir   * (upperLen * std::sin(rootAngle));
    Vector3 newTipDir = (worldFootGoal - newMid).Normalize();
    Vector3 newTip = newMid + newTipDir * lowerLen;

    // Convert world-position deltas into local rotations.
    auto deltaQuat = [&](const Vector3& from, const Vector3& to) {
        const float d = Vector3::Dot(from, to);
        if (d > 1.0f - kFootLockEpsilon) return Quaternion::Identity();
        if (d < -1.0f + kFootLockEpsilon) {
            Vector3 ax = Vector3::Cross(Vector3(0, 1, 0), from);
            if (ax.Length() < kFootLockEpsilon) ax = Vector3::Cross(Vector3(1, 0, 0), from);
            return Quaternion::FromAxisAngle(ax.Normalize(), 3.14159265358979f);
        }
        Vector3 ax = Vector3::Cross(from, to).Normalize();
        return Quaternion::FromAxisAngle(ax, std::acos(d));
    };

    Vector3 oldRootDir = upperVec.Normalize();
    Vector3 newRootDir = (newMid - root.Pos).Normalize();
    Quaternion rootDelta = deltaQuat(oldRootDir, newRootDir);
    Quaternion newRootWorldRot = (rootDelta * root.Rot).Normalized();

    // Recover parent world rotation from the upper leg's stored local rot,
    // then derive the new local rot. Use the SlerpShort antipode helper to
    // ensure the conversion stays on the short hemisphere.
    Quaternion parentWorld = (root.Rot * pose.Rotations[upperLegIdx].Conjugated()).Normalized();
    Quaternion newRootLocal = (parentWorld.Conjugated() * newRootWorldRot).Normalized();
    pose.Rotations[upperLegIdx] = SlerpShort(pose.Rotations[upperLegIdx], newRootLocal, 1.0f);

    // Recompute mid world after upper update.
    BoneWorld midAfter = ComputeWorld(pose, skel, lowerLegIdx);
    Vector3 oldMidDir = (tip.Pos - mid.Pos).Normalize();
    Vector3 newMidDir = (newTip - newMid).Normalize();
    // Bring oldMidDir into the new mid's frame: rotate by rootDelta.
    Vector3 oldMidDirInNewFrame = rootDelta.Rotate(oldMidDir);
    Quaternion midDelta = deltaQuat(oldMidDirInNewFrame, newMidDir);
    Quaternion newMidWorldRot = (midDelta * midAfter.Rot).Normalized();
    Quaternion midParentWorld = (midAfter.Rot * pose.Rotations[lowerLegIdx].Conjugated()).Normalized();
    Quaternion newMidLocal = (midParentWorld.Conjugated() * newMidWorldRot).Normalized();
    pose.Rotations[lowerLegIdx] = SlerpShort(pose.Rotations[lowerLegIdx], newMidLocal, 1.0f);
}

} // namespace

FootLockOp::FootLockOp(const nlohmann::json& params)
{
    if (params.is_object()) {
        if (params.contains("lockThreshold") && params["lockThreshold"].is_number()) {
            try { m_LockThreshold = params["lockThreshold"].get<float>(); } catch (...) {}
        }
        if (params.contains("blend") && params["blend"].is_number()) {
            try { m_Blend = params["blend"].get<float>(); } catch (...) {}
        }
    }
    m_LockThreshold = std::max(0.0f, m_LockThreshold);
    m_Blend = std::clamp(m_Blend, 0.0f, 1.0f);
}

void FootLockOp::Reset()
{
    m_PrevSrcValid    = { false, false };
    m_PrevAnchorValid = { false, false };
}

void FootLockOp::Execute(const OpExecuteContext& ctx, AnimationPose& outPose)
{
    using Mathematics::Vector3;

    if (!ctx.TargetRig || !ctx.SourceRig || !ctx.SourcePose || !ctx.Eval) return;
    if (!ctx.Eval->TargetSkeleton || !ctx.Eval->SourceSkeleton) return;

    const auto& tgtSkel = *ctx.Eval->TargetSkeleton;
    const auto& srcSkel = *ctx.Eval->SourceSkeleton;

    const float dt = std::max(ctx.Eval->DeltaTime, kFootLockMinDt);
    const float thresholdSq = m_LockThreshold * m_LockThreshold;

    struct FootChain {
        HumanBone foot;
        HumanBone lower;
        HumanBone upper;
    };
    const FootChain feet[2] = {
        { HumanBone::LeftFoot,  HumanBone::LeftLowerLeg,  HumanBone::LeftUpperLeg  },
        { HumanBone::RightFoot, HumanBone::RightLowerLeg, HumanBone::RightUpperLeg },
    };

    for (int fi = 0; fi < 2; ++fi)
    {
        const FootChain& fc = feet[fi];
        const int srcFootIdx = FindBoneByCanonical(*ctx.SourceRig, srcSkel, fc.foot);
        const int tgtFootIdx = FindBoneByCanonical(*ctx.TargetRig, tgtSkel, fc.foot);
        const int tgtLowerIdx = FindBoneByCanonical(*ctx.TargetRig, tgtSkel, fc.lower);
        const int tgtUpperIdx = FindBoneByCanonical(*ctx.TargetRig, tgtSkel, fc.upper);
        if (srcFootIdx < 0 || tgtFootIdx < 0 || tgtLowerIdx < 0 || tgtUpperIdx < 0)
            continue;

        // Source foot world position from the source pose.
        BoneWorld srcWorld = ComputeWorld(*ctx.SourcePose, srcSkel, srcFootIdx);
        BoneWorld tgtWorld = ComputeWorld(outPose, tgtSkel, tgtFootIdx);

        // Planar speed: XZ-only difference / dt (Y isolated so jumps don't
        // unlock the foot — the lock cares about ground sliding).
        bool lock = false;
        if (m_PrevSrcValid[fi]) {
            const float dx = srcWorld.Pos.x - m_PrevSrcFootPos[fi].x;
            const float dz = srcWorld.Pos.z - m_PrevSrcFootPos[fi].z;
            const float speedSq = (dx * dx + dz * dz) / (dt * dt);
            lock = (speedSq < thresholdSq) && m_PrevAnchorValid[fi];
        }

        if (lock && m_Blend > 0.0f) {
            // Blend the goal between current target foot world pos (no lock)
            // and the prev anchor (full lock) — m_Blend == 1 is full pin.
            Vector3 goal = tgtWorld.Pos * (1.0f - m_Blend) + m_PrevAnchor[fi] * m_Blend;
            SolveLegIK(outPose, tgtSkel, tgtUpperIdx, tgtLowerIdx, tgtFootIdx, goal);
            // After the IK adjust, store the new target foot world position
            // as the next frame's anchor (deterministic anchor, even at
            // partial blend).
            BoneWorld lockedWorld = ComputeWorld(outPose, tgtSkel, tgtFootIdx);
            m_PrevAnchor[fi]      = lockedWorld.Pos;
            m_PrevAnchorValid[fi] = true;
        } else {
            m_PrevAnchor[fi]      = tgtWorld.Pos;
            m_PrevAnchorValid[fi] = true;
        }

        m_PrevSrcFootPos[fi] = srcWorld.Pos;
        m_PrevSrcValid[fi]   = true;
    }
}

std::unique_ptr<OpStackNode> FootLockOp::Create(const nlohmann::json& params)
{
    return std::make_unique<FootLockOp>(params);
}

} // namespace Animation
} // namespace GameEngine
