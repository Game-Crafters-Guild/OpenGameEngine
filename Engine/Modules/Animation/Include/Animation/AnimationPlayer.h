#pragma once

#include "Animation/AnimationLibrary.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"

#include <string>

namespace GameEngine
{
namespace Animation
{

struct AnimationPlayerActive
{
    std::string Name;
    GUID AssetGuid;
    AssetType Type = AssetType::Animation;
    float32 TimeSeconds = 0.0f;
    float32 Speed = 1.0f;
    float32 DefaultBlendSeconds = 0.0f;
    float32 SectionStartSeconds = 0.0f;
    float32 SectionEndSeconds = 0.0f;
    bool HasSection = false;
    bool Loop = true;
};

class AnimationPlayer
{
public:
    void SetLibrary(const AnimationLibrary* library) { m_Library = library; }

    bool Play(const std::string& animationName);
    void PlayAsset(GUID assetGuid, AssetType type = AssetType::Animation, bool loop = true);
    bool PlaySection(const std::string& animationName, float32 startSeconds, float32 endSeconds);
    bool PlayWithBlend(const std::string& animationName, float32 blendSeconds);
    bool Queue(const std::string& animationName);
    void Seek(float32 timeSeconds);
    void Pause();
    void Resume();
    void Stop();
    void Update(float32 deltaSeconds, float32 currentDurationSeconds = 0.0f);

    bool IsPlaying() const { return m_Playing; }
    bool IsPaused() const { return m_Paused; }
    bool IsBlending() const { return m_BlendDurationSeconds > 0.0f && m_BlendElapsedSeconds < m_BlendDurationSeconds; }
    float32 GetCurrentTime() const { return m_Current.TimeSeconds; }
    const AnimationPlayerActive& Current() const { return m_Current; }
    const AnimationPlayerActive& Previous() const { return m_Previous; }
    float32 GetBlendWeight() const;

private:
    bool Resolve(const std::string& animationName, AnimationPlayerActive& outActive) const;
    void Start(AnimationPlayerActive active, float32 blendSeconds);

    const AnimationLibrary* m_Library = nullptr;
    AnimationPlayerActive m_Current;
    AnimationPlayerActive m_Previous;
    std::string m_QueuedName;
    float32 m_BlendElapsedSeconds = 0.0f;
    float32 m_BlendDurationSeconds = 0.0f;
    bool m_Playing = false;
    bool m_Paused = false;
};

} // namespace Animation
} // namespace GameEngine
