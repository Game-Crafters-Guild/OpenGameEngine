#include "Engine/Rendering/AmbientOcclusion/GtaoHistory.h"
#include "Engine/Rendering/AmbientOcclusion/GtaoQuality.h"
#include <gtest/gtest.h>

using namespace GameEngine::Engine::Renderer;

// Presets step resolution first, then slice directions. Neither changes how much
// occlusion the sweep reports (GtaoComputeTest.PresetsAgreeOnOcclusionAmount
// measures that on the GPU); the radial tap count, which does, is not a field
// here at all.
TEST(GtaoQuality, PresetsBuyResolutionThenDirections)
{
    const auto medium = GetGtaoQualitySettings(GtaoQuality::Medium);
    const auto high = GetGtaoQualitySettings(GtaoQuality::High);
    const auto ultra = GetGtaoQualitySettings(GtaoQuality::Ultra);
    EXPECT_EQ(medium.ResolutionDivisor, 2u);
    EXPECT_EQ(high.ResolutionDivisor, 1u);
    EXPECT_EQ(ultra.ResolutionDivisor, 1u);
    EXPECT_EQ(medium.Directions, high.Directions);
    EXPECT_LT(high.Directions, ultra.Directions);
    EXPECT_GT(kGtaoSweepSteps, 0u);
    GtaoQuality quality = GtaoQuality::Medium;
    EXPECT_TRUE(ParseGtaoQuality("ultra", quality));
    EXPECT_EQ(quality, GtaoQuality::Ultra);
    EXPECT_FALSE(ParseGtaoQuality("unknown", quality));
    EXPECT_EQ(quality, GtaoQuality::Ultra);
    EXPECT_FALSE(ParseGtaoQuality("low", quality));
}

TEST(GtaoHistory, RequiresSuccessfulAdjacentFrameAndResidentMatchingResources)
{
    GtaoHistoryKey key{1920, 1080, 1, GtaoQuality::Medium, 2.0f, 0.1f, 1.0f};
    EXPECT_TRUE(CanReuseGtaoHistory(10, 11, key, key, false, true));
    EXPECT_FALSE(CanReuseGtaoHistory(~uint64_t{0}, 0, key, key, false, true));
    EXPECT_FALSE(CanReuseGtaoHistory(10, 10, key, key, false, true));
    EXPECT_FALSE(CanReuseGtaoHistory(10, 12, key, key, false, true));
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, key, true, true));
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, key, false, false));
    auto changed = key;
    changed.Width = 1280;
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, changed, false, true));
    changed = key;
    changed.Height = 720;
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, changed, false, true));
    changed = key;
    changed.Camera = 2;
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, changed, false, true));
    changed = key;
    changed.Quality = GtaoQuality::High;
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, changed, false, true));
    changed = key;
    changed.Radius = 4.0f;
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, changed, false, true));
    changed = key;
    changed.Thickness = 0.5f;
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, changed, false, true));
    changed = key;
    changed.Intensity = 2.0f;
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, changed, false, true));
    changed = key;
    changed.World = 2;
    EXPECT_FALSE(CanReuseGtaoHistory(10, 11, key, changed, false, true));
}
