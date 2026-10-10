#pragma once

#include <chrono>
#include <optional>

namespace GameEngine::UI
{

// The single boundary between the caret metrics the OS reports (milliseconds)
// and the seconds-valued floats the UI carries — UIManager's clock, the caret
// primitive's force-visible deadline and the shader's phase rate are all in
// seconds. Keeping both conversions here is what makes the caret family
// greppable as one unit: a search for millisecond literals elsewhere in the UI
// will not find caret timings, because there are none left to find.

// Rate at which the UI SDF shader advances the caret's on/off phase, in toggles
// per second. It is the reciprocal of the blink half-period reported by
// Platform::GetCaretBlinkHalfPeriod, and is the form SdfPushConstants carries
// because the shader steps phase with floor(timeSeconds * rate).
//
// A half-period of std::nullopt is the OS "never blink" accessibility setting.
// It maps to 0.0f, which is not a degenerate input to the shader but the exact
// expression of "solid": floor(timeSeconds * 0.0) is pinned at 0, so the phase
// never reaches the hidden half and the caret stays lit indefinitely.
//
// A non-positive half-period describes no blink either, and would put an
// infinity into a push constant, so it takes that same 0.0f path.
constexpr float CaretPhaseTogglesPerSecond(std::optional<std::chrono::milliseconds> blinkHalfPeriod)
{
    if (!blinkHalfPeriod.has_value() || blinkHalfPeriod->count() <= 0)
        return 0.0f;
    return 1000.0f / static_cast<float>(blinkHalfPeriod->count());
}

// Seconds for which a caret is held solid after an input that moves it, so the
// caret is never in its hidden half at the moment the user looks for it.
//
// One blink half-period: the caret is granted exactly the on-phase it would
// have received had the phase restarted, which is what the OS does when typing
// resets the caret timer. Deriving it keeps the pause proportional to the
// user's configured rate instead of imposing a fixed one on top of it.
//
// "Never blink" needs no window, and neither does a half-period that cannot
// describe a blink: the caret is already solid, so both return 0.0f.
constexpr float CaretForceVisibleSeconds(std::optional<std::chrono::milliseconds> blinkHalfPeriod)
{
    if (!blinkHalfPeriod.has_value() || blinkHalfPeriod->count() <= 0)
        return 0.0f;
    return static_cast<float>(blinkHalfPeriod->count()) / 1000.0f;
}

// The "hold the caret solid until" deadline a caret primitive carries, in the
// UIManager clock's seconds — the value the shader compares its own clock
// against before it will advance the blink phase.
//
// THE DEADLINE IS STAMPED BY THE INPUT THAT MOVES THE CARET AND THEN LEFT
// ALONE. One re-derived while the caret is merely being drawn sits a whole
// window ahead of the clock on every frame it is written, so the shader's test
// reads `t >= t + window` and the blink phase is never reached: the caret is
// solid for as long as repaints keep arriving. Every control that draws a caret
// holds one of these, and calls Bump only from an input handler — which is also
// what keeps the rule in one place instead of once per control.
class CaretForceVisibleDeadline
{
  public:
    // Extends the deadline to cover one blink half-period from `nowSeconds`.
    // Never shortens it, so overlapping inputs extend visibility rather than
    // truncating a window one of them already granted.
    void Bump(float nowSeconds, std::optional<std::chrono::milliseconds> blinkHalfPeriod)
    {
        const float until = nowSeconds + CaretForceVisibleSeconds(blinkHalfPeriod);
        if (until > m_Until)
            m_Until = until;
    }

    float Until() const { return m_Until; }

  private:
    float m_Until = 0.0f;
};

} // namespace GameEngine::UI
