// Where the physical sky gets its brightness from.
//
// The atmosphere is driven by one number — SkySettings::primarySunIntensity, the sun's
// top-of-atmosphere irradiance on the 203-nit scale. Every part of the physical sky is linear in it:
// the sky-view LUT's single- and multiple-scattering source terms, the sun disc's radiance, and the
// IBL capture baked from the same LUT. Put it on a different scale from the scene's lights and the
// dome, the disc and the ambient fill all go dark together while the surfaces underneath stay lit —
// a black sky over a lit landscape.
//
// The scene's sun is what sets that scale. `SkyEnvironment::SunLight` is an optional two-way-sync
// link for driving or following one specific light; it is not a precondition for the sky knowing how
// bright the sun is. These tests run SkyEnvironmentSystem::Update against a real World and
// RenderServices, exactly as SkySunColorTests' tint fanout does, and read the value production
// consumes.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h" // kReferenceWhiteNits, LightIntensityToUnitless
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h" // World::AddComponentImmediate / GetComponent definitions
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "Engine/Rendering/FogSunLightingResolver.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "EngineLogCapture.h"
#include "Rendering/Sky/SkySettings.h"
#include "Scene/SceneIO.h"
#include "SkySunAnchorHarness.h"
#include "Types/ColorUtils.h"

using namespace GameEngine;

namespace
{

// A shipped clear-sky sun, and the value the tests below expect the sky to land on: the light's
// illuminance on the 203 scale, lifted to the top of the atmosphere. The lift constant lives in
// SkyEnvironmentSystem.cpp; mirroring it here is what makes a drift in either one visible.
constexpr float kShippedSunLux = 100000.0f;
constexpr float kSkyIrradianceScale = 1.33f;

float ExpectedAnchorForLux(float lux)
{
    return (lux / Components::kReferenceWhiteNits) * kSkyIrradianceScale;
}

// The sky's brightness, in stops, relative to the scene's sun. 0 = the two agree.
float StopsFromSun(float anchor, float lux)
{
    return std::log2(anchor / ExpectedAnchorForLux(lux));
}

float RelTolerance(float expected)
{
    return std::max(std::abs(expected) * 1e-4f, 1e-6f);
}

// The distinctive phrase of the no-sun notice, in one place so the tests below and the production
// string cannot drift apart silently.
constexpr const char* kNoSunNoticeMarker = "the sky's sun";

} // namespace

// THE DEFECT. A scene with a sun and an out-of-the-box physical sky: no `SunLight`, because that
// field is an optional link and nothing makes an author set it. The sky must still be lit by the
// scene's sun. Reading only the link leaves the dome at the SkySettings default of 10, six stops
// under a 100000 lx sun, which renders as a black sky over lit ground.
TEST(SkySunAnchor, AnUnlinkedSkyTakesItsBrightnessFromTheScenesSun)
{
    SkySunAnchorHarness harness;
    harness.AddDirectional(kShippedSunLux, Components::LightUnit::Lux);
    harness.AddDefaultSky();

    ASSERT_EQ(harness.Sky().Mode, Components::SkyMode::Physical) << "the default sky is Physical";
    ASSERT_FALSE(harness.World.IsValid(harness.Sky().SunLight)) << "the default sky links no light";

    const float anchor = harness.RunAndReadAnchor();
    const float expected = ExpectedAnchorForLux(kShippedSunLux);
    EXPECT_NEAR(anchor, expected, RelTolerance(expected))
        << "the sky is " << StopsFromSun(anchor, kShippedSunLux) << " stops off the scene's sun";
}

// The same statement as a property rather than a number: whatever the arithmetic, the sky may not
// sit a stop away from the sun that lights the scene. This is the assertion a future change to the
// irradiance model has to keep, and the one the black sky violated by six stops.
TEST(SkySunAnchor, TheSkyStaysWithinAStopOfTheScenesSunAcrossItsRange)
{
    for (const float lux : {2000.0f, 20000.0f, kShippedSunLux, 130000.0f})
    {
        SkySunAnchorHarness harness;
        harness.AddDirectional(lux, Components::LightUnit::Lux);
        harness.AddDefaultSky();

        const float stops = StopsFromSun(harness.RunAndReadAnchor(), lux);
        EXPECT_LT(std::abs(stops), 1.0f) << "at " << lux << " lx the sky is " << stops << " stops off";
    }
}

// It TRACKS the sun, it does not merely start near it: brighten the scene's sun and the sky follows
// on the same scale, so a dome and the surfaces below it can never drift apart.
TEST(SkySunAnchor, TheSkyTracksTheScenesSunWhenItsIntensityChanges)
{
    SkySunAnchorHarness harness;
    const ECS::EntityHandle sun = harness.AddDirectional(kShippedSunLux, Components::LightUnit::Lux);
    harness.AddDefaultSky();

    const float before = harness.RunAndReadAnchor();

    auto* light = harness.World.GetComponent<Components::Light>(sun);
    ASSERT_NE(light, nullptr);
    Components::Light dimmed = *light;
    dimmed.Intensity = kShippedSunLux * 0.25f;
    harness.World.AddComponentImmediate(sun, dimmed);

    const float after = harness.RunAndReadAnchor();
    EXPECT_NEAR(after, before * 0.25f, RelTolerance(before * 0.25f));
}

// Units, not raw numbers. The same illuminance authored in lux and as a unitless value must anchor
// the sky identically — the conversion belongs to the light, and the sky reads through it.
TEST(SkySunAnchor, TheAuthoredUnitIsResolvedBeforeTheSkyReadsIt)
{
    SkySunAnchorHarness lux;
    lux.AddDirectional(kShippedSunLux, Components::LightUnit::Lux);
    lux.AddDefaultSky();

    SkySunAnchorHarness unitless;
    unitless.AddDirectional(Components::LightIntensityToUnitless(kShippedSunLux,
                                                                 Components::LightUnit::Lux),
                            Components::LightUnit::Unitless);
    unitless.AddDefaultSky();

    const float a = lux.RunAndReadAnchor();
    const float b = unitless.RunAndReadAnchor();
    // Pin the absolute value first: two harnesses that both fall back to the sunless default agree
    // with each other, so `a == b` on its own is satisfied by a sky that read no light at all.
    const float expected = ExpectedAnchorForLux(kShippedSunLux);
    EXPECT_NEAR(a, expected, RelTolerance(expected))
        << "the lux harness did not anchor on its own sun, so the equality below proves nothing";
    EXPECT_NEAR(a, b, RelTolerance(a));
}

// An explicit link is a decision, so it outranks the scan. A scene that links a specific light gets
// that light's illuminance even when a brighter directional exists — otherwise the field an author
// set would be quietly overruled by one they did not.
TEST(SkySunAnchor, AnExplicitLinkOutranksABrighterDirectional)
{
    SkySunAnchorHarness harness;
    const ECS::EntityHandle keyed = harness.AddDirectional(20000.0f, Components::LightUnit::Lux);
    harness.AddDirectional(kShippedSunLux, Components::LightUnit::Lux);

    Components::SkyEnvironment sky{};
    sky.SunLight = keyed;
    // The follow path: the link is authoritative and the sky writes nothing back to it.
    sky.TimeOfDayDrivesSunLight = false;
    harness.SetSky(sky);

    const float expected = ExpectedAnchorForLux(20000.0f);
    EXPECT_NEAR(harness.RunAndReadAnchor(), expected, RelTolerance(expected));
}

// With no link, the sun is the BRIGHTEST directional — the one the scene is actually lit by, not
// whichever happens to be first.
TEST(SkySunAnchor, TheBrightestDirectionalIsTheSun)
{
    SkySunAnchorHarness harness;
    harness.AddDirectional(500.0f, Components::LightUnit::Lux);
    harness.AddDirectional(kShippedSunLux, Components::LightUnit::Lux);
    harness.AddDirectional(9000.0f, Components::LightUnit::Lux);
    harness.AddDefaultSky();

    const float expected = ExpectedAnchorForLux(kShippedSunLux);
    EXPECT_NEAR(harness.RunAndReadAnchor(), expected, RelTolerance(expected));
}

// A light that lights nothing is not a sun. A disabled directional, and one with CastsLight off,
// must both be passed over — anchoring to either would light the sky from a light no surface sees.
TEST(SkySunAnchor, ADirectionalThatEmitsNothingIsNotTheSun)
{
    SkySunAnchorHarness harness;

    const ECS::EntityHandle off = harness.World.CreateEntity();
    Components::Light disabled{};
    disabled.Type = Components::LightType::Directional;
    disabled.Intensity = 1000000.0f;
    disabled.IntensityUnit = Components::LightUnit::Lux;
    harness.World.AddComponentImmediate(off, disabled);
    ECS::Entity(&harness.World, off).SetEnabled<Components::Light>(false);
    harness.World.AddComponentImmediate(off, Components::Transform{});

    const ECS::EntityHandle dark = harness.World.CreateEntity();
    Components::Light nonEmitting{};
    nonEmitting.Type = Components::LightType::Directional;
    nonEmitting.Intensity = 1000000.0f;
    nonEmitting.IntensityUnit = Components::LightUnit::Lux;
    nonEmitting.CastsLight = false;
    harness.World.AddComponentImmediate(dark, nonEmitting);
    harness.World.AddComponentImmediate(dark, Components::Transform{});

    harness.AddDirectional(kShippedSunLux, Components::LightUnit::Lux);
    harness.AddDefaultSky();

    const float expected = ExpectedAnchorForLux(kShippedSunLux);
    EXPECT_NEAR(harness.RunAndReadAnchor(), expected, RelTolerance(expected));
}

// Only a DIRECTIONAL light is the sun. A bright point light must not become one.
TEST(SkySunAnchor, APunctualLightIsNotTheSun)
{
    SkySunAnchorHarness harness;

    const ECS::EntityHandle point = harness.World.CreateEntity();
    Components::Light lamp{};
    lamp.Type = Components::LightType::Point;
    lamp.Intensity = 1000000.0f;
    lamp.IntensityUnit = Components::LightUnit::Lumen;
    harness.World.AddComponentImmediate(point, lamp);
    harness.World.AddComponentImmediate(point, Components::Transform{});

    harness.AddDirectional(kShippedSunLux, Components::LightUnit::Lux);
    harness.AddDefaultSky();

    const float expected = ExpectedAnchorForLux(kShippedSunLux);
    EXPECT_NEAR(harness.RunAndReadAnchor(), expected, RelTolerance(expected));
}

// A scene with no sun at all has nothing to anchor to, so the dome keeps the SkySettings default —
// but it says so, once, and the notice names the fix. A sky that renders dark without a word is the
// failure this whole file exists for; the fallback is allowed, the silence is not.
TEST(SkySunAnchor, ASceneWithNoSunKeepsTheFallbackAndSaysSoOnce)
{
    std::vector<std::string> logLines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&logLines, Logger::LogLevel::Warning);

    // Positive control on the instrument: a capture that sees nothing is indistinguishable from a
    // warning that was never emitted, and the reading below is a count.
    Logger::Log::Warning("SkySunAnchor log-capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(GameEngine::TestLog::CountLinesContaining(logLines, "SkySunAnchor log-capture self test"),
              1u)
        << "the log capture is not receiving engine warnings — the count below would be a false zero";
    logLines.clear();

    SkySunAnchorHarness harness;
    harness.AddDefaultSky();

    const Rendering::SkySettings untouched{};
    const float first = harness.RunAndReadAnchor();
    EXPECT_FLOAT_EQ(first, untouched.primarySunIntensity)
        << "a sunless scene must keep the documented fallback, not invent a sun";
    EXPECT_FLOAT_EQ(harness.RunAndReadAnchor(), untouched.primarySunIntensity);

    Logger::Log::Flush();
    std::string dump;
    for (const auto& l : logLines)
        dump += "\n  | " + l;
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(logLines, kNoSunNoticeMarker), 1u)
        << "expected exactly one notice across two ticks; captured " << logLines.size()
        << " line(s):" << dump;
    const std::string notice =
        GameEngine::TestLog::FirstLineContaining(logLines, kNoSunNoticeMarker);
    EXPECT_NE(notice.find("Add or enable a directional light"), std::string::npos)
        << "the notice must name the fix: " << notice;

    // Re-arm. A sun appears and the sky takes it; the sun goes away and the scene is sunless again,
    // so the notice is due a second time. Once-per-episode that never re-arms goes silent on every
    // scene after the first.
    const ECS::EntityHandle sun =
        harness.AddDirectional(kShippedSunLux, Components::LightUnit::Lux);
    const float expected = ExpectedAnchorForLux(kShippedSunLux);
    EXPECT_NEAR(harness.RunAndReadAnchor(), expected, RelTolerance(expected))
        << "the sky did not pick up the sun that just appeared";
    Logger::Log::Flush();
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(logLines, kNoSunNoticeMarker), 1u)
        << "a tick with a sun must not add a notice";

    harness.World.DestroyEntityImmediate(sun);
    EXPECT_FLOAT_EQ(harness.RunAndReadAnchor(), untouched.primarySunIntensity)
        << "with the sun gone the sky must return to the documented fallback";
    Logger::Log::Flush();
    std::string reArmDump;
    for (const auto& l : logLines)
        reArmDump += "\n  | " + l;
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(logLines, kNoSunNoticeMarker), 2u)
        << "the notice must re-arm once a sun has existed; captured " << logLines.size()
        << " line(s):" << reArmDump;
}

// A gradient sky authors its own brightness and never reads the solar anchor, so a scene with no sun
// is a perfectly good setup for one — and it says nothing. A warning that fires on a correct scene
// is one an author learns to ignore, which costs the notice above its whole value.
TEST(SkySunAnchor, AGradientSkyWithNoSunSaysNothing)
{
    std::vector<std::string> logLines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&logLines, Logger::LogLevel::Warning);

    Logger::Log::Warning("SkySunAnchor gradient log-capture self test");
    Logger::Log::Flush();
    ASSERT_EQ(
        GameEngine::TestLog::CountLinesContaining(logLines, "SkySunAnchor gradient log-capture self test"),
        1u)
        << "the log capture is not receiving engine warnings — the count below would be a false zero";
    logLines.clear();

    SkySunAnchorHarness harness;
    Components::SkyEnvironment sky{};
    sky.Mode = Components::SkyMode::Gradient;
    harness.SetSky(sky);
    harness.RunAndReadAnchor();

    Logger::Log::Flush();
    std::string dump;
    for (const auto& l : logLines)
        dump += "\n  | " + l;
    EXPECT_EQ(GameEngine::TestLog::CountLinesContaining(logLines, kNoSunNoticeMarker), 0u)
        << "captured " << logLines.size() << " line(s):" << dump;
}

// ---- The linked sun: one owner for its illuminance ------------------------------------------------
//
// A sky that drives a linked directional light owns the light's direction and colour. The light's
// Intensity is the author's noon illuminance and stays theirs at every hour: the sky reads it and
// never writes it. These tests run the production system and read what the light's consumers read,
// the resolved colour times the resolved intensity.

namespace
{

constexpr float kMidnightHours = 0.0f;
constexpr float kNoonHours = 12.0f;

struct DeliveredLight
{
    float Rgb[3] = {0.0f, 0.0f, 0.0f};
};

DeliveredLight Delivered(const Components::Light& light)
{
    float colour[3] = {0.0f, 0.0f, 0.0f};
    float intensity = 0.0f;
    Components::ResolveLightColorIntensity(light, colour, intensity);
    DeliveredLight out;
    for (int c = 0; c < 3; ++c)
        out.Rgb[c] = colour[c] * intensity;
    return out;
}

// A directional light linked to a sky that drives it, the way the default scene ships.
struct LinkedSunHarness
{
    SkySunAnchorHarness Rig;
    ECS::EntityHandle Sun;

    LinkedSunHarness() = default;

    explicit LinkedSunHarness(float lux)
    {
        Sun = Rig.AddDirectional(lux, Components::LightUnit::Lux);
        Components::SkyEnvironment sky{};
        sky.SunLight = Sun;
        EXPECT_TRUE(sky.TimeOfDayDrivesSunLight) << "the shipped sky drives its linked light";
        Rig.SetSky(sky);
    }

    // Adopt a world a scene file was just loaded into: its one sky and the light it links.
    void AdoptLoadedScene()
    {
        Rig.World.Query<ECS::Read<Components::SkyEnvironment>>().Each(
            [this](ECS::EntityHandle e, const Components::SkyEnvironment& sky) {
                Rig.SkyEntity = e;
                Sun = sky.SunLight;
            });
    }

    void SetHours(float hours)
    {
        Components::SkyEnvironment sky = Rig.Sky();
        sky.TimeOfDayHours = hours;
        Rig.SetSky(sky);
    }

    void SetShowMoon(bool show)
    {
        Components::SkyEnvironment sky = Rig.Sky();
        sky.ShowMoon = show;
        Rig.SetSky(sky);
    }

    void Tick() { Rig.System.Update(Rig.World, 0.0f); }

    Components::Light Light() const
    {
        const auto* light = Rig.World.GetComponent<Components::Light>(Sun);
        EXPECT_NE(light, nullptr);
        return light ? *light : Components::Light{};
    }

    // An author's edit, applied the way the inspector commits one.
    void Author(const Components::Light& edited) { Rig.World.AddComponentImmediate(Sun, edited); }
};

// What a freshly authored sun delivers at `hours`: the reference every reproduction compares with.
DeliveredLight ReferenceDelivered(float lux, float hours)
{
    LinkedSunHarness reference(lux);
    reference.SetHours(hours);
    reference.Tick();
    return Delivered(reference.Light());
}

void ExpectSameDelivered(const DeliveredLight& actual, const DeliveredLight& expected, const char* what)
{
    for (int c = 0; c < 3; ++c)
        EXPECT_NEAR(actual.Rgb[c], expected.Rgb[c], std::max(std::abs(expected.Rgb[c]) * 1e-5f, 1e-9f))
            << what << ", channel " << c << ": " << actual.Rgb[c] / expected.Rgb[c]
            << "x the authored sun's light";
}

} // namespace

// The sky reads the sun's illuminance at every hour, so it never has a reason to write it. At
// midnight the linked light still holds the author's 100 000 lx; the moonlight is in its colour.
TEST(SkyLinkedSun, TheLinkedLightKeepsItsAuthoredIntensityAtNight)
{
    LinkedSunHarness harness(kShippedSunLux);
    harness.SetHours(kMidnightHours);
    harness.Tick();
    harness.Tick();

    EXPECT_EQ(harness.Light().Intensity, kShippedSunLux)
        << "the sky rewrote the linked light's authored illuminance";
    EXPECT_EQ(harness.Light().IntensityUnit, Components::LightUnit::Lux);
}

// An author opens the scene at night and types a softer sun into the light. That value is theirs:
// the next frame must not overwrite it, and noon must then deliver it.
TEST(SkyLinkedSun, AnEditMadeAtNightSurvivesTheNextFrame)
{
    constexpr float kSofterSunLux = 20000.0f;

    LinkedSunHarness harness(kShippedSunLux);
    harness.SetHours(kMidnightHours);
    harness.Tick();

    Components::Light edited = harness.Light();
    edited.Intensity = kSofterSunLux;
    harness.Author(edited);
    harness.Tick();

    EXPECT_EQ(harness.Light().Intensity, kSofterSunLux) << "the edit was overwritten on the next frame";

    harness.SetHours(kNoonHours);
    harness.Tick();
    harness.Tick();
    ExpectSameDelivered(Delivered(harness.Light()), ReferenceDelivered(kSofterSunLux, kNoonHours),
                        "noon after the night edit");
}

// Switching the light's unit converts its value and must change nothing an author can see: the
// night on the frame after the switch is the night before it, and noon is the authored noon.
TEST(SkyLinkedSun, ChangingTheUnitAtNightKeepsTheNightAndTheDay)
{
    LinkedSunHarness harness(kShippedSunLux);
    harness.SetHours(kMidnightHours);
    harness.Tick();
    harness.Tick();
    const DeliveredLight nightBefore = Delivered(harness.Light());
    ASSERT_GT(nightBefore.Rgb[1], 0.0f) << "the shipped night must deliver moonlight to compare with";

    // The light inspector's unit change: convert through the unitless scale.
    Components::Light edited = harness.Light();
    const float unitless = Components::LightIntensityToUnitless(edited.Intensity, edited.IntensityUnit);
    edited.IntensityUnit = Components::LightUnit::Unitless;
    edited.Intensity = Components::UnitlessToLightIntensity(unitless, Components::LightUnit::Unitless);
    harness.Author(edited);
    harness.Tick();

    ExpectSameDelivered(Delivered(harness.Light()), nightBefore, "the night on the frame after the switch");

    harness.SetHours(kNoonHours);
    harness.Tick();
    harness.Tick();
    ExpectSameDelivered(Delivered(harness.Light()), ReferenceDelivered(kShippedSunLux, kNoonHours),
                        "noon after the unit switch");
}

// A scene saved at night stores what the light holds. Reloaded into a fresh world and a fresh
// system, noon must deliver the sun the author set, not the night's moonlight.
TEST(SkyLinkedSun, ASceneSavedAtNightReloadsWithItsDaySun)
{
    const std::filesystem::path scenePath =
        std::filesystem::temp_directory_path() / "SkyLinkedSun_saved_at_night.scene";
    {
        LinkedSunHarness saved(kShippedSunLux);
        saved.SetHours(kMidnightHours);
        saved.Tick();
        saved.Tick();
        ASSERT_TRUE(Scene::SaveSceneToFile(saved.Rig.World, scenePath));
    }

    LinkedSunHarness reloaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(reloaded.Rig.World, scenePath));
    std::filesystem::remove(scenePath);
    reloaded.AdoptLoadedScene();
    ASSERT_TRUE(reloaded.Rig.World.IsValid(reloaded.Sun)) << "the sky's link to its sun must reload";
    ASSERT_EQ(reloaded.Rig.Sky().TimeOfDayHours, kMidnightHours);

    reloaded.SetHours(kNoonHours);
    reloaded.Tick();
    reloaded.Tick();
    ExpectSameDelivered(Delivered(reloaded.Light()), ReferenceDelivered(kShippedSunLux, kNoonHours),
                        "noon after reloading a scene saved at night");
}

// ---- What the driven light delivers across the day ------------------------------------------------
//
// By day the sun/moon blend is exactly zero, so the light is the sun's ground colour at the author's
// intensity. Across the handover it is the energy blend of the two bodies,
// (1 - w) x sun + w x moonLightIlluminanceScale x moon (MixLinkedLightGroundColor), recorded here
// from that blend. The full-night row was recorded when the night's scale was formed as
// 1 - (1 - moonLightIlluminanceScale), which lands 9.3e-9 (a relative 9.3e-7) under 0.01f: 12 to
// 15 units in the last place on these channels, and nothing else may move it.

namespace
{

// How a row's channels are compared with their goldens.
enum class GoldenMatch
{
    Day,       // blend exactly zero: within the extinction bound, the colour too; the intensity bitwise
    Twilight,  // within the extinction bound, a channel under kDarkChannelUnitless on both sides is dark
    FullNight, // within kFullNightUlpBound units in the last place
};

struct GoldenHour
{
    float Hours;
    GoldenMatch Match;
    uint32_t MoonOn[3];
    uint32_t MoonOff[3];
    uint32_t DayColour[3]; // the light's own colour, Day rows only
};

// A body's ground colour is its colour above the atmosphere times exp(zenithTau - tau), with each
// optical depth tau summed over the quadrature in AtmosphereTransmittance.cpp. Compilers contract that
// sum into fused multiply-adds differently (clang does by default, MSVC does not), which moves tau by
// a small fraction of itself; through the exponential that becomes the same absolute error in the
// channel's logarithm, so the channel's relative error grows with the light's extinction rather than
// staying flat. A channel therefore matches within kRelativeBoundPerExtinction x (1 + extinction),
// where the extinction is ln(unextinguished / golden), the optical depth the golden records against
// the light's unextinguished value, and the 1 covers the zenith path and the arithmetic after the
// exponential. A moonlit channel counts the moon's light scale (0.01) as extinction too, which
// loosens its bound. The largest macOS arm64 drift, at twilight, sits 3.1 times under the bound, and
// no channel above the dark floor is allowed more than a relative 2.1e-4, far tighter than any change
// to the sky or the handover. At twilight a channel under the dark floor carries no light and no
// information, so both sides under it compare as equal.
constexpr float kRelativeBoundPerExtinction = 1e-5f;
// In the light's unitless intensity per channel (LightIntensityToUnitless: 203 lux per unit, so
// 100 000 lx resolves to 492.61), the floor is 2.03e-4 lx per channel.
constexpr float kDarkChannelUnitless = 1e-6f;
// The light's own colour with no extra air mass in the way: the default sky's sun tint, 1 (the noon row).
constexpr float kUnextinguishedColour = 1.0f;
// Full night: the rounding of the night's scale above, and no more.
constexpr int64_t kFullNightUlpBound = 16;

// Resolved colour x resolved intensity of the linked light per channel, in the engine's unitless
// intensity (LightIntensityToUnitless), a shipped 100 000 lx sun (492.61 unitless).
constexpr GoldenHour kDeliveredGoldens[] = {
    {12.0f, GoldenMatch::Day, {0x43F64E30u, 0x43F64E30u, 0x43F64E30u},
     {0x43F64E30u, 0x43F64E30u, 0x43F64E30u}, {0x3F800000u, 0x3F800000u, 0x3F800000u}},
    {17.75f, GoldenMatch::Day, {0x438B61A3u, 0x4307B199u, 0x41B0F193u},
     {0x438B61A3u, 0x4307B199u, 0x41B0F193u}, {0x3F10DE11u, 0x3E8D08DFu, 0x3D37E87Du}},
    {18.15f, GoldenMatch::Twilight, {0x41013847u, 0x3E37C901u, 0x38C71840u},
     {0x41012E70u, 0x3E36D393u, 0x374A2607u}, {}},
    {18.5f, GoldenMatch::Twilight, {0x3C7FE244u, 0x3C3B89D6u, 0x3BA6B559u},
     {0x390F2168u, 0x2FCBF804u, 0x1887FE43u}, {}},
    {0.0f, GoldenMatch::FullNight, {0x3CC0C5B0u, 0x3CD11927u, 0x3CF41936u},
     {0x00000000u, 0x00000000u, 0x00000000u}, {}},
};

// Distance in units in the last place between two finite floats of any sign.
int64_t UlpDistance(float a, float b)
{
    const auto ordered = [](float v) {
        const int32_t bits = std::bit_cast<int32_t>(v);
        return bits < 0 ? static_cast<int64_t>(INT32_MIN) - bits : static_cast<int64_t>(bits);
    };
    return std::abs(ordered(a) - ordered(b));
}

// `unextinguished` is the value the channel takes with no atmosphere between the light and the
// ground: the light's resolved intensity for a delivered channel, 1 for its colour.
bool ChannelMatches(GoldenMatch match, float delivered, float want, float unextinguished)
{
    switch (match)
    {
    case GoldenMatch::Twilight:
        if (std::abs(delivered) < kDarkChannelUnitless && std::abs(want) < kDarkChannelUnitless)
            return true;
        [[fallthrough]];
    case GoldenMatch::Day:
    {
        const float extinction = std::max(0.0f, std::log(unextinguished / std::abs(want)));
        const float bound = kRelativeBoundPerExtinction * (1.0f + extinction);
        return std::abs(delivered - want) <= bound * std::abs(want);
    }
    case GoldenMatch::FullNight:
        return UlpDistance(delivered, want) <= kFullNightUlpBound;
    }
    return false;
}

} // namespace

// The light's consumers see its colour times its intensity, so that product is what is pinned at each
// hour. By day the factors are pinned as well: the colour the sky writes within the same extinction
// bound as the product, and the intensity the author set to the bit, because the sky never computes
// it and no contraction can move it.
TEST(SkyLinkedSun, TheDeliveredLightMatchesItsGoldensAcrossTheDay)
{
    const float unextinguished =
        Components::LightIntensityToUnitless(kShippedSunLux, Components::LightUnit::Lux);
    for (const bool showMoon : {true, false})
    {
        LinkedSunHarness harness(kShippedSunLux);
        harness.SetShowMoon(showMoon);
        for (const GoldenHour& golden : kDeliveredGoldens)
        {
            harness.SetHours(golden.Hours);
            harness.Tick();
            const DeliveredLight delivered = Delivered(harness.Light());
            const uint32_t* expected = showMoon ? golden.MoonOn : golden.MoonOff;
            for (int c = 0; c < 3; ++c)
            {
                const float want = std::bit_cast<float>(expected[c]);
                EXPECT_TRUE(ChannelMatches(golden.Match, delivered.Rgb[c], want, unextinguished))
                    << "hour " << golden.Hours << ", moon " << (showMoon ? "shown" : "hidden")
                    << ", channel " << c << ": delivered " << delivered.Rgb[c] << ", expected " << want
                    << ", " << UlpDistance(delivered.Rgb[c], want) << " units in the last place apart";
            }
            if (golden.Match != GoldenMatch::Day)
                continue;
            const Components::Light light = harness.Light();
            for (int c = 0; c < 3; ++c)
            {
                const float wantColour = std::bit_cast<float>(golden.DayColour[c]);
                EXPECT_TRUE(ChannelMatches(golden.Match, light.Color[c], wantColour, kUnextinguishedColour))
                    << "hour " << golden.Hours << ", colour channel " << c << ": " << light.Color[c]
                    << ", expected " << wantColour << ", " << UlpDistance(light.Color[c], wantColour)
                    << " units in the last place apart";
            }
            EXPECT_EQ(std::bit_cast<uint32_t>(light.Intensity), std::bit_cast<uint32_t>(kShippedSunLux))
                << "hour " << golden.Hours;
        }
    }
}

// The color of a driven light is the sky's, derived every frame; when the drive ends it must not stay
// behind as if the author had set it. The sky system hands the light back white on the next frame,
// whichever way the drive ended (an HDRI skybox taking over ends it too: DrivenSunLight is then
// invalid, SkySunIlluminance.AnHdriSkyboxDrivesNothing, and the release runs before the system's
// skybox branch, which needs a GPU texture a headless harness cannot upload), and nothing else: a
// change that keeps the drive, a sky that never drove the light, and a color the author put back
// all stay as they are.
TEST(SkyLinkedSun, EndingTheDriveHandsTheLightBackWhite)
{
    constexpr float kDuskHours = 18.5f;

    struct Ending
    {
        const char* How;
        void (*Apply)(LinkedSunHarness& harness, ECS::EntityHandle otherLight);
    };
    const Ending endings[] = {
        {"drive turned off", [](LinkedSunHarness& h, ECS::EntityHandle) {
             auto sky = h.Rig.Sky(); sky.TimeOfDayDrivesSunLight = false; h.Rig.SetSky(sky); }},
        {"link cleared", [](LinkedSunHarness& h, ECS::EntityHandle) {
             auto sky = h.Rig.Sky(); sky.SunLight = {}; h.Rig.SetSky(sky); }},
        {"link moved to another light", [](LinkedSunHarness& h, ECS::EntityHandle other) {
             auto sky = h.Rig.Sky(); sky.SunLight = other; h.Rig.SetSky(sky); }},
        {"sky disabled", [](LinkedSunHarness& h, ECS::EntityHandle) {
             ECS::Entity(&h.Rig.World, h.Rig.SkyEntity).SetEnabled<Components::SkyEnvironment>(false); }},
        {"sky component removed", [](LinkedSunHarness& h, ECS::EntityHandle) {
             h.Rig.World.RemoveComponentImmediate<Components::SkyEnvironment>(h.Rig.SkyEntity); }},
        {"sky entity deleted", [](LinkedSunHarness& h, ECS::EntityHandle) {
             h.Rig.World.DestroyEntityImmediate(h.Rig.SkyEntity); }},
        {"sky entity deactivated", [](LinkedSunHarness& h, ECS::EntityHandle) {
             h.Rig.World.SetEntityEnabledImmediate(h.Rig.SkyEntity, false); }},
    };

    for (const Ending& ending : endings)
    {
        LinkedSunHarness harness(kShippedSunLux);
        const ECS::EntityHandle other = harness.Rig.AddDirectional(kShippedSunLux, Components::LightUnit::Lux);
        harness.SetHours(kDuskHours);
        harness.Tick();
        ASSERT_LT(harness.Light().Color[2], 0.5f) << ending.How << ": dusk must tint the driven light";

        ending.Apply(harness, other);
        harness.Tick();

        for (int c = 0; c < 3; ++c)
            EXPECT_EQ(harness.Light().Color[c], 1.0f) << ending.How << ", channel " << c;
        EXPECT_EQ(harness.Light().Intensity, kShippedSunLux) << ending.How;
    }

    // Hiding the moon keeps the drive: the light stays the sky's (black at full night).
    {
        LinkedSunHarness harness(kShippedSunLux);
        harness.SetHours(kMidnightHours);
        harness.Tick();
        harness.SetShowMoon(false);
        harness.Tick();
        EXPECT_EQ(harness.Light().Color[1], 0.0f) << "a hidden moon keeps the drive; the night is black";
    }

    // A second sky the system does not render never drove its light: turning it off touches nothing.
    {
        LinkedSunHarness harness(kShippedSunLux);
        const ECS::EntityHandle fill = harness.Rig.AddDirectional(500.0f, Components::LightUnit::Lux);
        Components::Light fillLight = *harness.Rig.World.GetComponent<Components::Light>(fill);
        fillLight.Color[0] = 0.2f;
        fillLight.Color[1] = 0.4f;
        fillLight.Color[2] = 1.0f;
        harness.Rig.World.AddComponentImmediate(fill, fillLight);
        const ECS::EntityHandle secondSky = harness.Rig.World.CreateEntity();
        Components::SkyEnvironment second{};
        second.SunLight = fill;
        harness.Rig.World.AddComponentImmediate(secondSky, second);
        harness.SetHours(kDuskHours);
        harness.Tick();
        ECS::Entity(&harness.Rig.World, secondSky).SetEnabled<Components::SkyEnvironment>(false);
        harness.Tick();
        const auto* after = harness.Rig.World.GetComponent<Components::Light>(fill);
        EXPECT_EQ(after->Color[0], 0.2f);
        EXPECT_EQ(after->Color[2], 1.0f) << "a sky that never drove the fill light must not reset it";
    }

    // A color put on the light after the drive ended (here, what an undo restores) is the author's.
    {
        LinkedSunHarness harness(kShippedSunLux);
        harness.SetHours(kDuskHours);
        harness.Tick();
        auto sky = harness.Rig.Sky();
        sky.SunLight = {};
        harness.Rig.SetSky(sky);
        Components::Light authored = harness.Light();
        authored.Color[0] = 1.0f;
        authored.Color[1] = 0.5f;
        authored.Color[2] = 0.25f;
        harness.Author(authored);
        harness.Tick();
        EXPECT_EQ(harness.Light().Color[1], 0.5f) << "the author's color must survive the release";
    }
}

// The fog's sun at full night. Height fog and clouds take their sun from the primary directional
// when it lights anything, and from the sky otherwise. With the moon hidden the driven sun is black
// at its authored intensity, so a gate on the intensity alone hands the fog a black sun where it
// used to fall back to the sky. Expected values are what the system produced before the night moved
// into the colour, when the hidden moon zeroed the intensity instead.
TEST(SkyLinkedSun, TheFogSunAtNightIsUnchanged)
{
    struct NightFog
    {
        bool ShowMoon;
        float DeliveredLuminance; // luminance(fog colour) x fog intensity
    };
    constexpr NightFog kNightFog[] = {{true, 0.0254095f}, {false, 0.0168973f}};
    constexpr float kRecordedPrecision = 1e-5f; // the values were recorded to six significant digits

    for (const NightFog& expected : kNightFog)
    {
        LinkedSunHarness harness(kShippedSunLux);
        harness.SetShowMoon(expected.ShowMoon);
        harness.SetHours(kMidnightHours);
        harness.Tick();
        harness.Tick();

        Engine::Renderer::ExtractedLight extracted{};
        extracted.type = Components::LightType::Directional;
        Components::ResolveLightColorIntensity(harness.Light(), extracted.color, extracted.intensity);
        extracted.castsLight = 1u;
        auto& services = harness.Rig.Services;
        const auto camera = services.Views().AllocateCamera("NightFogCamera");
        const auto view = services.Views().AllocateView("NightFogView", camera);
        services.Views().SetViewWorldId(view, 0u);
        services.SubmitLight(0u, extracted);
        services.FinalizeWorldLights(0u);

        // The one selection both fog paths use: a black sun is no fog sun.
        const auto* lit = Engine::Renderer::SelectLitPrimaryDirectional(services.GetWorldLights(0u));
        EXPECT_EQ(lit != nullptr, expected.ShowMoon)
            << "moon " << (expected.ShowMoon ? "shown" : "hidden") << ": the fog sun selection";

        const float fallbackDir[3] = {0.0f, -1.0f, 0.0f};
        const float fallbackColor[3] = {1.0f, 1.0f, 1.0f};
        const auto fog = Engine::Renderer::ResolveHeightFogSunLighting(
            services, view, /*trackDirectionalLight=*/true, /*sunIntensityScale=*/1.0f, fallbackDir,
            fallbackColor, /*fallbackIntensity=*/1.0f);
        const float delivered = ColorUtils::LinearRec709Luminance(fog.color) * fog.intensity;
        EXPECT_NEAR(delivered, expected.DeliveredLuminance, expected.DeliveredLuminance * kRecordedPrecision)
            << "moon " << (expected.ShowMoon ? "shown" : "hidden") << ": the fog's sun at 00:00";
    }
}
