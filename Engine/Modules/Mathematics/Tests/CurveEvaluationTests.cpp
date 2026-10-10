#include "Mathematics/Curve.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace GameEngine;

namespace
{
// The floor the sky's illuminance curve evaluates with: 0.001 lx, below starlight.
constexpr float kFloorLux = 0.001f;

// What the physical sun of the default sky (the equator at the March equinox, a 100 000 lx sun)
// delivers facing it as it sets, read from SkySunPath::PhysicalSunIlluminanceLux at the hours it
// crosses each elevation (17:40, 17:52, 18:00, 18:12, 18:24).
constexpr float kLuxAtFiveDegrees = 40266.98f;
constexpr float kLuxAtTwoDegrees = 18877.57f;
constexpr float kLuxAtHorizon = 4857.80f;
constexpr float kLuxAtMinusThreeDegrees = 126.33f;
constexpr float kLuxAtMinusSixDegrees = 0.697f;

Math::CurveKey Key(float time, float value, Math::CurveInterp interp = Math::CurveInterp::Linear)
{
    Math::CurveKey key;
    key.Time = time;
    key.Value = value;
    key.Interp = interp;
    return key;
}

float Logarithmic(const std::vector<Math::CurveKey>& keys, float t, float period = 0.0f)
{
    return Math::EvaluateCurveKeysLogarithmic(keys.data(), static_cast<uint32_t>(keys.size()), t, kFloorLux,
                                              period);
}

float Linear(const std::vector<Math::CurveKey>& keys, float t)
{
    return Math::EvaluateCurveKeys(keys.data(), static_cast<uint32_t>(keys.size()), t);
}
} // namespace

// The physical sun at sunset keyed where it crosses 5, 2, 0, -3 and -6 degrees, as Smooth keys with
// Auto tangents. A Hermite through these values in lux undershoots to about -229 lx at 18:15, where
// the sun delivers 34 lx; in log2 the same keys fall monotonically and stay positive.
TEST(CurveEvaluation, LogarithmicIsMonotoneWhereTheLinearEvaluatorUndershoots)
{
    const std::vector<Math::CurveKey> keys = {
        Key(17.0f + 40.0f / 60.0f, kLuxAtFiveDegrees, Math::CurveInterp::Smooth),
        Key(17.0f + 52.0f / 60.0f, kLuxAtTwoDegrees, Math::CurveInterp::Smooth),
        Key(18.0f, kLuxAtHorizon, Math::CurveInterp::Smooth),
        Key(18.2f, kLuxAtMinusThreeDegrees, Math::CurveInterp::Smooth),
        Key(18.4f, kLuxAtMinusSixDegrees, Math::CurveInterp::Smooth),
    };

    float lowestLinear = std::numeric_limits<float>::max();
    float previous = std::numeric_limits<float>::max();
    constexpr int kSamples = 44 * 60; // one every second from 17:40 to 18:24
    for (int s = 0; s <= kSamples; ++s)
    {
        const float t = keys.front().Time + (keys.back().Time - keys.front().Time) * static_cast<float>(s) /
                                                static_cast<float>(kSamples);
        lowestLinear = std::min(lowestLinear, Linear(keys, t));
        const float value = Logarithmic(keys, t);
        EXPECT_GT(value, 0.0f) << "at " << t << " h";
        EXPECT_LE(value, previous) << "at " << t << " h";
        previous = value;
    }
    EXPECT_LT(lowestLinear, 0.0f) << "the key set no longer shows the undershoot this test guards against";
}

// Keys that stay within one octave: the log domain returns every key exactly, and between keys it
// stays within the bracketing keys and within the gap between geometric and arithmetic interpolation
// of a doubling: 1 - 2^u / (1 + u) peaks at u = 1 / ln 2 - 1, where it is 1 - e ln 2 / 2 (5.8 %).
TEST(CurveEvaluation, LogarithmicAgreesWithTheLinearEvaluatorAcrossOneOctave)
{
    const std::vector<Math::CurveKey> keys = {Key(0.0f, 100.0f), Key(1.0f, 200.0f), Key(2.5f, 150.0f),
                                              Key(4.0f, 110.0f)};
    for (const Math::CurveKey& key : keys)
    {
        EXPECT_EQ(Logarithmic(keys, key.Time), key.Value);
        EXPECT_EQ(Logarithmic(keys, key.Time), Linear(keys, key.Time));
    }

    const float bound = 1.0f - std::exp(1.0f) * std::log(2.0f) / 2.0f + 1e-5f;
    for (int s = 0; s <= 400; ++s)
    {
        const float t = 4.0f * static_cast<float>(s) / 400.0f;
        const float logarithmic = Logarithmic(keys, t);
        const float linear = Linear(keys, t);
        EXPECT_LE(std::abs(logarithmic - linear) / linear, bound) << "at " << t;
        EXPECT_GE(logarithmic, 100.0f - 1e-3f) << "at " << t;
        EXPECT_LE(logarithmic, 200.0f + 1e-3f) << "at " << t;
    }
}

// The design's twilight figure: between the -3 degree key (126 lx at 18:12) and the -6 degree key
// (0.7 lx at 18:24), a log-linear segment reads 34 lx at 18:15, where the physical sun delivers 34 lx.
TEST(CurveEvaluation, LogarithmicReproducesTheTwilightFigure)
{
    const std::vector<Math::CurveKey> keys = {Key(18.2f, 126.0f), Key(18.4f, 0.7f)};
    EXPECT_NEAR(Logarithmic(keys, 18.25f), 34.4f, 0.1f);
    EXPECT_NEAR(Logarithmic(keys, 18.3f), std::sqrt(126.0f * 0.7f), 1e-3f);
}

// Zero, negative and NaN keys are the floor: exactly 0 at the key, finite and positive between it
// and a lit key, and 0 wherever the interpolation reaches the floor.
TEST(CurveEvaluation, LogarithmicHandlesZeroNegativeAndNotANumber)
{
    const float values[] = {0.0f, -5.0f, std::numeric_limits<float>::quiet_NaN()};
    for (const float dark : values)
    {
        const std::vector<Math::CurveKey> keys = {Key(0.0f, dark), Key(10.0f, 1000.0f), Key(20.0f, dark)};
        EXPECT_EQ(Logarithmic(keys, 0.0f), 0.0f);
        EXPECT_EQ(Logarithmic(keys, 20.0f), 0.0f);
        EXPECT_EQ(Logarithmic(keys, 30.0f), 0.0f);
        // log2 of the floor (about -10) to log2(1000) (about 10): a quarter of the way is 0.001^0.75 * 1000^0.25.
        EXPECT_NEAR(Logarithmic(keys, 2.5f), std::pow(kFloorLux, 0.75f) * std::pow(1000.0f, 0.25f), 1e-5f);
        for (int s = 1; s < 200; ++s)
        {
            const float value = Logarithmic(keys, 20.0f * static_cast<float>(s) / 200.0f);
            EXPECT_TRUE(std::isfinite(value));
            EXPECT_GT(value, 0.0f);
        }
    }
    const std::vector<Math::CurveKey> belowTheFloor = {Key(0.0f, kFloorLux * 0.5f), Key(1.0f, kFloorLux)};
    EXPECT_EQ(Logarithmic(belowTheFloor, 0.5f), 0.0f);
}

// The key shapes and the ends: Step holds the left key, a period closes the cycle log-linearly across
// the wrap gap (as EvaluateCurveKeysWrapped does linearly), and without one the end keys hold.
TEST(CurveEvaluation, LogarithmicKeepsTheKeyShapesAndTheEnds)
{
    const std::vector<Math::CurveKey> stepped = {Key(0.0f, 8.0f, Math::CurveInterp::Step), Key(1.0f, 2.0f)};
    EXPECT_EQ(Logarithmic(stepped, 0.75f), 8.0f);

    const std::vector<Math::CurveKey> day = {Key(6.0f, 10.0f), Key(12.0f, 100000.0f), Key(18.0f, 1000.0f)};
    EXPECT_EQ(Logarithmic(day, 3.0f), 10.0f);
    EXPECT_EQ(Logarithmic(day, 21.0f), 1000.0f);
    // The wrap gap runs 18:00 to 06:00; midnight is its middle, the geometric mean of 1000 and 10.
    EXPECT_NEAR(Logarithmic(day, 0.0f, 24.0f), 100.0f, 1e-3f);
    EXPECT_NEAR(Logarithmic(day, 24.0f, 24.0f), 100.0f, 1e-3f);
    EXPECT_NEAR(Logarithmic(day, 9.0f, 24.0f), 1000.0f, 1e-2f);
}

// A Smooth key stores its tangent in the authored unit (500 lx per hour here); read as a slope of
// log2(value) it would be astronomically steep. The logarithmic evaluator gives the key its Auto tangent
// in log2 instead, so the curve stays finite and rises monotonically between the rising keys.
TEST(CurveEvaluation, LogarithmicIgnoresAStoredTangentInTheAuthoredUnit)
{
    constexpr float kLuxPerHour = 500.0f;
    std::vector<Math::CurveKey> keys = {Key(10.0f, 1000.0f, Math::CurveInterp::Smooth),
                                        Key(12.0f, 2000.0f, Math::CurveInterp::Smooth)};
    for (Math::CurveKey& key : keys)
    {
        key.TangentMode = Math::CurveTangentMode::Manual;
        key.InTangent = kLuxPerHour;
        key.OutTangent = kLuxPerHour;
    }
    float previous = 0.0f;
    for (int s = 0; s <= 200; ++s)
    {
        const float value = Logarithmic(keys, 10.0f + 2.0f * static_cast<float>(s) / 200.0f);
        EXPECT_TRUE(std::isfinite(value)) << s;
        EXPECT_GE(value, previous) << s;
        previous = value;
    }
    EXPECT_NEAR(Logarithmic(keys, 11.0f), std::sqrt(1000.0f * 2000.0f), 1e-2f);
}

// The logarithmic evaluator is the linear evaluator run over log2 of every key, converted back:
// Linear, Step, and Smooth keys whose Auto tangents read both neighbours, anywhere in the curve.
TEST(CurveEvaluation, LogarithmicIsTheLinearEvaluatorOverTheLogOfEveryKey)
{
    const std::vector<Math::CurveKey> keys = {
        Key(0.0f, 0.05f, Math::CurveInterp::Smooth),   Key(3.0f, 2.0f, Math::CurveInterp::Smooth),
        Key(5.0f, 400.0f, Math::CurveInterp::Linear),  Key(6.5f, 9000.0f, Math::CurveInterp::Smooth),
        Key(9.0f, 90000.0f, Math::CurveInterp::Smooth), Key(12.0f, 100000.0f, Math::CurveInterp::Step),
        Key(14.0f, 60000.0f, Math::CurveInterp::Smooth), Key(18.0f, 30.0f, Math::CurveInterp::Smooth),
        Key(20.0f, 0.3f, Math::CurveInterp::Linear),
    };
    std::vector<Math::CurveKey> logKeys = keys;
    for (Math::CurveKey& key : logKeys)
        key.Value = std::log2(key.Value);

    for (int s = 0; s <= 2000; ++s)
    {
        const float t = 20.0f * static_cast<float>(s) / 2000.0f;
        const float expected = std::exp2(Linear(logKeys, t));
        EXPECT_NEAR(Logarithmic(keys, t), expected, expected * 1e-5f) << "at " << t << " h";
    }
}

// A Flat key inside a Smooth run keeps its zero slope in the log domain: the curve levels off at
// the key instead of taking the Auto slope its neighbours would give it (log2(100 / 10) / 10 h, about
// 0.33 per hour). The two segments bend differently, so a central difference leaves about 0.001.
TEST(CurveEvaluation, LogarithmicKeepsAFlatKeyFlat)
{
    std::vector<Math::CurveKey> keys = {Key(0.0f, 10.0f, Math::CurveInterp::Smooth),
                                        Key(5.0f, 1000.0f, Math::CurveInterp::Smooth),
                                        Key(10.0f, 100.0f, Math::CurveInterp::Smooth)};
    keys[1].TangentMode = Math::CurveTangentMode::Flat;
    constexpr float kStepHours = 0.01f;
    const float slope =
        (std::log2(Logarithmic(keys, 5.0f + kStepHours)) - std::log2(Logarithmic(keys, 5.0f - kStepHours))) /
        (2.0f * kStepHours);
    EXPECT_NEAR(slope, 0.0f, 0.05f);
}
