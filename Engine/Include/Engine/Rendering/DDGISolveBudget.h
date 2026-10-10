#pragma once

#include "Types/Types.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{
// Adaptive per-tick ray budget for the DDGI solve, the reference library's
// cadence controller (gi_probes.js tick(): probeBudgetAfterCadenceMiss /
// probeBudgetAfterInteraction): the solve takes GPU time the frame can spare
// instead of a fixed slice. An EMA of solve-tick spacing shrinks the budget
// hard (×0.5 toward a responsiveness floor) the moment the cadence slips past
// 60 Hz, holds a cooldown so a render-bound scene does not saw-tooth between
// floor and ceiling, and grows back in small steps while headroom lasts.
//
// Fed only on ACCEPTED solve ticks (the caller returns before this while the
// idle gate holds the field), so held stretches never read as solve pressure.
// All thresholds are display-cadence numbers (60 Hz misses), not
// platform-tuned trace sizes — they carry over from the reference unscaled.
class DDGISolveBudget
{
  public:
    // Advance one accepted solve tick. frameDtMs is the frame delta in the
    // caller's clock (the same one the blend's hysteresis normalization uses).
    void Tick(float frameDtMs);

    // Strict-idle resume: a field held during interaction must not come back
    // at its stale pre-interaction maximum and compete with the same first
    // resting frames that absorb catch-up work. Clamps toward the resume
    // reserve and re-measures cadence from scratch.
    void OnRestResume();

    // Converged-field throttle (DDGIConvergedSolve::Throttle): while set,
    // RaysPerTick is capped at a small patrol budget — enough to keep
    // sweeping the grid so a lighting change is noticed, not enough to cost
    // the frame anything. The cadence controller keeps running underneath,
    // so releasing the throttle returns to whatever it had settled on.
    void SetConvergedThrottle(bool throttled) { m_ConvergedThrottle = throttled; }
    bool IsConvergedThrottled() const { return m_ConvergedThrottle; }
    // True while engaging the throttle would change nothing: the cadence
    // controller already sits at or below the patrol budget. The caller skips
    // the reduction dispatch and readback in that state — measured on a frame
    // with no cadence headroom, they were pure cost.
    bool ThrottleWouldReduce() const { return m_RaysPerTick > kRaysPerTickConverged; }

    uint32 RaysPerTick() const
    {
        return m_ConvergedThrottle ? std::min(m_RaysPerTick, kRaysPerTickConverged) : m_RaysPerTick;
    }

  private:
    static constexpr uint32 kRaysPerTickMax = 98304;       // ceiling: whole-Sponza-class grids in one tick
    static constexpr uint32 kRaysPerTickMin = 2048;        // responsiveness floor on weak GPUs
    static constexpr uint32 kRaysPerTickRestResume = 4096; // post-interaction reserve
    // Patrol budget while the field is converged: 32 probes/tick at 64 rays,
    // so a 12^3 grid is re-swept in about a second at 60 Hz — the bound on
    // how late a lighting change can be noticed.
    static constexpr uint32 kRaysPerTickConverged = 2048;
    static constexpr uint32 kGrowStepRays = 1024;
    static constexpr float kShrinkFactor = 0.5f;
    static constexpr float kEmaAlpha = 0.2f;
    static constexpr float kShrinkAboveEmaMs = 18.5f;  // 60 Hz cadence miss
    static constexpr float kGrowBelowEmaMs = 17.2f;    // headroom under 60 Hz
    static constexpr float kOverloadDtMs = 100.0f;   // outside the EMA window; require repeated misses
    static constexpr float kPauseDtMs = 1000.0f;     // debugger/host gaps are pauses, not solve pressure
    static constexpr uint32 kOverloadStrikes = 2;    // ignore one unrelated stall; back off if it repeats
    static constexpr uint32 kShrinkCooldownTicks = 120;  // ~2 s hold before growing again
    static constexpr uint32 kRestResumeCooldownTicks = 30;

    void Shrink();

    uint32 m_RaysPerTick = kRaysPerTickMax;
    bool m_ConvergedThrottle = false;
    float m_DtEmaMs = 0.0f;
    uint32 m_Cooldown = 0;
    uint32 m_OverloadStreak = 0;
};

}  // namespace GameEngine::Engine::Renderer
