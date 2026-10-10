#pragma once

#include "ECS/Entity.h"
#include "Mathematics/Geometry.h"
#include "Types/Types.h"

#include <array>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::TerrainECS
{
struct PlanarHeightQuery;
}

namespace GameEngine::Editor
{

// The piece's underside centre plus its four corners. Five probes catch the case
// the single centre sample misses: a slab bedded at its middle with one corner
// standing proud of a corridor's shoulder.
constexpr uint32 kGroundGapProbeCount = 5u;

// Why a piece carries no gap numbers. Each value is a different cause with a
// different fix, so they never collapse into one "false": an unstreamed tile is
// waited out, a piece off the terrain edge is moved, and a wall-like piece has no
// underside to measure at all.
enum class GroundGapStatus : uint8
{
    Measured,
    // Every probe was refused: the piece hangs off the terrain footprint, or the
    // tile under it has not streamed in.
    NoGroundSampled,
    // The underside faces sideways rather than down, so it has no height above a
    // world XZ to compare against the ground.
    UndersideNearVertical,
    // The underside has no area — a piece whose local bounds are flat in X or Z,
    // or a transform that collapses them.
    DegenerateFootprint,
};

// One probe: a point ON the piece's underside, and the ground directly below it.
struct GroundGapProbe
{
    float32 X = 0.0f;
    float32 Z = 0.0f;
    // The underside's own height at this XZ. Exact, not interpolated: the probe
    // is a point of the underside quad rather than a sample of a plane fitted to
    // it.
    float32 UndersideY = 0.0f;
    float32 GroundY = 0.0f;
    float32 Gap = 0.0f;
    bool Sampled = false;
};

// The signed vertical distance between the composed ground and a placed piece's
// underside, measured over the piece's own footprint.
//
// SIGN: POSITIVE = floating (the piece sits above the ground), NEGATIVE =
// buried. This is the axis a conforming placement actually fails on — a station
// tilt improves as the ground is flattened under a slab, which is exactly when
// the gap opens.
//
// This is NOT the fence bury check in FenceLayout.h. That one is one-sided (it
// reports burial and says so at its own site that floating is out of scope),
// reports the OPPOSITE sign, and measures the pose's plant line rather than the
// mesh box. The two numbers are not comparable.
struct PieceGroundGap
{
    ECS::EntityHandle Piece{};
    GroundGapStatus Status = GroundGapStatus::NoGroundSampled;
    // The underside quad's centre height — the piece's own underside, not the
    // lowest corner of a box around it.
    float32 UndersideCenterY = 0.0f;
    // Probe 0 is the underside centre; 1..4 are its corners. Positioned even
    // when the ground refuses them, so a caller can see WHICH corner hangs off
    // the terrain.
    std::array<GroundGapProbe, kGroundGapProbeCount> Probes{};
    // Over the probed footprint: the most buried and the most floating reading,
    // plus the underside centre. MinGap comes from the probe whose ground stands
    // highest relative to the underside above it, MaxGap from the lowest. A
    // tilted piece bedded at its middle reads a NEGATIVE MinGap and a POSITIVE
    // MaxGap at once — that spread is the reading, not an error.
    float32 MinGap = 0.0f;
    float32 MaxGap = 0.0f;
    float32 CenterGap = 0.0f;
    // Probes that returned a trustworthy height, out of those attempted. A
    // piece overhanging the terrain edge or a tile that has not streamed in
    // leaves these short, and a partial reading is not a clean one.
    uint32 ProbesSampled = 0u;
    uint32 ProbesRequested = 0u;

    [[nodiscard]] bool FullySampled() const
    {
        return ProbesRequested > 0u && ProbesSampled == ProbesRequested;
    }
};

// Measure one piece against the composed ground.
//
// The underside is the piece's own lower face — the oriented box's lower local-Y
// quad, taken from the world matrix's columns — and every probe is a point of
// that quad. A world AABB cannot stand in for it: under rotation the AABB's
// min.y is the lowest UNDERSIDE CORNER rather than the underside beneath each
// probe, and the AABB's XZ footprint is wider than the piece, so its corners
// sample ground the piece does not cover. Both errors grow with tilt.
//
// Raising the piece raises every gap by the same amount, whatever its
// orientation — the metric stays monotonic in the piece's height.
//
// `out.Status` says whether the reading is usable; on anything but Measured the
// gaps are left at zero rather than filled with a number that reads like a
// measurement.
void MeasurePieceGroundGap(const TerrainECS::PlanarHeightQuery& ground,
                           const Mathematics::BoundingBox& localBox,
                           const float32 worldMatrix[16], PieceGroundGap& out);

// Measure every placed piece parented to `route`.
//
// Pieces are the route's direct children carrying both LocalBounds and
// WorldTransform — what the spline placement controllers create. The order is
// the child-scan order, NOT the authored station ordinal: the controllers keep
// that mapping in private state and renumber it whenever spacing changes.
//
// WorldTransform is written by TransformHierarchySystem, not by the placement
// controllers, so a piece created this frame is invisible here until that system
// has run.
void MeasureRouteGroundGaps(ECS::World& world, ECS::EntityHandle route,
                            const TerrainECS::PlanarHeightQuery& ground,
                            std::vector<PieceGroundGap>& out);

} // namespace GameEngine::Editor
