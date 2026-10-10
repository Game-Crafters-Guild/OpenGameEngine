#pragma once

#include <algorithm>
#include <cmath>
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkySystemConfig.h"
#include "Rendering/Sky/SolarPath.h"

namespace GameEngine {
namespace Rendering {

struct SkySystemState
{
    float sunDirWS[3] = { 0.0f, 1.0f, 0.0f };
    float moonDirWS[3] = { 0.0f, -1.0f, 0.0f };
    /// 0 = full sun primary lighting, 1 = full moon; smoothly ramped near sunset.
    float primaryMoonBlend = 0.0f;
};

inline void Normalize3(float v[3])
{
    float len2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    if (len2 <= 0.0f) {
        v[0] = 0.0f; v[1] = 1.0f; v[2] = 0.0f;
        return;
    }
    float invLen = 1.0f / std::sqrt(len2);
    v[0] *= invLen; v[1] *= invLen; v[2] *= invLen;
}

inline float Smoothstep01(float edge0, float edge1, float x)
{
    if (edge1 <= edge0)
        return (x >= edge1) ? 1.0f : 0.0f;
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Piecewise-linear curve through midnight (0h), dawn (6h), midday (12h), sunset (18h), wrapping to midnight at 24h.
inline float SampleSkyScalarKeyframeCurve(float timeOfDayHours,
                                          float vMidnight,
                                          float vDawn,
                                          float vMidday,
                                          float vSunset)
{
    constexpr float kMidnightH = 0.0f;
    constexpr float kDawnH = 6.0f;
    constexpr float kMiddayH = 12.0f;
    constexpr float kSunsetH = 18.0f;
    constexpr float kDayHours = 24.0f;

    float t = std::fmod(timeOfDayHours, kDayHours);
    if (t < 0.0f)
        t += kDayHours;

    auto lerp = [](float a, float b, float u) { return a + (b - a) * u; };

    if (t < kDawnH)
        return lerp(vMidnight, vDawn, (t - kMidnightH) / (kDawnH - kMidnightH));
    if (t < kMiddayH)
        return lerp(vDawn, vMidday, (t - kDawnH) / (kMiddayH - kDawnH));
    if (t < kSunsetH)
        return lerp(vMidday, vSunset, (t - kMiddayH) / (kSunsetH - kMiddayH));
    return lerp(vSunset, vMidnight, (t - kSunsetH) / (kDayHours - kSunsetH));
}

inline float SampleSkyScalarKeyframeCurve(float timeOfDayHours,
                                          const float (&keyTimesHours)[4],
                                          float vMidnight,
                                          float vDawn,
                                          float vMidday,
                                          float vSunset)
{
    constexpr float kDayHours = 24.0f;
    constexpr float kEpsilon = 1e-4f;

    float t = std::fmod(timeOfDayHours, kDayHours);
    if (t < 0.0f)
        t += kDayHours;

    const float values[4] = {vMidnight, vDawn, vMidday, vSunset};
    float keyTimes[4] = {
        std::clamp(keyTimesHours[0], 0.0f, kDayHours - kEpsilon),
        std::clamp(keyTimesHours[1], 0.0f, kDayHours - kEpsilon),
        std::clamp(keyTimesHours[2], 0.0f, kDayHours - kEpsilon),
        std::clamp(keyTimesHours[3], 0.0f, kDayHours - kEpsilon),
    };

    // Keep a strict ordering so interpolation segments remain well-defined.
    keyTimes[1] = std::max(keyTimes[1], keyTimes[0] + kEpsilon);
    keyTimes[2] = std::max(keyTimes[2], keyTimes[1] + kEpsilon);
    keyTimes[3] = std::max(keyTimes[3], keyTimes[2] + kEpsilon);
    keyTimes[3] = std::min(keyTimes[3], kDayHours - kEpsilon);

    auto lerp = [](float a, float b, float u) { return a + (b - a) * u; };

    for (int i = 0; i < 3; ++i)
    {
        const float t0 = keyTimes[i];
        const float t1 = keyTimes[i + 1];
        if (t >= t0 && t < t1)
        {
            const float denom = std::max(kEpsilon, t1 - t0);
            const float u = (t - t0) / denom;
            return lerp(values[i], values[i + 1], u);
        }
    }

    const float wrapStart = keyTimes[3];
    const float wrapEnd = keyTimes[0] + kDayHours;
    const float wrappedT = (t < keyTimes[0]) ? (t + kDayHours) : t;
    const float wrapDenom = std::max(kEpsilon, wrapEnd - wrapStart);
    const float u = std::clamp((wrappedT - wrapStart) / wrapDenom, 0.0f, 1.0f);
    return lerp(values[3], values[0], u);
}

inline void SampleSkyVec3KeyframeCurve(float timeOfDayHours,
                                       const float (&km)[3],
                                       const float (&kd)[3],
                                       const float (&kn)[3],
                                       const float (&ks)[3],
                                       float out[3])
{
    out[0] = SampleSkyScalarKeyframeCurve(timeOfDayHours, km[0], kd[0], kn[0], ks[0]);
    out[1] = SampleSkyScalarKeyframeCurve(timeOfDayHours, km[1], kd[1], kn[1], ks[1]);
    out[2] = SampleSkyScalarKeyframeCurve(timeOfDayHours, km[2], kd[2], kn[2], ks[2]);
}

inline void SampleSkyVec3KeyframeCurve(float timeOfDayHours,
                                       const float (&keyTimesHours)[4],
                                       const float (&km)[3],
                                       const float (&kd)[3],
                                       const float (&kn)[3],
                                       const float (&ks)[3],
                                       float out[3])
{
    out[0] = SampleSkyScalarKeyframeCurve(timeOfDayHours, keyTimesHours, km[0], kd[0], kn[0], ks[0]);
    out[1] = SampleSkyScalarKeyframeCurve(timeOfDayHours, keyTimesHours, km[1], kd[1], kn[1], ks[1]);
    out[2] = SampleSkyScalarKeyframeCurve(timeOfDayHours, keyTimesHours, km[2], kd[2], kn[2], ks[2]);
}

/// Places the sun and the moon for the hour in `settings.timeOfDayHours`.
///
/// The sun rides its daily circle in `solarFrame` (see SolarPath.h):
///   sun(t) = SolarDirection(frame, cos(theta), sin(theta)),  theta = (t/24 - 0.25) * 2pi
/// so 06:00 is the eastern end of the celestial equator, 12:00 is solar noon and 18:00 the western
/// end. At latitude 0 on the equinox with north 0 the frame is exactly east = +X, meridian = +Y,
/// declination 0, and this is the great circle through the zenith.
///
/// The moon rides the SAME circle (the sun's declination), offset in hour angle by
/// 2pi * moonArcPosition; the visual phase mask is separate. It has no declination of its own.
// The handover band's spans in the sun's up component, below and above sunBelowHorizonDotThreshold.
inline constexpr float kMoonBlendSpanBelow = 0.12f;
inline constexpr float kMoonBlendSpanAbove = 0.07f;

// The up component of the sun's direction at and above which the primary light is the sun's alone:
// the handover band's upper edge, 0.02 (1.15 degrees) with the default config.
inline float SunOnlyUpDot(const SkySystemConfig& config)
{
    return config.sunBelowHorizonDotThreshold + kMoonBlendSpanAbove;
}

// How far the primary light has handed over from the sun to the moon, for a sun whose direction has
// `sunUpDot` as its up component: 0 while the sun is well up, 1 once it is below the twilight floor
// (the band's lower edge), smooth between. Always 0 when the config keeps the sun primary.
inline float PrimaryMoonBlend(const SkySystemConfig& config, float sunUpDot)
{
    if (!config.useMoonWhenSunBelowHorizon)
        return 0.0f;
    const float edgeMoonFull = config.sunBelowHorizonDotThreshold - kMoonBlendSpanBelow;
    return 1.0f - Smoothstep01(edgeMoonFull, SunOnlyUpDot(config), sunUpDot);
}

// The default moon's illuminance as a fraction of a clear sun's: the stylized
// moonLightIlluminanceScale, or 0 when the moon is hidden, so a hidden moon does not keep lighting
// the scene. The sky scales it to its Moonlight and to the light it drives.
inline float LinkedLightMoonScale(const SkySystemConfig& config, bool showMoon)
{
    return showMoon ? config.moonLightIlluminanceScale : 0.0f;
}

inline void ComputeSimpleSunMoon(const SkySettings& settings,
                                 const SkySystemConfig& config,
                                 const SolarFrame& solarFrame,
                                 SkySystemState& outState)
{
    constexpr float kPi = 3.14159265f;
    constexpr float kTwoPi = 2.0f * kPi;

    // Wrap time-of-day to [0, 1).
    float t = settings.timeOfDayHours / 24.0f;
    t = t - std::floor(t);

    float theta = (t - 0.25f) * kTwoPi;
    SolarDirection(solarFrame, std::cos(theta), std::sin(theta), outState.sunDirWS);

    float cycle = settings.moonArcPosition;
    cycle = cycle - std::floor(cycle);
    const float moonTheta = theta + cycle * kTwoPi;
    SolarDirection(solarFrame, std::cos(moonTheta), std::sin(moonTheta), outState.moonDirWS);

    outState.primaryMoonBlend = PrimaryMoonBlend(config, outState.sunDirWS[1]);
}

// Helper that updates SkySettings primary sun direction/color/intensity based on the computed state.
// The caller can then use primarySunDir both for the sky shaders and to drive the main directional light.
//
// `sunTint` scales the SUN's share of the source colour only; the moon's share stays
// `config.moonColor` untouched. The asymmetry is the point rather than an oversight: the tint is art
// direction for the sun, and the moon DISC is drawn straight from config.moonColor with no tint
// applied, so tinting the moon's light too would hang an orange dome over a grey moon. Pass white
// for no tint.
//
// `moonSourceIntensity` is the moonlight's own source on the same scale as primarySunIntensity, the
// moonlight's magnitude before moonIntensityScale: the night's source follows it rather than the
// sun's, so dimming the sun leaves the night sky where it is.
inline void ApplySimpleLightingToSkySettings(const SkySystemState& state,
                                             const SkySystemConfig& config,
                                             const float sunTint[3],
                                             float moonSourceIntensity,
                                             SkySettings& ioSky)
{
    const float kSunIntensity = ioSky.primarySunIntensity > 0.0f ? ioSky.primarySunIntensity : 40.0f;

    const float w = config.useMoonWhenSunBelowHorizon ? state.primaryMoonBlend : 0.0f;

    float blendedDir[3] = {
        state.sunDirWS[0] * (1.0f - w) + state.moonDirWS[0] * w,
        state.sunDirWS[1] * (1.0f - w) + state.moonDirWS[1] * w,
        state.sunDirWS[2] * (1.0f - w) + state.moonDirWS[2] * w,
    };
    // A moon opposite the sun — the default arc position — makes the midpoint of this lerp the
    // zero vector, and Normalize3's fallback would aim the primary light straight down from the
    // zenith for those frames. Snap to whichever body the blend already favours instead.
    constexpr float kDegenerateBlendLengthSq = 1e-4f;
    const float blendedLengthSq = blendedDir[0] * blendedDir[0] + blendedDir[1] * blendedDir[1] +
                                  blendedDir[2] * blendedDir[2];
    if (blendedLengthSq < kDegenerateBlendLengthSq)
    {
        const float* dominant = (w < 0.5f) ? state.sunDirWS : state.moonDirWS;
        blendedDir[0] = dominant[0];
        blendedDir[1] = dominant[1];
        blendedDir[2] = dominant[2];
    }
    Normalize3(blendedDir);
    ioSky.primarySunDir[0] = blendedDir[0];
    ioSky.primarySunDir[1] = blendedDir[1];
    ioSky.primarySunDir[2] = blendedDir[2];

    const float moonIntensity = moonSourceIntensity * config.moonIntensityScale;
    ioSky.primarySunIntensity = kSunIntensity * (1.0f - w) + moonIntensity * w;

    ioSky.primarySunColor[0] = (1.0f - w) * sunTint[0] + w * config.moonColor[0];
    ioSky.primarySunColor[1] = (1.0f - w) * sunTint[1] + w * config.moonColor[1];
    ioSky.primarySunColor[2] = (1.0f - w) * sunTint[2] + w * config.moonColor[2];
}

} // namespace Rendering
} // namespace GameEngine
