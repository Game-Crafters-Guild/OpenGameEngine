// Mesh invariants for the region fill.
//
// These are the properties the marching-squares area fill claims to hold BY
// CONSTRUCTION, so each one is checked against real output rather than argued:
// a closed boundary, no T-junctions, no degenerate triangles, upward winding, a
// shoreline that moves continuously, and chunk seams that agree bit for bit.

#include "SplineGeometry/SplineFillMesher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using V3 = Mathematics::Vector3;

namespace
{

struct TestGround
{
    std::vector<float32> Heights;
    SplineGroundGrid Grid;

    TestGround(uint32 countX, uint32 countZ, float32 spacing, float32 originX, float32 originZ,
               const std::function<float32(float32, float32)>& height)
    {
        Grid.OriginX = originX;
        Grid.OriginZ = originZ;
        Grid.SpacingX = spacing;
        Grid.SpacingZ = spacing;
        Grid.CountX = countX;
        Grid.CountZ = countZ;
        Heights.resize(static_cast<size_t>(countX) * countZ);
        for (uint32 cz = 0; cz < countZ; ++cz)
            for (uint32 cx = 0; cx < countX; ++cx)
                Heights[Grid.Index(cx, cz)] = height(Grid.WorldX(cx), Grid.WorldZ(cz));
        Grid.Heights = Heights;
    }
};

std::vector<SplineStripStation> MakeStations(const std::vector<V3>& points)
{
    const V3 worldUp(0.0f, 1.0f, 0.0f);
    std::vector<SplineStripStation> stations(points.size());
    float32 distance = 0.0f;
    for (size_t i = 0; i < points.size(); ++i)
    {
        if (i > 0u)
        {
            const V3 step = points[i] - points[i - 1u];
            distance += std::sqrt(V3::Dot(step, step));
        }
        const V3 ahead = points[std::min(i + 1u, points.size() - 1u)];
        const V3 behind = points[i == 0u ? 0u : i - 1u];
        V3 forward = ahead - behind;
        const float32 length = std::sqrt(V3::Dot(forward, forward));
        forward = length > 1.0e-6f ? forward * (1.0f / length) : V3(0.0f, 0.0f, 1.0f);
        V3 right = V3::Cross(worldUp, V3(forward.x, 0.0f, forward.z));
        const float32 rightLength = std::sqrt(V3::Dot(right, right));
        right = rightLength > 1.0e-6f ? right * (1.0f / rightLength) : V3(1.0f, 0.0f, 0.0f);

        stations[i].Position = points[i];
        stations[i].Forward = forward;
        stations[i].Right = right;
        stations[i].Up = V3::Cross(forward, right);
        stations[i].Distance = distance;
    }
    return stations;
}

std::vector<SplineStripStation> StraightRun(float32 waterline, float32 fromZ, float32 toZ,
                                            float32 step)
{
    std::vector<V3> points;
    for (float32 z = fromZ; z <= toZ + 1.0e-4f; z += step)
        points.push_back(V3(0.0f, waterline, z));
    return MakeStations(points);
}

SplineFillChunkRange WholeGrid(const SplineGroundGrid& grid)
{
    return SplineFillChunkRange{0u, 0u, grid.CountX, grid.CountZ};
}

// A trench scene with an ordinary, irregular shoreline.
struct TrenchScene
{
    TestGround Ground;
    std::vector<SplineStripStation> Stations;
    SplineFillParams Params;
    SplineFillResult Field;

    explicit TrenchScene(float32 waterline = 0.5f)
        : Ground(61u, 61u, 0.5f, -15.0f, -15.0f, [](float32 x, float32 z)
          {
              // A wobbling channel, so the shoreline crosses cells at every angle.
              const float32 bed = std::sin(z * 0.4f) * 2.0f;
              const float32 across = std::abs(x - bed);
              return across <= 2.0f ? 0.0f : (across - 2.0f) * 6.0f;
          })
    {
        std::vector<V3> points;
        for (float32 z = -12.0f; z <= 12.0f; z += 0.5f)
            points.push_back(V3(std::sin(z * 0.4f) * 2.0f, waterline, z));
        Stations = MakeStations(points);
        Params.MaxHalfWidth = 10.0f;
        Params.EdgeDrop = 0.1f;
        Field = BuildSplineFillField(Stations, Ground.Grid, Params);
    }
};

// Undirected edge key for the watertightness check.
using EdgeKey = std::pair<uint32, uint32>;
EdgeKey MakeEdge(uint32 a, uint32 b) { return a < b ? EdgeKey{a, b} : EdgeKey{b, a}; }

} // namespace

TEST(SplineFillMesher, AFullyWetBlockIsExactlyTwoTrianglesPerCell)
{
    // Everything below the waterline and inside the reach: every cell is full.
    TestGround ground(11u, 11u, 1.0f, -5.0f, -5.0f,
                      [](float32, float32) { return -1.0f; });
    const std::vector<SplineStripStation> stations = StraightRun(0.5f, -4.0f, 4.0f, 0.5f);

    SplineFillParams params;
    params.MaxHalfWidth = 100.0f; // no corridor clipping inside this grid
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);
    ASSERT_EQ(field.Diagnostics.WetCorners, ground.Grid.CornerCount());

    SplineFillMeshParams meshParams;
    const SplineFillMesh mesh =
        BuildSplineFillMesh(field, ground.Grid, stations, WholeGrid(ground.Grid), meshParams);

    const uint32 cells = (ground.Grid.CountX - 1u) * (ground.Grid.CountZ - 1u);
    EXPECT_EQ(mesh.Indices.size(), static_cast<size_t>(cells) * 6u);
    // No shoreline anywhere, so no edge vertices: exactly the corner lattice.
    EXPECT_EQ(mesh.Vertices.size(), static_cast<size_t>(ground.Grid.CornerCount()));
}

TEST(SplineFillMesher, TheSurfaceIsClosedAndHasNoTJunctions)
{
    const TrenchScene scene;
    ASSERT_GT(scene.Field.Diagnostics.WetCorners, 0u);

    SplineFillMeshParams meshParams;
    const SplineFillMesh mesh = BuildSplineFillMesh(scene.Field, scene.Ground.Grid, scene.Stations,
                                                    WholeGrid(scene.Ground.Grid), meshParams);
    ASSERT_TRUE(mesh.IsValid());
    ASSERT_EQ(mesh.Indices.size() % 3u, 0u);

    // Every edge is used once (a boundary edge) or twice (an interior edge).
    // Three or more uses would mean a fold; and because shoreline vertices are
    // owned by the lattice edge they cross, a neighbouring cell cannot invent a
    // second vertex at a different position on the same edge — which is what a
    // T-junction IS.
    std::map<EdgeKey, uint32> edgeUses;
    for (size_t i = 0; i + 2u < mesh.Indices.size(); i += 3u)
    {
        ++edgeUses[MakeEdge(mesh.Indices[i], mesh.Indices[i + 1u])];
        ++edgeUses[MakeEdge(mesh.Indices[i + 1u], mesh.Indices[i + 2u])];
        ++edgeUses[MakeEdge(mesh.Indices[i + 2u], mesh.Indices[i])];
    }
    for (const auto& [edge, uses] : edgeUses)
        EXPECT_LE(uses, 2u) << "edge (" << edge.first << ", " << edge.second << ") used " << uses
                            << " times — the surface folds there";

    // No vertex sits ON another triangle's edge without being one of its
    // endpoints: the direct T-junction test, run over the boundary edges where a
    // T-junction could actually appear.
    std::vector<uint32> boundary;
    for (const auto& [edge, uses] : edgeUses)
        if (uses == 1u)
        {
            boundary.push_back(edge.first);
            boundary.push_back(edge.second);
        }
    ASSERT_FALSE(boundary.empty()) << "a filled trench must have a shoreline";

    for (size_t e = 0; e + 1u < boundary.size(); e += 2u)
    {
        const V3 a = mesh.Vertices[boundary[e]].Position;
        const V3 b = mesh.Vertices[boundary[e + 1u]].Position;
        const V3 ab{b.x - a.x, 0.0f, b.z - a.z};
        const float32 lengthSq = ab.x * ab.x + ab.z * ab.z;
        if (lengthSq < 1.0e-12f)
            continue;
        for (uint32 v = 0; v < mesh.Vertices.size(); ++v)
        {
            if (v == boundary[e] || v == boundary[e + 1u])
                continue;
            const V3 p = mesh.Vertices[v].Position;
            const float32 t = ((p.x - a.x) * ab.x + (p.z - a.z) * ab.z) / lengthSq;
            if (t <= 0.01f || t >= 0.99f)
                continue;
            const float32 dx = p.x - (a.x + ab.x * t);
            const float32 dz = p.z - (a.z + ab.z * t);
            EXPECT_GT(dx * dx + dz * dz, 1.0e-8f)
                << "vertex " << v << " lies on a boundary edge it is not an endpoint of";
        }
    }
}

TEST(SplineFillMesher, NoTriangleIsDegenerateAndEveryOneFacesUp)
{
    const TrenchScene scene;
    SplineFillMeshParams meshParams;
    const SplineFillMesh mesh = BuildSplineFillMesh(scene.Field, scene.Ground.Grid, scene.Stations,
                                                    WholeGrid(scene.Ground.Grid), meshParams);
    ASSERT_TRUE(mesh.IsValid());

    for (size_t i = 0; i + 2u < mesh.Indices.size(); i += 3u)
    {
        const uint32 i0 = mesh.Indices[i];
        const uint32 i1 = mesh.Indices[i + 1u];
        const uint32 i2 = mesh.Indices[i + 2u];
        ASSERT_TRUE(i0 != i1 && i1 != i2 && i0 != i2) << "repeated index at triangle " << i / 3u;

        const V3 p0 = mesh.Vertices[i0].Position;
        const V3 p1 = mesh.Vertices[i1].Position;
        const V3 p2 = mesh.Vertices[i2].Position;
        // Plan-view signed area: positive under the engine's winding rule, which
        // is that cross(p1 - p0, p2 - p0) points along the surface normal.
        const float32 twiceArea =
            (p1.z - p0.z) * (p2.x - p0.x) - (p1.x - p0.x) * (p2.z - p0.z);
        EXPECT_GT(twiceArea, 1.0e-9f)
            << "triangle " << i / 3u << " is degenerate or wound face-down in plan view";
    }
}

TEST(SplineFillMesher, TheShorelineMovesContinuouslyAsTheWaterlineIsSwept)
{
    // The anti-staircase claim, and the one a per-cell binary mask fails: a tiny
    // change in the waterline must move the shoreline by a tiny amount, not jump
    // it by a whole cell.
    // A steady ramp, closed downhill at x = -6 so the water has something to
    // stand behind; without that bank it would run off the ramp and there would
    // be no shoreline to sweep.
    const float32 spacing = 0.5f;
    TestGround ground(41u, 21u, spacing, -10.0f, -5.0f,
                      [](float32 x, float32) { return x <= -6.0f ? 10.0f : x * 0.5f; });

    float32 previousExtent = 0.0f;
    float32 largestJump = 0.0f;
    for (int step = 0; step <= 20; ++step)
    {
        const float32 waterline = static_cast<float32>(step) * 0.01f; // 1 cm at a time
        const std::vector<SplineStripStation> stations =
            StraightRun(waterline, -3.0f, 3.0f, 0.5f);
        SplineFillParams params;
        params.MaxHalfWidth = 8.0f;
        params.EdgeDrop = 0.1f;
        const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

        SplineFillMeshParams meshParams;
        const SplineFillMesh mesh =
            BuildSplineFillMesh(field, ground.Grid, stations, WholeGrid(ground.Grid), meshParams);
        ASSERT_TRUE(mesh.IsValid()) << "waterline " << waterline;

        // The furthest the water reaches up the ramp.
        float32 extent = -1.0e9f;
        for (const SplineVertex& vertex : mesh.Vertices)
            extent = std::max(extent, vertex.Position.x);

        if (step > 0)
            largestJump = std::max(largestJump, std::abs(extent - previousExtent));
        previousExtent = extent;
    }

    // Ground rises 0.5 m per metre, so 1 cm of waterline should move the
    // shoreline 2 cm. A cell-quantised mask would sit still and then jump the
    // full 0.5 m cell.
    EXPECT_LT(largestJump, spacing * 0.5f)
        << "the shoreline jumped by " << largestJump << " m — that is a staircase, not a contour";
    EXPECT_GT(largestJump, 0.0f) << "the shoreline did not move at all; the sweep is not testing";
}

TEST(SplineFillMesher, TheRimSitsEdgeDropBelowTheWaterlineAndInteriorVerticesSitOnIt)
{
    const TrenchScene scene;
    SplineFillMeshParams meshParams;
    meshParams.EdgeDrop = scene.Params.EdgeDrop;
    const SplineFillMesh mesh = BuildSplineFillMesh(scene.Field, scene.Ground.Grid, scene.Stations,
                                                    WholeGrid(scene.Ground.Grid), meshParams);
    ASSERT_TRUE(mesh.IsValid());

    // Corner vertices land exactly on a lattice point and carry the waterline;
    // shoreline vertices sit off-lattice and EdgeDrop lower.
    uint32 interior = 0;
    uint32 rim = 0;
    for (const SplineVertex& vertex : mesh.Vertices)
    {
        const float32 gx = (vertex.Position.x - scene.Ground.Grid.OriginX) /
                           scene.Ground.Grid.SpacingX;
        const float32 gz = (vertex.Position.z - scene.Ground.Grid.OriginZ) /
                           scene.Ground.Grid.SpacingZ;
        const bool onLattice = std::abs(gx - std::round(gx)) < 1.0e-4f &&
                               std::abs(gz - std::round(gz)) < 1.0e-4f;
        if (onLattice)
        {
            ++interior;
            EXPECT_NEAR(vertex.Position.y, scene.Stations.front().Position.y, 1.0e-3f);
        }
        else
        {
            ++rim;
            EXPECT_NEAR(vertex.Position.y,
                        scene.Stations.front().Position.y - meshParams.EdgeDrop, 1.0e-3f);
        }
    }
    EXPECT_GT(interior, 0u);
    EXPECT_GT(rim, 0u);
}

// Bit-identity at a chunk seam is load-bearing for SHADING now, not only for
// cracks: uv0 is a measurement the water shader reads, so a seam that agreed
// about position and disagreed about arc or depth would draw a foam line or a
// ripple-phase break straight down a lattice column.
TEST(SplineFillMesher, ChunkBoundariesProduceBitIdenticalVerticesFromBothSides)
{
    const TrenchScene scene;
    SplineFillMeshParams meshParams;

    // Two blocks sharing the corner column at x index 30.
    SplineFillChunkRange left{0u, 0u, 31u, scene.Ground.Grid.CountZ};
    SplineFillChunkRange right{30u, 0u, scene.Ground.Grid.CountX, scene.Ground.Grid.CountZ};

    const SplineFillMesh a =
        BuildSplineFillMesh(scene.Field, scene.Ground.Grid, scene.Stations, left, meshParams);
    const SplineFillMesh b =
        BuildSplineFillMesh(scene.Field, scene.Ground.Grid, scene.Stations, right, meshParams);
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(b.IsValid());

    // Collect every vertex either side placed on the shared lattice column, keyed
    // by its Z so the two sides can be compared entry for entry.
    const float32 seamX = scene.Ground.Grid.WorldX(30u);
    const auto onSeam = [&](const SplineFillMesh& mesh)
    {
        std::map<float32, SplineVertex> byZ;
        for (const SplineVertex& vertex : mesh.Vertices)
            if (vertex.Position.x == seamX)
                byZ[vertex.Position.z] = vertex;
        return byZ;
    };
    const auto seamA = onSeam(a);
    const auto seamB = onSeam(b);
    ASSERT_FALSE(seamA.empty()) << "the seam column must carry vertices, or nothing is compared";

    uint32 compared = 0;
    for (const auto& [z, vertex] : seamA)
    {
        const auto found = seamB.find(z);
        if (found == seamB.end())
            continue;
        ++compared;
        // BIT-identical, not merely close: both sides evaluate the same field at
        // the same corner with the same expression, so a crack cannot open.
        EXPECT_EQ(vertex.Position.x, found->second.Position.x);
        EXPECT_EQ(vertex.Position.y, found->second.Position.y);
        EXPECT_EQ(vertex.Position.z, found->second.Position.z);
        // uv0 must agree for the same reason and by the same argument: it is a
        // pure function of the corners the edge spans, with no chunk-local term.
        EXPECT_EQ(vertex.UV.x, found->second.UV.x) << "arc disagrees across the chunk seam at z=" << z;
        EXPECT_EQ(vertex.UV.y, found->second.UV.y) << "depth disagrees across the chunk seam at z=" << z;
    }
    EXPECT_GT(compared, 5u) << "too few shared seam vertices to call this a seam test";
}

// uv0 carries a MEASUREMENT -- arc along the flow and still-water depth, both in
// world metres -- rather than a texture chart. A water shader cannot recover
// either from position, and one that guesses depth from a world axis draws its
// shoreline wherever that axis happens to cross instead of where the bank is.
//
// A basin with a KNOWN waterline over a KNOWN bed, so both channels have an
// exact expected value at every vertex rather than a property to argue about.
TEST(SplineFillMesher, Uv0IsArcAlongFlowAndStillWaterDepthInMetres)
{
    // Bed slopes across the channel, so depth VARIES and a constant cannot pass:
    // 2 m under the centreline, 0.5 m at the lip, wall beyond it.
    const auto bedHeight = [](float32 x) { return -2.0f + std::abs(x) * 0.5f; };
    TestGround ground(41u, 41u, 0.5f, -10.0f, -10.0f, [&](float32 x, float32)
                      { return std::abs(x) <= 3.0f ? bedHeight(x) : 10.0f; });
    // Flat waterline at y = 0, running along +Z from z = -8, so the expected arc
    // at any vertex is simply position.z + 8.
    const std::vector<SplineStripStation> stations = StraightRun(0.0f, -8.0f, 8.0f, 0.5f);

    SplineFillParams params;
    params.MaxHalfWidth = 8.0f;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    SplineFillMeshParams meshParams;
    meshParams.EdgeDrop = params.EdgeDrop;
    const SplineFillMesh mesh =
        BuildSplineFillMesh(field, ground.Grid, stations, WholeGrid(ground.Grid), meshParams);
    ASSERT_TRUE(mesh.IsValid());

    uint32 surfaceVertices = 0;
    uint32 shorelineVertices = 0;
    for (const SplineVertex& vertex : mesh.Vertices)
    {
        // Arc is affine in z here and the shoreline interpolates on the same t as
        // the position, so this holds for surface and shoreline vertices alike.
        EXPECT_NEAR(vertex.UV.x, vertex.Position.z + 8.0f, 1.0e-3f)
            << "uv0.x must be arc length along the flow in metres";

        // A surface vertex sits ON the waterline; a shoreline vertex is buried
        // EdgeDrop below it. The two carry different exact depths.
        if (vertex.Position.y > -0.5f * params.EdgeDrop)
        {
            ++surfaceVertices;
            EXPECT_NEAR(vertex.UV.y, -bedHeight(vertex.Position.x), 1.0e-3f)
                << "uv0.y must be waterline minus bed, in metres, at x = "
                << vertex.Position.x;
        }
        else
        {
            ++shorelineVertices;
            // Inside is depth + EdgeDrop wherever the GROUND decides the
            // crossing, and it does everywhere in this basin, so interpolating
            // depth on the crossing fraction lands on exactly -EdgeDrop. That is
            // what puts a shader's zero-clamped foam band precisely on the
            // waterline however the bank wanders.
            EXPECT_NEAR(vertex.UV.y, -params.EdgeDrop, 1.0e-3f)
                << "a ground-decided shoreline vertex must read exactly -EdgeDrop";
        }

        // U runs along the flow, so the tangent is Forward (+Z here) and the
        // handedness is the sweep's +1.
        EXPECT_NEAR(vertex.Normal.y, 1.0f, 1.0e-3f);
        EXPECT_NEAR(vertex.Tangent.z, 1.0f, 1.0e-3f);
        EXPECT_FLOAT_EQ(vertex.Tangent.w, 1.0f);
    }
    EXPECT_GT(surfaceVertices, 100u) << "too few surface vertices to call this a depth test";
    EXPECT_GT(shorelineVertices, 20u) << "too few shoreline vertices to call this a rim test";
}

// The claim uv0.x makes is ARC ALONG THE CHANNEL, not distance along an axis.
// The two are only equal on a straight run, so the discriminating case is a
// meander: its arc must outrun its axial extent by the length the wobble adds.
TEST(SplineFillMesher, Uv0XFollowsTheChannelRatherThanAWorldAxis)
{
    const TrenchScene scene;
    ASSERT_TRUE(scene.Field.HasRegion());

    SplineFillMeshParams meshParams;
    meshParams.EdgeDrop = scene.Params.EdgeDrop;
    const SplineFillMesh mesh = BuildSplineFillMesh(scene.Field, scene.Ground.Grid, scene.Stations,
                                                    WholeGrid(scene.Ground.Grid), meshParams);
    ASSERT_TRUE(mesh.IsValid());

    float32 minArc = mesh.Vertices.front().UV.x;
    float32 maxArc = minArc;
    float32 minZ = mesh.Vertices.front().Position.z;
    float32 maxZ = minZ;
    for (const SplineVertex& vertex : mesh.Vertices)
    {
        minArc = std::min(minArc, vertex.UV.x);
        maxArc = std::max(maxArc, vertex.UV.x);
        minZ = std::min(minZ, vertex.Position.z);
        maxZ = std::max(maxZ, vertex.Position.z);
        // Global depth bound: Inside is a MINIMUM over three terms, so
        // Inside <= depth + EdgeDrop at both ends of every crossing, and the
        // interpolant therefore cannot fall below -EdgeDrop anywhere -- whichever
        // term decided the shoreline. A shader clamping at zero can never be
        // handed a rim that reads as deeper shore than the ground says.
        EXPECT_GE(vertex.UV.y, -scene.Params.EdgeDrop - 1.0e-4f);
    }
    // The centreline is x = 2 sin(0.4 z), whose arc exceeds its z-extent by ~14%.
    EXPECT_GT(maxArc - minArc, (maxZ - minZ) * 1.05f)
        << "uv0.x tracks a world axis, not the channel: a meander must add length";

    // Monotone downstream: the run advances in +Z throughout, so an upstream
    // slice of the region must carry strictly smaller arc than a downstream one.
    const float32 upstreamEdge = minZ + (maxZ - minZ) * 0.25f;
    const float32 downstreamEdge = minZ + (maxZ - minZ) * 0.75f;
    float32 worstUpstream = -std::numeric_limits<float32>::infinity();
    float32 bestDownstream = std::numeric_limits<float32>::infinity();
    uint32 upstream = 0;
    uint32 downstream = 0;
    for (const SplineVertex& vertex : mesh.Vertices)
    {
        if (vertex.Position.z <= upstreamEdge)
        {
            worstUpstream = std::max(worstUpstream, vertex.UV.x);
            ++upstream;
        }
        else if (vertex.Position.z >= downstreamEdge)
        {
            bestDownstream = std::min(bestDownstream, vertex.UV.x);
            ++downstream;
        }
    }
    ASSERT_GT(upstream, 20u);
    ASSERT_GT(downstream, 20u);
    EXPECT_LT(worstUpstream, bestDownstream) << "uv0.x must increase downstream";
}

// Depth is a property of the WATER, so it must not move when the mesh does.
// Two runs of the same basin at different placer origins would otherwise
// disagree; the fill stamps world metres, and this pins that.
TEST(SplineFillMesher, DepthIsIndependentOfWhereTheLatticeStarts)
{
    const auto build = [](float32 originShift)
    {
        TestGround ground(41u, 41u, 0.5f, -10.0f + originShift, -10.0f,
                          [originShift](float32 x, float32)
                          {
                              const float32 local = x - originShift;
                              return std::abs(local) <= 3.0f ? -1.5f : 10.0f;
                          });
        std::vector<V3> points;
        for (float32 z = -8.0f; z <= 8.0f + 1.0e-4f; z += 0.5f)
            points.push_back(V3(originShift, 0.0f, z));
        const std::vector<SplineStripStation> stations = MakeStations(points);

        SplineFillParams params;
        params.MaxHalfWidth = 8.0f;
        params.EdgeDrop = 0.1f;
        const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

        SplineFillMeshParams meshParams;
        meshParams.EdgeDrop = params.EdgeDrop;
        return BuildSplineFillMesh(field, ground.Grid, stations, WholeGrid(ground.Grid),
                                   meshParams);
    };

    const SplineFillMesh here = build(0.0f);
    const SplineFillMesh shifted = build(2.5f); // a whole number of cells along
    ASSERT_TRUE(here.IsValid());
    ASSERT_EQ(here.Vertices.size(), shifted.Vertices.size());

    for (size_t i = 0; i < here.Vertices.size(); ++i)
    {
        EXPECT_FLOAT_EQ(here.Vertices[i].UV.y, shifted.Vertices[i].UV.y)
            << "depth moved with the lattice origin at vertex " << i;
        EXPECT_FLOAT_EQ(here.Vertices[i].UV.x, shifted.Vertices[i].UV.x)
            << "arc moved with the lattice origin at vertex " << i;
    }
}

TEST(SplineFillMesher, ABlockWithNoWetCellProducesNothing)
{
    const TrenchScene scene;
    // A block well clear of the channel.
    const SplineFillChunkRange empty{0u, 0u, 4u, 4u};
    SplineFillMeshParams meshParams;
    const SplineFillMesh mesh =
        BuildSplineFillMesh(scene.Field, scene.Ground.Grid, scene.Stations, empty, meshParams);
    EXPECT_FALSE(mesh.IsValid());
    EXPECT_TRUE(mesh.Vertices.empty());
    EXPECT_TRUE(mesh.Indices.empty());
}

TEST(SplineFillMesher, CarvingCoversEveryCellExactlyOnce)
{
    TestGround ground(21u, 17u, 0.5f, 0.0f, 0.0f, [](float32, float32) { return 0.0f; });
    const std::vector<SplineFillChunkRange> chunks = CarveFillChunks(ground.Grid, 8u);
    ASSERT_FALSE(chunks.empty());

    std::set<std::pair<uint32, uint32>> covered;
    for (const SplineFillChunkRange& chunk : chunks)
    {
        for (uint32 cz = chunk.MinCornerZ; cz + 1u < chunk.MaxCornerZ; ++cz)
        {
            for (uint32 cx = chunk.MinCornerX; cx + 1u < chunk.MaxCornerX; ++cx)
            {
                const auto cell = std::make_pair(cx, cz);
                EXPECT_TRUE(covered.insert(cell).second)
                    << "cell (" << cx << ", " << cz << ") is carved into two chunks";
            }
        }
    }
    const size_t cells = static_cast<size_t>(ground.Grid.CountX - 1u) * (ground.Grid.CountZ - 1u);
    EXPECT_EQ(covered.size(), cells) << "the carve left cells uncovered";
}
