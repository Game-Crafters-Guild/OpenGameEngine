#pragma once

#include "Animation/AnimGraphNode.h"

#include <memory>
#include <string>

namespace GameEngine::Animation
{

class AnimationMontage;
struct AnimationPose;
class MontageInstance;

// Graph node that normally passes through its child's pose, but when a montage
// is active, blends between the child pose and the montage pose. A playing
// montage adds the events it crosses to the context's collector, from the frame
// it starts to the frame it finishes, whatever its own blend weight.
class MontageSlotNode : public AnimGraphNode
{
public:
    MontageSlotNode();
    ~MontageSlotNode() override;

    void SetSource(std::unique_ptr<AnimGraphNode> node);
    const AnimGraphNode* GetSource() const;

    void PlayMontage(const AnimationMontage* montage);
    void StopMontage();

    bool IsPlaying() const;
    void JumpToSection(const std::string& sectionName);

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    std::unique_ptr<AnimGraphNode> m_Source;
    std::unique_ptr<MontageInstance> m_ActiveMontage;
    AnimationPose* m_MontagePose = nullptr; // heap-allocated scratch pose
};

} // namespace GameEngine::Animation
