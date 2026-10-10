#pragma once

#include "Types/Types.h"

#include <vector>

namespace GameEngine::Spline
{
class SplineData;
}

namespace GameEngine::TerrainECS
{

// ---- Route geometry ---------------------------------------------------------
//
// A route's geometry is the authored spline RESAMPLED BY ARC LENGTH into a
// station polyline, and the polyline — not the analytic curve behind it — is
// what the bake evaluates. Every consumer (the CPU bake, and the offline
// instrument that audits it) evaluates this same object segment-nearest, so
// they agree to float rounding instead of agreeing to a model.
//
// The distinction is not cosmetic. A Catmull-Rom curve and a polyline through
// points sampled on it are different surfaces: the chord cuts the corner by
// roughly spacing^2 / (8 * radiusOfCurvature). Two evaluators that each pick
// their own resampling density differ by that much everywhere the route bends,
// which reads as a small plausible offset rather than as a bug.

// One resampled station: where the route is, how high it grades there, and how
// wide it is. HalfWidth is the spline's own swept radius at that arc length.
struct RouteStation
{
    float32 X = 0.0f;
    float32 Z = 0.0f;
    float32 Y = 0.0f;         // route grade at this station, world Y
    float32 HalfWidth = 0.0f; // swept half-width in metres
};

// The resampled route. Built once per gather and read per texel, so it owns no
// behaviour of its own beyond being the geometry.
struct RoutePolyline
{
    std::vector<RouteStation> Stations;

    // Fewer than two stations is not a route: there is no segment to project
    // onto, and every consumer reads that as "contributes nothing".
    bool IsValid() const { return Stations.size() >= 2; }
};

// What the polyline says about one XZ sample: the distance to the nearest point
// on it, and the route's own grade and width interpolated to that point.
// Distance is +inf for an invalid polyline, which every weight ramp reads as
// fully outside.
struct RouteSample
{
    float32 Distance = 0.0f;
    float32 HalfWidth = 0.0f;
    float32 Height = 0.0f;

    // Distance from the swept band's EDGE, POSITIVE INSIDE — the convention
    // every shape ramp takes (ShapeFalloffWeight). An invalid polyline's +inf
    // distance leaves this -inf, which the ramp reads as fully outside.
    float32 DistanceFromEdge() const { return HalfWidth - Distance; }
};

// Arc-length spacing bounds. Scene text is not a trusted input — the spacing is
// a plain float field and the station count is length/spacing — so a hand-edited
// or corrupt spacing must not turn a gather into a hang or an allocation storm.
// The floor is well below any authoring intent (5 cm on a heightfield sampled at
// 50 cm); the cap bounds a single route's stations independently of it.
inline constexpr float32 kMinRouteStationSpacing = 0.05f;
inline constexpr uint32 kMaxRouteStations = 65536u;

// Resample `spline` by arc length at `stationSpacing` metres into `out`.
//
// The spline is expected in WORLD space (the modifier gather transforms it
// before resolving the shape) and to carry a valid arc-length cache.
//
// `stationSpacing` is an UPPER BOUND, not the exact step: the route is divided
// into ceil(length / spacing) equal intervals, so the first station is the route
// start, the last is exactly its end, and every interval is the same length.
// Stepping by the literal spacing instead would leave a short final interval
// whose length depends on the route's — a second geometry rule for the offline
// twin to reproduce, at the end of the route where the grade matters most.
//
// `out` is cleared first; an unevaluable spline leaves it empty (IsValid false).
void BuildRoutePolyline(const Spline::SplineData& spline, float32 stationSpacing,
                        RoutePolyline& out);

// Nearest point on the polyline in XZ, with the route's grade and half-width
// interpolated linearly along the winning segment.
//
// Segment-nearest, not curve-nearest: the projection is clamped to each segment
// and the segments are the geometry. Degenerate segments (both endpoints at the
// same XZ) are skipped rather than dividing by zero.
[[nodiscard]] RouteSample ClosestStationXZ(const RoutePolyline& polyline,
                                           float32 worldX, float32 worldZ);

// World-space XZ bounds of the polyline's swept footprint, padded by `falloff`.
//
// Derived from the stations rather than from the curve's cached segment boxes,
// because the stations are what the bake evaluates: a box drawn around the curve
// can be both looser than needed and — where a chord leaves a segment's own box —
// not a superset of what the polyline covers.
//
// Leaves an INVERTED box for an invalid polyline, which every bake loop reads as
// "no samples", exactly as an unresolved spline does.
void RoutePolylineBoundsXZ(const RoutePolyline& polyline, float32 falloff,
                           float32& outMinX, float32& outMinZ,
                           float32& outMaxX, float32& outMaxZ);

} // namespace GameEngine::TerrainECS
