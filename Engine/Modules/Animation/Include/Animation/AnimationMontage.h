#pragma once

#include "Animation/AnimationEvent.h"

#include <string>
#include <vector>

namespace GameEngine::Animation
{

struct BoneMask;

inline constexpr float kDefaultBlendInDuration = 0.2f;
inline constexpr float kDefaultBlendOutDuration = 0.2f;
inline constexpr float kDefaultPlayRate = 1.0f;

struct MontageSection
{
    std::string Name;
    float StartTime = 0.0f;
    float EndTime = 0.0f;
    std::string NextSection; // empty = continue linearly
};

// A montage is a clip with sections, events, and blend parameters.
// It represents a gameplay action (attack, emote, dodge) that temporarily
// overrides the animation graph's output via a MontageSlotNode.
class AnimationMontage
{
public:
    void SetClipGuid(const std::string& guid);
    const std::string& GetClipGuid() const;

    void SetClipDuration(float duration);
    float GetClipDuration() const;

    void SetBlendInDuration(float seconds);
    void SetBlendOutDuration(float seconds);
    float GetBlendInDuration() const;
    float GetBlendOutDuration() const;

    void AddSection(const MontageSection& section);
    const std::vector<MontageSection>& GetSections() const;
    const MontageSection* FindSection(const std::string& name) const;

    AnimationEventTrack& GetEventTrack();
    const AnimationEventTrack& GetEventTrack() const;

    void SetBoneMask(const BoneMask* mask);
    const BoneMask* GetBoneMask() const;

    void SetPlayRate(float rate);
    float GetPlayRate() const;

private:
    std::string m_ClipGuid;
    float m_ClipDuration = 0.0f;
    float m_BlendInDuration = kDefaultBlendInDuration;
    float m_BlendOutDuration = kDefaultBlendOutDuration;
    float m_PlayRate = kDefaultPlayRate;
    std::vector<MontageSection> m_Sections;
    AnimationEventTrack m_EventTrack;
    const BoneMask* m_BoneMask = nullptr;
};

} // namespace GameEngine::Animation
