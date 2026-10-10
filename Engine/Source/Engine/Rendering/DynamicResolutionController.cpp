#include "Engine/Rendering/DynamicResolutionController.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Engine::Renderer
{
namespace
{
// Below this the scalable cost is indistinguishable from zero and the ratio law
// would divide by noise.
constexpr float kMinScalableMs = 0.01f;

// Upper bound on banked slew budget (see m_SecondsSinceChange).
constexpr float kMaxSlewAccrualSeconds = 1.0f;

// A cost sample only reflects the current scale once the measurement pipeline
// has flushed: the render graph resolves timestamps FramesInFlight frames late
// and the EMA needs a few samples on top. Observations taken sooner than this
// after a change are contaminated by the previous scale and would bias the fit
// toward "no response".
constexpr float kFitSettleSeconds = 0.15f;

// Minimum time an accepted scale is held before the next change, INCLUDING an
// emergency drop. Two independent reasons, either of which alone justifies it:
// a position held for less than the settle time can never be measured (so the
// response fit would stay blind exactly during the descent that should be
// teaching it), and a change reallocates ~22 pool textures, so changing every
// frame is itself a performance problem. Descent speed is not lost: the slew
// budget accrues across the hold, so one change can cross many rungs at once.
constexpr float kMinHoldSeconds = kFitSettleSeconds;

// Fit gates. Variance is over x = scale^2; 0.002 corresponds to roughly a 0.06
// spread in scale around mid-range — enough for the slope to mean something.
// Observations are per settled POSITION, not per frame, so the count is small.
constexpr double kMinFitObservations = 3.0;
constexpr double kMinFitXVariance = 0.002;

bool IsFinitePositive(float v)
{
    return std::isfinite(v) && v > 0.0f;
}
} // namespace

const char* ToString(DynamicResolutionMode mode)
{
    switch (mode)
    {
    case DynamicResolutionMode::Fixed:
        return "fixed";
    case DynamicResolutionMode::Dynamic:
        return "dynamic";
    case DynamicResolutionMode::Off:
        break;
    }
    return "off";
}

DynamicResolutionMode ResolveStartupDynamicResolutionMode(std::string_view persistedMode,
                                                          float persistedScale)
{
    if (persistedMode == "fixed")
        return DynamicResolutionMode::Fixed;
    if (persistedMode == "dynamic")
        return DynamicResolutionMode::Dynamic;
    if (persistedMode == "off")
        return DynamicResolutionMode::Off;

    // No mode key: a project from before the setting existed. Its render scale
    // is the only statement of intent it has. Match the behaviour it has today
    // rather than reinterpreting it. The 0.995 threshold is the same one the
    // pipeline pre-pass uses to decide whether the split engages at all.
    if (std::isfinite(persistedScale) && persistedScale > 0.0f &&
        (persistedScale < 0.995f || persistedScale > 1.005f))
        return DynamicResolutionMode::Fixed;
    return DynamicResolutionMode::Off;
}

void DynamicResolutionController::ResponseFit::Decay(double factor)
{
    N *= factor;
    SumX *= factor;
    SumY *= factor;
    SumXX *= factor;
    SumXY *= factor;
}

void DynamicResolutionController::ResponseFit::Observe(double x, double y)
{
    N += 1.0;
    SumX += x;
    SumY += y;
    SumXX += x * x;
    SumXY += x * y;
}

bool DynamicResolutionController::ResponseFit::Solve(float& outUnscalableMs,
                                                     float& outScalableAtFullMs) const
{
    if (N < kMinFitObservations)
        return false;
    const double meanX = SumX / N;
    const double varX = SumXX / N - meanX * meanX;
    if (!(varX > kMinFitXVariance))
        return false; // the scale has not moved enough to have measured a slope
    const double slope = (SumXY / N - meanX * (SumY / N)) / varX;
    const double intercept = SumY / N - slope * meanX;
    if (!std::isfinite(slope) || !std::isfinite(intercept))
        return false;
    outScalableAtFullMs = static_cast<float>(slope);
    outUnscalableMs = static_cast<float>(intercept);
    return true;
}

void DynamicResolutionController::Configure(const DynamicResolutionConfig& config)
{
    m_Config = config;

    // Sanitize rather than trust: these come from project settings and the
    // debug server, and a NaN target would silently freeze the loop.
    if (!IsFinitePositive(m_Config.TargetGpuMs))
        m_Config.TargetGpuMs = 16.667f;
    if (!std::isfinite(m_Config.MinScale) || m_Config.MinScale <= 0.0f)
        m_Config.MinScale = 0.5f;
    if (!std::isfinite(m_Config.MaxScale) || m_Config.MaxScale <= 0.0f)
        m_Config.MaxScale = 1.0f;
    m_Config.MinScale = std::clamp(m_Config.MinScale, 0.5f, 1.0f);
    m_Config.MaxScale = std::clamp(m_Config.MaxScale, m_Config.MinScale, 1.0f);
    m_Config.DeadbandFraction = std::clamp(m_Config.DeadbandFraction, 0.0f, 0.9f);
    m_Config.DropGain = std::clamp(m_Config.DropGain, 0.0f, 1.0f);
    m_Config.RaiseGain = std::clamp(m_Config.RaiseGain, 0.0f, 1.0f);
    m_Config.MaxDropPerSecond = std::max(m_Config.MaxDropPerSecond, 0.0f);
    m_Config.MaxRisePerSecond = std::max(m_Config.MaxRisePerSecond, 0.0f);
    m_Config.DwellSeconds = std::max(m_Config.DwellSeconds, 0.0f);
    m_Config.QuantizeStep = std::clamp(m_Config.QuantizeStep, 0.001f, 0.5f);
    m_Config.SampleSmoothing = std::clamp(m_Config.SampleSmoothing, 0.01f, 1.0f);
    m_Config.EmergencyFraction = std::max(m_Config.EmergencyFraction, 1.0f);
    m_Config.MinUsefulSavingFraction = std::clamp(m_Config.MinUsefulSavingFraction, 0.0f, 1.0f);
    m_Config.ResponseDecay = std::clamp(m_Config.ResponseDecay, 0.5f, 0.999f);
    m_Config.RelatchCostFraction = std::max(m_Config.RelatchCostFraction, 0.01f);

    m_Scale = std::clamp(m_Scale, m_Config.MinScale, m_Config.MaxScale);
}

void DynamicResolutionController::Reset(float scale)
{
    if (!std::isfinite(scale) || scale <= 0.0f)
        scale = 1.0f;
    m_Scale = std::clamp(scale, m_Config.MinScale, m_Config.MaxScale);
    m_SmoothedGpuMs = 0.0f;
    m_DwellRemaining = 0.0f;
    m_SecondsSinceChange = 0.0f;
    m_HasSample = false;
    m_Fit = ResponseFit{};
    m_ObservedThisPosition = false;
    m_ScalingIneffective = false;
    m_IneffectiveLatchCostMs = 0.0f;
    m_Stats = Stats{};
    m_Stats.LastRequestedScale = m_Scale;
}

float DynamicResolutionController::QuantizeScale(float scale) const
{
    if (!std::isfinite(scale))
        return m_Config.MaxScale;
    scale = std::clamp(scale, m_Config.MinScale, m_Config.MaxScale);

    // Rung ladder anchored at MaxScale and stepping down, so the top of the
    // range is always exactly reachable (a ladder anchored at 0 would leave
    // MaxScale unreachable whenever it is not a step multiple). MinScale stays
    // reachable through the final clamp, which is why it need not be a rung.
    //
    // Round to NEAREST rung. Rounding always downward looks safer but makes the
    // top of the ladder unreachable: the correction is a damped fraction of the
    // remaining gap, so near MaxScale each step is a fraction of a rung and a
    // downward round cancels it forever. Half a rung of overshoot is far inside
    // the deadband; a permanently unreachable MaxScale is not.
    const float below = m_Config.MaxScale - scale;
    const float rungs = std::round(below / m_Config.QuantizeStep);
    const float quantized = m_Config.MaxScale - rungs * m_Config.QuantizeStep;
    return std::clamp(quantized, m_Config.MinScale, m_Config.MaxScale);
}

float DynamicResolutionController::Update(const DynamicResolutionSample& sample,
                                          float deltaSeconds)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
        deltaSeconds = 0.0f;
    ++m_Stats.Ticks;
    m_DwellRemaining = std::max(m_DwellRemaining - deltaSeconds, 0.0f);
    m_Stats.DwellRemainingSeconds = m_DwellRemaining;
    // Capped so a long quiet stretch cannot bank unbounded travel and let the
    // scale teleport on the first frame the load changes.
    m_SecondsSinceChange = std::min(m_SecondsSinceChange + deltaSeconds, kMaxSlewAccrualSeconds);

    // A missing or nonsensical sample holds the current scale. The measurement
    // resolves several frames late and can legitimately drop out (query pool
    // cap, a frame that never presented) — holding is correct, guessing is not.
    if (!sample.Valid || !IsFinitePositive(sample.GpuMs))
        return m_Scale;

    // The lever is not connected to anything this frame (no TAA-eligible view).
    // Moving it would be pure quality loss with no response to learn from, and
    // any cost change observed now belongs to something else — so hold, and do
    // not poison the response fit with observations the scale did not cause.
    m_Stats.ScaleDisconnected = !sample.ScaleIsConnected;
    if (!sample.ScaleIsConnected)
        return m_Scale;

    if (!m_HasSample)
    {
        m_SmoothedGpuMs = sample.GpuMs;
        m_HasSample = true;
    }
    else
    {
        m_SmoothedGpuMs += m_Config.SampleSmoothing * (sample.GpuMs - m_SmoothedGpuMs);
    }
    m_Stats.SmoothedGpuMs = m_SmoothedGpuMs;

    const float target = m_Config.TargetGpuMs;
    const float error = m_SmoothedGpuMs - target;
    const bool emergency = m_SmoothedGpuMs > target * m_Config.EmergencyFraction;

    // ── Measured response ──────────────────────────────────────────────────
    // Observe only once the sample reflects the CURRENT scale; a sample taken
    // right after a change still carries the previous one and would flatten the
    // fitted slope toward "scaling does nothing".
    //
    // Exactly ONE observation per settled position, not one per frame. The fit
    // is a regression over scale, so its memory should be counted in distinct
    // scales visited; sampling every frame would let a long stay at one scale
    // flood the statistics and collapse the x-variance the slope depends on —
    // the fit would go blind precisely when the controller had parked.
    if (!m_ObservedThisPosition && m_SecondsSinceChange >= kFitSettleSeconds)
    {
        // x is the APPLIED ratio, not the requested scale: the pipeline snaps
        // the internal extent to even pixels and only splits at all when the
        // view is TAA-active, so the two differ. Fitting against the request
        // would model a resolution the renderer never rendered.
        const float observedScale =
            IsFinitePositive(sample.AppliedScale) ? sample.AppliedScale : m_Scale;
        m_ObservedThisPosition = true;
        m_Fit.Decay(m_Config.ResponseDecay);
        m_Fit.Observe(static_cast<double>(observedScale) * observedScale, m_SmoothedGpuMs);
    }

    float fitUnscalable = 0.0f;
    float fitScalable = 0.0f;
    m_Stats.ResponseFitValid = m_Fit.Solve(fitUnscalable, fitScalable);
    if (m_Stats.ResponseFitValid)
    {
        const float fullRangeSaving =
            fitScalable * (m_Config.MaxScale * m_Config.MaxScale -
                           m_Config.MinScale * m_Config.MinScale);
        m_Stats.EstimatedUnscalableMs = std::clamp(fitUnscalable, 0.0f, m_SmoothedGpuMs);
        m_Stats.EstimatedFullRangeSavingMs = fullRangeSaving;

        // A latched verdict is only revisited when the content changes enough
        // to invalidate it.
        if (m_ScalingIneffective)
        {
            if (std::fabs(m_SmoothedGpuMs - m_IneffectiveLatchCostMs) >
                m_IneffectiveLatchCostMs * m_Config.RelatchCostFraction)
            {
                m_ScalingIneffective = false;
                m_Fit = ResponseFit{}; // the old slope described different content
            }
        }
        else if (m_Config.MinUsefulSavingFraction > 0.0f &&
                 fullRangeSaving < target * m_Config.MinUsefulSavingFraction)
        {
            // Geometry-bound (or otherwise fill-insensitive) content: driving
            // the scale to the floor would buy almost nothing. Spending half
            // the resolution for that is a strictly bad trade, so hand it back.
            m_ScalingIneffective = true;
            m_IneffectiveLatchCostMs = m_SmoothedGpuMs;
        }
    }
    m_Stats.ScalingIneffective = m_ScalingIneffective;

    if (m_ScalingIneffective && m_Scale >= m_Config.MaxScale - 1e-4f)
    {
        m_Stats.LastRequestedScale = m_Config.MaxScale;
        m_Stats.SaturatedLow = false;
        m_Stats.SaturatedHigh = false;
        return m_Scale; // already native; nothing to give back
    }

    // The deadband is about not chasing noise. It must not gate the walk back
    // to native when the lever has been judged useless — that decision is
    // independent of whether we are currently on budget.
    if (!m_ScalingIneffective && std::fabs(error) <= target * m_Config.DeadbandFraction)
    {
        m_Stats.LastRequestedScale = m_Scale;
        m_Stats.SaturatedLow = false;
        m_Stats.SaturatedHigh = false;
        return m_Scale;
    }

    // ── Ideal scale ────────────────────────────────────────────────────────
    // Cost model: cost(s) = U + S * s^2, with U the unscalable component (the
    // post-TAA chain, UI and shadow raster all run at fixed resolution) and
    // the scalable half proportional to internal pixel AREA. Solving for the
    // scale that lands on target gives s' = s * sqrt((target - U) / (cost - U)).
    //
    // With U unknown (0), that reduces to s' = s * sqrt(target / cost), which
    // systematically UNDER-corrects whenever U > 0 — the residual after one
    // step is U * (1 - target/cost). Under-correction is the safe direction:
    // it converges geometrically instead of overshooting, which is exactly the
    // damping a loop whose own action changes the measured quantity needs.
    // A caller-supplied split wins; otherwise the online fit supplies U as soon
    // as it has seen enough spread to mean anything. Both are optional — with
    // neither, the law degrades to the damped form above.
    float unscalable = 0.0f;
    if (std::isfinite(sample.UnscalableGpuMs) && sample.UnscalableGpuMs > 0.0f)
        unscalable = std::clamp(sample.UnscalableGpuMs, 0.0f, m_SmoothedGpuMs);
    else if (m_Stats.ResponseFitValid)
        unscalable = std::clamp(fitUnscalable, 0.0f, m_SmoothedGpuMs);

    const float scalableCost = std::max(m_SmoothedGpuMs - unscalable, kMinScalableMs);
    const float scalableTarget = target - unscalable;

    float ideal;
    if (m_ScalingIneffective)
    {
        // The lever has been measured and it does not move this content's
        // cost. Reduced resolution is then pure quality loss, so give it back
        // regardless of whether we are over budget — being over budget is not
        // a reason to pay a price that buys nothing.
        ideal = m_Config.MaxScale;
    }
    else if (scalableTarget <= kMinScalableMs)
    {
        // The fixed cost alone misses the target: no scale reaches it. Go to
        // the floor and let the saturation flag surface the fact. Still worth
        // doing — unlike the ineffective case, the lever does buy real time
        // here, it just cannot buy enough.
        ideal = m_Config.MinScale;
    }
    else
    {
        ideal = m_Scale * std::sqrt(scalableTarget / scalableCost);
    }
    ideal = std::clamp(ideal, m_Config.MinScale, m_Config.MaxScale);
    m_Stats.LastRequestedScale = ideal;

    // ── Gain, slew limit, clamp ────────────────────────────────────────────
    const bool dropping = ideal < m_Scale;
    const float gain = dropping ? m_Config.DropGain : m_Config.RaiseGain;
    float desired = m_Scale + gain * (ideal - m_Scale);

    // The slew budget accrues over the time since the last ACCEPTED change, not
    // per frame. Quantization means most frames produce no change at all, and a
    // per-frame budget smaller than one rung could never cross one — a rise
    // ladder with step 0.02 and a 0.15/s limit would deadlock permanently at
    // 0.0025 per frame. Accruing keeps the contract ("scale travels at most
    // R units per second") exact while letting a rung actually be reached.
    const float maxStep =
        (dropping ? m_Config.MaxDropPerSecond : m_Config.MaxRisePerSecond) * m_SecondsSinceChange;
    desired = std::clamp(desired, m_Scale - maxStep, m_Scale + maxStep);
    desired = std::clamp(desired, m_Config.MinScale, m_Config.MaxScale);

    float quantized = QuantizeScale(desired);

    // Rounding to the NEAREST rung can land up to half a step beyond the slew
    // bound, and with frequent changes that half-step bias accumulates into a
    // real breach of the per-second contract. Step back toward the current
    // scale instead; the progress rule below then waits for budget to accrue
    // rather than drifting past the limit.
    if (std::fabs(quantized - m_Scale) > maxStep + 1e-6f)
    {
        const float back = quantized > m_Scale ? quantized - m_Config.QuantizeStep
                                               : quantized + m_Config.QuantizeStep;
        quantized = std::clamp(back, m_Config.MinScale, m_Config.MaxScale);
        if (std::fabs(quantized - m_Scale) > maxStep + 1e-6f)
            quantized = m_Scale;
    }

    // Guaranteed progress. The damped correction shrinks as the scale nears the
    // ideal, so it can fall below half a rung and quantize back to where it
    // started while the error is still outside the deadband — a silent stall
    // short of the answer. When that happens, take exactly one rung toward the
    // ideal, but only if that rung does not step past it. The overshoot guard
    // is what keeps this from reintroducing the hunting the gain prevents.
    if (quantized == m_Scale && ideal != m_Scale)
    {
        const float rung = ideal < m_Scale ? m_Scale - m_Config.QuantizeStep
                                           : m_Scale + m_Config.QuantizeStep;
        const bool overshoots = ideal < m_Scale ? rung < ideal : rung > ideal;
        const bool withinSlew = std::fabs(rung - m_Scale) <= maxStep + 1e-6f;
        if (!overshoots && withinSlew)
            quantized = std::clamp(rung, m_Config.MinScale, m_Config.MaxScale);
    }

    // Saturation is "pinned against a limit and the error still points past
    // it". SaturatedLow means the target is genuinely unreachable — the cost
    // that does not scale already exceeds it — and is worth a warning.
    // SaturatedHigh is the ordinary healthy case of having headroom at full
    // resolution.
    m_Stats.SaturatedLow =
        !m_ScalingIneffective && m_Scale <= m_Config.MinScale + 1e-4f && error > 0.0f;
    m_Stats.SaturatedHigh = m_Scale >= m_Config.MaxScale - 1e-4f && error < 0.0f;

    // Only accept a change that moves the way the error asked. Quantization
    // snaps to the ladder, and when the current scale is off-ladder (right
    // after a Reset to a user-chosen value) that snap can land on the wrong
    // side of it — which would turn a requested rise into a drop.
    if (quantized == m_Scale || (dropping && quantized > m_Scale) ||
        (!dropping && quantized < m_Scale))
        return m_Scale;

    // Minimum hold, which even an emergency respects (see kMinHoldSeconds).
    if (m_SecondsSinceChange < kMinHoldSeconds)
        return m_Scale;

    // Dwell gate. An emergency drop is allowed through — protecting frame rate
    // outranks pool churn — but a rise never is, because a premature rise is
    // what turns a settled loop back into an oscillating one.
    const bool dwellBlocked = m_DwellRemaining > 0.0f && !(emergency && quantized < m_Scale);
    if (dwellBlocked)
        return m_Scale;

    if (quantized < m_Scale)
        ++m_Stats.AcceptedDrops;
    else
        ++m_Stats.AcceptedRises;

    m_Scale = quantized;
    m_DwellRemaining = m_Config.DwellSeconds;
    m_SecondsSinceChange = 0.0f;
    m_ObservedThisPosition = false;
    m_Stats.DwellRemainingSeconds = m_DwellRemaining;
    return m_Scale;
}

} // namespace GameEngine::Engine::Renderer
