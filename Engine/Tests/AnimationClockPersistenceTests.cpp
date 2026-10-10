// AnimationClockPersistenceTests.cpp
//
// Guards the water "bounce back" fix: the animated-material clocks (uTimeParams.x
// wind/general, uTimeParams.y water scroll) must derive from the process-global
// Time::GetCumulativeSeconds() rather than from RenderServices-resident
// accumulators. Because the clock lives outside the renderer, destroying and
// recreating a RenderServices (device-lost recovery, window/context rebuild) must
// NOT reset animation phase to zero — which previously snapped every animated UV
// back to t=0 and made water flow backward for a frame.
//
// Hermetic: the getters read the global clock and touch no GPU device, so these
// tests construct RenderServices without Initialize() and never need Vulkan.

#include <gtest/gtest.h>

#include "Core/Time.h"
#include "Engine/Rendering/RenderServices.h"

#include <cmath>
#include <memory>
#include <optional>

using namespace GameEngine;
using GameEngine::Engine::Renderer::RenderServices;

namespace
{
// Mirrors the file-local kScrollAnimationPeriodSeconds in RenderServicesFrameGraph.cpp:
// the wrap window for the bounded scroll clock (uTimeParams.y).
constexpr double kScrollAnimationPeriodSeconds = 1000.0;

// Deterministic step used by editor UI replay (fixed 1/60), the cadence that must
// stay byte-reproducible.
constexpr float kReplayStep = 1.0f / 60.0f;

void PumpFrames(int count, float measuredDelta)
{
    for (int i = 0; i < count; ++i)
        Time::Detail::SetFrameDeltaTime(measuredDelta);
}
} // namespace

// The unbounded clock (uTimeParams.x) equals the global cumulative seconds, and
// the scroll clock (uTimeParams.y) is its fmod-bounded companion — both read from
// Time, not from any per-instance member.
TEST(AnimationClockPersistence, UniformsDeriveFromGlobalCumulativeClock)
{
    Time::SetDeterministicStep(std::nullopt);

    RenderServices rs;

    PumpFrames(3, 0.05f);
    const double cumulative = Time::GetCumulativeSeconds();

    EXPECT_FLOAT_EQ(rs.GetShaderAnimationTimeSeconds(), static_cast<float>(cumulative));
    EXPECT_FLOAT_EQ(rs.GetScrollAnimationTimeSeconds(),
                    static_cast<float>(std::fmod(cumulative, kScrollAnimationPeriodSeconds)));

    Time::SetDeterministicStep(std::nullopt);
}

// The core regression: time continues across a RenderServices destroy/recreate
// instead of resetting to zero.
TEST(AnimationClockPersistence, SurvivesRenderServicesReinitWithoutResettingToZero)
{
    Time::SetDeterministicStep(kReplayStep);

    // A first renderer observes the clock after some warmup frames.
    auto rs1 = std::make_unique<RenderServices>();
    PumpFrames(10, 0.016f);
    const double afterWarmup = Time::GetCumulativeSeconds();
    const float t1 = rs1->GetShaderAnimationTimeSeconds();
    EXPECT_FLOAT_EQ(t1, static_cast<float>(afterWarmup));
    ASSERT_GT(t1, 0.0f) << "clock must have advanced past zero before the rebuild";

    // Simulate device-lost recovery / window rebuild: tear the renderer down, run
    // more frames, then bring a brand-new renderer up.
    rs1.reset();
    PumpFrames(7, 0.016f);
    auto rs2 = std::make_unique<RenderServices>();

    const double afterRebuild = Time::GetCumulativeSeconds();
    const float t2 = rs2->GetShaderAnimationTimeSeconds();

    EXPECT_FLOAT_EQ(t2, static_cast<float>(afterRebuild));
    EXPECT_GT(t2, t1) << "animation time must continue after RenderServices re-init, not reset to 0";
    // The freshly constructed renderer sees the full elapsed time, not a fresh
    // zero-based accumulation.
    EXPECT_NEAR(static_cast<double>(t2) - static_cast<double>(t1),
                7.0 * static_cast<double>(kReplayStep), 1e-6);

    Time::SetDeterministicStep(std::nullopt);
}

// Determinism contract for movie capture / UI replay: when a deterministic step is
// pinned, the cumulative clock advances by exactly that step regardless of the
// measured per-frame delta, so replay stays byte-reproducible.
TEST(AnimationClockPersistence, DeterministicStepOverridesMeasuredDelta)
{
    Time::SetDeterministicStep(kReplayStep);

    const double before = Time::GetCumulativeSeconds();
    // Wildly varying measured deltas — all must be ignored while the step is pinned.
    Time::Detail::SetFrameDeltaTime(5.0f);
    Time::Detail::SetFrameDeltaTime(0.0f);
    Time::Detail::SetFrameDeltaTime(0.3f);
    const double after = Time::GetCumulativeSeconds();

    EXPECT_NEAR(after - before, 3.0 * static_cast<double>(kReplayStep), 1e-9)
        << "pinned deterministic step must drive the cumulative clock, not the measured delta";

    Time::SetDeterministicStep(std::nullopt);
}

// Without a deterministic step the measured delta drives the clock, and negative
// deltas are clamped to zero (never rewind — that would itself flow water backward).
TEST(AnimationClockPersistence, MeasuredDeltaDrivesClockAndNegativesClampToZero)
{
    Time::SetDeterministicStep(std::nullopt);

    const double before = Time::GetCumulativeSeconds();
    Time::Detail::SetFrameDeltaTime(0.25f);
    Time::Detail::SetFrameDeltaTime(0.25f);
    EXPECT_NEAR(Time::GetCumulativeSeconds() - before, 0.5, 1e-6);

    const double beforeNeg = Time::GetCumulativeSeconds();
    Time::Detail::SetFrameDeltaTime(-1.0f);
    EXPECT_NEAR(Time::GetCumulativeSeconds() - beforeNeg, 0.0, 1e-12)
        << "a negative measured delta must not rewind the cumulative clock";
}

// A multi-second frame hitch must advance the animation clock by only the cap,
// not the full delta — the fix for the observed backward-snap (a large
// single-frame advance jumps a scrolling UV a non-integer number of tiles). The
// real Time delta reported by GetDeltaTime() stays the true measured value.
TEST(AnimationClockPersistence, LargeFrameHitchIsCappedForAnimationClockOnly)
{
    // Mirrors the file-local kMaxAnimationDeltaSeconds in Time.cpp.
    constexpr double kMaxAnimationDelta = 0.25;
    Time::SetDeterministicStep(std::nullopt);

    const double before = Time::GetCumulativeSeconds();
    Time::Detail::SetFrameDeltaTime(5.0f); // a 5-second shader-compile hitch
    const double after = Time::GetCumulativeSeconds();

    EXPECT_NEAR(after - before, kMaxAnimationDelta, 1e-6)
        << "a frame hitch must advance the animation clock by only the cap, not the full delta";
    // The real per-frame delta used elsewhere is untouched by the animation cap.
    EXPECT_FLOAT_EQ(Time::GetDeltaTime(), 5.0f);
}

// The scroll clock wraps at kScrollAnimationPeriodSeconds so a long session keeps
// fp32 sub-frame precision. Driven here with in-range (uncapped) measured deltas.
TEST(AnimationClockPersistence, ScrollClockWrapsAtPeriod)
{
    Time::SetDeterministicStep(std::nullopt);

    RenderServices rs;
    // Drive well past one wrap period using in-range deltas (below the hitch cap,
    // so each is applied in full).
    const int frames = static_cast<int>(kScrollAnimationPeriodSeconds / 0.2) + 100;
    for (int i = 0; i < frames; ++i)
        Time::Detail::SetFrameDeltaTime(0.2f);

    const double cumulative = Time::GetCumulativeSeconds();
    ASSERT_GT(cumulative, kScrollAnimationPeriodSeconds);

    const float scroll = rs.GetScrollAnimationTimeSeconds();
    EXPECT_FLOAT_EQ(scroll, static_cast<float>(std::fmod(cumulative, kScrollAnimationPeriodSeconds)));
    EXPECT_LT(scroll, static_cast<float>(kScrollAnimationPeriodSeconds));
    EXPECT_GE(scroll, 0.0f);
    // The unbounded clock keeps climbing past the wrap window.
    EXPECT_GT(rs.GetShaderAnimationTimeSeconds(), static_cast<float>(kScrollAnimationPeriodSeconds));
}
