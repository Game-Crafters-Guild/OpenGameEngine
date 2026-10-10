#pragma once

#include "Mathematics/Vector3.h"
#include "SplineGeometry/SplineProfile.h"
#include "SplineGeometry/SplineStation.h"
#include "SplineGeometry/SplineVertex.h"
#include "Types/Types.h"

#include <limits>
#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// How the width channel scales the cross-section. A road that widens should not
// get taller, but a rampart that widens probably should, so this is a named
// mode rather than a hard rule.
enum class SplineProfileScale : uint8
{
    LateralOnly = 0,
    Uniform,
    None,
};

struct SplineStripParams
{
    SplineProfileScale WidthScale = SplineProfileScale::LateralOnly;
    // U = Distance * TilesPerMetreU; V = scaled profile perimeter *
    // TilesPerMetreV. NEVER normalized: normalizing gives a 10 m wall and a
    // 100 m wall the same number of brick courses, which is the stretching
    // world-metre UVs exist to avoid. V is measured on the SCALED profile, so a
    // road that tapers keeps its texel density instead of shearing its texture.
    float32 TilesPerMetreU = 1.0f;
    float32 TilesPerMetreV = 1.0f;
    // U origin in metres: U = (Distance - UOriginMetres) * TilesPerMetreU.
    // Non-finite (the default) rebases to the first station, the right thing
    // for a standalone single-mesh build. A caller cutting one run into chunks
    // that share boundary stations passes the run's global origin instead, so
    // every chunk lies on ONE U axis and the shared ring gets the same U from
    // both sides.
    float32 UOriginMetres = std::numeric_limits<float32>::quiet_NaN();
    // Closed loop: nudge the effective U density so the run spans a whole
    // number of tiles. Total length is rarely an integer tile count, so a loop
    // otherwise shows a partial tile where its end meets its start; the density
    // change is sub-1% and there is no case for the visible seam.
    //
    // The snap rounds against the span THIS CALL is given. A chunking caller
    // must therefore snap once over the full run, pre-multiply the density, and
    // leave this off — per-chunk snapping drifts the density chunk by chunk.
    bool SnapUForClosedLoop = false;
    // Finite: a welded loop of this length, in the stations' Distance metres,
    // whose faces each snap to a whole number of tiles. A side face runs
    // LoopLengthMetres + |x| · the loop's face turn on its side (the last
    // station's FaceTurnLeft or FaceTurnRight), and its U density is rounded so
    // that length spans whole tiles; the top and bottom faces round the
    // centreline length. Every face then closes on itself at the seam, and the
    // two sides of a bend tile within half a tile of one size. A caller that
    // cuts the loop into chunks passes the whole loop here, so every chunk
    // rounds against one length. Non-finite (the default) leaves TilesPerMetreU
    // as given.
    float32 LoopLengthMetres = std::numeric_limits<float32>::quiet_NaN();
    float32 LoopFaceTurnLeft = 0.0f;
    float32 LoopFaceTurnRight = 0.0f;
    // The top and bottom snap over LoopLengthMetres plus the loop's whole arc
    // allowance (the last station's TopArcAllowance), so the top meets itself
    // at the seam on a whole tile too.
    float32 LoopTopArcAllowance = 0.0f;
    // Metres of Distance per unit of the profile's lateral offset, which scales
    // |x| wherever U reads a face turn: a sweep built in a placer's local space
    // and measured in world metres passes the placer's scale.
    float32 LateralMetresPerUnit = 1.0f;
    // Honoured only for a closed profile. An open strip has no polygon to
    // triangulate and capping it would emit a degenerate sliver.
    bool GenerateCaps = true;
    // Finite: the side faces (those whose edge normal is more lateral than
    // vertical) and the caps take V as the vertex's height above this datum
    // times TilesPerMetreV, so coursed stone stays level on a slope and meets
    // itself across corners and chunks; top and bottom faces keep the
    // perimeter V. Height is the vertex's Y in the stations' own space: a caller
    // building in a placer's local space passes a datum in that space, and gets
    // level courses in the world only when the placer keeps +Y up. Non-finite
    // (the default) keeps the perimeter V everywhere, which is what paths and
    // roads want.
    float32 HeightVDatumMetres = std::numeric_limits<float32>::quiet_NaN();
};

struct SplineStripMesh
{
    std::vector<SplineVertex> Vertices;
    std::vector<uint32> Indices;
    Mathematics::Vector3 MinBounds{};
    Mathematics::Vector3 MaxBounds{};

    // Vertices per station ring, including the extra slots Hard creases and the
    // closed profile's V seam add. The first RingCount * RingVertexCount
    // vertices are the rings in station order; any riser and cap vertices
    // follow.
    uint32 RingVertexCount = 0;
    uint32 RingCount = 0;
    // Stations dropped because their distance did not advance past the previous
    // one. A duplicated station contributes a zero-length quad -- no surface,
    // but a discontinuity in the tangent frame -- so they are skipped and
    // counted rather than welded into the strip. A corner's Crease and Pivot
    // rings stand at their corner's distance by design and are kept.
    uint32 DroppedStations = 0;

    [[nodiscard]] bool IsValid() const { return !Vertices.empty() && !Indices.empty(); }
};

// Sweep `profile` along `stations`, welding each ring to the next.
//
// Longitudinal continuity is C0 BY CONSTRUCTION: consecutive rings share their
// vertices, so there is no seam to misalign, no wedge of missing area on a
// curve, and no quantised piece count. That is the structural difference from
// rigid tile placement, not a quality setting -- a rigid tile cannot bury one
// end in a slope or overlap its neighbour if there are no tile ends.
//
// Returns an empty mesh when the profile has fewer than two points or fewer
// than two stations survive the ascending-distance filter.
[[nodiscard]] SplineStripMesh BuildSplineStrip(const SplineProfile& profile,
                                               std::span<const SplineStripStation> stations,
                                               const SplineStripParams& params);

} // namespace GameEngine::SplineGeometry
