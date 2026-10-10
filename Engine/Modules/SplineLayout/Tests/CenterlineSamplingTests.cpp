#include "SplineLayout/CenterlineSampling.h"

#include <gtest/gtest.h>
#include <limits>

using namespace GameEngine;
using GameEngine::SplineLayout::CenterlineSampleCount;
using GameEngine::SplineLayout::kCenterlineStepMetres;
using GameEngine::SplineLayout::kMaxCenterlineLengthMetres;
using GameEngine::SplineLayout::kMaxCenterlineSamples;
using GameEngine::SplineLayout::WorldCenterlineLength;
using M4 = GameEngine::Mathematics::Matrix4x4;

namespace
{

// Metres actually covered per sample once the count is fixed. This is the
// quantity that must not drift with path length: the drape's measured length
// falls as it grows, and the tile count falls with it.
float32 RealizedStep(float32 worldLength)
{
    return worldLength / static_cast<float32>(CenterlineSampleCount(worldLength) - 1u);
}

} // namespace

// ---- Sample count ----

TEST(CenterlineSampling, StepStaysPinnedAcrossEveryLengthInBudget)
{
    // 2000 m is the case a 2048-sample cap used to land at 0.977 m per sample:
    // a silent threshold past which long paths lose tiles and the seams return.
    for (const float32 length : {1.0f, 9.0f, 40.0f, 250.0f, 1023.0f, 1024.0f, 2000.0f, 4000.0f,
                                 kMaxCenterlineLengthMetres})
    {
        EXPECT_LE(RealizedStep(length), kCenterlineStepMetres + 1.0e-4f) << "length " << length;
    }
}

TEST(CenterlineSampling, CountFollowsTheStepBelowTheBudget)
{
    EXPECT_EQ(CenterlineSampleCount(9.0f), 20u);  //  9 / 0.5 + 2
    EXPECT_EQ(CenterlineSampleCount(40.0f), 82u); // 40 / 0.5 + 2
    EXPECT_EQ(CenterlineSampleCount(2000.0f), 4002u);
}

TEST(CenterlineSampling, TheBudgetIsTheOnlyCeilingAndItIsExplicit)
{
    EXPECT_EQ(kMaxCenterlineSamples,
              static_cast<uint32>(kMaxCenterlineLengthMetres / kCenterlineStepMetres) + 2u);
    EXPECT_EQ(CenterlineSampleCount(kMaxCenterlineLengthMetres), kMaxCenterlineSamples);
    EXPECT_EQ(CenterlineSampleCount(kMaxCenterlineLengthMetres * 2.0f), kMaxCenterlineSamples);
    // Past the budget the step does coarsen — that is the state the caller
    // warns about, and it must not begin before the documented length.
    EXPECT_GT(RealizedStep(kMaxCenterlineLengthMetres * 2.0f), kCenterlineStepMetres);
}

TEST(CenterlineSampling, DegenerateLengthsDegradeToTheTwoSampleMinimum)
{
    EXPECT_EQ(CenterlineSampleCount(0.0f), 2u);
    EXPECT_EQ(CenterlineSampleCount(-5.0f), 2u);
    EXPECT_EQ(CenterlineSampleCount(std::numeric_limits<float32>::quiet_NaN()), 2u);
    EXPECT_EQ(CenterlineSampleCount(std::numeric_limits<float32>::infinity()),
              kMaxCenterlineSamples);
    EXPECT_EQ(CenterlineSampleCount(1.0e30f), kMaxCenterlineSamples);
}

// ---- Local-to-world arc length ----

TEST(CenterlineSampling, ArcLengthIsScaledOutOfEntityLocalSpace)
{
    // SplineData arc lengths are entity-local; the draped polyline, the tile
    // spacing and the step are world metres. A scaled spline entity must not
    // change how finely the drape is sampled in world space.
    EXPECT_NEAR(WorldCenterlineLength(40.0f, M4::Identity()), 40.0f, 1.0e-3f);

    const M4 uniform(glm::scale(glm::mat4(1.0f), glm::vec3(3.0f)));
    EXPECT_NEAR(WorldCenterlineLength(40.0f, uniform), 120.0f, 1.0e-3f);
    EXPECT_EQ(CenterlineSampleCount(WorldCenterlineLength(40.0f, uniform)), 242u);

    // Non-uniform: the largest axis wins, so the world step is never coarser
    // than asked for on any axis.
    const M4 stretched(glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.5f)));
    EXPECT_NEAR(WorldCenterlineLength(40.0f, stretched), 80.0f, 1.0e-3f);

    // Rotation and translation carry no scale.
    const M4 rotated(glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0.0f, 1.0f, 0.0f)));
    EXPECT_NEAR(WorldCenterlineLength(40.0f, rotated), 40.0f, 1.0e-3f);
    const M4 translated = M4::Translation(Mathematics::Vector3(500.0f, 12.0f, -7.0f));
    EXPECT_NEAR(WorldCenterlineLength(40.0f, translated), 40.0f, 1.0e-3f);
}
