#include "Animation/Nodes/BlendSpace1DNode.h"

#include "Animation/EvaluationContext.h"
#include "Types/StringId.h"

#include <algorithm>
#include <string_view>

namespace GameEngine::Animation
{

void BlendSpace1DNode::AddSample(std::unique_ptr<AnimGraphNode> node, float position)
{
    m_Samples.push_back({std::move(node), position});
}

void BlendSpace1DNode::Sort()
{
    std::sort(m_Samples.begin(), m_Samples.end(),
              [](const BlendSpace1DSample& a, const BlendSpace1DSample& b)
              { return a.Position < b.Position; });
}

void BlendSpace1DNode::SetParameterName(std::string_view name)
{
    m_ParameterName.assign(name);
    m_ParameterId = HashStringId(name);
}

void BlendSpace1DNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (m_ParameterId != 0 && ctx.Parameters)
    {
        auto it = ctx.Parameters->find(m_ParameterId);
        if (it != ctx.Parameters->end())
            m_Parameter = ParamAsFloat(it->second.Value);
    }

    if (m_Samples.empty())
    {
        return;
    }

    if (m_Samples.size() == 1)
    {
        if (m_Samples[0].Node)
            m_Samples[0].Node->Evaluate(ctx, outPose);
        else
            outPose.Resize(0);
        return;
    }

    float clampedParam = std::clamp(m_Parameter, m_Samples.front().Position,
                                    m_Samples.back().Position);

    // Find the two bracketing samples via linear scan (sample counts are small)
    size_t upperIdx = 0;
    for (size_t i = 0; i < m_Samples.size(); ++i)
    {
        if (m_Samples[i].Position >= clampedParam)
        {
            upperIdx = i;
            break;
        }
    }

    // Exact match or at/below the first sample
    if (upperIdx == 0 || m_Samples[upperIdx].Position == clampedParam)
    {
        if (m_Samples[upperIdx].Node)
            m_Samples[upperIdx].Node->Evaluate(ctx, outPose);
        else
            outPose.Resize(0);
        return;
    }

    size_t lowerIdx = upperIdx - 1;
    const auto& lower = m_Samples[lowerIdx];
    const auto& upper = m_Samples[upperIdx];

    float range = upper.Position - lower.Position;
    float alpha = (clampedParam - lower.Position) / range;

    if (!lower.Node || !upper.Node)
    {
        if (lower.Node)
            lower.Node->Evaluate(ctx, outPose);
        else if (upper.Node)
            upper.Node->Evaluate(ctx, outPose);
        else
            outPose.Resize(0);
        return;
    }
    EvaluateAtWeight(*lower.Node, ctx, 1.0f - alpha, outPose);
    EvaluateAtWeight(*upper.Node, ctx, alpha, m_TempPose);
    AnimationPose::Blend(outPose, m_TempPose, alpha, outPose);
}

} // namespace GameEngine::Animation
