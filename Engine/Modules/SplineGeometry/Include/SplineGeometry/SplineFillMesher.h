#pragma once

#include "SplineGeometry/SplineFillField.h"
#include "SplineGeometry/SplineStation.h"
#include "SplineGeometry/SplineVertex.h"
#include "Types/Types.h"

#include <limits>
#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// Turns a flooded region into a water surface.
//
// The primitive is marching squares used as an AREA fill rather than as a
// contour: each cell emits the polygon of ITSELF that lies inside the region, so
// there is no boundary polygon to extract and no polygon-with-holes
// triangulator to write for a region that routinely has islands.
//
// Three properties are obtained by construction rather than by a repair pass:
//
//  - NO T-JUNCTIONS. A shoreline vertex belongs to the lattice EDGE it crosses,
//    not to a cell, so the two cells sharing that edge index the same vertex and
//    cannot disagree about where the shoreline is.
//  - WATERTIGHT AT SADDLES. The cell-centre decider chooses only how one cell's
//    interior connects; the edge crossings are untouched, so no two neighbours
//    can disagree and no crack can open.
//  - A BURIED RIM. The contour runs at the field's own iso (Inside = 0, i.e.
//    phi = -EdgeDrop) and the boundary vertex sits EdgeDrop below the waterline,
//    so the edge tucks into the bank instead of z-fighting it.
//
// UV0 IS A MEASUREMENT, NOT A TEXTURE CHART:
//
//    uv0.x   arc length along the flow, world metres
//    uv0.y   still-water depth beneath this vertex, world metres
//
// A water shader needs to know how deep the water is and which way it runs;
// neither is recoverable from a texture chart, and a shader that guesses them
// from world position gets a shoreline wherever an axis happens to cross rather
// than where the bank is. So the mesh carries the two quantities and the shader
// derives its own tiling from them (metres times a per-material 1/m density),
// which is why no tiles-per-metre lives here.
//
// Both are world metres and are stamped in world space, so a caller that moves
// the vertices into a placer's local space must NOT transform uv0 with them.
//
// The DEPTH is the surface's, not the vertex's: a rim vertex is deliberately
// sunk EdgeDrop below the water, so uv0.y there is the depth of the water above
// it and not position.y minus the bed. Where the shoreline is decided by the
// ground (the usual case) that lands at exactly -EdgeDrop, because Inside is
// affine in depth there -- so a shader clamping at zero gets its foam band
// exactly on the waterline.
//
// The ARC is the field's nearest-station projection, so it is single-valued but
// steps across a medial axis, where the governing station flips between two legs
// of one basin. That step is real -- the two legs ARE at different arc positions
// -- but it is not free. The shipped water shader tiles its three ripple layers
// and its glint on uv0.x, so the step shows as a PHASE seam in the ripple
// wherever two legs share a basin. Accepted: a noise-phase discontinuity along a
// line the surface already steps across is far less wrong than the shoreline
// error it replaces.
//
// Do NOT expect MedialStepMetres to find it. That diagnostic measures the
// WATERLINE step between adjacent wet corners, which is ~0 when the two legs sit
// at the same altitude -- exactly the case where the arc step is largest.

struct SplineFillMeshParams
{
    // Metres the shoreline vertex sits below its waterline. The same EdgeDrop
    // the field contoured with -- one number doing both halves of burying an
    // edge, as it already did for the ribbon.
    float32 EdgeDrop = 0.1f;
};

// The block of CELLS a single mesh covers, in corner coordinates. Chunking runs
// on the lattice rather than on arc length: chunk boundaries then fall on
// lattice lines, both sides evaluate the same field at the same corner, and the
// duplicated boundary vertices are BIT-IDENTICAL rather than merely close.
struct SplineFillChunkRange
{
    uint32 MinCornerX = 0;
    uint32 MinCornerZ = 0;
    // One past the last CORNER, so a range covers (MaxCornerX - MinCornerX - 1)
    // cells across.
    uint32 MaxCornerX = 0;
    uint32 MaxCornerZ = 0;
};

struct SplineFillMesh
{
    std::vector<SplineVertex> Vertices;
    std::vector<uint32> Indices;
    Mathematics::Vector3 MinBounds{};
    Mathematics::Vector3 MaxBounds{};

    [[nodiscard]] bool IsValid() const { return !Vertices.empty() && !Indices.empty(); }
};

// Mesh one lattice block of the flooded region. Vertices come out in the space
// the ground grid and stations were given in -- world, for every caller that has
// a terrain, since a terrain lattice exists in no other space.
//
// An empty mesh means this block holds no wet cell, which is a hole in the
// chunk plan rather than a failure.
[[nodiscard]] SplineFillMesh BuildSplineFillMesh(const SplineFillResult& field,
                                                 const SplineGroundGrid& ground,
                                                 std::span<const SplineStripStation> stations,
                                                 const SplineFillChunkRange& range,
                                                 const SplineFillMeshParams& params);

// Cover a grid in blocks of at most `cellsPerChunk` cells per axis, in row-major
// order. Chunk COUNT changes as the region grows or shrinks, which the
// controller's slot plan already handles by index.
[[nodiscard]] std::vector<SplineFillChunkRange> CarveFillChunks(const SplineGroundGrid& ground,
                                                                uint32 cellsPerChunk);

} // namespace GameEngine::SplineGeometry
