#include "SplineGeometry/SplineStripBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace GameEngine::SplineGeometry
{
namespace
{

using V2 = Mathematics::Vector2;
using V3 = Mathematics::Vector3;
using V4 = Mathematics::Vector4;

// Smallest advance in metres that counts as a new station. Below it the two
// rings are the same ring: the quad between them has no area, and its
// zero-length longitudinal edge has no direction for a tangent frame.
constexpr float32 kMinStationAdvanceMetres = 1.0e-5f;

// Below this a scaled profile edge has collapsed (a taper pinching the width to
// nothing), and its direction is taken from the unscaled profile instead.
constexpr float32 kMinEdgeLength = 1.0e-6f;

[[nodiscard]] bool IsFinite(const V3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] V3 NormalizedOr(const V3& v, const V3& fallback)
{
    const float32 lenSq = V3::Dot(v, v);
    if (!std::isfinite(lenSq) || lenSq < 1.0e-12f)
        return fallback;
    return v * (1.0f / std::sqrt(lenSq));
}

[[nodiscard]] V2 PerpendicularOutward(const V2& edgeDir)
{
    // The single winding rule the profile documents: outward is the edge
    // direction rotated +90 degrees in (lateral, vertical).
    return V2{-edgeDir.y, edgeDir.x};
}

[[nodiscard]] V2 NormalizedOr2D(const V2& v, const V2& fallback)
{
    const float32 lenSq = v.x * v.x + v.y * v.y;
    if (!std::isfinite(lenSq) || lenSq < 1.0e-12f)
        return fallback;
    const float32 inv = 1.0f / std::sqrt(lenSq);
    return V2{v.x * inv, v.y * inv};
}

// One emitted vertex position within a ring. A profile point becomes two slots
// where it creases, and a closed profile gains one more so its V coordinate can
// reach the full perimeter at the seam without unwelding the geometry.
struct RingSlot
{
    uint32 PointIndex = 0;
    int32 EdgeA = -1;
    int32 EdgeB = -1; // -1 when this slot takes a single edge's normal
    bool SeamV = false;
};

struct ProfileTopology
{
    std::vector<RingSlot> Slots;
    // Profile edges as slot pairs; each becomes one quad per station interval.
    std::vector<std::pair<uint32, uint32>> Quads;
    uint32 EdgeCount = 0;
};

[[nodiscard]] ProfileTopology BuildTopology(const SplineProfile& profile)
{
    ProfileTopology topo;
    const uint32 pointCount = static_cast<uint32>(profile.Points.size());
    topo.EdgeCount = profile.Closed ? pointCount : pointCount - 1u;

    std::vector<int32> incomingSlot(pointCount, -1);
    std::vector<int32> outgoingSlot(pointCount, -1);

    for (uint32 j = 0; j < pointCount; ++j)
    {
        const int32 inEdge = profile.Closed
                                 ? static_cast<int32>((j + pointCount - 1u) % pointCount)
                                 : (j == 0u ? -1 : static_cast<int32>(j - 1u));
        const int32 outEdge = profile.Closed
                                  ? static_cast<int32>(j)
                                  : (j + 1u == pointCount ? -1 : static_cast<int32>(j));

        // An open profile's ends have one adjacent edge each, so they split
        // whether or not they were flagged: there is no second normal to
        // average with.
        const bool split = profile.Points[j].Hard != 0u || inEdge < 0 || outEdge < 0;
        if (!split)
        {
            const uint32 slot = static_cast<uint32>(topo.Slots.size());
            topo.Slots.push_back(RingSlot{j, inEdge, outEdge, false});
            incomingSlot[j] = static_cast<int32>(slot);
            outgoingSlot[j] = static_cast<int32>(slot);
            continue;
        }
        if (inEdge >= 0)
        {
            incomingSlot[j] = static_cast<int32>(topo.Slots.size());
            topo.Slots.push_back(RingSlot{j, inEdge, -1, false});
        }
        if (outEdge >= 0)
        {
            outgoingSlot[j] = static_cast<int32>(topo.Slots.size());
            topo.Slots.push_back(RingSlot{j, outEdge, -1, false});
        }
    }

    topo.Quads.reserve(topo.EdgeCount);
    for (uint32 e = 0; e < topo.EdgeCount; ++e)
    {
        const uint32 pointA = e;
        const uint32 pointB = (e + 1u) % pointCount;
        uint32 slotB = static_cast<uint32>(incomingSlot[pointB]);
        // The wrap edge lands back on point 0, whose V is 0 while the seam
        // needs the whole perimeter. A split point 0 (hard corner, true of
        // every closed preset) gives this edge sole ownership of the incoming
        // slot, which then carries the seam V itself; an unsplit point 0
        // shares its one slot with edge 0, so the seam gets a duplicate slot.
        // Either way the UV splits and the geometry stays welded.
        if (profile.Closed && e + 1u == topo.EdgeCount)
        {
            if (incomingSlot[0] != outgoingSlot[0])
            {
                topo.Slots[slotB].SeamV = true;
            }
            else
            {
                RingSlot seam = topo.Slots[slotB];
                seam.SeamV = true;
                slotB = static_cast<uint32>(topo.Slots.size());
                topo.Slots.push_back(seam);
            }
        }
        topo.Quads.emplace_back(static_cast<uint32>(outgoingSlot[pointA]), slotB);
    }
    return topo;
}

// Stations that are finite and strictly advance. A caller producing them from a
// draped polyline can repeat one where the ground doubled back, and a repeated
// station is not a shorter strip -- it is a ring with no interval.
//
// A corner's Crease and Pivot rings are the exception: they stand at their
// corner's Distance by design, and take it exactly, so every ring of the corner
// carries one U. With nothing before it such a ring simply opens the strip.
[[nodiscard]] std::vector<SplineStripStation> FilterStations(
    std::span<const SplineStripStation> stations, uint32& outDropped)
{
    std::vector<SplineStripStation> kept;
    kept.reserve(stations.size());
    for (const SplineStripStation& s : stations)
    {
        const bool usable = IsFinite(s.Position) && IsFinite(s.Right) && IsFinite(s.Up) &&
                            IsFinite(s.Forward) && std::isfinite(s.Distance) &&
                            std::isfinite(s.HalfWidthLeft) && std::isfinite(s.HalfWidthRight) &&
                            std::isfinite(s.RollRadians) && IsFinite(s.MitreRight) &&
                            std::isfinite(s.GroundOffset) && std::isfinite(s.TopOffset) &&
                            std::isfinite(s.FaceTurnLeft) && std::isfinite(s.TopArcAllowance) &&
                            std::isfinite(s.FaceTurnRight);
        if (!usable)
            continue;
        if (kept.empty())
        {
            kept.push_back(s);
            kept.back().Join = SplineStationJoin::Advance;
            continue;
        }
        if (s.Join != SplineStationJoin::Advance)
        {
            kept.push_back(s);
            kept.back().Distance = kept[kept.size() - 2u].Distance;
            continue;
        }
        if (s.Distance <= kept.back().Distance + kMinStationAdvanceMetres)
            continue;
        kept.push_back(s);
    }
    outDropped = static_cast<uint32>(stations.size() - kept.size());
    return kept;
}

// A triangle counts as folded only when it faces more than about 95 degrees away
// from its vertices' normals. Paths and roads carry triangles facing just past 90
// degrees from their normals; the margin leaves those split on the default
// diagonal, which keeps paths and roads byte-identical (SplineSweepParity).
constexpr float32 kFoldedCosine = -0.1f;

// Whether triangle (a, b, c) faces the way its vertices' normals do: false only
// for a triangle clearly folded back through itself.
[[nodiscard]] bool FacesItsNormals(const SplineVertex& a, const SplineVertex& b,
                                   const SplineVertex& c)
{
    const V3 geometric = V3::Cross(b.Position - a.Position, c.Position - a.Position);
    const V3 normals = a.Normal + b.Normal + c.Normal;
    const float32 area = std::sqrt(V3::Dot(geometric, geometric));
    const float32 normalLength = std::sqrt(V3::Dot(normals, normals));
    if (!(area > 0.0f) || !(normalLength > 0.0f))
        return true; // a degenerate triangle has no facing to judge
    return V3::Dot(geometric, normals) >= kFoldedCosine * area * normalLength;
}

// Append one band quad (a, b on this ring; d, c on the next, d beside a) as two
// triangles in the profile's outward winding. The b-d diagonal is the default;
// a quad that is not convex — the top where a corner's inside narrows toward
// its bounded mitre point — folds one of those triangles back, and is split on
// the a-c diagonal instead when that one does not.
void AppendBandQuad(std::vector<uint32>& indices, const std::vector<SplineVertex>& vertices,
                    uint32 a, uint32 b, uint32 c, uint32 d)
{
    const bool defaultHolds = FacesItsNormals(vertices[a], vertices[d], vertices[b]) &&
                              FacesItsNormals(vertices[b], vertices[d], vertices[c]);
    const bool otherHolds = FacesItsNormals(vertices[a], vertices[d], vertices[c]) &&
                            FacesItsNormals(vertices[a], vertices[c], vertices[b]);
    if (!defaultHolds && otherHolds)
    {
        indices.insert(indices.end(), {a, d, c, a, c, b});
        return;
    }
    indices.insert(indices.end(), {a, d, b, b, d, c});
}

// Width scale for one station, per side of travel, shared by the ring sweep,
// the risers and the caps: a riser or cap must measure its UVs on the same
// scaled cross-section its ring was built from.
//
// A profile point picks its side by the SIGN of its lateral coordinate, and a
// point on the centreline (x == 0) scales to zero either way. When the two
// half-widths are equal both sides resolve to one value and every vertex is the
// single-half-width arithmetic unchanged.
struct StationScale
{
    float32 Left = 1.0f;
    float32 Right = 1.0f;
    bool ScaleVertical = false;
};

[[nodiscard]] StationScale ScaleOfStation(const SplineStripStation& station,
                                          const SplineProfile& profile, SplineProfileScale mode)
{
    StationScale scale;
    if (mode != SplineProfileScale::None)
    {
        const float32 left = std::max(0.0f, station.HalfWidthLeft) / profile.NominalHalfWidth;
        const float32 right = std::max(0.0f, station.HalfWidthRight) / profile.NominalHalfWidth;
        scale.Left = std::isfinite(left) ? left : 1.0f;
        scale.Right = std::isfinite(right) ? right : 1.0f;
        scale.ScaleVertical = mode == SplineProfileScale::Uniform;
    }
    return scale;
}

// The lateral and vertical multipliers a profile point takes, given the side
// its lateral coordinate falls on.
[[nodiscard]] std::pair<float32, float32> ScaleForPoint(const StationScale& scale, float32 lateral)
{
    const float32 s = (lateral < 0.0f) ? scale.Left : scale.Right;
    return {s, scale.ScaleVertical ? s : 1.0f};
}

// How a riser or cap measures its UVs: lateral metres across, and V either the
// height above a datum (a wall's side faces) or the profile's own height.
struct EndFaceUV
{
    float32 TilesPerMetreV = 1.0f;
    bool HeightV = false;
    float32 HeightVDatumMetres = 0.0f;
};

// Close the step between ring `earlier` and the Crease ring after it, whose
// raised (non-grounded) points stand at another height: one polygon in the
// crease plane, the earlier ring's raised points in profile order, then the
// later ring's in reverse, facing the lower side, from which it is seen.
void EmitRiser(SplineStripMesh& mesh, const SplineProfile& profile, std::span<const uint32> raised,
               std::span<const uint32> firstSlotOfPoint, uint32 earlier,
               const SplineStripStation& earlierStation, const SplineStripStation& laterStation,
               SplineProfileScale widthScale, const EndFaceUV& uvs)
{
    const uint32 ringVertexCount = mesh.RingVertexCount;
    const float32 rise = laterStation.TopOffset - earlierStation.TopOffset;
    const V3 ahead = NormalizedOr(NormalizedOr(earlierStation.Forward, V3{0.0f, 0.0f, 1.0f}) +
                                      NormalizedOr(laterStation.Forward, V3{0.0f, 0.0f, 1.0f}),
                                  NormalizedOr(laterStation.Forward, V3{0.0f, 0.0f, 1.0f}));
    const auto ringPosition = [&](uint32 ring, uint32 point) -> V3
    {
        return mesh.Vertices[static_cast<size_t>(ring) * ringVertexCount + firstSlotOfPoint[point]]
            .Position;
    };
    // The two rings differ only in their tops, which move along the Up they
    // share, so the riser is the plane through the earlier ring's raised points
    // and that Up. Stepping up it faces back along the run toward the lower
    // stretch before it; stepping down, ahead.
    const V3 lateral =
        NormalizedOr(ringPosition(earlier, raised.back()) - ringPosition(earlier, raised.front()),
                     NormalizedOr(laterStation.Right, V3{1.0f, 0.0f, 0.0f}));
    V3 riserNormal = NormalizedOr(V3::Cross(lateral, laterStation.Up), ahead);
    if ((V3::Dot(riserNormal, ahead) > 0.0f) == (rise > 0.0f))
        riserNormal = riserNormal * -1.0f;
    // U runs along the lateral axis and V up the riser.
    const float32 handedness =
        V3::Dot(V3::Cross(riserNormal, lateral), laterStation.Up) >= 0.0f ? 1.0f : -1.0f;
    const StationScale scale = ScaleOfStation(laterStation, profile, widthScale);

    const uint32 base = static_cast<uint32>(mesh.Vertices.size());
    const auto emitCorner = [&](uint32 ring, uint32 point)
    {
        SplineVertex vertex;
        vertex.Position = ringPosition(ring, point);
        vertex.Normal = riserNormal;
        const auto [lateralScale, verticalScale] =
            ScaleForPoint(scale, profile.Points[point].Position.x);
        const float32 riserV = uvs.HeightV ? vertex.Position.y - uvs.HeightVDatumMetres
                                           : profile.Points[point].Position.y * verticalScale;
        vertex.UV = V2{profile.Points[point].Position.x * lateralScale * uvs.TilesPerMetreV,
                       riserV * uvs.TilesPerMetreV};
        vertex.Tangent = V4{lateral.x, lateral.y, lateral.z, handedness};
        mesh.Vertices.push_back(vertex);
    };
    for (const uint32 point : raised)
        emitCorner(earlier, point);
    for (auto it = raised.rbegin(); it != raised.rend(); ++it)
        emitCorner(earlier + 1u, *it);

    const uint32 corners = static_cast<uint32>(raised.size()) * 2u;
    const V3 fanNormal = V3::Cross(mesh.Vertices[base + 1u].Position - mesh.Vertices[base].Position,
                                   mesh.Vertices[base + 2u].Position - mesh.Vertices[base].Position);
    const bool flip = V3::Dot(fanNormal, riserNormal) < 0.0f;
    for (uint32 k = 1u; k + 1u < corners; ++k)
    {
        mesh.Indices.push_back(base);
        mesh.Indices.push_back(base + (flip ? k + 1u : k));
        mesh.Indices.push_back(base + (flip ? k : k + 1u));
    }
}

// The U density at which `length` metres span a whole number of tiles, nearest
// to `tilesPerMetre`.
[[nodiscard]] float32 WholeTileDensity(float32 length, float32 tilesPerMetre)
{
    if (!(length > kMinEdgeLength) || !(tilesPerMetre > 0.0f))
        return tilesPerMetre;
    return std::max(1.0f, std::round(length * tilesPerMetre)) / length;
}

} // namespace

SplineStripMesh BuildSplineStrip(const SplineProfile& profile,
                                 std::span<const SplineStripStation> stations,
                                 const SplineStripParams& params)
{
    SplineStripMesh mesh;
    if (!profile.IsValid())
        return mesh;

    const std::vector<SplineStripStation> walk = FilterStations(stations, mesh.DroppedStations);
    if (walk.size() < 2u)
        return mesh;

    const ProfileTopology topo = BuildTopology(profile);
    if (topo.Slots.empty() || topo.Quads.empty())
        return mesh;

    const uint32 pointCount = static_cast<uint32>(profile.Points.size());
    const uint32 ringVertexCount = static_cast<uint32>(topo.Slots.size());
    const uint32 ringCount = static_cast<uint32>(walk.size());
    mesh.RingVertexCount = ringVertexCount;
    mesh.RingCount = ringCount;

    // A finite UOriginMetres puts every chunk of a split run on one shared U
    // axis; the default rebases to this span's own front. The closed-loop snap
    // below always rounds against THIS span's length — a chunking caller snaps
    // once over the full run instead and leaves SnapUForClosedLoop off.
    const float32 frontDistance = walk.front().Distance;
    const float32 originDistance =
        std::isfinite(params.UOriginMetres) ? params.UOriginMetres : frontDistance;
    const float32 totalLength = walk.back().Distance - frontDistance;

    float32 tilesPerMetreU = std::isfinite(params.TilesPerMetreU) ? params.TilesPerMetreU : 1.0f;
    const float32 tilesPerMetreV =
        std::isfinite(params.TilesPerMetreV) ? params.TilesPerMetreV : 1.0f;
    const bool heightV = std::isfinite(params.HeightVDatumMetres);
    const bool faceSnap = std::isfinite(params.LoopLengthMetres);
    if (params.SnapUForClosedLoop && totalLength > kMinEdgeLength && tilesPerMetreU > 0.0f)
    {
        // Round the run to a whole number of tiles. A loop's length is rarely
        // an integer count, and the sub-1% density change this costs is
        // invisible where the partial tile at the seam is not.
        const float32 wholeTiles = std::max(1.0f, std::round(totalLength * tilesPerMetreU));
        tilesPerMetreU = wholeTiles / totalLength;
    }

    mesh.Vertices.resize(static_cast<size_t>(ringVertexCount) * ringCount);

    // Scratch reused per station: the scaled cross-section, its edge normals
    // and its cumulative perimeter all change when the width channel does.
    std::vector<V2> scaledPoints(pointCount);
    std::vector<V2> edgeNormals(topo.EdgeCount);
    std::vector<float32> pointV(pointCount, 0.0f);

    for (uint32 r = 0; r < ringCount; ++r)
    {
        const SplineStripStation& station = walk[r];
        const StationScale scale = ScaleOfStation(station, profile, params.WidthScale);

        for (uint32 j = 0; j < pointCount; ++j)
        {
            const auto [lateralScale, verticalScale] =
                ScaleForPoint(scale, profile.Points[j].Position.x);
            scaledPoints[j] = V2{profile.Points[j].Position.x * lateralScale,
                                 profile.Points[j].Position.y * verticalScale};
        }

        float32 perimeter = 0.0f;
        for (uint32 e = 0; e < topo.EdgeCount; ++e)
        {
            const uint32 a = e;
            const uint32 b = (e + 1u) % pointCount;
            const V2 delta{scaledPoints[b].x - scaledPoints[a].x,
                           scaledPoints[b].y - scaledPoints[a].y};
            const float32 length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
            // A pinched-to-nothing edge keeps the unscaled profile's direction
            // so its face still has a normal to shade with.
            const V2 unscaled{profile.Points[b].Position.x - profile.Points[a].Position.x,
                              profile.Points[b].Position.y - profile.Points[a].Position.y};
            const V2 dir = (length > kMinEdgeLength)
                               ? V2{delta.x / length, delta.y / length}
                               : NormalizedOr2D(unscaled, V2{1.0f, 0.0f});
            edgeNormals[e] = NormalizedOr2D(PerpendicularOutward(dir), V2{0.0f, 1.0f});

            if (b != 0u)
                pointV[b] = pointV[a] + length;
            perimeter = pointV[a] + length;
        }

        // Roll the basis about Forward, matching the evaluator's own convention
        // so the channel means one thing across the spline stack.
        V3 right = station.Right;
        V3 up = station.Up;
        V3 mitreRight = station.MitreRight;
        if (std::abs(station.RollRadians) > 0.0f)
        {
            const float32 c = std::cos(station.RollRadians);
            const float32 s = std::sin(station.RollRadians);
            const V3 rolledRight = right * c + up * s;
            const V3 rolledUp = up * c - right * s;
            mitreRight = mitreRight * c + up * (s * std::sqrt(V3::Dot(mitreRight, mitreRight)));
            right = rolledRight;
            up = rolledUp;
        }
        const V3 forward = NormalizedOr(station.Forward, V3{0.0f, 0.0f, 1.0f});
        const float32 u = (station.Distance - originDistance) * tilesPerMetreU;

        for (uint32 slotIndex = 0; slotIndex < ringVertexCount; ++slotIndex)
        {
            const RingSlot& slot = topo.Slots[slotIndex];
            const V2& local = scaledPoints[slot.PointIndex];

            V2 normal2D = edgeNormals[static_cast<uint32>(slot.EdgeA)];
            if (slot.EdgeB >= 0)
            {
                const V2& other = edgeNormals[static_cast<uint32>(slot.EdgeB)];
                // Two exactly opposed edges (a fold back on itself) average to
                // zero and name no direction; one side's normal is then the
                // only answer available.
                normal2D = NormalizedOr2D(V2{normal2D.x + other.x, normal2D.y + other.y}, normal2D);
            }

            SplineVertex& vertex =
                mesh.Vertices[static_cast<size_t>(r) * ringVertexCount + slotIndex];
            const uint8 side = (local.x < 0.0f) ? kSplineMitreLeftSide : kSplineMitreRightSide;
            const V3& lateralAxis = (station.MitreSides & side) != 0u ? mitreRight : right;
            vertex.Position = station.Position + lateralAxis * local.x + up * local.y;
            // A side face (its edge normal more lateral than vertical) runs its
            // own length along the run; the top and bottom take the centreline's,
            // projected onto the ring's forward on a corner ring (below).
            const bool sideFace = std::abs(normal2D.x) > std::abs(normal2D.y);
            if (profile.Points[slot.PointIndex].Grounded != 0u && station.GroundOffset != 0.0f)
                vertex.Position = vertex.Position + up * station.GroundOffset;
            if (profile.Points[slot.PointIndex].Grounded == 0u && station.TopOffset != 0.0f)
                vertex.Position = vertex.Position + up * station.TopOffset;
            vertex.Normal =
                NormalizedOr(right * normal2D.x + up * normal2D.y, V3{0.0f, 1.0f, 0.0f});
            float32 v = slot.SeamV ? perimeter : pointV[slot.PointIndex];
            // U runs along Forward, so the tangent IS Forward; V runs along the
            // profile edge, which is cross(Normal, Forward), so the handedness
            // is +1 under the winding rule above.
            float32 handedness = 1.0f;
            if (heightV && std::abs(normal2D.x) > std::abs(normal2D.y))
            {
                // A side face measures V up from the datum instead. The winding
                // runs the right-hand face's edge DOWN, so there V climbs against
                // the edge and the handedness flips.
                v = vertex.Position.y - params.HeightVDatumMetres;
                handedness = normal2D.x > 0.0f ? -1.0f : 1.0f;
            }
            float32 faceU = u;
            const float32 faceTurn =
                !sideFace ? 0.0f
                          : (side == kSplineMitreLeftSide ? station.FaceTurnLeft : station.FaceTurnRight);
            // The top and bottom take the centreline U plus the arc the top has
            // swept around round corners so far (TopArcAllowance), and on a
            // corner ring that U projected along the ring's forward:
            // U = s + allowance + dot(offset, forward). A mitre's two rings then
            // give each leg's joints square to that leg, meeting on the mitre
            // line with a kink; a round fan's outer edge runs its arc at true
            // scale while its inside pinches on the inner mitre point; and the
            // bands into and out of every corner run at true scale on both edges.
            // Off corners the offset is square to forward and projects to nothing.
            const float32 projected =
                sideFace
                    ? 0.0f
                    : (station.TopArcAllowance +
                       (station.MitreSides == 0u ? 0.0f : V3::Dot(lateralAxis * local.x, forward))) *
                          params.LateralMetresPerUnit;
            if (faceTurn != 0.0f || faceSnap || projected != 0.0f)
            {
                const float32 lateral = std::abs(local.x) * params.LateralMetresPerUnit;
                const float32 loopTurn =
                    !sideFace ? 0.0f
                              : (side == kSplineMitreLeftSide ? params.LoopFaceTurnLeft
                                                              : params.LoopFaceTurnRight);
                const float32 loopAllowance =
                    sideFace ? 0.0f : params.LoopTopArcAllowance * params.LateralMetresPerUnit;
                const float32 density =
                    faceSnap ? WholeTileDensity(params.LoopLengthMetres + lateral * loopTurn +
                                                    loopAllowance,
                                                tilesPerMetreU)
                             : tilesPerMetreU;
                faceU = (station.Distance - originDistance + lateral * faceTurn + projected) * density;
            }
            vertex.UV = V2{faceU, v * tilesPerMetreV};
            vertex.Tangent = V4{forward.x, forward.y, forward.z, handedness};
        }
    }

    // Stitch each ring to the next. Ring r's vertices are referenced by the quad
    // behind it AND the quad ahead of it -- that shared reference IS the weld,
    // and it is why the strip has no seam to misalign and no end to bury.
    mesh.Indices.reserve(static_cast<size_t>(topo.Quads.size()) * (ringCount - 1u) * 6u);
    for (uint32 r = 0; r + 1u < ringCount; ++r)
    {
        // A crease repeats its ring's positions: a band to it would have no area.
        if (walk[r + 1u].Join == SplineStationJoin::Crease)
            continue;
        const uint32 base = r * ringVertexCount;
        const uint32 next = (r + 1u) * ringVertexCount;
        for (const auto& [slotA, slotB] : topo.Quads)
        {
            const uint32 a = base + slotA;
            const uint32 b = base + slotB;
            const uint32 c = next + slotB;
            const uint32 d = next + slotA;
            // cross(second - first, third - first) lands on the profile's
            // outward normal, which is also the CCW winding a Jolt mesh shape
            // expects.
            AppendBandQuad(mesh.Indices, mesh.Vertices, a, b, c, d);
        }
    }

    // Any slot of a point carries the same position, so one lookup per point
    // reuses the ring's own vertices: a riser or cap built from them cannot
    // drift off the strip.
    std::vector<uint32> firstSlotOfPoint(pointCount, 0u);
    for (uint32 slotIndex = ringVertexCount; slotIndex-- > 0u;)
        firstSlotOfPoint[topo.Slots[slotIndex].PointIndex] = slotIndex;

    // A step: a Crease ring whose top stands at another height than the ring
    // before it (EmitRiser). Only a closed profile has a top to step.
    if (profile.Closed)
    {
        std::vector<uint32> raised;
        for (uint32 j = 0; j < pointCount; ++j)
        {
            if (profile.Points[j].Grounded == 0u)
                raised.push_back(j);
        }
        const EndFaceUV uvs{tilesPerMetreV, heightV, params.HeightVDatumMetres};
        for (uint32 r = 0; r + 1u < ringCount && raised.size() >= 2u; ++r)
        {
            const float32 rise = walk[r + 1u].TopOffset - walk[r].TopOffset;
            if (walk[r + 1u].Join != SplineStationJoin::Crease || std::abs(rise) <= kMinEdgeLength)
                continue;
            EmitRiser(mesh, profile, raised, firstSlotOfPoint, r, walk[r], walk[r + 1u],
                      params.WidthScale, uvs);
        }
    }

    // End caps close a solid cross-section. An open strip has no polygon to
    // triangulate, so capping it is not a toggle with a wrong setting -- it
    // simply does not apply.
    if (profile.Closed && params.GenerateCaps && pointCount >= 3u)
    {
        const auto emitCap = [&](uint32 ring, bool front)
        {
            const uint32 base = static_cast<uint32>(mesh.Vertices.size());
            const SplineStripStation& station = walk[ring];
            const StationScale scale = ScaleOfStation(station, profile, params.WidthScale);
            const V3 forward = NormalizedOr(station.Forward, V3{0.0f, 0.0f, 1.0f});
            const V3 capNormal = front ? forward * -1.0f : forward;

            for (uint32 j = 0; j < pointCount; ++j)
            {
                const size_t sourceIndex =
                    static_cast<size_t>(ring) * ringVertexCount + firstSlotOfPoint[j];
                SplineVertex vertex;
                vertex.Position = mesh.Vertices[sourceIndex].Position;
                vertex.Normal = capNormal;
                // The cap's UVs are its own cross-section in world metres — the
                // SCALED cross-section, since the width channel has already
                // sized this ring — so a capped end tiles at the same density
                // as the sides.
                const auto [lateralScale, verticalScale] =
                    ScaleForPoint(scale, profile.Points[j].Position.x);
                const float32 capV =
                    heightV ? vertex.Position.y - params.HeightVDatumMetres
                            : profile.Points[j].Position.y * verticalScale;
                vertex.UV = V2{profile.Points[j].Position.x * lateralScale * tilesPerMetreV,
                               capV * tilesPerMetreV};
                vertex.Tangent = V4{1.0f, 0.0f, 0.0f, 1.0f};
                mesh.Vertices.push_back(vertex);
            }

            // Fan from point 0. Exact for every preset, which are convex by
            // construction; a non-convex Custom profile gets a fan too, which
            // is wrong rather than crashing and is why ear clipping is named.
            for (uint32 j = 1u; j + 1u < pointCount; ++j)
            {
                mesh.Indices.push_back(base);
                if (front)
                {
                    mesh.Indices.push_back(base + j);
                    mesh.Indices.push_back(base + j + 1u);
                }
                else
                {
                    mesh.Indices.push_back(base + j + 1u);
                    mesh.Indices.push_back(base + j);
                }
            }
        };
        emitCap(0u, /*front=*/true);
        emitCap(ringCount - 1u, /*front=*/false);
    }

    V3 minBounds{std::numeric_limits<float32>::max(), std::numeric_limits<float32>::max(),
                 std::numeric_limits<float32>::max()};
    V3 maxBounds{std::numeric_limits<float32>::lowest(), std::numeric_limits<float32>::lowest(),
                 std::numeric_limits<float32>::lowest()};
    for (const SplineVertex& v : mesh.Vertices)
    {
        minBounds.x = std::min(minBounds.x, v.Position.x);
        minBounds.y = std::min(minBounds.y, v.Position.y);
        minBounds.z = std::min(minBounds.z, v.Position.z);
        maxBounds.x = std::max(maxBounds.x, v.Position.x);
        maxBounds.y = std::max(maxBounds.y, v.Position.y);
        maxBounds.z = std::max(maxBounds.z, v.Position.z);
    }
    mesh.MinBounds = minBounds;
    mesh.MaxBounds = maxBounds;
    return mesh;
}

} // namespace GameEngine::SplineGeometry
