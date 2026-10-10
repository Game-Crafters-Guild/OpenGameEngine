#include "Inspectors/SkySunReadoutText.h"

#include "Inspectors/LuxText.h"
#include "Inspectors/SkySunPathCaptions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace GameEngine
{
namespace
{
constexpr int kMinutesPerHour = 60;
constexpr int kMinutesPerDay = 24 * kMinutesPerHour;
constexpr int kNoonMinutes = 12 * kMinutesPerHour;
constexpr float kHoursPerDay = 24.0f;
constexpr float kNoonHours = 12.0f;
constexpr float kDegreesToRadians = 0.01745329252f;
constexpr float kFullTurnDegrees = 360.0f;
// Within this of the zenith the noon sun is "straight overhead", and within this of the nadir the
// midnight sun is "directly below you": the readout shows one decimal.
constexpr float kOverheadDegrees = 89.95f;
// Within this of 0 an elevation shows as 0.0°: the readout says the sun is on the horizon instead.
constexpr float kOnTheHorizonDegrees = 0.05f;
// Below this an illuminance rounds to "0.00 lx": the readout says there is none instead.
constexpr float kNegligibleLux = 0.005f;
// A set sun's remaining light is worth a sentence only from this up; under it the sun "gives no
// direct light" rather than a figure that reads as noise.
constexpr float kSetSunWorthSayingLux = 0.5f;
// The handover is complete (full night) from this weight.
constexpr float kFullNightWeight = 0.999f;
// A bearing within a quarter turn of 0 lies toward north.
constexpr float kQuarterTurnDegrees = 90.0f;
constexpr float kThreeQuarterTurnDegrees = 270.0f;

std::string Degrees(float degrees)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f\xC2\xB0", degrees);
    return text;
}

// "7.5° above the horizon", "2.1° below the horizon", or "on the horizon" when it would read 0.0°
// either way.
std::string AboveOrBelow(float elevationDegrees)
{
    if (std::abs(elevationDegrees) < kOnTheHorizonDegrees)
        return "on the horizon";
    return elevationDegrees >= 0.0f ? Degrees(elevationDegrees) + " above the horizon"
                                    : Degrees(-elevationDegrees) + " below the horizon";
}

// "16 h 24 min", or "12 h" on the hour, rounded to the minute, each figure held to its unit by a
// non-breaking space.
std::string Duration(float hours)
{
    const int minutes = static_cast<int>(std::lround(hours * static_cast<float>(kMinutesPerHour)));
    char text[24];
    if (minutes % kMinutesPerHour == 0)
        std::snprintf(text, sizeof(text), "%d\xC2\xA0h", minutes / kMinutesPerHour);
    else
        std::snprintf(text, sizeof(text), "%d\xC2\xA0h %d\xC2\xA0min", minutes / kMinutesPerHour,
                      minutes % kMinutesPerHour);
    return text;
}

// "at heading 240°", with the axis it lies along when it does: "at heading 180° (along -Z)".
std::string HeadingText(float degrees)
{
    float heading = std::fmod(std::round(degrees), kFullTurnDegrees);
    if (heading < 0.0f)
        heading += kFullTurnDegrees;
    char text[32];
    std::snprintf(text, sizeof(text), "at heading %.0f\xC2\xB0", heading);
    const std::string caption = HeadingCaption(heading);
    return caption.empty() ? std::string(text) : std::string(text) + " " + caption;
}

// Where the noon sun stands: to the north or south on Earth, at a heading on a Custom path.
std::string NoonSide(const SkySunDay& day)
{
    if (day.CustomPath)
        return HeadingText(day.AxisHeadingDegrees + day.NoonAzimuthDegrees);
    const bool towardNorth =
        day.NoonAzimuthDegrees < kQuarterTurnDegrees || day.NoonAzimuthDegrees > kThreeQuarterTurnDegrees;
    return towardNorth ? "to the north" : "to the south";
}

// The noon line says where the noon sun stands, side included, on a day whose own line has not said
// where it is. On a day the sun never rises it gives the noon height alone.
bool NoonLineGivesSide(const SkySunDay& day)
{
    return day.Kind == Editor::SkySunDayKind::RisesAndSets || day.Kind == Editor::SkySunDayKind::NeverSets;
}

// On every path the sun climbs from its lowest, at midnight, to its highest, at noon, and sinks again
// after (SolarPath.h), so the hour says which way it is going.
bool SunIsRising(float hours)
{
    return std::fmod(hours, kHoursPerDay) < kNoonHours;
}

// What the sun gives now, after the Now line's lead: its figures while it is up, what twilight still
// carries of its light once it has set, or that it gives nothing. `whose` names the sun's light for a
// line that has not named the sun.
std::string SunFigures(const SkySunNow& now, const std::string& whose)
{
    if (now.SunElevationDegrees > -kOnTheHorizonDegrees)
    {
        const float levelGroundLux =
            std::max(0.0f, now.SunLux * std::sin(now.SunElevationDegrees * kDegreesToRadians));
        // A sun the line calls "on the horizon" lights no level ground, whatever the last digit says.
        const bool none = levelGroundLux < kNegligibleLux || now.SunElevationDegrees < kOnTheHorizonDegrees;
        const std::string levelGround = none ? std::string("none") : FormatLux(levelGroundLux);
        return FormatLux(now.SunLux) + " on a surface facing it, " + levelGround + " on level ground.";
    }
    if (now.SunLux >= kSetSunWorthSayingLux)
        return "through twilight " + whose + " light gives " + FormatLux(now.SunLux) + " on a surface facing it.";
    return "no direct sunlight reaches the ground.";
}

int MinutesOfDay(float hours)
{
    return std::clamp(static_cast<int>(std::lround(hours * static_cast<float>(kMinutesPerHour))), 0, kMinutesPerDay);
}

// A sun that circles at one height: along the horizon, or above or below it all day.
std::string CirclingSunLine(const SkySunDay& day)
{
    if (std::abs(day.NoonElevationDegrees) < kOnTheHorizonDegrees)
        return "The sun stays on the horizon all day.";
    return "The sun circles " + AboveOrBelow(day.NoonElevationDegrees) +
           (day.NoonElevationDegrees > 0.0f ? " all day and never sets." : " all day and never rises.");
}

// A sun that does not move: where it stands, by its height and its heading.
std::string StillSunLine(const SkySunDay& day)
{
    if (day.NoonElevationDegrees >= kOverheadDegrees)
        return "The sun stands straight overhead all day.";
    if (day.NoonElevationDegrees <= -kOverheadDegrees)
        return "The sun stands directly below you all day.";
    return "The sun stands still " + AboveOrBelow(day.NoonElevationDegrees) + ", " + NoonSide(day) + ", all day.";
}
} // namespace

std::string SolarClockText(float hours)
{
    const int minutes = MinutesOfDay(hours);
    char text[8];
    std::snprintf(text, sizeof(text), "%02d:%02d", minutes / kMinutesPerHour, minutes % kMinutesPerHour);
    return text;
}

std::string SkySunDayLine(const SkySunDay& day)
{
    switch (day.Kind)
    {
    case Editor::SkySunDayKind::StandsStill:
        return StillSunLine(day);
    case Editor::SkySunDayKind::CirclesAtOneHeight:
        return CirclingSunLine(day);
    case Editor::SkySunDayKind::NeverRises:
        return day.CustomPath ? "The sun never rises on this path." : "The sun does not rise today (polar night).";
    case Editor::SkySunDayKind::NeverSets:
    {
        const std::string never =
            day.CustomPath ? "The sun never sets on this path." : "The sun does not set today (polar day).";
        return never + " At its lowest, at midnight, it is " + AboveOrBelow(day.MidnightElevationDegrees) + ".";
    }
    case Editor::SkySunDayKind::RisesAndSets:
        break;
    }
    const float halfDay = 0.5f * day.DayLengthHours;
    return "Sunrise " + SolarClockText(12.0f - halfDay) + " and sunset " + SolarClockText(12.0f + halfDay) + ": " +
           Duration(day.DayLengthHours) + " of daylight.";
}

std::string SkySunNoonLine(const SkySunDay& day)
{
    if (day.Kind == Editor::SkySunDayKind::NeverRises)
        return "At its highest, at noon, the sun is " + AboveOrBelow(day.NoonElevationDegrees) + ".";
    if (!NoonLineGivesSide(day))
        return {};
    const bool overhead = day.NoonElevationDegrees >= kOverheadDegrees;
    std::string text = overhead ? std::string("At noon the sun is straight overhead")
                                : "At noon the sun is " + AboveOrBelow(day.NoonElevationDegrees) + ", " + NoonSide(day);
    if (day.NoonLux > 0.0f)
        text += (overhead ? " and gives " : ", and gives ") + FormatLux(day.NoonLux) + " on a surface facing it";
    return text + ".";
}

bool SkySunNowIsNoon(const SkySunNow& now)
{
    return MinutesOfDay(now.Hours) == kNoonMinutes;
}

std::string SkySunNowLine(const SkySunDay& day, const SkySunNow& now)
{
    // A sun that stands still is where it is at every hour: the line gives what it delivers, not where.
    if (day.Kind == Editor::SkySunDayKind::StandsStill)
        return "Now: " + SunFigures(now, "the sun's");
    const bool atNoon = SkySunNowIsNoon(now);
    const std::string lead =
        atNoon ? std::string("Now, at noon, the sun is ") : "Now, at " + SolarClockText(now.Hours) + ", the sun is ";
    if (now.SunElevationDegrees <= -kOverheadDegrees)
        return lead + "directly below you (" + AboveOrBelow(now.SunElevationDegrees) + ") and gives no light.";
    // One phrase for one position, the noon line's: overhead is "straight overhead" here too.
    std::string where = lead;
    if (now.SunElevationDegrees >= kOverheadDegrees)
        where += "straight overhead";
    else if (atNoon && NoonLineGivesSide(day))
        where += AboveOrBelow(now.SunElevationDegrees) + ", " + NoonSide(day);
    else
        where += AboveOrBelow(now.SunElevationDegrees);
    const bool twilight = now.SunElevationDegrees <= -kOnTheHorizonDegrees && now.SunLux >= kSetSunWorthSayingLux;
    return where + (twilight ? "; " : ": ") + SunFigures(now, "its");
}

std::string SkyMoonNowLine(const SkySunDay& day, const SkySunNow& now)
{
    if (now.SunElevationDegrees > -kOnTheHorizonDegrees || !now.HandsOverToMoon)
        return {};
    if (!now.MoonRises)
    {
        if (!day.CustomPath)
            return "The moon does not rise tonight either, so there is no moonlight.";
        // A still sun below the horizon makes every hour night.
        return day.Kind == Editor::SkySunDayKind::StandsStill
                   ? "The moon does not rise on this path either: there is no moonlight."
                   : "The moon does not rise on this path either: the night has no moonlight.";
    }
    if (now.HandoverWeight >= kFullNightWeight)
    {
        return now.NightLux < kNegligibleLux ? std::string("The moon gives no light now.")
                                             : "Moonlight now: " + FormatLux(now.NightLux) +
                                                   " on a surface facing the moon.";
    }
    // The handover completes only with the sun well below the horizon: where it never gets that low,
    // there is no full night to give a figure for.
    if (now.MidnightHandoverWeight < kFullNightWeight)
        return day.CustomPath ? "It never gets fully dark on this path." : "It never gets fully dark tonight.";
    // While the sky hands the light over, its figure is not moonlight: say where the handover ends.
    const std::string lead =
        SunIsRising(now.Hours) ? "Night is lifting. At full night " : "Night is still falling. At full night ";
    if (now.MoonHidden)
        return lead + "there is no moonlight: the moon is hidden.";
    if (now.FullNightLux < kNegligibleLux)
        return lead + "the moon stays too low to light the scene.";
    return lead + "the moon gives up to " + FormatLux(now.FullNightLux) + " on a surface facing it.";
}

std::string SkySunNotDrivingLine(SkySunNotDriving reason, const std::string& lightName)
{
    const std::string quoted = "\"" + lightName + "\"";
    switch (reason)
    {
    case SkySunNotDriving::DriveOff:
        return "The sky is not lighting the scene: turn on Time -> Light Source below to let it move and colour " +
               quoted + ".";
    case SkySunNotDriving::LightHasParent:
        return "The sky cannot move " + quoted + ": it has a parent, and the sky can only move a light that has none.";
    case SkySunNotDriving::NotLinked:
        break;
    }
    return "The sky is not lighting the scene: link " + quoted + " as the Sun Light above to let it move and colour "
           "that light.";
}

} // namespace GameEngine
