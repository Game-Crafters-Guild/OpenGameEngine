#include "Rendering/Sky/SolarPath.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace Rendering {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegreesToRadians = kPi / 180.0;
constexpr double kRadiansToDegrees = 180.0 / kPi;
constexpr double kTropicalYearDays = 365.2422;
constexpr double kAxialTiltRadians = 23.44 * kDegreesToRadians;
constexpr double kOrbitalEccentricity = 0.0167;
// Perihelion falls on 3 or 4 January. The 0.1-degree agreement with the Spencer series holds only
// with perihelion near this day.
constexpr double kPerihelionDay = 4.0;

// An altitude above the horizon, latitude or axis alike: clamped to [-90, 90] degrees, and 0 when
// not finite.
float SanitisedAltitudeDegrees(float altitudeDegrees)
{
    return std::isfinite(altitudeDegrees)
               ? std::clamp(altitudeDegrees, -kMaximumLatitudeDegrees, kMaximumLatitudeDegrees)
               : 0.0f;
}

double SanitisedAltitudeRadians(float altitudeDegrees)
{
    return static_cast<double>(SanitisedAltitudeDegrees(altitudeDegrees)) * kDegreesToRadians;
}

double HeadingRadians(float headingDegrees)
{
    return std::isfinite(headingDegrees) ? static_cast<double>(headingDegrees) * kDegreesToRadians : 0.0;
}

int32_t SanitisedDay(int32_t dayOfYear)
{
    return std::clamp(dayOfYear, int32_t{1}, kDaysInCalendarYear);
}

double MeanAnomaly(double day)
{
    return 2.0 * kPi * (day - kPerihelionDay) / kTropicalYearDays;
}

} // namespace

float SanitisedHeadingDegrees(float degrees)
{
    if (!std::isfinite(degrees))
        return 0.0f;
    const float wrapped = std::fmod(degrees, kFullTurnDegrees);
    const float positive = wrapped < 0.0f ? wrapped + kFullTurnDegrees : wrapped;
    // A tiny negative heading wraps to exactly 360 in float, which is 0; adding +0 turns -0 into +0,
    // so a heading of -0 builds the same frame as the default.
    return positive < kFullTurnDegrees ? positive + 0.0f : 0.0f;
}

float SolarDeclinationRadians(int32_t dayOfYear)
{
    const double day = static_cast<double>(SanitisedDay(dayOfYear));
    const double equinox = static_cast<double>(kMarchEquinoxDay);
    // Ecliptic longitude from the March equinox: the mean motion plus the equation of centre,
    // 2e (sin M(day) - sin M(equinox)). The difference of sines is written as a product so the
    // longitude is exactly zero on the equinox day, whatever the rounding of each sine.
    const double meanMotion = 2.0 * kPi * (day - equinox) / kTropicalYearDays;
    const double midAnomaly = 0.5 * (MeanAnomaly(day) + MeanAnomaly(equinox));
    const double longitude =
        meanMotion + 4.0 * kOrbitalEccentricity * std::cos(midAnomaly) * std::sin(0.5 * meanMotion);
    return static_cast<float>(std::asin(std::sin(kAxialTiltRadians) * std::sin(longitude)));
}

SolarPathAngles EarthPathAngles(const SolarSite& site)
{
    SolarPathAngles angles;
    angles.PoleAltitudeRadians = SanitisedAltitudeRadians(site.LatitudeDegrees);
    angles.PoleHeadingRadians = HeadingRadians(site.NorthDegrees);
    angles.DeclinationRadians = static_cast<double>(SolarDeclinationRadians(site.DayOfYear));
    return angles;
}

SolarPathAngles CustomPathAngles(float axisAltitudeDegrees, float axisHeadingDegrees, float noonHeightDegrees)
{
    const float noonHeight = std::isfinite(noonHeightDegrees)
                                 ? std::clamp(noonHeightDegrees, kLowestNoonHeightDegrees, kHighestNoonHeightDegrees)
                                 : kOverheadNoonHeightDegrees;
    SolarPathAngles angles;
    angles.PoleAltitudeRadians = SanitisedAltitudeRadians(axisAltitudeDegrees);
    angles.PoleHeadingRadians = HeadingRadians(axisHeadingDegrees);
    // The noon sun stands (90 - noon height) degrees from the zenith on the side away from the pole's
    // heading, and the pole (90 - altitude) degrees from the zenith on its own side, so the sun is
    // (noon height + altitude) degrees from the pole: its declination is that less 90.
    const double declinationDegrees =
        static_cast<double>(noonHeight) + static_cast<double>(SanitisedAltitudeDegrees(axisAltitudeDegrees)) - 90.0;
    angles.DeclinationRadians = std::clamp(declinationDegrees, -90.0, 90.0) * kDegreesToRadians;
    return angles;
}

NoonHeightRange ReachableNoonHeights(float axisAltitudeDegrees)
{
    const float altitude = SanitisedAltitudeDegrees(axisAltitudeDegrees);
    return {-altitude, kAxisSideHorizonNoonHeightDegrees - altitude};
}

float ReachableNoonHeightDegrees(float noonHeightDegrees, float axisAltitudeDegrees)
{
    const NoonHeightRange reach = ReachableNoonHeights(axisAltitudeDegrees);
    const float height = std::isfinite(noonHeightDegrees) ? noonHeightDegrees : kOverheadNoonHeightDegrees;
    return std::clamp(height, reach.Lowest, reach.Highest);
}

SolarFrame MakeSolarFrame(const SolarPathAngles& angles)
{
    const double sinAltitude = std::sin(angles.PoleAltitudeRadians);
    const double cosAltitude = std::cos(angles.PoleAltitudeRadians);
    const double sinHeading = std::sin(angles.PoleHeadingRadians);
    const double cosHeading = std::cos(angles.PoleHeadingRadians);

    // Up = +Y, the pole's heading = +Z turned by the heading's yaw, east = +X turned by the same
    // yaw; the meridian leans away from the heading.
    SolarFrame frame{};
    frame.East[0] = static_cast<float>(cosHeading);
    frame.East[1] = 0.0f;
    frame.East[2] = static_cast<float>(-sinHeading);
    frame.Meridian[0] = static_cast<float>(-sinAltitude * sinHeading);
    frame.Meridian[1] = static_cast<float>(cosAltitude);
    frame.Meridian[2] = static_cast<float>(-sinAltitude * cosHeading);
    frame.Pole[0] = static_cast<float>(cosAltitude * sinHeading);
    frame.Pole[1] = static_cast<float>(sinAltitude);
    frame.Pole[2] = static_cast<float>(cosAltitude * cosHeading);

    frame.CosDeclination = static_cast<float>(std::cos(angles.DeclinationRadians));
    frame.SinDeclination = static_cast<float>(std::sin(angles.DeclinationRadians));
    return frame;
}

float SolarHourFromDirection(const SolarFrame& frame, const float direction[3])
{
    constexpr float kPiFloat = 3.14159265f;
    constexpr float kTwoPi = 2.0f * kPiFloat;

    // direction = cos(delta) (cos(theta) east + sin(theta) meridian) + sin(delta) pole: the
    // equatorial components recover theta, then invert theta = (t - 0.25) * 2 pi.
    const float c = direction[0] * frame.East[0] + direction[1] * frame.East[1] + direction[2] * frame.East[2];
    const float s = direction[0] * frame.Meridian[0] + direction[1] * frame.Meridian[1] +
                    direction[2] * frame.Meridian[2];
    const float theta = std::atan2(s, c); // (-pi, pi]
    float t = theta / kTwoPi + 0.25f;
    t = t - std::floor(t); // [0, 1)
    return t * 24.0f;
}

SolarPosition SolarPositionAtHour(const SolarFrame& frame, float hours)
{
    constexpr float kPiFloat = 3.14159265f;
    constexpr float kTwoPi = 2.0f * kPiFloat;

    // The same hour-to-angle mapping ComputeSimpleSunMoon uses, so the position is the rendered sun's.
    float t = hours / 24.0f;
    t = t - std::floor(t);
    const float theta = (t - 0.25f) * kTwoPi;
    float direction[3];
    SolarDirection(frame, std::cos(theta), std::sin(theta), direction);

    // The site's north is its east turned a quarter turn about +Y, against the engine's yaw.
    const float north[3] = {-frame.East[2], 0.0f, frame.East[0]};
    const float towardEast = direction[0] * frame.East[0] + direction[2] * frame.East[2];
    const float towardNorth = direction[0] * north[0] + direction[2] * north[2];

    SolarPosition position;
    position.ElevationDegrees =
        static_cast<float>(std::asin(std::clamp(direction[1], -1.0f, 1.0f)) * kRadiansToDegrees);
    float azimuth = static_cast<float>(std::atan2(towardEast, towardNorth) * kRadiansToDegrees);
    if (azimuth < 0.0f)
        azimuth += kFullTurnDegrees;
    position.AzimuthDegrees = azimuth < kFullTurnDegrees ? azimuth : 0.0f;
    return position;
}

float SolarDayLengthHours(const SolarPathAngles& angles)
{
    const double altitude = angles.PoleAltitudeRadians;
    const double declination = angles.DeclinationRadians;
    // Sunrise equation, cos(H0) = -tan(pole altitude) tan(declination), written without tangents so
    // a vertical pole stays finite. Outside [-1, 1] the sun never sets (polar day) or never rises
    // (polar night).
    const double denominator = std::cos(altitude) * std::cos(declination);
    const double numerator = -std::sin(altitude) * std::sin(declination);
    double cosHalfDay = 0.0;
    if (std::abs(denominator) > 0.0)
        cosHalfDay = numerator / denominator;
    else if (numerator != 0.0)
        cosHalfDay = numerator > 0.0 ? 1.0 : -1.0;
    cosHalfDay = std::clamp(cosHalfDay, -1.0, 1.0);
    return static_cast<float>(24.0 / kPi * std::acos(cosHalfDay));
}

float SolarNoonElevationDegrees(const SolarPathAngles& angles)
{
    const double altitude = angles.PoleAltitudeRadians * kRadiansToDegrees;
    const double declination = angles.DeclinationRadians * kRadiansToDegrees;
    return static_cast<float>(90.0 - std::abs(altitude - declination));
}

float SolarMidnightElevationDegrees(const SolarPathAngles& angles)
{
    // At midnight the sun is half a turn round its circle from noon, on the pole's side of the sky.
    const double altitude = angles.PoleAltitudeRadians * kRadiansToDegrees;
    const double declination = angles.DeclinationRadians * kRadiansToDegrees;
    return static_cast<float>(std::abs(altitude + declination) - 90.0);
}

} // namespace Rendering
} // namespace GameEngine
