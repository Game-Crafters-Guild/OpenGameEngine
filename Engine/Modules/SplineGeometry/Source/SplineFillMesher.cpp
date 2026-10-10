#include "SplineGeometry/SplineFillMesher.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineGeometry
{
namespace
{

using V2 = Mathematics::Vector2;
using V3 = Mathematics::Vector3;
using V4 = Mathematics::Vector4;

constexpr uint32 kNoVertex = 0xFFFFFFFFu;

// Where along an edge the region boundary sits.
//
// `Inside` is a continuous metre-valued field (the ground, corridor and
// containment constraints intersected), so its zero is the shoreline. The two
// endpoints of any boundary edge bracket that zero: a corner is in the region
// only if the 4-connected flood reached it, and two 4-adjacent corners that both
// have Inside > 0 are necessarily in the SAME component -- so "one endpoint in,
// one out" implies the outside one has Inside <= 0.
float32 CrossingFraction(float32 insideA, float32 insideB)
{
    const float32 denominator = insideA - insideB;
    if (!std::isfinite(denominator) || !(denominator > 0.0f))
        return 0.5f; // only reachable via an unreadable corner, which the caller refuses
    return std::clamp(insideA / denominator, 0.0f, 1.0f);
}

} // namespace

std::vector<SplineFillChunkRange> CarveFillChunks(const SplineGroundGrid& ground,
                                                  uint32 cellsPerChunk)
{
    std::vector<SplineFillChunkRange> chunks;
    if (!ground.IsValid() || cellsPerChunk == 0u)
        return chunks;

    for (uint32 z = 0; z + 1u < ground.CountZ; z += cellsPerChunk)
    {
        for (uint32 x = 0; x + 1u < ground.CountX; x += cellsPerChunk)
        {
            SplineFillChunkRange range;
            range.MinCornerX = x;
            range.MinCornerZ = z;
            // One past the last corner: the block's cells are bounded by corners
            // it SHARES with the next block, so both sides see the same lattice
            // line and evaluate the same field there.
            range.MaxCornerX = std::min(x + cellsPerChunk + 1u, ground.CountX);
            range.MaxCornerZ = std::min(z + cellsPerChunk + 1u, ground.CountZ);
            chunks.push_back(range);
        }
    }
    return chunks;
}

SplineFillMesh BuildSplineFillMesh(const SplineFillResult& field, const SplineGroundGrid& ground,
                                   std::span<const SplineStripStation> stations,
                                   const SplineFillChunkRange& range,
                                   const SplineFillMeshParams& params)
{
    SplineFillMesh mesh;
    if (!ground.IsValid() || stations.empty() ||
        field.Corners.size() != static_cast<size_t>(ground.CornerCount()))
        return mesh;
    if (range.MaxCornerX <= range.MinCornerX + 1u || range.MaxCornerZ <= range.MinCornerZ + 1u)
        return mesh;
    if (range.MaxCornerX > ground.CountX || range.MaxCornerZ > ground.CountZ)
        return mesh;

    const uint32 nx = range.MaxCornerX - range.MinCornerX;
    const uint32 nz = range.MaxCornerZ - range.MinCornerZ;
    const float32 drop = std::max(0.0f, params.EdgeDrop);

    // Dense slot arrays over the block: one per corner, one per horizontal
    // lattice edge, one per vertical lattice edge. A crossing belongs to its
    // EDGE, so the two cells sharing that edge index the same vertex and cannot
    // disagree about where the shoreline is -- that is what makes T-junctions
    // impossible rather than repaired.
    std::vector<uint32> cornerSlot(static_cast<size_t>(nx) * nz, kNoVertex);
    std::vector<uint32> edgeXSlot(static_cast<size_t>(nx) * nz, kNoVertex); // (cx,cz)->(cx+1,cz)
    std::vector<uint32> edgeZSlot(static_cast<size_t>(nx) * nz, kNoVertex); // (cx,cz)->(cx,cz+1)

    const auto localIndex = [&](uint32 cx, uint32 cz)
    {
        return static_cast<size_t>(cz - range.MinCornerZ) * nx + (cx - range.MinCornerX);
    };

    const auto emit = [&](const V3& position, uint32 station, const V2& uv)
    {
        const uint32 governing =
            station < stations.size() ? station : static_cast<uint32>(stations.size() - 1u);
        const SplineStripStation& frame = stations[governing];

        SplineVertex vertex;
        vertex.Position = position;
        // The surface is level across the flow and slopes along it at the
        // centreline's own grade, which is exactly the station frame's up.
        vertex.Normal = frame.Up;
        // (arc along the flow, still-water depth), both in world metres.
        vertex.UV = uv;
        // U runs along the flow, so the tangent IS Forward -- the same frame the
        // swept ribbon builds, so a filled river and a channel one agree about
        // it. V is a DEPTH and names no direction across the surface, so the
        // bitangent is the sweep's convention, cross(Normal, Tangent) at
        // handedness +1, rather than something derived from V.
        V3 tangent = frame.Forward;
        const float32 length = std::sqrt(V3::Dot(tangent, tangent));
        tangent = length > 1.0e-6f ? tangent * (1.0f / length) : V3{1.0f, 0.0f, 0.0f};
        vertex.Tangent = V4{tangent.x, tangent.y, tangent.z, 1.0f};

        mesh.Vertices.push_back(vertex);
        return static_cast<uint32>(mesh.Vertices.size() - 1u);
    };

    const auto cornerVertex = [&](uint32 cx, uint32 cz)
    {
        uint32& slot = cornerSlot[localIndex(cx, cz)];
        if (slot != kNoVertex)
            return slot;
        const uint32 index = ground.Index(cx, cz);
        const SplineFillCorner& corner = field.Corners[index];
        // Interior vertices sit at the WATER surface, not at the bed: the
        // distance between them is the depth, and that distance IS uv0.y.
        slot = emit(V3{ground.WorldX(cx), corner.Waterline, ground.WorldZ(cz)}, corner.Station,
                    V2{corner.ArcDistance, corner.Waterline - ground.Heights[index]});
        return slot;
    };

    // A shoreline vertex, owned by the lattice edge it crosses. Everything it
    // reads is a pure function of that edge's two corners, so a neighbouring
    // CHUNK -- which recomputes rather than shares it -- lands on bit-identical
    // values.
    const auto edgeVertex = [&](uint32 cx, uint32 cz, bool alongX)
    {
        uint32& slot = (alongX ? edgeXSlot : edgeZSlot)[localIndex(cx, cz)];
        if (slot != kNoVertex)
            return slot;

        const uint32 bx = alongX ? cx + 1u : cx;
        const uint32 bz = alongX ? cz : cz + 1u;
        const SplineFillCorner& a = field.Corners[ground.Index(cx, cz)];
        const SplineFillCorner& b = field.Corners[ground.Index(bx, bz)];

        // Order the interpolation from the corner that is further inside, so
        // both cells sharing the edge -- and both chunks -- compute the same t
        // from the same operands in the same order.
        const bool aFirst = a.Inside >= b.Inside;
        const SplineFillCorner& first = aFirst ? a : b;
        const SplineFillCorner& second = aFirst ? b : a;
        const float32 firstX = ground.WorldX(aFirst ? cx : bx);
        const float32 firstZ = ground.WorldZ(aFirst ? cz : bz);
        const float32 secondX = ground.WorldX(aFirst ? bx : cx);
        const float32 secondZ = ground.WorldZ(aFirst ? bz : cz);
        const float32 firstGround = ground.Heights[ground.Index(aFirst ? cx : bx, aFirst ? cz : bz)];
        const float32 secondGround =
            ground.Heights[ground.Index(aFirst ? bx : cx, aFirst ? bz : cz)];

        const float32 t = CrossingFraction(first.Inside, second.Inside);
        const float32 waterline = first.Waterline + (second.Waterline - first.Waterline) * t;

        // Depth and arc interpolate on the SAME t as the position, so both stay
        // continuous along the shoreline and stay a pure function of this edge's
        // two corners -- which is what keeps a neighbouring chunk, recomputing
        // rather than sharing, bit-identical here.
        //
        // Wherever the ground decides the crossing, Inside IS depth + EdgeDrop,
        // so this lands on exactly -EdgeDrop: a shader clamping depth at zero
        // puts its shoreline precisely on the waterline. Where the corridor or
        // the containment term decided it instead the value only RISES, and the
        // bound is global: Inside is a minimum over the three terms, so
        // Inside <= depth + EdgeDrop at both ends, and interpolating on t gives
        // uv0.y >= -EdgeDrop EVERYWHERE. Such a rim is a limit of the search
        // rather than a bank, and it draws less shore, never more.
        const float32 firstDepth = first.Waterline - firstGround;
        const float32 secondDepth = second.Waterline - secondGround;

        // The rim is BURIED rather than coplanar: the contour already sits
        // laterally past the waterline crossing (the field's iso is at
        // phi = -EdgeDrop), and the vertex drops the same distance again. Only
        // the POSITION drops; uv0.y is the water's depth here, not this vertex's
        // distance below the bed.
        slot = emit(V3{firstX + (secondX - firstX) * t, waterline - drop,
                       firstZ + (secondZ - firstZ) * t},
                    first.Station,
                    V2{first.ArcDistance + (second.ArcDistance - first.ArcDistance) * t,
                       firstDepth + (secondDepth - firstDepth) * t});
        return slot;
    };

    // One cell's inside-polygon: the boundary walk in order, remembering which
    // entries are lattice corners so the saddle split can find its ears without
    // searching for them.
    struct PolygonEntry
    {
        uint32 Slot = kNoVertex;
        bool IsCorner = false;
    };
    std::vector<PolygonEntry> polygon;
    polygon.reserve(8u);

    for (uint32 cz = range.MinCornerZ; cz + 1u < range.MaxCornerZ; ++cz)
    {
        for (uint32 cx = range.MinCornerX; cx + 1u < range.MaxCornerX; ++cx)
        {
            const SplineFillCorner& c00 = field.Corners[ground.Index(cx, cz)];
            const SplineFillCorner& c01 = field.Corners[ground.Index(cx, cz + 1u)];
            const SplineFillCorner& c11 = field.Corners[ground.Index(cx + 1u, cz + 1u)];
            const SplineFillCorner& c10 = field.Corners[ground.Index(cx + 1u, cz)];

            const bool w00 = c00.Wet != 0u;
            const bool w01 = c01.Wet != 0u;
            const bool w11 = c11.Wet != 0u;
            const bool w10 = c10.Wet != 0u;
            const uint32 wetCount = (w00 ? 1u : 0u) + (w01 ? 1u : 0u) + (w11 ? 1u : 0u) +
                                    (w10 ? 1u : 0u);
            if (wetCount == 0u)
                continue;

            // Walk the cell boundary c00 -> c01 -> c11 -> c10, taking each wet
            // corner and each crossing of an edge whose endpoints disagree. The
            // order is what makes cross(p1 - p0, p2 - p0) point UP, the same
            // winding rule the sweep states for its own quads.
            polygon.clear();
            const auto step = [&](bool wetHere, uint32 cornerX, uint32 cornerZ, bool wetNext,
                                  uint32 edgeX, uint32 edgeZ, bool alongX)
            {
                if (wetHere)
                    polygon.push_back({cornerVertex(cornerX, cornerZ), true});
                if (wetHere != wetNext)
                    polygon.push_back({edgeVertex(edgeX, edgeZ, alongX), false});
            };
            step(w00, cx, cz, w01, cx, cz, /*alongX=*/false);
            step(w01, cx, cz + 1u, w11, cx, cz + 1u, /*alongX=*/true);
            step(w11, cx + 1u, cz + 1u, w10, cx + 1u, cz, /*alongX=*/false);
            step(w10, cx + 1u, cz, w00, cx, cz, /*alongX=*/true);

            if (polygon.size() < 3u)
                continue;

            // The two ambiguous cases: the wet corners are diagonal, and the
            // boundary walk has just joined them through the cell. The mean of
            // the four corner values -- the standard asymptotic decider -- says
            // whether the region really passes through the middle. This chooses
            // only how THIS cell's interior connects; the edge crossings above
            // are untouched, so no neighbour can disagree and no crack opens.
            const bool diagonal = wetCount == 2u && (w00 == w11) && (w01 == w10) && (w00 != w01);
            if (diagonal)
            {
                const float32 centre =
                    (c00.Inside + c01.Inside + c11.Inside + c10.Inside) * 0.25f;
                if (!(centre > 0.0f) && polygon.size() == 6u)
                {
                    // Separate: one triangle around each wet corner, between the
                    // two crossings that flank it.
                    for (size_t i = 0; i < polygon.size(); ++i)
                    {
                        if (!polygon[i].IsCorner)
                            continue;
                        mesh.Indices.push_back(
                            polygon[(i + polygon.size() - 1u) % polygon.size()].Slot);
                        mesh.Indices.push_back(polygon[i].Slot);
                        mesh.Indices.push_back(polygon[(i + 1u) % polygon.size()].Slot);
                    }
                    continue;
                }
            }

            // Every case's inside-polygon is convex -- a square, or a square with
            // one or two opposite corners cut off -- so a fan is a valid
            // triangulation and needs no ear clipping.
            for (size_t i = 1; i + 1u < polygon.size(); ++i)
            {
                mesh.Indices.push_back(polygon[0].Slot);
                mesh.Indices.push_back(polygon[i].Slot);
                mesh.Indices.push_back(polygon[i + 1u].Slot);
            }
        }
    }

    if (mesh.Vertices.empty() || mesh.Indices.empty())
    {
        mesh.Vertices.clear();
        mesh.Indices.clear();
        return mesh;
    }

    V3 minBounds = mesh.Vertices.front().Position;
    V3 maxBounds = minBounds;
    for (const SplineVertex& vertex : mesh.Vertices)
    {
        minBounds.x = std::min(minBounds.x, vertex.Position.x);
        minBounds.y = std::min(minBounds.y, vertex.Position.y);
        minBounds.z = std::min(minBounds.z, vertex.Position.z);
        maxBounds.x = std::max(maxBounds.x, vertex.Position.x);
        maxBounds.y = std::max(maxBounds.y, vertex.Position.y);
        maxBounds.z = std::max(maxBounds.z, vertex.Position.z);
    }
    mesh.MinBounds = minBounds;
    mesh.MaxBounds = maxBounds;
    return mesh;
}

} // namespace GameEngine::SplineGeometry
