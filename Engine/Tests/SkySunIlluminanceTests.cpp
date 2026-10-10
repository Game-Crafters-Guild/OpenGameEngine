// The sky's Sun illuminance row edits one value, in lux, stored on the sky's linked directional
// light. The row's logic lives in SkySunIlluminance so it can be tested here, without the editor:
// which light the row may edit, which light the sky reads, and the conversion between lux and the
// light's own unit.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "Components/Hierarchy.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

using namespace GameEngine;
namespace SunIlluminance = Components::SkySunIlluminance;

namespace
{

ECS::EntityHandle AddLight(ECS::World& world, Components::LightType type, float intensity,
                           Components::LightUnit unit)
{
    const ECS::EntityHandle e = world.CreateEntity();
    Components::Light light{};
    light.Type = type;
    light.Intensity = intensity;
    light.IntensityUnit = unit;
    world.AddComponentImmediate(e, light);
    world.AddComponentImmediate(e, Components::Transform{});
    return e;
}

ECS::EntityHandle AddDirectional(ECS::World& world, float lux)
{
    return AddLight(world, Components::LightType::Directional, lux, Components::LightUnit::Lux);
}

} // namespace

// The row edits the linked light and nothing else. With no link it has nothing to edit, even
// though the sky still reads the scene's brightest directional light: an edit that landed on a
// light the row never named could change target under the author between two edits.
TEST(SkySunIlluminance, ResolvesTheLinkedLightOnlyForEditing)
{
    ECS::World world;
    const ECS::EntityHandle key = AddDirectional(world, 20000.0f);
    const ECS::EntityHandle brighter = AddDirectional(world, Components::kClearNoonSunIlluminanceLux);

    Components::SkyEnvironment unlinked{};
    EXPECT_FALSE(SunIlluminance::EditableSunLight(world, unlinked).IsValid())
        << "an unlinked sky must not edit any light";
    EXPECT_EQ(SunIlluminance::ResolvedSunLight(world, unlinked), brighter)
        << "an unlinked sky reads the scene's brightest directional light";

    Components::SkyEnvironment linked{};
    linked.SunLight = key;
    EXPECT_EQ(SunIlluminance::EditableSunLight(world, linked), key);
    EXPECT_EQ(SunIlluminance::ResolvedSunLight(world, linked), key)
        << "the link outranks a brighter directional light";

    // A link to something that is not a directional light is no sun to edit.
    const ECS::EntityHandle lamp =
        AddLight(world, Components::LightType::Point, 1500.0f, Components::LightUnit::Lumen);
    Components::SkyEnvironment linkedToLamp{};
    linkedToLamp.SunLight = lamp;
    EXPECT_FALSE(SunIlluminance::EditableSunLight(world, linkedToLamp).IsValid());
    EXPECT_EQ(SunIlluminance::ResolvedSunLight(world, linkedToLamp), brighter);

    // No emitting directional light at all: no sun.
    ECS::World empty;
    EXPECT_FALSE(SunIlluminance::ResolvedSunLight(empty, unlinked).IsValid());
}

// Driving needs a light the sky can rotate: a directional light with a Transform and no parent,
// linked to an enabled sky whose time of day drives it.
TEST(SkySunIlluminance, OnlyAnEnabledDrivingSkyDrivesAnUnparentedDirectionalLight)
{
    ECS::World world;
    const ECS::EntityHandle sun = AddDirectional(world, Components::kClearNoonSunIlluminanceLux);

    Components::SkyEnvironment sky{};
    sky.SunLight = sun;
    EXPECT_EQ(SunIlluminance::LightSkyWouldDrive(world, sky), sun);

    Components::SkyEnvironment following = sky;
    following.TimeOfDayDrivesSunLight = false;
    EXPECT_FALSE(SunIlluminance::LightSkyWouldDrive(world, following).IsValid());

    const ECS::EntityHandle holder = world.CreateEntity();
    Components::Parent parent{};
    parent.parent = holder;
    world.AddComponentImmediate(sun, parent);
    EXPECT_FALSE(SunIlluminance::IsDrivableSunLight(world, sun)) << "the drive writes a local rotation";
    EXPECT_FALSE(SunIlluminance::LightSkyWouldDrive(world, sky).IsValid());
}

// The row shows lux whatever the light's unit, and writes back in the light's unit. Both directions
// go through the unitless scale, so a round trip returns the value an author typed.
TEST(SkySunIlluminance, ConvertsLuxToAndFromEveryDirectionalUnit)
{
    constexpr float kLux = 20000.0f;
    for (const Components::LightUnit unit :
         {Components::LightUnit::Lux, Components::LightUnit::Unitless, Components::LightUnit::Candela,
          Components::LightUnit::Lumen})
    {
        const float stored = SunIlluminance::LightIntensityFromLux(kLux, unit);
        EXPECT_NEAR(SunIlluminance::LuxFromLightIntensity(stored, unit), kLux, kLux * 1e-6f)
            << "unit " << static_cast<int>(unit);
        // The light's consumers read the same brightness whichever unit holds it.
        EXPECT_NEAR(Components::LightIntensityToUnitless(stored, unit),
                    Components::LightIntensityToUnitless(kLux, Components::LightUnit::Lux),
                    kLux * 1e-8f)
            << "unit " << static_cast<int>(unit);
    }

    EXPECT_EQ(SunIlluminance::LightIntensityFromLux(kLux, Components::LightUnit::Lux), kLux)
        << "a lux light stores exactly what the author typed";
    EXPECT_NEAR(SunIlluminance::LightIntensityFromLux(Components::kReferenceWhiteNits,
                                                      Components::LightUnit::Unitless),
                1.0f, 1e-6f)
        << "203 lx is unitless 1";
}

// What the readout shows: the lux a surface facing the light receives, its intensity times the
// luminance of its colour. White delivers the intensity; a black light delivers nothing.
TEST(SkySunIlluminance, TheDeliveredLuxIsTheIntensityTimesTheColoursLuminance)
{
    Components::Light light{};
    light.Type = Components::LightType::Directional;
    light.Intensity = 50000.0f;
    light.IntensityUnit = Components::LightUnit::Lux;
    EXPECT_NEAR(SunIlluminance::DeliveredLux(light), 50000.0f, 0.5f);

    light.Color[0] = 0.0f;
    light.Color[1] = 0.5f;
    light.Color[2] = 0.0f;
    EXPECT_NEAR(SunIlluminance::DeliveredLux(light), 50000.0f * 0.5f * 0.7152f, 0.5f);

    light.Color[1] = 0.0f;
    EXPECT_EQ(SunIlluminance::DeliveredLux(light), 0.0f);
}

// The light inspector names the sky that drives its light. Only the sky the renderer uses (the
// first enabled one) drives anything, and only while its time of day drives the light.
TEST(SkySunIlluminance, FindsTheSkyThatDrivesALight)
{
    ECS::World world;
    const ECS::EntityHandle sun = AddDirectional(world, Components::kClearNoonSunIlluminanceLux);
    const ECS::EntityHandle unlinkedLight = AddDirectional(world, 500.0f);

    const ECS::EntityHandle skyEntity = world.CreateEntity();
    Components::SkyEnvironment sky{};
    sky.SunLight = sun;
    world.AddComponentImmediate(skyEntity, sky);

    EXPECT_EQ(SunIlluminance::SkyDrivingLight(world, sun), skyEntity);
    EXPECT_FALSE(SunIlluminance::SkyDrivingLight(world, unlinkedLight).IsValid());

    sky.TimeOfDayDrivesSunLight = false;
    world.AddComponentImmediate(skyEntity, sky);
    EXPECT_FALSE(SunIlluminance::SkyDrivingLight(world, sun).IsValid()) << "a following sky drives nothing";
}

// Only the sky the system renders drives anything. A second enabled sky linked to another light
// drives nothing; with the first sky disabled, the second is the rendered one and drives its light.
TEST(SkySunIlluminance, OnlyTheRenderedSkyDrivesALight)
{
    ECS::World world;
    const ECS::EntityHandle sun = AddDirectional(world, Components::kClearNoonSunIlluminanceLux);
    const ECS::EntityHandle fill = AddDirectional(world, 500.0f);

    const ECS::EntityHandle first = world.CreateEntity();
    Components::SkyEnvironment firstSky{};
    firstSky.SunLight = sun;
    world.AddComponentImmediate(first, firstSky);
    const ECS::EntityHandle second = world.CreateEntity();
    Components::SkyEnvironment secondSky{};
    secondSky.SunLight = fill;
    world.AddComponentImmediate(second, secondSky);

    EXPECT_EQ(SunIlluminance::RenderedSky(world), first);
    EXPECT_EQ(SunIlluminance::DrivenSunLight(world), sun);
    EXPECT_EQ(SunIlluminance::SkyDrivingLight(world, sun), first);
    EXPECT_FALSE(SunIlluminance::SkyDrivingLight(world, fill).IsValid()) << "the second sky drives nothing";

    ECS::Entity(&world, first).SetEnabled<Components::SkyEnvironment>(false);
    EXPECT_EQ(SunIlluminance::RenderedSky(world), second);
    EXPECT_EQ(SunIlluminance::DrivenSunLight(world), fill);

    // A deactivated entity is not rendered either: the system's query skips it.
    ECS::Entity(&world, first).SetEnabled<Components::SkyEnvironment>(true);
    ASSERT_TRUE(world.SetEntityEnabledImmediate(first, false));
    EXPECT_EQ(SunIlluminance::RenderedSky(world), second);
}

// An enabled skybox with an HDRI replaces the sky outright: the system renders the skybox and
// drives no light, so nothing reports the light as driven.
TEST(SkySunIlluminance, AnHdriSkyboxDrivesNothing)
{
    ECS::World world;
    const ECS::EntityHandle sun = AddDirectional(world, Components::kClearNoonSunIlluminanceLux);
    const ECS::EntityHandle skyEntity = world.CreateEntity();
    Components::SkyEnvironment sky{};
    sky.SunLight = sun;
    world.AddComponentImmediate(skyEntity, sky);
    ASSERT_EQ(SunIlluminance::DrivenSunLight(world), sun);

    const ECS::EntityHandle boxEntity = world.CreateEntity();
    Components::Skybox box{};
    world.AddComponentImmediate(boxEntity, box);
    EXPECT_EQ(SunIlluminance::DrivenSunLight(world), sun) << "a skybox with no HDRI does not replace the sky";

    std::snprintf(box.HDRIAssetGuid, sizeof(box.HDRIAssetGuid), "%s", "0123456789abcdef0123456789abcdef");
    world.AddComponentImmediate(boxEntity, box);
    EXPECT_FALSE(SunIlluminance::RenderedSky(world).IsValid());
    EXPECT_FALSE(SunIlluminance::DrivenSunLight(world).IsValid());
    EXPECT_FALSE(SunIlluminance::SkyDrivingLight(world, sun).IsValid());
}

// The fields of a light the sky drives, as the light inspector disables them and the sky system writes
// them: the direction always; the colour while the light holds the illuminance or while the colour
// toggle is on; the Intensity under the curve. A light no sky drives has none: unlinked, a parented
// light, the drive off, or an HDRI skybox in place of the sky.
TEST(SkySunIlluminance, ReportsTheDrivenFields)
{
    using Components::SkySunIlluminanceSource;
    constexpr uint8_t kDirection = SunIlluminance::kDrivesDirection;
    constexpr uint8_t kColor = SunIlluminance::kDrivesColor;
    constexpr uint8_t kIntensity = SunIlluminance::kDrivesIntensity;
    struct Mode
    {
        SkySunIlluminanceSource Source;
        bool DriveColor;
        uint8_t Expected;
    };
    for (const Mode mode : {Mode{SkySunIlluminanceSource::Light, true, uint8_t(kDirection | kColor)},
                            Mode{SkySunIlluminanceSource::Light, false, uint8_t(kDirection | kColor)},
                            Mode{SkySunIlluminanceSource::Curve, true, uint8_t(kDirection | kColor | kIntensity)},
                            Mode{SkySunIlluminanceSource::Curve, false, uint8_t(kDirection | kIntensity)}})
    {
        ECS::World world;
        const ECS::EntityHandle sun = AddDirectional(world, Components::kClearNoonSunIlluminanceLux);
        const ECS::EntityHandle other = AddDirectional(world, Components::kClearNoonSunIlluminanceLux);
        Components::SkyEnvironment sky{};
        sky.SunLight = sun;
        sky.SunIlluminanceSource = mode.Source;
        sky.DriveSunColor = mode.DriveColor;
        const ECS::EntityHandle skyEntity = world.CreateEntity();
        world.AddComponentImmediate(skyEntity, sky);
        const std::string what = std::string(mode.Source == SkySunIlluminanceSource::Curve ? "curve" : "light") +
                                 (mode.DriveColor ? ", colour on" : ", colour off");
        EXPECT_EQ(SunIlluminance::DrivenLightFields(world, sun), mode.Expected) << what;
        EXPECT_EQ(SunIlluminance::DrivenLightFields(world, other), 0) << what << ": a light the sky does not link";

        sky.TimeOfDayDrivesSunLight = false;
        world.AddComponentImmediate(skyEntity, sky);
        EXPECT_EQ(SunIlluminance::DrivenLightFields(world, sun), 0) << what << ", drive off";
        sky.TimeOfDayDrivesSunLight = true;
        sky.SunLight = {};
        world.AddComponentImmediate(skyEntity, sky);
        EXPECT_EQ(SunIlluminance::DrivenLightFields(world, sun), 0) << what << ", unlinked";
        sky.SunLight = sun;
        world.AddComponentImmediate(skyEntity, sky);

        world.AddComponentImmediate(sun, Components::Parent{other});
        EXPECT_EQ(SunIlluminance::DrivenLightFields(world, sun), 0) << what << ", parented";
        world.RemoveComponentImmediate<Components::Parent>(sun);
        ASSERT_EQ(SunIlluminance::DrivenLightFields(world, sun), mode.Expected) << what;

        Components::Skybox box{};
        std::snprintf(box.HDRIAssetGuid, sizeof(box.HDRIAssetGuid), "%s", "0123456789abcdef0123456789abcdef");
        world.AddComponentImmediate(world.CreateEntity(), box);
        EXPECT_EQ(SunIlluminance::DrivenLightFields(world, sun), 0) << what << ", HDRI skybox";
    }
}
