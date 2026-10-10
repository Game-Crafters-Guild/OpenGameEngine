#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/AnimationPose.h"
#include "Animation/BoneMask.h"

#include <memory>

namespace GameEngine::Animation
{

// Blends an overlay pose onto a base pose using a per-bone mask.
// Bones with mask weight 1.0 get the overlay; weight 0.0 keeps the base.
// Intermediate values lerp between base and overlay per bone.
class LayeredBlendNode : public AnimGraphNode
{
public:
    void SetBase(std::unique_ptr<AnimGraphNode> node);
    void SetOverlay(std::unique_ptr<AnimGraphNode> node);
    void SetMask(const BoneMask& mask);
    void SetBlendWeight(float weight);

    const AnimGraphNode* GetBase() const;
    const AnimGraphNode* GetOverlay() const;
    const BoneMask& GetMask() const;
    float GetBlendWeight() const;

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    std::unique_ptr<AnimGraphNode> m_Base;
    std::unique_ptr<AnimGraphNode> m_Overlay;
    BoneMask m_Mask;
    float m_BlendWeight = 1.0f;
    AnimationPose m_OverlayPose;
};

} // namespace GameEngine::Animation
