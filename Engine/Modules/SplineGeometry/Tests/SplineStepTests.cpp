#include "SplineGeometry/SplineCorner.h"
#include "SplineGeometry/SplineStep.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "SplineWallWalk.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using namespace GameEngine::SplineGeometry::Testing;

namespace
{

constexpr float32 kWallHeight = 3.0f;
constexpr float32 kHalfWidth = 0.3f;

// A wall up a slope: 10 m north climbing 1 m to a Linear corner, then 10 m east
// climbing another metre. Its two runs stand level at 1 m and 2 m, so their tops
// are 4 m and 5 m, and the corner steps up by a metre.
const std::vector<V3> kClimbing = {V3{0.0f, 0.0f, 0.0f}, V3{0.0f, 1.0f, 10.0f},
                                   V3{10.0f, 2.0f, 10.0f}};

struct SteppedWall
{
    std::vector<SplineStripStation> Stations;
    SplineStripMesh Mesh;
    float32 CornerDistance = 0.0f;
};

SteppedWall BuildStepped(const std::vector<V3>& points, SplineCornerStyle style)
{
    SteppedWall wall;
    Walk walk = WalkPolyline(points, 0.5f, kHalfWidth);
    wall.CornerDistance = walk.Stations[walk.Corners[0].StationIndex].Distance;
    (void)ApplyCorners(walk.Stations, walk.Corners, style, kUp, false);
    const float32 pointDistances[] = {wall.CornerDistance};
    StepTopsAtPoints(walk.Stations, pointDistances, kUp, false);
    wall.Stations = walk.Stations;

    SplineStripParams params;
    params.UOriginMetres = 0.0f;
    params.GenerateCaps = false;
    params.HeightVDatumMetres = 0.0f;
    wall.Mesh = BuildSplineStrip(Wall(2.0f * kHalfWidth, kWallHeight), wall.Stations, params);
    return wall;
}

// The top of ring r: its highest vertex, and its lowest.
float32 TopOf(const SplineStripMesh& mesh, uint32 r)
{
    float32 top = std::numeric_limits<float32>::lowest();
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
        top = std::max(top, mesh.Vertices[r * mesh.RingVertexCount + k].Position.y);
    return top;
}

float32 BottomOf(const SplineStripMesh& mesh, uint32 r)
{
    float32 bottom = std::numeric_limits<float32>::max();
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
        bottom = std::min(bottom, mesh.Vertices[r * mesh.RingVertexCount + k].Position.y);
    return bottom;
}

// The riser vertices: whatever follows the rings when the caps are off.
std::vector<SplineVertex> RiserVertices(const SplineStripMesh& mesh)
{
    return {mesh.Vertices.begin() + static_cast<ptrdiff_t>(mesh.RingCount) * mesh.RingVertexCount,
            mesh.Vertices.end()};
}

// Triangles among the riser vertices whose winding disagrees with their normal.
uint32 MiswoundRiserTriangles(const SplineStripMesh& mesh)
{
    const uint32 ringVertices = mesh.RingCount * mesh.RingVertexCount;
    uint32 miswound = 0;
    for (size_t t = 0; t + 2u < mesh.Indices.size(); t += 3u)
    {
        if (mesh.Indices[t] < ringVertices)
            continue;
        const SplineVertex& a = mesh.Vertices[mesh.Indices[t]];
        const SplineVertex& b = mesh.Vertices[mesh.Indices[t + 1u]];
        const SplineVertex& c = mesh.Vertices[mesh.Indices[t + 2u]];
        const V3 geometric = V3::Cross(b.Position - a.Position, c.Position - a.Position);
        if (V3::Dot(geometric, a.Normal) <= 0.0f)
            ++miswound;
    }
    return miswound;
}

} // namespace

// Each run keeps a level top at the wall's height above its highest station, so
// the top never stands lower than that anywhere and the base stays on the slope.
// The mitre's Crease ring carries the step, which one riser closes: a quad from
// the lower top to the higher, facing back toward the lower run, wound to its
// normal, with V the height above the wall's datum as on the side faces, so the
// courses meet across the step.
TEST(SplineStep, AMitredWallUpASlopeStepsItsLevelTopAtTheCorner)
{
    const SteppedWall wall = BuildStepped(kClimbing, SplineCornerStyle::Mitre);
    ASSERT_TRUE(wall.Mesh.IsValid());
    uint32 creases = 0;
    for (uint32 r = 0; r < wall.Mesh.RingCount; ++r)
    {
        const SplineStripStation& station = wall.Stations[r];
        const bool secondRun = station.Distance > wall.CornerDistance + 1.0e-4f ||
                               (station.Distance > wall.CornerDistance - 1.0e-4f &&
                                station.Join == SplineStationJoin::Crease);
        creases += station.Join == SplineStationJoin::Crease ? 1u : 0u;
        EXPECT_NEAR(TopOf(wall.Mesh, r), secondRun ? 5.0f : 4.0f, 1.0e-4f) << "ring " << r;
        EXPECT_GE(TopOf(wall.Mesh, r) - BottomOf(wall.Mesh, r), kWallHeight - 1.0e-4f) << "ring " << r;
    }
    EXPECT_EQ(creases, 1u);

    const std::vector<SplineVertex> riser = RiserVertices(wall.Mesh);
    ASSERT_EQ(riser.size(), 4u);
    uint32 low = 0;
    uint32 high = 0;
    for (const SplineVertex& v : riser)
    {
        low += std::abs(v.Position.y - 4.0f) < 1.0e-4f ? 1u : 0u;
        high += std::abs(v.Position.y - 5.0f) < 1.0e-4f ? 1u : 0u;
        EXPECT_NEAR(v.UV.y, v.Position.y, 1.0e-4f) << "riser V is the height above the datum";
        // Back toward the first run, which runs north (+Z) into the corner.
        EXPECT_LT(v.Normal.z, -0.5f);
    }
    EXPECT_EQ(low, 2u);
    EXPECT_EQ(high, 2u);
    EXPECT_EQ(MiswoundRiserTriangles(wall.Mesh), 0u);
}

// Walking the same wall downhill, the first run is the higher and the riser
// faces ahead, down onto the lower run.
TEST(SplineStep, AWallDownASlopeStepsDownFacingTheLowerRun)
{
    const std::vector<V3> descending(kClimbing.rbegin(), kClimbing.rend());
    const SteppedWall wall = BuildStepped(descending, SplineCornerStyle::Mitre);
    ASSERT_TRUE(wall.Mesh.IsValid());
    EXPECT_NEAR(TopOf(wall.Mesh, 0u), 5.0f, 1.0e-4f);
    EXPECT_NEAR(TopOf(wall.Mesh, wall.Mesh.RingCount - 1u), 4.0f, 1.0e-4f);
    const std::vector<SplineVertex> riser = RiserVertices(wall.Mesh);
    ASSERT_EQ(riser.size(), 4u);
    // The second run runs south (-Z) away from the corner.
    for (const SplineVertex& v : riser)
        EXPECT_LT(v.Normal.z, -0.5f);
    EXPECT_EQ(MiswoundRiserTriangles(wall.Mesh), 0u);
}

// A round corner's fan stands at the higher level, and a Crease copy of its first
// ring at the lower level carries the step, so no band slopes from one level to
// the other and one riser closes it.
TEST(SplineStep, ARoundCornersStepStandsOnACreaseCopyOfTheFan)
{
    const SteppedWall wall = BuildStepped(kClimbing, SplineCornerStyle::Round);
    ASSERT_TRUE(wall.Mesh.IsValid());
    uint32 creases = 0;
    uint32 fanRings = 0;
    for (uint32 r = 0; r < wall.Mesh.RingCount; ++r)
    {
        const SplineStripStation& station = wall.Stations[r];
        creases += station.Join == SplineStationJoin::Crease ? 1u : 0u;
        if (station.Join == SplineStationJoin::Pivot)
        {
            ++fanRings;
            EXPECT_NEAR(TopOf(wall.Mesh, r), 5.0f, 1.0e-4f) << "ring " << r;
        }
    }
    EXPECT_EQ(creases, 1u);
    EXPECT_GT(fanRings, 0u);
    EXPECT_EQ(RiserVertices(wall.Mesh).size(), 4u);
    EXPECT_EQ(MiswoundRiserTriangles(wall.Mesh), 0u);
}

// A welded loop whose first and last runs stand at different levels steps at its
// seam: a Crease copy of the last ring at the first run's level closes it, and
// that copy's top meets the first ring's exactly, so the loop's two ends still
// weld. This loop's first run is level at 0 m and the other three at 1 m, so it
// steps up at point 1 and back down at the seam: two risers.
TEST(SplineStep, AWeldedLoopStepsAtItsSeam)
{
    Walk walk = WalkPolyline({V3{0.0f, 0.0f, 0.0f}, V3{0.0f, 0.0f, 10.0f}, V3{10.0f, 1.0f, 10.0f},
                              V3{10.0f, 1.0f, 0.0f}, V3{0.0f, 0.0f, 0.0f}},
                             0.5f, kHalfWidth);
    std::vector<float32> pointDistances;
    for (const SplineCornerSite& site : walk.Corners)
        pointDistances.push_back(walk.Stations[site.StationIndex].Distance);
    walk.Corners.insert(walk.Corners.begin(), SplineCornerSite{0u, 0u});
    CloseStationLoop(walk.Stations, kUp);
    (void)ApplyCorners(walk.Stations, walk.Corners, SplineCornerStyle::Mitre, kUp, true);
    StepTopsAtPoints(walk.Stations, pointDistances, kUp, true);

    const SplineStripStation& seam = walk.Stations.back();
    EXPECT_EQ(seam.Join, SplineStationJoin::Crease);
    SplineStripParams params;
    params.UOriginMetres = 0.0f;
    params.GenerateCaps = false;
    const SplineStripMesh mesh = BuildSplineStrip(Wall(2.0f * kHalfWidth, kWallHeight), walk.Stations, params);
    ASSERT_TRUE(mesh.IsValid());
    const uint32 lastRing = (mesh.RingCount - 1u) * mesh.RingVertexCount;
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
    {
        EXPECT_EQ(std::memcmp(&mesh.Vertices[k].Position, &mesh.Vertices[lastRing + k].Position,
                              sizeof(V3)),
                  0)
            << "slot " << k;
    }
    EXPECT_NEAR(TopOf(mesh, 0u), kWallHeight, 1.0e-4f);
    EXPECT_NEAR(TopOf(mesh, mesh.RingCount - 2u), 1.0f + kWallHeight, 1.0e-4f);
    EXPECT_EQ(RiserVertices(mesh).size(), 8u);
    EXPECT_EQ(MiswoundRiserTriangles(mesh), 0u);
}

// A stepped wall stands plumb: on a steep climb (3 m in 5 m, 31 degrees, then a
// corner) every top vertex stands straight above a base vertex. A ring left
// leaning with the grade carried its raised top along the run as well as up —
// 1.3 to 3 m on the terrain the wall was first shown on — and fanned the side
// faces between rings.
TEST(SplineStep, ASteppedWallOnASteepClimbStandsPlumb)
{
    const SteppedWall wall = BuildStepped({V3{0.0f, 0.0f, 0.0f}, V3{0.0f, 3.0f, 5.0f},
                                           V3{5.0f, 5.0f, 5.0f}},
                                          SplineCornerStyle::Mitre);
    ASSERT_TRUE(wall.Mesh.IsValid());
    float32 worstShift = 0.0f;
    for (uint32 r = 0; r < wall.Mesh.RingCount; ++r)
    {
        const float32 bottom = BottomOf(wall.Mesh, r);
        for (uint32 k = 0; k < wall.Mesh.RingVertexCount; ++k)
        {
            const V3& top = wall.Mesh.Vertices[r * wall.Mesh.RingVertexCount + k].Position;
            if (top.y - bottom < 1.0f)
                continue;
            float32 nearest = 1.0e9f;
            for (uint32 m = 0; m < wall.Mesh.RingVertexCount; ++m)
            {
                const V3& base = wall.Mesh.Vertices[r * wall.Mesh.RingVertexCount + m].Position;
                if (top.y - base.y < 1.0f)
                    continue;
                nearest = std::min(nearest, std::hypot(top.x - base.x, top.z - base.z));
            }
            worstShift = std::max(worstShift, nearest);
        }
    }
    EXPECT_LT(worstShift, 1.0e-3f);
    EXPECT_EQ(MiswoundRiserTriangles(wall.Mesh), 0u);
}
