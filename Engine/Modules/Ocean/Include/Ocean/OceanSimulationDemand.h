#pragma once

#include <atomic>
#include <chrono>

namespace GameEngine::Ocean
{

/// Whether the wave simulation must run with no water in view, and whether the
/// height fields' CPU copies are still current. Both are judged on the steady
/// clock, not on frames: the declare runs once per presented window, so a frame
/// count would lapse with each extra window, and gameplay queries arrive from
/// fixed-rate systems whose period is unrelated to the frame rate.
class OceanSimulationDemand
{
public:
    using Clock = std::chrono::steady_clock;

    /// How long a gameplay query keeps the wave simulation running, and how long
    /// a wave dispatch keeps the height fields' CPU copies current.
    static constexpr std::chrono::milliseconds kHold{500};

    /// A gameplay surface query arrived. Any thread.
    void NoteSurfaceQuery() { m_SurfaceQueried.store(true, std::memory_order_relaxed); }

    /// Folds the queries noted since the previous call into the hold. Declare
    /// thread; any number of calls per frame.
    void Update(Clock::time_point now);

    /// The wave simulation was dispatched. Declare thread.
    void NoteWaveDispatch(Clock::time_point now);

    /// True while a gameplay query arrived within kHold.
    bool HasRecentSurfaceQueries(Clock::time_point now) const;

    /// True when no wave dispatch happened within kHold: a height field's CPU copy
    /// holds waves from an earlier ocean time.
    bool IsWaveDataStale(Clock::time_point now) const;

private:
    std::atomic<bool> m_SurfaceQueried{false};
    bool m_HasQuery = false;
    bool m_HasWaveDispatch = false;
    Clock::time_point m_LastQuery{};
    Clock::time_point m_LastWaveDispatch{};
};

} // namespace GameEngine::Ocean
