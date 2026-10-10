#pragma once

#include "Mathematics/Easing.h"
#include "Types/Types.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{

using TweenHandle = uint32;

enum class TweenStatus : uint8
{
    Running,
    Paused,
    Completed,
    Killed
};

struct TweenRequest
{
    std::string Name;
    float32 From = 0.0f;
    float32 To = 1.0f;
    float32 DurationSeconds = 1.0f;
    float32 DelaySeconds = 0.0f;
    int32 LoopCount = 0; // 0 = play once, -1 = forever
    bool Yoyo = false;
    Math::TweenEasing Easing = Math::TweenEasing::Smooth;
    std::function<void(float32)> OnValue;
    std::function<void()> OnComplete;
};

class TweenService
{
public:
    TweenHandle Add(TweenRequest request);
    TweenHandle Interval(float32 durationSeconds, std::function<void()> onComplete = {});
    void Pause(TweenHandle handle);
    void Resume(TweenHandle handle);
    void Kill(TweenHandle handle);
    void Clear();
    void Update(float32 deltaSeconds);

    bool IsAlive(TweenHandle handle) const;
    TweenStatus GetStatus(TweenHandle handle) const;
    size_t ActiveCount() const;

private:
    struct TweenInstance
    {
        TweenHandle Handle = 0;
        TweenRequest Request;
        float32 Elapsed = 0.0f;
        int32 CompletedLoops = 0;
        TweenStatus Status = TweenStatus::Running;
    };

    TweenInstance* Find(TweenHandle handle);
    const TweenInstance* Find(TweenHandle handle) const;

    std::vector<TweenInstance> m_Tweens;
    TweenHandle m_NextHandle = 1;
};

} // namespace Animation
} // namespace GameEngine
