// The swept wall, through its controller's own pipeline (Placement/SplineWallBuild.h):
// the defects the swept-wall design names, each held at the number the design
// gives, and the corners, loops, base, steps and texture coordinates it builds.
//
// The conform ray is replaced by an analytic ground (what ConformRayDown returns
// on a plane): its height and its normal. The placer is the identity unless a
// test scales it, so local and world coincide and every measurement is in world
// metres.

#include "Mathematics/Matrix4x4.h"
#include "Placement/CenterlineSampling.h"
#include "Placement/PieceEntity.h"
#include "Placement/RecipeValidation.h"
#include "Placement/SplineChunkSlots.h"
#include "Placement/SplineSweepStations.h"
#include "Placement/SplineWallBuild.h"
#include "Placement/TileLayout.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineGeometry/SplineProfile.h"
#include "SplineGeometry/SplineStripBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace GameEngine;
using V3 = Mathematics::Vector3;
namespace SG = GameEngine::SplineGeometry;

namespace
{

// SplineWallController.cpp pins these.
constexpr float32 kChunkLengthMetres = 50.0f;
constexpr uint32 kMaxChunks = 200;

constexpr float32 kWallHeight = 3.0f;

using Ground = std::function<float32(float32 x, float32 z)>;

// How the wall is placed beyond its spline, recipe and ground: the placer's
// world matrix.
struct WallOptions
{
    Mathematics::Matrix4x4 Placer = Mathematics::Matrix4x4::Identity();
    // Report every ground normal facing down, as the conform ray does when it
    // strikes the underside of a double-sided face.
    bool DownwardNormals = false;
};

struct WallRun
{
    std::vector<SG::SplineStripStation> Stations;
    std::vector<SG::SplineStripMesh> Chunks;
    std::vector<SG::SplineCornerIssue> Issues;
};

// A spline of straight segments whose points keep the default 5 m radius: the
// wall reads no width channel, so its thickness is the recipe's alone.
Spline::SplineData LinearSpline(const std::vector<V3>& points, bool closed)
{
    Spline::SplineData data;
    data.Type = Spline::SplineType::Linear;
    data.Closed = closed;
    for (const V3& p : points)
        data.AddPoint(p);
    Spline::RebuildSplineCache(data);
    return data;
}

Components::SplineWall WallOf(float32 thickness,
                              Components::SplineWallCorner corner = Components::SplineWallCorner::Mitre,
                              Components::SplineWallGrade grade = Components::SplineWallGrade::Racked)
{
    Components::SplineWall wall;
    wall.Thickness = thickness;
    wall.Height = kWallHeight;
    wall.Corner = corner;
    wall.Grade = grade;
    return wall;
}

// The wall controller's rebuild for `recipe` on `ground`. The run's meshes are in
// the placer's local space; with the default identity placer that is world space.
WallRun BuildWall(const Spline::SplineData& data, const Ground& ground,
                  const Components::SplineWall& recipe = WallOf(0.6f), const WallOptions& options = {})
{
    const SG::SplineProfile profile = Editor::BuildWallProfile(recipe);
    const Mathematics::Matrix4x4& placer = options.Placer;
    const Mathematics::Matrix4x4 invPlacer = Editor::InvertPlacerWorld(placer.Data());
    const Editor::SweepShape shape = Editor::SweepShapeForWall(recipe);
    const Editor::SweepSamples samples = Editor::SampleSweepCenterline(
        data, Editor::CenterlineSampleCount(data.TotalArcLength), placer, shape);

    // What the conform ray reports on this ground: its height, and the normal of
    // its gradient.
    constexpr float32 kProbe = 1.0e-2f;
    std::vector<Editor::CenterSample> center(samples.Frames.size());
    for (size_t i = 0; i < center.size(); ++i)
    {
        center[i].Pos = placer.TransformPoint(samples.Frames[i].Position);
        center[i].Normal = V3(0.0f, 1.0f, 0.0f);
        if (!samples.Probed[i])
            continue;
        const float32 x = center[i].Pos.x;
        const float32 z = center[i].Pos.z;
        center[i].Pos.y = ground(x, z);
        const float32 riseX = (ground(x + kProbe, z) - ground(x - kProbe, z)) / (2.0f * kProbe);
        const float32 riseZ = (ground(x, z + kProbe) - ground(x, z - kProbe)) / (2.0f * kProbe);
        center[i].Normal = Editor::NormalizedOrFallback(V3(-riseX, 1.0f, -riseZ),
                                                        V3(0.0f, 1.0f, 0.0f)) *
                           (options.DownwardNormals ? -1.0f : 1.0f);
    }
    Editor::DrapeInsertedSamples(samples, placer, center);
    Editor::HoldSurfaceAcrossGaps(center);

    Editor::SweepStationStream stream =
        Editor::BuildSweepStations(data, samples, center, invPlacer, profile.NominalHalfWidth,
                                   SG::SplineProfileScale::None, false, shape);
    const bool closedLoop = data.IsEffectivelyClosed();
    Editor::StepWallTop(stream, recipe, invPlacer, closedLoop);
    const SG::SplineStripParams stripParams =
        Editor::WallStripParams(stream, closedLoop, Editor::LargestAxisScale(placer));

    WallRun run;
    for (const auto& [first, last] :
         Editor::CarveChunkRanges(stream.WorldDistance, kChunkLengthMetres, kMaxChunks))
    {
        run.Chunks.push_back(SG::BuildSplineStrip(
            profile,
            std::span<const SG::SplineStripStation>(stream.Local.data() + first, last - first + 1u),
            stripParams));
    }
    run.Stations = std::move(stream.Local);
    run.Issues = std::move(stream.CornerIssues);
    return run;
}

float32 Level(float32, float32)
{
    return 0.0f;
}

// Side-face triangles whose winding disagrees with the normals the builder gave
// them: a face folded back through itself.
uint32 InvertedSideTriangles(const SG::SplineStripMesh& mesh)
{
    uint32 inverted = 0;
    const uint32 ringVertices = mesh.RingCount * mesh.RingVertexCount;
    for (size_t t = 0; t + 2u < mesh.Indices.size(); t += 3u)
    {
        if (mesh.Indices[t] >= ringVertices)
            continue;
        const SG::SplineVertex& a = mesh.Vertices[mesh.Indices[t]];
        const SG::SplineVertex& b = mesh.Vertices[mesh.Indices[t + 1u]];
        const SG::SplineVertex& c = mesh.Vertices[mesh.Indices[t + 2u]];
        if (std::abs(a.Normal.y) > 0.5f)
            continue;
        const V3 geometric = V3::Cross(b.Position - a.Position, c.Position - a.Position);
        if (V3::Dot(geometric, geometric) < 1.0e-16f)
            continue;
        if (V3::Dot(geometric, a.Normal + b.Normal + c.Normal) < 0.0f)
            ++inverted;
    }
    return inverted;
}

// Bands of the run (ring intervals, across every chunk) that reference a vertex
// at each ring position, keyed by the position's bits: a welded surface reaches
// every ring position from both sides. Crease pairs and a welded loop's seam put
// two rings on one set of positions, which this counts as one.
std::map<std::tuple<uint32, uint32, uint32>, uint32> BandsPerRingPosition(const WallRun& run)
{
    const auto key = [](const V3& p)
    {
        uint32 bits[3];
        std::memcpy(bits, &p, sizeof(bits));
        return std::tuple<uint32, uint32, uint32>{bits[0], bits[1], bits[2]};
    };
    std::map<std::tuple<uint32, uint32, uint32>, uint32> bands;
    for (const SG::SplineStripMesh& mesh : run.Chunks)
    {
        const uint32 ringVertices = mesh.RingCount * mesh.RingVertexCount;
        for (uint32 v = 0; v < ringVertices; ++v)
            bands.emplace(key(mesh.Vertices[v].Position), 0u);
        // Each band is two triangles per profile edge; count a band once per
        // ring position it touches.
        std::set<std::pair<uint32, std::tuple<uint32, uint32, uint32>>> seen;
        for (size_t t = 0; t + 2u < mesh.Indices.size(); t += 3u)
        {
            for (size_t i = t; i < t + 3u; ++i)
            {
                const uint32 v = mesh.Indices[i];
                if (v >= ringVertices)
                    continue;
                const uint32 band = std::min({mesh.Indices[t], mesh.Indices[t + 1u],
                                              mesh.Indices[t + 2u]}) /
                                    mesh.RingVertexCount;
                const auto position = key(mesh.Vertices[v].Position);
                if (seen.emplace(band, position).second)
                    ++bands[position];
            }
        }
    }
    return bands;
}

uint32 CapTriangles(const WallRun& run)
{
    uint32 caps = 0;
    for (const SG::SplineStripMesh& mesh : run.Chunks)
    {
        const uint32 ringVertices = mesh.RingCount * mesh.RingVertexCount;
        for (size_t t = 0; t + 2u < mesh.Indices.size(); t += 3u)
            caps += mesh.Indices[t] >= ringVertices ? 1u : 0u;
    }
    return caps;
}

} // namespace

// A Linear wall's authored points are stations with mitred rings, whether the
// point falls on a drape sample or between two: every ring of the run, measured
// across the wall perpendicular to the leg it stands on, is the full thickness,
// and no side face folds. A sweep that cuts across the corner on the
// central-difference bisector leaves 0.707 of the thickness there, 0.85 between
// samples, with the inside face folded on 1 m and 2 m walls.
TEST(SplineSweepWall, ALinearNinetyDegreeCornerKeepsTheWallsFullThickness)
{
    constexpr float32 kLeg = 10.0f;
    for (const float32 thickness : {0.6f, 1.0f, 2.0f})
    {
        for (const float32 secondLeg : {10.0f, 7.3f})
        {
            const Spline::SplineData data = LinearSpline(
                {V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, kLeg), V3(secondLeg, 0.0f, kLeg)}, false);
            const WallRun run = BuildWall(data, Level, WallOf(thickness));
            ASSERT_EQ(run.Chunks.size(), 1u);
            const SG::SplineStripMesh& mesh = run.Chunks.front();
            ASSERT_TRUE(mesh.IsValid());

            float32 thinnest = 1.0e9f;
            for (uint32 r = 0; r < mesh.RingCount; ++r)
            {
                const SG::SplineStripStation& station = run.Stations[r];
                float32 least = 1.0e9f;
                float32 most = -1.0e9f;
                V3 left{};
                V3 right{};
                for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
                {
                    const V3& p = mesh.Vertices[r * mesh.RingVertexCount + k].Position;
                    if (p.y > 0.5f * kWallHeight)
                        continue;
                    const float32 lateral = V3::Dot(p - station.Position, station.Right);
                    if (lateral < least)
                    {
                        least = lateral;
                        left = p;
                    }
                    if (lateral > most)
                    {
                        most = lateral;
                        right = p;
                    }
                }
                // Across leg 1 (travel +Z) is X; across leg 2 (travel +X) is Z.
                // A ring on the corner stands on both legs.
                const bool onFirstLeg = station.Position.z < kLeg - 1.0e-4f ||
                                        std::abs(station.Forward.z) > std::abs(station.Forward.x);
                const float32 across = onFirstLeg ? std::abs(right.x - left.x)
                                                  : std::abs(right.z - left.z);
                thinnest = std::min(thinnest, across);
            }
            EXPECT_NEAR(thinnest, thickness, 1.0e-3f)
                << "thickness " << thickness << ", second leg " << secondLeg;
            EXPECT_EQ(InvertedSideTriangles(mesh), 0u)
                << "thickness " << thickness << ", second leg " << secondLeg;
            EXPECT_TRUE(run.Issues.empty());
        }
    }
}

// Between corners a smooth wall follows its curve: no chord of the run strays
// more than the 2 cm sagitta from the spline it was sampled on.
TEST(SplineSweepWall, ASmoothWallFollowsItsCurveWithinTheSagitta)
{
    Spline::SplineData data;
    data.Type = Spline::SplineType::CatmullRom;
    // A tight S, whose bends the 0.5 m drape step alone would facet.
    data.AddPoint(V3(0.0f, 0.0f, 0.0f), 0.3f);
    data.AddPoint(V3(1.5f, 0.0f, 1.5f), 0.3f);
    data.AddPoint(V3(0.0f, 0.0f, 3.0f), 0.3f);
    data.AddPoint(V3(1.5f, 0.0f, 4.5f), 0.3f);
    Spline::RebuildSplineCache(data);

    const WallRun run = BuildWall(data, Level);
    float32 worst = 0.0f;
    for (size_t i = 0; i + 1u < run.Stations.size(); ++i)
    {
        const V3 a = run.Stations[i].Position;
        const V3 b = run.Stations[i + 1u].Position;
        // The curve's midpoint between the two stations, against the chord's.
        const Spline::ClosestPointResult closest =
            Spline::FindClosestPoint(data, (a + b) * 0.5f);
        worst = std::max(worst, closest.Distance);
    }
    EXPECT_LE(worst, Editor::kSweepSagittaMetres);
    // The curve is tight enough that the drape's own samples did not suffice.
    EXPECT_GT(run.Stations.size(), Editor::CenterlineSampleCount(data.TotalArcLength));
}

// A closed wall is welded on itself: its first and last rings are the same
// positions, no caps are emitted, and every ring position of the run is reached
// by a band from each side. Unwelded, a closed 10 m Linear square leaves its
// seam rings 0.71 m apart at the corner they share, with two caps facing each
// other inside the wall.
TEST(SplineSweepWall, AClosedSquareStitchesItsSeamWithNoCaps)
{
    for (const float32 side : {10.0f, 23.7f})
    {
        const Spline::SplineData data = LinearSpline(
            {V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, side), V3(side, 0.0f, side), V3(side, 0.0f, 0.0f)},
            true);
        const WallRun run = BuildWall(data, Level, WallOf(1.0f));
        ASSERT_FALSE(run.Chunks.empty());
        const SG::SplineStripMesh& first = run.Chunks.front();
        const SG::SplineStripMesh& last = run.Chunks.back();
        ASSERT_TRUE(first.IsValid() && last.IsValid());

        EXPECT_EQ(CapTriangles(run), 0u) << "side " << side;
        const uint32 lastRing = (last.RingCount - 1u) * last.RingVertexCount;
        for (uint32 k = 0; k < first.RingVertexCount; ++k)
        {
            EXPECT_EQ(std::memcmp(&first.Vertices[k].Position,
                                  &last.Vertices[lastRing + k].Position, sizeof(V3)),
                      0)
                << "side " << side << ", slot " << k;
        }
        for (const auto& [position, bands] : BandsPerRingPosition(run))
            EXPECT_EQ(bands, 2u) << "side " << side;
    }
}

// A smooth closed wall welds the same way, and its seam ring looks along the
// loop rather than one-sidedly ahead.
TEST(SplineSweepWall, AClosedSmoothWallWeldsItsSeam)
{
    Spline::SplineData data;
    data.Type = Spline::SplineType::CatmullRom;
    data.Closed = true;
    for (const V3& p : {V3(0.0f, 0.0f, -12.0f), V3(12.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 12.0f),
                        V3(-12.0f, 0.0f, 0.0f)})
        data.AddPoint(p, 0.3f);
    Spline::RebuildSplineCache(data);

    const WallRun run = BuildWall(data, Level);
    EXPECT_EQ(CapTriangles(run), 0u);
    for (const auto& [position, bands] : BandsPerRingPosition(run))
        EXPECT_EQ(bands, 2u);
    const SG::SplineStripStation& seam = run.Stations.front();
    const SG::SplineStripStation& before = run.Stations[run.Stations.size() - 2u];
    const SG::SplineStripStation& after = run.Stations[1];
    const V3 across = after.Position - before.Position;
    EXPECT_NEAR(V3::Dot(seam.Forward, across * (1.0f / std::sqrt(V3::Dot(across, across)))), 1.0f,
                1.0e-5f);
}

// A wall's grounded base points sink by the fall of the ground across the wall,
// read from the conform hit's normal, so no base vertex stands above the ground
// under it. Hung from the centreline's drape alone, the downhill edge of a 1 m
// wall on a 20-degree cross slope stands 0.18 m (0.5 · tan 20°) clear.
TEST(SplineSweepWall, AWallAcrossATwentyDegreeSlopeKeepsItsBaseInTheGround)
{
    const float32 fall = std::tan(20.0f * 3.14159265f / 180.0f);
    // Falls toward +X, across a run along +Z; then the same slope under a run
    // that turns a 90-degree corner and climbs it.
    const Ground slope = [fall](float32 x, float32) { return -x * fall; };
    const std::vector<std::vector<V3>> runs = {
        {V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 20.0f)},
        {V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 10.0f), V3(-8.0f, 0.0f, 10.0f)},
    };
    for (size_t run = 0; run < runs.size(); ++run)
    {
        const WallRun wall = BuildWall(LinearSpline(runs[run], false), slope, WallOf(1.0f));
        float32 highest = -1.0e9f;
        float32 lowest = 1.0e9f;
        for (const SG::SplineStripMesh& mesh : wall.Chunks)
        {
            for (uint32 v = 0; v < mesh.RingCount * mesh.RingVertexCount; ++v)
            {
                const V3& p = mesh.Vertices[v].Position;
                const float32 aboveGround = p.y - slope(p.x, p.z);
                if (aboveGround > 0.5f * kWallHeight)
                    continue; // the top
                highest = std::max(highest, aboveGround);
                lowest = std::min(lowest, aboveGround);
            }
        }
        EXPECT_LE(highest, 1.0e-4f) << "run " << run;
        // Sunk by the fall and no more: the uphill edge is buried by at most the
        // fall across the whole width, 1 m · tan 20°, or across the mitre's
        // diagonal, √2 times that, at the 90-degree corner.
        const float32 widest = run == 0u ? 1.0f : std::sqrt(2.0f);
        EXPECT_GE(lowest, -widest * fall - 1.0e-4f) << "run " << run;
    }
}

// A corner past the mitre limit is validation, reported once: rebuilding the
// same wall says nothing more, nor does dragging the point to another turn that
// is still past the limit; fixing the corner clears the report, and breaking it
// again reports it again, naming the point.
TEST(SplineSweepWall, ACornerPastTheMitreLimitIsReportedOncePerChange)
{
    const auto wallTurning = [](float32 degrees)
    {
        const float32 turn = degrees * 3.14159265f / 180.0f;
        return BuildWall(LinearSpline({V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 10.0f),
                                       V3(8.0f * std::sin(turn), 0.0f, 10.0f + 8.0f * std::cos(turn))},
                                      false),
                         Level, WallOf(0.6f));
    };
    const WallRun hairpinRun = wallTurning(150.0f);
    ASSERT_EQ(hairpinRun.Issues.size(), 1u);
    EXPECT_EQ(hairpinRun.Issues[0].Kind, SG::SplineCornerIssueKind::PastMitreLimit);
    EXPECT_EQ(hairpinRun.Issues[0].PointIndex, 1u);
    EXPECT_NEAR(hairpinRun.Issues[0].TurnDegrees, 150.0f, 0.05f);

    std::vector<std::string> logged;
    const std::vector<std::string> hairpin = Editor::CornerValidation("SplineWall", hairpinRun.Issues);
    ASSERT_EQ(hairpin.size(), 1u);
    EXPECT_NE(hairpin[0].find("point 1 turns past the 120-degree mitre limit"), std::string::npos)
        << hairpin[0];
    EXPECT_EQ(hairpin[0].rfind("SplineWall: ", 0), 0u) << "the line opens with the recipe's name";

    EXPECT_TRUE(Editor::ReportRecipeValidation(hairpin, "Keep Wall", 7u, logged));
    EXPECT_FALSE(Editor::ReportRecipeValidation(Editor::CornerValidation("SplineWall", wallTurning(150.0f).Issues),
                                                "Keep Wall", 7u, logged));
    EXPECT_FALSE(Editor::ReportRecipeValidation(Editor::CornerValidation("SplineWall", wallTurning(140.0f).Issues),
                                                "Keep Wall", 7u, logged));

    const std::vector<std::string> fixed = Editor::CornerValidation("SplineWall", wallTurning(90.0f).Issues);
    EXPECT_TRUE(fixed.empty());
    EXPECT_FALSE(Editor::ReportRecipeValidation(fixed, "Keep Wall", 7u, logged));
    EXPECT_TRUE(logged.empty());

    EXPECT_TRUE(Editor::ReportRecipeValidation(hairpin, "Keep Wall", 7u, logged));
}

// The wall reads no width channel: a 2 m wall on a spline whose points carry a
// 0.3 m radius is 2 m thick, and its corners are measured on that thickness, so
// its inside face stays unfolded at a 90-degree corner between samples. Read
// from the channel instead, a fresh spline's 5 m radius made a 10 m wall.
TEST(SplineSweepWall, TheSplinesWidthNeitherSizesTheWallNorItsCorners)
{
    Spline::SplineData data;
    data.Type = Spline::SplineType::Linear;
    for (const V3& p : {V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 10.0f), V3(7.3f, 0.0f, 10.0f)})
        data.AddPoint(p, 0.3f);
    Spline::RebuildSplineCache(data);
    const WallRun run = BuildWall(data, Level, WallOf(2.0f));
    uint32 inverted = 0;
    for (const SG::SplineStripMesh& mesh : run.Chunks)
        inverted += InvertedSideTriangles(mesh);
    EXPECT_EQ(inverted, 0u);
    for (const SG::SplineStripStation& station : run.Stations)
    {
        EXPECT_FLOAT_EQ(station.HalfWidthLeft, 1.0f);
        EXPECT_FLOAT_EQ(station.HalfWidthRight, 1.0f);
    }
}

// The grounded base sinks by the wall's own thickness: a 2 m wall on a spline
// whose points carry a 0.3 m radius has no base vertex above a 20-degree cross
// slope.
TEST(SplineSweepWall, TheBaseSinksByTheWallsOwnThickness)
{
    const float32 fall = std::tan(20.0f * 3.14159265f / 180.0f);
    const Ground slope = [fall](float32 x, float32) { return -x * fall; };
    Spline::SplineData data;
    data.Type = Spline::SplineType::Linear;
    for (const V3& p : {V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 20.0f)})
        data.AddPoint(p, 0.3f);
    Spline::RebuildSplineCache(data);
    const WallRun wall = BuildWall(data, slope, WallOf(2.0f));
    float32 highest = -1.0e9f;
    for (const SG::SplineStripMesh& mesh : wall.Chunks)
    {
        for (uint32 v = 0; v < mesh.RingCount * mesh.RingVertexCount; ++v)
        {
            const V3& p = mesh.Vertices[v].Position;
            const float32 aboveGround = p.y - slope(p.x, p.z);
            if (aboveGround < 0.5f * kWallHeight * 0.3f)
                highest = std::max(highest, aboveGround);
        }
    }
    EXPECT_LE(highest, 1.0e-4f);
}

// A leg too short for its corners is reported through the same validation as the
// mitre limit, naming the points and the fix.
TEST(SplineSweepWall, LegsTooShortForTheirCornersAreReportedWithTheFix)
{
    const WallRun shortEnd = BuildWall(
        LinearSpline({V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 10.0f), V3(0.3f, 0.0f, 10.0f)}, false),
        Level, WallOf(1.0f));
    const std::vector<std::string> endLines = Editor::CornerValidation("SplineWall", shortEnd.Issues);
    ASSERT_EQ(endLines.size(), 1u);
    EXPECT_NE(endLines[0].find("point 1 "), std::string::npos) << endLines[0];
    EXPECT_NE(endLines[0].find("Lengthen the end leg or thin the wall"), std::string::npos)
        << endLines[0];

    const WallRun tightU = BuildWall(
        LinearSpline({V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 10.0f), V3(0.6f, 0.0f, 10.0f),
                      V3(0.6f, 0.0f, 0.0f)},
                     false),
        Level, WallOf(1.0f));
    const std::vector<std::string> uLines = Editor::CornerValidation("SplineWall", tightU.Issues);
    ASSERT_EQ(uLines.size(), 1u);
    EXPECT_NE(uLines[0].find("points 1 and 2"), std::string::npos) << uLines[0];
    EXPECT_NE(uLines[0].find("Lengthen the leg between them or thin the wall"), std::string::npos)
        << uLines[0];
}

// The ground's slope is read in the placer's local space through the normal's
// own transform, so a non-uniformly scaled placer sinks the base by the fall
// the world ground has, and a ground normal reported facing down is the same
// slope: under a placer scaled 2x across the wall, and with downward normals,
// no base vertex stands above a 20-degree cross slope or deeper in it than the
// fall across the wall.
TEST(SplineSweepWall, TheBaseSinksByTheWorldSlopeUnderAScaledPlacerAndDownwardNormals)
{
    const float32 fall = std::tan(20.0f * 3.14159265f / 180.0f);
    const Ground slope = [fall](float32 x, float32) { return -x * fall; };
    for (const bool downward : {false, true})
    {
        for (const float32 acrossScale : {1.0f, 2.0f})
        {
            WallOptions options;
            options.DownwardNormals = downward;
            float32* matrix = options.Placer.Data();
            matrix[0] = acrossScale;
            const WallRun wall = BuildWall(
                LinearSpline({V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 20.0f)}, false), slope,
                WallOf(1.0f), options);
            float32 highest = -1.0e9f;
            float32 lowest = 1.0e9f;
            for (const SG::SplineStripMesh& mesh : wall.Chunks)
            {
                for (uint32 v = 0; v < mesh.RingCount * mesh.RingVertexCount; ++v)
                {
                    const V3 p = options.Placer.TransformPoint(mesh.Vertices[v].Position);
                    const float32 aboveGround = p.y - slope(p.x, p.z);
                    if (aboveGround < 0.5f * kWallHeight)
                    {
                        highest = std::max(highest, aboveGround);
                        lowest = std::min(lowest, aboveGround);
                    }
                }
            }
            EXPECT_LE(highest, 1.0e-4f) << "downward " << downward << ", scale " << acrossScale;
            // Sunk by the fall across the wall and no further: the uphill edge
            // stands in the ground by the wall's world width times tan 20°.
            EXPECT_GE(lowest, -acrossScale * fall - 1.0e-4f)
                << "downward " << downward << ", scale " << acrossScale;
        }
    }
}

namespace
{

// A cubic Bezier from (0,0,0) north to (0,0,10), then east to (10,0,10), with
// straight handles. `broken` points the middle point's two handles along its two
// legs, a 90-degree kink; otherwise they are collinear, and the curve turns
// smoothly through the point.
Spline::SplineData BezierElbow(bool broken)
{
    Spline::SplineData data;
    data.Type = Spline::SplineType::CubicBezier;
    data.AddPoint(V3(0.0f, 0.0f, 0.0f), 0.3f);
    data.AddPoint(V3(0.0f, 0.0f, 10.0f), 0.3f);
    data.AddPoint(V3(10.0f, 0.0f, 10.0f), 0.3f);
    data.Points[0].TangentOut = V3(0.0f, 0.0f, 3.0f);
    data.Points[1].TangentIn = broken ? V3(0.0f, 0.0f, -3.0f) : V3(-3.0f, 0.0f, -3.0f);
    data.Points[1].TangentOut = broken ? V3(3.0f, 0.0f, 0.0f) : V3(3.0f, 0.0f, 3.0f);
    data.Points[2].TangentIn = V3(-3.0f, 0.0f, 0.0f);
    Spline::RebuildSplineCache(data);
    return data;
}

Editor::SweepSamples SampleShape(const Spline::SplineData& data, const Editor::SweepShape& shape)
{
    return Editor::SampleSweepCenterline(data, Editor::CenterlineSampleCount(data.TotalArcLength),
                                         Mathematics::Matrix4x4::Identity(), shape);
}

} // namespace

// A smooth spline's authored point is a corner where its tangent turns by more
// than five degrees across it — a Bezier whose handles are broken there — and
// the sweep puts a ring on it; one whose handles are collinear is part of the
// curve and is not.
TEST(SplineSweepWall, ABezierPointWithBrokenHandlesIsACornerAndACollinearOneIsNot)
{
    Editor::SweepShape shape;
    shape.Corners = SG::SplineCornerStyle::Mitre;
    shape.FollowCurve = true;

    const Editor::SweepSamples broken = SampleShape(BezierElbow(true), shape);
    ASSERT_EQ(broken.Corners.size(), 1u);
    EXPECT_EQ(broken.Corners[0].PointIndex, 1u);
    const V3 onPoint = broken.Frames[broken.Corners[0].StationIndex].Position;
    EXPECT_NEAR(onPoint.x, 0.0f, 1.0e-4f);
    EXPECT_NEAR(onPoint.z, 10.0f, 1.0e-4f);

    EXPECT_TRUE(SampleShape(BezierElbow(false), shape).Corners.empty());
}

// A sweep that steps at its points asks for a sample on every interior authored
// point, corner or not, and gets them listed in order, each standing on its point.
TEST(SplineSweepWall, EveryInteriorPointIsASampleWhenTheShapeStepsThere)
{
    Spline::SplineData data;
    data.Type = Spline::SplineType::CatmullRom;
    for (const V3& p : {V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 10.0f), V3(10.0f, 0.0f, 14.0f),
                        V3(20.0f, 0.0f, 10.0f)})
        data.AddPoint(p, 0.3f);
    Spline::RebuildSplineCache(data);

    Editor::SweepShape shape;
    shape.FollowCurve = true;
    shape.StationAtPoints = true;
    const Editor::SweepSamples samples = SampleShape(data, shape);
    ASSERT_EQ(samples.Points.size(), 2u);
    EXPECT_LT(samples.Points[0], samples.Points[1]);
    for (uint32 i = 0; i < 2u; ++i)
    {
        const V3 position = samples.Frames[samples.Points[i]].Position;
        EXPECT_NEAR(position.x, data.Points[i + 1u].Position.x, 1.0e-4f) << "point " << i + 1u;
        EXPECT_NEAR(position.z, data.Points[i + 1u].Position.z, 1.0e-4f) << "point " << i + 1u;
    }
    EXPECT_TRUE(samples.Corners.empty()) << "no corners were asked for";
}

namespace
{

// The ring vertices of every chunk that lie on a side face: a lateral normal.
template <typename Visit>
void ForEachSideVertex(const WallRun& run, Visit&& visit)
{
    for (const SG::SplineStripMesh& mesh : run.Chunks)
    {
        for (uint32 v = 0; v < mesh.RingCount * mesh.RingVertexCount; ++v)
        {
            if (std::abs(mesh.Vertices[v].Normal.y) < 0.5f)
                visit(mesh, v);
        }
    }
}

} // namespace

// On a straight run no face turns, so every side face takes exactly the U the
// top does, to the bit: U along each face changes nothing where the faces run
// with the centreline.
TEST(SplineSweepWall, AStraightWallsSideFacesTakeTheCentrelinesUToTheBit)
{
    const WallRun run = BuildWall(LinearSpline({V3(0.0f, 0.0f, 0.0f), V3(3.0f, 0.0f, 71.0f)}, false),
                                  Level, WallOf(1.0f));
    ASSERT_GE(run.Chunks.size(), 2u);
    uint32 sideVertices = 0;
    for (const SG::SplineStripMesh& mesh : run.Chunks)
    {
        for (uint32 r = 0; r < mesh.RingCount; ++r)
        {
            // The first slot of a ring is point 0's bottom-face slot, which takes
            // the centreline's U.
            const float32 centreU = mesh.Vertices[r * mesh.RingVertexCount].UV.x;
            for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
            {
                const SG::SplineVertex& v = mesh.Vertices[r * mesh.RingVertexCount + k];
                if (std::abs(v.Normal.y) > 0.5f)
                    continue;
                ++sideVertices;
                EXPECT_EQ(std::memcmp(&v.UV.x, &centreU, sizeof(float32)), 0) << "ring " << r << ", slot " << k;
            }
        }
    }
    EXPECT_GT(sideVertices, 0u);
}

// A welded square closes every face on itself: each side face starts its U at
// zero at the seam and ends it on a whole metre, within half a metre of its own
// length — the outer face 8 half-thicknesses longer than the centreline, the
// inner as much shorter.
TEST(SplineSweepWall, EachSideFaceOfAWeldedWallClosesOnAWholeMetre)
{
    const float32 side = 10.3f;
    const WallRun run = BuildWall(
        LinearSpline({V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, side), V3(side, 0.0f, side), V3(side, 0.0f, 0.0f)},
                     true),
        Level, WallOf(1.0f));
    ASSERT_FALSE(run.Chunks.empty());
    const SG::SplineStripMesh& first = run.Chunks.front();
    const SG::SplineStripMesh& last = run.Chunks.back();
    const uint32 lastRing = (last.RingCount - 1u) * last.RingVertexCount;
    const float32 centreline = 4.0f * side;
    uint32 sides = 0;
    for (uint32 k = 0; k < first.RingVertexCount; ++k)
    {
        const SG::SplineVertex& opening = first.Vertices[k];
        const SG::SplineVertex& closing = last.Vertices[lastRing + k];
        if (std::abs(opening.Normal.y) > 0.5f)
            continue;
        ++sides;
        EXPECT_EQ(opening.UV.x, 0.0f) << "slot " << k;
        EXPECT_NEAR(closing.UV.x, std::round(closing.UV.x), 1.0e-3f) << "slot " << k;
        const bool outer = closing.UV.x > centreline;
        const float32 faceLength = centreline + (outer ? 4.0f : -4.0f);
        EXPECT_LE(std::abs(closing.UV.x - faceLength), 0.5f) << "slot " << k;
    }
    EXPECT_EQ(sides, 4u);
}

// The side faces take V as height above the wall's lowest base point, so the
// courses stay level on a slope: every side vertex's V is its height above that
// one datum, on a wall climbing 1 in 10 along its run.
TEST(SplineSweepWall, SideFacesTakeVAsTheHeightAboveTheWallsLowestBase)
{
    const Ground climb = [](float32, float32 z) { return 0.1f * z; };
    const WallRun run = BuildWall(LinearSpline({V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 30.0f)}, false),
                                  climb, WallOf(1.0f));
    float32 lowestBase = 1.0e9f;
    ForEachSideVertex(run, [&](const SG::SplineStripMesh& mesh, uint32 v)
                      { lowestBase = std::min(lowestBase, mesh.Vertices[v].Position.y); });
    uint32 checked = 0;
    ForEachSideVertex(run,
                      [&](const SG::SplineStripMesh& mesh, uint32 v)
                      {
                          ++checked;
                          const SG::SplineVertex& vertex = mesh.Vertices[v];
                          EXPECT_NEAR(vertex.UV.y, vertex.Position.y - lowestBase, 1.0e-4f);
                      });
    EXPECT_GT(checked, 0u);
}

// A stepped wall on a hill levels each run between two points at the wall's
// height above that run's highest ground, and closes the step with a riser: no
// ring stands shorter than the wall's height, and the two runs' tops stand at
// 3.5 m and 4 m where the ground under them peaks at 0.5 m and 1 m.
TEST(SplineSweepWall, ASteppedWallOnAHillLevelsEachRunsTop)
{
    const Ground hill = [](float32 x, float32 z) { return 0.05f * (x + z); };
    const WallRun run = BuildWall(
        LinearSpline({V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 10.0f), V3(10.0f, 0.0f, 10.0f)}, false),
        hill, WallOf(0.6f, Components::SplineWallCorner::Mitre, Components::SplineWallGrade::Stepped));
    ASSERT_EQ(run.Chunks.size(), 1u);
    const SG::SplineStripMesh& mesh = run.Chunks.front();
    uint32 creases = 0;
    bool secondRun = false;
    for (uint32 r = 0; r < mesh.RingCount; ++r)
    {
        const SG::SplineStripStation& station = run.Stations[r];
        if (station.Join == SG::SplineStationJoin::Crease)
        {
            ++creases;
            secondRun = true;
        }
        float32 top = -1.0e9f;
        float32 bottom = 1.0e9f;
        for (uint32 k = 0; k < mesh.RingVertexCount; ++k)
        {
            top = std::max(top, mesh.Vertices[r * mesh.RingVertexCount + k].Position.y);
            bottom = std::min(bottom, mesh.Vertices[r * mesh.RingVertexCount + k].Position.y);
        }
        EXPECT_NEAR(top, secondRun ? 4.0f : 3.5f, 1.0e-3f) << "ring " << r;
        EXPECT_GE(top - bottom, kWallHeight - 1.0e-3f) << "ring " << r;
    }
    EXPECT_EQ(creases, 1u);
    EXPECT_EQ(mesh.Vertices.size() - static_cast<size_t>(mesh.RingCount) * mesh.RingVertexCount,
              4u + 8u)
        << "one riser and the two end caps";
}

// Corner = Round on a spline of straight segments builds its corners round: nine
// rings on a 90-degree turn, the outside an arc of half the thickness.
TEST(SplineSweepWall, ARoundCornerOnAStraightSegmentSplineIsBuiltRound)
{
    const WallRun run = BuildWall(
        LinearSpline({V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 10.0f), V3(10.0f, 0.0f, 10.0f)}, false),
        Level, WallOf(0.6f, Components::SplineWallCorner::Round));
    EXPECT_TRUE(run.Issues.empty());
    uint32 fan = 0;
    for (const SG::SplineStripStation& station : run.Stations)
        fan += std::abs(station.Distance - 10.0f) < 1.0e-4f ? 1u : 0u;
    EXPECT_EQ(fan, 9u);
    ASSERT_EQ(run.Chunks.size(), 1u);
    EXPECT_EQ(InvertedSideTriangles(run.Chunks.front()), 0u);
}

// A curve that bends tighter than half the wall's thickness folds the inside
// face through itself. A 0.6 m wall on a 1 cm U-turn does, and is reported by
// the two points the curve runs between, with the fix; the same wall on a gentle
// curve reports nothing.
TEST(SplineSweepWall, ACurveTighterThanTheWallIsReportedByItsPoints)
{
    Spline::SplineData uTurn;
    uTurn.Type = Spline::SplineType::CatmullRom;
    for (const V3& p : {V3(0.0f, 0.0f, 0.0f), V3(0.0f, 0.0f, 10.0f), V3(0.01f, 0.0f, 10.0f),
                        V3(0.01f, 0.0f, 0.0f)})
        uTurn.AddPoint(p, 0.3f);
    Spline::RebuildSplineCache(uTurn);
    const WallRun folded = BuildWall(uTurn, Level, WallOf(0.6f));
    std::vector<SG::SplineCornerIssue> folds;
    for (const SG::SplineCornerIssue& issue : folded.Issues)
    {
        if (issue.Kind == SG::SplineCornerIssueKind::InnerFold)
            folds.push_back(issue);
    }
    ASSERT_FALSE(folds.empty());
    const std::vector<std::string> lines = Editor::CornerValidation("SplineWall", folds);
    bool namedTheTurn = false;
    for (const std::string& line : lines)
    {
        namedTheTurn |= line.find("between points 1 and 2") != std::string::npos;
        EXPECT_NE(line.find("Widen the curve or thin the wall"), std::string::npos) << line;
    }
    EXPECT_TRUE(namedTheTurn);

    for (const SG::SplineCornerIssue& issue : BuildWall(BezierElbow(false), Level, WallOf(0.6f)).Issues)
        EXPECT_NE(issue.Kind, SG::SplineCornerIssueKind::InnerFold);
}
