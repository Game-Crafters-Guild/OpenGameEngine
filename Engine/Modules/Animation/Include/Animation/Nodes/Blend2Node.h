#pragma once

#include "Animation/AnimGraphNode.h"

#include <memory>

namespace GameEngine
{
namespace Animation
{

// Blends the output of two child nodes by a weight parameter.
// Weight 0 = fully input A, weight 1 = fully input B.
class Blend2Node : public AnimGraphNode
{
public:
    void SetInputA(std::unique_ptr<AnimGraphNode> node);
    void SetInputB(std::unique_ptr<AnimGraphNode> node);
    void SetWeight(float weight);

    float GetWeight() const;
    const AnimGraphNode* GetInputA() const;
    const AnimGraphNode* GetInputB() const;

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    std::unique_ptr<AnimGraphNode> m_InputA;
    std::unique_ptr<AnimGraphNode> m_InputB;
    float m_Weight = 0.0f;
};

} // namespace Animation
} // namespace GameEngine
