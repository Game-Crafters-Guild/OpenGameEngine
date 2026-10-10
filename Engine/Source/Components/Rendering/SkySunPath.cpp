#include "Components/Rendering/SkySunPath.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunDrive.h"
#include "Rendering/Sky/AtmosphereTransmittance.h"
#include "Rendering/Sky/SkySystem.h"
#include "Types/ColorUtils.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Components::SkySunPath
{
namespace
{
constexpr float kDegreesToRadians = 0.01745329252f;
constexpr double kRadiansToDegrees = 57.295779513082320876;
constexpr float kHundredthsPerDegree = 100.0f;
} // namespace

Rendering::SolarPathAngles PathAngles(const SkyEnvironment& sky)
{
    if (sky.SunPath == SkySunPathKind::Custom)
        return Rendering::CustomPathAngles(sky.CustomAxisAltitude, sky.CustomAxisHeading, sky.CustomNoonHeight);
    return Rendering::EarthPathAngles({sky.Latitude, sky.DayOfYear, sky.NorthHeading});
}

void SetLatitude(SkyEnvironment& sky, float degrees)
{
    sky.Latitude = std::clamp(degrees, -Rendering::kMaximumLatitudeDegrees, Rendering::kMaximumLatitudeDegrees);
}

void SetNorthHeading(SkyEnvironment& sky, float degrees)
{
    sky.NorthHeading = Rendering::SanitisedHeadingDegrees(degrees);
}

void SetCustomAxisHeading(SkyEnvironment& sky, float degrees)
{
    sky.CustomAxisHeading = Rendering::SanitisedHeadingDegrees(degrees);
}

void PreviewCustomAxisAltitude(SkyEnvironment& sky, float degrees)
{
    sky.CustomAxisAltitude = std::clamp(degrees, -Rendering::kMaximumLatitudeDegrees, Rendering::kMaximumLatitudeDegrees);
}

void SetCustomAxisAltitude(SkyEnvironment& sky, float degrees)
{
    PreviewCustomAxisAltitude(sky, degrees);
    sky.CustomNoonHeight = Rendering::ReachableNoonHeightDegrees(sky.CustomNoonHeight, sky.CustomAxisAltitude);
}

void SetCustomNoonHeight(SkyEnvironment& sky, float degrees)
{
    sky.CustomNoonHeight = Rendering::ReachableNoonHeightDegrees(degrees, sky.CustomAxisAltitude);
}

bool SeedCustomPathFromEarth(SkyEnvironment& sky)
{
    const SkyEnvironment defaults{};
    if (sky.CustomAxisHeading != defaults.CustomAxisHeading || sky.CustomAxisAltitude != defaults.CustomAxisAltitude ||
        sky.CustomNoonHeight != defaults.CustomNoonHeight)
        return false;
    const Rendering::SolarPathAngles earth =
        Rendering::EarthPathAngles({sky.Latitude, sky.DayOfYear, sky.NorthHeading});
    const float altitude = static_cast<float>(earth.PoleAltitudeRadians * kRadiansToDegrees);
    const float declination = static_cast<float>(earth.DeclinationRadians * kRadiansToDegrees);
    sky.CustomAxisHeading = Rendering::SanitisedHeadingDegrees(sky.NorthHeading);
    sky.CustomAxisAltitude = altitude;
    // Kept to 0.01 degrees: the seeded height is a number the author reads and may retype, and a
    // hundredth of a degree is far under the model's own accuracy.
    const float noonHeight =
        std::round((Rendering::kOverheadNoonHeightDegrees - altitude + declination) * kHundredthsPerDegree) /
        kHundredthsPerDegree;
    sky.CustomNoonHeight = Rendering::ReachableNoonHeightDegrees(noonHeight, altitude);
    return true;
}

float PhysicalSunIlluminanceLux(const SkyEnvironment& sky, float sunElevationDegrees, float sunLux, float hours)
{
    const float dayKeyTimesHours[4] = {sky.DayKeyTimesHours.Midnight, sky.DayKeyTimesHours.Dawn,
                                       sky.DayKeyTimesHours.Midday, sky.DayKeyTimesHours.Sunset};
    float sunTint[3];
    Rendering::SampleSkyVec3KeyframeCurve(hours, dayKeyTimesHours, sky.SunTintKeys.Midnight, sky.SunTintKeys.Dawn,
                                          sky.SunTintKeys.Midday, sky.SunTintKeys.Sunset, sunTint);

    float ground[3];
    Rendering::EvaluateGroundLevelSunColor(Rendering::ScatteringAtmosphere(), sunTint,
                                           std::sin(sunElevationDegrees * kDegreesToRadians), ground);
    return sunLux * ColorUtils::LinearRec709Luminance(ground);
}

float DrivenMoonBlend(const SkyEnvironment& sky, float sunElevationDegrees)
{
    return sky.AutoSunMoon
               ? Rendering::PrimaryMoonBlend(Rendering::SkySystemConfig{}, std::sin(sunElevationDegrees * kDegreesToRadians))
               : 0.0f;
}

float MoonHighestElevationDegrees(const SkyEnvironment& sky)
{
    return Rendering::SolarNoonElevationDegrees(PathAngles(sky));
}

float FullNightMoonLux(const SkyEnvironment& sky)
{
    const float highest = MoonHighestElevationDegrees(sky);
    if (highest <= 0.0f)
        return 0.0f;
    // At full night the light the sky drives delivers the moonlight at the moon's elevation, in both
    // illuminance sources (SkyEnvironmentSystem's drive).
    return SkySunDrive::MoonlightLux(sky, std::sin(highest * kDegreesToRadians));
}

float DrivenSunShareLux(const SkyEnvironment& sky, float sunElevationDegrees, float lightLux, float hours)
{
    // The light's colour is the sun's ground colour weighted by (1 - blend) plus the moon's weighted
    // by the blend times the moon's scale (SkyEnvironmentSystem's drive, an energy blend), so the
    // sun's share is its own term alone.
    const float moonBlend = DrivenMoonBlend(sky, sunElevationDegrees);
    const float sunLux = sky.SunIlluminanceSource == SkySunIlluminanceSource::Curve
                             ? SkySunDrive::CurveIlluminanceLux(sky.SunIlluminanceCurve, hours)
                             : PhysicalSunIlluminanceLux(sky, sunElevationDegrees, lightLux, hours);
    return (1.0f - moonBlend) * sunLux;
}

} // namespace GameEngine::Components::SkySunPath
