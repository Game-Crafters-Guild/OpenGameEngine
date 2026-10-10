// ShadowDistanceFitTests — the directional shadow range fitted to the scene a
// view sees (ShadowMapRenderFeature::FitShadowDistanceToScene, the ShadowMap
// node's "fitShadowDistanceToScene"): the range follows the scene at any scale,
// the fade band starts beyond the scene for every fade fraction, and the range
// holds while the scene's reach moves inside its band. Pure CPU.

#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include <gtest/gtest.h>

using GameEngine::Engine::Renderer::ShadowMapRenderFeature;

namespace
{

constexpr float kNear = 0.1f;
constexpr float kFade = 0.1f;

float Fit(float previous, float reach, float fade = kFade)
{
    return ShadowMapRenderFeature::FitShadowDistanceToScene(previous, reach, kNear, fade);
}

} // namespace

TEST(ShadowDistanceFit, RangeFollowsTheSceneAtAnyScale)
{
    // A 2 m model on a floor ending 14 m ahead of the camera: the cascades
    // partition the scene, not the 200 m default.
    const float small = Fit(0.0f, 14.0f);
    EXPECT_NEAR(small, 14.0f * ShadowMapRenderFeature::kSceneFitHeadroom / (1.0f - kFade), 1e-4f);
    EXPECT_LT(small, 20.0f);

    // A 500 m scene: the range grows past the default.
    const float large = Fit(0.0f, 520.0f);
    EXPECT_NEAR(large, 520.0f * ShadowMapRenderFeature::kSceneFitHeadroom / (1.0f - kFade), 1e-2f);
    EXPECT_GT(large, 200.0f);
}

TEST(ShadowDistanceFit, FadeBandStartsBeyondTheSceneForEveryFraction)
{
    for (const float fade : {0.0f, 0.1f, 0.25f, 0.5f})
    {
        const float range = Fit(0.0f, 14.0f, fade);
        EXPECT_NEAR(range * (1.0f - fade), 14.0f * ShadowMapRenderFeature::kSceneFitHeadroom, 1e-4f)
            << "fade " << fade;
    }
}

TEST(ShadowDistanceFit, RangeHoldsWhileTheReachStaysInsideItsBand)
{
    const float range = Fit(0.0f, 14.0f);
    const float bandEnd = range * (1.0f - kFade);
    const float bandStart = bandEnd / ShadowMapRenderFeature::kSceneFitShrinkRatio;
    EXPECT_EQ(Fit(range, 14.0f), range) << "a static scene never refits";
    EXPECT_EQ(Fit(range, bandEnd * 0.999f), range);
    EXPECT_EQ(Fit(range, bandStart * 1.001f), range);

    const float grown = Fit(range, bandEnd * 1.01f);
    EXPECT_GT(grown, range) << "a reach that would enter the fade grows the range";
    EXPECT_NEAR(grown * (1.0f - kFade), bandEnd * 1.01f * ShadowMapRenderFeature::kSceneFitHeadroom, 1e-3f);

    const float shrunk = Fit(range, bandStart * 0.99f);
    EXPECT_LT(shrunk, range) << "a scene far inside the range shrinks it";
    EXPECT_EQ(Fit(shrunk, bandStart * 0.99f), shrunk) << "the refit is stable";
}

TEST(ShadowDistanceFit, NothingAheadOfTheCameraKeepsThePreviousRange)
{
    EXPECT_EQ(Fit(0.0f, kNear), 0.0f) << "no fit and no previous range";
    EXPECT_EQ(Fit(0.0f, -5.0f), 0.0f);
    EXPECT_EQ(Fit(17.0f, -5.0f), 17.0f);
}
