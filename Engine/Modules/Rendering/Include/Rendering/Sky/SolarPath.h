#pragma once

#include <cstdint>

namespace GameEngine {
namespace Rendering {

/// The sun's path across the sky: the daily circle the sun traces about a celestial pole. For a
/// place on Earth and a day of the year the pole stands at the latitude and the circle's declination
/// comes from the day; a custom path authors the pole and the circle directly.
///
/// Geometric throughout: every angle is for the centre of the disc, with no refraction. Published
/// sunrise tables add about 0.83 degrees of refraction and disc radius, which lengthens a day at
/// 51.5 degrees north by about 14 minutes.
///
/// The hour is local apparent solar time: 12:00 is always solar noon. There is no longitude, time
/// zone or equation of time. Up is world +Y everywhere, so the path does not follow a curved
/// planet's local vertical.

/// 21 March in a non-leap year: the day the declination is exactly zero.
inline constexpr int32_t kMarchEquinoxDay = 80;
/// Days in the (non-leap) calendar year DayOfYear counts in.
inline constexpr int32_t kDaysInCalendarYear = 365;
inline constexpr float kMaximumLatitudeDegrees = 90.0f;
inline constexpr float kFullTurnDegrees = 360.0f;
/// A custom path's noon height is measured from the horizon on the side away from the axis, over
/// the top of the sky: 0 and 180 are the horizon (away side, axis side), 90 the zenith, and below 0
/// or above 180 the noon sun is under the horizon. Straight down is -90 on the away side and 270 on
/// the axis side; which of these an axis can reach is ReachableNoonHeights.
inline constexpr float kAxisSideHorizonNoonHeightDegrees = 180.0f;
inline constexpr float kLowestNoonHeightDegrees = -90.0f;
inline constexpr float kHighestNoonHeightDegrees = 270.0f;
/// The noon height that puts a custom path's noon sun straight overhead of a level axis: the
/// default, which reproduces the default Earth path.
inline constexpr float kOverheadNoonHeightDegrees = 90.0f;

/// Where a scene is on Earth and which way its north faces.
struct SolarSite
{
    /// Degrees, north positive, in [-90, 90].
    float LatitudeDegrees = 0.0f;
    /// 1 = 1 January, in a non-leap year.
    int32_t DayOfYear = kMarchEquinoxDay;
    /// The heading of north about +Y, in degrees, in the engine's rotation convention
    /// (Quaternion::FromAxisAngle about +Y): 0 puts north at +Z, 90 at +X.
    float NorthDegrees = 0.0f;
};

/// The three angles a sun path is built from, however they were authored, in radians: how high the
/// celestial pole stands above the horizon (the latitude, on Earth), the compass heading it lies
/// toward (north, on Earth), and the declination of the sun's daily circle from the celestial
/// equator.
struct SolarPathAngles
{
    double PoleAltitudeRadians = 0.0;
    double PoleHeadingRadians = 0.0;
    double DeclinationRadians = 0.0;
};

/// The frame the sun's daily circle is traced in. East, Meridian and Pole are orthonormal; East and
/// Meridian span the celestial equator.
struct SolarFrame
{
    float East[3];
    /// Where the celestial equator culminates: cos(pole altitude) up + sin(pole altitude) away from
    /// the pole's heading (south, on Earth).
    float Meridian[3];
    /// The celestial pole: sin(pole altitude) up + cos(pole altitude) toward its heading.
    float Pole[3];
    float CosDeclination;
    float SinDeclination;
};

/// A heading in degrees brought into [0, 360); a non-finite heading reads as 0.
float SanitisedHeadingDegrees(float degrees);

/// The sun's declination on `dayOfYear`, in radians: a circular orbit plus the equation of centre,
/// measured from the March equinox. Within 0.1 degrees of the Spencer (1971) series every day and
/// exactly zero on kMarchEquinoxDay.
float SolarDeclinationRadians(int32_t dayOfYear);

/// The angles of `site`'s path: the pole at the latitude, toward north, and the declination of the
/// day. A non-finite latitude or north reads as 0; the latitude is clamped to [-90, 90] and the day
/// to [1, 365].
SolarPathAngles EarthPathAngles(const SolarSite& site);

/// The angles of a custom path: an axis `axisAltitudeDegrees` above the horizon (clamped to
/// [-90, 90]) toward `axisHeadingDegrees`, and the sun's height at noon measured over the top: from
/// the horizon on the side away from the axis heading, through the zenith at 90, to the horizon on
/// the axis side at 180, and below the horizon either side of that (clamped to [-90, 270]). The
/// declination is noon height + axis altitude - 90, clamped to [-90, 90]; that clamp is where the
/// reachable noon heights depend on the axis. A non-finite angle reads as its default: 0, or 90 for
/// the noon height.
SolarPathAngles CustomPathAngles(float axisAltitudeDegrees, float axisHeadingDegrees, float noonHeightDegrees);

/// The noon heights an axis `axisAltitudeDegrees` above the horizon can reach: [-altitude,
/// 180 - altitude], the heights whose declination stays in [-90, 90], so each gives a different
/// path. An axis tilted up reaches noon heights below the horizon on the away side (the polar night);
/// one tilted down reaches heights past 180, below the horizon on the axis side.
struct NoonHeightRange
{
    float Lowest;
    float Highest;
};
NoonHeightRange ReachableNoonHeights(float axisAltitudeDegrees);

/// `noonHeightDegrees` brought into ReachableNoonHeights(axisAltitudeDegrees); a non-finite height
/// reads as overhead, then brought into reach.
float ReachableNoonHeightDegrees(float noonHeightDegrees, float axisAltitudeDegrees);

/// The frame for a path's angles.
SolarFrame MakeSolarFrame(const SolarPathAngles& angles);

/// The direction toward the sun at the point of its daily circle whose angle from the eastern end
/// of the equator is theta, given as its cosine and sine. theta = (hours / 24 - 1/4) * 2 pi, so
/// 06:00 is theta = 0 and solar noon is theta = pi / 2. The result is unit length by construction
/// and is deliberately not renormalised: at latitude 0 on the equinox with north 0 every frame
/// component is exactly 0 or 1, so the direction is exactly (cos theta, sin theta, +0).
inline void SolarDirection(const SolarFrame& frame, float cosTheta, float sinTheta, float out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = frame.CosDeclination * (cosTheta * frame.East[i] + sinTheta * frame.Meridian[i]) +
                 frame.SinDeclination * frame.Pole[i];
}

/// The solar hour, in [0, 24), whose direction is nearest `direction`: the hour angle of its
/// projection onto the celestial equator. Exact for a direction on the day's circle.
float SolarHourFromDirection(const SolarFrame& frame, const float direction[3]);

/// Where the sun stands at one hour, as an observer on the ground reads it.
struct SolarPosition
{
    /// Degrees above the horizon; negative below it.
    float ElevationDegrees = 0.0f;
    /// Compass bearing in degrees, in [0, 360): 0 = the site's north, 90 = its east. Measured in the
    /// site's own frame, so the North yaw turns the scene under it without changing it.
    float AzimuthDegrees = 0.0f;
};

/// The sun's elevation and azimuth at `hours` of local solar time on `frame`'s day: the quantities
/// anything that describes the day (the inspector's preview, its sunrise and noon readout) reads.
SolarPosition SolarPositionAtHour(const SolarFrame& frame, float hours);

/// Hours the centre of the sun spends above the horizon on the path, in [0, 24]: 0 when it never
/// rises (polar night), 24 when it never sets (polar day).
float SolarDayLengthHours(const SolarPathAngles& angles);

/// The sun's elevation at solar noon, its highest, in degrees; negative when it never rises.
float SolarNoonElevationDegrees(const SolarPathAngles& angles);

/// The sun's elevation at solar midnight, its lowest, in degrees; positive when it never sets.
float SolarMidnightElevationDegrees(const SolarPathAngles& angles);

} // namespace Rendering
} // namespace GameEngine
