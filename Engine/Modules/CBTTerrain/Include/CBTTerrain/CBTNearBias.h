#pragma once

// CBTNearBias.h — CPU-side view-priority near-bias radius curve for the CBT terrain
// renderer (clipmap/bisector-pool arc slice S2, Option C). Resolves the full-detail
// "near" disc radius and the max-coarsen "far" radius from the camera's altitude above
// the surface. This is a CPU-only tuning POLICY, not a GPU memory-layout mirror (that is
// CBTLayout.h): the shader receives only the two resolved radii through
// CBTFrameParams::NearBias.{y,z} and ramps the split/merge pixel thresholds between them
// under pool contention (cbt_kernels.comp — smoothstep(nearR, farR, distToCam)). Because
// only the resolved radii cross to the GPU, tuning this curve needs no shader edit.
//
// The curve is deliberately SUB-LINEAR in altitude (nearR = coef * sqrt(altitude)) so the
// full-detail disc shrinks SLOWLY on descent. The prior linear curve (nearR = 1.5 *
// altitude) shrank the disc in lock-step with altitude, so a feature at a FIXED ground
// distance fell out of the disc as the camera descended toward it — some terrain became
// LESS tessellated as you zoomed in (the round-8e / PR #620 zoom-in report). Under sqrt,
// sqrt(4x)/sqrt(x) = 2: the disc halves per 4x altitude drop instead of quartering, so a
// feature ~3000 m out stays inside the disc through a 4000 m -> 1000 m descent
// (nearR 6325 m -> 3162 m, both > 3000 m) instead of coarsening up to ~2.3x.
//
// The bias only shapes DEMAND and only fires under pool contention (the in-shader
// occupancy gate), so this curve never breaks conformity and is a no-op on an unsaturated
// frame.

#include <algorithm>
#include <cmath>

namespace GameEngine::CBTTerrain
{

// nearR = kNearBiasNearSqrtCoef * sqrt(altitude), the full-detail disc radius (m). At
// coef = 100 the disc equals the altitude at alt = 10000 m (a 45-degree cone) and stays
// proportionally WIDER below that. This preserves the prior disc size at the high-altitude
// saturated pose (alt 4000 -> 6325 m vs the old 6000 m, ~5% larger) while roughly doubling
// it at the mid-altitude descent target (alt 1000 -> 3162 m vs the old 1500 m). Above alt
// ~4444 m the sqrt curve is actually SMALLER than the old linear one, so extreme-altitude
// demand is if anything reduced. The altitude clamp to 1 m (below) floors the disc at
// coef metres (100 m) for a camera on the deck, so no separate radius floor is needed.
inline constexpr float kNearBiasNearSqrtCoef = 100.0f;

// Far radius = max(kNearBiasFarAltMul * altitude, 2 * nearR), the distance by which the
// split/merge thresholds reach full coarsening. The altitude term dominates when the camera
// is high (a horizon-scale coarsen field); the 2 * nearR floor guarantees at least a
// one-octave coarsen band around the disc when the camera is low.
inline constexpr float kNearBiasFarAltMul = 6.0f;

// Far-field split-threshold multiplier (dimensionless, NearBias.w). Beyond the far radius
// the split/merge pixel thresholds are scaled up by this factor under full contention, so
// the far field stops splitting sooner and the freed slots refill the near disc.
inline constexpr float kNearBiasMaxCoarsen = 6.0f;

struct CBTNearBiasRadii
{
    float NearRadius = 0.0f; // full-detail disc radius (m); inside it the coarsen factor is 1
    float FarRadius = 0.0f;  // max-coarsen radius (m); at/after it the coarsen factor is maximal
};

// Resolve the near/far radii for a camera altitude above the surface (m). Clamps altitude to
// >= 1 m so the curve is well-defined at the surface (and floors the near disc at coef m).
// FarRadius > NearRadius always holds (the 2 * nearR floor), which the shader's
// smoothstep(nearR, farR, .) requires.
inline CBTNearBiasRadii ComputeNearBiasRadii(float altitude)
{
    const float alt = std::max(altitude, 1.0f);
    const float nearR = kNearBiasNearSqrtCoef * std::sqrt(alt);
    const float farR = std::max(kNearBiasFarAltMul * alt, 2.0f * nearR);
    return {nearR, farR};
}

} // namespace GameEngine::CBTTerrain
