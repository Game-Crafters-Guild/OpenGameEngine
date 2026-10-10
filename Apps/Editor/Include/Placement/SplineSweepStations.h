#pragma once

#include "Mathematics/Matrix4x4.h"
#include "Placement/TileLayout.h"
#include "Spline/SplineTypes.h"
#include "SplineGeometry/SplineCorner.h"
#include "SplineGeometry/SplineStation.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Spline { class SplineData; }

namespace GameEngine::Editor
{

// What a sweep builds beyond its sampled centreline. The default adds nothing,
// which is how paths and roads sweep.
struct SweepShape
{
    // What stands at a corner: every interior authored point of a Linear
    // spline, and a point of a smooth one where its tangent turns by more than
    // kSweepCornerDeflectionDegrees. None leaves the points between samples,
    // where the sweep cuts across them.
    SplineGeometry::SplineCornerStyle Corners = SplineGeometry::SplineCornerStyle::None;
    // Put a sample on every interior authored point, corner or not, and list
    // them (SweepSamples::Points), for a sweep that steps at its points.
    bool StationAtPoints = false;
    // On a smooth spline, add samples between the drape's own until each chord
    // lies within kSweepSagittaMetres of the curve.
    bool FollowCurve = false;
    // On a closed spline, weld the run to itself: the last ring repeats the
    // first, whose frame looks across the seam, and the ends take no caps.
    bool CloseLoop = false;
    // Sink the profile's grounded points by the fall of the ground across the
    // run, read from the normal of each sample's conform hit, so a base on a
    // cross slope stands in the ground along its downhill edge.
    bool GroundBase = false;
    // Lay each side face's U along that face's own length rather than the
    // centreline's (SplineGeometry::AccumulateFaceTurn), so the outside and the
    // inside of a bend or a corner tile at one size.
    bool FaceU = false;
};

// SplineExtrude's shape: paths, roads and water sweep their samples exactly as
// sampled, with no corners, weld, grounded base or face U. The extrude
// controller and the byte-identity pin (SplineSweepParity) both build from it.
[[nodiscard]] SweepShape SweepShapeForExtrude();

// A smooth spline's authored point is a corner when its tangent turns by more
// than this across it (a Bezier with broken handles): a gentler kink is part of
// the curve, which the sweep follows at the sagitta.
inline constexpr float32 kSweepCornerDeflectionDegrees = 5.0f;

// Steepest cross slope a grounded base sinks for: tan 60°. Ground steeper than
// that across a wall is a cliff face, not a slope to stand in, so the base sinks
// as if it were this steep; a hit normal lying flat would otherwise sink it
// without bound.
inline constexpr float32 kMaxGroundedCrossSlope = 1.7320508f;

// The chord-to-curve deviation a curve-following sweep allows. Past 2 cm a
// faceted wall reads as faceted; below it no author has a reason to go.
inline constexpr float32 kSweepSagittaMetres = 0.02f;
// How many times one drape interval may be halved to meet the sagitta, which
// bounds the rings a hostile curve can mint: three halvings leave at most seven
// samples inside an interval, 6.25 cm apart at the 0.5 m drape step, which
// meets the sagitta down to a 2.5 cm radius.
inline constexpr uint32 kMaxSagittaHalvings = 3;

// The centreline a sweep drapes, before the drape: the uniform samples the
// ground is probed at, plus the samples its shape inserts between them.
struct SweepSamples
{
    // Placer-local frames in run order.
    std::vector<Spline::SplineFrame> Frames;
    // Local arc distance of each frame, which the spline's channels are keyed by.
    std::vector<float32> LocalDistance;
    // 1 for a sample the ground is probed under; 0 for an inserted one, which
    // takes its drape from the probed samples either side (DrapeInsertedSamples)
    // so a finer sweep casts no extra rays.
    std::vector<uint8> Probed;
    // The samples standing on authored corner points, ascending.
    std::vector<SplineGeometry::SplineCornerSite> Corners;
    // The samples standing on interior authored points, corner or not,
    // ascending: filled when the shape asks for a station at every point.
    std::vector<uint32> Points;
};

// `sampleCount` samples uniform in arc length, plus, for `shape`: a sample on
// every interior authored point that is a corner (so the corner lands on a ring
// instead of between two; a welded loop's first point, where the loop closes, is
// its first and last sample) or, when the shape asks, on every interior point;
// and on a smooth spline, samples inside every drape interval whose chord strays
// more than kSweepSagittaMetres of world distance from the curve at its
// midpoint.
[[nodiscard]] SweepSamples SampleSweepCenterline(const Spline::SplineData& data,
                                                 uint32 sampleCount,
                                                 const Mathematics::Matrix4x4& placerWorld,
                                                 const SweepShape& shape);

// Give every inserted sample of `center` (draped world samples, one per frame)
// the drape of the probed samples either side: their height above the authored
// curve and their ground normal, interpolated along the run. An inserted sample
// beside ground no ray found is marked unmeasured, for HoldSurfaceAcrossGaps.
void DrapeInsertedSamples(const SweepSamples& samples, const Mathematics::Matrix4x4& placerWorld,
                          std::span<CenterSample> center);

// The station stream a swept recipe is built from, derived from its draped
// centreline: one station per sample, walked by true 3-D length.
struct SweepStationStream
{
    // The builder's input, in the placer's local space, carrying the spline's
    // width channel.
    std::vector<SplineGeometry::SplineStripStation> Local;
    // The same stations in world space and without widths, filled only when
    // asked for: the water fill floods the terrain lattice, which exists in no
    // other space.
    std::vector<SplineGeometry::SplineStripStation> World;
    // World metres run along the draped centreline to each Local station. It is
    // what chunks are carved on and what U is measured from.
    std::vector<float32> WorldDistance;
    // What the author should be told about the corners, by authored point:
    // corners past the mitre limit, built Round, legs too short for their
    // corners' reach, and curves that bend tighter than the wall is wide.
    std::vector<SplineGeometry::SplineCornerIssue> CornerIssues;
    // WorldDistance of each of SweepSamples::Points, which the corners keep:
    // where a stepped top steps.
    std::vector<float32> PointDistances;
};

// Build the stations for `center`, the draped world-space samples taken at
// `samples.Frames`, and give the Local stream the corners, the welded loop and
// the grounded base `shape` asks for.
//
// Each Local station carries the half-widths the builder will place its
// profile at under `widthScale`: the spline's width channel, or under
// SplineProfileScale::None, which ignores station widths, the profile's own
// `nominalHalfWidth`. The corners' reach and the grounded base's sink are
// measured on them, so both follow the wall as it is built.
//
// The frame is derived from the SHARED polyline rather than from a probe under
// each station, so consecutive stations cannot lean opposite ways the way
// independently sampled rigid tiles do on rippled ground.
//
// Frame directions map through the inverse world matrix and are normalized
// INDIVIDUALLY: under a non-uniform placer scale the mapped frame loses its
// orthogonality, and the builder's normals — computed as if Right ⊥ Up — skew
// with it, up to ~18° at 2:1 anisotropy on a 45° profile edge. The render-side
// normal matrix cannot repair a normal that is already wrong in local space:
// keep placers uniformly scaled where shading matters.
[[nodiscard]] SweepStationStream BuildSweepStations(
    const Spline::SplineData& data, const SweepSamples& samples,
    std::span<const CenterSample> center, const Mathematics::Matrix4x4& invPlacerWorld,
    float32 nominalHalfWidth, SplineGeometry::SplineProfileScale widthScale, bool worldStations,
    const SweepShape& shape);

// The author-facing validation for a sweep's corners: one line per issue,
// opening with `recipeName` (the component the author added), naming its point
// and stating the fix, for ReportRecipeValidation. A line carries the point and
// the kind of problem only, so dragging a point that stays past the limit does
// not report it again.
[[nodiscard]] std::vector<std::string> CornerValidation(
    std::string_view recipeName, std::span<const SplineGeometry::SplineCornerIssue> issues);

} // namespace GameEngine::Editor
