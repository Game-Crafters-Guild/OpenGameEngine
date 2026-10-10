// The Scene View toolbar's post-processing switches, applied to the view's blended
// settings: switching post processing off, or Bloom off in its menu, must leave no
// bloom gate active, the depth veil included (it keeps the highlight chain alive on
// its own).

#include <gtest/gtest.h>

#include "SceneView/SceneViewPostProcessSwitches.h"

#include "Engine/Rendering/PostProcessSettings.h"

#include <cstdint>

using GameEngine::Editor::ApplySceneViewPostProcessSwitches;
using GameEngine::Editor::SceneViewPostProcessSwitches;
using GameEngine::Engine::Renderer::PostProcessSettings;

namespace
{
// Any tonemap value the switches can assign; the bloom assertions do not read it.
constexpr int32_t kOffTonemapMode = 0;

PostProcessSettings BloomWithDepthVeilAndLensDirt()
{
    PostProcessSettings settings{};
    settings.BloomIntensity = 0.45f;
    settings.BloomScatteringAmount = 0.3f;
    settings.BloomDepthVeilEnabled = 1;
    settings.BloomDepthVeilIntensity = 1.0f;
    settings.BloomDepthVeilStart = 0.0f;
    settings.BloomDepthVeilEnd = 15.0f;
    settings.BloomLensDirtEnabled = 1;
    settings.BloomLensDirtIntensity = 1.0f;
    return settings;
}
} // namespace

TEST(SceneViewPostProcessSwitches, PostProcessingOrBloomOffTurnsTheWholeBloomOff)
{
    const PostProcessSettings authored = BloomWithDepthVeilAndLensDirt();
    ASSERT_TRUE(authored.IsBloomHighlightsActive());
    ASSERT_TRUE(authored.IsBloomScatteringActive());
    ASSERT_TRUE(authored.IsBloomLensDirtActive());

    PostProcessSettings allOn = authored;
    ApplySceneViewPostProcessSwitches({}, kOffTonemapMode, allOn);
    EXPECT_TRUE(allOn.IsBloomHighlightsActive()) << "every switch on leaves bloom running";
    EXPECT_TRUE(allOn.IsBloomScatteringActive());

    const struct
    {
        const char* Label;
        SceneViewPostProcessSwitches Switches;
    } cases[] = {
        {"post processing off", {.PostProcessing = false}},
        {"Bloom off", {.Bloom = false}},
    };
    for (const auto& c : cases)
    {
        PostProcessSettings settings = authored;
        ApplySceneViewPostProcessSwitches(c.Switches, kOffTonemapMode, settings);
        EXPECT_FALSE(settings.IsBloomActive()) << c.Label;
        EXPECT_FALSE(settings.IsBloomHighlightsActive()) << c.Label << ": the depth veil keeps highlights on";
        EXPECT_FALSE(settings.IsBloomScatteringActive()) << c.Label;
        EXPECT_FALSE(settings.IsBloomLensDirtActive()) << c.Label;
    }
}
