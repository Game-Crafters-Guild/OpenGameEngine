// The sky inspector's sun readout: its wording at day, dusk, polar day and polar night, read as an
// author reads it, and the info card it is shown in.

#include <gtest/gtest.h>

#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunPath.h"
#include "Editor/Settings/InfoCardAppearanceSettings.h"
#include "InspectorLayoutFixture.h"
#include "Inspectors/LuxText.h"
#include "Inspectors/SkySunPathCaptions.h"
#include "Inspectors/SkySunReadoutCard.h"
#include "Inspectors/SkySunReadoutText.h"
#include "Rendering/Sky/SolarPath.h"
#include "Sky/SkySunDayKind.h"
#include "UI/Controls/Label.h"
#include "UI/InfoCard.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using Editor::SkySunDayKind;

namespace
{

// The day the readout describes for a place, read off the solar path the sky uses.
SkySunDay EarthDay(float latitude, int32_t day, float noonLux)
{
    const Rendering::SolarPathAngles angles = Rendering::EarthPathAngles({latitude, day, 0.0f});
    Components::SkyEnvironment sky{};
    sky.Latitude = latitude;
    sky.DayOfYear = day;
    SkySunDay figures;
    figures.Kind = Editor::ClassifySkySunDay(sky);
    figures.DayLengthHours = Rendering::SolarDayLengthHours(angles);
    figures.NoonElevationDegrees = Rendering::SolarNoonElevationDegrees(angles);
    figures.MidnightElevationDegrees = Rendering::SolarMidnightElevationDegrees(angles);
    figures.NoonAzimuthDegrees =
        Rendering::SolarPositionAtHour(Rendering::MakeSolarFrame(angles), 12.0f).AzimuthDegrees;
    figures.NoonLux = noonLux;
    return figures;
}

// What the card reads off the sky at `hours`, from a sun light of `lightLux`, with the sun where the
// sky's path puts it: the figures the moon line is written from.
SkySunNow NightOnThePath(const Components::SkyEnvironment& sky, float hours, float lightLux)
{
    namespace SunPath = Components::SkySunPath;
    const Rendering::SolarPathAngles angles = SunPath::PathAngles(sky);
    SkySunNow now;
    now.Hours = hours;
    now.SunElevationDegrees =
        Rendering::SolarPositionAtHour(Rendering::MakeSolarFrame(angles), hours).ElevationDegrees;
    now.SunLux = SunPath::DrivenSunShareLux(sky, now.SunElevationDegrees, lightLux, hours);
    now.HandoverWeight = SunPath::DrivenMoonBlend(sky, now.SunElevationDegrees);
    now.MidnightHandoverWeight = SunPath::DrivenMoonBlend(sky, Rendering::SolarMidnightElevationDegrees(angles));
    now.FullNightLux = SunPath::FullNightMoonLux(sky);
    now.MoonRises = SunPath::MoonHighestElevationDegrees(sky) > 0.0f;
    return now;
}

Components::SkyEnvironment EarthSky(float latitude, int32_t day)
{
    Components::SkyEnvironment sky{};
    sky.Latitude = latitude;
    sky.DayOfYear = day;
    return sky;
}

// Expected text with each '~' standing for the non-breaking space the readout keeps between a figure
// and its unit (U+00A0), so an expectation stays readable.
std::string NoBreak(std::string text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '~')
            out += "\xC2\xA0";
        else
            out += c;
    }
    return out;
}

std::vector<const Label*> CardLines(const UIElement& card)
{
    std::vector<const Label*> lines;
    for (const auto& child : card.GetChildren())
    {
        if (const auto* label = dynamic_cast<const Label*>(child.get()))
            lines.push_back(label);
    }
    return lines;
}

} // namespace

// By day: the sunrise and sunset with the length of the day, then where the noon sun stands and
// what it gives, then what the sun gives now, facing it and on level ground.
TEST(SkySunReadout, ADayReadsAsSunriseSunsetNoonAndNow)
{
    // London in June, as the design's table gives it.
    SkySunDay london;
    london.DayLengthHours = 16.4f;
    london.NoonElevationDegrees = 61.96f;
    london.MidnightElevationDegrees = -15.1f;
    london.NoonAzimuthDegrees = 180.0f;
    london.NoonLux = 98582.0f;
    // Figures on the card are kept to three significant figures.
    EXPECT_EQ(SkySunDayLine(london), NoBreak("Sunrise 03:48 and sunset 20:12: 16~h 24~min of daylight."));
    EXPECT_EQ(SkySunNoonLine(london),
              NoBreak("At noon the sun is 62.0\xC2\xB0 above the horizon, to the south, and gives 98~600~lx on a surface "
              "facing it."));

    SkySunDay equator = EarthDay(0.0f, Rendering::kMarchEquinoxDay, 100000.0f);
    EXPECT_EQ(SkySunDayLine(equator), NoBreak("Sunrise 06:00 and sunset 18:00: 12~h of daylight."))
        << "a whole number of hours says no minutes";
    EXPECT_EQ(SkySunNoonLine(equator),
              NoBreak("At noon the sun is straight overhead and gives 100~000~lx on a surface facing it."));
    equator.NoonLux = 0.0f;
    EXPECT_EQ(SkySunNoonLine(equator), "At noon the sun is straight overhead.") << "no sun light, no figure";

    SkySunNow now;
    now.Hours = 17.75f;
    now.SunElevationDegrees = 30.0f;
    now.SunLux = 53200.0f;
    EXPECT_EQ(SkySunNowLine(london, now),
              NoBreak("Now, at 17:45, the sun is 30.0\xC2\xB0 above the horizon: 53~200~lx on a surface facing it, "
                      "26~600~lx on level ground."));
    EXPECT_EQ(SkyMoonNowLine(london, now), "") << "by day the night share says nothing";
}

// At 12:00 the Now line is the noon line and more: it takes the noon line's side and its words for
// the sun overhead, so the card shows it alone rather than one position in two sentences.
TEST(SkySunReadout, AtNoonTheNowLineSaysWhatTheNoonLineWould)
{
    SkySunDay london;
    london.DayLengthHours = 16.4f;
    london.NoonElevationDegrees = 61.96f;
    london.NoonAzimuthDegrees = 180.0f;
    london.NoonLux = 98582.0f;
    SkySunNow noon;
    noon.Hours = 12.0f;
    noon.SunElevationDegrees = 61.96f;
    noon.SunLux = 98582.0f;
    ASSERT_TRUE(SkySunNowIsNoon(noon));
    EXPECT_EQ(SkySunNowLine(london, noon),
              NoBreak("Now, at noon, the sun is 62.0\xC2\xB0 above the horizon, to the south: 98~600~lx on a surface "
                      "facing it, 87~000~lx on level ground."));

    SkySunNow overhead = noon;
    overhead.SunElevationDegrees = 90.0f;
    overhead.SunLux = 100000.0f;
    EXPECT_EQ(SkySunNowLine(EarthDay(0.0f, Rendering::kMarchEquinoxDay, 100000.0f), overhead),
              NoBreak("Now, at noon, the sun is straight overhead: 100~000~lx on a surface facing it, 100~000~lx on "
                      "level ground."));

    // A day whose own line says where the noon sun is gives the Now line no side to repeat.
    SkySunDay circling;
    circling.CustomPath = true;
    circling.Kind = SkySunDayKind::CirclesAtOneHeight;
    circling.NoonElevationDegrees = 61.96f;
    EXPECT_EQ(SkySunNowLine(circling, noon),
              NoBreak("Now, at noon, the sun is 62.0\xC2\xB0 above the horizon: 98~600~lx on a surface facing it, "
                      "87~000~lx on level ground."));

    // A day the sun never rises: its noon height is the noon line's, which the card leaves out at
    // 12:00, so the height is said once, by the Now line.
    SkySunDay neverRises;
    neverRises.CustomPath = true;
    neverRises.Kind = SkySunDayKind::NeverRises;
    neverRises.DayLengthHours = 0.0f;
    neverRises.NoonElevationDegrees = -50.0f;
    neverRises.MidnightElevationDegrees = -80.0f;
    SkySunNow darkNoon = noon;
    darkNoon.SunElevationDegrees = -50.0f;
    darkNoon.SunLux = 0.0f;
    EXPECT_EQ(SkySunDayLine(neverRises), "The sun never rises on this path.");
    EXPECT_EQ(SkySunNoonLine(neverRises), "At its highest, at noon, the sun is 50.0\xC2\xB0 below the horizon.");
    EXPECT_EQ(SkySunNowLine(neverRises, darkNoon),
              "Now, at noon, the sun is 50.0\xC2\xB0 below the horizon: no direct sunlight reaches the ground.");

    SkySunNow minuteLater = noon;
    minuteLater.Hours = 12.0f + 1.0f / 60.0f;
    EXPECT_FALSE(SkySunNowIsNoon(minuteLater));
    EXPECT_EQ(SkySunNowLine(london, minuteLater),
              NoBreak("Now, at 12:01, the sun is 62.0\xC2\xB0 above the horizon: 98~600~lx on a surface facing it, "
                      "87~000~lx on level ground."));
}

// With the sun down the moon line says what the moon gives. While the sky is still handing the light
// over from the sun, the light's figure is not moonlight, so the line gives only where the handover
// ends, the most the night's moon gives; at full night it gives the moonlight itself, or says there
// is none. On a path where the moon never rises it says so at any hour of the night.
TEST(SkySunReadout, TheMoonLineGivesMoonlightNotTheHandover)
{
    SkySunNow dusk;
    dusk.Hours = 18.4167f;
    dusk.SunElevationDegrees = -6.3f;
    dusk.SunLux = 0.03f;
    dusk.NightLux = 45.6f;
    dusk.HandoverWeight = 0.8f;
    dusk.FullNightLux = 5.158f;
    EXPECT_EQ(SkySunNowLine(SkySunDay{}, dusk),
              "Now, at 18:25, the sun is 6.3\xC2\xB0 below the horizon: no direct sunlight reaches the ground.")
        << "0.03 lx is under the half-lux worth saying";
    EXPECT_EQ(SkyMoonNowLine(SkySunDay{}, dusk),
              NoBreak("Night is still falling. At full night the moon gives up to 5.2~lx on a surface facing it."))
        << "no live figure while the handover runs";

    // What the sky still gives of the set sun is its light carried on through twilight, as the card's
    // tooltip says.
    SkySunNow twilight = dusk;
    twilight.SunElevationDegrees = -3.8f;
    twilight.SunLux = 30.0f;
    EXPECT_EQ(SkySunNowLine(SkySunDay{}, twilight),
              NoBreak("Now, at 18:25, the sun is 3.8\xC2\xB0 below the horizon; through twilight its light gives "
                      "30.0~lx on a surface facing it."));

    SkySunNow hiddenMoon = dusk;
    hiddenMoon.FullNightLux = 0.0f;
    hiddenMoon.MoonHidden = true;
    EXPECT_EQ(SkyMoonNowLine(SkySunDay{}, hiddenMoon),
              "Night is still falling. At full night there is no moonlight: the moon is hidden.");

    SkySunNow lowMoon = dusk;
    lowMoon.FullNightLux = 0.001f;
    EXPECT_EQ(SkyMoonNowLine(SkySunDay{}, lowMoon),
              "Night is still falling. At full night the moon stays too low to light the scene.");

    SkySunNow sunUp = dusk;
    sunUp.SunElevationDegrees = 0.5f;
    EXPECT_EQ(SkyMoonNowLine(SkySunDay{}, sunUp), "") << "nothing about the moon while the sun is up";

    SkySunNow night;
    night.Hours = 0.0f;
    night.SunElevationDegrees = -66.9f;
    night.SunLux = 0.0f;
    night.NightLux = 5.158f;
    night.HandoverWeight = 1.0f;
    night.FullNightLux = 5.158f;
    EXPECT_EQ(SkySunNowLine(SkySunDay{}, night),
              "Now, at 00:00, the sun is 66.9\xC2\xB0 below the horizon: no direct sunlight reaches the ground.");
    EXPECT_EQ(SkyMoonNowLine(SkySunDay{}, night), NoBreak("Moonlight now: 5.2~lx on a surface facing the moon."));

    SkySunNow midnight = night;
    midnight.SunElevationDegrees = -90.0f;
    EXPECT_EQ(SkySunNowLine(SkySunDay{}, midnight),
              "Now, at 00:00, the sun is directly below you (90.0\xC2\xB0 below the horizon) and gives no light.");

    SkySunNow moonless = night;
    moonless.NightLux = 0.0f;
    EXPECT_EQ(SkyMoonNowLine(SkySunDay{}, moonless), "The moon gives no light now.");
}

// With Auto Sun/Moon off the sun is placed by hand and the sky hands nothing to the moon, so there is
// no night to describe, whatever the handover figures read (they are 0 all night).
TEST(SkySunReadout, ASunPlacedByHandHasNoMoonLine)
{
    SkySunNow placedByHand;
    placedByHand.Hours = 0.0f;
    placedByHand.SunElevationDegrees = -30.0f;
    placedByHand.HandoverWeight = 0.0f;
    placedByHand.MidnightHandoverWeight = 0.0f;
    placedByHand.FullNightLux = 4.8f;
    placedByHand.HandsOverToMoon = false;
    EXPECT_EQ(SkyMoonNowLine(EarthDay(51.5f, Rendering::kMarchEquinoxDay, 100000.0f), placedByHand), "");
}

// Where the sun never rises the moon, which rides the sun's circle, never rises either, at any hour
// of the night. An Earth card says it of tonight, a Custom card of its path; under a still sun every
// hour is night, and there is no night to speak of apart from the day.
TEST(SkySunReadout, WhereTheMoonNeverRisesTheCardSaysSoInItsOwnWords)
{
    SkySunNow polarDusk;
    polarDusk.Hours = 18.4167f;
    polarDusk.SunElevationDegrees = -6.3f;
    polarDusk.HandoverWeight = 0.8f;
    polarDusk.MoonRises = false;
    SkySunNow polarMidnight = polarDusk;
    polarMidnight.Hours = 0.0f;
    polarMidnight.SunElevationDegrees = -38.4f;
    polarMidnight.HandoverWeight = 1.0f;

    const SkySunDay earth = EarthDay(75.0f, 355, 100000.0f);
    EXPECT_EQ(SkyMoonNowLine(earth, polarDusk), "The moon does not rise tonight either, so there is no moonlight.");
    EXPECT_EQ(SkyMoonNowLine(earth, polarMidnight), "The moon does not rise tonight either, so there is no moonlight.");

    SkySunDay custom = earth;
    custom.CustomPath = true;
    EXPECT_EQ(SkyMoonNowLine(custom, polarDusk), "The moon does not rise on this path either: the night has no moonlight.");
    EXPECT_EQ(SkyMoonNowLine(custom, polarMidnight),
              "The moon does not rise on this path either: the night has no moonlight.");

    SkySunDay still = custom;
    still.Kind = SkySunDayKind::StandsStill;
    EXPECT_EQ(SkyMoonNowLine(still, polarDusk), "The moon does not rise on this path either: there is no moonlight.");
}

// While the sky hands the light over, the line says which way the night is going: the sun climbs from
// midnight to noon on every path, so before noon the night is lifting and after it falling. London
// at the equinox, 3.7 degrees below the horizon at 05:36 and again at 18:24.
TEST(SkySunReadout, TheHandoverSaysWhetherNightIsFallingOrLifting)
{
    const Components::SkyEnvironment london = EarthSky(51.5f, Rendering::kMarchEquinoxDay);
    const SkySunDay day = EarthDay(51.5f, Rendering::kMarchEquinoxDay, 100000.0f);

    const SkySunNow dawn = NightOnThePath(london, 5.6f, 100000.0f);
    ASSERT_NEAR(dawn.SunElevationDegrees, -3.7f, 0.1f);
    ASSERT_GT(dawn.HandoverWeight, 0.0f);
    ASSERT_LT(dawn.HandoverWeight, 0.999f);
    ASSERT_GT(dawn.FullNightLux, 1.0f);
    EXPECT_EQ(SkyMoonNowLine(day, dawn),
              "Night is lifting. At full night the moon gives up to " + FormatLux(dawn.FullNightLux) +
                  " on a surface facing it.");

    const SkySunNow dusk = NightOnThePath(london, 18.4f, 100000.0f);
    ASSERT_NEAR(dusk.SunElevationDegrees, dawn.SunElevationDegrees, 0.01f) << "the matching hour after noon";
    EXPECT_EQ(SkyMoonNowLine(day, dusk),
              "Night is still falling. At full night the moon gives up to " + FormatLux(dusk.FullNightLux) +
                  " on a surface facing it.");
}

// The handover completes only with the sun about ten degrees under the horizon. Where the midnight
// sun stays above that, as at 60 degrees north at midsummer, full night never comes, and the card
// promises no full-night figure.
TEST(SkySunReadout, ANightThatNeverGetsFullyDarkSaysSo)
{
    const Components::SkyEnvironment north = EarthSky(60.0f, 172);
    const SkySunNow midnight = NightOnThePath(north, 0.0f, 100000.0f);
    ASSERT_LT(midnight.SunElevationDegrees, 0.0f);
    ASSERT_GT(midnight.SunElevationDegrees, -9.6f);
    ASSERT_LT(midnight.MidnightHandoverWeight, 0.999f);

    SkySunDay day = EarthDay(60.0f, 172, 100000.0f);
    EXPECT_EQ(SkyMoonNowLine(day, midnight), "It never gets fully dark tonight.");
    EXPECT_EQ(SkyMoonNowLine(day, NightOnThePath(north, 23.0f, 100000.0f)), "It never gets fully dark tonight.");
    day.CustomPath = true;
    EXPECT_EQ(SkyMoonNowLine(day, midnight), "It never gets fully dark on this path.");
}

// The time slider runs to 24:00 and the graph of the day labels its end 24:00: the card says the
// same, not 00:00.
TEST(SkySunReadout, TheDaysEndReadsAsItDoesUnderTheGraph)
{
    EXPECT_EQ(SolarClockText(0.0f), "00:00");
    EXPECT_EQ(SolarClockText(23.999f), "24:00");
    EXPECT_EQ(SolarClockText(24.0f), "24:00");

    SkySunNow now;
    now.Hours = 24.0f;
    now.SunElevationDegrees = -66.9f;
    now.HandoverWeight = 1.0f;
    EXPECT_EQ(SkySunNowLine(SkySunDay{}, now),
              "Now, at 24:00, the sun is 66.9\xC2\xB0 below the horizon: no direct sunlight reaches the ground.");
}

// Figures from 100 lux up are kept to three significant figures; below, one decimal from 1 lux and
// two under it. Grouped and joined to their unit by no-break spaces.
TEST(SkySunReadout, FiguresAreKeptToThreeSignificantFigures)
{
    EXPECT_EQ(FormatLux(98582.0f), NoBreak("98~600~lx"));
    EXPECT_EQ(FormatLux(36606.0f), NoBreak("36~600~lx"));
    EXPECT_EQ(FormatLux(100000.0f), NoBreak("100~000~lx"));
    EXPECT_EQ(FormatLux(99960.0f), NoBreak("100~000~lx")) << "rounding up across a power of ten";
    EXPECT_EQ(FormatLux(4564.0f), NoBreak("4~560~lx"));
    EXPECT_EQ(FormatLux(100.0f), NoBreak("100~lx"));
    EXPECT_EQ(FormatLux(45.64f), NoBreak("45.6~lx"));
    EXPECT_EQ(FormatLux(5.158f), NoBreak("5.2~lx"));
    EXPECT_EQ(FormatLux(0.71f), NoBreak("0.71~lx"));
}

// Polar day and polar night are plain sentences: what happens, the term in brackets, and the one
// figure that still describes the day. A Custom path, which has no seasons, says it of the path.
TEST(SkySunReadout, PolarDayAndPolarNightArePlainSentences)
{
    const SkySunDay polarDay = EarthDay(70.0f, 172, 100000.0f);
    EXPECT_EQ(SkySunDayLine(polarDay),
              "The sun does not set today (polar day). At its lowest, at midnight, it is 3.4\xC2\xB0 above the horizon.");
    EXPECT_NE(SkySunNoonLine(polarDay), "");

    const SkySunDay polarNight = EarthDay(70.0f, 355, 100000.0f);
    EXPECT_EQ(SkySunDayLine(polarNight), "The sun does not rise today (polar night).");
    EXPECT_EQ(SkySunNoonLine(polarNight), "At its highest, at noon, the sun is 3.4\xC2\xB0 below the horizon.")
        << "the noon line, which the card leaves out at 12:00";

    SkySunDay neverSets = polarDay;
    neverSets.CustomPath = true;
    EXPECT_EQ(SkySunDayLine(neverSets),
              "The sun never sets on this path. At its lowest, at midnight, it is 3.4\xC2\xB0 above the horizon.");
}

// A sun that grazes the horizon is "on the horizon", not "0.0° below the horizon": an axis 30 degrees
// up with a noon height of 120 puts the midnight sun exactly on it.
TEST(SkySunReadout, AGrazingSunIsOnTheHorizon)
{
    SkySunDay day;
    day.CustomPath = true;
    day.Kind = SkySunDayKind::NeverSets;
    day.DayLengthHours = 24.0f;
    day.NoonElevationDegrees = 60.0f;
    day.MidnightElevationDegrees = -0.00001f;
    EXPECT_EQ(SkySunDayLine(day), "The sun never sets on this path. At its lowest, at midnight, it is on the horizon.");
}

// An axis straight down with a noon height of 180: the sun circles along the horizon all day. The
// card says so once, not "12 h of daylight" from the degenerate sunrise equation, and the
// level-ground figure is "none" rather than 0.00 lx.
TEST(SkySunReadout, ASunOnTheHorizonAllDaySaysSo)
{
    SkySunDay edge;
    edge.CustomPath = true;
    edge.Kind = SkySunDayKind::CirclesAtOneHeight;
    edge.DayLengthHours = 12.0f;
    edge.NoonElevationDegrees = 0.00001f;
    edge.MidnightElevationDegrees = -0.00001f;
    edge.NoonLux = 4564.0f;
    EXPECT_EQ(SkySunDayLine(edge), "The sun stays on the horizon all day.");
    EXPECT_EQ(SkySunNoonLine(edge), "") << "the day line has said where the noon sun is";

    SkySunNow now;
    now.Hours = 12.0f;
    now.SunElevationDegrees = 0.0001f;
    now.SunLux = 4564.0f;
    EXPECT_EQ(SkySunNowLine(edge, now),
              NoBreak("Now, at noon, the sun is on the horizon: 4~560~lx on a surface facing it, none on level ground."));
}

// A Custom path has no compass, so its noon sun is placed by its heading: the axis heading on the
// axis side, the opposite heading away from it, glossed at the four axes.
TEST(SkySunReadout, ACustomNoonIsPlacedByItsHeading)
{
    SkySunDay day;
    day.CustomPath = true;
    day.AxisHeadingDegrees = 60.0f;
    day.DayLengthHours = 10.0f;
    day.NoonElevationDegrees = 25.0f;
    day.NoonAzimuthDegrees = 180.0f;
    EXPECT_EQ(SkySunNoonLine(day), "At noon the sun is 25.0\xC2\xB0 above the horizon, at heading 240\xC2\xB0.");
    day.AxisHeadingDegrees = 0.0f;
    day.NoonAzimuthDegrees = 179.99998f;
    EXPECT_EQ(SkySunNoonLine(day),
              "At noon the sun is 25.0\xC2\xB0 above the horizon, at heading 180\xC2\xB0 (along\xC2\xA0\xE2\x80\x91" "Z).");
    day.NoonAzimuthDegrees = 0.0f;
    EXPECT_EQ(SkySunNoonLine(day),
              "At noon the sun is 25.0\xC2\xB0 above the horizon, at heading 0\xC2\xB0 (along\xC2\xA0+Z).");
}

// A vertical axis has no noon side: the sun circles at one height, said once.
TEST(SkySunReadout, AVerticalAxisCirclesAtOneHeight)
{
    SkySunDay day;
    day.CustomPath = true;
    day.Kind = SkySunDayKind::CirclesAtOneHeight;
    day.DayLengthHours = 24.0f;
    day.NoonElevationDegrees = 30.0f;
    day.MidnightElevationDegrees = 30.0f;
    EXPECT_EQ(SkySunDayLine(day), "The sun circles 30.0\xC2\xB0 above the horizon all day and never sets.");
    EXPECT_EQ(SkySunNoonLine(day), "");
    day.DayLengthHours = 0.0f;
    day.NoonElevationDegrees = -20.0f;
    day.MidnightElevationDegrees = -20.0f;
    EXPECT_EQ(SkySunDayLine(day), "The sun circles 20.0\xC2\xB0 below the horizon all day and never rises.");
}

// A noon height at either end of the axis's reach puts the sun on the axis, or straight opposite
// it: it does not circle, so the card says where it stands, not a lowest and a highest that are the
// same figure.
TEST(SkySunReadout, ASunOnItsAxisStandsStill)
{
    SkySunDay day;
    day.CustomPath = true;
    day.Kind = SkySunDayKind::StandsStill;
    day.DayLengthHours = 24.0f;
    day.NoonElevationDegrees = 60.0f;
    day.MidnightElevationDegrees = 60.0f;
    day.NoonAzimuthDegrees = 0.0f;
    day.NoonLux = 90000.0f;
    EXPECT_EQ(SkySunDayLine(day),
              "The sun stands still 60.0\xC2\xB0 above the horizon, at heading 0\xC2\xB0 (along\xC2\xA0+Z), all day.");
    EXPECT_EQ(SkySunNoonLine(day), "") << "the day line has said where the noon sun is";

    day.AxisHeadingDegrees = 60.0f;
    day.NoonAzimuthDegrees = 180.0f;
    day.NoonElevationDegrees = -20.0f;
    day.MidnightElevationDegrees = -20.0f;
    EXPECT_EQ(SkySunDayLine(day), "The sun stands still 20.0\xC2\xB0 below the horizon, at heading 240\xC2\xB0, all day.");

    day.AxisHeadingDegrees = 0.0f;
    day.NoonElevationDegrees = 0.0f;
    day.MidnightElevationDegrees = 0.0f;
    EXPECT_EQ(SkySunDayLine(day),
              "The sun stands still on the horizon, at heading 180\xC2\xB0 (along\xC2\xA0\xE2\x80\x91" "Z), all day.");

    day.NoonElevationDegrees = 90.0f;
    EXPECT_EQ(SkySunDayLine(day), "The sun stands straight overhead all day.");
    day.NoonElevationDegrees = -90.0f;
    EXPECT_EQ(SkySunDayLine(day), "The sun stands directly below you all day.");

    // The hour means nothing to a sun that does not move: the Now line gives what it delivers, not where.
    SkySunNow up;
    up.Hours = 9.0f;
    up.SunElevationDegrees = 60.0f;
    up.SunLux = 90000.0f;
    EXPECT_EQ(SkySunNowLine(day, up), NoBreak("Now: 90~000~lx on a surface facing it, 77~900~lx on level ground."));
    SkySunNow below = up;
    below.SunElevationDegrees = -20.0f;
    below.SunLux = 0.0f;
    EXPECT_EQ(SkySunNowLine(day, below), "Now: no direct sunlight reaches the ground.");
    below.SunElevationDegrees = -2.0f;
    below.SunLux = 411.0f;
    EXPECT_EQ(SkySunNowLine(day, below),
              NoBreak("Now: through twilight the sun's light gives 411~lx on a surface facing it."));
}

// The day is read from the sky's own path: the readout words this reading and the overlay marks it.
TEST(SkySunReadout, TheDayIsReadFromTheSkysPath)
{
    Components::SkyEnvironment earth{};
    earth.Latitude = 51.5f;
    earth.DayOfYear = 172;
    EXPECT_EQ(Editor::ClassifySkySunDay(earth), SkySunDayKind::RisesAndSets);
    earth.Latitude = 75.0f;
    EXPECT_EQ(Editor::ClassifySkySunDay(earth), SkySunDayKind::NeverSets);
    earth.DayOfYear = 355;
    EXPECT_EQ(Editor::ClassifySkySunDay(earth), SkySunDayKind::NeverRises);
    earth.Latitude = 90.0f;
    EXPECT_EQ(Editor::ClassifySkySunDay(earth), SkySunDayKind::NeverRises) << "only a Custom axis circles at one height";

    const auto custom = [](float axisAltitude, float noonHeight) {
        Components::SkyEnvironment sky{};
        sky.SunPath = Components::SkySunPathKind::Custom;
        sky.CustomAxisAltitude = axisAltitude;
        sky.CustomNoonHeight = noonHeight;
        return Editor::ClassifySkySunDay(sky);
    };
    EXPECT_EQ(custom(30.0f, 60.0f), SkySunDayKind::RisesAndSets);
    EXPECT_EQ(custom(70.0f, 60.0f), SkySunDayKind::NeverSets);
    EXPECT_EQ(custom(75.0f, -8.43f), SkySunDayKind::NeverRises);
    EXPECT_EQ(custom(-90.0f, 180.0f), SkySunDayKind::CirclesAtOneHeight) << "along the horizon";
    EXPECT_EQ(custom(89.5f, 30.0f), SkySunDayKind::CirclesAtOneHeight);
    EXPECT_EQ(custom(0.0f, 0.0f), SkySunDayKind::StandsStill) << "on the horizon, opposite a level axis";
    EXPECT_EQ(custom(60.0f, 120.0f), SkySunDayKind::StandsStill) << "on the axis";
    EXPECT_EQ(custom(60.0f, 170.0f), SkySunDayKind::StandsStill) << "a stored height out of reach is used in reach";
    EXPECT_EQ(custom(90.0f, 90.0f), SkySunDayKind::StandsStill) << "overhead on a vertical axis";
    EXPECT_EQ(custom(60.0f, 119.0f), SkySunDayKind::NeverSets) << "a degree from the axis the sun circles it";
}

// The Height at noon caption: its side of the axis, the horizon at both ends, under the horizon
// beyond them, and nothing but "(overhead)" and "(below)" under a vertical axis, which has no sides.
// Each is short enough to stay on one line beside its number in the narrowest inspector.
TEST(SkySunReadout, TheNoonHeightCaptionFollowsTheAxis)
{
    EXPECT_EQ(NoonHeightCaption(-8.43f, 75.0f), "(below, far side)");
    EXPECT_EQ(NoonHeightCaption(200.0f, -30.0f), "(below, axis side)");
    EXPECT_EQ(NoonHeightCaption(-20.0f, 90.0f), "(below)");
    EXPECT_EQ(NoonHeightCaption(200.0f, -90.0f), "(below)") << "straight down has no axis side";
    EXPECT_EQ(NoonHeightCaption(90.0f, 0.0f), "(overhead)");
    EXPECT_EQ(NoonHeightCaption(25.0f, 70.0f), "(far side)");
    EXPECT_EQ(NoonHeightCaption(120.0f, 30.0f), "(axis side)");
    EXPECT_EQ(NoonHeightCaption(0.0f, 0.0f), "(horizon, far side)");
    EXPECT_EQ(NoonHeightCaption(180.0f, -90.0f + 2.0f), "(horizon, axis side)");
    EXPECT_EQ(NoonHeightCaption(30.0f, 90.0f), "");
    EXPECT_EQ(NoonHeightCaption(30.0f, 89.2f), "") << "within about a degree of vertical";
    EXPECT_EQ(NoonHeightCaption(90.0f, 90.0f), "(overhead)");
}

// Text wraps at a space and after an ordinary hyphen, so an axis gloss is held together: a no-break
// space after "along" and a non-breaking hyphen for the minus. "(along -Z)" stays on one line, in a
// caption or in the card.
TEST(SkySunReadout, AnAxisGlossIsOneUnit)
{
    for (const float heading : {0.0f, 90.0f, 180.0f, 270.0f})
    {
        const std::string caption = HeadingCaption(heading);
        ASSERT_FALSE(caption.empty()) << heading;
        EXPECT_EQ(caption.find('-'), std::string::npos) << caption;
        EXPECT_EQ(caption.find(' '), std::string::npos) << caption;
    }
    EXPECT_EQ(HeadingCaption(180.0f), "(along\xC2\xA0\xE2\x80\x91" "Z)");
    EXPECT_EQ(HeadingCaption(270.0f), "(along\xC2\xA0\xE2\x80\x91" "X)");
}

TEST(SkySunReadout, WhyTheSkyLightsNothing)
{
    EXPECT_EQ(SkySunNotDrivingLine(SkySunNotDriving::DriveOff, "Sun"),
              "The sky is not lighting the scene: turn on Time -> Light Source below to let it move and colour "
              "\"Sun\".");
    EXPECT_EQ(SkySunNotDrivingLine(SkySunNotDriving::LightHasParent, "Sun"),
              "The sky cannot move \"Sun\": it has a parent, and the sky can only move a light that has none.");
    EXPECT_EQ(SkySunNotDrivingLine(SkySunNotDriving::NotLinked, "Sun"),
              "The sky is not lighting the scene: link \"Sun\" as the Sun Light above to let it move and colour "
              "that light.");
}

// The readout is one info card of the shared kind, styled by the card's own sheet: it carries the
// card class and its lines the card's text class, it stays visible when "Show Info Cards" is off
// (it is live data, not an explanation), and a line with nothing to say is hidden.
TEST(SkySunReadout, TheCardIsAnAlwaysVisibleInfoCardWithALinePerFigure)
{
    auto& appearance = Editor::InfoCardAppearanceSettings::Get();
    const bool cardsWereVisible = appearance.GetCardsVisible();
    appearance.SetCardsVisible(false);

    UIElement parent;
    SkySunReadoutCard card(&parent);
    appearance.SetCardsVisible(cardsWereVisible);

    ASSERT_EQ(parent.GetChildren().size(), 1u);
    const UIElement* element = card.GetCard();
    ASSERT_EQ(element, parent.GetChildren()[0].get());
    EXPECT_TRUE(element->HasClass("editor-info-card"));
    EXPECT_TRUE(element->HasClass(EditorUI::kInfoCardAlwaysVisibleClass));
    EXPECT_FALSE(element->HasClass(EditorUI::kInfoCardOffClass));
    EXPECT_FALSE(element->GetTooltip().empty()) << "the card explains solar time and what its figures include";

    SkySunReadoutLines lines;
    lines.Day = "Sunrise 06:00 and sunset 18:00: 12 h 0 min of daylight.";
    lines.Noon = "At noon the sun is straight overhead.";
    lines.Now = "Now, at 12:00, the sun is 90.0\xC2\xB0 above the horizon.";
    card.Show(lines);

    const std::vector<const Label*> shown = CardLines(*element);
    ASSERT_EQ(shown.size(), 4u);
    const std::string expected[] = {lines.Day, lines.Noon, lines.Now, ""};
    for (size_t i = 0; i < shown.size(); ++i)
    {
        EXPECT_TRUE(shown[i]->HasClass("editor-info-card-text")) << i;
        EXPECT_EQ(shown[i]->GetText(), expected[i]) << i;
        EXPECT_EQ(shown[i]->HasClass("hidden"), expected[i].empty()) << i;
    }

    lines.Moon = "Moonlight now: 5.1 lx on a surface facing the moon.";
    card.Show(lines);
    EXPECT_FALSE(CardLines(*element)[3]->HasClass("hidden"));
}

// The card's tooltip is the only place the readout says its times are solar time, so resting the
// pointer on any line of the card has to bring it up: the line under the pointer has no tooltip of
// its own, and the card's answers for it.
TEST(SkySunReadout, RestingThePointerOnALineOfTheCardShowsTheCardsTooltip)
{
    auto root = std::make_unique<UIElement>();
    SkySunReadoutCard card(root.get());
    SkySunReadoutLines lines;
    lines.Day = "Sunrise 06:00 and sunset 18:00: 12 h of daylight.";
    lines.Noon = "At noon the sun is straight overhead.";
    card.Show(lines);
    UIElement* element = card.GetCard();

    InspectorLayoutTesting::InspectorLayoutFixture fixture;
    fixture.ExtraSheets = {"Assets/UI/theme/core.css"};
    if (!fixture.Build(std::move(root)))
        GTEST_SKIP() << fixture.Diagnostic;
    ASSERT_GT(element->GetLayoutWidth(), 0.0f);
    ASSERT_GT(element->GetLayoutHeight(), 0.0f);

    const UIElement* noonLine = element->GetChildren()[1].get();
    ASSERT_GT(noonLine->GetLayoutHeight(), 0.0f);
    fixture.Ui->OnMouseMove(noonLine->GetLayoutX() + 0.5f * noonLine->GetLayoutWidth(),
                            noonLine->GetLayoutY() + 0.5f * noonLine->GetLayoutHeight());
    // A second of frames: past the hover delay, and the frame the bubble takes to measure its text.
    constexpr int kFrames = 60;
    constexpr float kFrameSeconds = 1.0f / 60.0f;
    for (int i = 0; i < kFrames; ++i)
        fixture.Ui->Update(kFrameSeconds, /*interactive=*/true);

    const UIElement* hovered = fixture.Ui->GetHoveredElement();
    ASSERT_NE(hovered, nullptr);
    EXPECT_TRUE(hovered == noonLine || hovered == element) << "the pointer rests on the card";

    UIElement* uiRoot = fixture.Ui->GetRootElement();
    const auto* bubbleText = dynamic_cast<const Label*>(uiRoot->FindById("ui-tooltip-text"));
    ASSERT_NE(bubbleText, nullptr) << "no tooltip was brought up";
    EXPECT_EQ(bubbleText->GetText(), element->GetTooltip());
    EXPECT_NE(bubbleText->GetText().find("solar time"), std::string::npos);
    const UIElement* bubble = uiRoot->FindById("ui-tooltip");
    ASSERT_NE(bubble, nullptr);
    EXPECT_GE(bubble->GetLayoutX(), 0.0f) << "the bubble is on screen, not parked";
    EXPECT_GT(bubble->GetLayoutWidth(), 0.0f);
}
