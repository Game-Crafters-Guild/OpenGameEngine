#include "SplineGeometry/SplineStripBuilder.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;

namespace
{

using V3 = Mathematics::Vector3;

constexpr float32 kPi = 3.14159265358979323846f;

using HeightField = std::function<float32(float32 x, float32 z)>;

V3 Normalize(const V3& v)
{
    const float32 len = std::sqrt(V3::Dot(v, v));
    return v * (1.0f / len);
}

// Walks a draped centerline by TRUE 3-D length and emits one roll-free station
// per sample -- the same construction the editor controller performs once the
// conform raycast has replaced this height function.
//
// The frame is "racked": Forward follows the draped grade, so the sweep pitches
// with the ground. Right is the ground-plane lateral axis, matching the
// placement controller's convention that Cross(worldUp, forward) is the RIGHT
// of travel in this LH +Y-up engine.
std::vector<SplineStripStation> DrapedStations(const HeightField& height, float32 fromZ,
                                               float32 toZ, uint32 count, float32 halfWidth)
{
    std::vector<V3> points(count);
    for (uint32 i = 0; i < count; ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(count - 1u);
        const float32 z = fromZ + (toZ - fromZ) * t;
        points[i] = V3{0.0f, height(0.0f, z), z};
    }

    std::vector<SplineStripStation> stations(count);
    float32 travelled = 0.0f;
    const V3 worldUp{0.0f, 1.0f, 0.0f};
    for (uint32 i = 0; i < count; ++i)
    {
        if (i > 0u)
            travelled += std::sqrt(V3::Dot(points[i] - points[i - 1u], points[i] - points[i - 1u]));

        const V3 ahead = points[std::min(i + 1u, count - 1u)];
        const V3 behind = points[i == 0u ? 0u : i - 1u];
        const V3 forward = Normalize(ahead - behind);
        const V3 right = Normalize(V3::Cross(worldUp, V3{forward.x, 0.0f, forward.z}));

        SplineStripStation& s = stations[i];
        s.Position = points[i];
        s.Forward = forward;
        s.Right = right;
        s.Up = V3::Cross(forward, right);
        s.HalfWidthLeft = halfWidth;
        s.HalfWidthRight = halfWidth;
        s.Distance = travelled;
    }
    return stations;
}

std::vector<SplineStripStation> StraightStations(uint32 count, float32 step, float32 halfWidth)
{
    return DrapedStations([](float32, float32) { return 0.0f; }, 0.0f,
                          step * static_cast<float32>(count - 1u), count, halfWidth);
}

SplineProfile FlatRibbon(float32 width)
{
    SplineProfileParams p;
    p.Shape = SplineProfileShape::Bevel;
    p.Width = width;
    p.EdgeDrop = 0.0f;
    p.EdgeInset = 0.0f;
    return BuildProfile(p);
}

SplineProfile WallProfile(float32 thickness, float32 height)
{
    SplineProfileParams p;
    p.Shape = SplineProfileShape::Rectangle;
    p.Width = thickness;
    p.Height = height;
    return BuildProfile(p);
}

// The engine's front-face rule, read off a primitive that renders correctly
// today (PrimitiveGenerator::GeneratePlane, an up-facing quad): for a triangle
// (a, b, c), cross(b - a, c - a) is the outward normal. It is also the CCW
// winding a Jolt mesh shape expects, so collision inherits whatever this pins.
V3 TriangleNormal(const SplineStripMesh& mesh, size_t triangle)
{
    const V3& a = mesh.Vertices[mesh.Indices[triangle * 3u + 0u]].Position;
    const V3& b = mesh.Vertices[mesh.Indices[triangle * 3u + 1u]].Position;
    const V3& c = mesh.Vertices[mesh.Indices[triangle * 3u + 2u]].Position;
    return V3::Cross(b - a, c - a);
}

} // namespace

TEST(SplineStrip, AdjacentStationsShareTheirVerticesSoThereIsNoSeamToMisalign)
{
    const SplineProfile profile = FlatRibbon(2.0f);
    const auto stations = StraightStations(8u, 1.0f, 1.0f);

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    EXPECT_EQ(mesh.RingCount, 8u);
    // An open two-point profile splits both ends (one adjacent edge each), so
    // the ring is exactly two vertices wide.
    EXPECT_EQ(mesh.RingVertexCount, 2u);
    // No cap vertices for an open profile: the vertex buffer is rings and
    // nothing else. That total is the weld -- a concatenation of independent
    // quads would need twice as many.
    EXPECT_EQ(mesh.Vertices.size(), static_cast<size_t>(mesh.RingCount) * mesh.RingVertexCount);

    // Every interior ring is referenced by triangles on BOTH sides, which is
    // what "welded" means operationally.
    std::vector<int> behind(mesh.RingCount, 0);
    std::vector<int> ahead(mesh.RingCount, 0);
    for (size_t t = 0; t < mesh.Indices.size() / 3u; ++t)
    {
        uint32 lowRing = std::numeric_limits<uint32>::max();
        uint32 highRing = 0u;
        for (uint32 k = 0; k < 3u; ++k)
        {
            const uint32 ring = mesh.Indices[t * 3u + k] / mesh.RingVertexCount;
            lowRing = std::min(lowRing, ring);
            highRing = std::max(highRing, ring);
        }
        ASSERT_EQ(highRing, lowRing + 1u) << "a triangle spans more than one station interval";
        ahead[lowRing] += 1;
        behind[highRing] += 1;
    }
    for (uint32 r = 1u; r + 1u < mesh.RingCount; ++r)
    {
        EXPECT_GT(behind[r], 0) << "ring " << r << " is not welded to the ring behind it";
        EXPECT_GT(ahead[r], 0) << "ring " << r << " is not welded to the ring ahead of it";
    }
}

TEST(SplineStrip, AFlatRibbonOnLevelGroundFacesTheSky)
{
    // Gate claim 1 of the design's measurement slice, and the classic first-try
    // failure of any sweep. Note the editor's width-band GIZMO winds its quads
    // the other way -- it gets away with it because it draws unlit and
    // two-sided, so its winding is not evidence for a lit mesh.
    const SplineProfile profile = FlatRibbon(3.0f);
    const auto stations = StraightStations(5u, 2.0f, 1.5f);

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    ASSERT_GT(mesh.Indices.size(), 0u);
    for (size_t t = 0; t < mesh.Indices.size() / 3u; ++t)
    {
        const V3 n = TriangleNormal(mesh, t);
        EXPECT_GT(n.y, 0.0f) << "triangle " << t << " faces the ground";
    }
    for (const SplineVertex& v : mesh.Vertices)
        EXPECT_NEAR(v.Normal.y, 1.0f, 1e-5f);
}

TEST(SplineStrip, AWallFacesOutwardOnEverySide)
{
    const SplineProfile profile = WallProfile(1.0f, 3.0f);
    const auto stations = StraightStations(4u, 2.0f, 0.5f);

    SplineStripParams params;
    params.GenerateCaps = false; // side faces only, so "outward" means lateral
    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.IsValid());
    const V3 centre{0.0f, 1.5f, 0.0f};
    for (size_t t = 0; t < mesh.Indices.size() / 3u; ++t)
    {
        const V3 n = TriangleNormal(mesh, t);
        const V3& a = mesh.Vertices[mesh.Indices[t * 3u + 0u]].Position;
        const V3& b = mesh.Vertices[mesh.Indices[t * 3u + 1u]].Position;
        const V3& c = mesh.Vertices[mesh.Indices[t * 3u + 2u]].Position;
        const V3 mid = (a + b + c) * (1.0f / 3.0f);
        // Compare only in the cross-section plane: the wall is long in Z and
        // its centre line runs through it.
        const V3 outward{mid.x - centre.x, mid.y - centre.y, 0.0f};
        EXPECT_GT(n.x * outward.x + n.y * outward.y, 0.0f)
            << "wall triangle " << t << " faces into the solid";
    }
}

TEST(SplineStrip, UIsArcLengthInMetresAndStrictlyIncreasesAlongTheRun)
{
    const SplineProfile profile = FlatRibbon(2.0f);
    // A descent makes authored arc length and draped length differ, which is
    // exactly when a normalized or authored-arc U would compress the texture.
    const auto stations = DrapedStations([](float32, float32 z) { return -0.5f * z; }, 0.0f, 20.0f,
                                         21u, 1.0f);

    SplineStripParams params;
    params.TilesPerMetreU = 0.25f;
    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.IsValid());
    ASSERT_EQ(mesh.RingCount, 21u);
    for (uint32 r = 1u; r < mesh.RingCount; ++r)
    {
        const float32 previous = mesh.Vertices[(r - 1u) * mesh.RingVertexCount].UV.x;
        const float32 current = mesh.Vertices[r * mesh.RingVertexCount].UV.x;
        EXPECT_GT(current, previous) << "U went backwards at ring " << r;
    }

    // U is the DRAPED distance, not the authored one: over a 20 m run at a
    // 1:2 grade the ground distance is 20*sqrt(1.25).
    const float32 expectedLength = 20.0f * std::sqrt(1.25f);
    const float32 finalU = mesh.Vertices[(mesh.RingCount - 1u) * mesh.RingVertexCount].UV.x;
    EXPECT_NEAR(finalU, expectedLength * 0.25f, 1e-3f);
    EXPECT_NEAR(mesh.Vertices[0].UV.x, 0.0f, 1e-6f);
}

TEST(SplineStrip, VSpansTheProfilePerimeterInMetresAcrossTheStrip)
{
    const SplineProfile profile = FlatRibbon(3.0f);
    const auto stations = StraightStations(3u, 1.0f, 1.5f);

    SplineStripParams params;
    params.TilesPerMetreV = 0.5f;
    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.IsValid());
    ASSERT_EQ(mesh.RingVertexCount, 2u);
    EXPECT_NEAR(mesh.Vertices[0].UV.y, 0.0f, 1e-6f);
    // A 3 m wide ribbon crosses 3 m of perimeter, tiled at 0.5 per metre.
    EXPECT_NEAR(mesh.Vertices[1].UV.y, 1.5f, 1e-5f);
}

TEST(SplineStrip, VFollowsTheWidthChannelSoATaperKeepsItsTexelDensity)
{
    const SplineProfile profile = FlatRibbon(2.0f); // nominal half-width 1 m
    auto stations = StraightStations(2u, 4.0f, 1.0f);
    stations[1].HalfWidthLeft = 3.0f; // taper out to a 6 m ribbon
    stations[1].HalfWidthRight = 3.0f;

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    // Narrow end crosses 2 m, wide end 6 m. A V frozen at the authored
    // perimeter would stretch the texture across the wide end instead.
    EXPECT_NEAR(mesh.Vertices[1].UV.y, 2.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[mesh.RingVertexCount + 1u].UV.y, 6.0f, 1e-5f);
}

TEST(SplineStrip, TheWidthChannelScalesLaterallyWithoutChangingHeight)
{
    const SplineProfile profile = WallProfile(2.0f, 4.0f); // nominal half-width 1 m
    auto stations = StraightStations(2u, 2.0f, 1.0f);
    stations[1].HalfWidthLeft = 2.5f;
    stations[1].HalfWidthRight = 2.5f;

    SplineStripParams params;
    params.WidthScale = SplineProfileScale::LateralOnly;
    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.IsValid());
    const auto ringExtent = [&](uint32 ring, bool vertical)
    {
        float32 lo = std::numeric_limits<float32>::max();
        float32 hi = std::numeric_limits<float32>::lowest();
        for (uint32 s = 0; s < mesh.RingVertexCount; ++s)
        {
            const V3& p = mesh.Vertices[ring * mesh.RingVertexCount + s].Position;
            const float32 value = vertical ? p.y : p.x;
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }
        return hi - lo;
    };

    EXPECT_NEAR(ringExtent(0u, false), 2.0f, 1e-5f);
    EXPECT_NEAR(ringExtent(1u, false), 5.0f, 1e-5f); // a road that widens...
    EXPECT_NEAR(ringExtent(0u, true), 4.0f, 1e-5f);
    EXPECT_NEAR(ringExtent(1u, true), 4.0f, 1e-5f);  // ...does not get taller
}

TEST(SplineStrip, UniformScaleRaisesARampartThatWidens)
{
    const SplineProfile profile = WallProfile(2.0f, 4.0f);
    auto stations = StraightStations(2u, 2.0f, 1.0f);
    stations[1].HalfWidthLeft = 2.0f;
    stations[1].HalfWidthRight = 2.0f;

    SplineStripParams params;
    params.WidthScale = SplineProfileScale::Uniform;
    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.IsValid());
    float32 tallest = 0.0f;
    for (uint32 s = 0; s < mesh.RingVertexCount; ++s)
        tallest = std::max(tallest, mesh.Vertices[mesh.RingVertexCount + s].Position.y);
    EXPECT_NEAR(tallest, 8.0f, 1e-5f);
}

TEST(SplineStrip, RollBanksTheCrossSectionAboutTravel)
{
    const SplineProfile profile = FlatRibbon(2.0f);
    auto stations = StraightStations(2u, 2.0f, 1.0f);
    stations[0].RollRadians = kPi * 0.5f;
    stations[1].RollRadians = kPi * 0.5f;

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    // A quarter turn about Forward stands the ribbon on edge: its lateral
    // extent becomes vertical. Matches the evaluator's own roll convention, so
    // the channel means one thing across the spline stack.
    EXPECT_NEAR(mesh.Vertices[0].Position.x, 0.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[0].Position.y, -1.0f, 1e-5f);
    EXPECT_NEAR(mesh.Vertices[1].Position.y, 1.0f, 1e-5f);
}

TEST(SplineStrip, ClosedProfilesCapAndOpenOnesDoNot)
{
    const auto stations = StraightStations(3u, 1.0f, 0.5f);

    const SplineStripMesh wall = BuildSplineStrip(WallProfile(1.0f, 2.0f), stations, {});
    const SplineStripMesh path = BuildSplineStrip(FlatRibbon(2.0f), stations, {});

    ASSERT_TRUE(wall.IsValid());
    ASSERT_TRUE(path.IsValid());
    // Caps are DERIVED from the profile, not authored: capping an open strip
    // would emit a degenerate sliver, so there is no setting to get wrong.
    EXPECT_GT(wall.Vertices.size(), static_cast<size_t>(wall.RingCount) * wall.RingVertexCount);
    EXPECT_EQ(path.Vertices.size(), static_cast<size_t>(path.RingCount) * path.RingVertexCount);

    // The two caps face opposite ways, and away from the strip.
    bool sawBack = false;
    bool sawFront = false;
    for (const SplineVertex& v : wall.Vertices)
    {
        if (v.Normal.z < -0.99f)
            sawBack = true;
        if (v.Normal.z > 0.99f)
            sawFront = true;
    }
    EXPECT_TRUE(sawBack);
    EXPECT_TRUE(sawFront);
}

TEST(SplineStrip, EveryRingVertexOfAClosedProfileIsReferencedByTheStitch)
{
    // The wrap edge's V seam rides point 0's incoming slot when the point is
    // hard (true of every closed preset): a duplicate slot there would be an
    // orphaned vertex per ring — never indexed, only wasting a vertex-buffer
    // column on every ring of every wall.
    SplineStripParams params;
    params.GenerateCaps = false;
    const SplineStripMesh wall =
        BuildSplineStrip(WallProfile(1.0f, 2.0f), StraightStations(3u, 1.0f, 0.5f), params);

    ASSERT_TRUE(wall.IsValid());
    // Four hard corners split into exactly eight slots; the seam adds no ninth.
    EXPECT_EQ(wall.RingVertexCount, 8u);
    std::vector<bool> referenced(wall.Vertices.size(), false);
    for (uint32 index : wall.Indices)
        referenced[index] = true;
    for (size_t v = 0; v < referenced.size(); ++v)
        EXPECT_TRUE(referenced[v]) << "vertex " << v << " is orphaned";
}

TEST(SplineStrip, CapUVsTileAtTheSidesDensityOnTheScaledCrossSection)
{
    // WallProfile(2,4) is authored at nominal half-width 1 m; stations carry
    // half-width 2 m, so the built cross-section is twice the authored one.
    // Cap UVs measured on the UNSCALED profile would tile at half the sides'
    // density.
    const auto stations = StraightStations(3u, 1.0f, 2.0f);
    SplineStripParams params;
    params.TilesPerMetreV = 0.5f;
    const SplineStripMesh wall = BuildSplineStrip(WallProfile(2.0f, 4.0f), stations, params);

    ASSERT_TRUE(wall.IsValid());
    const size_t ringVerts = static_cast<size_t>(wall.RingCount) * wall.RingVertexCount;
    ASSERT_GT(wall.Vertices.size(), ringVerts) << "no cap vertices were generated";
    float32 maxU = 0.0f;
    for (size_t v = ringVerts; v < wall.Vertices.size(); ++v)
        maxU = std::max(maxU, std::abs(wall.Vertices[v].UV.x));
    // The scaled lateral extent is +/-2 m, tiled at 0.5 tiles per metre.
    EXPECT_NEAR(maxU, 1.0f, 1e-5f);
}

TEST(SplineStrip, TheClosedLoopSnapRoundsTheRunToAWholeNumberOfTiles)
{
    const SplineProfile profile = FlatRibbon(2.0f);
    const auto stations = StraightStations(11u, 1.03f, 1.0f); // 10.3 m run

    SplineStripParams params;
    params.TilesPerMetreU = 1.0f;
    params.SnapUForClosedLoop = true;
    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, params);

    ASSERT_TRUE(mesh.IsValid());
    const float32 finalU = mesh.Vertices[(mesh.RingCount - 1u) * mesh.RingVertexCount].UV.x;
    EXPECT_NEAR(finalU, std::round(finalU), 1e-4f);
    EXPECT_NEAR(finalU, 10.0f, 1e-4f);
}

TEST(SplineStrip, ChunksSplitFromOneRunShareTheirBoundaryU)
{
    // The extrude controller's shape: one run cut into spans that SHARE their
    // boundary station, every station carrying its GLOBAL run distance. The
    // shared ring is the last ring of the front chunk and the first ring of the
    // back chunk; with a per-chunk U origin the texture restarts at 0 there.
    const SplineProfile profile = FlatRibbon(2.0f);
    const auto stations = StraightStations(11u, 1.03f, 1.0f);
    const std::span<const SplineStripStation> frontSpan(stations.data(), 6u);
    const std::span<const SplineStripStation> backSpan(stations.data() + 5u, 6u);

    SplineStripParams params;
    params.TilesPerMetreU = 1.0f;
    params.UOriginMetres = 0.0f; // one U axis across every chunk of the run

    const SplineStripMesh front = BuildSplineStrip(profile, frontSpan, params);
    const SplineStripMesh back = BuildSplineStrip(profile, backSpan, params);

    ASSERT_TRUE(front.IsValid());
    ASSERT_TRUE(back.IsValid());
    const float32 frontBoundaryU =
        front.Vertices[(front.RingCount - 1u) * front.RingVertexCount].UV.x;
    const float32 backBoundaryU = back.Vertices[0].UV.x;
    EXPECT_FLOAT_EQ(frontBoundaryU, backBoundaryU);
    EXPECT_GT(backBoundaryU, 0.0f) << "the back chunk restarted its U axis";

    // The default preserves the standalone behaviour: a builder user handing a
    // span that starts mid-run still gets a texture that starts at 0.
    const SplineStripMesh lone = BuildSplineStrip(profile, backSpan, SplineStripParams{});
    ASSERT_TRUE(lone.IsValid());
    EXPECT_FLOAT_EQ(lone.Vertices[0].UV.x, 0.0f);
}

TEST(SplineStrip, AChunkedClosedLoopSnapsItsTileDensityOnceOverTheWholeRun)
{
    // A chunking caller snaps the closed-loop density ONCE over the full run
    // and passes the result pre-multiplied, with the builder's own snap off:
    // per-chunk snapping would round each chunk against its own length,
    // drifting density chunk by chunk and leaving a partial tile at the seam.
    const SplineProfile profile = FlatRibbon(2.0f);
    const auto stations = StraightStations(11u, 1.03f, 1.0f); // 10.3 m run
    const float32 runLength = stations.back().Distance;

    SplineStripParams params;
    params.TilesPerMetreU = std::round(runLength * 1.0f) / runLength; // 10 whole tiles
    params.SnapUForClosedLoop = false;
    params.UOriginMetres = 0.0f;

    // Deliberately UNEVEN chunks: a per-chunk computation cannot hide in
    // symmetry.
    const std::span<const SplineStripStation> frontSpan(stations.data(), 4u);
    const std::span<const SplineStripStation> backSpan(stations.data() + 3u, 8u);
    const SplineStripMesh front = BuildSplineStrip(profile, frontSpan, params);
    const SplineStripMesh back = BuildSplineStrip(profile, backSpan, params);
    ASSERT_TRUE(front.IsValid());
    ASSERT_TRUE(back.IsValid());

    // No partial tile at the loop seam: the run ends on a whole tile count.
    const float32 finalU = back.Vertices[(back.RingCount - 1u) * back.RingVertexCount].UV.x;
    EXPECT_NEAR(finalU, 10.0f, 1e-4f);
    // The chunks agree at their shared ring.
    EXPECT_FLOAT_EQ(front.Vertices[(front.RingCount - 1u) * front.RingVertexCount].UV.x,
                    back.Vertices[0].UV.x);
    // And the density is uniform across both chunks: every 1.03 m station
    // interval spans exactly one tile of the snapped 10-over-10.3 density.
    const auto ringUStep = [](const SplineStripMesh& mesh, uint32 r)
    {
        return mesh.Vertices[r * mesh.RingVertexCount].UV.x -
               mesh.Vertices[(r - 1u) * mesh.RingVertexCount].UV.x;
    };
    for (uint32 r = 1u; r < front.RingCount; ++r)
        EXPECT_NEAR(ringUStep(front, r), 1.0f, 1e-3f) << "front chunk ring " << r;
    for (uint32 r = 1u; r < back.RingCount; ++r)
        EXPECT_NEAR(ringUStep(back, r), 1.0f, 1e-3f) << "back chunk ring " << r;
}

TEST(SplineStrip, StationsThatDoNotAdvanceAreDroppedRatherThanWelded)
{
    const SplineProfile profile = FlatRibbon(2.0f);
    auto stations = StraightStations(4u, 1.0f, 1.0f);
    stations[2].Distance = stations[1].Distance; // a doubled-back drape sample

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    EXPECT_EQ(mesh.DroppedStations, 1u);
    EXPECT_EQ(mesh.RingCount, 3u);
}

TEST(SplineStrip, TooFewUsableStationsProduceNoMeshRatherThanADegenerateOne)
{
    const SplineProfile profile = FlatRibbon(2.0f);
    auto stations = StraightStations(2u, 1.0f, 1.0f);
    stations[1].Position.y = std::nanf("");

    EXPECT_FALSE(BuildSplineStrip(profile, stations, {}).IsValid());
    EXPECT_FALSE(BuildSplineStrip(profile, {}, {}).IsValid());
    EXPECT_FALSE(BuildSplineStrip(SplineProfile{}, StraightStations(4u, 1.0f, 1.0f), {}).IsValid());
}

TEST(SplineStrip, BoundsCoverEveryVertexAndStayFinite)
{
    // The bounds are folded into the mesh content hash the GPU registry keys its
    // in-place re-upload on, so garbage bounds are both a wrong AABB and a
    // corrupted change detector.
    const SplineProfile profile = WallProfile(1.0f, 3.0f);
    const auto stations = DrapedStations([](float32, float32 z) { return 0.2f * z; }, 0.0f, 10.0f,
                                         11u, 0.5f);

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    for (const SplineVertex& v : mesh.Vertices)
    {
        EXPECT_GE(v.Position.x, mesh.MinBounds.x);
        EXPECT_GE(v.Position.y, mesh.MinBounds.y);
        EXPECT_GE(v.Position.z, mesh.MinBounds.z);
        EXPECT_LE(v.Position.x, mesh.MaxBounds.x);
        EXPECT_LE(v.Position.y, mesh.MaxBounds.y);
        EXPECT_LE(v.Position.z, mesh.MaxBounds.z);
    }
    EXPECT_TRUE(std::isfinite(mesh.MinBounds.y));
    EXPECT_TRUE(std::isfinite(mesh.MaxBounds.y));
}

TEST(SplineStrip, TheStripFollowsAGroundStepWithinTolerance)
{
    // A step is the shape a chord approximation cuts through: the strip must
    // track it at its sampled stations rather than bridging it.
    const auto step = [](float32, float32 z) { return z < 10.0f ? 0.0f : 2.0f; };
    const SplineProfile profile = FlatRibbon(2.0f);
    const auto stations = DrapedStations(step, 0.0f, 20.0f, 201u, 1.0f);

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    uint32 offSurface = 0;
    for (const SplineVertex& v : mesh.Vertices)
    {
        if (std::abs(v.Position.y - step(v.Position.x, v.Position.z)) > 1e-4f)
            ++offSurface;
    }
    // Only the single station interval that straddles the riser can disagree
    // with the surface, and it disagrees at its two rings, not in between.
    EXPECT_LE(offSurface, 2u * mesh.RingVertexCount);
}

// ---------------------------------------------------------------------------
// Acceptance contrasts: the three rigid-tile failure classes reported against
// the placed footpath. Each is a defect the strip cannot have, and each is
// asserted on the run that produces it -- a 45-degree descent, steeper than the
// 35-degree tilt ceiling that trades floating for burial.
// ---------------------------------------------------------------------------

TEST(SplineStrip, OnASteepDescentNoPartOfTheStripBuriesItselfInTheGround)
{
    // Reported: "the path ... half buried in the terrain" where the slope goes
    // down. A rigid tile pinned flatter than the ground drives its downhill end
    // under the surface; a strip has no tile ends to drive under anything.
    const auto slope = [](float32, float32 z) { return -z; }; // 45 degrees down
    const SplineProfile profile = FlatRibbon(2.5f);
    const auto stations = DrapedStations(slope, 0.0f, 30.0f, 61u, 1.25f);

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    for (const SplineVertex& v : mesh.Vertices)
    {
        const float32 ground = slope(v.Position.x, v.Position.z);
        EXPECT_GE(v.Position.y, ground - 1e-3f)
            << "a vertex sank " << (ground - v.Position.y) << " m into the ground";
        EXPECT_LE(v.Position.y - ground, 1e-3f) << "a vertex floats above the ground";
    }

    // The contrast, computed rather than asserted about placement: a 2.4 m tile
    // held to the 35-degree ceiling on 45-degree ground leaves its downhill end
    // this far from the surface it is supposed to lie on.
    const float32 tileHalfLength = 1.2f;
    const float32 ceilingError =
        tileHalfLength * (std::tan(45.0f * kPi / 180.0f) - std::tan(35.0f * kPi / 180.0f)) *
        std::cos(45.0f * kPi / 180.0f);
    EXPECT_GT(ceilingError, 0.1f) << "the contrast this test exists to draw has vanished";
}

TEST(SplineStrip, TheStripNeverDoublesBackOnItselfSoNoTwoPartsOverlap)
{
    // Reported: path pieces "overlap each other incorrectly" on the descent.
    // Rigid tiles overlap when their footprint exceeds the spacing the draped
    // walk gives them; consecutive rings cannot overlap because they share the
    // vertices between them.
    const auto slope = [](float32, float32 z) { return -z; };
    const SplineProfile profile = FlatRibbon(2.5f);
    const auto stations = DrapedStations(slope, 0.0f, 30.0f, 61u, 1.25f);

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    for (uint32 r = 1u; r < mesh.RingCount; ++r)
    {
        for (uint32 s = 0; s < mesh.RingVertexCount; ++s)
        {
            const V3& previous = mesh.Vertices[(r - 1u) * mesh.RingVertexCount + s].Position;
            const V3& current = mesh.Vertices[r * mesh.RingVertexCount + s].Position;
            // Travel is +Z here; every ring must advance along it.
            EXPECT_GT(current.z, previous.z) << "ring " << r << " doubled back";
        }
    }
}

TEST(SplineStrip, AdjacentSectionsDoNotLeanOppositeWaysOnBumpyGround)
{
    // Reported: "some footpaths tilted into the air on one side. I suspect some
    // footpaths behind it are tilted the opposite direction." That seesaw is
    // per-tile normal sampling: each tile independently reads the surface under
    // its own centre, so a ripple alternates their lean.
    //
    // What this asserts is the STATION PRODUCER plus the sweep, not the sweep
    // alone: frames derived from a shared polyline cannot alternate, because
    // consecutive stations are built from overlapping samples of one curve.
    const auto ripple = [](float32, float32 z) { return 0.15f * std::sin(z * 2.0f); };
    const SplineProfile profile = FlatRibbon(2.5f);
    const auto stations = DrapedStations(ripple, 0.0f, 30.0f, 121u, 1.25f);

    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, {});

    ASSERT_TRUE(mesh.IsValid());
    const auto countSignFlips = [](const std::vector<float32>& heights)
    {
        uint32 flips = 0;
        float32 previousPitch = 0.0f;
        for (size_t i = 1; i < heights.size(); ++i)
        {
            const float32 pitch = heights[i] - heights[i - 1];
            if (i > 1u && ((pitch > 0.0f) != (previousPitch > 0.0f)))
                ++flips;
            previousPitch = pitch;
        }
        return flips;
    };

    std::vector<float32> stripHeights;
    std::vector<float32> groundHeights;
    for (uint32 r = 0; r < mesh.RingCount; ++r)
    {
        const V3& p = mesh.Vertices[r * mesh.RingVertexCount].Position;
        stripHeights.push_back(p.y);
        groundHeights.push_back(ripple(p.x, p.z));
    }

    // The bound is the GROUND's own turning-point count, not a number picked by
    // hand: the strip may follow every rise and fall the terrain has and must
    // not invent one. An alternating-lean artefact flips at nearly every
    // station, so it would land near the ring count instead.
    const uint32 signFlips = countSignFlips(stripHeights);
    EXPECT_LE(signFlips, countSignFlips(groundHeights));
    EXPECT_LT(signFlips, mesh.RingCount / 4u);
}

// ---- Per-side half-widths -------------------------------------------------
//
// The builder scales each profile point by the half-width of the SIDE its
// lateral coordinate falls on. Equal half-widths must therefore reproduce the
// single-half-width sweep exactly, and unequal ones must move one edge without
// touching the other.

namespace
{

bool BitsEqual(float32 a, float32 b)
{
    return std::memcmp(&a, &b, sizeof(float32)) == 0;
}

} // namespace

// The Channel-mode invariant, stated as bits rather than as a tolerance.
//
// Driving both sides from one authored channel is what the width channel does,
// and that path predates per-side widths: any change to the arithmetic — a
// blend of the two sides, a different multiply order, a divide that used to be
// a multiply — moves the last mantissa bits of every vertex and re-textures
// existing scenes without anyone editing them. EXPECT_NEAR would not see it.
// The reference below is the single-scale formula written out by hand, so this
// goes red if the builder stops being exactly that for symmetric stations.
TEST(SplineStripBuilder, SymmetricHalfWidthsReproduceTheSingleScaleSweepBitForBit)
{
    // Deliberately NOT a power of two on either side of the ratio. A nominal
    // half-width of 2 makes scale = w/2 exactly equal to w * 0.5, so a builder
    // that swapped the divide for a reciprocal multiply would slip past a
    // bit-compare built on those constants. 4.5 gives a nominal of 2.25 and a
    // scale of 3/2.25, which is not exactly representable — the arithmetic has
    // to match, not merely agree.
    const SplineProfile profile = FlatRibbon(4.5f);
    ASSERT_TRUE(profile.IsValid());
    ASSERT_FLOAT_EQ(profile.NominalHalfWidth, 2.25f);

    constexpr float32 kHalfWidth = 3.0f;
    const std::vector<SplineStripStation> stations =
        StraightStations(/*count=*/8u, /*step=*/2.0f, kHalfWidth);

    SplineStripParams params;
    params.WidthScale = SplineProfileScale::LateralOnly;
    params.UOriginMetres = 0.0f;
    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, params);
    ASSERT_TRUE(mesh.IsValid());
    ASSERT_EQ(mesh.RingCount, stations.size());

    const float32 scale = kHalfWidth / profile.NominalHalfWidth;
    const uint32 pointCount = static_cast<uint32>(profile.Points.size());

    // An open Bevel splits nothing, so slot order is point order and the ring
    // maps one-to-one onto the profile.
    ASSERT_EQ(mesh.RingVertexCount, pointCount);

    for (uint32 r = 0; r < mesh.RingCount; ++r)
    {
        const SplineStripStation& s = stations[r];
        for (uint32 j = 0; j < pointCount; ++j)
        {
            // The pre-per-side expression, verbatim: one lateral scale for the
            // whole cross-section and a vertical scale of exactly 1.
            const V3 expected = s.Position + s.Right * (profile.Points[j].Position.x * scale) +
                                s.Up * (profile.Points[j].Position.y * 1.0f);
            const V3& actual = mesh.Vertices[r * mesh.RingVertexCount + j].Position;

            EXPECT_TRUE(BitsEqual(expected.x, actual.x))
                << "ring " << r << " point " << j << " x drifted from the single-scale sweep";
            EXPECT_TRUE(BitsEqual(expected.y, actual.y))
                << "ring " << r << " point " << j << " y drifted from the single-scale sweep";
            EXPECT_TRUE(BitsEqual(expected.z, actual.z))
                << "ring " << r << " point " << j << " z drifted from the single-scale sweep";
        }
    }
}

// An asymmetric channel is the case a single half-width cannot express: the
// left edge and the right edge answer to different measurements, and neither
// may drag the other.
TEST(SplineStripBuilder, AsymmetricHalfWidthsPlaceEachEdgeIndependently)
{
    const SplineProfile profile = FlatRibbon(4.0f);
    ASSERT_TRUE(profile.IsValid());

    constexpr float32 kLeft = 2.0f;
    constexpr float32 kRight = 6.0f;
    std::vector<SplineStripStation> stations = StraightStations(4u, 2.0f, 1.0f);
    for (SplineStripStation& s : stations)
    {
        s.HalfWidthLeft = kLeft;
        s.HalfWidthRight = kRight;
    }

    SplineStripParams params;
    params.WidthScale = SplineProfileScale::LateralOnly;
    params.UOriginMetres = 0.0f;
    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, params);
    ASSERT_TRUE(mesh.IsValid());

    // The run travels +Z, so Right is +X and the two half-widths land straight
    // on the bounds. Left is the NEGATIVE lateral direction.
    EXPECT_NEAR(mesh.MinBounds.x, -kLeft, 1.0e-5f) << "the left edge must sit at its own width";
    EXPECT_NEAR(mesh.MaxBounds.x, kRight, 1.0e-5f) << "the right edge must sit at its own width";

    // Widening one side must not shift the centreline: the ribbon is not
    // re-centred on its own extent.
    for (uint32 r = 0; r < mesh.RingCount; ++r)
    {
        const V3& p = mesh.Vertices[r * mesh.RingVertexCount].Position;
        EXPECT_NEAR(p.z, stations[r].Position.z, 1.0e-5f);
    }
}

// Zero on one side is a legal measurement — a waterline that meets the bank at
// the centreline has no width there — and must collapse that side only.
TEST(SplineStripBuilder, AZeroHalfWidthCollapsesOnlyItsOwnSide)
{
    const SplineProfile profile = FlatRibbon(4.0f);
    std::vector<SplineStripStation> stations = StraightStations(4u, 2.0f, 1.0f);
    for (SplineStripStation& s : stations)
    {
        s.HalfWidthLeft = 0.0f;
        s.HalfWidthRight = 5.0f;
    }

    SplineStripParams params;
    params.WidthScale = SplineProfileScale::LateralOnly;
    params.UOriginMetres = 0.0f;
    const SplineStripMesh mesh = BuildSplineStrip(profile, stations, params);
    ASSERT_TRUE(mesh.IsValid());

    EXPECT_NEAR(mesh.MinBounds.x, 0.0f, 1.0e-5f);
    EXPECT_NEAR(mesh.MaxBounds.x, 5.0f, 1.0e-5f);
}

// ---- Frozen output ----------------------------------------------------------
//
// The builder's stations may carry corners, creases and ground offsets, but a
// station that asks for none of them must sweep exactly as it always has: the
// generic sweep's paths, roads and walls are built from such stations, and a
// last-bit move in any of them re-textures and re-shades saved scenes nobody
// edited.

namespace
{

// FNV-1a over the mesh's raw bytes: one ULP anywhere changes it.
uint64 MeshDigest(const SplineStripMesh& mesh)
{
    uint64 value = 1469598103934665603ull;
    const auto fold = [&value](const void* data, size_t size)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            value ^= static_cast<uint64>(p[i]);
            value *= 1099511628211ull;
        }
    };
    fold(mesh.Vertices.data(), mesh.Vertices.size() * sizeof(SplineVertex));
    fold(mesh.Indices.data(), mesh.Indices.size() * sizeof(uint32));
    fold(&mesh.MinBounds, sizeof(mesh.MinBounds));
    fold(&mesh.MaxBounds, sizeof(mesh.MaxBounds));
    return value;
}

// A capped wall and a crowned road over rolling, curving ground, with roll and a
// width that changes: every term of the per-vertex arithmetic is exercised.
uint64 FrozenRunDigest(SplineProfileShape shape)
{
    SplineProfileParams profileParams;
    profileParams.Shape = shape;
    profileParams.Width = 1.3f;
    profileParams.Height = 2.7f;
    const SplineProfile profile = BuildProfile(profileParams);

    std::vector<SplineStripStation> stations = DrapedStations(
        [](float32 x, float32 z) { return 0.4f * std::sin(0.3f * z) + 0.05f * x; }, 0.0f, 23.0f,
        47u, 0.65f);
    for (size_t i = 0; i < stations.size(); ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(stations.size() - 1u);
        stations[i].HalfWidthLeft = 0.65f + 0.2f * t;
        stations[i].HalfWidthRight = 0.65f + 0.2f * t;
        stations[i].RollRadians = 0.1f * std::sin(3.0f * t);
    }

    SplineStripParams params;
    params.TilesPerMetreU = 0.7f;
    params.TilesPerMetreV = 1.3f;
    return MeshDigest(BuildSplineStrip(profile, stations, params));
}

} // namespace

// x86-64 digests captured from the builder at 699615abcd, before it learned
// corners. Per architecture for the reason SplineChannelParity states: libm's
// sin/cos round differently on arm64, where no baseline has been captured.
TEST(SplineStripBuilder, AStationWithNoCornerOrGroundOffsetSweepsByteForByteAsBefore)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    GTEST_SKIP() << "x86-64 baseline only";
#endif
    EXPECT_EQ(FrozenRunDigest(SplineProfileShape::Rectangle), 14795171210429567218ull);
    EXPECT_EQ(FrozenRunDigest(SplineProfileShape::Crown), 16412760035002659206ull);
}

// A station's GroundOffset moves the profile's grounded points along Up and
// leaves every other point where it was: a wall's base sinks into a cross slope
// while its top stays at its height above the station.
TEST(SplineStripBuilder, OnlyGroundedPointsTakeTheGroundOffset)
{
    const SplineProfile wall = WallProfile(1.0f, 3.0f);
    ASSERT_TRUE(wall.Points[0].Grounded != 0u && wall.Points[3].Grounded != 0u);
    ASSERT_TRUE(wall.Points[1].Grounded == 0u && wall.Points[2].Grounded == 0u);

    const std::vector<SplineStripStation> level = StraightStations(4u, 1.0f, 0.5f);
    std::vector<SplineStripStation> sunk = level;
    for (SplineStripStation& station : sunk)
        station.GroundOffset = -0.25f;

    const SplineStripMesh before = BuildSplineStrip(wall, level, {});
    const SplineStripMesh after = BuildSplineStrip(wall, sunk, {});
    ASSERT_EQ(before.Vertices.size(), after.Vertices.size());
    uint32 moved = 0;
    for (size_t v = 0; v < before.Vertices.size(); ++v)
    {
        const V3& a = before.Vertices[v].Position;
        const V3& b = after.Vertices[v].Position;
        EXPECT_FLOAT_EQ(a.x, b.x);
        EXPECT_FLOAT_EQ(a.z, b.z);
        if (a.y < 1.5f)
        {
            EXPECT_NEAR(b.y, a.y - 0.25f, 1.0e-6f) << "base vertex " << v;
            ++moved;
        }
        else
        {
            EXPECT_FLOAT_EQ(b.y, a.y) << "top vertex " << v;
        }
    }
    EXPECT_GT(moved, 0u);
}

// ---- V on the side faces --------------------------------------------------

// Without a datum a wall's V runs around its cross-section: 0 at the left base,
// the height at the left top, across the top, and down the right face to the
// perimeter. This is the generic sweep's V, and the datum below must not move
// it for a caller that does not ask.
TEST(SplineStripBuilder, WithoutADatumVRunsAroundThePerimeter)
{
    const SplineProfile wall = WallProfile(1.0f, 3.0f);
    const SplineStripMesh mesh = BuildSplineStrip(wall, StraightStations(3u, 1.0f, 0.5f), {});
    ASSERT_TRUE(mesh.IsValid());
    for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
    {
        const SplineVertex& vertex = mesh.Vertices[k];
        const float32 x = vertex.Position.x;
        const float32 y = vertex.Position.y;
        const bool leftFace = std::abs(vertex.Normal.x + 1.0f) < 1.0e-5f;
        const bool top = std::abs(vertex.Normal.y - 1.0f) < 1.0e-5f;
        const bool rightFace = std::abs(vertex.Normal.x - 1.0f) < 1.0e-5f;
        if (leftFace)
            EXPECT_NEAR(vertex.UV.y, y, 1.0e-5f);
        else if (top)
            EXPECT_NEAR(vertex.UV.y, 3.0f + (x + 0.5f), 1.0e-5f);
        else if (rightFace)
            EXPECT_NEAR(vertex.UV.y, 4.0f + (3.0f - y), 1.0e-5f);
        EXPECT_EQ(vertex.Tangent.w, 1.0f);
    }
}

// With a datum, the side faces and the caps take V as height above it, so on a
// slope the courses stay level and meet across rings; the top keeps the
// perimeter V, and the right face's tangent handedness is -1 because its V
// climbs against the edge the winding runs down.
TEST(SplineStripBuilder, SideFacesTakeTheirHeightAboveTheDatumAsV)
{
    const SplineProfile wall = WallProfile(1.0f, 3.0f);
    const std::vector<SplineStripStation> stations =
        DrapedStations([](float32, float32 z) { return 0.25f * z; }, 0.0f, 8.0f, 9u, 0.5f);

    SplineStripParams perimeter;
    perimeter.TilesPerMetreV = 0.5f;
    SplineStripParams height = perimeter;
    height.HeightVDatumMetres = -2.0f;
    const SplineStripMesh around = BuildSplineStrip(wall, stations, perimeter);
    const SplineStripMesh level = BuildSplineStrip(wall, stations, height);
    ASSERT_EQ(around.Vertices.size(), level.Vertices.size());

    uint32 sides = 0;
    uint32 caps = 0;
    const uint32 ringVertices = level.RingCount * level.RingVertexCount;
    for (uint32 v = 0; v < level.Vertices.size(); ++v)
    {
        const SplineVertex& vertex = level.Vertices[v];
        const float32 heightV = (vertex.Position.y + 2.0f) * 0.5f;
        if (v >= ringVertices)
        {
            EXPECT_NEAR(vertex.UV.y, heightV, 1.0e-5f) << "cap vertex " << v;
            ++caps;
            continue;
        }
        const bool side = std::abs(vertex.Normal.x) > std::abs(vertex.Normal.y);
        if (side)
        {
            EXPECT_NEAR(vertex.UV.y, heightV, 1.0e-5f) << "side vertex " << v;
            EXPECT_EQ(vertex.Tangent.w, vertex.Normal.x > 0.0f ? -1.0f : 1.0f) << "side vertex " << v;
            ++sides;
        }
        else
        {
            EXPECT_EQ(vertex.UV.y, around.Vertices[v].UV.y) << "top or bottom vertex " << v;
            EXPECT_EQ(vertex.Tangent.w, 1.0f);
        }
        EXPECT_EQ(vertex.UV.x, around.Vertices[v].UV.x);
    }
    EXPECT_GT(sides, 0u);
    EXPECT_GT(caps, 0u);
}
