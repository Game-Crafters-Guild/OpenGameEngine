#include "Core/Time.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace GameEngine::Time
{
namespace
{
// Refreshed once per frame on the main thread, read widely (including from job
// threads), so an atomic guards against torn reads. Relaxed ordering suffices:
// consumers want a coherent recent scalar, not cross-thread happens-before.
std::atomic<float> s_FrameDeltaSeconds{0.0f};

// NaN = no deterministic override in effect; GetDeltaTime() then reports the
// measured frame delta. A real value pins the reported cadence.
std::atomic<float> s_DeterministicStep{std::numeric_limits<float>::quiet_NaN()};

// Monotonic sum of every frame's effective animation delta. Written only by the
// single per-frame SetFrameDeltaTime call on the main thread; read from any
// thread. Relaxed suffices for the same reason as the delta scalar above.
std::atomic<double> s_CumulativeSeconds{0.0};

// Cap on the measured per-frame delta fed into the cumulative animation clock —
// Unity's maximumDeltaTime analog (Unity defaults to 1/3s). A multi-second frame
// hitch (e.g. a mid-session shader-compile stall) would otherwise advance an
// animated UV scroll by a large, non-integer number of tiles in one frame, which
// reads as the pattern snapping backward. Capping bleeds the stall across the
// recovery frames instead. Applies to the measured delta only: the real Time
// delta (GetDeltaTime) is never capped, and a pinned deterministic step is
// applied exactly so movie-capture / replay stays byte-reproducible.
constexpr float kMaxAnimationDeltaSeconds = 0.25f;
} // namespace

float GetDeltaTime()
{
    const float step = s_DeterministicStep.load(std::memory_order_relaxed);
    return std::isnan(step) ? s_FrameDeltaSeconds.load(std::memory_order_relaxed) : step;
}

double GetCumulativeSeconds()
{
    return s_CumulativeSeconds.load(std::memory_order_relaxed);
}

void SetDeterministicStep(std::optional<float> stepSeconds)
{
    s_DeterministicStep.store(stepSeconds.has_value() ? *stepSeconds
                                                      : std::numeric_limits<float>::quiet_NaN(),
                              std::memory_order_relaxed);
}

namespace Detail
{
void SetFrameDeltaTime(float deltaSeconds)
{
    const float clamped = deltaSeconds < 0.0f ? 0.0f : deltaSeconds;
    s_FrameDeltaSeconds.store(clamped, std::memory_order_relaxed);

    // Advance the cumulative animation clock. When a deterministic step is pinned
    // it is applied exactly (replay byte-determinism; a fixed movie-capture step
    // must not be truncated). Otherwise the measured delta drives it, capped at
    // kMaxAnimationDeltaSeconds so a frame hitch can't jump animation phase. Single
    // writer (main thread), so a load/store pair suffices without an atomic RMW.
    const float step = s_DeterministicStep.load(std::memory_order_relaxed);
    const float animationDelta =
        std::isnan(step) ? std::min(clamped, kMaxAnimationDeltaSeconds) : step;
    s_CumulativeSeconds.store(
        s_CumulativeSeconds.load(std::memory_order_relaxed) + static_cast<double>(animationDelta),
        std::memory_order_relaxed);
}
} // namespace Detail

} // namespace GameEngine::Time
