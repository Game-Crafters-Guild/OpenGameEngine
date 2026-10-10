#include "Animation/Ops/LookAtOp.h"

#include "Animation/AnimationPose.h"
#include "Animation/HumanoidRig.h"
#include "Animation/PoseUtilities.h"
#include "Animation/QuaternionMath.h"
#include "Animation/SkeletonData.h"

#include <glm/gtc/quaternion.hpp>

namespace GameEngine
{
namespace Animation
{

namespace
{

constexpr float kLookAtEpsilon = 1.0e-6f;

// Aim quaternion mapping `from` direction to `to` direction (both unit). For
// near-anti-parallel pairs (dot ~ -1), picks an axis from a fixed table to
// avoid singularity. Mirrors the GPU shader logic so CPU and GPU agree.
Mathematics::Quaternion AimQuat(const Mathematics::Vector3& from,
                                const Mathematics::Vector3& to)
{
    using Mathematics::Vector3;
    const float d = Vector3::Dot(from, to);
    if (d > 1.0f - kLookAtEpsilon) {
        return Mathematics::Quaternion::Identity();
    }
    if (d < -1.0f + kLookAtEpsilon) {
        // Pick a perpendicular axis (the world-up cross is degenerate for
        // pole-on alignment; fall through to world-X then world-Y).
        Vector3 axis = Vector3::Cross(Vector3(0, 1, 0), from);
        if (axis.Length() < kLookAtEpsilon)
            axis = Vector3::Cross(Vector3(1, 0, 0), from);
        axis = axis.Normalize();
        return Mathematics::Quaternion::FromAxisAngle(axis, 3.14159265358979f);
    }
    Vector3 axis = Vector3::Cross(from, to).Normalize();
    const float angle = std::acos(d);
    return Mathematics::Quaternion::FromAxisAngle(axis, angle);
}

// Find a target-skeleton bone matching the canonical slot. HumanoidRig only
// stores SOURCE-side mappings; for the target we fall back to bone-name
// equality with the canonical slot's printable name. Sufficient for the
// shipping test fixtures and for Mixamo / Synty rigs whose bone names map
// 1:1 with HumanBone canonical names. A proper target-side canonical
// resolver is future work.
int FindTgtBoneByCanonical(const HumanoidRig& tgtRig,
                           const SkeletonData& tgtSkel,
                           HumanBone canonical)
{
    if (canonical == HumanBone::None) return -1;
    for (const auto& m : tgtRig.BoneMap())
    {
        if (m.Canonical != canonical) continue;
        for (uint32 b = 0; b < tgtSkel.BoneCount; ++b) {
            if (b < tgtSkel.BoneNames.size() && tgtSkel.BoneNames[b] == m.SourceBoneName)
                return static_cast<int>(b);
        }
    }
    return -1;
}

// Walk parent chain and accumulate world rotation + position from outPose's
// local TRS. Returns identity world rotation + zero position when boneIdx is
// invalid.
void ComputeWorldTransform(const AnimationPose& pose,
                           const SkeletonData& skel,
                           int boneIdx,
                           Mathematics::Quaternion& outWorldRot,
                           Mathematics::Vector3& outWorldPos)
{
    using Mathematics::Quaternion;
    using Mathematics::Vector3;

    if (boneIdx < 0 || static_cast<uint32_t>(boneIdx) >= pose.BoneCount) {
        outWorldRot = Quaternion::Identity();
        outWorldPos = Vector3(0, 0, 0);
        return;
    }
    // Build chain from root to bone.
    int chain[64];
    int len = 0;
    int b = boneIdx;
    while (b >= 0 && len < 64) {
        chain[len++] = b;
        b = (static_cast<uint32_t>(b) < skel.Parent.size()) ? skel.Parent[b] : -1;
    }
    // Apply parent-first.
    Quaternion accumRot = Quaternion::Identity();
    Vector3 accumPos(0, 0, 0);
    for (int i = len - 1; i >= 0; --i) {
        const int idx = chain[i];
        const Quaternion& localR = pose.Rotations[idx];
        const Vector3& localT    = pose.Positions[idx];
        accumPos = accumPos + accumRot.Rotate(localT);
        accumRot = (accumRot * localR).Normalized();
    }
    outWorldRot = accumRot;
    outWorldPos = accumPos;
}

} // namespace

LookAtOp::LookAtOp(const nlohmann::json& params)
{
    using Mathematics::Vector3;
    if (params.is_object()) {
        if (params.contains("target") && params["target"].is_array() && params["target"].size() == 3) {
            try {
                m_Target = Vector3(params["target"][0].get<float>(),
                                   params["target"][1].get<float>(),
                                   params["target"][2].get<float>());
                m_HasTarget = true;
            } catch (...) { m_HasTarget = false; }
        } else if (params.contains("targetX") && params.contains("targetY") && params.contains("targetZ")) {
            try {
                m_Target = Vector3(params["targetX"].get<float>(),
                                   params["targetY"].get<float>(),
                                   params["targetZ"].get<float>());
                m_HasTarget = true;
            } catch (...) { m_HasTarget = false; }
        }
    }
}

void LookAtOp::Execute(const OpExecuteContext& ctx, AnimationPose& outPose)
{
    using Mathematics::Quaternion;
    using Mathematics::Vector3;

    if (!m_HasTarget) return;
    if (!ctx.TargetRig || !ctx.Eval || !ctx.Eval->TargetSkeleton) return;

    const auto& tgtSkel = *ctx.Eval->TargetSkeleton;

    auto applyAim = [&](HumanBone canonical) {
        const int boneIdx = FindTgtBoneByCanonical(*ctx.TargetRig, tgtSkel, canonical);
        if (boneIdx < 0 || static_cast<uint32_t>(boneIdx) >= outPose.BoneCount) return;

        Quaternion worldRot;
        Vector3    worldPos;
        ComputeWorldTransform(outPose, tgtSkel, boneIdx, worldRot, worldPos);

        Vector3 toTarget = m_Target - worldPos;
        const float dist = toTarget.Length();
        if (dist < kLookAtEpsilon) return;
        toTarget = toTarget * (1.0f / dist);

        // Convention: head's local "forward" is +Z in target rest pose. The
        // current world forward is worldRot.Rotate(+Z).
        Vector3 currentFwd = worldRot.Rotate(Vector3(0, 0, 1));
        const float fwdLen = currentFwd.Length();
        if (fwdLen < kLookAtEpsilon) return;
        currentFwd = currentFwd * (1.0f / fwdLen);

        Quaternion deltaWorld = AimQuat(currentFwd, toTarget);

        // newWorldRot = deltaWorld * worldRot. Convert to local relative to
        // the current parent's world rotation. Parent = worldRot * inv(localRot).
        Quaternion newWorldRot = (deltaWorld * worldRot).Normalized();
        Quaternion localRot = outPose.Rotations[boneIdx];
        Quaternion parentWorldRot = (worldRot * localRot.Conjugated()).Normalized();
        Quaternion newLocal = (parentWorldRot.Conjugated() * newWorldRot).Normalized();

        outPose.Rotations[boneIdx] = newLocal;
    };

    applyAim(HumanBone::Head);
    applyAim(HumanBone::LeftEye);
    applyAim(HumanBone::RightEye);
}

std::unique_ptr<OpStackNode> LookAtOp::Create(const nlohmann::json& params)
{
    return std::make_unique<LookAtOp>(params);
}

} // namespace Animation
} // namespace GameEngine
