#include "Components/Rendering/SkySunDrive.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunPath.h"
#include "Rendering/Sky/AtmosphereTransmittance.h"
#include "Rendering/Sky/SkySystem.h"
#include "Rendering/Sky/SolarPath.h"
#include "Types/ColorUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace GameEngine::Components::SkySunDrive
{
namespace
{
constexpr float kDayHours = 24.0f;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDegreesToRadians = kPi / 180.0;

// Where the seed places its keys, in degrees of sun elevation, on either side of noon.
constexpr float kSeedElevationsDegrees[] = {-6.0f, -4.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 35.0f, 55.0f};
// Below this elevation a seeded segment is Linear in the log domain; above it, Smooth with Auto
// tangents. The keys placed at it read it back a hair either side, so it is compared with a margin.
constexpr float kSmoothAboveElevationDegrees = 5.0f - 0.01f;
// The seed is refined until it is within this many stops of the model...
constexpr float kSeedToleranceStops = 0.5f;
// ...wherever the model gives more than this, in lux.
constexpr float kSeedMinimumModelLux = 0.01f;
// The refinement compares the seed with the model once a minute.
constexpr int kSeedSamplesPerDay = 24 * 60;
// The preview samples the model every 15 minutes, both ends of the day included.
constexpr int kPreviewSamples = 24 * 4 + 1;

float ElevationAtHour(const Rendering::SolarFrame& frame, float hours)
{
    return Rendering::SolarPositionAtHour(frame, hours).ElevationDegrees;
}

// The two hours at which the sun's centre crosses `elevationDegrees` on the path, earlier first, or
// false when it never does (the whole day is above or below it).
bool HoursAtElevation(const Rendering::SolarPathAngles& angles, float elevationDegrees, float& morning, float& evening)
{
    const double sinPole = std::sin(angles.PoleAltitudeRadians);
    const double cosPole = std::cos(angles.PoleAltitudeRadians);
    const double sinDeclination = std::sin(angles.DeclinationRadians);
    const double cosDeclination = std::cos(angles.DeclinationRadians);
    const double denominator = cosPole * cosDeclination;
    if (std::abs(denominator) < 1e-9)
        return false;
    const double cosHourAngle =
        (std::sin(elevationDegrees * kDegreesToRadians) - sinPole * sinDeclination) / denominator;
    if (cosHourAngle <= -1.0 || cosHourAngle >= 1.0)
        return false;
    const double hoursFromNoon = std::acos(cosHourAngle) * 12.0 / kPi;
    morning = static_cast<float>(12.0 - hoursFromNoon);
    evening = static_cast<float>(12.0 + hoursFromNoon);
    return true;
}

// Three significant figures: a seed key an author reads and may retype ("97 700", "0.697"), far inside
// the seed's half-stop tolerance.
float ThreeSignificantFigures(float value)
{
    const double scale = std::pow(10.0, std::floor(std::log10(static_cast<double>(value))) - 2.0);
    return static_cast<float>(std::round(value / scale) * scale);
}

// A seed key: the model's value to three significant figures, held at the curve's floor where the
// model gives less, so a key at night reads 0.001 lx in the inspector and the scene file rather than a
// denormal.
Math::CurveKey SeedKey(float hours, float lux)
{
    Math::CurveKey key{};
    key.Time = hours;
    key.Value = lux > kCurveFloorLux ? ThreeSignificantFigures(lux) : kCurveFloorLux;
    key.Interp = Math::CurveInterp::Linear;
    key.TangentMode = Math::CurveTangentMode::Auto;
    return key;
}

// Smooth where the segment's two ends are both above kSmoothAboveElevationDegrees, Linear otherwise.
void AssignSeedInterpolation(Math::Curve& curve, const Rendering::SolarFrame& frame)
{
    for (uint8_t k = 0; k < curve.KeyCount; ++k)
    {
        const float start = ElevationAtHour(frame, curve.Keys[k].Time);
        const bool hasNext = k + 1 < curve.KeyCount;
        const float end = hasNext ? ElevationAtHour(frame, curve.Keys[k + 1].Time) : start;
        curve.Keys[k].Interp = (hasNext && start >= kSmoothAboveElevationDegrees && end >= kSmoothAboveElevationDegrees)
                                   ? Math::CurveInterp::Smooth
                                   : Math::CurveInterp::Linear;
    }
}

bool HasKeyAt(const Math::Curve& curve, float hours)
{
    for (uint8_t k = 0; k < curve.KeyCount; ++k)
        if (std::abs(curve.Keys[k].Time - hours) < 1e-3f)
            return true;
    return false;
}

// The curve's error against the model in stops, where the model gives light: the sample with the
// largest error, or -1 when every sample is within tolerance.
int WorstSample(const Math::Curve& curve, const std::array<float, kSeedSamplesPerDay>& model)
{
    int worst = -1;
    float worstStops = kSeedToleranceStops;
    for (int m = 0; m < kSeedSamplesPerDay; ++m)
    {
        if (model[m] <= kSeedMinimumModelLux)
            continue;
        const float hours = static_cast<float>(m) / 60.0f;
        const float curveLux = std::max(CurveIlluminanceLux(curve, hours), kCurveFloorLux);
        const float stops = std::abs(std::log2(curveLux / model[m]));
        if (stops > worstStops)
        {
            worstStops = stops;
            worst = m;
        }
    }
    return worst;
}

float LuminanceOfGround(const float source[3], float upDot)
{
    float ground[3];
    Rendering::EvaluateGroundLevelSunColor(Rendering::ScatteringAtmosphere(), source, upDot, ground);
    return ColorUtils::LinearRec709Luminance(ground);
}

// The default sky's physical curve for a 100 000 lx sun (PhysicalSeedCurve at the component's
// defaults), recorded as literal keys: hours, lux, and the segment's interpolation (0 Linear,
// 1 Smooth).
constexpr float kDefaultSunIlluminanceKeys[][3] = {
    {0.0f, 0.001f, 0.0f},
    {5.44999981f, 0.0146f, 0.0f},
    {5.5999999f, 0.697f, 0.0f},
    {5.73333311f, 22.1f, 0.0f},
    {5.86666679f, 778.0f, 0.0f},
    {5.9333334f, 4860.0f, 0.0f},
    {6.0f, 4860.0f, 0.0f},
    {6.0666666f, 11300.0f, 0.0f},
    {6.13333321f, 18900.0f, 0.0f},
    {6.33333349f, 40300.0f, 1.0f},
    {6.66666651f, 62600.0f, 1.0f},
    {7.33333349f, 81800.0f, 1.0f},
    {8.33333302f, 92400.0f, 1.0f},
    {9.66666698f, 97700.0f, 1.0f},
    {12.0f, 100000.0f, 1.0f},
    {14.333333f, 97700.0f, 1.0f},
    {15.666667f, 92400.0f, 1.0f},
    {16.666666f, 81800.0f, 1.0f},
    {17.333334f, 62600.0f, 1.0f},
    {17.666666f, 40300.0f, 0.0f},
    {17.8666668f, 18900.0f, 0.0f},
    {17.9333324f, 11300.0f, 0.0f},
    {18.0f, 4860.0f, 0.0f},
    {18.0666676f, 4860.0f, 0.0f},
    {18.1333332f, 778.0f, 0.0f},
    {18.2666664f, 22.1f, 0.0f},
    {18.3999996f, 0.697f, 0.0f},
    {18.5499992f, 0.0146f, 0.0f},
};

float ModelLux(const SkyEnvironment& sky, const Rendering::SolarFrame& frame, float sunLux, float hours)
{
    return SkySunPath::PhysicalSunIlluminanceLux(sky, ElevationAtHour(frame, hours), sunLux, hours);
}
} // namespace

float CurveIlluminanceLux(const Math::Curve& curve, float hours)
{
    float wrapped = std::fmod(hours, kDayHours);
    if (wrapped < 0.0f)
        wrapped += kDayHours;
    return Math::EvaluateCurveKeysLogarithmic(curve.Keys, curve.KeyCount, wrapped, kCurveFloorLux, kDayHours);
}

float PhysicalIlluminanceLux(const SkyEnvironment& sky, float sunLux, float hours)
{
    return ModelLux(sky, Rendering::MakeSolarFrame(SkySunPath::PathAngles(sky)), sunLux, hours);
}

bool SunOnlyHours(const SkyEnvironment& sky, float& outStart, float& outEnd)
{
    const Rendering::SolarPathAngles angles = SkySunPath::PathAngles(sky);
    const float threshold = static_cast<float>(std::asin(Rendering::SunOnlyUpDot(Rendering::SkySystemConfig{})) / kDegreesToRadians);
    if (HoursAtElevation(angles, threshold, outStart, outEnd))
        return true;
    outStart = 0.0f;
    outEnd = kDayHours;
    return Rendering::SolarNoonElevationDegrees(angles) >= threshold;
}

std::vector<Math::CurveKey> PhysicalCurveSamples(const SkyEnvironment& sky, float sunLux)
{
    const Rendering::SolarFrame frame = Rendering::MakeSolarFrame(SkySunPath::PathAngles(sky));
    std::vector<Math::CurveKey> samples;
    samples.reserve(kPreviewSamples);
    for (int i = 0; i < kPreviewSamples; ++i)
    {
        const float hours = kDayHours * static_cast<float>(i) / static_cast<float>(kPreviewSamples - 1);
        samples.push_back(SeedKey(hours, ModelLux(sky, frame, sunLux, hours)));
    }
    return samples;
}

Math::Curve PhysicalSeedCurve(const SkyEnvironment& sky, float sunLux)
{
    const Rendering::SolarPathAngles angles = SkySunPath::PathAngles(sky);
    const Rendering::SolarFrame frame = Rendering::MakeSolarFrame(angles);
    Math::Curve curve{};
    const float dayLength = Rendering::SolarDayLengthHours(angles);
    if (dayLength <= 0.0f || dayLength >= kDayHours)
    {
        for (int hour = 0; hour < static_cast<int>(kDayHours); ++hour)
            curve.TryInsert(SeedKey(static_cast<float>(hour), ModelLux(sky, frame, sunLux, static_cast<float>(hour))));
        AssignSeedInterpolation(curve, frame);
        return curve;
    }

    curve.TryInsert(SeedKey(0.0f, ModelLux(sky, frame, sunLux, 0.0f)));
    curve.TryInsert(SeedKey(12.0f, ModelLux(sky, frame, sunLux, 12.0f)));
    for (const float elevation : kSeedElevationsDegrees)
    {
        float morning = 0.0f;
        float evening = 0.0f;
        if (!HoursAtElevation(angles, elevation, morning, evening))
            continue;
        curve.TryInsert(SeedKey(morning, ModelLux(sky, frame, sunLux, morning)));
        curve.TryInsert(SeedKey(evening, ModelLux(sky, frame, sunLux, evening)));
    }
    AssignSeedInterpolation(curve, frame);

    std::array<float, kSeedSamplesPerDay> model{};
    for (int m = 0; m < kSeedSamplesPerDay; ++m)
        model[m] = ModelLux(sky, frame, sunLux, static_cast<float>(m) / 60.0f);
    while (curve.KeyCount < Math::Curve::Capacity)
    {
        const int worst = WorstSample(curve, model);
        if (worst < 0)
            break;
        const float hours = static_cast<float>(worst) / 60.0f;
        if (HasKeyAt(curve, hours))
            break;
        curve.TryInsert(SeedKey(hours, model[worst]));
        AssignSeedInterpolation(curve, frame);
    }
    return curve;
}

Math::Curve DefaultSunIlluminanceCurve()
{
    Math::Curve curve{};
    for (const auto& key : kDefaultSunIlluminanceKeys)
    {
        Math::CurveKey k = SeedKey(key[0], key[1]);
        k.Interp = static_cast<Math::CurveInterp>(static_cast<int>(key[2]));
        curve.TryInsert(k);
    }
    return curve;
}

bool IsDefaultSunIlluminanceCurve(const Math::Curve& curve)
{
    const Math::Curve defaults = DefaultSunIlluminanceCurve();
    return std::memcmp(&curve, &defaults, sizeof(Math::Curve)) == 0;
}

bool SeedCurveIfUnauthored(SkyEnvironment& sky, float sunLux)
{
    if (!IsDefaultSunIlluminanceCurve(sky.SunIlluminanceCurve))
        return false;
    sky.SunIlluminanceCurve = PhysicalSeedCurve(sky, sunLux);
    return true;
}

float SkySourceLux(const SkyEnvironment& sky, float hours)
{
    float sunStart = 0.0f;
    float sunEnd = 0.0f;
    if (!SunOnlyHours(sky, sunStart, sunEnd))
        return NoonReferenceLux(sky);
    float wrapped = std::fmod(hours, kDayHours);
    if (wrapped < 0.0f)
        wrapped += kDayHours;
    float sampled = wrapped;
    if (wrapped < sunStart || wrapped > sunEnd)
    {
        const float toStart = wrapped < sunStart ? sunStart - wrapped : sunStart + kDayHours - wrapped;
        const float fromEnd = wrapped > sunEnd ? wrapped - sunEnd : wrapped + kDayHours - sunEnd;
        sampled = toStart <= fromEnd ? sunStart : sunEnd;
    }
    const float model = PhysicalIlluminanceLux(sky, kClearNoonSunIlluminanceLux, sampled);
    if (model <= 0.0f)
        return NoonReferenceLux(sky);
    return kClearNoonSunIlluminanceLux * CurveIlluminanceLux(sky.SunIlluminanceCurve, sampled) / model;
}

float NoonReferenceLux(const SkyEnvironment& sky)
{
    const float noonElevation = Rendering::SolarNoonElevationDegrees(SkySunPath::PathAngles(sky));
    const Rendering::SkySystemConfig config{};
    const float noonUpDot = std::max(static_cast<float>(std::sin(noonElevation * kDegreesToRadians)),
                                     Rendering::SunOnlyUpDot(config));
    const float white[3] = {1.0f, 1.0f, 1.0f};
    return CurveIlluminanceLux(sky.SunIlluminanceCurve, 12.0f) / LuminanceOfGround(white, noonUpDot);
}

float MoonlightLux(const SkyEnvironment& sky, float moonUpDot)
{
    if (!sky.ShowMoon)
        return 0.0f;
    const Rendering::SkySystemConfig config{};
    float moonGround[3];
    Rendering::EvaluateGroundLevelSunColor(Rendering::ScatteringAtmosphere(), config.moonColor, moonUpDot, moonGround);
    return MoonlightLuxFromGround(sky, moonGround);
}

float MoonlightLuxFromGround(const SkyEnvironment& sky, const float moonGround[3])
{
    if (!sky.ShowMoon)
        return 0.0f;
    const Rendering::SkySystemConfig config{};
    return SanitisedMoonlightLux(sky.MoonlightIlluminance) * ColorUtils::LinearRec709Luminance(moonGround) /
           ColorUtils::LinearRec709Luminance(config.moonColor);
}

float MoonlightRatio(const SkyEnvironment& sky)
{
    return SanitisedMoonlightLux(sky.MoonlightIlluminance) / kDefaultMoonlightIlluminanceLux;
}

float SanitisedMoonlightLux(float lux)
{
    return std::isfinite(lux) ? std::clamp(lux, 0.0f, kMoonlightIlluminanceMaxLux) : kDefaultMoonlightIlluminanceLux;
}

} // namespace GameEngine::Components::SkySunDrive
