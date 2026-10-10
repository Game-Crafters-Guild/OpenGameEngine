#pragma once

#include "Spline/SplineTypes.h"
#include "Types/StringId.h"

namespace GameEngine::Spline
{

class SplineData;

// Well-known per-point channel names. Consumers sample authored per-point data
// by channel name through SampleChannelAtDistance; the names live here so every
// consumer spells them identically.
namespace SplineChannels
{
inline constexpr StringId kWidth = "width"_sid;
} // namespace SplineChannels

// ---- Core evaluation ----

// Evaluate the spline at global parametric t in [0, 1].
// t=0 is the first point, t=1 is the last (or wraps for closed splines).
[[nodiscard]] SplineFrame Evaluate(const SplineData& spline, float32 t);

// Evaluate position only (faster than full frame computation).
[[nodiscard]] Mathematics::Vector3 EvaluatePosition(const SplineData& spline, float32 t);

// ---- Arc-length parameterization ----

// Evaluate at a distance along the spline (requires valid arc-length cache).
[[nodiscard]] SplineFrame EvaluateAtDistance(const SplineData& spline, float32 distance);

// Convert between parametric t and arc-length distance.
[[nodiscard]] float32 ParametricToDistance(const SplineData& spline, float32 t);
[[nodiscard]] float32 DistanceToParametric(const SplineData& spline, float32 distance);

// ---- Spatial queries ----

// Find the closest point on the spline to a query position.
// Uses segment AABB broad phase + ternary search refinement.
[[nodiscard]] ClosestPointResult FindClosestPoint(const SplineData& spline,
                                                   const Mathematics::Vector3& queryPos);

// Signed distance from a point to the spline's swept volume.
// Positive = outside the radius envelope, negative = inside.
// Requires valid segment bounds cache.
[[nodiscard]] float32 SignedDistanceToSpline(const SplineData& spline,
                                             const Mathematics::Vector3& queryPos);

// ---- Ground-plane (XZ) queries ----
//
// These project both the curve and the query onto the XZ plane, so the spline's
// altitude never enters the distance. That is what a top-down footprint needs —
// a road 200 m up covers the same ground as one at sea level — and it keeps the
// curve's Y available as DATA: the returned Position is the full 3-D point, so
// callers can read the height to flatten to at the closest point.
// Taking x/z rather than a Vector3 keeps the projection visible at every call
// site instead of silently discarding a component the caller passed in.

// Closest point on the spline's XZ projection. Distance is the XZ distance;
// Position and T address the full 3-D curve.
[[nodiscard]] ClosestPointResult FindClosestPointXZ(const SplineData& spline,
                                                     float32 worldX, float32 worldZ);

// Signed XZ distance to the spline's swept footprint: the XZ distance to the
// path minus the interpolated radius there. Positive = outside the footprint,
// negative = inside. Points enclosed by a closed spline are always negative.
[[nodiscard]] float32 SignedDistanceToSplineXZ(const SplineData& spline,
                                               float32 worldX, float32 worldZ);

// Signed XZ distance to the swept BAND only: the region a closed spline encloses
// never counts as inside, so a closed path stays a ring instead of becoming a
// disc. Identical to SignedDistanceToSplineXZ on open splines, which enclose
// nothing — the two differ exactly where a modifier volume must choose between
// "along this loop" and "inside this loop".
[[nodiscard]] float32 SignedDistanceToSplineBandXZ(const SplineData& spline,
                                                   float32 worldX, float32 worldZ);

// ---- Per-point channels ----

// Sample a named per-point channel at an arc-length distance. Values are
// interpolated linearly per segment — never through the Catmull-Rom basis, so
// a channel sample can't overshoot its authored knot values. Returns
// `fallback` for an unknown channel or an invalid spline.
//
// Facade: today SplineChannels::kWidth is the only channel and is backed by
// SplineControlPoint::Radius; the generalized channel store lands behind this
// exact signature.
[[nodiscard]] float32 SampleChannelAtDistance(const SplineData& spline, StringId channel,
                                              float32 distance, float32 fallback);

// ---- Cache management ----

// Rebuild arc-length LUT, segment AABBs, and CatmullRom auto-tangents.
// Call after modifying points. Clears the dirty flag.
void RebuildSplineCache(SplineData& spline);

// ---- Utility ----

// Sample N evenly-spaced points along the spline by arc length.
// Useful for mesh generation, rendering, and uniform placement.
void SampleUniform(const SplineData& spline, uint32 sampleCount,
                   std::vector<SplineFrame>& outFrames);

} // namespace GameEngine::Spline
