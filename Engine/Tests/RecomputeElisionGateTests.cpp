// RecomputeElisionGateTests.cpp
// Covers the idle-recompute-elision core (Rendering::RecomputeElisionGate /
// ElisionInputBlob — exact-input skip decisions over consecutive identical
// frames) and the engine-side light-list version signal
// (RenderServices::FinalizeWorldLights memcmp bump). These are the pure CPU
// halves of frame-attribution lever #2; the GPU halves (dispatch skipping in
// GPUCullingPipeline::EndFrameRG / GPUDrawStreamBuilder::ScheduleUnifiedScatter)
// are runtime-verified in the editor.

#include <gtest/gtest.h>

#include "Engine/Rendering/RenderServices.h"
#include "Rendering/Core/RecomputeElision.h"

#include <cstdint>
#include <vector>

using GameEngine::Engine::Renderer::ExtractedLight;
using GameEngine::Engine::Renderer::RenderServices;
using GameEngine::Rendering::ElisionCause;
using GameEngine::Rendering::ElisionInputBlob;
using GameEngine::Rendering::ParseIdleElisionLoggingEnabled;
using GameEngine::Rendering::RecomputeElisionGate;
using LightType = GameEngine::Components::LightType;

namespace
{
ElisionInputBlob MakeBlob(uint64_t a, float b, uint32_t c)
{
    ElisionInputBlob blob;
    blob.Append(a);
    blob.Append(b);
    blob.Append(c);
    return blob;
}

ExtractedLight MakePoint(uint32_t sortId, float intensity)
{
    ExtractedLight l{};
    l.type = LightType::Point;
    l.SortId = sortId;
    l.intensity = intensity;
    l.range = 5.0f;
    return l;
}
} // namespace

TEST(ElisionInputBlob, EqualityIsByteExact)
{
    EXPECT_EQ(MakeBlob(1, 2.0f, 3), MakeBlob(1, 2.0f, 3));
    EXPECT_NE(MakeBlob(1, 2.0f, 3), MakeBlob(1, 2.0f, 4));
    EXPECT_NE(MakeBlob(1, 2.0f, 3), MakeBlob(2, 2.0f, 3));
    // Bitwise, not semantic: -0.0f != +0.0f in the blob (conservative — a
    // representation flip recomputes).
    EXPECT_NE(MakeBlob(1, 0.0f, 3), MakeBlob(1, -0.0f, 3));
    // Different lengths never compare equal.
    ElisionInputBlob shorter;
    shorter.Append(uint64_t{1});
    EXPECT_NE(shorter, MakeBlob(1, 2.0f, 3));
}

TEST(RecomputeElisionGate, SettleThenSkip)
{
    RecomputeElisionGate gate;
    constexpr uint32_t kSettle = 3;

    // Frame 1: first evaluate — baseline, never a skip.
    auto d = gate.Evaluate(1, MakeBlob(7, 1.0f, 9), true, kSettle);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, ElisionCause::FirstEvaluate);

    // Frames 2..3: identical inputs but not settled yet.
    d = gate.Evaluate(2, MakeBlob(7, 1.0f, 9), true, kSettle);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, ElisionCause::NotSettled);
    d = gate.Evaluate(3, MakeBlob(7, 1.0f, 9), true, kSettle);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, ElisionCause::NotSettled);

    // Frame 4: third consecutive match — skips from here on.
    d = gate.Evaluate(4, MakeBlob(7, 1.0f, 9), true, kSettle);
    EXPECT_TRUE(d.Skip);
    EXPECT_EQ(d.Cause, ElisionCause::Skipped);
    d = gate.Evaluate(5, MakeBlob(7, 1.0f, 9), true, kSettle);
    EXPECT_TRUE(d.Skip);

    EXPECT_EQ(gate.GetStats().Evaluated, 5u);
    EXPECT_EQ(gate.GetStats().Skipped, 2u);
}

TEST(RecomputeElisionGate, InputChangeWakesAndResettles)
{
    RecomputeElisionGate gate;
    constexpr uint32_t kSettle = 2;
    (void)gate.Evaluate(1, MakeBlob(1, 1.0f, 1), true, kSettle);
    (void)gate.Evaluate(2, MakeBlob(1, 1.0f, 1), true, kSettle);
    auto d = gate.Evaluate(3, MakeBlob(1, 1.0f, 1), true, kSettle);
    ASSERT_TRUE(d.Skip);

    // One changed byte disengages the SAME frame — no stale skip.
    d = gate.Evaluate(4, MakeBlob(1, 2.0f, 1), true, kSettle);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, ElisionCause::InputsChanged);

    // Re-settle takes the full window again.
    d = gate.Evaluate(5, MakeBlob(1, 2.0f, 1), true, kSettle);
    EXPECT_FALSE(d.Skip);
    d = gate.Evaluate(6, MakeBlob(1, 2.0f, 1), true, kSettle);
    EXPECT_TRUE(d.Skip);
}

TEST(RecomputeElisionGate, EvaluationGapNeverSkipsAcross)
{
    RecomputeElisionGate gate;
    constexpr uint32_t kSettle = 2;
    (void)gate.Evaluate(1, MakeBlob(1, 1.0f, 1), true, kSettle);
    (void)gate.Evaluate(2, MakeBlob(1, 1.0f, 1), true, kSettle);
    ASSERT_TRUE(gate.Evaluate(3, MakeBlob(1, 1.0f, 1), true, kSettle).Skip);

    // Stamp 5 (skipped 4): identical bytes but the gate was absent a frame —
    // its retained regions are untrusted, so equality must not skip.
    auto d = gate.Evaluate(5, MakeBlob(1, 1.0f, 1), true, kSettle);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, ElisionCause::EvaluationGap);

    // Consecutive evaluation resumes the settle count from zero.
    d = gate.Evaluate(6, MakeBlob(1, 1.0f, 1), true, kSettle);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, ElisionCause::NotSettled);
    d = gate.Evaluate(7, MakeBlob(1, 1.0f, 1), true, kSettle);
    EXPECT_TRUE(d.Skip);
}

TEST(RecomputeElisionGate, ForceRecomputeBreaksASettledRun)
{
    RecomputeElisionGate gate;
    constexpr uint32_t kSettle = 1;
    (void)gate.Evaluate(1, MakeBlob(1, 1.0f, 1), true, kSettle);
    ASSERT_TRUE(gate.Evaluate(2, MakeBlob(1, 1.0f, 1), true, kSettle).Skip);

    gate.ForceRecompute();
    auto d = gate.Evaluate(3, MakeBlob(1, 1.0f, 1), true, kSettle);
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, ElisionCause::Forced);

    // Force is one-shot and restarts the settle window.
    d = gate.Evaluate(4, MakeBlob(1, 1.0f, 1), true, kSettle);
    EXPECT_TRUE(d.Skip);
}

TEST(RecomputeElisionGate, DisabledCountsTheAchievableRate)
{
    RecomputeElisionGate gate;
    constexpr uint32_t kSettle = 1;
    (void)gate.Evaluate(1, MakeBlob(1, 1.0f, 1), /*enabled*/ false, kSettle);
    const auto d = gate.Evaluate(2, MakeBlob(1, 1.0f, 1), /*enabled*/ false, kSettle);
    // Never skips while disabled, but the would-be cause is still measured.
    EXPECT_FALSE(d.Skip);
    EXPECT_EQ(d.Cause, ElisionCause::Skipped);
    EXPECT_EQ(gate.GetStats().Skipped, 0u);
    EXPECT_EQ(gate.GetStats().CauseCounts[static_cast<size_t>(ElisionCause::Skipped)], 1u);
}

// ── Engine-side light-list version (FinalizeWorldLights memcmp bump) ──

TEST(WorldLightListVersion, BumpsOnFirstListAndOnlyOnChange)
{
    RenderServices rs;
    EXPECT_EQ(rs.WorldLightListVersion(0u), 0u);

    // One "frame": reset the world's lights, submit, finalize — the exact
    // per-frame extraction sequence.
    auto frame = [&rs](const std::vector<ExtractedLight>& lights)
    {
        rs.ResetWorldLights(0u);
        for (const ExtractedLight& l : lights)
            rs.SubmitLight(0u, l);
        rs.FinalizeWorldLights(0u);
    };

    // Frame 1: two lights — first finalized list bumps from the empty snapshot.
    frame({MakePoint(1, 5.0f), MakePoint(2, 1.0f)});
    EXPECT_EQ(rs.WorldLightListVersion(0u), 1u);

    // Frame 2: identical resubmission (extraction repeats at idle) — no bump.
    frame({MakePoint(1, 5.0f), MakePoint(2, 1.0f)});
    EXPECT_EQ(rs.WorldLightListVersion(0u), 1u);

    // Frame 3: an intensity edit — bump.
    frame({MakePoint(1, 5.0f), MakePoint(2, 2.0f)});
    EXPECT_EQ(rs.WorldLightListVersion(0u), 2u);

    // Frame 4: light removed (list shrank) — bump; emptied next — bump again.
    frame({MakePoint(1, 5.0f)});
    EXPECT_EQ(rs.WorldLightListVersion(0u), 3u);
    frame({});
    EXPECT_EQ(rs.WorldLightListVersion(0u), 4u);

    // Steady empty — no bump.
    frame({});
    EXPECT_EQ(rs.WorldLightListVersion(0u), 4u);
}

// The cause window exists to name the blocker of a run that never engages. A
// gate that skips everything it evaluates has no blocker, so it must stay quiet
// rather than reprint identical cumulative totals every window forever.
TEST(RecomputeElisionGate, WindowReportsOnlyWhenTheWindowLostElision)
{
    constexpr uint64_t kPeriod = 4;
    RecomputeElisionGate gate;
    uint64_t stamp = 0;
    auto step = [&](bool enabled)
    { return gate.Evaluate(++stamp, MakeBlob(1, 2.0f, 3), enabled, /*settleFrames=*/1); };

    // Window 1 carries the FirstEvaluate and the settle, so it has something to say.
    for (uint64_t i = 0; i < kPeriod - 1; ++i)
    {
        step(true);
        EXPECT_FALSE(gate.ShouldReportWindow(kPeriod)) << "reported mid-window at " << i;
    }
    step(true);
    EXPECT_TRUE(gate.ShouldReportWindow(kPeriod)) << "the window that first engaged must report";

    // Window 2 skips every evaluation: nothing to name.
    for (uint64_t i = 0; i < kPeriod - 1; ++i)
    {
        step(true);
        ASSERT_FALSE(gate.ShouldReportWindow(kPeriod));
    }
    step(true);
    EXPECT_FALSE(gate.ShouldReportWindow(kPeriod)) << "a fully skipped window has no blocker to name";

    // Window 3 loses elision on one evaluation — that is exactly the case the
    // window exists for, and the totals must come back.
    step(false);
    for (uint64_t i = 0; i < kPeriod - 2; ++i)
    {
        step(true);
        ASSERT_FALSE(gate.ShouldReportWindow(kPeriod));
    }
    step(true);
    EXPECT_TRUE(gate.ShouldReportWindow(kPeriod))
        << "a window that lost elision must report; silence there is the failure "
           "the window was added to prevent";
}

// A gate running with elision disabled never advances Skipped, so every window
// differs and the measuring run keeps reporting its achievable rate.
TEST(RecomputeElisionGate, DisabledGateKeepsReportingItsWindows)
{
    constexpr uint64_t kPeriod = 4;
    RecomputeElisionGate gate;
    uint64_t stamp = 0;
    for (int window = 0; window < 3; ++window)
    {
        for (uint64_t i = 0; i < kPeriod; ++i)
            gate.Evaluate(++stamp, MakeBlob(7, 8.0f, 9), /*enabled=*/false, /*settleFrames=*/1);
        EXPECT_TRUE(gate.ShouldReportWindow(kPeriod))
            << "measuring run went silent at window " << window;
    }
}

// `Skipped` is resolved independently of `enabled`, so a decision can carry it
// while Skip is false. Logging the raw cause there produced "disengaged:
// Skipped", which reads as its own opposite.
TEST(RecomputeElisionGate, DisengageReasonNamesTheDisabledStateRatherThanSkipped)
{
    using GameEngine::Rendering::ElisionDisengageReason;

    RecomputeElisionGate gate;
    uint64_t stamp = 0;
    auto step = [&](bool enabled)
    { return gate.Evaluate(++stamp, MakeBlob(4, 5.0f, 6), enabled, /*settleFrames=*/1); };

    const RecomputeElisionGate::Decision first = step(true);
    ASSERT_EQ(first.Cause, ElisionCause::FirstEvaluate);
    EXPECT_STREQ(ElisionDisengageReason(first.Cause, first.Skip), "FirstEvaluate")
        << "a real recompute cause must still be named verbatim";

    const RecomputeElisionGate::Decision engaged = step(true);
    ASSERT_TRUE(engaged.Skip);
    ASSERT_EQ(engaged.Cause, ElisionCause::Skipped);

    // Same settled inputs, elision now disabled: cause Skipped, Skip false.
    const RecomputeElisionGate::Decision disabled = step(false);
    ASSERT_FALSE(disabled.Skip);
    ASSERT_EQ(disabled.Cause, ElisionCause::Skipped);
    EXPECT_STREQ(ElisionDisengageReason(disabled.Cause, disabled.Skip),
                 "Disabled (inputs settled)")
        << "a disengage line must not report the cause 'Skipped' as its reason";
}

// GE_IDLE_ELISION_LOG is an opt-in diagnostic, so its polarity is the OPPOSITE
// of the GE_IDLE_ELISION kill switches next to it: unset means "do not log".
// A predicate copied from a kill switch reads every non-"0" string as on, which
// turns GE_IDLE_ELISION_LOG=false into a request FOR the logs.
TEST(RecomputeElisionGate, LoggingTogglePolicyIsOffByDefault)
{
    EXPECT_FALSE(ParseIdleElisionLoggingEnabled(nullptr));
    EXPECT_FALSE(ParseIdleElisionLoggingEnabled(""));
    EXPECT_FALSE(ParseIdleElisionLoggingEnabled("0"));
    EXPECT_FALSE(ParseIdleElisionLoggingEnabled("false"));
    EXPECT_FALSE(ParseIdleElisionLoggingEnabled("False"));
    EXPECT_TRUE(ParseIdleElisionLoggingEnabled("1"));
    EXPECT_TRUE(ParseIdleElisionLoggingEnabled("true"));
    EXPECT_TRUE(ParseIdleElisionLoggingEnabled("on"));
}
