#pragma once

// Dynamic resolution scaling (DRS) control law. Moves the engine-default
// render scale to hold a GPU frame-time target, on top of the fixed-scale path
// that ships in RenderServices::SetDefaultRenderScale + the RenderPipeline
// pre-pass split.
//
// The controller is deliberately pure: cost samples and delta time in, a scale
// out. It owns no GPU state, reads no globals, and never touches RenderServices
// — which is what makes the stability properties (deadband, asymmetric rates,
// rate limiting, dwell, quantization) unit-testable against a synthetic cost
// model instead of only observable on a GPU.

#include "Types/Types.h"

#include <string_view>

namespace GameEngine::Engine::Renderer
{

// How the engine-default render scale is chosen. Three first-class user choices, not a
// hierarchy: Fixed is a fully supported configuration for anyone who has
// decided the quality/perf trade on their own hardware, not a fallback or a
// testing affordance.
enum class DynamicResolutionMode : uint8
{
    Off = 0,     // Native. Render scale pinned to 1.0 — the unsplit path. DEFAULT.
    Fixed = 1,   // The user's static Render Scale slider drives the scale.
    Dynamic = 2, // This controller drives the scale to hold TargetGpuMs.
};

// Settings-file token for a mode, and its inverse. Round-trips exactly.
const char* ToString(DynamicResolutionMode mode);

// Resolve the mode a project should start in.
//
// `persistedMode` is empty for a project saved before the mode setting existed.
// Such a project carries only rendering.taaRenderScale, and a sub-1.0 value
// there means the user deliberately chose a reduced scale — it must keep
// behaving exactly as it does today, which is Fixed. Defaulting it to Off would
// silently undo their choice; defaulting it to Dynamic would opt them into an
// unevaluated feature. Both are wrong.
DynamicResolutionMode ResolveStartupDynamicResolutionMode(std::string_view persistedMode,
                                                          float persistedScale);

struct DynamicResolutionConfig
{
    // GPU milliseconds the controller steers toward. The scale falls when the
    // measured cost exceeds this and rises when it sits below.
    float TargetGpuMs = 16.667f;

    // Same range the fixed path clamps to (RenderServices::SetDefaultRenderScale).
    // MaxScale below 1.0 keeps TAAU permanently engaged, which avoids the one
    // history reset that crossing the pre-pass split threshold costs.
    float MinScale = 0.5f;
    float MaxScale = 1.0f;

    // No action while |cost - target| <= target * DeadbandFraction. Sized to
    // sit above frame-to-frame GPU noise so the scale does not chatter.
    float DeadbandFraction = 0.10f;

    // Fraction of the computed correction applied per accepted update.
    // Asymmetric on purpose: drop decisively to protect frame rate, rise
    // gently so the loop cannot pump against its own cost change.
    float DropGain = 0.90f;
    float RaiseGain = 0.25f;

    // Hard slew limits in scale units per second. Down is ~8x up for the same
    // reason the gains are asymmetric.
    float MaxDropPerSecond = 1.20f;
    float MaxRisePerSecond = 0.15f;

    // Minimum time an accepted scale must hold before another change is taken.
    // Every change reallocates ~20 render-extent pool textures and restarts
    // any temporal history whose desc moved, so changes must be rare events.
    float DwellSeconds = 0.30f;

    // Ladder rung size. Quantizing bounds the number of distinct extents the
    // resource pool ever sees; the pipeline pre-pass then even-snaps the pixel
    // extent on top of this.
    float QuantizeStep = 0.02f;

    // EMA weight on incoming cost samples (1.0 = no smoothing). Damps the
    // measurement so a single spike frame cannot move the ladder.
    float SampleSmoothing = 0.35f;

    // A cost above target * EmergencyFraction bypasses the dwell timer so a
    // sudden load spike is answered immediately rather than up to DwellSeconds
    // later. Never bypasses the rate limit.
    float EmergencyFraction = 1.50f;

    // ── Response estimation ────────────────────────────────────────────────
    // If driving the scale from MaxScale all the way to MinScale is predicted
    // to save less than TargetGpuMs * MinUsefulSavingFraction, the render-scale
    // lever is judged INEFFECTIVE for the current content and the controller
    // returns to MaxScale instead of spending resolution for nothing.
    //
    // This is the geometry-bound case and it is not hypothetical: on the town
    // scene, DepthPrepass + RenderEntities dominate and are largely
    // resolution-independent, so 0.76 measures as no useful win. Note that
    // classifying passes as "pre-TAA therefore scalable" would NOT catch it —
    // those passes are in the scalable set, they simply do not respond. Only
    // the measured response separates the two.
    //
    // 0 disables the check entirely. It has to be an explicit zero rather than
    // a zero threshold: a genuinely flat response fits a slope that is only
    // zero to within an ulp, and its sign is arbitrary.
    float MinUsefulSavingFraction = 0.15f;

    // Per-observation decay on the response fit's sufficient statistics.
    // Steady-state memory is 1/(1-decay) observations.
    float ResponseDecay = 0.92f;

    // Once "ineffective" latches, re-probe only after the smoothed cost moves
    // this fraction away from where it latched — i.e. the content changed.
    // Without a latch the loop would drop, learn it is useless, return to
    // native, forget, and drop again.
    float RelatchCostFraction = 0.25f;
};

// One measurement handed to the controller. GpuMs is GPU-busy time, not a
// frame wall-clock period: a period measure folds in vsync waits and CPU-bound
// idle, which would make the controller chase a cost the scale cannot move.
struct DynamicResolutionSample
{
    float GpuMs = 0.0f;

    // The portion of GpuMs that does NOT respond to render scale (post-TAA
    // chain, UI, shadow raster). Optional: 0 means "unknown", which degrades
    // the control law to its damped form rather than making it wrong.
    float UnscalableGpuMs = 0.0f;

    // The scale the renderer ACTUALLY applied when this cost was measured,
    // after the even-pixel snap and the TAA-active gate. A requested scale is
    // not always the applied one, and fitting the response model against a
    // number the renderer never used would integrate error against a fiction.
    // 0 means "unknown" and falls back to the controller's own scale.
    float AppliedScale = 0.0f;

    // Whether any view can apply the render-scale lever at all — TAA active,
    // not letterboxed, single-sample, with a materialized resolve target. This
    // is ELIGIBILITY, not engagement: a view sitting at scale 1.0 is still
    // connected. When false the lever does nothing, so moving it would produce
    // pure quality loss with no measurable response to learn from.
    bool ScaleIsConnected = true;

    bool Valid = false;
};

class DynamicResolutionController
{
public:
    struct Stats
    {
        float SmoothedGpuMs = 0.0f;
        float LastRequestedScale = 1.0f; // pre-quantize, pre-dwell
        float DwellRemainingSeconds = 0.0f;
        // Update() calls accepted by the caller's once-per-app-frame dedup.
        // Ticks/app-frames must be ~1: above 1 means multiple windows are
        // each ticking the controller (dwell/slew/EMA run at N× real time).
        uint64 Ticks = 0;
        uint32 AcceptedDrops = 0;
        uint32 AcceptedRises = 0;
        // Target is unreachable: sitting at MinScale and still over budget, so
        // the unscalable cost alone exceeds the target. Surfaces as a
        // warn-once rather than an invisible permanent floor.
        bool SaturatedLow = false;
        bool SaturatedHigh = false;

        // Measured response of cost to the scale lever.
        bool ScaleDisconnected = false;     // no view can apply the lever (TAA off, letterboxed, ...)
        bool ResponseFitValid = false;      // enough spread in observed scales to solve
        bool ScalingIneffective = false;    // latched: the lever buys too little to be worth it
        float EstimatedUnscalableMs = 0.0f; // fitted U
        float EstimatedFullRangeSavingMs = 0.0f; // fitted saving from MaxScale to MinScale
    };

    void Configure(const DynamicResolutionConfig& config);
    const DynamicResolutionConfig& Config() const { return m_Config; }

    // Drop all adaptation state and restart at `scale`. Called when the mode
    // changes or the view resizes — the cost history is meaningless across
    // either.
    void Reset(float scale);

    // Advance one frame. Returns the scale to apply (== Scale()).
    float Update(const DynamicResolutionSample& sample, float deltaSeconds);

    float Scale() const { return m_Scale; }
    const Stats& GetStats() const { return m_Stats; }

    // Snap `scale` to the configured ladder, clamped to [MinScale, MaxScale].
    // Always rounds toward MinScale so quantization can only ever cost
    // performance headroom, never spend headroom the measurement did not show.
    float QuantizeScale(float scale) const;

private:
    // Exponentially-weighted least squares of cost against x = scale^2, giving
    // cost ~= U + S*x. U is the resolution-independent cost (post-TAA chain,
    // UI, shadows, and any geometry-bound work in the world half); S is what
    // the lever actually controls. Solvable only once the observed scales have
    // spread — until then the controller has not moved enough to have measured
    // anything, and the damped ratio law does the probing.
    struct ResponseFit
    {
        double N = 0.0;
        double SumX = 0.0;
        double SumY = 0.0;
        double SumXX = 0.0;
        double SumXY = 0.0;

        void Decay(double factor);
        void Observe(double x, double y);
        bool Solve(float& outUnscalableMs, float& outScalableAtFullMs) const;
    };

    DynamicResolutionConfig m_Config{};
    Stats m_Stats{};
    float m_Scale = 1.0f;
    float m_SmoothedGpuMs = 0.0f;
    float m_DwellRemaining = 0.0f;
    // Slew budget accrues over time since the last accepted change rather than
    // per frame, so a rung wider than one frame's budget is still reachable.
    float m_SecondsSinceChange = 0.0f;
    bool m_HasSample = false;

    ResponseFit m_Fit{};
    bool m_ObservedThisPosition = false;
    bool m_ScalingIneffective = false;
    float m_IneffectiveLatchCostMs = 0.0f;
};

} // namespace GameEngine::Engine::Renderer
