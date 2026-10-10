#include "Animation/Nodes/FootIKNode.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/PoseUtilities.h"
#include "Animation/Nodes/TwoBoneIKNode.h"

#include <Mathematics/Vector3.h>

#include <algorithm>
#include <cmath>

namespace GameEngine::Animation
{

void FootIKNode::SetSource(std::unique_ptr<AnimGraphNode> node)
{
    m_Source = std::move(node);
}

void FootIKNode::AddLeg(const FootIKLeg& leg)
{
    m_Legs.push_back(leg);
    m_GroundResults.resize(m_Legs.size());
}

void FootIKNode::SetGroundResult(uint32_t legIndex, const FootIKGroundResult& result)
{
    if (legIndex < m_GroundResults.size())
    {
        m_GroundResults[legIndex] = result;
    }
}

void FootIKNode::SetHipBoneIndex(uint32_t hipBone)
{
    m_HipBone = hipBone;
}

void FootIKNode::SetMaxHipOffset(float maxOffset)
{
    m_MaxHipOffset = maxOffset;
}

void FootIKNode::SetParentIndices(const std::vector<int32_t>& parentIndices)
{
    m_ParentIndices = parentIndices;
}

void FootIKNode::SetWeight(float weight)
{
    m_Weight = std::clamp(weight, 0.0f, 1.0f);
}

void FootIKNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (m_Source)
    {
        m_Source->Evaluate(ctx, outPose);
    }

    if (m_Weight <= 0.0f || m_Legs.empty() || outPose.BoneCount == 0)
        return;

    // Compute world-space pose for reading foot positions; reuse persistent scratch.
    PoseUtilities::ComputeWorldSpace(outPose, m_ParentIndices, m_ScratchWorld);
    WorldSpacePose& worldPose = m_ScratchWorld;

    // Calculate per-leg foot adjustments (vertical correction per foot).
    m_ScratchAdjustments.assign(m_Legs.size(), 0.0f);
    auto& footAdjustments = m_ScratchAdjustments;
    float minAdjustment = 0.0f;

    for (uint32_t i = 0; i < m_Legs.size(); ++i)
    {
        if (!m_GroundResults[i].Hit)
            continue;
        if (m_Legs[i].FootBone >= worldPose.Positions.size())
            continue;

        Mathematics::Vector3 footWorldPos = worldPose.Positions[m_Legs[i].FootBone];
        Mathematics::Vector3 contactPoint = footWorldPos + m_Legs[i].FootOffset;

        // Vertical distance from current contact point to ground.
        float adjustment = m_GroundResults[i].Position.y - contactPoint.y;
        adjustment = std::clamp(adjustment, -m_MaxHipOffset, m_MaxHipOffset);
        footAdjustments[i] = adjustment;

        // Track the lowest adjustment for hip offset.
        minAdjustment = std::min(minAdjustment, adjustment);
    }

    // Apply hip offset: drop the hip to accommodate the lowest foot.
    float hipOffset = minAdjustment * m_Weight;
    if (std::abs(hipOffset) > 1e-6f && m_HipBone < outPose.BoneCount)
    {
        outPose.Positions[m_HipBone].y += hipOffset;
    }

    // Recompute world-space after hip adjustment (reuses scratch).
    PoseUtilities::ComputeWorldSpace(outPose, m_ParentIndices, m_ScratchWorld);

    // For each leg with a ground hit, solve IK so the foot reaches the ground.
    for (uint32_t i = 0; i < m_Legs.size(); ++i)
    {
        if (!m_GroundResults[i].Hit)
            continue;
        if (m_Legs[i].FootBone >= worldPose.Positions.size())
            continue;

        // Use current world-space foot position with vertical adjustment as the target.
        Mathematics::Vector3 currentFootWorld = worldPose.Positions[m_Legs[i].FootBone];
        Mathematics::Vector3 ikTarget = Mathematics::Vector3(
            currentFootWorld.x,
            currentFootWorld.y + (footAdjustments[i] - minAdjustment) * m_Weight,
            currentFootWorld.z);

        TwoBoneIKNode::SolveTwoBoneIK(outPose, m_ParentIndices,
                                      m_Legs[i].HipBone, m_Legs[i].KneeBone, m_Legs[i].FootBone,
                                      ikTarget, m_Weight);
    }
}

} // namespace GameEngine::Animation
