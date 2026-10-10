#include "Components/Rendering/PostProcessEffects/HeightFogEffectPresets.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Components {
namespace {

float LerpFloat(float a, float b, float t)
{
    return a + (b - a) * t;
}

void LerpFloat3(const float* a, const float* b, float t, float* out)
{
    out[0] = LerpFloat(a[0], b[0], t);
    out[1] = LerpFloat(a[1], b[1], t);
    out[2] = LerpFloat(a[2], b[2], t);
}

float SampleDayScalar(float timeOfDayHours, const float dayKeyTimesHours[4],
                      float midnight, float dawn, float midday, float sunset)
{
    constexpr float kDayHours = 24.0f;
    constexpr float kEpsilon = 1e-4f;

    float t = std::fmod(timeOfDayHours, kDayHours);
    if (t < 0.0f)
        t += kDayHours;

    const float values[4] = {midnight, dawn, midday, sunset};
    const float keys[4] = {
        dayKeyTimesHours ? dayKeyTimesHours[0] : 0.0f,
        dayKeyTimesHours ? dayKeyTimesHours[1] : 6.0f,
        dayKeyTimesHours ? dayKeyTimesHours[2] : 12.0f,
        dayKeyTimesHours ? dayKeyTimesHours[3] : 18.0f,
    };
    float keyTimes[4] = {
        std::clamp(keys[0], 0.0f, kDayHours - kEpsilon),
        std::clamp(keys[1], 0.0f, kDayHours - kEpsilon),
        std::clamp(keys[2], 0.0f, kDayHours - kEpsilon),
        std::clamp(keys[3], 0.0f, kDayHours - kEpsilon),
    };
    keyTimes[1] = std::max(keyTimes[1], keyTimes[0] + kEpsilon);
    keyTimes[2] = std::max(keyTimes[2], keyTimes[1] + kEpsilon);
    keyTimes[3] = std::max(keyTimes[3], keyTimes[2] + kEpsilon);
    keyTimes[3] = std::min(keyTimes[3], kDayHours - kEpsilon);

    for (int i = 0; i < 3; ++i)
    {
        const float t0 = keyTimes[i];
        const float t1 = keyTimes[i + 1];
        if (t >= t0 && t < t1)
        {
            const float u = (t - t0) / std::max(kEpsilon, t1 - t0);
            return LerpFloat(values[i], values[i + 1], u);
        }
    }

    const float wrapStart = keyTimes[3];
    const float wrapEnd = keyTimes[0] + kDayHours;
    const float wrappedT = (t < keyTimes[0]) ? (t + kDayHours) : t;
    const float u = std::clamp((wrappedT - wrapStart) / std::max(kEpsilon, wrapEnd - wrapStart), 0.0f, 1.0f);
    return LerpFloat(values[3], values[0], u);
}

void SampleDayFloat3(float timeOfDayHours, const float dayKeyTimesHours[4],
                     const float* midnight, const float* dawn, const float* midday, const float* sunset,
                     float* out)
{
    out[0] = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, midnight[0], dawn[0], midday[0], sunset[0]);
    out[1] = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, midnight[1], dawn[1], midday[1], sunset[1]);
    out[2] = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, midnight[2], dawn[2], midday[2], sunset[2]);
}

} // namespace

void ApplyHeightFogPreset(HeightFogEffect& effect, HeightFogPreset preset)
{
    if (preset == HeightFogPreset::Custom)
    {
        effect.Preset = HeightFogPreset::Custom;
        return;
    }

    effect = HeightFogEffect{};
    effect.Preset = preset;
    effect.Enabled = true;
    effect.TrackDirectionalLight = true;
    effect.DistanceFogEnabled = true;
    effect.HeightFogEnabled = true;
    effect.UseTimeOfDay = false;

    switch (preset)
    {
    case HeightFogPreset::MorningHaze:
        effect.Intensity = 0.78f;
        effect.Density = 0.34f;
        effect.MaxOpacity = 0.58f;
        effect.MinDistance = 8.0f;
        effect.SmoothLength = 95.0f;
        effect.MaxDistance = 900.0f;
        effect.LayerMode = HeightFogLayerMode::Dominant;
        effect.BaseHeight = 4.0f;
        effect.TransitionLength = 145.0f;
        effect.HorizonHeightOffset = 18.0f;
        effect.HorizonHeightBlendStart = 360.0f;
        effect.HorizonHeightBlendEnd = 900.0f;
        effect.Emissive[0] = 0.58f;
        effect.Emissive[1] = 0.61f;
        effect.Emissive[2] = 0.66f;
        effect.GradientMode = HeightFogGradientMode::MainLight;
        effect.GradientStrength = 0.42f;
        effect.GradientLowColor[0] = 0.50f;
        effect.GradientLowColor[1] = 0.55f;
        effect.GradientLowColor[2] = 0.62f;
        effect.GradientHighColor[0] = 0.92f;
        effect.GradientHighColor[1] = 0.78f;
        effect.GradientHighColor[2] = 0.55f;
        effect.SunColor[0] = 1.0f;
        effect.SunColor[1] = 0.78f;
        effect.SunColor[2] = 0.48f;
        effect.SunIntensity = 1.1f;
        effect.SunIntensityScale = 0.85f;
        effect.Phase = -0.25f;
        effect.NoiseEnabled = true;
        effect.NoiseScale = 140.0f;
        effect.NoiseStrength = 0.12f;
        effect.NoiseContrast = 1.1f;
        effect.NoiseMin = 0.18f;
        effect.NoiseMax = 0.92f;
        effect.NoiseFadeStart = 280.0f;
        effect.NoiseFadeEnd = 760.0f;
        effect.SkyEnabled = true;
        effect.SkyPower = 1.15f;
        effect.SkyFillStart = 0.0f;
        effect.SkyFillEnd = 0.78f;
        effect.SkyHorizonOffset = 0.04f;
        break;
    case HeightFogPreset::GroundMist:
        effect.Intensity = 0.88f;
        effect.Density = 0.58f;
        effect.MaxOpacity = 0.70f;
        effect.MinDistance = 0.0f;
        effect.SmoothLength = 45.0f;
        effect.MaxDistance = 360.0f;
        effect.LayerMode = HeightFogLayerMode::Additive;
        effect.BaseHeight = -2.0f;
        effect.TransitionLength = 42.0f;
        effect.Emissive[0] = 0.62f;
        effect.Emissive[1] = 0.67f;
        effect.Emissive[2] = 0.72f;
        effect.GradientMode = HeightFogGradientMode::Height;
        effect.GradientStrength = 0.62f;
        effect.GradientLowColor[0] = 0.72f;
        effect.GradientLowColor[1] = 0.76f;
        effect.GradientLowColor[2] = 0.80f;
        effect.GradientHighColor[0] = 0.38f;
        effect.GradientHighColor[1] = 0.46f;
        effect.GradientHighColor[2] = 0.55f;
        effect.SunIntensity = 0.35f;
        effect.SunIntensityScale = 0.45f;
        effect.Phase = -0.15f;
        effect.NoiseEnabled = true;
        effect.NoiseScale = 70.0f;
        effect.NoiseStrength = 0.30f;
        effect.NoiseContrast = 1.45f;
        effect.NoiseMin = 0.12f;
        effect.NoiseMax = 0.82f;
        effect.NoiseFadeStart = 120.0f;
        effect.NoiseFadeEnd = 340.0f;
        effect.NoiseVelocity[0] = 0.012f;
        effect.NoiseVelocity[1] = 0.025f;
        effect.NoiseVelocity[2] = 0.0f;
        effect.SkyEnabled = true;
        effect.SkyFillStart = 0.0f;
        effect.SkyFillEnd = 0.48f;
        effect.SkyBottomStrength = 0.08f;
        break;
    case HeightFogPreset::MountainValley:
        effect.Intensity = 0.70f;
        effect.Density = 0.30f;
        effect.MaxOpacity = 0.55f;
        effect.MinDistance = 25.0f;
        effect.SmoothLength = 180.0f;
        effect.MaxDistance = 1600.0f;
        effect.LayerMode = HeightFogLayerMode::Dominant;
        effect.BaseHeight = 40.0f;
        effect.TransitionLength = 220.0f;
        effect.HorizonHeightOffset = 80.0f;
        effect.HorizonHeightBlendStart = 700.0f;
        effect.HorizonHeightBlendEnd = 1600.0f;
        effect.Emissive[0] = 0.42f;
        effect.Emissive[1] = 0.50f;
        effect.Emissive[2] = 0.58f;
        effect.GradientMode = HeightFogGradientMode::Distance;
        effect.GradientStrength = 0.38f;
        effect.GradientLowColor[0] = 0.36f;
        effect.GradientLowColor[1] = 0.44f;
        effect.GradientLowColor[2] = 0.52f;
        effect.GradientHighColor[0] = 0.70f;
        effect.GradientHighColor[1] = 0.77f;
        effect.GradientHighColor[2] = 0.84f;
        effect.SunColor[0] = 0.92f;
        effect.SunColor[1] = 0.86f;
        effect.SunColor[2] = 0.72f;
        effect.SunIntensityScale = 0.75f;
        effect.Phase = -0.40f;
        effect.SkyEnabled = true;
        effect.SkyPower = 1.45f;
        effect.SkyFillStart = 0.05f;
        effect.SkyFillEnd = 0.88f;
        effect.SkyHorizonOffset = 0.03f;
        break;
    case HeightFogPreset::HorizonPollution:
        effect.Intensity = 0.72f;
        effect.Density = 0.40f;
        effect.MaxOpacity = 0.60f;
        effect.MinDistance = 12.0f;
        effect.SmoothLength = 140.0f;
        effect.MaxDistance = 1100.0f;
        effect.LayerMode = HeightFogLayerMode::Additive;
        effect.BaseHeight = 0.0f;
        effect.TransitionLength = 150.0f;
        effect.HorizonHeightOffset = 35.0f;
        effect.HorizonHeightBlendStart = 420.0f;
        effect.HorizonHeightBlendEnd = 1100.0f;
        effect.Emissive[0] = 0.50f;
        effect.Emissive[1] = 0.47f;
        effect.Emissive[2] = 0.42f;
        effect.GradientMode = HeightFogGradientMode::MainLight;
        effect.GradientStrength = 0.70f;
        effect.GradientLowColor[0] = 0.42f;
        effect.GradientLowColor[1] = 0.39f;
        effect.GradientLowColor[2] = 0.36f;
        effect.GradientHighColor[0] = 0.95f;
        effect.GradientHighColor[1] = 0.62f;
        effect.GradientHighColor[2] = 0.38f;
        effect.SunColor[0] = 1.0f;
        effect.SunColor[1] = 0.55f;
        effect.SunColor[2] = 0.32f;
        effect.SunIntensity = 0.75f;
        effect.SunIntensityScale = 0.65f;
        effect.Phase = 0.18f;
        effect.SkyEnabled = true;
        effect.SkyPower = 0.95f;
        effect.SkyFillStart = 0.0f;
        effect.SkyFillEnd = 0.68f;
        effect.SkyHorizonOffset = 0.06f;
        break;
    case HeightFogPreset::Day:
        effect.Intensity = 0.52f;
        effect.Density = 0.20f;
        effect.MaxOpacity = 0.38f;
        effect.MinDistance = 20.0f;
        effect.SmoothLength = 190.0f;
        effect.MaxDistance = 1400.0f;
        effect.LayerMode = HeightFogLayerMode::Dominant;
        effect.BaseHeight = 8.0f;
        effect.TransitionLength = 180.0f;
        effect.HorizonHeightOffset = 25.0f;
        effect.HorizonHeightBlendStart = 650.0f;
        effect.HorizonHeightBlendEnd = 1400.0f;
        effect.Emissive[0] = 0.50f;
        effect.Emissive[1] = 0.58f;
        effect.Emissive[2] = 0.68f;
        effect.GradientMode = HeightFogGradientMode::Distance;
        effect.GradientStrength = 0.18f;
        effect.GradientLowColor[0] = 0.48f;
        effect.GradientLowColor[1] = 0.56f;
        effect.GradientLowColor[2] = 0.66f;
        effect.GradientHighColor[0] = 0.72f;
        effect.GradientHighColor[1] = 0.80f;
        effect.GradientHighColor[2] = 0.92f;
        effect.SunColor[0] = 1.0f;
        effect.SunColor[1] = 0.88f;
        effect.SunColor[2] = 0.62f;
        effect.SunIntensity = 0.85f;
        effect.SunIntensityScale = 0.55f;
        effect.Phase = -0.32f;
        effect.SkyEnabled = true;
        effect.SkyPower = 0.75f;
        effect.SkyFillStart = 0.05f;
        effect.SkyFillEnd = 0.82f;
        effect.SkyHorizonOffset = 0.02f;
        break;
    case HeightFogPreset::Night:
        effect.Intensity = 0.50f;
        effect.Density = 0.24f;
        effect.MaxOpacity = 0.48f;
        effect.MinDistance = 10.0f;
        effect.SmoothLength = 130.0f;
        effect.MaxDistance = 900.0f;
        effect.LayerMode = HeightFogLayerMode::Dominant;
        effect.BaseHeight = 0.0f;
        effect.TransitionLength = 120.0f;
        effect.Emissive[0] = 0.12f;
        effect.Emissive[1] = 0.16f;
        effect.Emissive[2] = 0.24f;
        effect.GradientMode = HeightFogGradientMode::Height;
        effect.GradientStrength = 0.35f;
        effect.GradientLowColor[0] = 0.08f;
        effect.GradientLowColor[1] = 0.12f;
        effect.GradientLowColor[2] = 0.20f;
        effect.GradientHighColor[0] = 0.18f;
        effect.GradientHighColor[1] = 0.23f;
        effect.GradientHighColor[2] = 0.34f;
        effect.SunColor[0] = 0.35f;
        effect.SunColor[1] = 0.42f;
        effect.SunColor[2] = 0.58f;
        effect.SunIntensity = 0.20f;
        effect.SunIntensityScale = 0.35f;
        effect.Phase = -0.2f;
        effect.NoiseEnabled = true;
        effect.NoiseScale = 180.0f;
        effect.NoiseStrength = 0.08f;
        effect.NoiseContrast = 1.05f;
        effect.NoiseMin = 0.10f;
        effect.NoiseMax = 0.95f;
        effect.NoiseFadeStart = 300.0f;
        effect.NoiseFadeEnd = 850.0f;
        effect.SkyEnabled = true;
        effect.SkyPower = 1.0f;
        effect.SkyFillStart = 0.0f;
        effect.SkyFillEnd = 0.65f;
        effect.SkyBottomStrength = 0.05f;
        break;
    default:
        break;
    }
}

float ComputeHeightFogNightBlend(float timeOfDayHours)
{
    constexpr float kDefaultDayKeyTimes[4] = {0.0f, 6.0f, 12.0f, 18.0f};
    return SampleDayScalar(timeOfDayHours, kDefaultDayKeyTimes, 1.0f, 0.25f, 0.0f, 0.30f);
}

void LerpHeightFogEffect(const HeightFogEffect& a, const HeightFogEffect& b, float t, HeightFogEffect& out)
{
    const float u = std::clamp(t, 0.0f, 1.0f);
    out = a;
    out.Intensity = LerpFloat(a.Intensity, b.Intensity, u);
    out.Density = LerpFloat(a.Density, b.Density, u);
    out.MaxOpacity = LerpFloat(a.MaxOpacity, b.MaxOpacity, u);
    out.MinDistance = LerpFloat(a.MinDistance, b.MinDistance, u);
    out.SmoothLength = LerpFloat(a.SmoothLength, b.SmoothLength, u);
    out.MaxDistance = LerpFloat(a.MaxDistance, b.MaxDistance, u);
    out.HorizonHeightOffset = LerpFloat(a.HorizonHeightOffset, b.HorizonHeightOffset, u);
    out.HorizonHeightBlendStart = LerpFloat(a.HorizonHeightBlendStart, b.HorizonHeightBlendStart, u);
    out.HorizonHeightBlendEnd = LerpFloat(a.HorizonHeightBlendEnd, b.HorizonHeightBlendEnd, u);
    out.BaseHeight = LerpFloat(a.BaseHeight, b.BaseHeight, u);
    out.TransitionLength = LerpFloat(a.TransitionLength, b.TransitionLength, u);
    LerpFloat3(a.Emissive, b.Emissive, u, out.Emissive);
    out.GradientStrength = LerpFloat(a.GradientStrength, b.GradientStrength, u);
    LerpFloat3(a.GradientLowColor, b.GradientLowColor, u, out.GradientLowColor);
    LerpFloat3(a.GradientHighColor, b.GradientHighColor, u, out.GradientHighColor);
    LerpFloat3(a.SunDirection, b.SunDirection, u, out.SunDirection);
    LerpFloat3(a.SunColor, b.SunColor, u, out.SunColor);
    out.SunIntensity = LerpFloat(a.SunIntensity, b.SunIntensity, u);
    out.SunIntensityScale = LerpFloat(a.SunIntensityScale, b.SunIntensityScale, u);
    out.Phase = LerpFloat(a.Phase, b.Phase, u);
    out.PhaseWeight0 = LerpFloat(a.PhaseWeight0, b.PhaseWeight0, u);
    out.PhaseWeight1 = LerpFloat(a.PhaseWeight1, b.PhaseWeight1, u);
    out.NoiseScale = LerpFloat(a.NoiseScale, b.NoiseScale, u);
    out.NoiseStrength = LerpFloat(a.NoiseStrength, b.NoiseStrength, u);
    LerpFloat3(a.NoiseVelocity, b.NoiseVelocity, u, out.NoiseVelocity);
    out.NoiseContrast = LerpFloat(a.NoiseContrast, b.NoiseContrast, u);
    out.NoiseMin = LerpFloat(a.NoiseMin, b.NoiseMin, u);
    out.NoiseMax = LerpFloat(a.NoiseMax, b.NoiseMax, u);
    out.NoiseFadeStart = LerpFloat(a.NoiseFadeStart, b.NoiseFadeStart, u);
    out.NoiseFadeEnd = LerpFloat(a.NoiseFadeEnd, b.NoiseFadeEnd, u);
    out.SkyPower = LerpFloat(a.SkyPower, b.SkyPower, u);
    out.SkyFillStart = LerpFloat(a.SkyFillStart, b.SkyFillStart, u);
    out.SkyFillEnd = LerpFloat(a.SkyFillEnd, b.SkyFillEnd, u);
    out.SkyHorizonOffset = LerpFloat(a.SkyHorizonOffset, b.SkyHorizonOffset, u);
    out.SkyBottomStrength = LerpFloat(a.SkyBottomStrength, b.SkyBottomStrength, u);

    if (u >= 0.5f)
    {
        out.DistanceFogEnabled = b.DistanceFogEnabled;
        out.HeightFogEnabled = b.HeightFogEnabled;
        out.LayerMode = b.LayerMode;
        out.AxisMode = b.AxisMode;
        out.GradientMode = b.GradientMode;
        out.TrackDirectionalLight = b.TrackDirectionalLight;
        out.NoiseEnabled = b.NoiseEnabled;
        out.SkyEnabled = b.SkyEnabled;
    }
    out.Preset = HeightFogPreset::Custom;
}

void ApplyHeightFogTimeOfDay(HeightFogEffect& effect, float timeOfDayHours)
{
    constexpr float kDefaultDayKeyTimes[4] = {0.0f, 6.0f, 12.0f, 18.0f};
    ApplyHeightFogTimeOfDay(effect, timeOfDayHours, kDefaultDayKeyTimes);
}

void ApplyHeightFogTimeOfDay(HeightFogEffect& effect, float timeOfDayHours, const float dayKeyTimesHours[4])
{
    HeightFogEffect day{};
    HeightFogEffect night{};
    HeightFogEffect dawn{};
    HeightFogEffect sunset{};
    ApplyHeightFogPreset(day, HeightFogPreset::Day);
    ApplyHeightFogPreset(night, HeightFogPreset::Night);
    ApplyHeightFogPreset(dawn, HeightFogPreset::MorningHaze);
    ApplyHeightFogPreset(sunset, HeightFogPreset::HorizonPollution);

    effect = day;
    effect.Intensity = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.Intensity, dawn.Intensity, day.Intensity, sunset.Intensity);
    effect.Density = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.Density, dawn.Density, day.Density, sunset.Density);
    effect.MaxOpacity = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.MaxOpacity, dawn.MaxOpacity, day.MaxOpacity, sunset.MaxOpacity);
    effect.MinDistance = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.MinDistance, dawn.MinDistance, day.MinDistance, sunset.MinDistance);
    effect.SmoothLength = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.SmoothLength, dawn.SmoothLength, day.SmoothLength, sunset.SmoothLength);
    effect.MaxDistance = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.MaxDistance, dawn.MaxDistance, day.MaxDistance, sunset.MaxDistance);
    effect.HorizonHeightOffset = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.HorizonHeightOffset, dawn.HorizonHeightOffset, day.HorizonHeightOffset, sunset.HorizonHeightOffset);
    effect.HorizonHeightBlendStart = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.HorizonHeightBlendStart, dawn.HorizonHeightBlendStart, day.HorizonHeightBlendStart, sunset.HorizonHeightBlendStart);
    effect.HorizonHeightBlendEnd = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.HorizonHeightBlendEnd, dawn.HorizonHeightBlendEnd, day.HorizonHeightBlendEnd, sunset.HorizonHeightBlendEnd);
    effect.BaseHeight = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.BaseHeight, dawn.BaseHeight, day.BaseHeight, sunset.BaseHeight);
    effect.TransitionLength = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.TransitionLength, dawn.TransitionLength, day.TransitionLength, sunset.TransitionLength);
    SampleDayFloat3(timeOfDayHours, dayKeyTimesHours, night.Emissive, dawn.Emissive, day.Emissive, sunset.Emissive, effect.Emissive);
    effect.GradientMode = HeightFogGradientMode::MainLight;
    effect.GradientStrength = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.GradientStrength, dawn.GradientStrength, day.GradientStrength, sunset.GradientStrength);
    SampleDayFloat3(timeOfDayHours, dayKeyTimesHours, night.GradientLowColor, dawn.GradientLowColor, day.GradientLowColor, sunset.GradientLowColor, effect.GradientLowColor);
    SampleDayFloat3(timeOfDayHours, dayKeyTimesHours, night.GradientHighColor, dawn.GradientHighColor, day.GradientHighColor, sunset.GradientHighColor, effect.GradientHighColor);
    SampleDayFloat3(timeOfDayHours, dayKeyTimesHours, night.SunDirection, dawn.SunDirection, day.SunDirection, sunset.SunDirection, effect.SunDirection);
    SampleDayFloat3(timeOfDayHours, dayKeyTimesHours, night.SunColor, dawn.SunColor, day.SunColor, sunset.SunColor, effect.SunColor);
    effect.SunIntensity = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.SunIntensity, dawn.SunIntensity, day.SunIntensity, sunset.SunIntensity);
    effect.SunIntensityScale = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.SunIntensityScale, dawn.SunIntensityScale, day.SunIntensityScale, sunset.SunIntensityScale);
    effect.Phase = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.Phase, dawn.Phase, day.Phase, sunset.Phase);
    effect.PhaseWeight0 = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.PhaseWeight0, dawn.PhaseWeight0, day.PhaseWeight0, sunset.PhaseWeight0);
    effect.PhaseWeight1 = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.PhaseWeight1, dawn.PhaseWeight1, day.PhaseWeight1, sunset.PhaseWeight1);
    effect.NoiseScale = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.NoiseScale, dawn.NoiseScale, day.NoiseScale, sunset.NoiseScale);
    effect.NoiseStrength = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.NoiseStrength, dawn.NoiseStrength, day.NoiseStrength, sunset.NoiseStrength);
    SampleDayFloat3(timeOfDayHours, dayKeyTimesHours, night.NoiseVelocity, dawn.NoiseVelocity, day.NoiseVelocity, sunset.NoiseVelocity, effect.NoiseVelocity);
    effect.NoiseContrast = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.NoiseContrast, dawn.NoiseContrast, day.NoiseContrast, sunset.NoiseContrast);
    effect.NoiseMin = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.NoiseMin, dawn.NoiseMin, day.NoiseMin, sunset.NoiseMin);
    effect.NoiseMax = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.NoiseMax, dawn.NoiseMax, day.NoiseMax, sunset.NoiseMax);
    effect.NoiseFadeStart = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.NoiseFadeStart, dawn.NoiseFadeStart, day.NoiseFadeStart, sunset.NoiseFadeStart);
    effect.NoiseFadeEnd = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.NoiseFadeEnd, dawn.NoiseFadeEnd, day.NoiseFadeEnd, sunset.NoiseFadeEnd);
    effect.SkyPower = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.SkyPower, dawn.SkyPower, day.SkyPower, sunset.SkyPower);
    effect.SkyFillStart = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.SkyFillStart, dawn.SkyFillStart, day.SkyFillStart, sunset.SkyFillStart);
    effect.SkyFillEnd = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.SkyFillEnd, dawn.SkyFillEnd, day.SkyFillEnd, sunset.SkyFillEnd);
    effect.SkyHorizonOffset = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.SkyHorizonOffset, dawn.SkyHorizonOffset, day.SkyHorizonOffset, sunset.SkyHorizonOffset);
    effect.SkyBottomStrength = SampleDayScalar(timeOfDayHours, dayKeyTimesHours, night.SkyBottomStrength, dawn.SkyBottomStrength, day.SkyBottomStrength, sunset.SkyBottomStrength);
    effect.DistanceFogEnabled = true;
    effect.HeightFogEnabled = true;
    effect.LayerMode = HeightFogLayerMode::Dominant;
    effect.TrackDirectionalLight = true;
    effect.NoiseEnabled = effect.NoiseStrength > 0.01f;
    effect.SkyEnabled = true;
    effect.UseTimeOfDay = true;
    effect.Preset = HeightFogPreset::Custom;
}

} // namespace GameEngine::Components
