#include "Animation/Nodes/TwoBoneIKNode.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/PoseUtilities.h"
#include "Mathematics/VectorOps.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Animation
{

namespace
{
    constexpr float kChainLengthEpsilon = 0.0001f;
    constexpr float kMinLengthSq = 1e-10f;

    // Compute rotation that takes vector 'from' to vector 'to'.
    // Both must be normalized.
    Mathematics::Quaternion RotationBetweenVectors(
        const Mathematics::Vector3& from,
        const Mathematics::Vector3& to)
    {
        const float dot = std::clamp(Mathematics::Vector3::Dot(from, to), -1.0f, 1.0f);

        // Nearly identical directions
        if (dot > 0.9999f)
            return Mathematics::Quaternion::Identity();

        // Nearly opposite directions
        if (dot < -0.9999f)
        {
            // Find a perpendicular axis
            Mathematics::Vector3 perp = Mathematics::Vector3::Cross(
                Mathematics::Vector3(1.0f, 0.0f, 0.0f), from);
            if (perp.Length() < 0.001f)
                perp = Mathematics::Vector3::Cross(Mathematics::Vector3(0.0f, 1.0f, 0.0f), from);
            perp = perp.Normalize();
            return Mathematics::Quaternion::FromAxisAngle(perp, Mathematics::Pi);
        }

        Mathematics::Vector3 axis = Mathematics::Vector3::Cross(from, to).Normalize();
        float angle = std::acos(dot);
        return Mathematics::Quaternion::FromAxisAngle(axis, angle);
    }

} // namespace

void TwoBoneIKNode::SetSource(std::unique_ptr<AnimGraphNode> node) { m_Source = std::move(node); }
const AnimGraphNode* TwoBoneIKNode::GetSource() const { return m_Source.get(); }
uint32_t TwoBoneIKNode::GetRootBone() const { return m_RootBone; }
uint32_t TwoBoneIKNode::GetMidBone() const { return m_MidBone; }
uint32_t TwoBoneIKNode::GetTipBone() const { return m_TipBone; }
float TwoBoneIKNode::GetWeight() const { return m_Weight; }

void TwoBoneIKNode::SetBoneIndices(uint32_t rootBone, uint32_t midBone, uint32_t tipBone)
{
    m_RootBone = rootBone;
    m_MidBone = midBone;
    m_TipBone = tipBone;
}

void TwoBoneIKNode::SetTarget(const Mathematics::Vector3& target) { m_Target = target; }
void TwoBoneIKNode::SetPoleTarget(const Mathematics::Vector3& poleTarget) { m_PoleTarget = poleTarget; }
void TwoBoneIKNode::SetWeight(float weight) { m_Weight = weight; }
void TwoBoneIKNode::SetParentIndices(const std::vector<int32_t>& parentIndices) { m_ParentIndices = parentIndices; }

void TwoBoneIKNode::SolveTwoBoneIK(
    AnimationPose& outPose,
    const std::vector<int32_t>& parentIndices,
    uint32_t rootBone,
    uint32_t midBone,
    uint32_t tipBone,
    const Mathematics::Vector3& target,
    float weight)
{
    if (weight <= 0.0f)
        return;

    const uint32_t boneCount = outPose.BoneCount;
    if (rootBone >= boneCount || midBone >= boneCount || tipBone >= boneCount)
        return;
    if (rootBone >= parentIndices.size() || midBone >= parentIndices.size() || tipBone >= parentIndices.size())
        return;

    // Compute world-space positions. The static helper allocates locally;
    // member-scope callers (Evaluate below) reuse m_ScratchWorld.
    thread_local WorldSpacePose tlsWorld;
    PoseUtilities::ComputeWorldSpace(outPose, parentIndices, tlsWorld);
    WorldSpacePose& worldPose = tlsWorld;

    Mathematics::Vector3 rootPos = worldPose.Positions[rootBone];
    Mathematics::Vector3 midPos = worldPose.Positions[midBone];
    Mathematics::Vector3 tipPos = worldPose.Positions[tipBone];

    // Current chain lengths
    float upperLen = (midPos - rootPos).Length();
    float lowerLen = (tipPos - midPos).Length();

    if (upperLen < kChainLengthEpsilon || lowerLen < kChainLengthEpsilon)
        return;

    // Target distance, clamped to reachable range
    Mathematics::Vector3 rootToTarget = target - rootPos;
    float targetDist = rootToTarget.Length();

    float minReach = std::abs(upperLen - lowerLen) + kChainLengthEpsilon;
    float maxReach = upperLen + lowerLen - kChainLengthEpsilon;
    targetDist = std::clamp(targetDist, minReach, maxReach);

    // Direction from root to target
    Mathematics::Vector3 targetDir = rootToTarget.Length() > kChainLengthEpsilon
                                         ? rootToTarget.Normalize()
                                         : Mathematics::Vector3(0.0f, 0.0f, 1.0f);

    // Angle at root joint (law of cosines)
    float cosRootAngle = (upperLen * upperLen + targetDist * targetDist - lowerLen * lowerLen)
                         / (2.0f * upperLen * targetDist);
    cosRootAngle = std::clamp(cosRootAngle, -1.0f, 1.0f);
    float rootAngle = std::acos(cosRootAngle);

    // Pick a bend direction perpendicular to the chain axis. Without an
    // explicit pole target, prefer the existing mid bone's offset from the
    // straight root-to-target line so the bend direction is stable.
    Mathematics::Vector3 chainDir = targetDir;
    Mathematics::Vector3 rootToCurrentMid = midPos - rootPos;
    Mathematics::Vector3 midOffPlane = rootToCurrentMid -
        chainDir * Mathematics::Vector3::Dot(rootToCurrentMid, chainDir);

    Mathematics::Vector3 bendDir;
    if (Mathematics::Vector3::Dot(midOffPlane, midOffPlane) > kMinLengthSq)
    {
        bendDir = midOffPlane.Normalize();
    }
    else
    {
        Mathematics::Vector3 up(0.0f, 1.0f, 0.0f);
        if (std::abs(Mathematics::Vector3::Dot(chainDir, up)) > 0.999f)
            up = Mathematics::Vector3(1.0f, 0.0f, 0.0f);
        bendDir = Mathematics::Vector3::Cross(chainDir, up).Normalize();
    }

    // Compute new mid position
    Mathematics::Vector3 newMidPos = rootPos +
        chainDir * (upperLen * std::cos(rootAngle)) +
        bendDir * (upperLen * std::sin(rootAngle));

    // Compute new tip position
    Mathematics::Vector3 midToTarget = target - newMidPos;
    float midToTargetLen = midToTarget.Length();
    Mathematics::Vector3 newTipPos;
    if (midToTargetLen > kChainLengthEpsilon)
        newTipPos = newMidPos + midToTarget.Normalize() * lowerLen;
    else
        newTipPos = newMidPos + chainDir * lowerLen;

    // Root bone: rotation that takes the original root->mid direction to new root->mid direction
    Mathematics::Vector3 origRootToMid = (midPos - rootPos);
    Mathematics::Vector3 newRootToMid = (newMidPos - rootPos);
    Mathematics::Quaternion rootDelta = Mathematics::Quaternion::Identity();
    if (origRootToMid.Length() > kChainLengthEpsilon && newRootToMid.Length() > kChainLengthEpsilon)
    {
        origRootToMid = origRootToMid.Normalize();
        newRootToMid = newRootToMid.Normalize();
        rootDelta = RotationBetweenVectors(origRootToMid, newRootToMid);
        Mathematics::Quaternion newRootWorldRot = rootDelta * worldPose.Rotations[rootBone];

        int32_t rootParent = parentIndices[rootBone];
        Mathematics::Quaternion rootParentWorldRot = rootParent >= 0 && static_cast<uint32_t>(rootParent) < worldPose.Rotations.size()
            ? worldPose.Rotations[rootParent]
            : Mathematics::Quaternion::Identity();

        Mathematics::Quaternion newRootLocalRot =
            PoseUtilities::WorldToLocalRotation(newRootWorldRot, rootParentWorldRot);

        outPose.Rotations[rootBone] = Mathematics::Quaternion::Slerp(
            outPose.Rotations[rootBone], newRootLocalRot, weight);
    }

    // Mid bone: rotation that takes the original mid->tip direction to new mid->tip direction
    Mathematics::Vector3 origMidToTip = (tipPos - midPos);
    Mathematics::Vector3 newMidToTip = (newTipPos - newMidPos);
    if (origMidToTip.Length() > kChainLengthEpsilon && newMidToTip.Length() > kChainLengthEpsilon)
    {
        origMidToTip = origMidToTip.Normalize();
        newMidToTip = newMidToTip.Normalize();
        Mathematics::Quaternion midDelta = RotationBetweenVectors(origMidToTip, newMidToTip);
        Mathematics::Quaternion newMidWorldRot = midDelta * worldPose.Rotations[midBone];

        Mathematics::Quaternion updatedRootWorldRot = rootDelta * worldPose.Rotations[rootBone];

        int32_t midParent = parentIndices[midBone];
        Mathematics::Quaternion midParentWorldRot;
        if (midParent == static_cast<int32_t>(rootBone))
            midParentWorldRot = updatedRootWorldRot;
        else if (midParent >= 0 && static_cast<uint32_t>(midParent) < worldPose.Rotations.size())
            midParentWorldRot = worldPose.Rotations[midParent];
        else
            midParentWorldRot = Mathematics::Quaternion::Identity();

        Mathematics::Quaternion newMidLocalRot =
            PoseUtilities::WorldToLocalRotation(newMidWorldRot, midParentWorldRot);

        outPose.Rotations[midBone] = Mathematics::Quaternion::Slerp(
            outPose.Rotations[midBone], newMidLocalRot, weight);
    }
}

void TwoBoneIKNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (m_Source)
        m_Source->Evaluate(ctx, outPose);

    if (m_Weight <= 0.0f)
        return;

    // Bounds-check root bone parent before any access (round-2 audit).
    const uint32_t boneCount = outPose.BoneCount;
    if (m_RootBone >= boneCount || m_MidBone >= boneCount || m_TipBone >= boneCount)
        return;
    if (m_RootBone >= m_ParentIndices.size() || m_MidBone >= m_ParentIndices.size() ||
        m_TipBone >= m_ParentIndices.size())
        return;

    // Use pole-target-aware path here; FootIK and other composite nodes use
    // SolveTwoBoneIK which derives the bend axis from the current pose.
    PoseUtilities::ComputeWorldSpace(outPose, m_ParentIndices, m_ScratchWorld);
    WorldSpacePose& worldPose = m_ScratchWorld;

    Mathematics::Vector3 rootPos = worldPose.Positions[m_RootBone];
    Mathematics::Vector3 midPos = worldPose.Positions[m_MidBone];
    Mathematics::Vector3 tipPos = worldPose.Positions[m_TipBone];

    float upperLen = (midPos - rootPos).Length();
    float lowerLen = (tipPos - midPos).Length();

    if (upperLen < kChainLengthEpsilon || lowerLen < kChainLengthEpsilon)
        return;

    Mathematics::Vector3 rootToTarget = m_Target - rootPos;
    float targetDist = rootToTarget.Length();

    float minReach = std::abs(upperLen - lowerLen) + kChainLengthEpsilon;
    float maxReach = upperLen + lowerLen - kChainLengthEpsilon;
    targetDist = std::clamp(targetDist, minReach, maxReach);

    Mathematics::Vector3 targetDir = rootToTarget.Length() > kChainLengthEpsilon
                                         ? rootToTarget.Normalize()
                                         : Mathematics::Vector3(0.0f, 0.0f, 1.0f);

    float cosRootAngle = (upperLen * upperLen + targetDist * targetDist - lowerLen * lowerLen)
                         / (2.0f * upperLen * targetDist);
    cosRootAngle = std::clamp(cosRootAngle, -1.0f, 1.0f);
    float rootAngle = std::acos(cosRootAngle);

    Mathematics::Vector3 chainDir = targetDir;
    Mathematics::Vector3 rootToPole = m_PoleTarget - rootPos;

    Mathematics::Vector3 poleOnPlane = rootToPole -
        chainDir * Mathematics::Vector3::Dot(rootToPole, chainDir);

    float polePlaneLenSq = Mathematics::Vector3::Dot(poleOnPlane, poleOnPlane);
    Mathematics::Vector3 bendDir;
    if (polePlaneLenSq > kMinLengthSq)
    {
        bendDir = poleOnPlane.Normalize();
    }
    else
    {
        Mathematics::Vector3 up(0.0f, 1.0f, 0.0f);
        if (std::abs(Mathematics::Vector3::Dot(chainDir, up)) > 0.999f)
            up = Mathematics::Vector3(1.0f, 0.0f, 0.0f);
        bendDir = Mathematics::Vector3::Cross(chainDir, up).Normalize();
    }

    Mathematics::Vector3 newMidPos = rootPos +
        chainDir * (upperLen * std::cos(rootAngle)) +
        bendDir * (upperLen * std::sin(rootAngle));

    Mathematics::Vector3 midToTarget = m_Target - newMidPos;
    float midToTargetLen = midToTarget.Length();
    Mathematics::Vector3 newTipPos;
    if (midToTargetLen > kChainLengthEpsilon)
        newTipPos = newMidPos + midToTarget.Normalize() * lowerLen;
    else
        newTipPos = newMidPos + chainDir * lowerLen;

    Mathematics::Vector3 origRootToMid = (midPos - rootPos);
    Mathematics::Vector3 newRootToMid = (newMidPos - rootPos);
    Mathematics::Quaternion rootDelta = Mathematics::Quaternion::Identity();
    if (origRootToMid.Length() > kChainLengthEpsilon && newRootToMid.Length() > kChainLengthEpsilon)
    {
        origRootToMid = origRootToMid.Normalize();
        newRootToMid = newRootToMid.Normalize();
        rootDelta = RotationBetweenVectors(origRootToMid, newRootToMid);
        Mathematics::Quaternion newRootWorldRot = rootDelta * worldPose.Rotations[m_RootBone];

        int32_t rootParent = m_ParentIndices[m_RootBone];
        Mathematics::Quaternion rootParentWorldRot = rootParent >= 0 && static_cast<uint32_t>(rootParent) < worldPose.Rotations.size()
            ? worldPose.Rotations[rootParent]
            : Mathematics::Quaternion::Identity();

        Mathematics::Quaternion newRootLocalRot =
            PoseUtilities::WorldToLocalRotation(newRootWorldRot, rootParentWorldRot);

        outPose.Rotations[m_RootBone] = Mathematics::Quaternion::Slerp(
            outPose.Rotations[m_RootBone], newRootLocalRot, m_Weight);
    }

    Mathematics::Vector3 origMidToTip = (tipPos - midPos);
    Mathematics::Vector3 newMidToTip = (newTipPos - newMidPos);
    if (origMidToTip.Length() > kChainLengthEpsilon && newMidToTip.Length() > kChainLengthEpsilon)
    {
        origMidToTip = origMidToTip.Normalize();
        newMidToTip = newMidToTip.Normalize();
        Mathematics::Quaternion midDelta = RotationBetweenVectors(origMidToTip, newMidToTip);
        Mathematics::Quaternion newMidWorldRot = midDelta * worldPose.Rotations[m_MidBone];

        Mathematics::Quaternion updatedRootWorldRot = rootDelta * worldPose.Rotations[m_RootBone];

        int32_t midParent = m_ParentIndices[m_MidBone];
        Mathematics::Quaternion midParentWorldRot;
        if (midParent == static_cast<int32_t>(m_RootBone))
            midParentWorldRot = updatedRootWorldRot;
        else if (midParent >= 0 && static_cast<uint32_t>(midParent) < worldPose.Rotations.size())
            midParentWorldRot = worldPose.Rotations[midParent];
        else
            midParentWorldRot = Mathematics::Quaternion::Identity();

        Mathematics::Quaternion newMidLocalRot =
            PoseUtilities::WorldToLocalRotation(newMidWorldRot, midParentWorldRot);

        outPose.Rotations[m_MidBone] = Mathematics::Quaternion::Slerp(
            outPose.Rotations[m_MidBone], newMidLocalRot, m_Weight);
    }
}

} // namespace GameEngine::Animation
