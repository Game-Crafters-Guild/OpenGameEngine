#include "Noise/GradientCurl3D.h"

#include <gtest/gtest.h>

#include <limits>

using GameEngine::Mathematics::Vector3;
using GameEngine::Noise::GradientCurl3D;

namespace
{
const Vector3 kPoint{0.285f, 0.045f, 0.915f};
} // namespace

TEST(GradientCurl3D, PreservesSeededReferenceSamples)
{
    // The field the particle noise processor has shown since its gradient option shipped,
    // sampled at 0.5 cells per unit around (0.17, -0.31, 1.43) with a 0.2 cell scroll.
    const Vector3 point{0.17f * 0.5f + 0.2f, -0.31f * 0.5f + 0.2f, 1.43f * 0.5f + 0.2f};
    const Vector3 expected{1.7340943814f, 0.9994704724f, -0.7334085702f};
    const auto actual = GradientCurl3D(point, 42);
    for (std::size_t axis = 0; axis < 3; ++axis)
        EXPECT_NEAR(actual[axis], expected[axis], 2e-6f);
}

TEST(GradientCurl3D, SeedReplaysAndChangesTheField)
{
    const auto first = GradientCurl3D(kPoint, 42);
    const auto replay = GradientCurl3D(kPoint, 42);
    const auto other = GradientCurl3D(kPoint, 43);
    for (std::size_t axis = 0; axis < 3; ++axis)
        EXPECT_FLOAT_EQ(first[axis], replay[axis]);
    EXPECT_GT((first - other).Length(), 0.001f);
}

TEST(GradientCurl3D, TheFieldIsDivergenceFree)
{
    constexpr float kStep = 0.001f;
    float divergence = 0.0f;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        auto left = kPoint;
        auto right = kPoint;
        left[axis] -= kStep;
        right[axis] += kStep;
        divergence += (GradientCurl3D(right, 42)[axis] - GradientCurl3D(left, 42)[axis]) / (2.0f * kStep);
    }
    EXPECT_NEAR(divergence, 0.0f, 0.01f);
}

TEST(GradientCurl3D, TheWrapIsContinuous)
{
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        Vector3 left{0.37f, 0.53f, 0.79f};
        Vector3 right = left;
        left[axis] = -0.0001f;
        right[axis] = 0.0001f;
        EXPECT_LT((GradientCurl3D(left, 42) - GradientCurl3D(right, 42)).Length(), 0.01f);
    }
}

TEST(GradientCurl3D, ANonFinitePointGivesZero)
{
    for (const float invalid : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN()})
    {
        EXPECT_FLOAT_EQ(GradientCurl3D({invalid, 0.0f, 0.0f}, 42).Length(), 0.0f);
        EXPECT_FLOAT_EQ(GradientCurl3D({0.0f, 0.0f, invalid}, 42).Length(), 0.0f);
    }
}
