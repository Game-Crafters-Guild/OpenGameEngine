// Shipped outdoor scenes must be lit for the stop the editor Scene View pins.
//
// The Scene View has no Camera entity of its own, so when its auto exposure is
// off (the default) it exposes at a fixed photographic stop —
// Components::kDefaultManualExposureEv, a sunlit exterior EV100. A scene
// whose sun is authored on the unitless scale (a few units, i.e. a few hundred
// lux) is five to eight stops under that stop and opens black, however bright it
// looks through a Fixed-exposure Game View. That is what shipped in
// WebSmoke.scene and Lanscape.scene.
//
// The gate binds the two halves that have to agree: what a scene authors above
// the ground — its key light, and the sky the seed scene has to draw — resolved
// through the engine's own photometric conversions, and the shared default the
// Scene View pins to. Change either past the tolerance and this fails.
//
// It is an ANALYTIC gate, not a rendered one: it computes the scene-linear
// values an 18 % Lambertian facing the key light, and each band of a gradient
// sky, land on after exposure. No headless harness in this tree renders a
// project scene, so a measured mean-luma gate has nothing to run on; this pins
// the quantities that went wrong. It says nothing about framing, shadowing, or
// anything the light and the sky do not dominate.

#include <gtest/gtest.h>

#include "StagedTestPaths.h"

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Rendering/Exposure.h"
#include "Scene/SceneIO.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

namespace
{
using GameEngine::Components::LightIntensityToUnitless;
using GameEngine::Components::LightType;
using GameEngine::Components::SkyMode;

constexpr const char* kWebSmokeScene = "Tools/Web/smoke-project/Assets/Scenes/WebSmoke.scene";
constexpr const char* kLanscapeScene = "Tests/Projects/Biggles/Assets/Scenes/Lanscape.scene";

// Middle grey: the albedo a correctly exposed key surface is judged against, and
// the scene-linear value it should land on.
constexpr float kMidGreyAlbedo = 0.18f;

// The scene-linear value the pinned stop maps to white.
constexpr float kStopWhite = 1.0f;

// Rec. 709 luma weights, for judging one gradient band against the stop.
constexpr float kLumaR = 0.2126f;
constexpr float kLumaG = 0.7152f;
constexpr float kLumaB = 0.0722f;

float Luma(const float rgb[3])
{
    return kLumaR * rgb[0] + kLumaG * rgb[1] + kLumaB * rgb[2];
}

// How far from middle grey a shipped scene's key surface may sit. Generous on
// purpose — this gate exists to catch a scene that is unlit by orders of
// magnitude, not to freeze anyone's art direction. The two scenes it covers sit
// at −0.44 stops; the unitless authoring it replaced sat at −8.
constexpr float kToleranceStops = 1.5f;

struct KeyLight
{
    GameEngine::ECS::EntityHandle Entity{};
    float UnitlessIntensity = 0.0f;
    int Count = 0;
};

std::filesystem::path StagedScenePath(const char* stagedRelativePath)
{
    return (GameEngine::TestPaths::StagedRoot() / stagedRelativePath).lexically_normal();
}

bool LoadStagedScene(GameEngine::ECS::World& world, const std::filesystem::path& scenePath)
{
    GameEngine::Scene::LoadOptions options{};
    options.mode = GameEngine::Scene::LoadMode::Replace;
    options.assetRootOverride = scenePath.parent_path();
    return GameEngine::Scene::LoadSceneFromFile(world, scenePath, options);
}

KeyLight ResolveKeyLight(GameEngine::ECS::World& world)
{
    KeyLight key{};
    world.Query<GameEngine::ECS::Read<GameEngine::Components::Light>>().Each(
        [&key](GameEngine::ECS::EntityHandle entity, const GameEngine::Components::Light& light)
        {
            if (light.Type != LightType::Directional)
                return;
            ++key.Count;
            key.Entity = entity;
            key.UnitlessIntensity = LightIntensityToUnitless(light.Intensity, light.IntensityUnit);
        });
    return key;
}

// The scene-linear radiance an 18 % Lambertian normal to the key light lands on
// once the Scene View's pinned stop is applied.
float ExposedMidGreyRadiance(float unitlessIntensity)
{
    const float exposure = GameEngine::Rendering::EvToLinearExposure(
        GameEngine::Components::kDefaultManualExposureEv);
    return (kMidGreyAlbedo / std::numbers::pi_v<float>) * unitlessIntensity * exposure;
}

void ExpectLitForTheSceneViewStop(const char* stagedRelativePath)
{
    const auto scenePath = StagedScenePath(stagedRelativePath);
    ASSERT_TRUE(std::filesystem::exists(scenePath))
        << scenePath.string() << " is not staged — StageTestAssets must mirror it";

    GameEngine::ECS::World world;
    ASSERT_TRUE(LoadStagedScene(world, scenePath)) << scenePath.string() << " failed to load";

    const KeyLight key = ResolveKeyLight(world);
    ASSERT_EQ(key.Count, 1) << scenePath.filename().string()
                            << " must carry exactly one enabled directional light for this gate to "
                               "have a key light to reason about";

    const float radiance = ExposedMidGreyRadiance(key.UnitlessIntensity);
    ASSERT_GT(radiance, 0.0f) << scenePath.filename().string() << " has no key illumination at all";
    const float stopsFromMidGrey = std::log2(radiance / kMidGreyAlbedo);

    EXPECT_NEAR(stopsFromMidGrey, 0.0f, kToleranceStops)
        << scenePath.filename().string() << " exposes " << stopsFromMidGrey
        << " stops from middle grey in the Scene View (key light " << key.UnitlessIntensity
        << " unitless, Scene View EV100 "
        << GameEngine::Components::kDefaultManualExposureEv
        << "). Author the sun in lux (Light.intensityUnit = Lux, ~100000 for a clear noon sun) "
           "rather than on the unitless scale.";
}
} // namespace

TEST(ShippedSceneExposure, WebSmokeSeedSceneIsLitForTheSceneViewStop)
{
    ExpectLitForTheSceneViewStop(kWebSmokeScene);
}

TEST(ShippedSceneExposure, LanscapeIsLitForTheSceneViewStop)
{
    ExpectLitForTheSceneViewStop(kLanscapeScene);
}

// The seed project is what a freshly planted project opens on, so its scene is the
// first thing anyone sees. Without a SkyEnvironment nothing draws the dome and
// everything above the horizon is the clear colour, i.e. black.
//
// Analytic, like its siblings: it reads the authored sky rather than a rendered
// frame. The gradient dome is drawn straight from the three authored colours
// scaled by GradientSkyIntensity / the 203-nit reference white, so its exposed
// luminance is decidable from the scene file alone.
TEST(ShippedSceneExposure, WebSmokeSeedSceneCarriesASkyLitForTheSameStop)
{
    const auto scenePath = StagedScenePath(kWebSmokeScene);
    ASSERT_TRUE(std::filesystem::exists(scenePath))
        << scenePath.string() << " is not staged — StageTestAssets must mirror it";

    GameEngine::ECS::World world;
    ASSERT_TRUE(LoadStagedScene(world, scenePath)) << scenePath.string() << " failed to load";

    GameEngine::Components::SkyEnvironment sky{};
    int skyCount = 0;
    world.Query<GameEngine::ECS::Read<GameEngine::Components::SkyEnvironment>>().Each(
        [&sky, &skyCount](const GameEngine::Components::SkyEnvironment& authored)
        {
            ++skyCount;
            sky = authored;
        });

    ASSERT_EQ(skyCount, 1)
        << "WebSmoke.scene must carry exactly one SkyEnvironment. With none, no sky pass is "
           "declared and the whole dome renders as the clear colour.";

    // The link is a FOLLOW, not a drive: the sky reads the light's direction, and
    // TimeOfDayDrivesSunLight off is what keeps it from rotating the light and
    // writing its color, so the sun stays where and as the scene authored it.
    ASSERT_TRUE(world.IsValid(sky.SunLight))
        << "WebSmoke.scene's SkyEnvironment.SunLight does not resolve. A dangling entity id "
           "parses cleanly and silently leaves the link empty, so the sky's sun and the scene's "
           "sun then point in different directions.";
    const KeyLight key = ResolveKeyLight(world);
    ASSERT_EQ(key.Count, 1) << "WebSmoke.scene must carry exactly one enabled directional light";
    EXPECT_EQ(sky.SunLight, key.Entity)
        << "WebSmoke.scene's SkyEnvironment must link the scene's own key light";
    EXPECT_FALSE(sky.TimeOfDayDrivesSunLight)
        << "WebSmoke.scene's SkyEnvironment drives its linked light, so the sky — not the "
           "scene — decides the sun's direction and color";

    ASSERT_EQ(sky.Mode, SkyMode::Gradient)
        << "WebSmoke.scene's SkyEnvironment is not in Gradient mode; the band check below only "
           "describes the gradient dome";

    const float exposure = GameEngine::Rendering::EvToLinearExposure(
        GameEngine::Components::kDefaultManualExposureEv);
    const float gradientScale =
        sky.GradientSkyIntensity / GameEngine::Components::kReferenceWhiteNits;
    const float brightestBandLuma =
        std::max({Luma(sky.GradientSkyTopColor),
                  Luma(sky.GradientSkyHorizonColor),
                  Luma(sky.GradientSkyBottomColor)}) *
        gradientScale * exposure;

    EXPECT_GT(brightestBandLuma, kMidGreyAlbedo)
        << "WebSmoke.scene's sky exposes its brightest band at " << brightestBandLuma
        << " scene-linear, at or under middle grey, so the sky reads darker than the ground it "
           "lights. GradientSkyIntensity is in nits ("
        << sky.GradientSkyIntensity << " here) against a Scene View pinned at EV100 "
        << GameEngine::Components::kDefaultManualExposureEv << ".";
    EXPECT_LT(brightestBandLuma, kStopWhite)
        << "WebSmoke.scene's sky exposes its brightest band at " << brightestBandLuma
        << " scene-linear, at or past the white the pinned stop maps to, so the sky clips. "
           "GradientSkyIntensity is in nits ("
        << sky.GradientSkyIntensity << " here) against a Scene View pinned at EV100 "
        << GameEngine::Components::kDefaultManualExposureEv << ".";
}
