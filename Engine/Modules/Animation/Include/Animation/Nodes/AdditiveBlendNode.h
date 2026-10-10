#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/AnimationPose.h"

#include <memory>

namespace GameEngine::Animation
{

struct BoneMask;

// Applies an additive animation on top of a base pose.
// The additive input provides delta transforms (position offset, rotation delta, scale multiplier).
// Result = base + weighted additive delta, optionally masked per bone.
class AdditiveBlendNode : public AnimGraphNode
{
public:
    void SetBase(std::unique_ptr<AnimGraphNode> node);
    void SetAdditive(std::unique_ptr<AnimGraphNode> node);
    void SetWeight(float weight);
    void SetMask(const BoneMask* mask);

    const AnimGraphNode* GetBase() const;
    const AnimGraphNode* GetAdditive() const;
    float GetWeight() const;

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    std::unique_ptr<AnimGraphNode> m_Base;
    std::unique_ptr<AnimGraphNode> m_Additive;
    float m_Weight = 1.0f;
    const BoneMask* m_Mask = nullptr;
    AnimationPose m_AdditivePose;
};

} // namespace GameEngine::Animation
