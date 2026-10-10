#pragma once

#include "Components/Rendering/LightPhotometry.h" // kReferenceWhiteNits — single exposure/light anchor
#include "Components/Rendering/PostProcessVolume.h" // ExposureMode

#include <algorithm>
#include <cmath>

namespace GameEngine::Rendering
{

// Absolute photographic EV100 that maps reference white (203 nits == scene-linear 1.0, the same
// LightPhotometry/emission anchor) to display white, i.e. exposureScale == 1.0 (today's look).
// Because physical lights are pre-divided by the 203 anchor, the engine's EV100 IS the real-world
// photographic scale: bright sun ~15, overcast ~12, interior ~7-9, night ~2-5.
inline const float kNeutralExposureEV = std::log2(Components::kReferenceWhiteNits);

// Auto exposure keys the metered average to 18 % grey: exposed (pre-tonemap) scene-linear 0.18, where
// an 18 % card lands under Manual/Physical exposure at the scene's photographic EV. Compensation (in
// stops) is the one lever that moves it.
inline constexpr float kAutoExposureMiddleGrey = 0.18f;

// Absolute EV100 -> the linear multiplier the tonemap applies. EV100 == kNeutralExposureEV -> 1.0
// (so a brighter scene -> higher EV -> smaller multiplier, the photographic convention).
inline float EvToLinearExposure(float ev100)
{
    return std::exp2(kNeutralExposureEV - ev100);
}

// The inverse: a linear exposure multiplier -> the absolute EV100 it represents.
inline float LinearExposureToEv(float scale)
{
    return kNeutralExposureEV - std::log2(scale > 1e-8f ? scale : 1e-8f);
}

// Absolute photographic EV100 from physical camera settings (APEX): EV100 = log2(N^2 / t * 100/ISO).
// f/16, 1/100 s, ISO 100 (sunny-16) -> ~14.6. Inputs are floored to stay finite for degenerate values.
inline float PhysicalCameraEv100(float aperture, float shutterTime, float iso)
{
    const float n = aperture > 1.0e-3f ? aperture : 1.0e-3f;
    const float t = shutterTime > 1.0e-6f ? shutterTime : 1.0e-6f;
    const float s = iso > 1.0f ? iso : 1.0f;
    return std::log2((n * n) / t * (100.0f / s));
}

// Resolve an authored exposure into the single linear scene multiplier the GPU tonemap consumes.
// Fixed uses the legacy linear `linearExposure`; every other (EV-based) mode uses `ev100` — Manual
// passes its authored EV100, Physical passes PhysicalCameraEv100(). ExposureCompensation is a +/-
// stop trim in all modes (+ brightens). Fixed + Exposure 1.0 + compensation 0 resolves to EXACTLY
// 1.0, so the render graph's "exposure == 1.0" tonemap-skip fast path is preserved for default scenes.
inline float ResolveExposureScale(Components::ExposureMode mode, float linearExposure,
                                  float ev100, float compensationEv)
{
    const float base = (mode == Components::ExposureMode::Fixed)
                           ? linearExposure
                           : EvToLinearExposure(ev100);
    return base * std::exp2(compensationEv);
}

// The EV100 to feed ResolveExposureScale for a given mode (Physical computes from the camera;
// Manual/anything-else uses the authored EV; Fixed is unused but returns the authored EV harmlessly).
inline float ExposureEv100ForMode(Components::ExposureMode mode, float manualEv100,
                                  float aperture, float shutterTime, float iso)
{
    return (mode == Components::ExposureMode::Physical)
               ? PhysicalCameraEv100(aperture, shutterTime, iso)
               : manualEv100;
}

// Intersect an auto-exposure adaptation envelope with narrow-only clamp bounds (e.g. a volume
// ExposureAdjustmentEffect): a clamp can tighten the envelope, never widen it, and the range
// stays non-inverted. Shared by the engine resolve and the editor Scene View override so the
// preview cannot drift from the final render.
inline void NarrowAutoExposureEnvelope(float& minEv, float& maxEv, float clampMinEv, float clampMaxEv)
{
    minEv = std::max(minEv, clampMinEv);
    maxEv = std::min(maxEv, clampMaxEv);
    if (maxEv < minEv)
        maxEv = minEv;
}

} // namespace GameEngine::Rendering
