#pragma once

#include "Rendering/Sky/SolarPath.h" // Rendering::SolarPathAngles

// Where a sky's sun travels: the path its Sun path choice selects (a place on Earth, or a custom
// axis and noon height, all fields of the sky) and the illuminance the physical model delivers along
// the way. The sky system, the sky inspector and its Scene View overlay all read it here, so the
// preview, the overlay and the frame agree.
//
// Free functions only: this directory is scanned for components, and every struct here would
// register as one.
namespace GameEngine::Components
{
struct SkyEnvironment;

namespace SkySunPath
{

// The angles of the path the sky's Sun path choice selects.
Rendering::SolarPathAngles PathAngles(const SkyEnvironment& sky);

// The sky's sun path fields as an author sets them, each brought into its range: the latitude and
// the axis altitude to [-90, 90], a heading to [0, 360), and the noon height to what the axis can
// reach. A new axis altitude brings the stored noon height back into reach in the same edit, so the
// sky never holds a height its path clamps away.
void SetLatitude(SkyEnvironment& sky, float degrees);
void SetNorthHeading(SkyEnvironment& sky, float degrees);
void SetCustomAxisHeading(SkyEnvironment& sky, float degrees);
void SetCustomAxisAltitude(SkyEnvironment& sky, float degrees);
void SetCustomNoonHeight(SkyEnvironment& sky, float degrees);

// What an axis altitude edit writes while it is in flight (a drag, a number being typed): the axis
// moves and the stored noon height stays the one the edit started with, which the path brings into
// the axis's reach (PathAngles). Bringing the stored height into reach is the commit's,
// SetCustomAxisAltitude: done at every step, an altitude passed on the way ("7" while typing "70")
// would cut a height the final axis reaches.
void PreviewCustomAxisAltitude(SkyEnvironment& sky, float degrees);

// Start a Custom path where the Earth path is, so the sun does not jump when the author first
// switches: while the three Custom fields are still the component's stored defaults, set the axis
// heading to north, the axis altitude to the latitude and the noon height to the Earth noon height of
// the day (90 - latitude + declination, brought into reach). Once any Custom field has been
// authored this does nothing, and switching keeps the authored values. True when it seeded.
bool SeedCustomPathFromEarth(SkyEnvironment& sky);

// What the physical model delivers, in lux, to a surface facing a sun `sunElevationDegrees` above
// the horizon at `hours`, when its clear, overhead illuminance is `sunLux`: the illuminance times
// the luminance of the ground-level transmittance at that elevation, with the sky's sun tint for
// that hour. The sun alone, with no handover to the moon. It takes an elevation rather than a
// site, so any path that says where the sun is over the day can be described by it.
float PhysicalSunIlluminanceLux(const SkyEnvironment& sky, float sunElevationDegrees, float sunLux, float hours);

// The sun's own share of what the light the sky drives delivers now to a surface facing it, in lux,
// for a sun `sunElevationDegrees` above the horizon at `hours`: what the sun gives (the sky's curve
// under the curve source, else PhysicalSunIlluminanceLux for a clear, overhead sun of `lightLux`) less
// the part the sky has handed over to the moon. 0 once the sun is below the twilight floor, where the
// light is all moon; the rest of what the light delivers is the night share the handover blends in.
float DrivenSunShareLux(const SkyEnvironment& sky, float sunElevationDegrees, float lightLux, float hours);

// How far the sky has handed the light it drives over from the sun to the moon, for a sun
// `sunElevationDegrees` above the horizon: 0 while the sun is well up, 1 below the twilight floor.
// Always 0 while the sun is placed by hand (AutoSunMoon off).
float DrivenMoonBlend(const SkyEnvironment& sky, float sunElevationDegrees);

// The highest the moon stands on the sky's path, in degrees above the horizon. The moon rides the
// sun's circle, offset along it (SkySystem.h), so its highest is the sun's, the noon elevation: on a
// path whose sun never rises the moon never rises either.
float MoonHighestElevationDegrees(const SkyEnvironment& sky);

// The most moonlight a night on the sky's path has, in lux: what the light the sky drives delivers at
// full night with the moon at its highest, the sky's Moonlight dimmed by the air it crosses at that
// height. 0 while the moon is hidden, and on a path where it never rises.
float FullNightMoonLux(const SkyEnvironment& sky);

} // namespace SkySunPath
} // namespace GameEngine::Components
