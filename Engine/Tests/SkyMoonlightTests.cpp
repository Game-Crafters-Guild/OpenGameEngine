// Moonlight in lux (SkyEnvironment::MoonlightIlluminance): what the light the sky drives delivers at
// full night, absolute rather than a fraction of the sun, and what the night sky and the moon disc are
// lit by. At the default it reproduces the night the sky has always had.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunDrive.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "EngineLogCapture.h"
#include "Rendering/Sky/AtmosphereTransmittance.h"
#include "Rendering/Sky/SkySystem.h"
#include "SkySunAnchorHarness.h"
#include "Types/ColorUtils.h"

using namespace GameEngine;

namespace
{
constexpr float kMidnightHours = 0.0f;

// A sky linked to one directional light, driving it as it ships (from the light).
struct NightRig
{
    SkySunAnchorHarness Rig;
    ECS::EntityHandle Sun;

    NightRig(float intensity, Components::LightUnit unit, float moonlightLux)
    {
        Sun = Rig.AddDirectional(intensity, unit);
        Components::SkyEnvironment sky{};
        sky.SunLight = Sun;
        sky.MoonlightIlluminance = moonlightLux;
        Rig.SetSky(sky);
    }

    void RunAt(float hours)
    {
        Components::SkyEnvironment sky = Rig.Sky();
        sky.TimeOfDayHours = hours;
        Rig.SetSky(sky);
        Rig.System.Update(Rig.World, 0.0f);
    }

    Components::Light Light() const { return *Rig.World.GetComponent<Components::Light>(Sun); }

    const Rendering::SkySettings& Settings() const
    {
        return Rig.Services.GetFeature<Engine::Renderer::SkyRenderFeature>()->GetSettings();
    }

    // The light's colour as the sky's own bodies give it, rebuilt from the rendered sun and moon with
    // the handover the frame used and `moonScale` on the moon.
    void ExpectedColour(float moonScale, float out[3]) const
    {
        const Rendering::SkySystemConfig config{};
        Rendering::SkySystemState state{};
        for (int c = 0; c < 3; ++c)
        {
            state.sunDirWS[c] = Settings().scatteringSunDir[c];
            state.moonDirWS[c] = Settings().moonDirWS[c];
        }
        state.primaryMoonBlend = Rendering::PrimaryMoonBlend(config, state.sunDirWS[1]);
        const float white[3] = {1.0f, 1.0f, 1.0f};
        Rendering::MixLinkedLightGroundColor(
            Rendering::EvaluateBodyGroundColors(Rendering::ScatteringAtmosphere(), config, state, white), moonScale, out);
    }
};
} // namespace

// The default moonlight is today's: at full night the shipped 100 000 lx sun light is the moon's
// ground colour times the exact stylized scale, bit for bit, and the night sky's source is the clear
// sun's lifted value times the moon's scale, as it always was.
TEST(SkyMoonlight, TheDefaultReproducesTodaysNight)
{
    NightRig rig(Components::kClearNoonSunIlluminanceLux, Components::LightUnit::Lux,
                 Components::kDefaultMoonlightIlluminanceLux);
    rig.RunAt(kMidnightHours);
    const Rendering::SkySystemConfig config{};
    float expected[3];
    rig.ExpectedColour(config.moonLightIlluminanceScale, expected);
    for (int c = 0; c < 3; ++c)
        EXPECT_EQ(rig.Light().Color[c], expected[c]) << "channel " << c;
    EXPECT_NEAR(Components::kDefaultMoonlightIlluminanceLux, 5.158f, 0.001f);

    const float clearSun =
        Components::LightIntensityToUnitless(Components::kClearNoonSunIlluminanceLux, Components::LightUnit::Lux) * 1.33f;
    EXPECT_EQ(rig.Settings().primarySunIntensity, clearSun * config.moonIntensityScale);
}

// Moonlight is absolute: a dimmer sun, or one authored in another unit, has the same night.
TEST(SkyMoonlight, NightIlluminanceIsAbsolute)
{
    NightRig clear(100000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    NightRig soft(20000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    NightRig unitless(2.0f, Components::LightUnit::Unitless, Components::kDefaultMoonlightIlluminanceLux);
    clear.RunAt(kMidnightHours);
    soft.RunAt(kMidnightHours);
    unitless.RunAt(kMidnightHours);
    const float night = Components::SkySunIlluminance::DeliveredLux(clear.Light());
    ASSERT_GT(night, 1.0f);
    EXPECT_NEAR(Components::SkySunIlluminance::DeliveredLux(soft.Light()), night, night * 1e-5f);
    EXPECT_NEAR(Components::SkySunIlluminance::DeliveredLux(unitless.Light()), night, night * 1e-5f);
}

// The night sky and the moon disc are lit by the moonlight: twice the moonlight doubles the night
// sky's source and the disc, and dimming the sun moves neither.
TEST(SkyMoonlight, TheNightSkyAndDiscFollowTheMoonlight)
{
    NightRig base(100000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    NightRig brighter(100000.0f, Components::LightUnit::Lux, 2.0f * Components::kDefaultMoonlightIlluminanceLux);
    NightRig dimSun(20000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    for (NightRig* rig : {&base, &brighter, &dimSun})
        rig->RunAt(kMidnightHours);
    EXPECT_NEAR(brighter.Settings().primarySunIntensity / base.Settings().primarySunIntensity, 2.0f, 1e-5f);
    EXPECT_NEAR(brighter.Settings().moonIntensity / base.Settings().moonIntensity, 2.0f, 1e-5f);
    EXPECT_NEAR(dimSun.Settings().primarySunIntensity, base.Settings().primarySunIntensity,
                base.Settings().primarySunIntensity * 1e-6f);
    EXPECT_NEAR(dimSun.Settings().moonIntensity, base.Settings().moonIntensity, base.Settings().moonIntensity * 1e-6f);
}

// By day the moonlight changes nothing: the light and the sky's day source are the same to the bit.
TEST(SkyMoonlight, DaylightIgnoresTheMoonlight)
{
    for (const float hours : {12.0f, 17.5f})
    {
        NightRig base(100000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
        NightRig bright(100000.0f, Components::LightUnit::Lux, 2000.0f);
        base.RunAt(hours);
        bright.RunAt(hours);
        for (int c = 0; c < 3; ++c)
            EXPECT_EQ(bright.Light().Color[c], base.Light().Color[c]) << "hour " << hours << ", channel " << c;
        EXPECT_EQ(bright.Light().Intensity, base.Light().Intensity) << "hour " << hours;
        EXPECT_EQ(bright.Settings().primarySunIntensity, base.Settings().primarySunIntensity) << "hour " << hours;
    }
}

// No moonlight is a dark night, not an error: the light delivers nothing, every value stays finite, and
// nothing is logged.
TEST(SkyMoonlight, ZeroIsADarkNightNotAnError)
{
    std::vector<std::string> logLines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&logLines, Logger::LogLevel::Warning);
    NightRig rig(100000.0f, Components::LightUnit::Lux, 0.0f);
    rig.RunAt(kMidnightHours);
    for (int c = 0; c < 3; ++c)
        EXPECT_TRUE(std::isfinite(rig.Light().Color[c])) << "channel " << c;
    EXPECT_EQ(Components::SkySunIlluminance::DeliveredLux(rig.Light()), 0.0f);
    EXPECT_TRUE(std::isfinite(rig.Settings().primarySunIntensity));
    EXPECT_TRUE(std::isfinite(rig.Settings().moonIntensity));
    EXPECT_TRUE(logLines.empty()) << logLines.front();
}

// The night horizon band (the sky's night horizon colour and the rim's night tint) is lit by the same
// moonlight as the dome above it: twice the moonlight doubles it, and at the default it is the
// authored keys exactly.
TEST(SkyMoonlight, TheNightHorizonFollowsTheMoonlight)
{
    NightRig base(100000.0f, Components::LightUnit::Lux, Components::kDefaultMoonlightIlluminanceLux);
    NightRig bright(100000.0f, Components::LightUnit::Lux, 2.0f * Components::kDefaultMoonlightIlluminanceLux);
    base.RunAt(kMidnightHours);
    bright.RunAt(kMidnightHours);
    const Components::SkyEnvironment defaults{};
    for (int c = 0; c < 3; ++c)
    {
        EXPECT_EQ(base.Settings().nightSkyHorizonColor[c], defaults.NightSkyHorizonColorKeys.Midnight[c]) << c;
        EXPECT_EQ(base.Settings().groundHorizonNightColor[c], defaults.GroundHorizonNightColorKeys.Midnight[c]) << c;
        EXPECT_NEAR(bright.Settings().nightSkyHorizonColor[c] / base.Settings().nightSkyHorizonColor[c], 2.0f, 1e-5f) << c;
        EXPECT_NEAR(bright.Settings().groundHorizonNightColor[c] / base.Settings().groundHorizonNightColor[c], 2.0f, 1e-5f)
            << c;
    }
}
