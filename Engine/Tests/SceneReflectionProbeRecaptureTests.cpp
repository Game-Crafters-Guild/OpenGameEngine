// When a scene reflection probe recaptures. A realtime probe recaptures at most
// once per interval and only while its world changes, then takes the
// convergence bakes after the change and stops: a static scene is not
// recaptured. The world epochs are read from RenderServices, which needs no
// device for them.

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneProbeRecaptureSchedule.h"

#include "AssetCore/GUID.h"
#include "Types/StringId.h"

#include <gtest/gtest.h>

using GameEngine::Engine::Renderer::ExtractedLight;
using GameEngine::Engine::Renderer::Material;
using GameEngine::Engine::Renderer::RenderServices;
using GameEngine::Engine::Renderer::SceneProbeRecaptureSchedule;
using GameEngine::Engine::Renderer::SceneProbeWorldEpochs;

namespace
{
constexpr float kInterval = 0.5f;
constexpr float kFrameSeconds = 1.0f / 60.0f;
constexpr float kOnceProbe = -1.0f;
constexpr uint64_t kInputs = 0x1234;
constexpr int kTenSecondsOfFrames = 600;
using GameEngine::operator""_sid;
constexpr GameEngine::StringId kRoughness = "roughness"_sid;
constexpr int kBakesPerChange = 1 + static_cast<int>(SceneProbeRecaptureSchedule::kConvergenceBakes);

// Ticks `frames` frames, baking whenever the schedule is due; returns the bake count.
int RunFrames(SceneProbeRecaptureSchedule& schedule, const SceneProbeWorldEpochs& epochs,
              float interval, int frames)
{
    int bakes = 0;
    for (int frame = 0; frame < frames; ++frame)
    {
        schedule.Tick(kFrameSeconds);
        if (!schedule.Due(kInputs, epochs, interval))
            continue;
        schedule.ConsumeBake(kInputs, epochs, interval >= 0.0f);
        ++bakes;
    }
    return bakes;
}
} // namespace

TEST(SceneProbeRecaptureSchedule, SettledRealtimeProbeStopsAfterItsConvergenceBakes)
{
    SceneProbeRecaptureSchedule schedule;
    EXPECT_EQ(RunFrames(schedule, SceneProbeWorldEpochs{}, kInterval, kTenSecondsOfFrames), kBakesPerChange)
        << "the first bake and its convergence bakes, then nothing";
}

TEST(SceneProbeRecaptureSchedule, EveryWorldEpochRecapturesAtTheIntervalThenConverges)
{
    SceneProbeRecaptureSchedule schedule;
    SceneProbeWorldEpochs epochs{};
    RunFrames(schedule, epochs, kInterval, kTenSecondsOfFrames);

    epochs.Lights = 1;
    EXPECT_EQ(RunFrames(schedule, epochs, kInterval, kTenSecondsOfFrames), kBakesPerChange);

    schedule.ConsumeBake(kInputs, epochs, /*realtime=*/true);
    ++epochs.Lights;
    schedule.Tick(kInterval / 2.0f);
    EXPECT_FALSE(schedule.Due(kInputs, epochs, kInterval)) << "no faster than the interval";
    schedule.Tick(kInterval / 2.0f);
    EXPECT_TRUE(schedule.Due(kInputs, epochs, kInterval));
    RunFrames(schedule, epochs, kInterval, kTenSecondsOfFrames);

    for (uint64_t SceneProbeWorldEpochs::*field :
         {&SceneProbeWorldEpochs::RenderContent, &SceneProbeWorldEpochs::ShadowCasters,
          &SceneProbeWorldEpochs::DynamicDepth, &SceneProbeWorldEpochs::Materials})
    {
        ++(epochs.*field);
        EXPECT_EQ(RunFrames(schedule, epochs, kInterval, kTenSecondsOfFrames), kBakesPerChange);
    }
}

TEST(SceneProbeRecaptureSchedule, OnceProbesIgnoreTheWorldAndIntervalZeroCapturesEveryFrame)
{
    SceneProbeRecaptureSchedule once;
    SceneProbeWorldEpochs epochs{};
    EXPECT_EQ(RunFrames(once, epochs, kOnceProbe, kTenSecondsOfFrames), kBakesPerChange);
    epochs.RenderContent = 7;
    EXPECT_EQ(RunFrames(once, epochs, kOnceProbe, kTenSecondsOfFrames), 0);
    EXPECT_TRUE(once.Due(kInputs + 1, epochs, kOnceProbe)) << "its own inputs still re-arm it";

    SceneProbeRecaptureSchedule everyFrame;
    EXPECT_EQ(RunFrames(everyFrame, SceneProbeWorldEpochs{}, 0.0f, 10), 10);
    EXPECT_FALSE(everyFrame.Due(0, SceneProbeWorldEpochs{}, 0.0f)) << "no content, no capture";
}

TEST(SceneProbeRecaptureSchedule, EpochsReadContentCasterLightAndMaterialVersions)
{
    RenderServices rs;
    constexpr uint64_t kWorld = 3;
    constexpr uint64_t kOtherWorld = 4;

    SceneProbeWorldEpochs before = SceneProbeWorldEpochs::Read(rs, kWorld);
    rs.NotifyRenderContentChanged(kWorld);
    EXPECT_NE(SceneProbeWorldEpochs::Read(rs, kWorld).RenderContent, before.RenderContent);

    before = SceneProbeWorldEpochs::Read(rs, kWorld);
    rs.NotifyShadowCasterContentChanged(kWorld, {}, /*unattributed=*/true);
    EXPECT_NE(SceneProbeWorldEpochs::Read(rs, kWorld).ShadowCasters, before.ShadowCasters);

    before = SceneProbeWorldEpochs::Read(rs, kWorld);
    rs.SubmitLight(kWorld, ExtractedLight{});
    rs.FinalizeWorldLights(kWorld);
    EXPECT_NE(SceneProbeWorldEpochs::Read(rs, kWorld).Lights, before.Lights);

    const SceneProbeWorldEpochs otherWorld = SceneProbeWorldEpochs::Read(rs, kOtherWorld);
    EXPECT_EQ(otherWorld.RenderContent, 0u) << "another world's edits do not move this one";
    EXPECT_EQ(otherWorld.ShadowCasters, 0u);
    EXPECT_EQ(otherWorld.Lights, 0u);

    before = SceneProbeWorldEpochs::Read(rs, kWorld);
    Material material = Material::TestFactory::Create(GameEngine::GUID::Generate(), "ProbeRecapture", 4);
    Material::TestFactory::AddPropertyLayout(material, kRoughness, 0, 4);
    material.SetFloat(kRoughness, 0.25f);
    EXPECT_NE(SceneProbeWorldEpochs::Read(rs, kWorld).Materials, before.Materials)
        << "a material property edit changes what the capture photographs";
}
