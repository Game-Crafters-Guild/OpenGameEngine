#pragma once

#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace GameEngine::MarkupECS
{

struct MarkupAreaPiece;

// A region's ground, as its display stands on it: the rings of its area sampled along the ground
// and the lid's points over the area, each at its ground height (the higher of the ground and the
// sea level). Built when the outline, the placement, the members, the sea level or the ground
// under it changes; the extrusion height is applied on top (BuildMarkupRegionMesh), so a height
// drag rebuilds no ground.
struct MarkupRegionGround
{
    // The rings' ground samples in world meters, ring after ring (RingEnds), each ring's closing
    // point not repeated. A region's outer rings run counter-clockwise seen from above (+Y) and
    // the holes its Exclude members cut run clockwise, so a wall wound out of its ring faces out of
    // the area on both. A region without members that change its area has one ring, its base
    // outline. An open outline (a path, or fewer than three knots) is one ring kept in order, with
    // no area.
    std::vector<Mathematics::Vector3> Outline;
    // Outline on the ground plane (x, z), in the same order.
    std::vector<Mathematics::Vector2> OutlineXZ;
    // One past each ring's last sample in Outline, in order.
    std::vector<uint32> RingEnds;
    // The highest of the outline's samples.
    float32 Top = 0.0f;
    // Whether Outline is a closed ring the walls stand on; false for an outline that is not
    // closed yet (OutlineOnly draws).
    bool Closed = false;
    // Whether the base outline crosses itself (a knot drag passing through such a state): the
    // walls stand on the base outline alone (the members do not apply), the lid is dropped until
    // the ring is simple again.
    bool Crosses = false;
    // The lid's points at their ground height, the outline's first; triangles index them,
    // counter-clockwise from above. Empty with no lid.
    std::vector<Mathematics::Vector3> LidPoints;
    std::vector<uint32> LidTriangles;
    // Each lid point's distance to the nearest ring on the ground plane, meters (0 on it).
    std::vector<float32> LidEdgeDistance;
    // The ring's pole of inaccessibility (MarkupECS::RegionLabelPoint) at its ground height: where
    // the label stands, ExtrudeHeight above. Inside even a C-shaped outline. An open outline's (a
    // path's) is the point halfway along it.
    Mathematics::Vector3 LabelGround{};
    // The bounding sphere of the rings at ground, for culling and the body cap's distance.
    Mathematics::Vector3 Center{};
    float32 Radius = 0.0f;
    // A path's (an open outline's) ground across its band: for each outline sample, the ground
    // kPathEdgeOffsets[k] meters to its left (k = 0 to 3), then to its right (PathAcross), so the
    // band's edges stand on the ground beside the line at any width. Empty for a closed outline.
    std::vector<float32> EdgeGround;

    std::size_t GetRingCount() const { return RingEnds.size(); }
    std::span<const Mathematics::Vector3> GetRing(std::size_t ring) const;
    std::span<const Mathematics::Vector2> GetRingXZ(std::size_t ring) const;
    // Whether the ground-plane point (x, z) lies in the area: inside an odd number of its rings.
    // False for an open outline.
    bool Contains(const Mathematics::Vector2& xz) const;
};

// The distances either side of a path's samples at which its ground is read (EdgeGround): the
// band's narrowest half width, then wider, to the half width at about two kilometers.
inline constexpr std::array<float32, 4> kPathEdgeOffsets{1.5f, 4.0f, 10.0f, 25.0f};

// The ground height at a world (x, z): the caller's ground (terrain only, so a region stands on
// the terrain under a house rather than on its roof) raised to the sea level.
using MarkupGroundSampler = std::function<float32(const Mathematics::Vector2& xz)>;

// The lid's triangles are split until no edge is longer than this or the region's larger extent
// over kLidEdgeExtentDivisions, whichever is longer, so the lid follows the ground's shape.
inline constexpr float32 kLidMinEdgeMeters = 4.0f;
inline constexpr float32 kLidEdgeExtentDivisions = 32.0f;
// The most lid triangles a region gets; the split stops there (a 30 km region still reads).
inline constexpr std::size_t kMaxLidTriangles = 16384;

// The ground-plane direction across an open outline at sample `i`, to the right of travel (+X when
// travelling +Z): the perpendicular of its two neighbours' chord.
Mathematics::Vector3 PathAcross(std::span<const Mathematics::Vector3> line, std::size_t i);

// The length along `line`'s points in order (a path's draped samples: its length along the ground).
float32 PolylineLength(std::span<const Mathematics::Vector3> line);

// The ground `halfWidth` meters to the right (`side` +1) or left (-1) of a path's sample `i`: its
// EdgeGround interpolated between the offsets, the outer one held past it; the sample's own height
// without EdgeGround. Exact on a planar cross-section. On a curved one (a crown, a gully across the
// band) the band's face runs straight from the line to the edge, so it sits off the ground by up to
// that cross-section's sag over the half width, and the edge by up to its sag between two offsets
// (2.5, 6 or 15 m apart); the band's lift (0.1 % of the distance) covers part of it at range, which
// a guide line on the ground accepts.
float32 PathEdgeHeight(const MarkupRegionGround& ground, std::size_t i, float32 side, float32 halfWidth);

// The ground of a region whose outline `outline` (world, y = the ground under each sample: the
// drape) is closed when `closed`: its one ring. Interior lid points take `groundAt`, and so does an
// open outline's ground across its band (EdgeGround). Consecutive samples closer than a millimeter
// are merged.
MarkupRegionGround BuildMarkupRegionGround(std::span<const Mathematics::Vector3> outline, bool closed,
                                           const MarkupGroundSampler& groundAt);

// The ground of a region whose members cut or extend its area: the rings of `area` (its pieces,
// CombineRegionArea) sampled along the ground at most `spacing` meters apart (and at most
// kMaxRegionKnots samples a ring, as the drape caps a spline), each sample at `groundAt`, with a
// wall on every ring and the lid over the pieces with their holes. The label point and the
// crossing test are the base outline's (`outline`, closed, as BuildMarkupRegionGround takes it);
// a base outline that crosses itself gives BuildMarkupRegionGround's ground, the members not
// applied. An empty `area` (the excludes cover the base) has no rings.
MarkupRegionGround BuildMarkupRegionAreaGround(std::span<const Mathematics::Vector3> outline,
                                               std::span<const MarkupAreaPiece> area, float32 spacing,
                                               const MarkupGroundSampler& groundAt);

// One vertex of a region's display: a world position and the distance to the nearest rim edge
// (the wall's top edge, or the lid's outline) over the rim's width, so the glow's rim lights a
// band kMarkupRegionRimWidthFraction of ExtrudeHeight wide along the top edge. 16 bytes, the
// layout markup_glow_mesh.vert reads.
struct MarkupRegionVertex
{
    Mathematics::Vector3 Position{};
    float32 Rim = 0.0f;
};
static_assert(sizeof(MarkupRegionVertex) == 16, "markup_glow_mesh.vert reads 16-byte vertices");

// The rim band's width as a fraction of the extrusion height: a box face's rim width
// (kMarkupGlowBoxRimWidth of its smaller half extent) for a wall ExtrudeHeight tall.
inline constexpr float32 kMarkupRegionRimWidthFraction = 0.125f;

// A region's display at one extrusion height, built from its ground.
struct MarkupRegionMesh
{
    // A triangle list, the walls first (WallVertexCount), then the lid. Every triangle is
    // front-facing from outside: the walls face out of the area (into the hole on a hole's ring),
    // the lid up.
    std::vector<MarkupRegionVertex> Vertices;
    std::size_t WallVertexCount = 0;
    // Where the label stands: the label point at ground + ExtrudeHeight.
    Mathematics::Vector3 Label{};
};

// The walls (one quad per sample pair of every ring, from the ground to ground + `extrudeHeight`)
// and the lid (every lid point raised by `extrudeHeight`, so it follows the ground at the walls'
// height). An outline that is not closed has neither.
MarkupRegionMesh BuildMarkupRegionMesh(const MarkupRegionGround& ground, float32 extrudeHeight);

} // namespace GameEngine::MarkupECS
