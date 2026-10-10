#pragma once

#include "Components/Spline/SplinePlacement.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/VectorOps.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <span>
#include <vector>

namespace GameEngine::SplineLayout
{

// One sample of the draped (conformed) centerline, in world space.
struct CenterSample
{
    Mathematics::Vector3 Pos;
    Mathematics::Vector3 Normal;
    // False marks a sample whose conform probe found no surface — a terrain
    // hole or edge, ground the ray was told to ignore, ground out of its
    // reach. Pos.y and Normal are then the authored fallback rather than
    // measured ground, and since tile stations come from this polyline's 3D
    // length, measuring that fallback would let one miss re-parameterise (and
    // so re-count) the whole spline. The layout holds the nearest measured
    // altitude across the gap instead.
    bool SurfaceValid = true;
};

// The single conform-miss rule, shared by tile placement and the spline
// drape display: replaces the altitude and normal of every unmeasured sample
// with the nearest measured one's — the run before it, or, for a leading gap
// (which has nothing before it), the first measured sample after it. The gap
// then contributes the length of a plausible surface instead of a round trip
// up to the authored altitude. A polyline with no measured sample at all is
// left untouched: the authored altitudes are then the only surface there is.
// SurfaceValid flags are not modified.
void HoldSurfaceAcrossGaps(std::vector<CenterSample>& center);

// World-space pose for one placed tile. Right/Up/Forward are orthonormal as
// built; the seam-shear pass may skew Right within the ground plane afterwards.
struct TilePose
{
    Mathematics::Vector3 Position;
    Mathematics::Vector3 Right;
    Mathematics::Vector3 Up;
    Mathematics::Vector3 Forward;
    // The conformed centerline point this station stands on, before footprint
    // centering and planting move the pivot off it. Structural recipes join
    // stations base-to-base, and re-deriving this from Position would duplicate
    // the planting formula at every such consumer.
    Mathematics::Vector3 Base;
    // This station's ordinal along the spline BEFORE dropout removed any, which
    // is NOT its index in the returned vector once stations start disappearing.
    // Everything keyed per station — the pool pick above all — must key on this,
    // or raising Dropout would renumber the survivors and reshuffle every mesh
    // downstream of the first gap.
    uint32 StationIndex = 0;
};

// Every caller builds a pose out of the result, so this must convert an
// unusable direction into the fallback rather than into one. Non-finite input
// needs saying explicitly: NaN compares false against everything, so a bare
// `lenSq < eps` test would wave it through to the normalize, and an infinite
// lenSq passes such a test honestly only to compute v * (1/inf): NaN where a
// component was infinite, a zero vector where finite components merely
// overflowed when squared.
inline Mathematics::Vector3 NormalizedOrFallback(const Mathematics::Vector3& v,
                                                 const Mathematics::Vector3& fallback)
{
    const float32 lenSq = Mathematics::Vector3::Dot(v, v);
    if (!std::isfinite(lenSq) || lenSq < 1.0e-8f)
        return fallback;
    return v * (1.0f / std::sqrt(lenSq));
}

// Holds `up` inside a cone of `maxTiltDegrees` around world up, rotating it
// back along its own tilt plane so the azimuth of the lean survives — a piece
// held at the ceiling still leans the way its ground does, just less far.
//
// A piece's width axis is derived by crossing against this up, so an unbounded
// up is what turns a width axis skyward on a near-vertical face. Bounding the
// up is the only place that fix belongs: clamping the width axis afterwards
// could not put the basis back together orthonormally.
inline Mathematics::Vector3 ClampTiltFromWorldUp(const Mathematics::Vector3& up,
                                                 float32 maxTiltDegrees)
{
    const Mathematics::Vector3 worldUp(0.0f, 1.0f, 0.0f);
    // A NaN ceiling must not become a NaN basis: std::clamp passes NaN straight
    // through (both of its comparisons are false), and cos(NaN) would poison
    // every pose downstream. Authored floats reach here unsanitized.
    const float32 maxTilt = std::isfinite(maxTiltDegrees)
                                ? std::clamp(maxTiltDegrees, 0.0f, 90.0f)
                                : Components::kDefaultSplineMaxTiltDegrees;
    const float32 cosMax = std::cos(maxTilt * Mathematics::Pi / 180.0f);
    const float32 cosTilt = Mathematics::Vector3::Dot(up, worldUp);
    if (!std::isfinite(cosTilt) || cosTilt >= cosMax)
        return up;
    // The component of `up` across world up carries the lean's azimuth. It
    // vanishes only when `up` is antiparallel to world up (an upside-down
    // normal), which names no azimuth at all — world up is then the answer.
    const Mathematics::Vector3 across = up - worldUp * cosTilt;
    const float32 acrossLenSq = Mathematics::Vector3::Dot(across, across);
    if (!std::isfinite(acrossLenSq) || acrossLenSq < 1.0e-8f)
        return worldUp;
    const Mathematics::Vector3 azimuth = across * (1.0f / std::sqrt(acrossLenSq));
    const float32 sinMax = std::sqrt(std::max(0.0f, 1.0f - cosMax * cosMax));
    return worldUp * cosMax + azimuth * sinMax;
}

// What the tilt ceiling held back over one layout, for the author-facing
// report. The layout is a leaf with no logger, so it counts and the controller
// does the saying.
struct TiltClampReport
{
    uint32 ClampedStations = 0;
    // Steepest departure from world up SEEN, before clamping — the number that
    // tells the author how far past the ceiling their ground actually goes.
    float32 SteepestDegrees = 0.0f;
    Mathematics::Vector3 SteepestAt{0.0f, 0.0f, 0.0f};
};

// Re-reads the true surface under one tile station. `at` is the station's
// interpolated centerline point; on a hit the probe writes the surface altitude
// and normal under its XZ and returns true, and on a miss it leaves both alone.
// It runs after the stations are fixed and cannot move one in XZ, so neither
// outcome can shift another tile.
using StationSurfaceProbe = std::function<bool(const Mathematics::Vector3& at,
                                               float32& outAltitude,
                                               Mathematics::Vector3& outNormal)>;

// Everything BuildTilePoses needs beyond the centerline itself.
struct TileLayoutParams
{
    // Metres between tile centers along the draped centerline (pre-sanitized
    // by the caller; never zero).
    float32 Spacing = 2.0f;
    Components::SplinePlacementFit Fit = Components::SplinePlacementFit::FitToLength;
    // Blend each tile's up toward the sampled surface normal (HeightAndSlope).
    bool AlignToSurfaceNormal = false;
    float32 SlopeBlend = 1.0f;
    // Ceiling on the blend result (ClampTiltFromWorldUp). Both spline recipes
    // share the constant so "how far a placed piece may lean" means one thing.
    float32 MaxTiltDegrees = Components::kDefaultSplineMaxTiltDegrees;
    // Optional: counts the stations the ceiling held and the steepest ground
    // under them, so the controller can tell the author. Null = don't count.
    TiltClampReport* OutTiltClamp = nullptr;
    // Tile mesh local bounds: center compensates hostile pivots (footprint
    // centering + base lift), and the extents also decide which local axis runs
    // along the path (SplineLayout/PieceAxis.h) — that axis sets the chord window.
    Mathematics::Vector3 MeshBoundsCenter{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 MeshBoundsHalfExtents{0.0f, 0.0f, 0.0f};
    // Which plane of the mesh lands on the conform hit.
    Components::SplinePlantMode PlantMode = Components::SplinePlantMode::BoundsMin;
    uint32 MaxTiles = 2048;
    // Caller-supplied station distances along the draped centerline, in metres
    // and ascending. Empty = derive them from Spacing/Fit. Structural recipes
    // fill a run with pieces of DIFFERENT lengths, so their stations are not
    // uniformly spaced and no pitch or count can express where they land; the
    // caller that solved the fill is the only thing that knows.
    std::span<const float32> StationDistances;
    // Optional yaw source per station, parallel to StationDistances (or empty).
    // A near-zero entry falls back to the chord across the station's own
    // footprint. A shared corner station takes the BISECTOR of its two runs'
    // end directions, which neither run's own chord can produce — each would
    // give the same post a different answer.
    std::span<const Mathematics::Vector3> StationForwards;
    // Empty leaves every station on the interpolated centerline, whose chords
    // between dense samples cut across crests, kerbs and berms.
    StationSurfaceProbe Probe;

    // ---- Per-station variation (Placement knobs; all default to no-ops) ----
    // Salts every draw below. Shared with the pool pick so one Seed reproduces
    // the whole layout.
    uint32 Seed = 0;
    // Metres a station may slide along the centerline, either way. Clamped to
    // half of Spacing: at exactly half, two neighbours can meet but cannot
    // cross, so the station order — and with it every yaw taken from a chord —
    // survives any jitter setting.
    float32 SpacingJitterMetres = 0.0f;
    // Degrees a station may turn about world up, either way, applied to the
    // travel direction before the surface basis is built around it.
    float32 YawJitterDegrees = 0.0f;
    // Metres a station may slide across travel, either way. Applied before the
    // surface probe, so a slid station conforms to the ground it lands on.
    float32 LateralJitterMetres = 0.0f;
    // Fraction of stations left empty, 0..1.
    float32 DropoutChance = 0.0f;
    // Metres at each end over which the keep probability ramps to zero, so a run
    // thins out rather than stopping square. 0 disables.
    float32 EndTaperMetres = 0.0f;
};

// Walks the draped centerline by true 3D length and builds one pose per tile
// station: yaw from the chord across the tile's own footprint (not the
// instantaneous tangent), pitch and roll from the blended surface normal, the
// mesh footprint centered on the line and planted onto the surface per
// PlantMode. Stations come from params.StationDistances when the caller
// supplied them, otherwise from Spacing/Fit. Unmeasured samples hold a
// measured neighbour's altitude before the length is taken; each station's own
// altitude and normal then come from Probe when one is supplied. Returns empty
// when the centerline has fewer than two samples or no length.
//
// Stations are then thinned (DropoutChance, EndTaperMetres) and wobbled
// (SpacingJitter/YawJitter/LateralJitter) by draws that are pure functions of
// (Seed, channel, station ordinal). With every knob at its default this does
// nothing at all — not "almost nothing": the draws are not taken, and the poses
// are bit-identical to a build with no jitter code in it.
std::vector<TilePose> BuildTilePoses(const std::vector<CenterSample>& center,
                                     const TileLayoutParams& params);

} // namespace GameEngine::SplineLayout
