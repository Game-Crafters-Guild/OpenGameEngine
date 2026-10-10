// What a sky drives into its sun light when the illuminance comes from its own curve (the lux curve):
// the Intensity, written in the light's unit; the colour as a hue; the moonlight at night; the sky's
// own brightness reference; the fields handed back when a drive ends; and the seed a first switch
// writes. Each runs SkyEnvironmentSystem::Update on a real World with headless RenderServices.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "Components/Hierarchy.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunDrive.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "Components/Rendering/SkySunPath.h"
#include "Components/Rendering/Skybox.h"
#include "ECS/Entity.h"
#include "ECS/SystemScheduling.h"
#include "ECSModules/Rendering/Systems/RegisterRenderingSystems.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "ECSModules/Rendering/Systems/TimelinePlaybackSystem.h"
#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "ECSModules/Rendering/Systems/RenderExtractionSystem.h"
#include "ECSModules/Rendering/Systems/ValueCurveSystem.h"
#include "Rendering/Sky/AtmosphereTransmittance.h"
#include "Rendering/Sky/SkySystem.h"
#include "SkySunAnchorHarness.h"
#include "Types/ColorUtils.h"

using namespace GameEngine;

namespace
{
namespace SunDrive = Components::SkySunDrive;
namespace SunIlluminance = Components::SkySunIlluminance;

constexpr float kShippedSunLux = 100000.0f;
constexpr float kSkyIrradianceScale = 1.33f; // mirrors SkyEnvironmentSystem.cpp, so a drift shows

// A sky linked to one directional light, driving it from its curve.
struct CurveRig
{
    SkySunAnchorHarness Rig;
    ECS::EntityHandle Sun;

    explicit CurveRig(float lux, Components::LightUnit unit = Components::LightUnit::Lux,
                      Components::SkySunIlluminanceSource source = Components::SkySunIlluminanceSource::Curve)
    {
        Sun = Rig.AddDirectional(lux, unit);
        Components::SkyEnvironment sky{};
        sky.SunLight = Sun;
        sky.SunIlluminanceSource = source;
        Rig.SetSky(sky);
    }

    template <typename Edit> void EditSky(Edit edit)
    {
        Components::SkyEnvironment sky = Rig.Sky();
        edit(sky);
        Rig.SetSky(sky);
    }

    void SetHours(float hours)
    {
        EditSky([hours](Components::SkyEnvironment& sky) { sky.TimeOfDayHours = hours; });
    }

    void Tick() { Rig.System.Update(Rig.World, 0.0f); }

    Components::Light Light() const { return *Rig.World.GetComponent<Components::Light>(Sun); }

    void Author(const Components::Light& light) { Rig.World.AddComponentImmediate(Sun, light); }

    const Rendering::SkySettings& Settings() const
    {
        return Rig.Services.GetFeature<Engine::Renderer::SkyRenderFeature>()->GetSettings();
    }
};

// A curve no seed made: one value all day.
Math::Curve FlatCurve(float lux)
{
    Math::Curve curve{};
    curve.TryInsert(Math::CurveKey{0.0f, lux});
    curve.TryInsert(Math::CurveKey{12.0f, lux});
    return curve;
}

float GroundLuminance(const float source[3], float upDot)
{
    float ground[3];
    Rendering::EvaluateGroundLevelSunColor(Rendering::ScatteringAtmosphere(), source, upDot, ground);
    return ColorUtils::LinearRec709Luminance(ground);
}

// The wave of the execution plan the system named `systemName` runs in, or -1 when it is not in the plan.
int WaveOfName(const ECS::SystemManager& manager, const std::string& systemName)
{
    const ECS::SystemExecutionPlan& plan = manager.GetExecutionPlan();
    for (size_t w = 0; w < plan.Waves.size(); ++w)
        for (const size_t index : plan.Waves[w].SystemIndices)
        {
            const char* name = manager.GetSequentialSystemName(index);
            if (name && name == systemName)
                return static_cast<int>(w);
        }
    return -1;
}

int WaveOf(const ECS::SystemManager& manager, const ECS::ISystem* system)
{
    return system ? WaveOfName(manager, system->GetName()) : -1;
}

// A system that does nothing: a link in a dependency chain the schedule test builds.
class ScheduleProbeSystem : public ECS::ISystem
{
  public:
    const char* GetName() const override { return "ScheduleProbe"; }
    void Update(ECS::World&, float) override {}
};

// Deeper than the production schedule's waves up to the transform hierarchy.
constexpr int kDeepChainLength = 16;

uint64_t LightAndTransformVersions(const ECS::World& world, ECS::EntityHandle light)
{
    return world.GetEntityColumnVersion(light, ECS::GetComponentTypeId<Components::Light>()) +
           world.GetEntityColumnVersion(light, ECS::GetComponentTypeId<Components::Transform>());
}
} // namespace

// While the sun is up (no handover), a Lux light's Intensity is the curve's value exactly: a lux value
// is not sent through the unitless scale and back, which is not exact for about one value in eight. A
// light authored in another unit delivers the same lux. The colour is a hue: unit luminance, with or
// without a colour temperature, and the chromaticity of the sun's ground colour.
TEST(SkySunDrive, TheCurveSetsTheIntensityInTheLightsUnitAndTheColourIsHueOnly)
{
    CurveRig lux(kShippedSunLux);
    CurveRig unitless(kShippedSunLux / Components::kReferenceWhiteNits, Components::LightUnit::Unitless);
    for (CurveRig* rig : {&lux, &unitless})
        rig->EditSky([](Components::SkyEnvironment& sky) {
            sky.SunIlluminanceCurve = SunDrive::PhysicalSeedCurve(sky, 61803.4f);
        });

    int checked = 0;
    for (float hours = 6.5f; hours <= 17.5f; hours += 7.0f / 60.0f)
    {
        lux.SetHours(hours);
        unitless.SetHours(hours);
        lux.Tick();
        unitless.Tick();
        const float curveLux = SunDrive::CurveIlluminanceLux(lux.Rig.Sky().SunIlluminanceCurve, hours);
        ASSERT_EQ(lux.Settings().scatteringSunDir[1] >= Rendering::SunOnlyUpDot(Rendering::SkySystemConfig{}), true)
            << "hour " << hours << " is in the handover; move the window";
        EXPECT_EQ(lux.Light().Intensity, curveLux) << "hour " << hours;
        const float unitlessLux = SunIlluminance::LuxFromLightIntensity(unitless.Light().Intensity,
                                                                        Components::LightUnit::Unitless);
        EXPECT_LE(std::abs(unitlessLux - curveLux), curveLux * std::numeric_limits<float>::epsilon())
            << "hour " << hours;

        const Components::Light light = lux.Light();
        EXPECT_NEAR(ColorUtils::LinearRec709Luminance(light.Color), 1.0f, 1e-6f) << "hour " << hours;
        const float white[3] = {1.0f, 1.0f, 1.0f};
        float ground[3];
        Rendering::EvaluateGroundLevelSunColor(Rendering::ScatteringAtmosphere(), white,
                                               lux.Settings().scatteringSunDir[1], ground);
        const float groundLuminance = ColorUtils::LinearRec709Luminance(ground);
        for (int c = 0; c < 3; ++c)
            EXPECT_NEAR(light.Color[c], ground[c] / groundLuminance, 1e-5f) << "hour " << hours << ", channel " << c;
        ++checked;
    }
    EXPECT_GT(checked, 90);

    // With a colour temperature the sky divides it out, so the colour the light resolves to is still
    // a hue at unit luminance.
    Components::Light warm = lux.Light();
    warm.UseColorTemperature = true;
    warm.ColorTemperature = 3200.0f;
    lux.Author(warm);
    lux.SetHours(15.0f);
    lux.Tick();
    float resolved[3];
    float unitlessIntensity = 0.0f;
    Components::ResolveLightColorIntensity(lux.Light(), resolved, unitlessIntensity);
    EXPECT_NEAR(ColorUtils::LinearRec709Luminance(resolved), 1.0f, 1e-6f);
}

// Below the twilight floor the light is the moon's: whatever the curve reads at midnight, the light
// delivers the moonlight at the moon's elevation.
TEST(SkySunDrive, AtNightTheMoonlightOwnsTheLight)
{
    CurveRig rig(kShippedSunLux);
    rig.EditSky([](Components::SkyEnvironment& sky) { sky.SunIlluminanceCurve = FlatCurve(50000.0f); });
    rig.SetHours(0.0f);
    rig.Tick();
    const float moonlight = SunDrive::MoonlightLux(rig.Rig.Sky(), rig.Settings().moonDirWS[1]);
    ASSERT_GT(moonlight, 1.0f);
    EXPECT_NEAR(SunIlluminance::DeliveredLux(rig.Light()), moonlight, moonlight * 1e-4f);
}

// The sky's own sun follows the curve at the current hour (a clear sun scaled by the curve over the
// physical model, held at the ends of the sun-only hours), never the Intensity the curve wrote.
// A curve reshaped at one hour moves the sky only there: with only the noon key dragged down, the
// afternoon's sky keeps the clear sun its unchanged keys give, so the dome matches the light the curve
// writes at 16:00 instead of the dimmed noon.
TEST(SkySunDrive, TheSkyFollowsTheCurveAtTheCurrentHour)
{
    CurveRig rig(kShippedSunLux);
    rig.EditSky([](Components::SkyEnvironment& sky) {
        sky.SunIlluminanceCurve = SunDrive::DefaultSunIlluminanceCurve();
        for (uint8_t k = 0; k < sky.SunIlluminanceCurve.KeyCount; ++k)
            if (sky.SunIlluminanceCurve.Keys[k].Time == 12.0f)
                sky.SunIlluminanceCurve.Keys[k].Value = 11775.0f;
    });
    rig.SetHours(16.0f);
    rig.Tick();
    const float atSixteen = rig.Settings().primarySunIntensity;

    CurveRig seeded(kShippedSunLux);
    seeded.EditSky([](Components::SkyEnvironment& sky) { sky.SunIlluminanceCurve = SunDrive::DefaultSunIlluminanceCurve(); });
    seeded.SetHours(16.0f);
    seeded.Tick();
    const float seededAtSixteen = seeded.Settings().primarySunIntensity;
    ASSERT_GT(seededAtSixteen, 0.0f);
    EXPECT_NEAR(atSixteen, seededAtSixteen, seededAtSixteen * 0.02f)
        << "the sky at 16:00 follows the noon key instead of the curve at 16:00";

    // At noon itself the sky follows the dragged key.
    rig.SetHours(12.0f);
    rig.Tick();
    EXPECT_NEAR(rig.Settings().primarySunIntensity / seededAtSixteen, 11775.0f / kShippedSunLux, 0.02f);
}

TEST(SkySunDrive, TheSkyFollowsTheCurvesNoon)
{
    CurveRig rig(kShippedSunLux);
    rig.EditSky([](Components::SkyEnvironment& sky) {
        sky.Latitude = 51.5f;
        sky.DayOfYear = 355;
        sky.SunIlluminanceCurve = SunDrive::PhysicalSeedCurve(sky, 40000.0f);
    });
    const float reference = SunDrive::NoonReferenceLux(rig.Rig.Sky());
    for (const float hours : {12.0f, 13.5f, 15.0f, 16.0f, 17.0f, 18.0f, 19.0f})
    {
        rig.SetHours(hours);
        rig.Tick();
        if (rig.Settings().scatteringSunDir[1] < 0.0f)
            continue;
        // primarySunIntensity is the sky's day source while the moon has not taken over.
        const float expected = Components::LightIntensityToUnitless(SunDrive::SkySourceLux(rig.Rig.Sky(), hours),
                                                                    Components::LightUnit::Lux) *
                               kSkyIrradianceScale;
        if (hours < 15.5f)
            EXPECT_EQ(rig.Settings().primarySunIntensity, expected) << "hour " << hours;
    }
    // At noon the sky's source is the noon reference.
    EXPECT_NEAR(SunDrive::SkySourceLux(rig.Rig.Sky(), 12.0f), reference, reference * 1e-4f);

    // The sky's sun at noon delivers the curve's noon: the reference times the noon extinction.
    const Components::SkyEnvironment sky = rig.Rig.Sky();
    const float noonElevation = Rendering::SolarNoonElevationDegrees(Components::SkySunPath::PathAngles(sky));
    const float white[3] = {1.0f, 1.0f, 1.0f};
    const float noonDelivered = reference * GroundLuminance(white, std::sin(noonElevation * 0.01745329252f));
    const float curveNoon = SunDrive::CurveIlluminanceLux(sky.SunIlluminanceCurve, 12.0f);
    EXPECT_NEAR(noonDelivered, curveNoon, curveNoon * 1e-5f);

    // Continuous across the 1.15-degree floor: on 21 December the noon sun crosses it near 65.4 N.
    Components::SkyEnvironment polar = sky;
    polar.SunIlluminanceCurve = FlatCurve(10000.0f);
    float previous = 0.0f;
    for (float latitude = 64.9f; latitude <= 66.0f; latitude += 0.01f)
    {
        polar.Latitude = latitude;
        const float r = SunDrive::NoonReferenceLux(polar);
        if (previous > 0.0f)
            EXPECT_LT(std::abs(std::log2(r / previous)), 0.05f) << "latitude " << latitude;
        previous = r;
    }
}

namespace
{
struct Ending
{
    const char* How;
    void (*Apply)(CurveRig& rig);
    bool EndsColour; // false: the colour stays the sky's, as it is while the light holds the illuminance
};

// Start the drive at dusk on a fresh system, which never saw it start (as when a scene opens with it
// running), so every ending hands the Intensity back as the noon reference.
void RunCurveDriveAtDusk(CurveRig& rig)
{
    rig.SetHours(18.5f);
    rig.Tick();
}
} // namespace

// Every way a drive ends hands each field it wrote back: the colour to white, the Intensity to the
// noon reference (no authored value was recorded here). Switching back to the light ends only the
// Intensity, because the colour then carries the night; unchecking the colour ends only the colour.
TEST(SkySunDrive, EachDriveThatEndsHandsItsFieldBack)
{
    const Ending endings[] = {
        {"source switched back to the light",
         [](CurveRig& rig) {
             rig.EditSky([](Components::SkyEnvironment& sky) {
                 sky.SunIlluminanceSource = Components::SkySunIlluminanceSource::Light;
             });
         },
         false},
        {"link cleared", [](CurveRig& rig) { rig.EditSky([](Components::SkyEnvironment& sky) { sky.SunLight = {}; }); },
         true},
        {"sky disabled",
         [](CurveRig& rig) {
             ECS::Entity(&rig.Rig.World, rig.Rig.SkyEntity).SetEnabled<Components::SkyEnvironment>(false);
         },
         true},
        {"sky entity deleted", [](CurveRig& rig) { rig.Rig.World.DestroyEntityImmediate(rig.Rig.SkyEntity); }, true},
        {"HDRI skybox added",
         [](CurveRig& rig) {
             Components::Skybox box{};
             box.HDRIAssetGuid[0] = 'x';
             rig.Rig.World.AddComponentImmediate(rig.Rig.World.CreateEntity(), box);
         },
         true},
    };
    for (const Ending& ending : endings)
    {
        CurveRig rig(kShippedSunLux);
        RunCurveDriveAtDusk(rig);
        const float reference = SunDrive::NoonReferenceLux(rig.Rig.Sky());
        ASSERT_LT(rig.Light().Intensity, reference * 0.01f) << ending.How << ": dusk must dim the driven light";
        ending.Apply(rig);
        rig.Tick();
        for (int c = 0; c < 3; ++c)
            EXPECT_EQ(rig.Light().Color[c] == 1.0f, ending.EndsColour) << ending.How << ", channel " << c;
        EXPECT_EQ(rig.Light().Intensity, reference) << ending.How;
    }

    // Back on the light at noon the light delivers what the curve delivered there.
    {
        CurveRig rig(kShippedSunLux);
        RunCurveDriveAtDusk(rig);
        rig.SetHours(12.0f);
        rig.Tick();
        const float curveNoon = SunIlluminance::DeliveredLux(rig.Light());
        rig.EditSky([](Components::SkyEnvironment& sky) {
            sky.SunIlluminanceSource = Components::SkySunIlluminanceSource::Light;
        });
        rig.Tick();
        rig.Tick();
        EXPECT_NEAR(SunIlluminance::DeliveredLux(rig.Light()), curveNoon, curveNoon * 1e-5f);
    }

    // Unchecking the colour ends the colour drive and keeps the curve's Intensity.
    {
        CurveRig rig(kShippedSunLux);
        RunCurveDriveAtDusk(rig);
        const float intensity = rig.Light().Intensity;
        rig.EditSky([](Components::SkyEnvironment& sky) { sky.DriveSunColor = false; });
        rig.Tick();
        for (int c = 0; c < 3; ++c)
            EXPECT_EQ(rig.Light().Color[c], 1.0f) << "colour unchecked, channel " << c;
        EXPECT_EQ(rig.Light().Intensity, intensity) << "colour unchecked";
    }
}

// A value the author put on the light after the sky wrote it is the author's: ending the drive
// leaves it.
TEST(SkySunDrive, AnAuthorsValueIsNeverOverwrittenOnRelease)
{
    CurveRig rig(kShippedSunLux);
    RunCurveDriveAtDusk(rig);
    Components::Light authored = rig.Light();
    authored.Intensity = 777.0f;
    authored.Color[0] = 0.5f;
    rig.Author(authored);
    rig.EditSky([](Components::SkyEnvironment& sky) { sky.SunLight = {}; });
    rig.Tick();
    EXPECT_EQ(rig.Light().Intensity, 777.0f);
    EXPECT_EQ(rig.Light().Color[0], 0.5f);
}

// At a time of day that does not move, nothing about the light changes after the drive has settled,
// so the drive takes no write on it: an idle editor frame stamps nothing.
TEST(SkySunDrive, AStaticTimeStampsNothing)
{
    CurveRig rig(kShippedSunLux);
    rig.SetHours(16.25f);
    rig.Tick();
    rig.Tick();
    const uint64_t before = LightAndTransformVersions(rig.Rig.World, rig.Sun);
    rig.Tick();
    EXPECT_EQ(LightAndTransformVersions(rig.Rig.World, rig.Sun), before);
}

// While the light holds the illuminance its colour carries the night, so the sky writes it whatever
// the colour toggle says: unchecked, the night is still the moon's, not the light's noon value.
TEST(SkySunDrive, ColourIsAlwaysDrivenWhileTheLightHoldsTheIlluminance)
{
    CurveRig rig(kShippedSunLux, Components::LightUnit::Lux, Components::SkySunIlluminanceSource::Light);
    rig.EditSky([](Components::SkyEnvironment& sky) { sky.DriveSunColor = false; });
    rig.SetHours(0.0f);
    rig.Tick();
    EXPECT_LT(SunIlluminance::DeliveredLux(rig.Light()), 10.0f) << "a night lit at noon strength";
}

// The seed rises from midnight to noon and falls after it, minute by minute, and stays within half a
// stop of the model wherever the model gives light: no dip, bump or darkness after sunset. Near noon
// the seed's keys, at three significant figures, can be equal, and a smooth segment across them and the
// log-domain round trip exp2(log2(v)) sit a few parts per million either side of flat, so "rises"
// allows ten parts per million and nothing more. No key sits below the curve's floor, and the default
// sky's seed is the default curve.
TEST(SkySunDrive, TheSeedIsMonotoneAndFollowsThePhysicalCurve)
{
    EXPECT_TRUE(SunDrive::IsDefaultSunIlluminanceCurve(SunDrive::PhysicalSeedCurve(Components::SkyEnvironment{},
                                                                                   kShippedSunLux)));
    struct Site
    {
        float Latitude;
        int32_t Day;
    };
    for (const Site site : {Site{0.0f, Rendering::kMarchEquinoxDay}, Site{51.5f, 172}, Site{51.5f, 355}})
    {
        Components::SkyEnvironment sky{};
        sky.Latitude = site.Latitude;
        sky.DayOfYear = site.Day;
        const Math::Curve seed = SunDrive::PhysicalSeedCurve(sky, kShippedSunLux);
        EXPECT_LE(seed.KeyCount, Math::Curve::Capacity);
        for (int key = 0; key < seed.KeyCount; ++key)
            EXPECT_GE(seed.Keys[key].Value, SunDrive::kCurveFloorLux) << "key " << key;
        float previous = 0.0f;
        for (int minute = 0; minute <= 24 * 60; ++minute)
        {
            const float hours = static_cast<float>(minute) / 60.0f;
            const float value = SunDrive::CurveIlluminanceLux(seed, hours);
            constexpr float kLogRoundTrip = 1e-5f;
            if (minute > 0 && minute <= 12 * 60)
                EXPECT_GE(value, previous * (1.0f - kLogRoundTrip))
                    << "latitude " << site.Latitude << " day " << site.Day << " at " << hours;
            if (minute > 12 * 60)
                EXPECT_LE(value, previous * (1.0f + kLogRoundTrip))
                    << "latitude " << site.Latitude << " day " << site.Day << " at " << hours;
            previous = value;
            const float model = SunDrive::PhysicalIlluminanceLux(sky, kShippedSunLux, hours);
            if (model > 0.01f)
                EXPECT_LE(std::abs(std::log2(std::max(value, SunDrive::kCurveFloorLux) / model)), 0.5f)
                    << "latitude " << site.Latitude << " day " << site.Day << " at " << hours;
        }
    }
}

// The order the frame runs the sky in, from the production schedule: after the other writers of a
// light's intensity (value curves, timeline tracks), so the curve's write is the frame's last, and
// before the transform hierarchy and render extraction, so this frame's sun is what they read.
TEST(SkySunDrive, TheSkyRunsAfterTheIntensityWritersAndBeforeTheHierarchy)
{
    ECS::SystemManager manager;
    {
        ECS::SystemScheduleBuilder builder;
        Engine::Renderer::AddRenderingSystemsToSchedule(builder, nullptr);
        builder.BuildAndRegisterWithWaves(manager);
    }
    const int sky = WaveOf(manager, manager.GetSystem<Engine::Renderer::SkyEnvironmentSystem>());
    ASSERT_GE(sky, 0);
    EXPECT_GT(sky, WaveOf(manager, manager.GetSystem<Engine::Renderer::ValueCurveSystem>()));
    EXPECT_GT(sky, WaveOf(manager, manager.GetSystem<Engine::Renderer::TimelinePlaybackSystem>()));
    EXPECT_LT(sky, WaveOf(manager, manager.GetSystem<Engine::Renderer::TransformHierarchySystem>()));
    EXPECT_LT(sky, WaveOf(manager, manager.GetSystem<Engine::Renderer::RenderExtractionSystem>()));

    // The hierarchy and extraction wait for the sky by edges of their own, not because the sky happens to
    // sit at a shallower wave: a sky placed behind a chain deeper than either still runs before both. The
    // first registration of a name wins, so the stand-in registered first takes the sky's place.
    ECS::SystemManager deep;
    {
        ECS::SystemScheduleBuilder builder;
        std::string previous;
        for (int link = 0; link < kDeepChainLength; ++link)
        {
            const std::string name = "SkyDepthProbe" + std::to_string(link);
            builder.Add<ScheduleProbeSystem>(name, ECS::SystemPhase::Early, 0,
                                             previous.empty() ? std::vector<ECS::SystemDependency>{}
                                                              : std::vector<ECS::SystemDependency>{previous});
            previous = name;
        }
        builder.Add<ScheduleProbeSystem>("SkyEnvironment", ECS::SystemPhase::Camera, 1,
                                         std::vector<ECS::SystemDependency>{previous});
        Engine::Renderer::AddRenderingSystemsToSchedule(builder, nullptr);
        builder.BuildAndRegisterWithWaves(deep);
    }
    const int deepSky = WaveOfName(deep, "SkyEnvironment");
    ASSERT_GE(deepSky, kDeepChainLength);
    EXPECT_LT(deepSky, WaveOf(deep, deep.GetSystem<Engine::Renderer::TransformHierarchySystem>()));
    EXPECT_LT(deepSky, WaveOf(deep, deep.GetSystem<Engine::Renderer::RenderExtractionSystem>()));
}

// Switching to the curve and back is lossless: the light reads the value its author gave it, after a
// dusk under the curve, after the unit changed during the drive, and when the drive ended by unlinking.
TEST(SkySunDrive, SwitchingToTheCurveAndBackReturnsTheAuthoredIntensity)
{
    enum class Variant
    {
        SwitchBack,
        UnitChangedMidDrive,
        Unlinked,
    };
    for (const Variant variant : {Variant::SwitchBack, Variant::UnitChangedMidDrive, Variant::Unlinked})
    {
        CurveRig rig(40000.0f, Components::LightUnit::Lux, Components::SkySunIlluminanceSource::Light);
        rig.SetHours(17.5f);
        rig.Tick();
        rig.EditSky([](Components::SkyEnvironment& sky) {
            sky.SunIlluminanceSource = Components::SkySunIlluminanceSource::Curve;
        });
        for (int frame = 0; frame < 200; ++frame)
        {
            rig.SetHours(17.5f + 0.01f * static_cast<float>(frame));
            rig.Tick();
            if (variant == Variant::UnitChangedMidDrive && frame == 100)
            {
                // The way the light inspector changes a unit: the same illuminance in the new unit.
                Components::Light light = rig.Light();
                light.Intensity = Components::UnitlessToLightIntensity(
                    Components::LightIntensityToUnitless(light.Intensity, light.IntensityUnit),
                    Components::LightUnit::Unitless);
                light.IntensityUnit = Components::LightUnit::Unitless;
                rig.Author(light);
            }
        }
        if (variant == Variant::Unlinked)
            rig.EditSky([](Components::SkyEnvironment& sky) { sky.SunLight = {}; });
        else
            rig.EditSky([](Components::SkyEnvironment& sky) {
                sky.SunIlluminanceSource = Components::SkySunIlluminanceSource::Light;
            });
        rig.Tick();
        const Components::Light light = rig.Light();
        EXPECT_EQ(light.Intensity, SunIlluminance::LightIntensityFromLux(40000.0f, light.IntensityUnit))
            << "variant " << static_cast<int>(variant);
    }
}

// A system that meets a light the curve already drives (a scene opened with the drive running) did not
// see the author's value, so it records nothing and hands back the noon reference, not the dusk value
// the scene stored.
TEST(SkySunDrive, ADriveAlreadyRunningOnLoadFallsBackToTheNoonReference)
{
    CurveRig rig(123.0f);
    rig.SetHours(18.4f);
    rig.Tick();
    rig.Tick();
    const float reference = SunDrive::NoonReferenceLux(rig.Rig.Sky());
    rig.EditSky([](Components::SkyEnvironment& sky) {
        sky.SunIlluminanceSource = Components::SkySunIlluminanceSource::Light;
    });
    rig.Tick();
    EXPECT_EQ(rig.Light().Intensity, reference);
}

// The first switch seeds only a curve no one has authored; an authored curve is kept as it is.
TEST(SkySunDrive, SwitchingToTheCurveKeepsAnAuthoredCurveAndSeedsOnlyTheDefault)
{
    Components::SkyEnvironment untouched{};
    untouched.Latitude = 51.5f;
    untouched.DayOfYear = 172;
    ASSERT_TRUE(SunDrive::IsDefaultSunIlluminanceCurve(untouched.SunIlluminanceCurve));
    EXPECT_TRUE(SunDrive::SeedCurveIfUnauthored(untouched, 60000.0f));
    const Math::Curve seed = SunDrive::PhysicalSeedCurve(untouched, 60000.0f);
    EXPECT_EQ(std::memcmp(&untouched.SunIlluminanceCurve, &seed, sizeof(Math::Curve)), 0);

    Components::SkyEnvironment authored{};
    authored.SunIlluminanceCurve = FlatCurve(20000.0f);
    const Math::Curve before = authored.SunIlluminanceCurve;
    EXPECT_FALSE(SunDrive::SeedCurveIfUnauthored(authored, 60000.0f));
    EXPECT_EQ(std::memcmp(&authored.SunIlluminanceCurve, &before, sizeof(Math::Curve)), 0);
}

namespace
{
// A scene switch inside one editor session: the editor clears the world and loads the next scene into
// it, the sky system persists, and World::Clear recycles entity indices.
struct SceneSwitchRig
{
    SkySunAnchorHarness Rig;
    ECS::EntityHandle Sun;

    void Open(float lightLux, Components::SkySunIlluminanceSource source, float hours)
    {
        Rig.World.Clear();
        Rig.SkyEntity = {};
        Sun = Rig.AddDirectional(lightLux, Components::LightUnit::Lux);
        Components::SkyEnvironment sky{};
        sky.SunLight = Sun;
        sky.SunIlluminanceSource = source;
        sky.TimeOfDayHours = hours;
        Rig.SetSky(sky);
    }

    void Tick() { Rig.System.Update(Rig.World, 0.0f); }

    float Intensity() const { return Rig.World.GetComponent<Components::Light>(Sun)->Intensity; }

    void SetSource(Components::SkySunIlluminanceSource source)
    {
        Components::SkyEnvironment sky = Rig.Sky();
        sky.SunIlluminanceSource = source;
        Rig.SetSky(sky);
    }
};
} // namespace

// A scene opened with the curve already driving, after another scene in the same session: the drive
// started unseen, so ending it hands back the noon reference, not the dusk value the scene stored.
TEST(SkySunDrive, ASceneOpenedWithTheCurveDrivingAfterAnotherFallsBackToTheNoonReference)
{
    SceneSwitchRig rig;
    rig.Open(40000.0f, Components::SkySunIlluminanceSource::Light, 12.0f);
    rig.Tick();
    rig.Open(3.0f, Components::SkySunIlluminanceSource::Curve, 18.4f);
    rig.Tick();
    rig.Tick();
    const float reference = SunDrive::NoonReferenceLux(rig.Rig.Sky());
    rig.SetSource(Components::SkySunIlluminanceSource::Light);
    rig.Tick();
    EXPECT_EQ(rig.Intensity(), reference) << "the next scene's stored dusk value was recorded as the author's";
}

// The value recorded when one scene's drive started never reaches the next scene's light.
TEST(SkySunDrive, AnAuthoredRecordDoesNotCrossIntoTheNextScene)
{
    SceneSwitchRig rig;
    rig.Open(40000.0f, Components::SkySunIlluminanceSource::Light, 17.5f);
    rig.Tick();
    rig.SetSource(Components::SkySunIlluminanceSource::Curve);
    rig.Tick();
    rig.Tick();
    rig.Open(3.0f, Components::SkySunIlluminanceSource::Curve, 18.4f);
    rig.Tick();
    rig.Tick();
    const float reference = SunDrive::NoonReferenceLux(rig.Rig.Sky());
    rig.SetSource(Components::SkySunIlluminanceSource::Light);
    rig.Tick();
    EXPECT_NE(rig.Intensity(), 40000.0f) << "the first scene's authored Intensity reached the next scene's light";
    EXPECT_EQ(rig.Intensity(), reference);
}
