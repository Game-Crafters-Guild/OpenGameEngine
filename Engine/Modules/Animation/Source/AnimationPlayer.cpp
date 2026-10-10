#include "Animation/AnimationPlayer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace GameEngine
{
namespace Animation
{

namespace
{

float32 Wrap(float32 value, float32 duration)
{
    if (duration <= 0.0f)
        return value;
    float32 wrapped = std::fmod(value, duration);
    if (wrapped < 0.0f)
        wrapped += duration;
    return wrapped;
}

} // namespace

bool AnimationPlayer::Play(const std::string& animationName)
{
    AnimationPlayerActive active;
    if (!Resolve(animationName, active))
        return false;
    const float32 blendSeconds = active.DefaultBlendSeconds;
    Start(std::move(active), blendSeconds);
    return true;
}

void AnimationPlayer::PlayAsset(GUID assetGuid, AssetType type, bool loop)
{
    AnimationPlayerActive active;
    active.AssetGuid = assetGuid;
    active.Type = type;
    active.Loop = loop;
    Start(std::move(active), 0.0f);
}

bool AnimationPlayer::PlaySection(const std::string& animationName, float32 startSeconds, float32 endSeconds)
{
    AnimationPlayerActive active;
    if (!Resolve(animationName, active))
        return false;
    active.HasSection = true;
    active.SectionStartSeconds = std::max(0.0f, startSeconds);
    active.SectionEndSeconds = std::max(active.SectionStartSeconds, endSeconds);
    active.TimeSeconds = active.SectionStartSeconds;
    Start(std::move(active), 0.0f);
    return true;
}

bool AnimationPlayer::PlayWithBlend(const std::string& animationName, float32 blendSeconds)
{
    AnimationPlayerActive active;
    if (!Resolve(animationName, active))
        return false;
    Start(std::move(active), std::max(0.0f, blendSeconds));
    return true;
}

bool AnimationPlayer::Queue(const std::string& animationName)
{
    AnimationPlayerActive active;
    if (!Resolve(animationName, active))
        return false;
    m_QueuedName = animationName;
    return true;
}

void AnimationPlayer::Seek(float32 timeSeconds)
{
    m_Current.TimeSeconds = std::max(0.0f, timeSeconds);
}

void AnimationPlayer::Pause()
{
    m_Paused = true;
}

void AnimationPlayer::Resume()
{
    if (m_Playing)
        m_Paused = false;
}

void AnimationPlayer::Stop()
{
    m_Playing = false;
    m_Paused = false;
    m_QueuedName.clear();
    m_Current = {};
    m_Previous = {};
    m_BlendElapsedSeconds = 0.0f;
    m_BlendDurationSeconds = 0.0f;
}

void AnimationPlayer::Update(float32 deltaSeconds, float32 currentDurationSeconds)
{
    if (!m_Playing || m_Paused)
        return;

    const float32 dt = std::max(0.0f, deltaSeconds);
    m_Current.TimeSeconds += dt * m_Current.Speed;
    if (IsBlending())
        m_BlendElapsedSeconds = std::min(m_BlendDurationSeconds, m_BlendElapsedSeconds + dt);

    const float32 start = m_Current.HasSection ? m_Current.SectionStartSeconds : 0.0f;
    const float32 end = m_Current.HasSection ? m_Current.SectionEndSeconds : currentDurationSeconds;
    const float32 duration = std::max(0.0f, end - start);
    if (duration <= 0.0f)
        return;

    if (m_Current.TimeSeconds <= end)
        return;

    if (!m_QueuedName.empty())
    {
        const std::string queued = std::move(m_QueuedName);
        m_QueuedName.clear();
        Play(queued);
        return;
    }

    if (m_Current.Loop)
    {
        m_Current.TimeSeconds = start + Wrap(m_Current.TimeSeconds - start, duration);
    }
    else
    {
        m_Current.TimeSeconds = end;
        m_Playing = false;
    }
}

float32 AnimationPlayer::GetBlendWeight() const
{
    if (m_BlendDurationSeconds <= 0.0f)
        return 1.0f;
    return std::max(0.0f, std::min(1.0f, m_BlendElapsedSeconds / m_BlendDurationSeconds));
}

bool AnimationPlayer::Resolve(const std::string& animationName, AnimationPlayerActive& outActive) const
{
    if (!m_Library)
        return false;
    const AnimationLibraryEntry* entry = m_Library->Find(animationName);
    if (!entry)
        return false;
    outActive.Name = entry->Name;
    outActive.AssetGuid = entry->AssetGuid;
    outActive.Type = entry->Type;
    outActive.DefaultBlendSeconds = entry->DefaultBlendSeconds;
    outActive.Loop = entry->Loop;
    return true;
}

void AnimationPlayer::Start(AnimationPlayerActive active, float32 blendSeconds)
{
    m_Previous = m_Current;
    m_Current = std::move(active);
    m_BlendDurationSeconds = blendSeconds;
    m_BlendElapsedSeconds = 0.0f;
    m_Playing = true;
    m_Paused = false;
}

} // namespace Animation
} // namespace GameEngine
