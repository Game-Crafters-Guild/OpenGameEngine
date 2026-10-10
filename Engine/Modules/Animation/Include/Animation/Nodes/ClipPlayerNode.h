#pragma once

#include "Animation/AnimGraphNode.h"
#include "AssetCore/GUID.h"

#include <memory>

namespace GameEngine
{
namespace Animation
{
class AnimationClip;

// Plays a single animation clip and outputs the sampled pose.
// This is the leaf node equivalent of the legacy Animator component.
class ClipPlayerNode : public AnimGraphNode
{
public:
    void SetClip(std::shared_ptr<AnimationClip> clip);
    void SetClipGuid(const GUID& guid);
    const GUID& GetClipGuid() const;
    void SetSpeed(float speed);
    void SetLooping(bool loop);
    void SetTime(float time);

    float GetTime() const;
    float GetDuration() const;
    float GetNormalizedTime() const;
    float GetSpeed() const;
    bool GetLooping() const;
    const std::shared_ptr<AnimationClip>& GetClip() const;

    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

private:
    std::shared_ptr<AnimationClip> m_Clip;
    GUID m_ClipGuid{};
    float m_Time = 0.0f;
    float m_Speed = 1.0f;
    bool m_Loop = true;
};

} // namespace Animation
} // namespace GameEngine
