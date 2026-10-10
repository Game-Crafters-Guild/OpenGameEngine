#include "Animation/TweenService.h"

#include <algorithm>
#include <utility>

namespace GameEngine
{
namespace Animation
{

namespace
{

float32 ClampTween01(float32 value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

} // namespace

TweenHandle TweenService::Add(TweenRequest request)
{
    TweenInstance instance;
    instance.Handle = m_NextHandle++;
    if (m_NextHandle == 0)
        m_NextHandle = 1;
    instance.Request = std::move(request);
    instance.Request.DurationSeconds = std::max(0.0f, instance.Request.DurationSeconds);
    instance.Request.DelaySeconds = std::max(0.0f, instance.Request.DelaySeconds);
    const TweenHandle handle = instance.Handle;
    m_Tweens.push_back(std::move(instance));
    return handle;
}

TweenHandle TweenService::Interval(float32 durationSeconds, std::function<void()> onComplete)
{
    TweenRequest request;
    request.DurationSeconds = durationSeconds;
    request.OnComplete = std::move(onComplete);
    return Add(std::move(request));
}

void TweenService::Pause(TweenHandle handle)
{
    if (auto* tween = Find(handle); tween && tween->Status == TweenStatus::Running)
        tween->Status = TweenStatus::Paused;
}

void TweenService::Resume(TweenHandle handle)
{
    if (auto* tween = Find(handle); tween && tween->Status == TweenStatus::Paused)
        tween->Status = TweenStatus::Running;
}

void TweenService::Kill(TweenHandle handle)
{
    if (auto* tween = Find(handle))
        tween->Status = TweenStatus::Killed;
}

void TweenService::Clear()
{
    m_Tweens.clear();
}

void TweenService::Update(float32 deltaSeconds)
{
    const float32 dt = std::max(0.0f, deltaSeconds);
    for (auto& tween : m_Tweens)
    {
        if (tween.Status != TweenStatus::Running)
            continue;

        tween.Elapsed += dt;
        if (tween.Elapsed < tween.Request.DelaySeconds)
            continue;

        const float32 localElapsed = tween.Elapsed - tween.Request.DelaySeconds;
        const float32 duration = tween.Request.DurationSeconds;
        const bool zeroDuration = duration <= 0.0f;
        float32 t = zeroDuration ? 1.0f : ClampTween01(localElapsed / duration);
        const bool reverse = tween.Request.Yoyo && ((tween.CompletedLoops & 1) != 0);
        if (reverse)
            t = 1.0f - t;

        const float32 eased = Math::EvalTweenEasing(tween.Request.Easing, t);
        const float32 value = tween.Request.From + (tween.Request.To - tween.Request.From) * eased;
        if (tween.Request.OnValue)
            tween.Request.OnValue(value);

        if (!zeroDuration && localElapsed < duration)
            continue;

        ++tween.CompletedLoops;
        const bool shouldLoop = tween.Request.LoopCount < 0 || tween.CompletedLoops <= tween.Request.LoopCount;
        if (shouldLoop)
        {
            tween.Elapsed = tween.Request.DelaySeconds;
            continue;
        }

        tween.Status = TweenStatus::Completed;
        if (tween.Request.OnComplete)
            tween.Request.OnComplete();
    }

    m_Tweens.erase(std::remove_if(m_Tweens.begin(), m_Tweens.end(),
        [](const TweenInstance& tween)
        {
            return tween.Status == TweenStatus::Completed || tween.Status == TweenStatus::Killed;
        }), m_Tweens.end());
}

bool TweenService::IsAlive(TweenHandle handle) const
{
    const auto* tween = Find(handle);
    return tween && tween->Status != TweenStatus::Completed && tween->Status != TweenStatus::Killed;
}

TweenStatus TweenService::GetStatus(TweenHandle handle) const
{
    if (const auto* tween = Find(handle))
        return tween->Status;
    return TweenStatus::Killed;
}

size_t TweenService::ActiveCount() const
{
    return std::count_if(m_Tweens.begin(), m_Tweens.end(),
        [](const TweenInstance& tween)
        {
            return tween.Status == TweenStatus::Running || tween.Status == TweenStatus::Paused;
        });
}

TweenService::TweenInstance* TweenService::Find(TweenHandle handle)
{
    for (auto& tween : m_Tweens)
    {
        if (tween.Handle == handle)
            return &tween;
    }
    return nullptr;
}

const TweenService::TweenInstance* TweenService::Find(TweenHandle handle) const
{
    for (const auto& tween : m_Tweens)
    {
        if (tween.Handle == handle)
            return &tween;
    }
    return nullptr;
}

} // namespace Animation
} // namespace GameEngine
