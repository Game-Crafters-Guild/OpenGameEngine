#pragma once

#include "Sky/SkySunDayKind.h"

#include <string>

namespace GameEngine
{

// The wording of the sky inspector's sun readout card, one sentence per line, from figures the card
// reads off the sky: what each figure is, in plain words, and only the lines that apply. Pure text,
// so each case reads the same in a test as on screen.

// The day the sky's sun path describes.
struct SkySunDay
{
    // Custom paths have no compass: the noon sun is placed by its heading instead.
    bool CustomPath = false;
    // What the sun does over the day, which picks the sentence.
    Editor::SkySunDayKind Kind = Editor::SkySunDayKind::RisesAndSets;
    // The Custom axis heading, in degrees; the noon sun's heading is this plus its azimuth.
    float AxisHeadingDegrees = 0.0f;
    float DayLengthHours = 12.0f;
    // The sun's highest and lowest, at noon and at midnight, in degrees above the horizon.
    float NoonElevationDegrees = 90.0f;
    float MidnightElevationDegrees = -90.0f;
    // Where the noon sun stands, in degrees clockwise seen from above from north (Earth) or from
    // the axis heading (Custom).
    float NoonAzimuthDegrees = 0.0f;
    // What the light the sky drives delivers from the sun at noon, facing it: the same quantity as
    // the Now line's, so the two agree at 12:00. 0 when the scene has no sun light.
    float NoonLux = 0.0f;

    bool operator==(const SkySunDay&) const = default;
};

// "Sunrise 06:00 and sunset 18:00: 12 h of daylight.", or a plain sentence for a day the sun never
// sets (polar day), never rises (polar night), circles at one height or along the horizon, or
// stands still.
std::string SkySunDayLine(const SkySunDay& day);

// "At noon the sun is 62.0° above the horizon, to the south, and gives 98 600 lx on a surface facing
// it.", or on a day the sun never rises its height alone: "At its highest, at noon, the sun is 8.4°
// below the horizon." Empty when the day line has already said where the noon sun is.
std::string SkySunNoonLine(const SkySunDay& day);

// What the light the sky drives delivers at the current hour, split between the sun and the night.
struct SkySunNow
{
    float Hours = 12.0f;
    float SunElevationDegrees = 90.0f;
    // The sun's own share, facing it; 0 once the sun is below the twilight floor.
    float SunLux = 0.0f;
    // The rest of what the light delivers: the night share the sky blends in as it hands the light
    // over from the sun to the moon.
    float NightLux = 0.0f;
    // How far that handover has gone: 0 by day, 1 at full night.
    float HandoverWeight = 0.0f;
    // How far the handover goes at midnight, with the sun at its lowest: short of full night, the
    // night never gets fully dark.
    float MidnightHandoverWeight = 1.0f;
    // The most the light delivers at full night: the moonlight with the moon at its highest on the
    // path (SkySunPath::FullNightMoonLux).
    float FullNightLux = 0.0f;
    // False on a path whose sun never rises: the moon rides the sun's circle, so it never rises
    // there either.
    bool MoonRises = true;
    // The sky's Show Moon is off, and a hidden moon lights nothing.
    bool MoonHidden = false;
    // False while Auto Sun/Moon is off: the sun is placed by hand and the sky hands nothing to the
    // moon (SkySunPath::DrivenMoonBlend), so there is no night to describe.
    bool HandsOverToMoon = true;

    bool operator==(const SkySunNow&) const = default;
};

// True when the hour reads 12:00. The Now line then says what the noon line would, side included,
// and the card shows it in the noon line's place: one sentence for one moment.
bool SkySunNowIsNoon(const SkySunNow& now);

// "Now, at 17:45, the sun is 7.5° above the horizon: 53 200 lx on a surface facing it, 6 900 lx on
// level ground.", with what twilight still carries of a set sun's light, or none, said as such. At
// 12:00 it is "Now, at noon, the sun is 62.0° above the horizon, to the south: ...", with the side
// `day`'s noon line gives. A sun that stands still is where it is at every hour, so its line gives
// no position: "Now: 90 000 lx on a surface facing it, 77 900 lx on level ground."
std::string SkySunNowLine(const SkySunDay& day, const SkySunNow& now);

// The moon, with the sun below the horizon: "Moonlight now: 5.2 lx on a surface facing the moon."
// at full night ("The moon gives no light now." when it gives none); while the sky is still handing
// the light over from the sun, only where that ends, led by the way the sun is going: "Night is
// still falling. At full night the moon gives up to 4.8 lx on a surface facing it." after noon,
// "Night is lifting. ..." before it, and no live figure. Where the night never gets fully dark, or
// the moon never rises, it says that instead. An Earth card speaks of tonight, a Custom card of
// this path. Nothing while the sun is up, or while it is placed by hand.
std::string SkyMoonNowLine(const SkySunDay& day, const SkySunNow& now);

// Why the sky lights nothing now, when it does not drive a light.
enum class SkySunNotDriving
{
    // A light is linked, but the sky's drive is off.
    DriveOff,
    // A light is linked, but it has a parent, and the sky can only drive a light without one.
    LightHasParent,
    // No light is linked; the scene's sun is `lightName`.
    NotLinked,
};

// "The sky is not lighting the scene: turn on Time -> Light Source below to let it move and colour
// "Sun"." and its siblings.
std::string SkySunNotDrivingLine(SkySunNotDriving reason, const std::string& lightName);

// "03:48", rounded to the minute, for an hour of the day from 0 to 24. The day's end reads 24:00,
// as it does under the graph of the day, so the card and the graph name one moment one way.
std::string SolarClockText(float hours);

} // namespace GameEngine
