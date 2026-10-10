#include <gtest/gtest.h>

#include "Platform/SystemMetrics.h"
#include "UI/CaretBlink.h"

#include <cmath>

using namespace GameEngine;

namespace
{
using Milliseconds = std::chrono::milliseconds;

// Mirror of the caret expression in ui_sdf.frag:
//     float phase = mod(floor(pc.timeSeconds * pc.caretPhaseTogglesPerSecond), 2.0);
//     caretAlpha = (phase < 1.0) ? 1.0 : 0.0;
// This is a copy of the shader's arithmetic, so it pins the CONTRACT (when the
// caret is lit for a given rate) rather than proving the GLSL itself; the GLSL
// side is a one-line read of the same uniform.
//
// SCOPE, so these are not read as more than they are: the expression below is
// reached only once a caret primitive's force-visible deadline has passed. No
// test in this file drives a control, so none of them establishes that any
// particular caret on screen blinks — these pin the timing arithmetic and
// nothing else, and every one of them passed while #788 was live. Whether a
// deadline is stamped at the right moment for the clock to overtake it is a
// separate question, and CaretForceVisibleDeadlineTests.cpp owns it.
bool CaretIsLit(float timeSeconds, float togglesPerSecond)
{
    const float phase = std::fmod(std::floor(timeSeconds * togglesPerSecond), 2.0f);
    return phase < 1.0f;
}
} // namespace

TEST(CaretBlinkTests, NeverBlinkBecomesAZeroRateThatLeavesTheCaretLitForever)
{
    EXPECT_FLOAT_EQ(UI::CaretPhaseTogglesPerSecond(std::nullopt), 0.0f);

    // The shader has no separate "solid" branch, so a zero rate has to produce
    // a lit caret at every time value it will ever be handed.
    for (const float t : {0.0f, 0.5f, 1.0f, 60.0f, 3600.0f, 86400.0f})
        EXPECT_TRUE(CaretIsLit(t, 0.0f)) << "t=" << t;
}

TEST(CaretBlinkTests, ADurationThatCannotDescribeABlinkTakesTheSameSolidPath)
{
    // Guards the push constant against an infinity if a platform ever reports 0.
    EXPECT_FLOAT_EQ(UI::CaretPhaseTogglesPerSecond(Milliseconds{0}), 0.0f);
    EXPECT_FLOAT_EQ(UI::CaretPhaseTogglesPerSecond(Milliseconds{-5}), 0.0f);
}

TEST(CaretBlinkTests, TheRateIsTheReciprocalOfTheHalfPeriod)
{
    EXPECT_FLOAT_EQ(UI::CaretPhaseTogglesPerSecond(Milliseconds{500}), 2.0f);
    EXPECT_FLOAT_EQ(UI::CaretPhaseTogglesPerSecond(Milliseconds{250}), 4.0f);
    EXPECT_FLOAT_EQ(UI::CaretPhaseTogglesPerSecond(Milliseconds{1000}), 1.0f);

    // The Windows default, and the value Chrome was measured at (531.1 +/- 14.7 ms).
    EXPECT_NEAR(UI::CaretPhaseTogglesPerSecond(Milliseconds{530}), 1.8867924f, 1e-5f);
}

TEST(CaretBlinkTests, TheCaretTogglesOnTheHalfPeriodBoundaryNotTheFullCycle)
{
    // 530 ms visible, 530 ms hidden, repeating — NOT 530 ms for a whole cycle.
    const float rate = UI::CaretPhaseTogglesPerSecond(Milliseconds{530});

    EXPECT_TRUE(CaretIsLit(0.0f, rate));
    EXPECT_TRUE(CaretIsLit(0.529f, rate));
    EXPECT_FALSE(CaretIsLit(0.531f, rate));
    EXPECT_FALSE(CaretIsLit(1.059f, rate));
    EXPECT_TRUE(CaretIsLit(1.061f, rate));
    EXPECT_TRUE(CaretIsLit(1.589f, rate));
    EXPECT_FALSE(CaretIsLit(1.591f, rate));
}

TEST(CaretBlinkTests, TheCaretRateFollowsTheOperatingSystemRatherThanABakedConstant)
{
    // Repeats the composition UIManager_RenderRG performs to fill the push
    // constant, against whatever this machine is set to. It re-runs that
    // arithmetic rather than observing the push constant, so it catches a
    // baked rate but not a render path that fails to write the field.
    const std::optional<Milliseconds> halfPeriod = Platform::GetCaretBlinkHalfPeriod();
    const float rate = UI::CaretPhaseTogglesPerSecond(halfPeriod);

    if (!halfPeriod.has_value())
    {
        EXPECT_FLOAT_EQ(rate, 0.0f);
        return;
    }

    EXPECT_NEAR(rate, 1000.0f / static_cast<float>(halfPeriod->count()), 1e-5f);

    // 1.2 toggles/second is an 833 ms half-period. On a machine not actually
    // configured to that, a rate of 1.2 means the push constant is carrying a
    // baked constant rather than the OS setting.
    if (halfPeriod->count() != 833)
        EXPECT_GT(std::fabs(rate - 1.2f), 1e-3f) << "caret rate is not tracking the OS setting";
}

TEST(CaretBlinkTests, TheForceVisibleWindowIsOneHalfPeriodSoItScalesWithTheUsersRate)
{
    // The window TextInput::BumpCaretForceVisible adds to the UI clock, which
    // is seconds-valued — the conversion out of the OS's milliseconds happens
    // here and nowhere else.
    EXPECT_FLOAT_EQ(UI::CaretForceVisibleSeconds(Milliseconds{530}), 0.530f);
    EXPECT_FLOAT_EQ(UI::CaretForceVisibleSeconds(Milliseconds{250}), 0.250f);
    EXPECT_FLOAT_EQ(UI::CaretForceVisibleSeconds(Milliseconds{1200}), 1.200f);

    // A caret that is solid anyway needs no window held open for it.
    EXPECT_FLOAT_EQ(UI::CaretForceVisibleSeconds(std::nullopt), 0.0f);
    EXPECT_FLOAT_EQ(UI::CaretForceVisibleSeconds(Milliseconds{0}), 0.0f);
    EXPECT_FLOAT_EQ(UI::CaretForceVisibleSeconds(Milliseconds{-5}), 0.0f);

    // 0.75 s was the invented window this replaced. Any machine whose caret
    // setting is not exactly 750 ms should now differ from it.
    const std::optional<Milliseconds> halfPeriod = Platform::GetCaretBlinkHalfPeriod();
    if (halfPeriod.has_value() && halfPeriod->count() > 0 && halfPeriod->count() != 750)
        EXPECT_GT(std::fabs(UI::CaretForceVisibleSeconds(halfPeriod) - 0.75f), 1e-3f)
            << "force-visible window is not tracking the OS setting";
}

TEST(CaretBlinkTests, TheForceVisibleWindowCoversExactlyTheOnPhaseTheCaretWouldHaveHad)
{
    // The window and the rate have to agree, or the caret either flickers off
    // early or sits solid past the phase it was granted. Holding for one
    // half-period from a phase boundary lands on the next boundary: lit for
    // the whole window, and free to go dark immediately after.
    const std::optional<Milliseconds> halfPeriod = Milliseconds{530};
    const float window = UI::CaretForceVisibleSeconds(halfPeriod);
    const float rate = UI::CaretPhaseTogglesPerSecond(halfPeriod);

    EXPECT_TRUE(CaretIsLit(0.0f, rate));
    EXPECT_TRUE(CaretIsLit(window - 0.001f, rate));
    EXPECT_FALSE(CaretIsLit(window + 0.001f, rate));
}
