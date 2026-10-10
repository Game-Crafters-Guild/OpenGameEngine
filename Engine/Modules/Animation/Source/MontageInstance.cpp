#include "Animation/MontageInstance.h"

#include "Animation/AnimationEvent.h"
#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"

#include <algorithm>

namespace GameEngine::Animation
{

MontageInstance::MontageInstance(const AnimationMontage* montage)
    : m_Montage(montage)
{
    // The internal ClipPlayerNode is a placeholder; clip-asset wiring is
    // future work, pending the montage system's full reintegration into
    // the ECS-driven AnimationSystem.
    m_ClipPlayer.SetSpeed(montage->GetPlayRate());
    m_ClipPlayer.SetLooping(false);
}

void MontageInstance::Update(float deltaTime)
{
    m_PreviousTime = m_CurrentTime;
    if (m_State == State::Finished)
        return;

    float scaledDelta = deltaTime * m_Montage->GetPlayRate();
    m_CurrentTime += scaledDelta;

    float clipDuration = m_Montage->GetClipDuration();
    float blendOutStart = clipDuration - m_Montage->GetBlendOutDuration();

    switch (m_State)
    {
    case State::BlendingIn:
    {
        m_BlendTimer += deltaTime;
        float blendInDuration = m_Montage->GetBlendInDuration();
        if (blendInDuration <= 0.0f)
        {
            m_BlendWeight = 1.0f;
            m_State = State::Playing;
        }
        else
        {
            m_BlendWeight = std::min(m_BlendTimer / blendInDuration, 1.0f);
            if (m_BlendWeight >= 1.0f)
            {
                m_State = State::Playing;
            }
        }
        // Check if we've already reached the blend-out point during blend-in
        if (m_CurrentTime >= blendOutStart && blendOutStart > 0.0f)
        {
            m_State = State::BlendingOut;
            m_BlendTimer = 0.0f;
        }
        break;
    }
    case State::Playing:
    {
        m_BlendWeight = 1.0f;
        if (m_CurrentTime >= blendOutStart && m_Montage->GetBlendOutDuration() > 0.0f)
        {
            m_State = State::BlendingOut;
            m_BlendTimer = 0.0f;
        }
        else if (m_CurrentTime >= clipDuration)
        {
            m_State = State::Finished;
            m_BlendWeight = 0.0f;
        }
        break;
    }
    case State::BlendingOut:
    {
        m_BlendTimer += deltaTime;
        float blendOutDuration = m_Montage->GetBlendOutDuration();
        if (blendOutDuration <= 0.0f)
        {
            m_BlendWeight = 0.0f;
            m_State = State::Finished;
        }
        else
        {
            m_BlendWeight = std::max(1.0f - (m_BlendTimer / blendOutDuration), 0.0f);
            if (m_BlendWeight <= 0.0f)
            {
                m_State = State::Finished;
            }
        }
        break;
    }
    case State::Finished:
        break;
    }
}

void MontageInstance::EvaluatePose(float deltaTime, AnimationPose& outPose)
{
    EvaluationContext ctx;
    ctx.DeltaTime = deltaTime;
    m_ClipPlayer.Evaluate(ctx, outPose);
}

float MontageInstance::GetBlendWeight() const
{
    return m_BlendWeight;
}

MontageInstance::State MontageInstance::GetState() const
{
    return m_State;
}

bool MontageInstance::IsFinished() const
{
    return m_State == State::Finished;
}

void MontageInstance::RequestBlendOut()
{
    if (m_State == State::BlendingIn || m_State == State::Playing)
    {
        m_State = State::BlendingOut;
        m_BlendTimer = 0.0f;
    }
}

void MontageInstance::JumpToSection(const std::string& sectionName)
{
    const MontageSection* section = m_Montage->FindSection(sectionName);
    if (!section)
        return;

    m_CurrentTime = section->StartTime;
    m_PreviousTime = section->StartTime;
    m_ClipPlayer.SetTime(section->StartTime);
}

void MontageInstance::CollectCrossedEvents(AnimationEventCollector& collector) const
{
    const PlaybackStep step{m_PreviousTime, m_CurrentTime, 0.0f, m_Montage->GetClipDuration(),
                            m_CurrentTime < m_PreviousTime ? PlaybackStepKind::Backward : PlaybackStepKind::Forward};
    ::GameEngine::Animation::CollectCrossedEvents(m_Montage->GetEventTrack(), step, collector);
}

float MontageInstance::GetCurrentTime() const
{
    return m_CurrentTime;
}

const AnimationMontage* MontageInstance::GetMontage() const
{
    return m_Montage;
}

} // namespace GameEngine::Animation
