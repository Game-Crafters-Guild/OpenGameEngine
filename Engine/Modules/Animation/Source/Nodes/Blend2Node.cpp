#include "Animation/Nodes/Blend2Node.h"

#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"

#include <algorithm>

namespace GameEngine
{
namespace Animation
{

void Blend2Node::SetInputA(std::unique_ptr<AnimGraphNode> node)
{
    m_InputA = std::move(node);
}

void Blend2Node::SetInputB(std::unique_ptr<AnimGraphNode> node)
{
    m_InputB = std::move(node);
}

void Blend2Node::SetWeight(float weight)
{
    m_Weight = std::clamp(weight, 0.0f, 1.0f);
}

float Blend2Node::GetWeight() const
{
    return m_Weight;
}

const AnimGraphNode* Blend2Node::GetInputA() const { return m_InputA.get(); }
const AnimGraphNode* Blend2Node::GetInputB() const { return m_InputB.get(); }

void Blend2Node::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (!m_InputA && !m_InputB)
    {
        outPose.Resize(0);
        return;
    }

    if (!m_InputA)
    {
        m_InputB->Evaluate(ctx, outPose);
        return;
    }

    if (!m_InputB)
    {
        m_InputA->Evaluate(ctx, outPose);
        return;
    }

    AnimationPose poseA;
    AnimationPose poseB;
    EvaluateAtWeight(*m_InputA, ctx, 1.0f - m_Weight, poseA);
    EvaluateAtWeight(*m_InputB, ctx, m_Weight, poseB);

    AnimationPose::Blend(poseA, poseB, m_Weight, outPose);
}

} // namespace Animation
} // namespace GameEngine
