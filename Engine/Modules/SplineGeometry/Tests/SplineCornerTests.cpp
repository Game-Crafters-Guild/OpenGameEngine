#include "SplineGeometry/SplineCorner.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "SplineWallWalk.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using namespace GameEngine::SplineGeometry::Testing;

// A Mitre's ring stands on the bisector scaled by 1 / cos(θ/2); without the
// scale a 90-degree corner is cos 45° = 0.707 of the wall's thickness. Measured
// across the wall, perpendicular to each leg, every ring at the corner is the
// full thickness, and no side face folds, on 0.6, 1 and 2 m walls.
TEST(SplineCorner, ALinearNinetyDegreeCornerKeepsTheWallsFullThickness)
{
    for (const float32 thickness : {0.6f, 1.0f, 2.0f})
    {
        Walk walk = WalkPolyline(Elbow(10.0f, 7.3f, 90.0f), 0.5f, thickness * 0.5f);
        const uint32 cornerIndex = walk.Corners[0].StationIndex;
        const V3 corner = walk.Stations[cornerIndex].Position;
        const float32 cornerDistance = walk.Stations[cornerIndex].Distance;
        const std::vector<SplineCornerIssue> fallbacks =
            ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false);
        EXPECT_TRUE(fallbacks.empty());

        const SplineStripMesh mesh = Sweep(Wall(thickness, 3.0f), walk.Stations);
        ASSERT_TRUE(mesh.IsValid());

        uint32 cornerRings = 0;
        for (uint32 r = 0; r < mesh.RingCount; ++r)
        {
            if (std::abs(walk.Stations[r].Distance - cornerDistance) > 1.0e-6f)
                continue;
            ++cornerRings;
            const RingBase base = BaseOf(mesh, r, corner, walk.Stations[r].Right);
            // Leg 1 runs +Z, so its across is +X; leg 2 runs +X, across is -Z.
            EXPECT_NEAR(std::abs(base.Right.x - base.Left.x), thickness, 1.0e-3f)
                << "thickness " << thickness << " across leg 1, ring " << r;
            EXPECT_NEAR(std::abs(base.Right.z - base.Left.z), thickness, 1.0e-3f)
                << "thickness " << thickness << " across leg 2, ring " << r;
        }
        EXPECT_EQ(cornerRings, 2u) << "a mitre is one ring emitted twice";
        EXPECT_EQ(InvertedSideTriangles(mesh), 0u) << "thickness " << thickness;
    }
}

// Both copies of the mitre ring stand on the same positions, the outer faces of
// the two legs meet on that one edge, and each copy carries its own leg's
// normals, so the corner is a sharp crease rather than a smoothed pillow.
TEST(SplineCorner, AMitreMeetsBothLegsOuterFacesOnOneSharpEdge)
{
    const float32 thickness = 1.0f;
    Walk walk = WalkPolyline(Elbow(10.0f, 7.3f, 90.0f), 0.5f, thickness * 0.5f);
    const float32 cornerDistance = walk.Stations[walk.Corners[0].StationIndex].Distance;
    (void)ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false);
    const SplineStripMesh mesh = Sweep(Wall(thickness, 3.0f), walk.Stations);
    ASSERT_TRUE(mesh.IsValid());

    uint32 incoming = mesh.RingCount;
    for (uint32 r = 0; r < mesh.RingCount; ++r)
    {
        if (std::abs(walk.Stations[r].Distance - cornerDistance) <= 1.0e-6f)
        {
            incoming = r;
            break;
        }
    }
    ASSERT_LT(incoming + 1u, mesh.RingCount);
    const uint32 outgoing = incoming + 1u;
    EXPECT_EQ(walk.Stations[outgoing].Join, SplineStationJoin::Crease);

    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
    {
        const V3& a = mesh.Vertices[incoming * mesh.RingVertexCount + k].Position;
        const V3& b = mesh.Vertices[outgoing * mesh.RingVertexCount + k].Position;
        EXPECT_EQ(std::memcmp(&a, &b, sizeof(V3)), 0) << "slot " << k;
    }

    // A right turn: the outside is the left (-X on leg 1, +Z on leg 2). The
    // outer bottom corner lies on leg 1's outer face (x = -0.5) and on leg 2's
    // (z = 10.5).
    const RingBase base = BaseOf(mesh, incoming, walk.Stations[incoming].Position,
                                 walk.Stations[incoming].Right);
    EXPECT_NEAR(base.Left.x, -0.5f, 1.0e-5f);
    EXPECT_NEAR(base.Left.z, 10.5f, 1.0e-5f);
    // The inner corner is where the two inside faces meet.
    EXPECT_NEAR(base.Right.x, 0.5f, 1.0e-5f);
    EXPECT_NEAR(base.Right.z, 9.5f, 1.0e-5f);

    // The side faces keep their own leg's normal up to the edge: 90 degrees
    // apart across the crease.
    float32 leastDot = 1.0f;
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
    {
        const V3& n0 = mesh.Vertices[incoming * mesh.RingVertexCount + k].Normal;
        const V3& n1 = mesh.Vertices[outgoing * mesh.RingVertexCount + k].Normal;
        if (std::abs(n0.y) > 0.5f)
            continue;
        leastDot = std::min(leastDot, std::abs(V3::Dot(n0, n1)));
    }
    EXPECT_LT(leastDot, 1.0e-5f);

    // No band joins the two copies: nothing references both rings at once.
    for (size_t t = 0; t < mesh.Indices.size(); t += 3u)
    {
        bool touchesIncoming = false;
        bool touchesOutgoing = false;
        for (size_t i = t; i < t + 3u; ++i)
        {
            touchesIncoming |= mesh.Indices[i] / mesh.RingVertexCount == incoming;
            touchesOutgoing |= mesh.Indices[i] / mesh.RingVertexCount == outgoing;
        }
        EXPECT_FALSE(touchesIncoming && touchesOutgoing) << "triangle " << t / 3u;
    }
}

// A Round corner pivots about the corner point: the outside of the turn is an
// arc of the wall's half-thickness, the inside meets at the inner mitre point,
// and a 90-degree turn takes ceil(90 / 10) = 9 rings that all carry the
// corner's U.
TEST(SplineCorner, ARoundNinetyDegreeCornerIsNineRingsOnTheArc)
{
    const float32 thickness = 0.6f;
    Walk walk = WalkPolyline(Elbow(10.0f, 7.3f, 90.0f), 0.5f, thickness * 0.5f);
    const V3 corner = walk.Stations[walk.Corners[0].StationIndex].Position;
    const float32 cornerDistance = walk.Stations[walk.Corners[0].StationIndex].Distance;
    const auto fallbacks =
        ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Round, kUp, false);
    EXPECT_TRUE(fallbacks.empty());
    const SplineStripMesh mesh = Sweep(Wall(thickness, 3.0f), walk.Stations);
    ASSERT_TRUE(mesh.IsValid());

    uint32 rings = 0;
    float32 cornerU = -1.0f;
    for (uint32 r = 0; r < mesh.RingCount; ++r)
    {
        if (std::abs(walk.Stations[r].Distance - cornerDistance) > 1.0e-6f)
            continue;
        ++rings;
        const RingBase base = BaseOf(mesh, r, corner, walk.Stations[r].Right);
        // Outside (left of a right turn): on the arc about the corner point.
        const V3 outer{base.Left.x - corner.x, 0.0f, base.Left.z - corner.z};
        EXPECT_NEAR(Length(outer), thickness * 0.5f, 1.0e-4f) << "ring " << r;
        // Inside: the inner mitre point.
        EXPECT_NEAR(base.Right.x, 0.3f, 1.0e-5f) << "ring " << r;
        EXPECT_NEAR(base.Right.z, 10.0f - 0.3f, 1.0e-5f) << "ring " << r;
        const float32 u = mesh.Vertices[r * mesh.RingVertexCount].UV.x;
        if (cornerU < 0.0f)
            cornerU = u;
        EXPECT_EQ(u, cornerU) << "ring " << r;
    }
    EXPECT_EQ(rings, 9u);
    EXPECT_EQ(InvertedSideTriangles(mesh), 0u);
}

// Past the mitre limit (a scale of 2, a 120-degree turn) a Mitre would place its
// ring ever further out: it is built Round and reported by its authored point.
// Its inside stands at the inner mitre point, 0.3 · tan 75° = 1.12 m back along
// each leg, which both legs here are long enough to hold.
TEST(SplineCorner, AMitreTurningPastTheLimitIsBuiltRoundAndReported)
{
    const float32 thickness = 0.6f;
    Walk walk = WalkPolyline(Elbow(10.0f, 7.3f, 150.0f), 0.5f, thickness * 0.5f);
    const V3 corner = walk.Stations[walk.Corners[0].StationIndex].Position;
    const float32 cornerDistance = walk.Stations[walk.Corners[0].StationIndex].Distance;
    const auto fallbacks =
        ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false);
    ASSERT_EQ(fallbacks.size(), 1u);
    EXPECT_EQ(fallbacks[0].Kind, SplineCornerIssueKind::PastMitreLimit);
    EXPECT_EQ(fallbacks[0].PointIndex, 1u);
    EXPECT_NEAR(fallbacks[0].TurnDegrees, 150.0f, 1.0e-2f);

    const SplineStripMesh mesh = Sweep(Wall(thickness, 3.0f), walk.Stations);
    ASSERT_TRUE(mesh.IsValid());
    uint32 rings = 0;
    for (uint32 r = 0; r < mesh.RingCount; ++r)
    {
        if (std::abs(walk.Stations[r].Distance - cornerDistance) > 1.0e-6f)
            continue;
        ++rings;
        const RingBase base = BaseOf(mesh, r, corner, walk.Stations[r].Right);
        EXPECT_NEAR(base.Right.x, 0.3f, 1.0e-4f) << "ring " << r;
        EXPECT_NEAR(base.Right.z, corner.z - 0.3f * std::tan(75.0f * kPi / 180.0f), 1.0e-4f)
            << "ring " << r;
    }
    EXPECT_EQ(rings, 15u);
}

// Closing a sampled loop makes its last station its first again, looking across
// the seam; a corner on the seam then opens the stream with its outgoing half
// and closes it with its incoming half, so the two end rings still share every
// position and each keeps its own leg's normals.
TEST(SplineCorner, AClosedLoopsSeamCornerSplitsAcrossTheSeam)
{
    const float32 side = 6.3f;
    Walk walk = WalkPolyline({V3{0.0f, 0.0f, 0.0f}, V3{0.0f, 0.0f, side}, V3{side, 0.0f, side},
                              V3{side, 0.0f, 0.0f}, V3{0.0f, 0.0f, 0.0f}},
                             0.5f, 0.5f);
    walk.Corners.insert(walk.Corners.begin(), SplineCornerSite{0u, 0u});
    CloseStationLoop(walk.Stations, kUp);
    EXPECT_EQ(std::memcmp(&walk.Stations.front().Position, &walk.Stations.back().Position, sizeof(V3)), 0);
    EXPECT_TRUE(ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, true).empty());

    SplineStripParams params;
    params.UOriginMetres = 0.0f;
    params.GenerateCaps = false;
    const SplineStripMesh mesh = BuildSplineStrip(Wall(1.0f, 3.0f), walk.Stations, params);
    ASSERT_TRUE(mesh.IsValid());
    EXPECT_EQ(mesh.Vertices.size(), static_cast<size_t>(mesh.RingCount) * mesh.RingVertexCount)
        << "no cap vertices";

    const uint32 lastRing = (mesh.RingCount - 1u) * mesh.RingVertexCount;
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
    {
        EXPECT_EQ(std::memcmp(&mesh.Vertices[k].Position, &mesh.Vertices[lastRing + k].Position,
                              sizeof(V3)),
                  0)
            << "slot " << k;
    }
    // The seam is the loop's first corner: the outgoing leg runs +Z, the
    // incoming one -X, and their outer faces meet at (-0.5, 0, -0.5).
    EXPECT_NEAR(walk.Stations.front().Forward.z, 1.0f, 1.0e-6f);
    EXPECT_NEAR(walk.Stations.back().Forward.x, -1.0f, 1.0e-6f);
    const RingBase base = BaseOf(mesh, 0u, walk.Stations.front().Position, walk.Stations.front().Right);
    EXPECT_NEAR(base.Left.x, -0.5f, 1.0e-5f);
    EXPECT_NEAR(base.Left.z, -0.5f, 1.0e-5f);
    EXPECT_EQ(InvertedSideTriangles(mesh), 0u);
}

// Without a corner on it, the seam frame looks from the station before the end
// to the one after the start.
TEST(SplineCorner, ClosingASmoothLoopLooksAcrossTheSeam)
{
    std::vector<SplineStripStation> stations(24);
    for (size_t i = 0; i < stations.size(); ++i)
    {
        const float32 angle = 2.0f * kPi * static_cast<float32>(i) / static_cast<float32>(stations.size() - 1u);
        stations[i].Position = V3{10.0f * std::sin(angle), 0.0f, 10.0f * std::cos(angle)};
        stations[i].Forward = V3{0.0f, 0.0f, 1.0f};
        stations[i].Distance = 10.0f * angle;
    }
    CloseStationLoop(stations, kUp);
    const V3 across = Normalize(stations[1].Position - stations[stations.size() - 2u].Position);
    EXPECT_NEAR(V3::Dot(stations.front().Forward, across), 1.0f, 1.0e-6f);
    EXPECT_EQ(std::memcmp(&stations.front().Forward, &stations.back().Forward, sizeof(V3)), 0);
    EXPECT_EQ(std::memcmp(&stations.front().Position, &stations.back().Position, sizeof(V3)), 0);
    EXPECT_FLOAT_EQ(stations.back().Distance, 20.0f * kPi);
}

// The inside faces of the two legs meet at the inner mitre point, halfWidth ·
// tan(θ/2) before and after the corner. A ring standing exactly that far out puts
// its inside vertex on that point, so the band from it to the mitre ring has no
// length on the inside face and carries a jump of U instead; one a centimetre
// further out does the same in all but name. With the corner on a sample and a
// 1 m wall at 90 degrees that ring is the sample 0.5 m out, and in the second
// walk the sample 0.51 m out: both are dropped, the inside face reaches the
// mitre ring in one band at least 5 cm long, and no side face anywhere in the
// run has a zero-area triangle.
TEST(SplineCorner, ARingAtTheInnerMitrePointIsDroppedSoTheInsideFaceHasNoEmptyBand)
{
    for (const float32 offset : {0.0f, 0.011f})
    {
        Walk walk = WalkPolyline(Elbow(10.0f, 10.0f, 90.0f), 0.5f, 0.5f);
        const uint32 nearIndex = walk.Corners[0].StationIndex - 1u;
        walk.Stations[nearIndex].Position.z -= offset;
        walk.Stations[nearIndex].Distance -= offset;
        const float32 cornerDistance = walk.Stations[walk.Corners[0].StationIndex].Distance;
        ASSERT_FLOAT_EQ(cornerDistance, 10.0f);
        (void)ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false);

        for (size_t i = 0; i < walk.Stations.size(); ++i)
        {
            const float32 fromCorner = std::abs(walk.Stations[i].Distance - cornerDistance);
            EXPECT_FALSE(fromCorner > 1.0e-6f && fromCorner < 0.5f + 0.05f)
                << "station " << i << " stands " << fromCorner << " m from the corner, offset " << offset;
        }

        const SplineStripMesh mesh = Sweep(Wall(1.0f, 3.0f), walk.Stations);
        ASSERT_TRUE(mesh.IsValid());
        uint32 emptySideTriangles = 0;
        const uint32 ringVertices = mesh.RingCount * mesh.RingVertexCount;
        for (size_t t = 0; t + 2u < mesh.Indices.size(); t += 3u)
        {
            if (mesh.Indices[t] >= ringVertices)
                continue;
            const SplineVertex& a = mesh.Vertices[mesh.Indices[t]];
            if (std::abs(a.Normal.y) > 0.5f)
                continue;
            const V3 geometric = V3::Cross(mesh.Vertices[mesh.Indices[t + 1u]].Position - a.Position,
                                           mesh.Vertices[mesh.Indices[t + 2u]].Position - a.Position);
            if (Length(geometric) < 1.0e-6f)
                ++emptySideTriangles;
        }
        EXPECT_EQ(emptySideTriangles, 0u) << "offset " << offset;
    }
}

// A corner's inside faces meet halfWidth · tan(θ/2) along each leg; a leg
// shorter than that plus the inside band cannot hold them, and the inside face
// folds. A 1 m wall at 90 degrees reaches 0.5 m: an end leg of 0.3 m is
// reported against its corner, one of 0.6 m is not.
TEST(SplineCorner, AnEndLegShorterThanTheCornersReachIsReported)
{
    for (const float32 endLeg : {0.3f, 0.6f})
    {
        Walk walk = WalkPolyline(Elbow(10.0f, endLeg, 90.0f), 0.1f, 0.5f);
        const std::vector<SplineCornerIssue> issues =
            ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false);
        if (endLeg < 0.5f)
        {
            ASSERT_EQ(issues.size(), 1u);
            EXPECT_EQ(issues[0].Kind, SplineCornerIssueKind::LegShorterThanReach);
            EXPECT_EQ(issues[0].PointIndex, 1u);
        }
        else
        {
            EXPECT_TRUE(issues.empty()) << "end leg " << endLeg;
        }
    }
}

// Two corners turning the same way (a U) share the leg between them: it must
// hold both reaches. With a 1 m wall at 90 degrees that is 1 m; a 0.6 m middle
// leg is reported against both points, a 1.2 m one is not. Two corners turning
// opposite ways (a Z) put their inside faces on opposite sides of the leg and
// are never reported.
TEST(SplineCorner, SameTurnCornersCloserThanTheirReachesAreReported)
{
    for (const float32 middle : {0.6f, 1.2f})
    {
        const std::vector<V3> u = {V3{0.0f, 0.0f, 0.0f}, V3{0.0f, 0.0f, 10.0f},
                                   V3{middle, 0.0f, 10.0f}, V3{middle, 0.0f, 0.0f}};
        const std::vector<V3> z = {V3{0.0f, 0.0f, 0.0f}, V3{0.0f, 0.0f, 10.0f},
                                   V3{middle, 0.0f, 10.0f}, V3{middle, 0.0f, 20.0f}};
        Walk uWalk = WalkPolyline(u, 0.1f, 0.5f);
        const std::vector<SplineCornerIssue> uIssues =
            ApplyCorners(uWalk.Stations, uWalk.Corners, SplineCornerStyle::Mitre, kUp, false);
        Walk zWalk = WalkPolyline(z, 0.1f, 0.5f);
        const std::vector<SplineCornerIssue> zIssues =
            ApplyCorners(zWalk.Stations, zWalk.Corners, SplineCornerStyle::Mitre, kUp, false);
        EXPECT_TRUE(zIssues.empty()) << "Z, middle leg " << middle;
        if (middle < 1.0f)
        {
            ASSERT_EQ(uIssues.size(), 1u) << "U, middle leg " << middle;
            EXPECT_EQ(uIssues[0].Kind, SplineCornerIssueKind::CornersOverlap);
            EXPECT_EQ(uIssues[0].PointIndex, 1u);
            EXPECT_EQ(uIssues[0].OtherPointIndex, 2u);
        }
        else
        {
            EXPECT_TRUE(uIssues.empty()) << "U, middle leg " << middle;
        }
    }
}

// The mitre limit includes its own boundary: a corner authored at exactly 120
// degrees is a Mitre, not a fallback reported as past the limit.
TEST(SplineCorner, ACornerAtExactlyTheMitreLimitIsMitred)
{
    Walk walk = WalkPolyline(Elbow(10.0f, 8.0f, 120.0f), 0.5f, 0.3f);
    const float32 cornerDistance = walk.Stations[walk.Corners[0].StationIndex].Distance;
    EXPECT_TRUE(ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false).empty());
    uint32 cornerRings = 0;
    for (const SplineStripStation& station : walk.Stations)
        cornerRings += std::abs(station.Distance - cornerDistance) <= 1.0e-6f ? 1u : 0u;
    EXPECT_EQ(cornerRings, 2u);
}

namespace
{

// How far each side face's U advance departs from the face's own length, band
// by band: the largest |ΔU / length − 1| over every stitched band of every side
// slot (a vertex with a lateral normal). 0 is a face whose texture runs at true
// scale along it. Bands with no length on the face (the inside of a round corner
// pivots on one point) carry nothing to measure and are skipped.
float32 WorstSideFaceStretch(const SplineStripMesh& mesh,
                             const std::vector<SplineStripStation>& stations)
{
    float32 worst = 0.0f;
    for (uint32 r = 0; r + 1u < mesh.RingCount; ++r)
    {
        if (stations[r + 1u].Join == SplineStationJoin::Crease)
            continue;
        for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
        {
            const SplineVertex& a = mesh.Vertices[r * mesh.RingVertexCount + k];
            const SplineVertex& b = mesh.Vertices[(r + 1u) * mesh.RingVertexCount + k];
            if (std::abs(a.Normal.y) > 0.5f)
                continue;
            const float32 length = Length(b.Position - a.Position);
            if (length < 1.0e-3f)
                continue;
            worst = std::max(worst, std::abs(std::abs(b.UV.x - a.UV.x) / length - 1.0f));
        }
    }
    return worst;
}

// The same measure along the top face's two edges (the arrises): each top
// vertex's U advance against the length its edge runs, band by band. Bands with
// no length on that edge (a round fan's inside pivots on one point) are skipped.
float32 WorstTopEdgeStretch(const SplineStripMesh& mesh,
                            const std::vector<SplineStripStation>& stations)
{
    float32 worst = 0.0f;
    for (uint32 r = 0; r + 1u < mesh.RingCount; ++r)
    {
        if (stations[r + 1u].Join == SplineStationJoin::Crease)
            continue;
        for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
        {
            const SplineVertex& a = mesh.Vertices[r * mesh.RingVertexCount + k];
            const SplineVertex& b = mesh.Vertices[(r + 1u) * mesh.RingVertexCount + k];
            if (a.Normal.y < 0.5f)
                continue;
            const float32 length = Length(b.Position - a.Position);
            if (length < 1.0e-3f)
                continue;
            worst = std::max(worst, std::abs(std::abs(b.UV.x - a.UV.x) / length - 1.0f));
        }
    }
    return worst;
}

} // namespace

// On a straight run every forward agrees, so no face turns and the sweep's U is
// the centreline's to the bit, face turn or not.
TEST(SplineCorner, AStraightRunGainsNoFaceTurnAndKeepsItsUExactly)
{
    Walk walk = WalkPolyline({V3{0.0f, 0.0f, 0.0f}, V3{3.0f, 0.4f, 17.0f}}, 0.5f, 0.5f);
    const SplineStripMesh before = Sweep(Wall(1.0f, 3.0f), walk.Stations);
    AccumulateFaceTurn(walk.Stations, kUp);
    for (const SplineStripStation& station : walk.Stations)
    {
        EXPECT_EQ(station.FaceTurnLeft, 0.0f);
        EXPECT_EQ(station.FaceTurnRight, 0.0f);
    }
    const SplineStripMesh after = Sweep(Wall(1.0f, 3.0f), walk.Stations);
    ASSERT_EQ(before.Vertices.size(), after.Vertices.size());
    EXPECT_EQ(std::memcmp(before.Vertices.data(), after.Vertices.data(),
                          before.Vertices.size() * sizeof(SplineVertex)),
              0);
}

// A mitre lengthens the outer face by halfWidth · tan(θ/2) on each leg and
// shortens the inner face as much. With U along each face, every band of every
// side face runs its texture at true scale into and out of the corner, on the
// corner-on-a-sample and corner-between-samples walks and on 1 m and 2 m walls;
// with the centreline's U the last band before the mitre stretched up to 1.9x on
// the outside and down to 0.1x on the inside.
TEST(SplineCorner, EachSideFaceOfAMitreTakesUAlongItsOwnLength)
{
    for (const float32 thickness : {1.0f, 2.0f})
    {
        for (const float32 legB : {10.0f, 7.3f})
        {
            Walk walk = WalkPolyline(Elbow(10.0f, legB, 90.0f), 0.5f, thickness * 0.5f);
            ASSERT_TRUE(
                ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false).empty());
            AccumulateFaceTurn(walk.Stations, kUp);
            const SplineStripMesh mesh = Sweep(Wall(thickness, 3.0f), walk.Stations);
            ASSERT_TRUE(mesh.IsValid());
            EXPECT_LT(WorstSideFaceStretch(mesh, walk.Stations), 1.0e-4f)
                << "thickness " << thickness << ", second leg " << legB;
        }
    }
}

// A mitre's top takes the centreline U projected onto each leg: the last top band
// before the mitre line runs at true scale along both its arrises, the outer one
// halfWidth longer than the centreline on a 1 m wall at 90 degrees and the inner
// one as much shorter. With the station's distance alone they ran 1.5x and 0.5x.
TEST(SplineCorner, EachTopEdgeOfAMitreTakesUAlongItsOwnLength)
{
    for (const float32 legB : {10.0f, 7.3f})
    {
        Walk walk = WalkPolyline(Elbow(10.0f, legB, 90.0f), 0.5f, 0.5f);
        ASSERT_TRUE(ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false).empty());
        AccumulateFaceTurn(walk.Stations, kUp);
        const SplineStripMesh mesh = Sweep(Wall(1.0f, 3.0f), walk.Stations);
        ASSERT_TRUE(mesh.IsValid());
        EXPECT_LT(WorstTopEdgeStretch(mesh, walk.Stations), 1.0e-4f) << "second leg " << legB;
    }
}

// A round corner's outside is an arc of the half-width, whose length its U must
// cover, and its inside pivots on the inner mitre point, which the legs' inside
// faces reach short of the corner. Past the mitre limit the same holds for the
// fallback. The arc is faceted at 10-degree rings, so a band's chord is 0.13 %
// shorter than the arc its U covers.
//
// The top follows the same rule: its outer edge advances by halfWidth times each
// ring's turn around the fan, the arc it sweeps, while its inside pinches on the
// inner mitre point; that arc is carried down the run, so the bands into and out
// of the fan run at true scale on both edges.
TEST(SplineCorner, EachSideFaceOfARoundCornerTakesUAlongItsOwnLength)
{
    constexpr float32 kHalfWidth = 0.3f;
    for (const float32 turnDegrees : {90.0f, 150.0f})
    {
        Walk walk = WalkPolyline(Elbow(10.0f, 7.3f, turnDegrees), 0.5f, kHalfWidth);
        (void)ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Round, kUp, false);
        AccumulateFaceTurn(walk.Stations, kUp);
        const SplineStripMesh mesh = Sweep(Wall(2.0f * kHalfWidth, 3.0f), walk.Stations);
        ASSERT_TRUE(mesh.IsValid());
        EXPECT_LT(WorstSideFaceStretch(mesh, walk.Stations), 2.0e-3f) << "turn " << turnDegrees;

        uint32 rimBands = 0;
        uint32 joiningBands = 0;
        for (uint32 r = 0; r + 1u < mesh.RingCount; ++r)
        {
            const SplineStripStation& from = walk.Stations[r];
            const SplineStripStation& to = walk.Stations[r + 1u];
            const bool inFan = to.Join == SplineStationJoin::Pivot;
            const bool joinsFan = !inFan && (from.MitreSides != 0u || to.MitreSides != 0u);
            if (!inFan && !joinsFan)
                continue;
            for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
            {
                const SplineVertex& a = mesh.Vertices[r * mesh.RingVertexCount + k];
                const SplineVertex& b = mesh.Vertices[(r + 1u) * mesh.RingVertexCount + k];
                if (a.Normal.y < 0.5f)
                    continue;
                const float32 edge = Length(b.Position - a.Position);
                const float32 dU = b.UV.x - a.UV.x;
                if (inFan)
                {
                    if (edge < 1.0e-4f)
                        continue; // the inside pinches on one point
                    ++rimBands;
                    const float32 ringTurn = std::acos(std::clamp(V3::Dot(from.Forward, to.Forward), -1.0f, 1.0f));
                    EXPECT_NEAR(dU, kHalfWidth * ringTurn, 1.0e-4f)
                        << "turn " << turnDegrees << ", fan band " << r << ", slot " << k;
                }
                else
                {
                    ++joiningBands;
                    EXPECT_NEAR(dU / edge, 1.0f, 1.0e-4f)
                        << "turn " << turnDegrees << ", band " << r << " joining the fan, slot " << k;
                }
            }
        }
        EXPECT_GT(rimBands, 0u);
        EXPECT_GT(joiningBands, 0u);
        EXPECT_LT(WorstTopEdgeStretch(mesh, walk.Stations), 2.0e-3f) << "top, turn " << turnDegrees;
    }
}

// On a curve the outer face of a 1 m wall on a 10 m radius is 5 % longer than the
// centreline and the inner 5 % shorter; U along each face tiles both at one size.
TEST(SplineCorner, BothFacesOfACurveTileAtTheirOwnLength)
{
    const float32 radius = 10.0f;
    std::vector<SplineStripStation> stations(64);
    for (size_t i = 0; i < stations.size(); ++i)
    {
        const float32 angle =
            0.5f * kPi * static_cast<float32>(i) / static_cast<float32>(stations.size() - 1u);
        SplineStripStation& s = stations[i];
        // A right turn about (radius, 0, 0), starting along +Z.
        s.Position = V3{radius - radius * std::cos(angle), 0.0f, radius * std::sin(angle)};
        s.Forward = V3{std::sin(angle), 0.0f, std::cos(angle)};
        s.Right = Normalize(V3::Cross(kUp, s.Forward));
        s.Up = V3::Cross(s.Forward, s.Right);
        s.HalfWidthLeft = 0.5f;
        s.HalfWidthRight = 0.5f;
        s.Distance = radius * angle;
    }
    AccumulateFaceTurn(stations, kUp);
    EXPECT_NEAR(stations.back().FaceTurnLeft, 0.5f * kPi, 1.0e-4f);
    EXPECT_NEAR(stations.back().FaceTurnRight, -0.5f * kPi, 1.0e-4f);
    const SplineStripMesh mesh = Sweep(Wall(1.0f, 3.0f), stations);
    ASSERT_TRUE(mesh.IsValid());
    EXPECT_LT(WorstSideFaceStretch(mesh, stations), 1.0e-3f);
}

// A welded loop closes each face on itself: the seam corner's outgoing half opens
// the stream at zero face turn and its incoming half closes it, so a square of
// four right-turning mitres ends with 8 · tan 45° on the outside and as much off
// the inside. With the loop's face snap every side face then ends its U on a whole
// tile where it began at zero, and runs within half a tile of its own length.
// The top meets itself at the seam's mitre joint, square to each leg, and is not
// a face that closes on itself.
TEST(SplineCorner, EachFaceOfAWeldedLoopClosesOnAWholeTile)
{
    const float32 side = 6.3f;
    Walk walk = WalkPolyline({V3{0.0f, 0.0f, 0.0f}, V3{0.0f, 0.0f, side}, V3{side, 0.0f, side},
                              V3{side, 0.0f, 0.0f}, V3{0.0f, 0.0f, 0.0f}},
                             0.5f, 0.5f);
    walk.Corners.insert(walk.Corners.begin(), SplineCornerSite{0u, 0u});
    CloseStationLoop(walk.Stations, kUp);
    ASSERT_TRUE(ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, true).empty());
    AccumulateFaceTurn(walk.Stations, kUp);
    const SplineStripStation& first = walk.Stations.front();
    const SplineStripStation& last = walk.Stations.back();
    EXPECT_EQ(first.FaceTurnLeft, 0.0f);
    EXPECT_EQ(first.FaceTurnRight, 0.0f);
    EXPECT_NEAR(last.FaceTurnLeft, 8.0f, 1.0e-4f);
    EXPECT_NEAR(last.FaceTurnRight, -8.0f, 1.0e-4f);

    SplineStripParams params;
    params.UOriginMetres = 0.0f;
    params.GenerateCaps = false;
    params.LoopLengthMetres = last.Distance;
    params.LoopFaceTurnLeft = last.FaceTurnLeft;
    params.LoopFaceTurnRight = last.FaceTurnRight;
    const SplineStripMesh mesh = BuildSplineStrip(Wall(1.0f, 3.0f), walk.Stations, params);
    ASSERT_TRUE(mesh.IsValid());

    const uint32 lastRing = (mesh.RingCount - 1u) * mesh.RingVertexCount;
    uint32 sideSlots = 0;
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
    {
        const SplineVertex& opening = mesh.Vertices[k];
        const SplineVertex& closing = mesh.Vertices[lastRing + k];
        if (std::abs(opening.Normal.y) > 0.5f)
            continue;
        ++sideSlots;
        EXPECT_EQ(opening.UV.x, 0.0f) << "slot " << k;
        EXPECT_NEAR(closing.UV.x, std::round(closing.UV.x), 1.0e-3f) << "slot " << k;
        const bool left = V3::Dot(opening.Position - first.Position, first.Right) < 0.0f;
        const float32 faceLength =
            last.Distance + 0.5f * (left ? last.FaceTurnLeft : last.FaceTurnRight);
        EXPECT_LE(std::abs(closing.UV.x - faceLength), 0.5f) << "slot " << k;
    }
    EXPECT_EQ(sideSlots, 4u);
}

namespace
{

// Perpendicular distance, in the level plane, from `p` to the line through
// `origin` along `direction`.
float32 PlanDistanceToLine(const V3& p, const V3& origin, const V3& direction)
{
    const V3 d = Normalize(V3{direction.x, 0.0f, direction.z});
    const V3 offset{p.x - origin.x, 0.0f, p.z - origin.z};
    return Length(offset - d * V3::Dot(offset, d));
}

} // namespace

// A 150-degree hairpin built past the mitre limit. With its inside on the corner
// line, the two legs' inside faces ran diagonally to the corner and their top
// faces overlapped, coplanar, for about 1.9 m; dropping the stations within the
// reach without moving the inside cut a slot through the top instead. Here the
// inside stands on the inner mitre point, which lies on both legs' inside faces
// (0.3 m from each leg's centreline on a 0.6 m wall): no station survives within
// the reach, so neither leg's top reaches past that point, and no side face folds.
TEST(SplineCorner, AHairpinPastTheLimitMeetsBothInsideFacesWithNoSlotOrOverlap)
{
    const float32 halfWidth = 0.3f;
    const std::vector<V3> points = Elbow(10.0f, 7.3f, 150.0f);
    Walk walk = WalkPolyline(points, 0.5f, halfWidth);
    const float32 cornerDistance = walk.Stations[walk.Corners[0].StationIndex].Distance;
    const float32 reach = halfWidth * std::tan(75.0f * kPi / 180.0f);
    (void)ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false);
    const SplineStripMesh mesh = Sweep(Wall(2.0f * halfWidth, 3.0f), walk.Stations);
    ASSERT_TRUE(mesh.IsValid());

    for (const SplineStripStation& station : walk.Stations)
    {
        const float32 fromCorner = std::abs(station.Distance - cornerDistance);
        EXPECT_TRUE(fromCorner < 1.0e-6f || fromCorner > reach)
            << "a station " << fromCorner << " m from the corner survives inside its reach";
    }
    uint32 cornerRings = 0;
    for (uint32 r = 0; r < mesh.RingCount; ++r)
    {
        if (std::abs(walk.Stations[r].Distance - cornerDistance) > 1.0e-6f)
            continue;
        ++cornerRings;
        const RingBase base = BaseOf(mesh, r, points[1], walk.Stations[r].Right);
        EXPECT_NEAR(PlanDistanceToLine(base.Right, points[0], points[1] - points[0]), halfWidth, 1.0e-4f)
            << "ring " << r << " inside off the incoming leg's inside face";
        EXPECT_NEAR(PlanDistanceToLine(base.Right, points[1], points[2] - points[1]), halfWidth, 1.0e-4f)
            << "ring " << r << " inside off the outgoing leg's inside face";
    }
    EXPECT_GT(cornerRings, 2u);
    EXPECT_EQ(InvertedSideTriangles(mesh), 0u);
    EXPECT_EQ(FoldedTopTriangles(mesh), 0u);
}

// Where a leg is shorter than the hairpin's reach, the inside point stands only
// as far back as that leg holds (its length less the 5 cm band), moved in along
// the bisector, so the inside face narrows toward the corner rather than folding
// back past the leg's end. It fits by construction, so no fold is reported; the
// corner is still reported past the mitre limit. Narrowing over the 5 cm band
// leaves a top quad that is not convex, which the builder splits on the diagonal
// that keeps both triangles facing up.
TEST(SplineCorner, AHairpinsInsideIsBoundedByAShortLeg)
{
    const float32 halfWidth = 0.3f;
    const float32 shortLeg = 0.8f;
    const std::vector<V3> points = Elbow(10.0f, shortLeg, 150.0f);
    Walk walk = WalkPolyline(points, 0.1f, halfWidth);
    const float32 cornerDistance = walk.Stations[walk.Corners[0].StationIndex].Distance;
    const std::vector<SplineCornerIssue> issues =
        ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].Kind, SplineCornerIssueKind::PastMitreLimit);

    const SplineStripMesh mesh = Sweep(Wall(2.0f * halfWidth, 3.0f), walk.Stations);
    ASSERT_TRUE(mesh.IsValid());
    const float32 held = shortLeg - 0.05f;
    const V3 incoming = Normalize(points[1] - points[0]);
    const V3 outgoing = Normalize(points[2] - points[1]);
    for (uint32 r = 0; r < mesh.RingCount; ++r)
    {
        if (std::abs(walk.Stations[r].Distance - cornerDistance) > 1.0e-6f)
            continue;
        const RingBase base = BaseOf(mesh, r, points[1], walk.Stations[r].Right);
        const V3 offset{base.Right.x - points[1].x, 0.0f, base.Right.z - points[1].z};
        EXPECT_NEAR(-V3::Dot(offset, incoming), held, 1.0e-4f) << "ring " << r;
        EXPECT_NEAR(V3::Dot(offset, outgoing), held, 1.0e-4f) << "ring " << r;
    }
    EXPECT_EQ(InvertedSideTriangles(mesh), 0u);
    EXPECT_EQ(FoldedTopTriangles(mesh), 0u);
}

// Both legs of a hairpin shorter than its reach: the inside narrows on both
// sides of the corner, and no side or top triangle folds.
TEST(SplineCorner, AHairpinWithBothLegsShortFoldsNoTopTriangle)
{
    Walk walk = WalkPolyline(Elbow(0.9f, 0.7f, 150.0f), 0.1f, 0.3f);
    (void)ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, false);
    const SplineStripMesh mesh = Sweep(Wall(0.6f, 3.0f), walk.Stations);
    ASSERT_TRUE(mesh.IsValid());
    EXPECT_EQ(InvertedSideTriangles(mesh), 0u);
    EXPECT_EQ(FoldedTopTriangles(mesh), 0u);
}

// A curve folds its wall's inside face where its radius is smaller than the
// inside half-width. On a 0.2 m circle every station of a 0.6 m wall folds; on
// an 8 m circle none does; and a station marked to be skipped is not measured.
TEST(SplineCorner, AStationOnACurveTighterThanTheWallIsAFold)
{
    const auto circle = [](float32 radius)
    {
        std::vector<SplineStripStation> stations(24);
        for (size_t i = 0; i < stations.size(); ++i)
        {
            const float32 angle = 0.5f * kPi * static_cast<float32>(i) / static_cast<float32>(stations.size() - 1u);
            stations[i].Position = V3{radius - radius * std::cos(angle), 0.0f, radius * std::sin(angle)};
            stations[i].HalfWidthLeft = 0.3f;
            stations[i].HalfWidthRight = 0.3f;
        }
        return stations;
    };
    const std::vector<SplineStripStation> tight = circle(0.2f);
    EXPECT_EQ(FindInnerFolds(tight, {}, kUp).size(), tight.size() - 2u);
    EXPECT_TRUE(FindInnerFolds(circle(8.0f), {}, kUp).empty());
    std::vector<uint8> skip(tight.size(), 1u);
    EXPECT_TRUE(FindInnerFolds(tight, skip, kUp).empty());
}

// A round corner on a welded loop's seam is split across the seam the way a
// mitre is: the outgoing half's ring opens the stream and the fan closes it,
// ending on that same ring, so the loop's two end rings still coincide. The fan
// is whole (nine rings at 90 degrees, all at the loop's length) and no side face
// folds.
TEST(SplineCorner, AClosedLoopsRoundSeamCornerSplitsAcrossTheSeam)
{
    const float32 side = 6.3f;
    Walk walk = WalkPolyline({V3{0.0f, 0.0f, 0.0f}, V3{0.0f, 0.0f, side}, V3{side, 0.0f, side},
                              V3{side, 0.0f, 0.0f}, V3{0.0f, 0.0f, 0.0f}},
                             0.5f, 0.5f);
    walk.Corners.insert(walk.Corners.begin(), SplineCornerSite{0u, 0u});
    CloseStationLoop(walk.Stations, kUp);
    EXPECT_TRUE(ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Round, kUp, true).empty());

    const float32 loopLength = walk.Stations.back().Distance;
    uint32 closingFan = 0;
    for (const SplineStripStation& station : walk.Stations)
        closingFan += std::abs(station.Distance - loopLength) < 1.0e-5f ? 1u : 0u;
    EXPECT_EQ(closingFan, 9u);
    EXPECT_NEAR(walk.Stations.front().Forward.z, 1.0f, 1.0e-5f) << "opens looking along the first leg";

    SplineStripParams params;
    params.UOriginMetres = 0.0f;
    params.GenerateCaps = false;
    const SplineStripMesh mesh = BuildSplineStrip(Wall(1.0f, 3.0f), walk.Stations, params);
    ASSERT_TRUE(mesh.IsValid());
    const uint32 lastRing = (mesh.RingCount - 1u) * mesh.RingVertexCount;
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
    {
        EXPECT_EQ(std::memcmp(&mesh.Vertices[k].Position, &mesh.Vertices[lastRing + k].Position, sizeof(V3)), 0)
            << "slot " << k;
    }
    EXPECT_EQ(InvertedSideTriangles(mesh), 0u);
}

// A welded loop of round corners carries the top's arc allowance all the way
// round. The loop's top snaps over its length plus that allowance, so where the
// seam's fan closes the stream and meets the ring that opened it, the top's U
// stands on a whole tile from where it began, within the snap's rounding.
TEST(SplineCorner, TheTopOfARoundCorneredLoopMeetsItselfAtTheSeam)
{
    const float32 side = 6.3f;
    Walk walk = WalkPolyline({V3{0.0f, 0.0f, 0.0f}, V3{0.0f, 0.0f, side}, V3{side, 0.0f, side},
                              V3{side, 0.0f, 0.0f}, V3{0.0f, 0.0f, 0.0f}},
                             0.5f, 0.5f);
    walk.Corners.insert(walk.Corners.begin(), SplineCornerSite{0u, 0u});
    CloseStationLoop(walk.Stations, kUp);
    ASSERT_TRUE(ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Round, kUp, true).empty());
    AccumulateFaceTurn(walk.Stations, kUp);
    const SplineStripStation& last = walk.Stations.back();
    EXPECT_EQ(walk.Stations.front().TopArcAllowance, 0.0f);
    EXPECT_NEAR(last.TopArcAllowance, 4.0f * 0.5f * 0.5f * kPi, 1.0e-4f) << "four quarter arcs of 0.5 m";

    SplineStripParams params;
    params.UOriginMetres = 0.0f;
    params.GenerateCaps = false;
    params.LoopLengthMetres = last.Distance;
    params.LoopFaceTurnLeft = last.FaceTurnLeft;
    params.LoopFaceTurnRight = last.FaceTurnRight;
    params.LoopTopArcAllowance = last.TopArcAllowance;
    const SplineStripMesh mesh = BuildSplineStrip(Wall(1.0f, 3.0f), walk.Stations, params);
    ASSERT_TRUE(mesh.IsValid());

    const uint32 lastRing = (mesh.RingCount - 1u) * mesh.RingVertexCount;
    uint32 topSlots = 0;
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
    {
        const SplineVertex& opening = mesh.Vertices[k];
        const SplineVertex& closing = mesh.Vertices[lastRing + k];
        if (opening.Normal.y < 0.5f)
            continue;
        ++topSlots;
        const float32 step = closing.UV.x - opening.UV.x;
        EXPECT_NEAR(step, std::round(step), 1.0e-3f) << "slot " << k;
        EXPECT_GE(std::round(step), 1.0f) << "slot " << k;
    }
    EXPECT_EQ(topSlots, 2u);
}
