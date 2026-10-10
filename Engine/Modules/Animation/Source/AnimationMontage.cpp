#include "Animation/AnimationMontage.h"

#include "Animation/BoneMask.h"

namespace GameEngine::Animation
{

void AnimationMontage::SetClipGuid(const std::string& guid)
{
    m_ClipGuid = guid;
}

const std::string& AnimationMontage::GetClipGuid() const
{
    return m_ClipGuid;
}

void AnimationMontage::SetClipDuration(float duration)
{
    m_ClipDuration = duration;
}

float AnimationMontage::GetClipDuration() const
{
    return m_ClipDuration;
}

void AnimationMontage::SetBlendInDuration(float seconds)
{
    m_BlendInDuration = seconds;
}

void AnimationMontage::SetBlendOutDuration(float seconds)
{
    m_BlendOutDuration = seconds;
}

float AnimationMontage::GetBlendInDuration() const
{
    return m_BlendInDuration;
}

float AnimationMontage::GetBlendOutDuration() const
{
    return m_BlendOutDuration;
}

void AnimationMontage::AddSection(const MontageSection& section)
{
    m_Sections.push_back(section);
}

const std::vector<MontageSection>& AnimationMontage::GetSections() const
{
    return m_Sections;
}

const MontageSection* AnimationMontage::FindSection(const std::string& name) const
{
    for (const auto& section : m_Sections)
    {
        if (section.Name == name)
            return &section;
    }
    return nullptr;
}

AnimationEventTrack& AnimationMontage::GetEventTrack()
{
    return m_EventTrack;
}

const AnimationEventTrack& AnimationMontage::GetEventTrack() const
{
    return m_EventTrack;
}

void AnimationMontage::SetBoneMask(const BoneMask* mask)
{
    m_BoneMask = mask;
}

const BoneMask* AnimationMontage::GetBoneMask() const
{
    return m_BoneMask;
}

void AnimationMontage::SetPlayRate(float rate)
{
    m_PlayRate = rate;
}

float AnimationMontage::GetPlayRate() const
{
    return m_PlayRate;
}

} // namespace GameEngine::Animation
