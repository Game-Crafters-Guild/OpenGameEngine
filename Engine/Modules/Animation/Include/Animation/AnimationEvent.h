#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace GameEngine
{
namespace Animation
{

// A single event marker on a clip timeline.
// Events fire when playback crosses their time during a frame.
struct AnimationEvent
{
    float Time = 0.0f;
    std::string Name;

    using Payload = std::variant<std::monostate, float, int32_t, bool, std::string>;
    Payload Data;
};

// An event that fired during a frame's animation wave. It holds a copy of the event, so it outlives a reload that
// replaces the clip or montage whose track held it.
struct FiredEvent
{
    AnimationEvent Event;
};

// The events one animator's playback fired during a frame's animation wave, in the order they fired. The wave
// clears it before any playback runs; engine systems read it after the wave, and scripts poll it through the
// scripting ABI, each event once.
class AnimationEventCollector
{
public:
    // Empties the collector, forgets what scripts polled and releases the clip playback claim.
    void Clear();
    void Add(const AnimationEvent& event);
    const std::vector<FiredEvent>& GetEvents() const;
    bool HasEvents() const;

    // The events fired since the last Clear that no script has polled yet, in the order they fired.
    std::span<const FiredEvent> GetUnpolledEvents() const;
    // Marks the first `count` of GetUnpolledEvents() polled; a larger count marks them all.
    void MarkPolled(size_t count);

    // True for the first call after Clear and false after it. An animator's clip playback runs one AnimatorRef per
    // skinned entity under it, all at the same time; the first to tick in a wave claims the collector and supplies
    // the events, so each fires once.
    bool TryClaimClipPlayback();

private:
    std::vector<FiredEvent> m_Events;
    size_t m_PolledCount = 0;
    bool m_ClipPlaybackClaimed = false;
};

// The events of a timeline, in seconds, in time order: a clip's (AnimationClip::GetEventTrack) or a montage's
// (AnimationMontage::GetEventTrack).
class AnimationEventTrack
{
public:
    // Inserts `event` after every event at or before its time, so events at one time keep the order they were added.
    void AddEvent(const AnimationEvent& event);
    const std::vector<AnimationEvent>& GetEvents() const;

private:
    std::vector<AnimationEvent> m_Events;
};

// How playback moved over a timeline in one frame.
enum class PlaybackStepKind : uint8_t
{
    Forward,  // From up to To within one lap.
    Wrapped,  // From up past LapEnd, then from LapStart up to To, less than a lap in all (a looping clip or section).
    WholeLap, // From up past LapEnd and round a whole lap or more, to To.
    Backward, // From down to To (a montage at a negative play rate); it never wraps.
};

// One frame of playback over a timeline whose lap runs from LapStart to LapEnd, in seconds. A seek is not a step: a
// step starts where the frame's playback started, after any seek.
struct PlaybackStep
{
    float From = 0.0f;
    float To = 0.0f;
    float LapStart = 0.0f;
    float LapEnd = 0.0f;
    PlaybackStepKind Kind = PlaybackStepKind::Forward;
};

// Adds to `collector` the events of `track` that `step` crosses, each once, in the order playback reaches them:
// - Forward: the events in [From, To), and, when the step reaches LapEnd from before it, the events in [From, LapEnd]
//   (a clip that does not loop stops at its end, and the events on its last frame fire on arrival);
// - Wrapped: the events in [From, LapEnd], then those in [LapStart, To);
// - WholeLap: the events in [From, LapEnd], then those in [LapStart, From): every event of the lap once, however many
//   laps the frame spanned;
// - Backward: the events in (To, From], latest first.
// Only events inside [LapStart, LapEnd] fire, a step that does not move fires nothing, and neither does a lap of no
// length.
void CollectCrossedEvents(const AnimationEventTrack& track, const PlaybackStep& step,
                          AnimationEventCollector& collector);

} // namespace Animation
} // namespace GameEngine
