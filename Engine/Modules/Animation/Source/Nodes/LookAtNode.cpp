#include "Animation/Nodes/LookAtNode.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/PoseUtilities.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Animation
{

namespace
{
    constexpr float kMinDirectionLength = 0.0001f;
} // namespace

void LookAtNode::SetSource(std::unique_ptr<AnimGraphNode> node) { m_Source = std::move(node); }
void LookAtNode::SetBoneIndex(uint32_t boneIndex) { m_BoneIndex = boneIndex; }
void LookAtNode::SetTarget(const Mathematics::Vector3& worldTarget) { m_Target = worldTarget; }
void LookAtNode::SetWeight(float weight) { m_Weight = weight; }
void LookAtNode::SetParentIndices(const std::vector<int32_t>& parentIndices) { m_ParentIndices = parentIndices; }
void LookAtNode::SetAimAxis(const Mathematics::Vector3& axis) { m_AimAxis = axis; }
void LookAtNode::SetMaxAngle(float maxAngleRadians) { m_MaxAngle = maxAngleRadians; }

const AnimGraphNode* LookAtNode::GetSource() const { return m_Source.get(); }
uint32_t LookAtNode::GetBoneIndex() const { return m_BoneIndex; }
float LookAtNode::GetWeight() const { return m_Weight; }
const Mathematics::Vector3& LookAtNode::GetAimAxis() const { return m_AimAxis; }
float LookAtNode::GetMaxAngle() const { return m_MaxAngle; }

void LookAtNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (m_Source)
        m_Source->Evaluate(ctx, outPose);

    if (m_Weight <= 0.0f)
        return;

    if (m_BoneIndex >= outPose.BoneCount)
        return;
    if (m_BoneIndex >= m_ParentIndices.size())
        return;

    PoseUtilities::ComputeWorldSpace(outPose, m_ParentIndices, m_ScratchWorld);
    WorldSpacePose& worldPose = m_ScratchWorld;

    Mathematics::Vector3 boneWorldPos = worldPose.Positions[m_BoneIndex];
    Mathematics::Quaternion boneWorldRot = worldPose.Rotations[m_BoneIndex];

    // Direction from bone to target in world space
    Mathematics::Vector3 toTarget = m_Target - boneWorldPos;
    if (toTarget.Length() < kMinDirectionLength)
        return;
    toTarget = toTarget.Normalize();

    // Current aim direction: rotate the aim axis by the bone's world rotation
    Mathematics::Vector3 currentAimDir = boneWorldRot.Rotate(m_AimAxis).Normalize();

    // Compute rotation from current aim to desired aim
    float dot = std::clamp(Mathematics::Vector3::Dot(currentAimDir, toTarget), -1.0f, 1.0f);
    float angle = std::acos(dot);

    if (angle < kMinDirectionLength)
        return;

    // Clamp the rotation angle
    float clampedAngle = std::min(angle, m_MaxAngle);

    // Compute rotation axis
    Mathematics::Vector3 rotAxis = Mathematics::Vector3::Cross(currentAimDir, toTarget);
    if (rotAxis.Length() < kMinDirectionLength)
    {
        // Aim directions are parallel (same or opposite)
        if (dot < 0.0f)
        {
            // Opposite: find a perpendicular axis
            Mathematics::Vector3 up(0.0f, 1.0f, 0.0f);
            if (std::abs(Mathematics::Vector3::Dot(currentAimDir, up)) > 0.999f)
                up = Mathematics::Vector3(1.0f, 0.0f, 0.0f);
            rotAxis = Mathematics::Vector3::Cross(currentAimDir, up).Normalize();
        }
        else
        {
            return;
        }
    }
    else
    {
        rotAxis = rotAxis.Normalize();
    }

    // Apply clamped rotation in world space
    Mathematics::Quaternion aimDelta =
        Mathematics::Quaternion::FromAxisAngle(rotAxis, clampedAngle);
    Mathematics::Quaternion newWorldRot = aimDelta * boneWorldRot;

    // Convert to local space
    int32_t parent = m_ParentIndices[m_BoneIndex];
    Mathematics::Quaternion parentWorldRot = (parent >= 0 && static_cast<uint32_t>(parent) < worldPose.Rotations.size())
        ? worldPose.Rotations[parent]
        : Mathematics::Quaternion::Identity();

    Mathematics::Quaternion newLocalRot =
        PoseUtilities::WorldToLocalRotation(newWorldRot, parentWorldRot);

    // Blend by weight
    outPose.Rotations[m_BoneIndex] = Mathematics::Quaternion::Slerp(
        outPose.Rotations[m_BoneIndex], newLocalRot, m_Weight);
}

} // namespace GameEngine::Animation
