#include "Animation/Nodes/FABRIKNode.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/PoseUtilities.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Animation
{

namespace
{
    constexpr float kMinDistanceSq = 1e-10f;
} // namespace

void FABRIKNode::SetSource(std::unique_ptr<AnimGraphNode> node) { m_Source = std::move(node); }
void FABRIKNode::SetChain(const std::vector<uint32_t>& boneIndices) { m_Chain = boneIndices; }
void FABRIKNode::SetTarget(const Mathematics::Vector3& target) { m_Target = target; }
void FABRIKNode::SetWeight(float weight) { m_Weight = weight; }
void FABRIKNode::SetParentIndices(const std::vector<int32_t>& parentIndices) { m_ParentIndices = parentIndices; }
void FABRIKNode::SetMaxIterations(uint32_t iterations) { m_MaxIterations = iterations; }
void FABRIKNode::SetTolerance(float tolerance) { m_Tolerance = tolerance; }

const AnimGraphNode* FABRIKNode::GetSource() const { return m_Source.get(); }
const std::vector<uint32_t>& FABRIKNode::GetChain() const { return m_Chain; }
float FABRIKNode::GetWeight() const { return m_Weight; }
uint32_t FABRIKNode::GetMaxIterations() const { return m_MaxIterations; }
float FABRIKNode::GetTolerance() const { return m_Tolerance; }

void FABRIKNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (m_Source)
        m_Source->Evaluate(ctx, outPose);

    if (m_Weight <= 0.0f || m_Chain.size() < 2)
        return;

    const uint32_t chainLen = static_cast<uint32_t>(m_Chain.size());

    // Bounds-check chain indices and parent table before reading.
    for (uint32_t boneIdx : m_Chain)
    {
        if (boneIdx >= outPose.BoneCount)
            return;
        if (boneIdx >= m_ParentIndices.size())
            return;
    }

    // Compute world-space positions into the persistent scratch buffer.
    PoseUtilities::ComputeWorldSpace(outPose, m_ParentIndices, m_ScratchWorld);
    WorldSpacePose& worldPose = m_ScratchWorld;

    // Reuse persistent vectors instead of constructing fresh ones each frame.
    m_ScratchPositions.resize(chainLen);
    m_ScratchOriginalPositions.resize(chainLen);
    m_ScratchBoneLengths.resize(chainLen > 0 ? chainLen - 1 : 0);

    auto& positions = m_ScratchPositions;
    auto& originalPositions = m_ScratchOriginalPositions;
    auto& boneLengths = m_ScratchBoneLengths;

    for (uint32_t i = 0; i < chainLen; ++i)
        positions[i] = worldPose.Positions[m_Chain[i]];

    for (uint32_t i = 0; i < chainLen; ++i)
        originalPositions[i] = positions[i];

    for (uint32_t i = 0; i < chainLen - 1; ++i)
        boneLengths[i] = (positions[i + 1] - positions[i]).Length();

    // Store the fixed root position
    Mathematics::Vector3 rootPos = positions[0];

    // Iterative FABRIK solve
    for (uint32_t iter = 0; iter < m_MaxIterations; ++iter)
    {
        // Check convergence
        float tipToTargetDist = (positions[chainLen - 1] - m_Target).Length();
        if (tipToTargetDist <= m_Tolerance)
            break;

        // Forward reaching: tip to root
        positions[chainLen - 1] = m_Target;
        for (uint32_t i = chainLen - 1; i > 0; --i)
        {
            Mathematics::Vector3 dir = positions[i - 1] - positions[i];
            float len = dir.Length();
            if (len * len > kMinDistanceSq)
                dir = dir.Normalize();
            else
                dir = Mathematics::Vector3(0.0f, 1.0f, 0.0f);

            positions[i - 1] = positions[i] + dir * boneLengths[i - 1];
        }

        // Backward reaching: root to tip
        positions[0] = rootPos;
        for (uint32_t i = 0; i < chainLen - 1; ++i)
        {
            Mathematics::Vector3 dir = positions[i + 1] - positions[i];
            float len = dir.Length();
            if (len * len > kMinDistanceSq)
                dir = dir.Normalize();
            else
                dir = Mathematics::Vector3(0.0f, 1.0f, 0.0f);

            positions[i + 1] = positions[i] + dir * boneLengths[i];
        }
    }

    // Blend with original positions by weight
    if (m_Weight < 1.0f)
    {
        for (uint32_t i = 0; i < chainLen; ++i)
            positions[i] = originalPositions[i] * (1.0f - m_Weight) + positions[i] * m_Weight;
    }

    // Convert world-space positions back to local-space rotations.
    // For each bone pair in the chain, compute the rotation delta and update.
    for (uint32_t i = 0; i < chainLen - 1; ++i)
    {
        uint32_t boneIdx = m_Chain[i];
        uint32_t childIdx = m_Chain[i + 1];

        // Original direction from this bone to next in world space
        Mathematics::Vector3 origDir = worldPose.Positions[childIdx] - worldPose.Positions[boneIdx];
        Mathematics::Vector3 newDir = positions[i + 1] - positions[i];

        float origLen = origDir.Length();
        float newLen = newDir.Length();

        if (origLen < 0.0001f || newLen < 0.0001f)
            continue;

        origDir = origDir.Normalize();
        newDir = newDir.Normalize();

        float dot = Mathematics::Vector3::Dot(origDir, newDir);
        if (dot > 0.9999f)
            continue;

        if (dot < -0.9999f)
        {
            Mathematics::Vector3 perp = Mathematics::Vector3::Cross(
                Mathematics::Vector3(1.0f, 0.0f, 0.0f), origDir);
            if (perp.Length() < 0.001f)
                perp = Mathematics::Vector3::Cross(Mathematics::Vector3(0.0f, 1.0f, 0.0f), origDir);
            perp = perp.Normalize();
            Mathematics::Quaternion delta = Mathematics::Quaternion::FromAxisAngle(
                perp, 3.14159265358979323846f);
            Mathematics::Quaternion newWorldRot = delta * worldPose.Rotations[boneIdx];

            int32_t parent = (boneIdx < m_ParentIndices.size()) ? m_ParentIndices[boneIdx] : -1;
            Mathematics::Quaternion parentWorldRot = (parent >= 0 && static_cast<uint32_t>(parent) < worldPose.Rotations.size())
                ? worldPose.Rotations[parent]
                : Mathematics::Quaternion::Identity();

            outPose.Rotations[boneIdx] =
                PoseUtilities::WorldToLocalRotation(newWorldRot, parentWorldRot);
            continue;
        }

        Mathematics::Vector3 axis = Mathematics::Vector3::Cross(origDir, newDir).Normalize();
        float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));

        Mathematics::Quaternion delta = Mathematics::Quaternion::FromAxisAngle(axis, angle);
        Mathematics::Quaternion newWorldRot = delta * worldPose.Rotations[boneIdx];

        int32_t parent = (boneIdx < m_ParentIndices.size()) ? m_ParentIndices[boneIdx] : -1;
        Mathematics::Quaternion parentWorldRot = (parent >= 0 && static_cast<uint32_t>(parent) < worldPose.Rotations.size())
            ? worldPose.Rotations[parent]
            : Mathematics::Quaternion::Identity();

        outPose.Rotations[boneIdx] =
            PoseUtilities::WorldToLocalRotation(newWorldRot, parentWorldRot);
    }
}

} // namespace GameEngine::Animation
