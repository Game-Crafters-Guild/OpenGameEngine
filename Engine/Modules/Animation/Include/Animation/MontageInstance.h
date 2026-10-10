#pragma once

#include "Animation/AnimationMontage.h"
#include "Animation/Nodes/ClipPlayerNode.h"

#include <string>

namespace GameEngine::Animation
{

struct AnimationPose;
class AnimationEventCollector;

// Runtime state for a montage that is currently playing.
class MontageInstance
{
public:
    enum class State
    {
        BlendingIn,
        Playing,
        BlendingOut,
        Finished
    };

    explicit MontageInstance(const AnimationMontage* montage);

    void Update(float deltaTime);
    void EvaluatePose(float deltaTime, AnimationPose& outPose);

    float GetBlendWeight() const;
    State GetState() const;
    bool IsFinished() const;

    void RequestBlendOut();
    // Moves the montage to the section's start. A jump crosses no events.
    void JumpToSection(const std::string& sectionName);

    // Adds to `collector` the montage's events the last Update crossed, in the order it crossed them: forward, or
    // latest first at a negative play rate.
    void CollectCrossedEvents(AnimationEventCollector& collector) const;

    float GetCurrentTime() const;
    const AnimationMontage* GetMontage() const;

private:
    const AnimationMontage* m_Montage = nullptr;
    State m_State = State::BlendingIn;
    float m_CurrentTime = 0.0f;
    // Where the last Update started; a jump moves it with the current time.
    float m_PreviousTime = 0.0f;
    float m_BlendWeight = 0.0f;
    float m_BlendTimer = 0.0f;
    ClipPlayerNode m_ClipPlayer;
};

} // namespace GameEngine::Animation
