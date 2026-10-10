#pragma once

#include <algorithm>
#include <cmath>

namespace GameEngine
{

// Logical seconds. Display frame rate never changes playback speed.
struct TimelineState
{
    float currentTime = 0.0f;
    float fullStart = 0.0f;   // full timeline domain
    float fullEnd = 10.0f;
    float rangeStart = 0.0f;  // playback loop range
    float rangeEnd = 10.0f;
    float viewStart = 0.0f;   // visible window (zoom/pan)
    float viewEnd = 10.0f;
    float fps = 60.0f;
    bool playing = false;
    bool paused = false;
    bool loop = true;
    bool reverse = false;     // when playing, advance time backwards
};

// Returns actual unwrapped movement for playhead-follow scrolling. Looping
// retains overshoot, including several full periods in a single update.
inline double AdvanceTimeline(TimelineState& state, double elapsedSeconds)
{
    if (!state.playing || state.paused || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0)
        return 0.0;
    if (!std::isfinite(state.rangeStart) || !std::isfinite(state.rangeEnd) ||
        !std::isfinite(state.currentTime) || state.rangeEnd <= state.rangeStart)
    {
        state.currentTime = std::isfinite(state.rangeStart) ? state.rangeStart : 0.0f;
        state.playing = false;
        state.paused = false;
        return 0.0;
    }

    const double start = state.rangeStart;
    const double end = state.rangeEnd;
    const double previous = std::clamp(static_cast<double>(state.currentTime), start, end);
    const double delta = state.reverse ? -elapsedSeconds : elapsedSeconds;
    const double next = previous + delta;
    if (state.loop)
    {
        // Retain the endpoint at exact arrival so both first and last poses
        // can be displayed. The next positive tick wraps in playback direction.
        if (next > end || next < start)
        {
            const double length = end - start;
            double offset = std::fmod(next - start, length);
            if (offset < 0.0) offset += length;
            state.currentTime = static_cast<float>(start + offset);
            if (state.reverse && offset == 0.0) state.currentTime = state.rangeEnd;
        }
        else state.currentTime = static_cast<float>(next);
        return delta;
    }

    state.currentTime = static_cast<float>(std::clamp(next, start, end));
    if ((!state.reverse && next >= end) || (state.reverse && next <= start))
    {
        state.playing = false;
        state.paused = false;
    }
    return static_cast<double>(state.currentTime) - previous;
}

} // namespace GameEngine
