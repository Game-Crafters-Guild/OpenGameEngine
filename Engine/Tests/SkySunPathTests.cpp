// The sky's sun path in the running system: the sky's latitude, day of year and north reach the sun
// the renderer draws, and the sky entity's rotation does not. SolarPath's own maths is pinned
// in RenderingSolarPathTests; these run SkyEnvironmentSystem::Update on a real World with headless
// RenderServices and read the settings production consumes.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "Components/Rendering/SkySunPath.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Sky/AtmosphereTransmittance.h"
#include "Rendering/Sky/SkySystem.h"
#include "Types/ColorUtils.h"
#include "Rendering/Sky/SolarPath.h"

using namespace GameEngine;

namespace
{

constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
constexpr float kDirectionTolerance = 1e-5f;

Components::Transform YawedTransform(float yawDegrees)
{
    const Mathematics::Quaternion yaw =
        Mathematics::Quaternion::FromAxisAngle(Mathematics::Vector3(0.0f, 1.0f, 0.0f), yawDegrees * kDegreesToRadians);
    return Components::Transform::FromTRS(Mathematics::Vector3{}, yaw, Mathematics::Vector3{1.0f, 1.0f, 1.0f});
}

struct SkySunPathHarness
{
    ECS::World World;
    Engine::Renderer::RenderServices Services;
    Engine::Renderer::SkyEnvironmentSystem System{&Services};

    // The sun direction the sky renders after one frame.
    Mathematics::Vector3 RenderedSun()
    {
        System.Update(World, 0.0f);
        const auto* feature = Services.GetFeature<Engine::Renderer::SkyRenderFeature>();
        EXPECT_TRUE(feature && feature->HasActiveSettings());
        if (!feature)
            return {};
        const float* sun = feature->GetSettings().scatteringSunDir;
        return {sun[0], sun[1], sun[2]};
    }
};

Mathematics::Vector3 ExpectedSun(const Rendering::SolarSite& site, float hours)
{
    Rendering::SkySettings settings{};
    settings.timeOfDayHours = hours;
    Rendering::SkySystemState state{};
    Rendering::ComputeSimpleSunMoon(settings, Rendering::SkySystemConfig{}, Rendering::MakeSolarFrame(Rendering::EarthPathAngles(site)), state);
    return {state.sunDirWS[0], state.sunDirWS[1], state.sunDirWS[2]};
}

void ExpectSameDirection(const Mathematics::Vector3& actual, const Mathematics::Vector3& expected)
{
    EXPECT_NEAR(actual.x, expected.x, kDirectionTolerance);
    EXPECT_NEAR(actual.y, expected.y, kDirectionTolerance);
    EXPECT_NEAR(actual.z, expected.z, kDirectionTolerance);
}

} // namespace

// The sky's Latitude, DayOfYear and NorthHeading are what the renderer's sun follows: a London
// winter noon is low and, with north turned to +X, leans to -X, whatever the sky entity's own
// rotation. Its identity case is every sky saved before the site existed: no Latitude, DayOfYear or
// north, and the previous great circle with the sunrise due east.
TEST(SkySunPath, TheSkysSiteFieldsPlaceTheRenderedSun)
{
    {
        SkySunPathHarness harness;
        const ECS::EntityHandle skyEntity = harness.World.CreateEntity();
        harness.World.AddComponentImmediate(skyEntity, YawedTransform(-35.0f));
        Components::SkyEnvironment sky{};
        sky.Latitude = 51.5f;
        sky.DayOfYear = 355;
        sky.NorthHeading = 90.0f;
        sky.TimeOfDayHours = 12.0f;
        harness.World.AddComponentImmediate(skyEntity, sky);

        const Mathematics::Vector3 sun = harness.RenderedSun();
        ExpectSameDirection(sun, ExpectedSun({51.5f, 355, 90.0f}, 12.0f));
        EXPECT_NEAR(std::asin(sun.y) / kDegreesToRadians, Rendering::SolarNoonElevationDegrees(Rendering::EarthPathAngles({51.5f, 355, 0.0f})), 0.01f);
        EXPECT_LT(sun.x, -0.9f) << "north at +X puts the northern winter noon sun low toward -X";
    }
    {
        SkySunPathHarness harness;
        const ECS::EntityHandle skyEntity = harness.World.CreateEntity();
        harness.World.AddComponentImmediate(skyEntity, Components::Transform{});
        Components::SkyEnvironment sky{};
        sky.TimeOfDayHours = 7.0f;
        harness.World.AddComponentImmediate(skyEntity, sky);

        const Mathematics::Vector3 sun = harness.RenderedSun();
        ExpectSameDirection(sun, ExpectedSun({}, 7.0f));
        EXPECT_EQ(sun.z, 0.0f) << "the default path stays in the east-up plane";
        EXPECT_GT(sun.x, 0.9f);
    }
}

// Follow mode: the linked light is authoritative and the sky reads the hour back from the light's
// direction. That hour must be read on the sky's own path (latitude, day and north),
// or a light pointed at 09:00 in London in June shows the dome and the day keys of some other hour.
TEST(SkySunPath, AFollowedLightReadsItsHourOnTheSkysPath)
{
    constexpr float kHours = 9.0f;
    constexpr float kHourTolerance = 1e-3f;
    const Rendering::SolarSite site{51.5f, 172, 37.0f};

    SkySunPathHarness harness;
    const Mathematics::Vector3 sun = ExpectedSun(site, kHours);
    const ECS::EntityHandle lightEntity = harness.World.CreateEntity();
    Components::Light light{};
    light.Type = Components::LightType::Directional;
    harness.World.AddComponentImmediate(lightEntity, light);
    // The light shines along its +Z, from the sun toward the scene.
    const Mathematics::Vector3 forward(0.0f, 0.0f, 1.0f);
    const Mathematics::Vector3 shine(-sun.x, -sun.y, -sun.z);
    const Mathematics::Vector3 axis = Mathematics::Vector3::Cross(forward, shine).Normalize();
    const float angle = std::acos(std::clamp(Mathematics::Vector3::Dot(forward, shine), -1.0f, 1.0f));
    const Components::Transform lightTransform = Components::Transform::FromTRS(
        Mathematics::Vector3{}, Mathematics::Quaternion::FromAxisAngle(axis, angle), Mathematics::Vector3{1.0f, 1.0f, 1.0f});
    harness.World.AddComponentImmediate(lightEntity, lightTransform);
    Components::WorldTransform lightWorld{};
    std::copy(std::begin(lightTransform.matrix), std::end(lightTransform.matrix), std::begin(lightWorld.matrix));
    harness.World.AddComponentImmediate(lightEntity, lightWorld);

    const ECS::EntityHandle skyEntity = harness.World.CreateEntity();
    harness.World.AddComponentImmediate(skyEntity, Components::Transform{});
    Components::SkyEnvironment sky{};
    sky.Latitude = site.LatitudeDegrees;
    sky.DayOfYear = site.DayOfYear;
    sky.NorthHeading = site.NorthDegrees;
    sky.TimeOfDayHours = 15.0f;
    sky.TimeOfDayDrivesSunLight = false;
    sky.SunLight = lightEntity;
    harness.World.AddComponentImmediate(skyEntity, sky);

    const Mathematics::Vector3 rendered = harness.RenderedSun();
    ExpectSameDirection(rendered, sun);
    const auto* feature = harness.Services.GetFeature<Engine::Renderer::SkyRenderFeature>();
    ASSERT_NE(feature, nullptr);
    EXPECT_NEAR(feature->GetSettings().timeOfDayHours, kHours, kHourTolerance);
}

// A sky placed on its own linked sun light: the drive rewrites that entity's rotation every frame,
// and the sun must not chase it. North is the sky's field, so the sun holds still across frames.
TEST(SkySunPath, ASkyOnItsOwnSunLightIgnoresThatRotation)
{
    SkySunPathHarness harness;
    const ECS::EntityHandle entity = harness.World.CreateEntity();
    harness.World.AddComponentImmediate(entity, YawedTransform(40.0f));
    Components::Light light{};
    light.Type = Components::LightType::Directional;
    harness.World.AddComponentImmediate(entity, light);
    Components::SkyEnvironment sky{};
    sky.Latitude = 51.5f;
    sky.DayOfYear = 172;
    sky.TimeOfDayHours = 9.0f;
    sky.SunLight = entity;
    harness.World.AddComponentImmediate(entity, sky);

    const Mathematics::Vector3 first = harness.RenderedSun();
    const Mathematics::Vector3 second = harness.RenderedSun();
    ExpectSameDirection(first, ExpectedSun({51.5f, 172, 0.0f}, 9.0f));
    ExpectSameDirection(second, first);
}

// The inspector's "Over the day" preview is PhysicalSunIlluminanceLux. By day it must be what the
// driven sun light actually delivers, or the graph describes a different sun from the one in the
// scene; and with the sun overhead it is the light's own illuminance, which is what that value means.
TEST(SkySunPath, ThePreviewIsWhatTheDrivenLightDeliversByDay)
{
    constexpr float kSunLux = 100000.0f;
    constexpr float kRelativeTolerance = 1e-4f;

    for (const float hours : {8.0f, 12.0f, 17.5f})
    {
        SkySunPathHarness harness;
        const ECS::EntityHandle lightEntity = harness.World.CreateEntity();
        Components::Light light{};
        light.Type = Components::LightType::Directional;
        light.Intensity = kSunLux;
        light.IntensityUnit = Components::LightUnit::Lux;
        harness.World.AddComponentImmediate(lightEntity, light);
        harness.World.AddComponentImmediate(lightEntity, Components::Transform{});

        const ECS::EntityHandle skyEntity = harness.World.CreateEntity();
        harness.World.AddComponentImmediate(skyEntity, Components::Transform{});
        Components::SkyEnvironment sky{};
        sky.Latitude = 51.5f;
        sky.DayOfYear = 172;
        sky.TimeOfDayHours = hours;
        sky.SunLight = lightEntity;
        harness.World.AddComponentImmediate(skyEntity, sky);
        harness.RenderedSun();

        const auto* driven = harness.World.GetComponent<Components::Light>(lightEntity);
        ASSERT_NE(driven, nullptr);
        const float delivered = Components::SkySunIlluminance::DeliveredLux(*driven);
        const Rendering::SolarPosition position =
            Rendering::SolarPositionAtHour(Rendering::MakeSolarFrame(Rendering::EarthPathAngles({51.5f, 172, 0.0f})), hours);
        const float preview =
            Components::SkySunPath::PhysicalSunIlluminanceLux(sky, position.ElevationDegrees, kSunLux, hours);
        EXPECT_NEAR(preview, delivered, delivered * kRelativeTolerance) << "hour " << hours;
    }

    const float overhead =
        Components::SkySunPath::PhysicalSunIlluminanceLux(Components::SkyEnvironment{}, 90.0f, kSunLux, 12.0f);
    EXPECT_NEAR(overhead, kSunLux, kSunLux * kRelativeTolerance) << "the default noon sun is overhead";
}

// The Sun path choice selects which fields place the sun. Under Earth the Custom fields do nothing;
// under Custom the Earth fields do nothing and the rendered sun follows the authored axis and noon
// height. The follow mode reads a light's hour back on the Custom path too.
TEST(SkySunPath, EachPathReadsOnlyItsOwnFields)
{
    constexpr float kHours = 9.5f;
    const Rendering::SolarPathAngles custom = Rendering::CustomPathAngles(35.0f, 120.0f, 70.0f);

    Components::SkyEnvironment sky{};
    sky.Latitude = 51.5f;
    sky.DayOfYear = 172;
    sky.NorthHeading = 20.0f;
    sky.CustomAxisHeading = 120.0f;
    sky.CustomAxisAltitude = 35.0f;
    sky.CustomNoonHeight = 70.0f;
    sky.TimeOfDayHours = kHours;
    {
        SkySunPathHarness harness;
        harness.World.AddComponentImmediate(harness.World.CreateEntity(), sky);
        ExpectSameDirection(harness.RenderedSun(), ExpectedSun({51.5f, 172, 20.0f}, kHours));
    }
    sky.SunPath = Components::SkySunPathKind::Custom;
    {
        SkySunPathHarness harness;
        harness.World.AddComponentImmediate(harness.World.CreateEntity(), sky);
        Rendering::SkySettings settings{};
        settings.timeOfDayHours = kHours;
        Rendering::SkySystemState state{};
        Rendering::ComputeSimpleSunMoon(settings, Rendering::SkySystemConfig{}, Rendering::MakeSolarFrame(custom), state);
        ExpectSameDirection(harness.RenderedSun(), {state.sunDirWS[0], state.sunDirWS[1], state.sunDirWS[2]});
    }
    for (float hours = 0.5f; hours < 24.0f; hours += 1.5f)
    {
        float sun[3];
        Rendering::SkySettings settings{};
        settings.timeOfDayHours = hours;
        Rendering::SkySystemState state{};
        const Rendering::SolarFrame frame = Rendering::MakeSolarFrame(Components::SkySunPath::PathAngles(sky));
        Rendering::ComputeSimpleSunMoon(settings, Rendering::SkySystemConfig{}, frame, state);
        std::copy(std::begin(state.sunDirWS), std::end(state.sunDirWS), sun);
        EXPECT_NEAR(Rendering::SolarHourFromDirection(frame, sun), hours, 1e-3f) << "hour " << hours;
    }
}

// Switching the path loses nothing: every field of both paths is still there after a round trip
// through the other path, and switching back renders the sun exactly where it was.
TEST(SkySunPath, SwitchingPathsLosesNothing)
{
    Components::SkyEnvironment sky{};
    sky.Latitude = -33.9f;
    sky.DayOfYear = 355;
    sky.NorthHeading = 200.0f;
    sky.CustomAxisHeading = 45.0f;
    sky.CustomAxisAltitude = -10.0f;
    sky.CustomNoonHeight = 130.0f;
    sky.TimeOfDayHours = 16.0f;

    SkySunPathHarness harness;
    const ECS::EntityHandle entity = harness.World.CreateEntity();
    harness.World.AddComponentImmediate(entity, sky);
    const Mathematics::Vector3 earthSun = harness.RenderedSun();

    for (const Components::SkySunPathKind kind : {Components::SkySunPathKind::Custom, Components::SkySunPathKind::Earth})
    {
        Components::SkyEnvironment switched = *harness.World.GetComponent<Components::SkyEnvironment>(entity);
        switched.SunPath = kind;
        harness.World.AddComponentImmediate(entity, switched);
        harness.RenderedSun();
    }

    const auto* after = harness.World.GetComponent<Components::SkyEnvironment>(entity);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->Latitude, sky.Latitude);
    EXPECT_EQ(after->DayOfYear, sky.DayOfYear);
    EXPECT_EQ(after->NorthHeading, sky.NorthHeading);
    EXPECT_EQ(after->CustomAxisHeading, sky.CustomAxisHeading);
    EXPECT_EQ(after->CustomAxisAltitude, sky.CustomAxisAltitude);
    EXPECT_EQ(after->CustomNoonHeight, sky.CustomNoonHeight);
    const Mathematics::Vector3 backOnEarth = harness.RenderedSun();
    EXPECT_EQ(backOnEarth.x, earthSun.x);
    EXPECT_EQ(backOnEarth.y, earthSun.y);
    EXPECT_EQ(backOnEarth.z, earthSun.z);
}

// What the driven light delivers splits into the sun's share and the night share. By day it is all
// sun; below the twilight floor the sun's share is 0 and all of it is the moon's; in between the sun's
// share is exactly what the light delivers less the moon's term, (1 - b) * physical sun, with the
// handover weight b the drive applies.
TEST(SkySunPath, TheSunsShareOfTheDrivenLightEndsAtTheTwilightFloor)
{
    constexpr float kSunLux = 100000.0f;
    constexpr float kRelativeTolerance = 1e-4f;
    struct Hour
    {
        float Hours;
        bool AllSun;
        bool AllMoon;
    };
    for (const Hour hour : {Hour{12.0f, true, false}, Hour{18.2f, false, false}, Hour{0.0f, false, true}})
    {
        SkySunPathHarness harness;
        const ECS::EntityHandle lightEntity = harness.World.CreateEntity();
        Components::Light light{};
        light.Type = Components::LightType::Directional;
        light.Intensity = kSunLux;
        light.IntensityUnit = Components::LightUnit::Lux;
        harness.World.AddComponentImmediate(lightEntity, light);
        harness.World.AddComponentImmediate(lightEntity, Components::Transform{});

        Components::SkyEnvironment sky{};
        sky.TimeOfDayHours = hour.Hours;
        sky.SunLight = lightEntity;
        harness.World.AddComponentImmediate(harness.World.CreateEntity(), sky);
        const Mathematics::Vector3 sun = harness.RenderedSun();

        const auto* driven = harness.World.GetComponent<Components::Light>(lightEntity);
        ASSERT_NE(driven, nullptr);
        const float delivered = Components::SkySunIlluminance::DeliveredLux(*driven);
        const float elevation = std::asin(sun.y) / kDegreesToRadians;
        const float sunShare = Components::SkySunPath::DrivenSunShareLux(sky, elevation, kSunLux, hour.Hours);
        ASSERT_GT(delivered, 0.0f) << "hour " << hour.Hours;
        if (hour.AllSun)
        {
            EXPECT_EQ(Components::SkySunPath::DrivenMoonBlend(sky, elevation), 0.0f);
            EXPECT_NEAR(sunShare, delivered, delivered * kRelativeTolerance) << "hour " << hour.Hours;
        }
        else if (hour.AllMoon)
        {
            EXPECT_EQ(sunShare, 0.0f) << "hour " << hour.Hours;
            EXPECT_EQ(Components::SkySunPath::DrivenMoonBlend(sky, elevation), 1.0f);
            // At the default equinox midnight the moon is overhead: the light gives the full-night value.
            EXPECT_NEAR(Components::SkySunPath::FullNightMoonLux(sky), delivered, delivered * 1e-3f);
        }
        else
        {
            EXPECT_GT(sunShare, 0.0f) << "hour " << hour.Hours;
            EXPECT_LT(sunShare, delivered) << "hour " << hour.Hours;
            // The moon's term, rebuilt from the rendered moon and the drive's constants: the light's
            // colour is the energy blend (1 - b) sunGround + b moonLightScale moonGround.
            const Rendering::SkySystemConfig config{};
            const float blend = Components::SkySunPath::DrivenMoonBlend(sky, elevation);
            ASSERT_GT(blend, 0.0f);
            ASSERT_LT(blend, 1.0f);
            const auto* feature = harness.Services.GetFeature<Engine::Renderer::SkyRenderFeature>();
            ASSERT_NE(feature, nullptr);
            float moonGround[3];
            Rendering::EvaluateGroundLevelSunColor(Rendering::ScatteringAtmosphere(), config.moonColor,
                                                   feature->GetSettings().moonDirWS[1], moonGround);
            const float moonTerm = kSunLux * blend * config.moonLightIlluminanceScale *
                                   ColorUtils::LinearRec709Luminance(moonGround);
            EXPECT_NEAR(sunShare, delivered - moonTerm, delivered * 1e-3f) << "hour " << hour.Hours;
        }
    }
}

namespace
{
// What the light a sky drives delivers at `hours`, in lux, when its clear, overhead illuminance is
// `lightLux`.
float DrivenLightLuxAt(Components::SkyEnvironment sky, float hours, float lightLux)
{
    SkySunPathHarness harness;
    const ECS::EntityHandle lightEntity = harness.World.CreateEntity();
    Components::Light light{};
    light.Type = Components::LightType::Directional;
    light.Intensity = lightLux;
    light.IntensityUnit = Components::LightUnit::Lux;
    harness.World.AddComponentImmediate(lightEntity, light);
    harness.World.AddComponentImmediate(lightEntity, Components::Transform{});
    sky.TimeOfDayHours = hours;
    sky.SunLight = lightEntity;
    harness.World.AddComponentImmediate(harness.World.CreateEntity(), sky);
    harness.RenderedSun();
    const auto* driven = harness.World.GetComponent<Components::Light>(lightEntity);
    return driven ? Components::SkySunIlluminance::DeliveredLux(*driven) : -1.0f;
}
} // namespace

// The moon rides the sun's circle, so the most moonlight a night has is what the light delivers with
// the moon at the circle's top, where the default moon stands at midnight; the air dims it there as
// it does the sun. A path whose sun never rises has no moonlight: its moon never rises either.
TEST(SkySunPath, TheNightsMoonlightIsTheMoonsAtItsHighest)
{
    constexpr float kSunLux = 100000.0f;
    Components::SkyEnvironment london{};
    london.Latitude = 51.5f;
    london.DayOfYear = Rendering::kMarchEquinoxDay;
    EXPECT_NEAR(Components::SkySunPath::MoonHighestElevationDegrees(london), 38.5f, 0.1f);
    const float delivered = DrivenLightLuxAt(london, 0.0f, kSunLux);
    ASSERT_GT(delivered, 0.0f);
    const float londonNight = Components::SkySunPath::FullNightMoonLux(london);
    EXPECT_NEAR(londonNight, delivered, delivered * 1e-3f);
    EXPECT_LT(londonNight, Components::SkySunPath::FullNightMoonLux(Components::SkyEnvironment{}))
        << "a moon 38.5 degrees up crosses more air than the equator's, overhead";

    Components::SkyEnvironment polarNight{};
    polarNight.Latitude = 75.0f;
    polarNight.DayOfYear = 355;
    EXPECT_LT(Components::SkySunPath::MoonHighestElevationDegrees(polarNight), 0.0f);
    EXPECT_EQ(Components::SkySunPath::FullNightMoonLux(polarNight), 0.0f);
    EXPECT_LT(DrivenLightLuxAt(polarNight, 0.0f, kSunLux), 0.005f) << "and the light gives nothing at its midnight";

    Components::SkyEnvironment hidden = london;
    hidden.ShowMoon = false;
    EXPECT_EQ(Components::SkySunPath::FullNightMoonLux(hidden), 0.0f);
}

namespace
{
// The seeded Custom path put beside the Earth path it was seeded from, every quarter hour of the day.
// The seeded noon height is kept to 0.01 degrees, so the two agree to that.
void ExpectSeededPathMatchesEarth(const Components::SkyEnvironment& seeded, const Rendering::SolarFrame& earth)
{
    const Rendering::SolarFrame custom = Rendering::MakeSolarFrame(Components::SkySunPath::PathAngles(seeded));
    for (int quarter = 0; quarter < 24 * 4; ++quarter)
    {
        const float hours = static_cast<float>(quarter) / 4.0f;
        const Rendering::SolarPosition expected = Rendering::SolarPositionAtHour(earth, hours);
        const Rendering::SolarPosition actual = Rendering::SolarPositionAtHour(custom, hours);
        EXPECT_NEAR(actual.ElevationDegrees, expected.ElevationDegrees, 0.01f) << "hour " << hours;
        const float azimuthDifference = std::abs(actual.AzimuthDegrees - expected.AzimuthDegrees);
        EXPECT_LT(std::min(azimuthDifference, 360.0f - azimuthDifference), 0.02f) << "hour " << hours;
    }
}
} // namespace

// The first switch to Custom starts where the Earth path is: for London on day 172 the seeded axis
// and noon height put the sun where Earth puts it through the day. Once seeded (or authored) the
// Custom fields are not seeded again.
TEST(SkySunPath, SeedingCustomFromEarthKeepsTheSunWhereItIs)
{
    Components::SkyEnvironment sky{};
    sky.Latitude = 51.5f;
    sky.DayOfYear = 172;
    sky.NorthHeading = 30.0f;
    const Rendering::SolarFrame earth = Rendering::MakeSolarFrame(Components::SkySunPath::PathAngles(sky));

    ASSERT_TRUE(Components::SkySunPath::SeedCustomPathFromEarth(sky));
    EXPECT_EQ(sky.CustomAxisHeading, 30.0f);
    EXPECT_EQ(sky.CustomAxisAltitude, 51.5f);
    EXPECT_EQ(sky.CustomNoonHeight, 61.94f) << "90 - 51.5 + 23.44 (the declination on day 172), to 0.01";
    sky.SunPath = Components::SkySunPathKind::Custom;
    ExpectSeededPathMatchesEarth(sky, earth);

    const Components::SkyEnvironment seeded = sky;
    sky.Latitude = 10.0f;
    EXPECT_FALSE(Components::SkySunPath::SeedCustomPathFromEarth(sky)) << "authored Custom fields are kept";
    EXPECT_EQ(sky.CustomAxisAltitude, seeded.CustomAxisAltitude);
    EXPECT_EQ(sky.CustomNoonHeight, seeded.CustomNoonHeight);
}

// The seed is exact at the poles too: in the polar night (75 north on day 355) the Earth noon sun is
// 8.43 degrees under the horizon, which the Custom path reaches as a noon height below 0. The day's
// figures read as the polar night's.
TEST(SkySunPath, SeedingCustomFromEarthKeepsAPolarNightSunWhereItIs)
{
    Components::SkyEnvironment sky{};
    sky.Latitude = 75.0f;
    sky.DayOfYear = 355;
    const Rendering::SolarPathAngles earthAngles = Components::SkySunPath::PathAngles(sky);
    const Rendering::SolarFrame earth = Rendering::MakeSolarFrame(earthAngles);

    ASSERT_TRUE(Components::SkySunPath::SeedCustomPathFromEarth(sky));
    EXPECT_EQ(sky.CustomAxisAltitude, 75.0f);
    EXPECT_LT(sky.CustomNoonHeight, 0.0f) << "the noon sun is under the horizon";
    EXPECT_NEAR(sky.CustomNoonHeight, Rendering::SolarNoonElevationDegrees(earthAngles), 0.01f);
    sky.SunPath = Components::SkySunPathKind::Custom;
    const Rendering::SolarPathAngles customAngles = Components::SkySunPath::PathAngles(sky);
    EXPECT_NEAR(Rendering::SolarNoonElevationDegrees(customAngles), Rendering::SolarNoonElevationDegrees(earthAngles), 0.01f);
    EXPECT_FLOAT_EQ(Rendering::SolarDayLengthHours(customAngles), 0.0f);
    ExpectSeededPathMatchesEarth(sky, earth);
}

// The sky's field setters keep each field in its range, and a new axis altitude brings the stored
// noon height back into the axis's reach in the same edit, so the sky never holds a height its path
// clamps away: 150 at altitude 30 becomes 120 when the axis rises to 60, and a polar-night height of
// -8.44 becomes -5 when the axis drops to 5.
TEST(SkySunPath, AnAxisAltitudeEditBringsTheNoonHeightBackIntoReach)
{
    Components::SkyEnvironment sky{};
    Components::SkySunPath::SetCustomAxisAltitude(sky, 30.0f);
    Components::SkySunPath::SetCustomNoonHeight(sky, 170.0f);
    EXPECT_EQ(sky.CustomNoonHeight, 150.0f) << "the reach at altitude 30 ends at 150";
    Components::SkySunPath::SetCustomAxisAltitude(sky, 60.0f);
    EXPECT_EQ(sky.CustomAxisAltitude, 60.0f);
    EXPECT_EQ(sky.CustomNoonHeight, 120.0f) << "the stored 150 is out of reach at altitude 60";

    Components::SkySunPath::SetCustomAxisAltitude(sky, 75.0f);
    Components::SkySunPath::SetCustomNoonHeight(sky, -8.44f);
    EXPECT_EQ(sky.CustomNoonHeight, -8.44f) << "a noon sun under the horizon is within a polar axis's reach";
    Components::SkySunPath::SetCustomAxisAltitude(sky, 5.0f);
    EXPECT_EQ(sky.CustomNoonHeight, -5.0f) << "the lower bound follows the axis too";

    Components::SkySunPath::SetCustomAxisAltitude(sky, -90.0f);
    Components::SkySunPath::SetCustomNoonHeight(sky, 30.0f);
    EXPECT_EQ(sky.CustomNoonHeight, 90.0f) << "the lowest height an axis straight down reaches";
    Components::SkySunPath::SetCustomAxisAltitude(sky, 200.0f);
    EXPECT_EQ(sky.CustomAxisAltitude, 90.0f);
    Components::SkySunPath::SetCustomAxisHeading(sky, -30.0f);
    EXPECT_EQ(sky.CustomAxisHeading, 330.0f);
    Components::SkySunPath::SetLatitude(sky, -100.0f);
    EXPECT_EQ(sky.Latitude, -90.0f);
    Components::SkySunPath::SetNorthHeading(sky, 365.0f);
    EXPECT_EQ(sky.NorthHeading, 5.0f);
}

// An axis altitude edit in flight moves the axis and leaves the stored noon height alone, so the
// path shows that height brought into reach while the edit passes through an altitude that cannot
// reach it, and has it back when the edit moves on.
TEST(SkySunPath, APreviewedAxisAltitudeLeavesTheStoredNoonHeight)
{
    Components::SkyEnvironment sky{};
    sky.SunPath = Components::SkySunPathKind::Custom;
    Components::SkySunPath::SetCustomAxisAltitude(sky, 75.0f);
    Components::SkySunPath::SetCustomNoonHeight(sky, -50.0f);

    Components::SkySunPath::PreviewCustomAxisAltitude(sky, 7.0f);
    EXPECT_EQ(sky.CustomAxisAltitude, 7.0f);
    EXPECT_EQ(sky.CustomNoonHeight, -50.0f);
    EXPECT_NEAR(Rendering::SolarNoonElevationDegrees(Components::SkySunPath::PathAngles(sky)), -7.0f, 1e-3f)
        << "the path uses the height in the axis's reach";

    Components::SkySunPath::PreviewCustomAxisAltitude(sky, 200.0f);
    EXPECT_EQ(sky.CustomAxisAltitude, 90.0f) << "a preview keeps the altitude in its range";

    Components::SkySunPath::PreviewCustomAxisAltitude(sky, 70.0f);
    EXPECT_NEAR(Rendering::SolarNoonElevationDegrees(Components::SkySunPath::PathAngles(sky)), -50.0f, 1e-3f);
}
