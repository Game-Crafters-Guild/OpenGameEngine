#include "Animation/Nodes/AdditiveBlendNode.h"

#include "Animation/BoneMask.h"
#include "Animation/EvaluationContext.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cstdint>

namespace GameEngine::Animation
{

using Mathematics::Quaternion;
using Mathematics::Vector3;

void AdditiveBlendNode::SetBase(std::unique_ptr<AnimGraphNode> node)
{
    m_Base = std::move(node);
}

void AdditiveBlendNode::SetAdditive(std::unique_ptr<AnimGraphNode> node)
{
    m_Additive = std::move(node);
}

void AdditiveBlendNode::SetWeight(float weight)
{
    m_Weight = weight;
}

void AdditiveBlendNode::SetMask(const BoneMask* mask)
{
    m_Mask = mask;
}

const AnimGraphNode* AdditiveBlendNode::GetBase() const { return m_Base.get(); }
const AnimGraphNode* AdditiveBlendNode::GetAdditive() const { return m_Additive.get(); }
float AdditiveBlendNode::GetWeight() const { return m_Weight; }

void AdditiveBlendNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (!m_Base && !m_Additive)
        return;

    if (!m_Base)
    {
        m_Additive->Evaluate(ctx, outPose);
        return;
    }

    m_Base->Evaluate(ctx, outPose);

    if (!m_Additive || m_Weight <= 0.0f)
        return;

    // The additive layer adds onto the base, which keeps its weight.
    EvaluateAtWeight(*m_Additive, ctx, m_Weight, m_AdditivePose);

    const uint32_t count =
        static_cast<uint32_t>(std::min(outPose.Positions.size(), m_AdditivePose.Positions.size()));

    for (uint32_t i = 0; i < count; ++i)
    {
        float maskWeight = (m_Mask && i < m_Mask->Weights.size()) ? m_Mask->Weights[i] : 1.0f;
        float effectiveWeight = m_Weight * maskWeight;

        if (effectiveWeight <= 0.0f)
            continue;

        outPose.Positions[i] = outPose.Positions[i] + m_AdditivePose.Positions[i] * effectiveWeight;

        Quaternion additiveRot =
            Quaternion::Slerp(Quaternion::Identity(), m_AdditivePose.Rotations[i], effectiveWeight);
        outPose.Rotations[i] = outPose.Rotations[i] * additiveRot;

        Vector3 identity(1.0f, 1.0f, 1.0f);
        Vector3 scaleFactor = identity * (1.0f - effectiveWeight) +
                              m_AdditivePose.Scales[i] * effectiveWeight;
        outPose.Scales[i] = Vector3(outPose.Scales[i].x * scaleFactor.x,
                                    outPose.Scales[i].y * scaleFactor.y,
                                    outPose.Scales[i].z * scaleFactor.z);
    }
}

} // namespace GameEngine::Animation
