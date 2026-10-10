// Control-law tests for dynamic resolution scaling. The controller is a pure
// function of (cost sample, dt), which is what lets the stability properties be
// pinned here instead of only being observable on a GPU: every test below drives
// it against a closed-loop synthetic cost model where changing the scale changes
// the very cost being measured — the feedback path that makes DRS oscillate when
// it is built wrong.

#include "Engine/Rendering/DynamicResolutionController.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using GameEngine::Engine::Renderer::DynamicResolutionConfig;
using GameEngine::Engine::Renderer::DynamicResolutionController;
using GameEngine::Engine::Renderer::DynamicResolutionSample;

namespace
{
constexpr float kDt = 1.0f / 60.0f;

// cost(s) = Unscalable + ScalableAtFull * s^2. The square is the point: the
// lever moves internal pixel AREA, and the unscalable term is the post-TAA
// chain / UI / shadow raster that render at fixed resolution no matter what
// the scale does.
struct SyntheticGpu
{
    float Unscalable = 4.0f;
    float ScalableAtFull = 12.0f;

    float Cost(float scale) const { return Unscalable + ScalableAtFull * scale * scale; }
};

DynamicResolutionSample MakeSample(float gpuMs, float unscalableMs = 0.0f)
{
    DynamicResolutionSample s{};
    s.GpuMs = gpuMs;
    s.UnscalableGpuMs = unscalableMs;
    s.Valid = true;
    return s;
}

// Mechanics config: deadband / slew / dwell / quantize behaviour. Response
// detection is OFF here because most of these tests feed a cost that does not
// depend on the scale, and a cost that ignores the lever is by definition a
// dead lever — the detector would (correctly) hand the resolution back and mask
// the mechanic under test. The detector has its own tests, which use
// ResponseConfig() and closed-loop models.
DynamicResolutionConfig TestConfig()
{
    DynamicResolutionConfig c{};
    c.TargetGpuMs = 10.0f;
    c.DwellSeconds = 0.10f;
    c.MinUsefulSavingFraction = 0.0f;
    return c;
}

// Production shape: the response detector armed at its default threshold.
DynamicResolutionConfig ResponseConfig()
{
    DynamicResolutionConfig c = TestConfig();
    c.MinUsefulSavingFraction = DynamicResolutionConfig{}.MinUsefulSavingFraction;
    return c;
}

// The first accepted change cannot land until the minimum hold has elapsed
// (a position has to be measurable, and each change reallocates pool targets).
// Drive frames until it lands and return the resulting scale.
float DriveUntilFirstChange(DynamicResolutionController& c, float costMs, int maxFrames = 240)
{
    const float start = c.Scale();
    for (int i = 0; i < maxFrames; ++i)
    {
        c.Update(MakeSample(costMs), kDt);
        if (c.Scale() != start)
            break;
    }
    return c.Scale();
}

// Run the closed loop: measure the model at the current scale, feed it back.
// Returns the scale trace.
std::vector<float> RunClosedLoop(DynamicResolutionController& c, const SyntheticGpu& model,
                                 int frames, bool provideUnscalableHint = false)
{
    std::vector<float> trace;
    trace.reserve(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
    {
        const float cost = model.Cost(c.Scale());
        c.Update(MakeSample(cost, provideUnscalableHint ? model.Unscalable : 0.0f), kDt);
        trace.push_back(c.Scale());
    }
    return trace;
}

int CountChanges(const std::vector<float>& trace, size_t from)
{
    int changes = 0;
    for (size_t i = from + 1; i < trace.size(); ++i)
        if (trace[i] != trace[i - 1])
            ++changes;
    return changes;
}
} // namespace

// ── Mode setting + migration ───────────────────────────────────────────────

TEST(DynamicResolutionControllerTests, ModeTokensRoundTrip)
{
    using GameEngine::Engine::Renderer::DynamicResolutionMode;
    using GameEngine::Engine::Renderer::ResolveStartupDynamicResolutionMode;
    using GameEngine::Engine::Renderer::ToString;

    for (auto mode : {DynamicResolutionMode::Off, DynamicResolutionMode::Fixed,
                      DynamicResolutionMode::Dynamic})
        EXPECT_EQ(ResolveStartupDynamicResolutionMode(ToString(mode), 1.0f), mode);

    // An unrecognised token falls through to the migration rule rather than
    // asserting — a hand-edited settings file must not brick the renderer.
    EXPECT_EQ(ResolveStartupDynamicResolutionMode("nonsense", 1.0f), DynamicResolutionMode::Off);
}

TEST(DynamicResolutionControllerTests, ExistingProjectWithReducedScaleMigratesToFixed)
{
    using GameEngine::Engine::Renderer::DynamicResolutionMode;
    using GameEngine::Engine::Renderer::ResolveStartupDynamicResolutionMode;

    // A project saved before the mode setting existed carries only
    // rendering.taaRenderScale. A sub-1.0 value there is a deliberate user
    // choice and must keep behaving exactly as it does today: Fixed.
    EXPECT_EQ(ResolveStartupDynamicResolutionMode("", 0.76f), DynamicResolutionMode::Fixed)
        << "a persisted reduced scale must not silently become native";
    EXPECT_EQ(ResolveStartupDynamicResolutionMode("", 0.5f), DynamicResolutionMode::Fixed);

    // ...and must never be silently opted into the unevaluated dynamic path.
    EXPECT_NE(ResolveStartupDynamicResolutionMode("", 0.76f), DynamicResolutionMode::Dynamic);

    // A project at native stays native.
    EXPECT_EQ(ResolveStartupDynamicResolutionMode("", 1.0f), DynamicResolutionMode::Off);
    // The same threshold the pipeline pre-pass uses to engage TAAU at all.
    EXPECT_EQ(ResolveStartupDynamicResolutionMode("", 0.999f), DynamicResolutionMode::Off);

    // Garbage scale values must not produce a reduced-resolution mode.
    EXPECT_EQ(ResolveStartupDynamicResolutionMode("", 0.0f), DynamicResolutionMode::Off);
    EXPECT_EQ(ResolveStartupDynamicResolutionMode("", std::nanf("")), DynamicResolutionMode::Off);

    // An explicit mode always wins over the migration heuristic.
    EXPECT_EQ(ResolveStartupDynamicResolutionMode("off", 0.76f), DynamicResolutionMode::Off);
}

// ── Applied vs requested scale ─────────────────────────────────────────────

TEST(DynamicResolutionControllerTests, DisconnectedLeverHoldsAndLearnsNothing)
{
    // No TAA-eligible view: the render scale is wired to nothing. Moving it
    // would be pure quality loss, and any cost change seen meanwhile belongs to
    // something else — so hold, and do not let those samples teach the response
    // fit that scaling "does not work".
    DynamicResolutionConfig cfg = ResponseConfig();
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(cfg.MaxScale);

    for (int i = 0; i < 600; ++i)
    {
        DynamicResolutionSample s = MakeSample(40.0f); // far over a 10 ms target
        s.ScaleIsConnected = false;
        c.Update(s, kDt);
    }
    EXPECT_FLOAT_EQ(c.Scale(), cfg.MaxScale) << "must not move a lever that is not connected";
    EXPECT_TRUE(c.GetStats().ScaleDisconnected);
    EXPECT_FALSE(c.GetStats().ResponseFitValid) << "disconnected frames must not feed the fit";
    EXPECT_FALSE(c.GetStats().ScalingIneffective)
        << "'no TAA view' is not the same finding as 'scaling does not help here'";

    // Reconnecting must let it work normally again.
    const SyntheticGpu fillBound{3.0f, 24.0f};
    RunClosedLoop(c, fillBound, 1500);
    EXPECT_LT(c.Scale(), cfg.MaxScale);
    EXPECT_FALSE(c.GetStats().ScaleDisconnected);
}

TEST(DynamicResolutionControllerTests, ResponseFitUsesAppliedScaleNotRequested)
{
    // The pipeline snaps the internal extent to even pixels and only splits
    // when TAA is active, so the applied ratio is not the requested scale. The
    // fit must model the resolution actually rendered. Here the renderer
    // applies HALF of whatever is requested; a fit against the request would
    // recover a wrong U, a fit against the applied ratio recovers the right one.
    const SyntheticGpu model{6.0f, 20.0f};
    DynamicResolutionConfig cfg = ResponseConfig();
    // The fit only conditions once the controller has had reason to visit a
    // spread of scales -- a loop that converges in two steps never measures a
    // slope, which is a real property of the design and not a test artifact.
    // A target just above the 6 ms unscalable floor keeps it sweeping the whole
    // range, which is also exactly the situation where the fit matters.
    cfg.TargetGpuMs = 6.5f;
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(cfg.MaxScale);

    for (int i = 0; i < 2000; ++i)
    {
        const float applied = c.Scale() * 0.5f;
        DynamicResolutionSample s = MakeSample(model.Cost(applied));
        s.AppliedScale = applied;
        c.Update(s, kDt);
    }
    ASSERT_TRUE(c.GetStats().ResponseFitValid);
    EXPECT_NEAR(c.GetStats().EstimatedUnscalableMs, model.Unscalable, 1.5f)
        << "fitted U = " << c.GetStats().EstimatedUnscalableMs
        << " — the fit modelled the requested scale, not the rendered one";
}

// ── Basic response ─────────────────────────────────────────────────────────

TEST(DynamicResolutionControllerTests, InvalidSampleHoldsScale)
{
    DynamicResolutionController c;
    c.Configure(TestConfig());
    c.Reset(0.8f);

    DynamicResolutionSample none{};
    none.Valid = false;
    EXPECT_FLOAT_EQ(c.Update(none, kDt), 0.8f);

    // The measurement resolves several frames late and can legitimately drop
    // out; a NaN or non-positive reading must hold, never be acted on.
    EXPECT_FLOAT_EQ(c.Update(MakeSample(std::nanf("")), kDt), 0.8f);
    EXPECT_FLOAT_EQ(c.Update(MakeSample(0.0f), kDt), 0.8f);
    EXPECT_FLOAT_EQ(c.Update(MakeSample(-3.0f), kDt), 0.8f);
}

TEST(DynamicResolutionControllerTests, DeadbandSuppressesChatter)
{
    DynamicResolutionConfig cfg = TestConfig();
    cfg.DeadbandFraction = 0.10f;
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(0.9f);

    // Anything within +/-10% of the 10 ms target is "on target".
    for (int i = 0; i < 200; ++i)
    {
        const float wobble = (i % 2 == 0) ? 10.9f : 9.1f;
        EXPECT_FLOAT_EQ(c.Update(MakeSample(wobble), kDt), 0.9f)
            << "in-deadband cost must never move the ladder (frame " << i << ")";
    }
}

TEST(DynamicResolutionControllerTests, OverBudgetDropsAndUnderBudgetRises)
{
    DynamicResolutionController c;
    c.Configure(TestConfig());
    c.Reset(0.9f);

    for (int i = 0; i < 60; ++i)
        c.Update(MakeSample(20.0f), kDt);
    EXPECT_LT(c.Scale(), 0.9f) << "sustained over-budget must reduce the scale";
    const float dropped = c.Scale();

    for (int i = 0; i < 600; ++i)
        c.Update(MakeSample(3.0f), kDt);
    EXPECT_GT(c.Scale(), dropped) << "sustained headroom must give resolution back";
}

TEST(DynamicResolutionControllerTests, ResponseIsAsymmetric)
{
    // Same relative error in both directions; the drop must travel further per
    // unit time than the rise. Protecting frame rate is urgent, spending
    // headroom is not.
    DynamicResolutionConfig cfg = TestConfig();
    cfg.DwellSeconds = 0.0f;
    cfg.QuantizeStep = 0.001f;

    DynamicResolutionController down;
    down.Configure(cfg);
    down.Reset(0.75f);
    for (int i = 0; i < 30; ++i)
        down.Update(MakeSample(20.0f), kDt);

    DynamicResolutionController up;
    up.Configure(cfg);
    up.Reset(0.75f);
    for (int i = 0; i < 30; ++i)
        up.Update(MakeSample(5.0f), kDt);

    const float dropTravel = 0.75f - down.Scale();
    const float riseTravel = up.Scale() - 0.75f;
    EXPECT_GT(dropTravel, riseTravel * 2.0f)
        << "drop " << dropTravel << " vs rise " << riseTravel;
}

// ── Limits ─────────────────────────────────────────────────────────────────

TEST(DynamicResolutionControllerTests, RateLimitBoundsPerSecondTravel)
{
    DynamicResolutionConfig cfg = TestConfig();
    cfg.DwellSeconds = 0.0f;
    cfg.QuantizeStep = 0.001f;
    cfg.MaxDropPerSecond = 0.5f;
    cfg.MaxRisePerSecond = 0.1f;
    cfg.EmergencyFraction = 1000.0f; // keep the dwell path out of this test

    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(1.0f);

    // The contract is travel per SECOND, not per frame: the slew budget accrues
    // between accepted changes so that a ladder rung wider than one frame's
    // budget is still reachable. One second of catastrophic over-budget must
    // therefore stay within one second of budget (plus at most the rung the
    // last accepted change landed on).
    const float dropStart = c.Scale();
    for (int i = 0; i < 60; ++i)
        c.Update(MakeSample(200.0f), kDt);
    const float dropped = dropStart - c.Scale();
    EXPECT_GT(dropped, 0.0f) << "a catastrophic frame must actually move the scale";
    EXPECT_LE(dropped, cfg.MaxDropPerSecond * 1.0f + cfg.QuantizeStep + 1e-4f)
        << "one second of drop exceeded one second of slew budget";

    c.Reset(0.5f);
    const float riseStart = c.Scale();
    for (int i = 0; i < 60; ++i)
        c.Update(MakeSample(0.5f), kDt);
    const float risen = c.Scale() - riseStart;
    EXPECT_GT(risen, 0.0f) << "a rung wider than one frame's budget must still be reachable";
    EXPECT_LE(risen, cfg.MaxRisePerSecond * 1.0f + cfg.QuantizeStep + 1e-4f)
        << "one second of rise exceeded one second of slew budget";
}

TEST(DynamicResolutionControllerTests, ClampsToConfiguredRange)
{
    DynamicResolutionConfig cfg = TestConfig();
    cfg.DwellSeconds = 0.0f;
    cfg.MinScale = 0.6f;
    cfg.MaxScale = 0.9f;
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(1.0f);
    EXPECT_FLOAT_EQ(c.Scale(), 0.9f) << "Reset must clamp into range";

    for (int i = 0; i < 2000; ++i)
        c.Update(MakeSample(500.0f), kDt);
    EXPECT_FLOAT_EQ(c.Scale(), 0.6f);

    for (int i = 0; i < 4000; ++i)
        c.Update(MakeSample(0.1f), kDt);
    EXPECT_FLOAT_EQ(c.Scale(), 0.9f);
}

TEST(DynamicResolutionControllerTests, ConfigureSanitizesHostileValues)
{
    DynamicResolutionConfig cfg{};
    cfg.TargetGpuMs = std::nanf("");
    cfg.MinScale = -5.0f;
    cfg.MaxScale = 42.0f;
    cfg.QuantizeStep = 0.0f;
    cfg.SampleSmoothing = 0.0f;
    cfg.DeadbandFraction = 5.0f;

    DynamicResolutionController c;
    c.Configure(cfg);
    const auto& out = c.Config();
    EXPECT_TRUE(std::isfinite(out.TargetGpuMs) && out.TargetGpuMs > 0.0f);
    EXPECT_GE(out.MinScale, 0.5f);
    EXPECT_LE(out.MaxScale, 1.0f);
    EXPECT_GE(out.MaxScale, out.MinScale);
    EXPECT_GT(out.QuantizeStep, 0.0f);
    EXPECT_GT(out.SampleSmoothing, 0.0f);
    EXPECT_LE(out.DeadbandFraction, 0.9f);
}

// ── Quantization ───────────────────────────────────────────────────────────

TEST(DynamicResolutionControllerTests, QuantizeLandsOnLadderAndKeepsMaxReachable)
{
    DynamicResolutionConfig cfg = TestConfig();
    cfg.MinScale = 0.5f;
    cfg.MaxScale = 1.0f;
    cfg.QuantizeStep = 0.03f; // deliberately not a divisor of the range
    DynamicResolutionController c;
    c.Configure(cfg);

    EXPECT_FLOAT_EQ(c.QuantizeScale(1.0f), 1.0f) << "the top rung must be exactly reachable";
    EXPECT_FLOAT_EQ(c.QuantizeScale(2.0f), 1.0f);
    EXPECT_FLOAT_EQ(c.QuantizeScale(0.0f), 0.5f);

    // Round to nearest: never further than half a rung from the request.
    for (float s = 0.5f; s <= 1.0f; s += 0.007f)
    {
        const float q = c.QuantizeScale(s);
        EXPECT_GE(q, cfg.MinScale - 1e-6f);
        EXPECT_LE(q, cfg.MaxScale + 1e-6f);
        EXPECT_LE(std::fabs(q - s), cfg.QuantizeStep * 0.5f + 1e-4f)
            << "quantized " << s << " to " << q << " — further than half a rung";

        // Every rung is MaxScale - n*step. The clamped endpoints are the two
        // deliberate exceptions: MinScale need not fall on the ladder.
        if (q > cfg.MinScale + 1e-6f && q < cfg.MaxScale - 1e-6f)
        {
            const float rungs = (cfg.MaxScale - q) / cfg.QuantizeStep;
            EXPECT_LT(std::fabs(rungs - std::round(rungs)), 1e-3f)
                << "quantized " << q << " is off the ladder";
        }
    }
}

TEST(DynamicResolutionControllerTests, ScaleAlwaysSitsOnALadderRung)
{
    // Every extent the pool ever sees comes from a rung, which is what bounds
    // the number of distinct render-target sizes it has to allocate.
    DynamicResolutionConfig cfg = TestConfig();
    cfg.QuantizeStep = 0.05f;
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(1.0f);

    SyntheticGpu model{3.0f, 14.0f};
    for (int i = 0; i < 400; ++i)
    {
        model.ScalableAtFull = 6.0f + 14.0f * static_cast<float>((i / 40) % 3);
        c.Update(MakeSample(model.Cost(c.Scale())), kDt);
        if (c.Scale() <= cfg.MinScale + 1e-6f)
            continue; // the clamped floor is a deliberate off-ladder endpoint
        const float rungs = (cfg.MaxScale - c.Scale()) / cfg.QuantizeStep;
        EXPECT_LT(std::fabs(rungs - std::round(rungs)), 1e-3f) << "off-ladder scale " << c.Scale();
    }
}

// ── Dwell ──────────────────────────────────────────────────────────────────

TEST(DynamicResolutionControllerTests, DwellRateLimitsAcceptedChanges)
{
    // Each accepted change reallocates ~20 render-extent pool textures, so the
    // dwell timer is a hard budget on how often that can happen.
    DynamicResolutionConfig cfg = TestConfig();
    cfg.DwellSeconds = 0.5f;
    cfg.EmergencyFraction = 1000.0f;
    cfg.QuantizeStep = 0.01f;
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(1.0f);

    std::vector<float> trace;
    for (int i = 0; i < 600; ++i) // 10 seconds
    {
        c.Update(MakeSample(14.0f), kDt);
        trace.push_back(c.Scale());
    }
    const int changes = CountChanges(trace, 0);
    EXPECT_LE(changes, 21) << "10 s at a 0.5 s dwell allows at most ~20 changes, saw " << changes;
    EXPECT_GT(changes, 0);
}

TEST(DynamicResolutionControllerTests, EmergencyDropBypassesDwellButNotRateLimit)
{
    DynamicResolutionConfig cfg = TestConfig();
    cfg.DwellSeconds = 5.0f; // effectively blocks ordinary changes
    cfg.EmergencyFraction = 1.5f;
    cfg.QuantizeStep = 0.01f;
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(1.0f);

    // Mild over-budget: dwell holds after the first accepted change.
    const float afterFirst = DriveUntilFirstChange(c, 12.0f);
    ASSERT_LT(afterFirst, 1.0f);
    for (int i = 0; i < 120; ++i) // 2 s, well inside the 5 s dwell
        c.Update(MakeSample(12.0f), kDt);
    EXPECT_FLOAT_EQ(c.Scale(), afterFirst) << "non-emergency changes must respect dwell";

    // A spike past 1.5x target must get through immediately.
    const float spikeStart = c.Scale();
    for (int i = 0; i < 30; ++i)
        c.Update(MakeSample(40.0f), kDt);
    EXPECT_LT(c.Scale(), afterFirst) << "an emergency spike must move the scale despite dwell";
    EXPECT_LE(spikeStart - c.Scale(),
              cfg.MaxDropPerSecond * (30.0f * kDt) + cfg.QuantizeStep + 1e-4f)
        << "emergency must bypass dwell, never the slew limit";
}

TEST(DynamicResolutionControllerTests, EmergencyNeverBypassesDwellUpward)
{
    // A premature rise is what turns a settled loop into an oscillating one.
    DynamicResolutionConfig cfg = TestConfig();
    cfg.DwellSeconds = 1.0f;
    cfg.QuantizeStep = 0.01f;
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(0.7f);

    const float afterFirst = DriveUntilFirstChange(c, 1.0f); // arms the dwell
    ASSERT_GT(afterFirst, 0.7f);
    for (int i = 0; i < 50; ++i) // 0.83 s, inside the 1 s dwell
        c.Update(MakeSample(1.0f), kDt);
    EXPECT_FLOAT_EQ(c.Scale(), afterFirst) << "rises must always wait out the dwell";
}

// ── Closed-loop stability (the reason the controller is shaped this way) ────

TEST(DynamicResolutionControllerTests, ConvergesAndSettlesUnderSyntheticCostModel)
{
    const SyntheticGpu model{4.0f, 24.0f}; // 28 ms at 1.0, 10 ms target
    DynamicResolutionConfig cfg = TestConfig();
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(1.0f);

    const std::vector<float> trace = RunClosedLoop(c, model, 900);

    const float settled = model.Cost(c.Scale());
    EXPECT_LE(std::fabs(settled - cfg.TargetGpuMs), cfg.TargetGpuMs * cfg.DeadbandFraction * 1.6f)
        << "settled at " << settled << " ms (scale " << c.Scale() << ")";

    // No limit cycle: once converged, the ladder must stop moving.
    EXPECT_LE(CountChanges(trace, 600), 2) << "controller is hunting instead of settling";
}

TEST(DynamicResolutionControllerTests, ConvergesFromBelowWithoutOvershooting)
{
    const SyntheticGpu model{2.0f, 5.0f}; // only 7 ms at 1.0 — plenty of headroom
    DynamicResolutionConfig cfg = TestConfig();
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(0.5f);

    const std::vector<float> trace = RunClosedLoop(c, model, 1800);
    EXPECT_FLOAT_EQ(c.Scale(), cfg.MaxScale) << "cheap scene must return to full resolution";

    // Monotone climb: the rise path must never overshoot and bounce back down.
    for (size_t i = 1; i < trace.size(); ++i)
        ASSERT_GE(trace[i], trace[i - 1] - 1e-6f)
            << "rise path reversed at frame " << i << " (" << trace[i - 1] << " -> " << trace[i]
            << ")";
}

TEST(DynamicResolutionControllerTests, ResistsOscillationUnderNoisyMeasurements)
{
    // The measurement is a real GPU timing with real jitter. Smoothing plus the
    // deadband must absorb it rather than translating it into ladder churn.
    const SyntheticGpu model{4.0f, 24.0f};
    DynamicResolutionConfig cfg = TestConfig();
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(1.0f);

    uint32_t rng = 0x1234567u;
    auto noise = [&rng]() -> float
    {
        rng = rng * 1664525u + 1013904223u;
        const float u = static_cast<float>((rng >> 8) & 0xFFFFu) / 65535.0f; // [0,1]
        return 0.90f + 0.20f * u;                                            // +/-10%
    };

    std::vector<float> trace;
    for (int i = 0; i < 1800; ++i)
    {
        c.Update(MakeSample(model.Cost(c.Scale()) * noise()), kDt);
        trace.push_back(c.Scale());
    }

    const int lateChanges = CountChanges(trace, 1200);
    EXPECT_LE(lateChanges, 6) << "+/-10% measurement noise produced " << lateChanges
                              << " late ladder changes — the loop is chattering";

    const float settled = model.Cost(c.Scale());
    EXPECT_LE(std::fabs(settled - cfg.TargetGpuMs), cfg.TargetGpuMs * 0.30f)
        << "noisy loop settled at " << settled << " ms";
}

TEST(DynamicResolutionControllerTests, UnscalableHintConvergesFasterThanBlindLoop)
{
    // Without the hint the ratio law under-corrects by the unscalable residual
    // and has to walk in; with it, the first correction is nearly exact. Both
    // must converge — the hint is an accuracy improvement, not a correctness
    // requirement.
    // 20 ms at 1.0 with 4 ms fixed. Solving 4 + 16s^2 = 10 puts the answer at
    // s = 0.61 — comfortably inside [0.5, 1.0], so the target is reachable and
    // the two paths differ only in how fast they get there.
    const SyntheticGpu model{4.0f, 16.0f};
    DynamicResolutionConfig cfg = TestConfig();
    cfg.DwellSeconds = 0.0f;
    cfg.QuantizeStep = 0.005f;

    auto framesToConverge = [&](bool hint) -> int
    {
        DynamicResolutionController c;
        c.Configure(cfg);
        c.Reset(1.0f);
        for (int i = 0; i < 3000; ++i)
        {
            const float cost = model.Cost(c.Scale());
            c.Update(MakeSample(cost, hint ? model.Unscalable : 0.0f), kDt);
            if (std::fabs(model.Cost(c.Scale()) - cfg.TargetGpuMs) <=
                cfg.TargetGpuMs * cfg.DeadbandFraction)
                return i;
        }
        return -1;
    };

    const int blind = framesToConverge(false);
    const int hinted = framesToConverge(true);
    ASSERT_GT(blind, 0);
    ASSERT_GT(hinted, 0);
    EXPECT_LE(hinted, blind) << "hinted " << hinted << " vs blind " << blind;
}

TEST(DynamicResolutionControllerTests, UnreachableTargetSaturatesAndFlags)
{
    // The unscalable cost alone busts the budget: no scale can hit the target.
    // The controller must park at the floor and say so, not thrash forever.
    const SyntheticGpu model{18.0f, 20.0f};
    DynamicResolutionConfig cfg = ResponseConfig(); // 10 ms target
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(1.0f);

    const std::vector<float> trace = RunClosedLoop(c, model, 900, /*hint=*/true);
    EXPECT_FLOAT_EQ(c.Scale(), cfg.MinScale);
    EXPECT_TRUE(c.GetStats().SaturatedLow)
        << "an unreachable target must surface, not sit as an invisible floor";
    EXPECT_EQ(CountChanges(trace, 600), 0) << "saturated loop must be quiet";
}

TEST(DynamicResolutionControllerTests, TracksAStepChangeInSceneCost)
{
    // Walking from a cheap vista into an expensive town and back: the classic
    // DRS workload. Both transitions must settle, and the expensive leg must
    // sit at a lower scale than the cheap one.
    DynamicResolutionConfig cfg = TestConfig();
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(1.0f);

    SyntheticGpu cheap{3.0f, 6.0f};  // 9 ms at 1.0 — under a 10 ms target
    SyntheticGpu heavy{3.0f, 24.0f}; // 27 ms at 1.0; target lands at s = 0.54

    RunClosedLoop(c, cheap, 600);
    const float cheapScale = c.Scale();
    EXPECT_FLOAT_EQ(cheapScale, cfg.MaxScale);

    RunClosedLoop(c, heavy, 900);
    const float heavyScale = c.Scale();
    EXPECT_LT(heavyScale, cheapScale);
    EXPECT_LE(std::fabs(heavy.Cost(heavyScale) - cfg.TargetGpuMs),
              cfg.TargetGpuMs * cfg.DeadbandFraction * 1.6f);

    const std::vector<float> back = RunClosedLoop(c, cheap, 1800);
    EXPECT_FLOAT_EQ(c.Scale(), cfg.MaxScale) << "must recover full resolution when the load lifts";
    EXPECT_LE(CountChanges(back, 1500), 2);
}

// ── Measured response / geometry-bound content ─────────────────────────────

// The real-world case on this project: the town scene is geometry-bound, so
// DepthPrepass + RenderEntities dominate and barely respond to render scale —
// 0.76 measured as no useful win. A whole-frame controller with no response
// model grinds to MinScale, halves the resolution, saves nothing, and stays
// there. It must instead measure that the lever is dead and hand the pixels
// back. Note that classifying passes as "pre-TAA therefore scalable" would not
// catch this: those passes ARE the scalable set, they simply do not respond.
TEST(DynamicResolutionControllerTests, GeometryBoundContentReturnsToNativeInsteadOfGrindingDown)
{
    const SyntheticGpu geometryBound{19.0f, 1.0f}; // 20 ms at 1.0, 19.25 ms at 0.5
    DynamicResolutionConfig cfg = ResponseConfig();    // 10 ms target — unreachable
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(cfg.MaxScale);

    const std::vector<float> trace = RunClosedLoop(c, geometryBound, 3000);

    EXPECT_TRUE(c.GetStats().ScalingIneffective)
        << "the controller must notice the lever does nothing here";
    EXPECT_FLOAT_EQ(c.Scale(), cfg.MaxScale)
        << "settled at " << c.Scale() << " — resolution spent for no measurable saving";
    EXPECT_FALSE(c.GetStats().SaturatedLow)
        << "'stuck at the floor' is the wrong diagnosis for dead-lever content";

    // It must probe — that is how the response gets measured — but not live down there.
    const float lowest = *std::min_element(trace.begin(), trace.end());
    EXPECT_LT(lowest, cfg.MaxScale) << "never probed, so the verdict was not measured";
    EXPECT_EQ(CountChanges(trace, 2400), 0) << "must be quiet once it has given up";
}

// The contrast case: an expensive, unreachable target where the lever DOES
// work. Dropping is still right there — it cannot reach target but it buys real
// milliseconds — so the ineffective verdict must not swallow it.
TEST(DynamicResolutionControllerTests, FillBoundContentStillDropsWhenTargetUnreachable)
{
    const SyntheticGpu fillBound{18.0f, 20.0f}; // 38 ms at 1.0, 23 ms at 0.5
    DynamicResolutionConfig cfg = ResponseConfig();
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(cfg.MaxScale);

    RunClosedLoop(c, fillBound, 1200);
    EXPECT_FALSE(c.GetStats().ScalingIneffective)
        << "a 15 ms full-range saving is not 'ineffective'";
    EXPECT_FLOAT_EQ(c.Scale(), cfg.MinScale);
    EXPECT_TRUE(c.GetStats().SaturatedLow);
}

TEST(DynamicResolutionControllerTests, ResponseFitRecoversTheUnscalableComponent)
{
    const SyntheticGpu model{8.0f, 16.0f}; // U = 8 ms
    DynamicResolutionConfig cfg = ResponseConfig();
    cfg.TargetGpuMs = 14.0f; // reachable at s = 0.61
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(cfg.MaxScale);

    RunClosedLoop(c, model, 1500);
    ASSERT_TRUE(c.GetStats().ResponseFitValid);
    EXPECT_NEAR(c.GetStats().EstimatedUnscalableMs, model.Unscalable, 1.5f)
        << "fitted U = " << c.GetStats().EstimatedUnscalableMs;
    EXPECT_NEAR(c.GetStats().EstimatedFullRangeSavingMs,
                model.ScalableAtFull * (1.0f - 0.25f), 3.0f);
}

TEST(DynamicResolutionControllerTests, IneffectiveVerdictIsRevisitedWhenContentChanges)
{
    // Give up on a geometry-bound frame, then walk into fill-bound content: the
    // latch must clear so DRS starts working again. Without the latch it would
    // re-probe forever; without the clear it would never work again.
    DynamicResolutionConfig cfg = ResponseConfig();
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(cfg.MaxScale);

    const SyntheticGpu geometryBound{19.0f, 1.0f};
    RunClosedLoop(c, geometryBound, 3000);
    ASSERT_TRUE(c.GetStats().ScalingIneffective);
    ASSERT_FLOAT_EQ(c.Scale(), cfg.MaxScale);

    const SyntheticGpu fillBound{3.0f, 24.0f}; // 27 ms at 1.0; target lands at 0.54
    RunClosedLoop(c, fillBound, 3000);
    EXPECT_FALSE(c.GetStats().ScalingIneffective)
        << "the verdict must not outlive the content it was measured on";
    EXPECT_LT(c.Scale(), cfg.MaxScale);
    EXPECT_LE(std::fabs(fillBound.Cost(c.Scale()) - cfg.TargetGpuMs),
              cfg.TargetGpuMs * cfg.DeadbandFraction * 1.6f);
}

TEST(DynamicResolutionControllerTests, ResetClearsAdaptationState)
{
    DynamicResolutionController c;
    c.Configure(TestConfig());
    c.Reset(1.0f);
    for (int i = 0; i < 300; ++i)
        c.Update(MakeSample(40.0f), kDt);
    EXPECT_LT(c.Scale(), 1.0f);
    EXPECT_GT(c.GetStats().AcceptedDrops, 0u);

    c.Reset(0.85f);
    EXPECT_FLOAT_EQ(c.Scale(), 0.85f);
    EXPECT_EQ(c.GetStats().AcceptedDrops, 0u);
    EXPECT_EQ(c.GetStats().AcceptedRises, 0u);
    EXPECT_FLOAT_EQ(c.GetStats().SmoothedGpuMs, 0.0f);

    // The smoothed cost must seed from the next sample, not blend against a
    // stale pre-reset value.
    c.Update(MakeSample(12.0f), kDt);
    EXPECT_FLOAT_EQ(c.GetStats().SmoothedGpuMs, 12.0f);
}

TEST(DynamicResolutionControllerTests, ZeroDeltaTimeCannotMoveTheScale)
{
    // A paused/stepped frame has dt == 0; the slew limit must collapse to zero
    // travel rather than dividing by it.
    DynamicResolutionConfig cfg = TestConfig();
    cfg.DwellSeconds = 0.0f;
    DynamicResolutionController c;
    c.Configure(cfg);
    c.Reset(0.8f);
    for (int i = 0; i < 100; ++i)
        EXPECT_FLOAT_EQ(c.Update(MakeSample(90.0f), 0.0f), 0.8f);
}
