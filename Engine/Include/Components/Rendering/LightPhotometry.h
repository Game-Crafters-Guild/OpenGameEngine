#pragma once

#include "Components/Rendering/Light.h" // LightType, LightUnit

#include <algorithm>
#include <cmath>

// CPU-side photometric helpers for lights. The extraction system resolves a light's authored
// color temperature + physical-intensity unit into the engine's plain linear color / unitless
// intensity here, so the GPU path is unchanged (zero shader cost).
namespace GameEngine::Components
{

// Correlated color temperature (Kelvin) -> LINEAR RGB tint, normalized so the brightest channel
// is 1.0 (it tints hue, it does not change overall brightness). Tanner-Helland blackbody
// approximation (valid ~1000-40000K) producing sRGB, then converted to linear. 6500K -> ~white.
inline void KelvinToLinearRGB(float kelvin, float outLinear[3])
{
    const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;

    auto saturate255 = [](float v) { return std::clamp(v, 0.0f, 255.0f); };
    float r, g, b;
    if (t <= 66.0f)
    {
        r = 255.0f;
        g = saturate255(99.4708025861f * std::log(t) - 161.1195681661f);
    }
    else
    {
        r = saturate255(329.698727446f * std::pow(t - 60.0f, -0.1332047592f));
        g = saturate255(288.1221695283f * std::pow(t - 60.0f, -0.0755148492f));
    }
    if (t >= 66.0f)
        b = 255.0f;
    else if (t <= 19.0f)
        b = 0.0f;
    else
        b = saturate255(138.5177312231f * std::log(t - 10.0f) - 305.0447927307f);

    auto srgbToLinear = [](float c)
    {
        c /= 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    float lr = srgbToLinear(r), lg = srgbToLinear(g), lb = srgbToLinear(b);
    const float maxc = std::max(lr, std::max(lg, lb));
    const float inv = maxc > 1e-5f ? 1.0f / maxc : 1.0f; // normalize -> brightest channel == 1 (pure tint)
    outLinear[0] = lr * inv;
    outLinear[1] = lg * inv;
    outLinear[2] = lb * inv;
}

// Physical-intensity unit calibration anchor, shared by the forward + inverse conversions so they
// stay a matched pair. Lux (illuminance) and candela (luminous intensity) BOTH re-anchor to the
// single BT.2408 reference white of 203 nits — the same anchor as GE_EMISSION_PAPERWHITE_NITS in
// surface_io.glsl. This couples the INPUT SCALE of lights to emission, not the output radiance: a
// 203-nit emitter outputs scene-linear 1.0 directly (no BRDF), whereas a light of 203 lx (or 203 cd
// at 1 m) becomes unitless intensity 1.0, which the diffuse BRDF then carries through its 1/pi term,
// albedo, and NdotL — so a white normal-facing Lambertian under it reads ~1/pi, by design. The point
// is that "203 of either" is the same reference-white input, so authoring lights and emission on one
// scale is coherent (final look is set by exposure). kLightFourPi is the sphere solid angle
// (flux -> intensity geometry), not a calibration knob.
// BT.2408 reference white in nits — the single anchor both units re-scale to. Kept equal to the
// shader's GE_EMISSION_PAPERWHITE_NITS (surface_io.glsl); C++ can't include GLSL, so they are
// hand-synced by value (a tuning anchor, not an ABI constant).
inline constexpr float kReferenceWhiteNits = 203.0f;
inline constexpr float kUnitlessPerLux     = 1.0f / kReferenceWhiteNits;
inline constexpr float kUnitlessPerCandela = 1.0f / kReferenceWhiteNits;
inline constexpr float kLightFourPi        = 12.56637061f;

// Illuminance of the sun on a surface facing it at noon on a clear day, in lux: the default of every
// new directional light and of the sun the sky reads.
inline constexpr float kClearNoonSunIlluminanceLux = 100000.0f;

// Physical-intensity unit -> the engine's unitless intensity. Unitless passes through unchanged,
// so existing lights are unaffected.
inline float LightIntensityToUnitless(float intensity, LightUnit unit)
{
    switch (unit)
    {
    case LightUnit::Lux:     return intensity * kUnitlessPerLux;                       // illuminance (directional)
    case LightUnit::Candela: return intensity * kUnitlessPerCandela;                  // luminous intensity
    case LightUnit::Lumen:   return (intensity / kLightFourPi) * kUnitlessPerCandela; // flux -> candela (point/spot)
    case LightUnit::Unitless:
    default:                 return intensity;                                        // pass through
    }
}

// Inverse of LightIntensityToUnitless: a unitless intensity -> the value it would take in `unit`.
// Used to convert a light's Intensity when its unit changes so effective brightness is preserved.
inline float UnitlessToLightIntensity(float unitless, LightUnit unit)
{
    switch (unit)
    {
    case LightUnit::Lux:     return unitless / kUnitlessPerLux;
    case LightUnit::Candela: return unitless / kUnitlessPerCandela;
    case LightUnit::Lumen:   return (unitless / kUnitlessPerCandela) * kLightFourPi;
    case LightUnit::Unitless:
    default:                 return unitless;
    }
}

// Resolve a light's authored color/intensity into the plain values the GPU paths consume: Color
// tinted by color temperature (when enabled) + Intensity converted from its unit. Shared by every
// light consumer (world meshes, ocean, terrain) so they all render the same resolved light.
inline void ResolveLightColorIntensity(const Light& light, float outColor[3], float& outIntensity)
{
    float tint[3] = {1.0f, 1.0f, 1.0f};
    if (light.UseColorTemperature)
        KelvinToLinearRGB(light.ColorTemperature, tint);
    outColor[0] = light.Color[0] * tint[0];
    outColor[1] = light.Color[1] * tint[1];
    outColor[2] = light.Color[2] * tint[2];
    outIntensity = LightIntensityToUnitless(light.Intensity, light.IntensityUnit);
}

} // namespace GameEngine::Components
