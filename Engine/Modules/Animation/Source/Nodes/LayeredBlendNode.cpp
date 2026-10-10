#include "Animation/Nodes/LayeredBlendNode.h"

#include "Animation/EvaluationContext.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cstdint>

namespace GameEngine::Animation
{

using Mathematics::Quaternion;
using Mathematics::Vector3;

void LayeredBlendNode::SetBase(std::unique_ptr<AnimGraphNode> node)
{
    m_Base = std::move(node);
}

void LayeredBlendNode::SetOverlay(std::unique_ptr<AnimGraphNode> node)
{
    m_Overlay = std::move(node);
}

void LayeredBlendNode::SetMask(const BoneMask& mask)
{
    m_Mask = mask;
}

void LayeredBlendNode::SetBlendWeight(float weight)
{
    m_BlendWeight = weight;
}

const AnimGraphNode* LayeredBlendNode::GetBase() const { return m_Base.get(); }
const AnimGraphNode* LayeredBlendNode::GetOverlay() const { return m_Overlay.get(); }
const BoneMask& LayeredBlendNode::GetMask() const { return m_Mask; }
float LayeredBlendNode::GetBlendWeight() const { return m_BlendWeight; }

void LayeredBlendNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (!m_Base && !m_Overlay)
        return;

    if (!m_Base)
    {
        m_Overlay->Evaluate(ctx, outPose);
        return;
    }

    // Without a bone mask the overlay covers every bone and the base supplies what it leaves; under a mask the base
    // shows through wherever the mask is below full, so it keeps its weight.
    const float overlayWeight = m_Overlay ? std::max(m_BlendWeight, 0.0f) : 0.0f;
    const float baseWeight = m_Mask.Weights.empty() ? std::max(1.0f - overlayWeight, 0.0f) : 1.0f;
    EvaluateAtWeight(*m_Base, ctx, baseWeight, outPose);

    if (!m_Overlay || m_BlendWeight <= 0.0f)
        return;

    EvaluateAtWeight(*m_Overlay, ctx, m_BlendWeight, m_OverlayPose);

    const uint32_t count =
        static_cast<uint32_t>(std::min(outPose.Positions.size(), m_OverlayPose.Positions.size()));

    for (uint32_t i = 0; i < count; ++i)
    {
        float maskWeight = (i < m_Mask.Weights.size()) ? m_Mask.Weights[i] : 1.0f;
        float effectiveWeight = maskWeight * m_BlendWeight;

        if (effectiveWeight <= 0.0f)
            continue;

        float oneMinusW = 1.0f - effectiveWeight;
        outPose.Positions[i] = outPose.Positions[i] * oneMinusW +
                               m_OverlayPose.Positions[i] * effectiveWeight;
        outPose.Rotations[i] =
            Quaternion::Slerp(outPose.Rotations[i], m_OverlayPose.Rotations[i], effectiveWeight);
        outPose.Scales[i] = outPose.Scales[i] * oneMinusW +
                            m_OverlayPose.Scales[i] * effectiveWeight;
    }
}

} // namespace GameEngine::Animation
