#include "Animation/AnimationEvent.h"

#include <algorithm>

namespace GameEngine
{
namespace Animation
{

namespace
{

// Adds the events of `events`, which are in time order, at a time from `low` up to `high`, `high` included when
// `includeHigh`, earliest first.
void AddForward(const std::vector<AnimationEvent>& events, float low, float high, bool includeHigh,
                AnimationEventCollector& collector)
{
    const auto first = std::lower_bound(events.begin(), events.end(), low,
                                        [](const AnimationEvent& event, float time) { return event.Time < time; });
    for (auto it = first; it != events.end(); ++it)
    {
        if (it->Time > high || (it->Time == high && !includeHigh))
            break;
        collector.Add(*it);
    }
}

// Adds the events of `events`, which are in time order, at a time above `low` and at or above `floor`, up to `high`
// included, latest first.
void AddBackward(const std::vector<AnimationEvent>& events, float low, float floor, float high,
                 AnimationEventCollector& collector)
{
    auto it = std::upper_bound(events.begin(), events.end(), high,
                               [](float time, const AnimationEvent& event) { return time < event.Time; });
    while (it != events.begin())
    {
        --it;
        if (it->Time <= low || it->Time < floor)
            break;
        collector.Add(*it);
    }
}

} // namespace

void AnimationEventCollector::Clear()
{
    m_Events.clear();
    m_PolledCount = 0;
    m_ClipPlaybackClaimed = false;
}

void AnimationEventCollector::Add(const AnimationEvent& event)
{
    m_Events.push_back({event});
}

const std::vector<FiredEvent>& AnimationEventCollector::GetEvents() const
{
    return m_Events;
}

bool AnimationEventCollector::HasEvents() const
{
    return !m_Events.empty();
}

std::span<const FiredEvent> AnimationEventCollector::GetUnpolledEvents() const
{
    return std::span<const FiredEvent>(m_Events).subspan(m_PolledCount);
}

void AnimationEventCollector::MarkPolled(size_t count)
{
    m_PolledCount = std::min(m_Events.size(), m_PolledCount + count);
}

bool AnimationEventCollector::TryClaimClipPlayback()
{
    if (m_ClipPlaybackClaimed)
        return false;
    m_ClipPlaybackClaimed = true;
    return true;
}

void AnimationEventTrack::AddEvent(const AnimationEvent& event)
{
    const auto position = std::upper_bound(m_Events.begin(), m_Events.end(), event.Time,
                                           [](float time, const AnimationEvent& other) { return time < other.Time; });
    m_Events.insert(position, event);
}

const std::vector<AnimationEvent>& AnimationEventTrack::GetEvents() const
{
    return m_Events;
}

void CollectCrossedEvents(const AnimationEventTrack& track, const PlaybackStep& step,
                          AnimationEventCollector& collector)
{
    const std::vector<AnimationEvent>& events = track.GetEvents();
    if (events.empty() || !(step.LapEnd > step.LapStart))
        return;

    switch (step.Kind)
    {
    case PlaybackStepKind::Forward:
        if (!(step.To > step.From))
            return;
        if (step.To >= step.LapEnd)
        {
            if (step.From < step.LapEnd)
                AddForward(events, std::max(step.From, step.LapStart), step.LapEnd, true, collector);
            return;
        }
        AddForward(events, std::max(step.From, step.LapStart), step.To, false, collector);
        return;
    case PlaybackStepKind::Wrapped:
    {
        // Less than a lap lands before it started; the head stops there too, so a frame that rounds to a whole lap
        // in float cannot fire an event twice.
        const float from = std::max(step.From, step.LapStart);
        AddForward(events, from, step.LapEnd, true, collector);
        AddForward(events, step.LapStart, std::min({step.To, from, step.LapEnd}), false, collector);
        return;
    }
    case PlaybackStepKind::WholeLap:
    {
        const float from = std::clamp(step.From, step.LapStart, step.LapEnd);
        AddForward(events, from, step.LapEnd, true, collector);
        AddForward(events, step.LapStart, from, false, collector);
        return;
    }
    case PlaybackStepKind::Backward:
        if (!(step.To < step.From))
            return;
        AddBackward(events, step.To, step.LapStart, std::min(step.From, step.LapEnd), collector);
        return;
    }
}

} // namespace Animation
} // namespace GameEngine
