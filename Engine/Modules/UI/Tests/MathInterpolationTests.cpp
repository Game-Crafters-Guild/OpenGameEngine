#include <gtest/gtest.h>

#include "Mathematics/Easing.h"
#include "Mathematics/Interpolation.h"

using namespace GameEngine::Math;

// ---------------------------------------------------------------------------
// Lerp
// ---------------------------------------------------------------------------

TEST(MathInterpolation, LerpAtZero)
{
    EXPECT_FLOAT_EQ(Lerp(0.0f, 10.0f, 0.0f), 0.0f);
}

TEST(MathInterpolation, LerpAtOne)
{
    EXPECT_FLOAT_EQ(Lerp(0.0f, 10.0f, 1.0f), 10.0f);
}

TEST(MathInterpolation, LerpAtHalf)
{
    EXPECT_FLOAT_EQ(Lerp(0.0f, 10.0f, 0.5f), 5.0f);
}

TEST(MathInterpolation, LerpNegativeValues)
{
    EXPECT_FLOAT_EQ(Lerp(-10.0f, 10.0f, 0.5f), 0.0f);
    EXPECT_FLOAT_EQ(Lerp(-20.0f, -10.0f, 0.5f), -15.0f);
}

// ---------------------------------------------------------------------------
// Clamp01
// ---------------------------------------------------------------------------

TEST(MathInterpolation, Clamp01BelowZero)
{
    EXPECT_FLOAT_EQ(Clamp01(-0.5f), 0.0f);
}

TEST(MathInterpolation, Clamp01AboveOne)
{
    EXPECT_FLOAT_EQ(Clamp01(1.5f), 1.0f);
}

TEST(MathInterpolation, Clamp01InRange)
{
    EXPECT_FLOAT_EQ(Clamp01(0.5f), 0.5f);
}

// ---------------------------------------------------------------------------
// SmoothStep
// ---------------------------------------------------------------------------

TEST(MathInterpolation, SmoothStepAtEdges)
{
    EXPECT_FLOAT_EQ(SmoothStep(0.0f, 1.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(SmoothStep(0.0f, 1.0f, 1.0f), 1.0f);
}

TEST(MathInterpolation, SmoothStepAtHalf)
{
    EXPECT_FLOAT_EQ(SmoothStep(0.0f, 1.0f, 0.5f), 0.5f);
}

// ---------------------------------------------------------------------------
// LerpColorARGB
// ---------------------------------------------------------------------------

TEST(MathInterpolation, LerpColorBlackToWhiteAtHalf)
{
    uint32_t result = LerpColorARGB(0xFF000000u, 0xFFFFFFFFu, 0.5f);
    EXPECT_EQ(result, 0xFF808080u);
}

TEST(MathInterpolation, LerpColorSameColorReturnsIdentity)
{
    uint32_t color = 0xAABBCCDDu;
    EXPECT_EQ(LerpColorARGB(color, color, 0.0f), color);
    EXPECT_EQ(LerpColorARGB(color, color, 0.5f), color);
    EXPECT_EQ(LerpColorARGB(color, color, 1.0f), color);
}

TEST(MathInterpolation, LerpColorAlphaChannel)
{
    uint32_t result = LerpColorARGB(0x00FF0000u, 0xFFFF0000u, 0.5f);
    uint32_t alpha = (result >> 24) & 0xFFu;
    EXPECT_NEAR(static_cast<float>(alpha), 128.0f, 1.0f);
    uint32_t red = (result >> 16) & 0xFFu;
    EXPECT_EQ(red, 0xFFu);
}

// GitHub #766. css-color-4 13.3 interpolates colours in PREMULTIPLIED alpha.
// The `transparent` keyword is rgba(0, 0, 0, 0), so interpolating toward it
// without premultiplying drags every channel toward zero and the colour goes
// dark on its way out. Chrome resolves red -> transparent at 50% to
// rgba(255, 0, 0, 0.5); un-premultiplied arithmetic gives rgba(128, 0, 0, 0.5).
TEST(MathInterpolation, LerpColorToTransparentKeepsChannelsAtChromesValue)
{
    const uint32_t result = LerpColorARGB(0xFFFF0000u, 0x00000000u, 0.5f);
    EXPECT_EQ(result, 0x80FF0000u);
}

// The channels holding is the whole property, not an artefact of t = 0.5: a
// premultiplied fade keeps the visible colour constant and moves only alpha.
TEST(MathInterpolation, LerpColorToTransparentHoldsHueAcrossTheFade)
{
    for (float t : {0.25f, 0.5f, 0.75f})
    {
        const uint32_t result = LerpColorARGB(0xFF3366CCu, 0x00000000u, t);
        EXPECT_EQ((result >> 16) & 0xFFu, 0x33u) << "t=" << t;
        EXPECT_EQ((result >> 8) & 0xFFu, 0x66u) << "t=" << t;
        EXPECT_EQ(result & 0xFFu, 0xCCu) << "t=" << t;
    }
}

// Both endpoints fully transparent: premultiplied space has no colour left to
// un-premultiply, and dividing by the zero result alpha must not escape.
TEST(MathInterpolation, LerpColorBetweenTransparentEndpointsStaysTransparent)
{
    EXPECT_EQ(LerpColorARGB(0x00FF0000u, 0x0000FF00u, 0.5f), 0x00000000u);
}

// The tests above all fade toward a transparent BLACK, so on their own they are
// also satisfied by a rule that merely leaves channels alone while alpha moves.
// Fading to a transparent BLUE separates the two: a zero-alpha endpoint
// contributes no colour at all, so the result stays pure red rather than
// picking up the blue. Chrome 150 resolves
//   rgba(255,0,0,1) -> rgba(0,0,255,0) at 50%  =>  rgba(255, 0, 0, 0.5)
// where un-premultiplied arithmetic gives rgba(128, 0, 128, 0.5).
TEST(MathInterpolation, LerpColorToATransparentNonBlackTakesNoColourFromIt)
{
    const uint32_t result = LerpColorARGB(0xFFFF0000u, 0x000000FFu, 0.5f);
    EXPECT_EQ(result, 0x80FF0000u);
}

// The general case: both alphas non-zero and unequal, so the un-premultiply
// divisor is neither 1 nor the endpoint alpha and every channel moves. Chrome
// 150 resolves
//   rgba(255,0,0,1) -> rgba(0,0,255,0.2) at 50%  =>  rgba(213, 0, 43, 0.6)
// where un-premultiplied arithmetic gives rgba(128, 0, 128, 0.6).
TEST(MathInterpolation, LerpColorBetweenPartialAlphasMatchesChrome)
{
    // 0.2 alpha is 51/255; Chrome's reported 0.6 result alpha is 153/255.
    const uint32_t result = LerpColorARGB(0xFFFF0000u, 0x330000FFu, 0.5f);
    EXPECT_EQ((result >> 24) & 0xFFu, 153u);
    EXPECT_EQ((result >> 16) & 0xFFu, 213u);
    EXPECT_EQ((result >> 8) & 0xFFu, 0u);
    EXPECT_EQ(result & 0xFFu, 43u);
}

// ---------------------------------------------------------------------------
// EvalEasing
// ---------------------------------------------------------------------------

TEST(MathInterpolation, EvalEasingLinear)
{
    EXPECT_FLOAT_EQ(EvalEasing(EasingFunction::Linear, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(EvalEasing(EasingFunction::Linear, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(EvalEasing(EasingFunction::Linear, 0.5f), 0.5f);
}

TEST(MathInterpolation, EvalEasingBoundaryConditions)
{
    const EasingFunction functions[] = {
        EasingFunction::Ease,
        EasingFunction::EaseIn,
        EasingFunction::EaseOut,
        EasingFunction::EaseInOut};

    for (auto fn : functions)
    {
        EXPECT_NEAR(EvalEasing(fn, 0.0f), 0.0f, 1e-5f);
        EXPECT_NEAR(EvalEasing(fn, 1.0f), 1.0f, 1e-5f);
    }
}

TEST(MathInterpolation, EvalEasingMonotonicity)
{
    const EasingFunction functions[] = {
        EasingFunction::Linear,
        EasingFunction::Ease,
        EasingFunction::EaseIn,
        EasingFunction::EaseOut,
        EasingFunction::EaseInOut};

    constexpr int kSamples = 50;
    for (auto fn : functions)
    {
        float prev = EvalEasing(fn, 0.0f);
        for (int i = 1; i <= kSamples; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(kSamples);
            float curr = EvalEasing(fn, t);
            EXPECT_GE(curr, prev - 1e-5f)
                << "Monotonicity violated at t=" << t
                << " for easing " << static_cast<int>(fn);
            prev = curr;
        }
    }
}

// ---------------------------------------------------------------------------
// CubicBezierEval
// ---------------------------------------------------------------------------

TEST(MathInterpolation, CubicBezierLinearIdentity)
{
    constexpr int kSamples = 20;
    for (int i = 0; i <= kSamples; ++i)
    {
        float t = static_cast<float>(i) / static_cast<float>(kSamples);
        float y = CubicBezierEval(0.0f, 0.0f, 1.0f, 1.0f, t);
        EXPECT_NEAR(y, t, 0.02f) << "Linear bezier diverges at t=" << t;
    }
}

TEST(MathInterpolation, CubicBezierBoundaries)
{
    EXPECT_FLOAT_EQ(CubicBezierEval(0.25f, 0.1f, 0.25f, 1.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(CubicBezierEval(0.25f, 0.1f, 0.25f, 1.0f, 1.0f), 1.0f);
    EXPECT_FLOAT_EQ(CubicBezierEval(0.42f, 0.0f, 0.58f, 1.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(CubicBezierEval(0.42f, 0.0f, 0.58f, 1.0f, 1.0f), 1.0f);
}
