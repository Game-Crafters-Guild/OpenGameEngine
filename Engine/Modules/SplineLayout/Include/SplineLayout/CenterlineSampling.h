#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Types/Types.h"

namespace GameEngine::SplineLayout
{

// Metres between dense samples of the draped centerline. Pinned: tile stations
// are derived from the measured length of the draped polyline, so anything that
// varies this step at runtime varies how many tiles the user gets.
inline constexpr float32 kCenterlineStepMetres = 0.5f;

// Longest centerline the pinned step covers within the sample budget. Past it
// the step coarsens, the drape measures short and tiles are lost, so the caller
// warns rather than crossing it silently.
inline constexpr float32 kMaxCenterlineLengthMetres = 4096.0f;

// One sample per step, plus the endpoint, plus one to close the final partial
// step.
inline constexpr uint32 kMaxCenterlineSamples =
    static_cast<uint32>(kMaxCenterlineLengthMetres / kCenterlineStepMetres) + 2u;

// Largest per-axis scale of the transform's basis: the local-to-world
// conversion factor for scalar spline quantities (arc lengths, channel
// widths). Exact under uniform scale; under non-uniform scale the largest
// axis wins so a world quantity derived from it is never under-scaled.
float32 LargestAxisScale(const Mathematics::Matrix4x4& worldMatrix);

// SplineData arc lengths are entity-local, while the draped polyline, the tile
// spacing and the step above are world metres. Scaling by the transform's
// largest axis keeps world-space sampling at least as fine as the step asks for
// under any scale.
float32 WorldCenterlineLength(float32 localArcLength,
                              const Mathematics::Matrix4x4& worldMatrix);

// Dense sample count for a world-space centerline length at the pinned step,
// clamped to the budget above. Non-positive and non-finite lengths degrade to
// the two-sample minimum rather than to an out-of-range cast.
uint32 CenterlineSampleCount(float32 worldLength);

} // namespace GameEngine::SplineLayout
