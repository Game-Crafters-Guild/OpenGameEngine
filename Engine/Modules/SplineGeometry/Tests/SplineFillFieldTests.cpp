// The region fill's field and flood, on synthetic heightfields.
//
// Each of the three geometry defect classes the ribbon could not fix has a test
// here that fails for a threshold and passes for a flood, or vice versa —
// connectivity is the property under test, not "does it fill".

#include "SplineGeometry/SplineFillField.h"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::SplineGeometry;
using V3 = Mathematics::Vector3;

namespace
{

// A ground lattice plus the storage behind it, so a test can hand out a grid
// without keeping the vectors alive by hand.
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

// The controller's own station construction: central-difference forward, right
// from cross(worldUp, forwardXZ), up from cross(forward, right), distance walked
// in true 3-D length. Points are already AT the waterline.
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

// A straight run down +Z at x = 0, every station at the same altitude.
std::vector<SplineStripStation> StraightRun(float32 waterline, float32 fromZ, float32 toZ,
                                            float32 step)
{
    std::vector<V3> points;
    for (float32 z = fromZ; z <= toZ + 1.0e-4f; z += step)
        points.push_back(V3(0.0f, waterline, z));
    return MakeStations(points);
}

const SplineFillCorner& At(const SplineFillResult& field, const SplineGroundGrid& grid, uint32 cx,
                           uint32 cz)
{
    return field.Corners[grid.Index(cx, cz)];
}

// Count 4-connected components among wet corners.
uint32 CountWetComponents(const SplineFillResult& field, const SplineGroundGrid& grid)
{
    std::vector<uint8> seen(field.Corners.size(), 0u);
    uint32 components = 0;
    std::vector<uint32> stack;
    for (uint32 start = 0; start < field.Corners.size(); ++start)
    {
        if (!field.Corners[start].Wet || seen[start])
            continue;
        ++components;
        stack.push_back(start);
        seen[start] = 1u;
        while (!stack.empty())
        {
            const uint32 index = stack.back();
            stack.pop_back();
            const uint32 cx = index % grid.CountX;
            const uint32 cz = index / grid.CountX;
            const auto visit = [&](uint32 other)
            {
                if (field.Corners[other].Wet && !seen[other])
                {
                    seen[other] = 1u;
                    stack.push_back(other);
                }
            };
            if (cx > 0u) visit(index - 1u);
            if (cx + 1u < grid.CountX) visit(index + 1u);
            if (cz > 0u) visit(index - grid.CountX);
            if (cz + 1u < grid.CountZ) visit(index + grid.CountX);
        }
    }
    return components;
}

} // namespace

// ---------------------------------------------------------------------------
// The basic shape: a trench fills to its walls and stops there.
// ---------------------------------------------------------------------------

TEST(SplineFillField, AStraightTrenchFillsToItsWallsAndNoFurther)
{
    // Flat bed out to |x| = 2, then walls climbing at 10 m per metre.
    TestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float32 x, float32)
    {
        const float32 wall = std::abs(x) - 2.0f;
        return wall <= 0.0f ? 0.0f : wall * 10.0f;
    });

    const std::vector<SplineStripStation> stations = StraightRun(0.5f, -15.0f, 15.0f, 0.5f);
    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.EdgeDrop = 0.1f;

    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);
    ASSERT_GT(field.Diagnostics.WetCorners, 0u);
    EXPECT_EQ(field.Diagnostics.DrySeeds, 0u);

    // The contour is at phi = -EdgeDrop, i.e. ground = waterline + EdgeDrop:
    // 0.6 = (|x| - 2) * 10 puts the wall crossing at |x| = 2.06.
    for (uint32 cz = 0; cz < ground.Grid.CountZ; ++cz)
    {
        for (uint32 cx = 0; cx < ground.Grid.CountX; ++cx)
        {
            const float32 x = ground.Grid.WorldX(cx);
            const float32 z = ground.Grid.WorldZ(cz);
            if (!At(field, ground.Grid, cx, cz).Wet)
                continue;
            EXPECT_LE(std::abs(x), 2.06f + 1.0e-3f)
                << "water climbed the wall at (" << x << ", " << z << ")";
        }
    }
    // And the bed itself is wet along the run.
    EXPECT_TRUE(At(field, ground.Grid, 40u, 40u).Wet) << "the trench centre must fill";
}

TEST(SplineFillField, TheRegionBoundaryIsTheMinusEdgeDropIso)
{
    // A ramp rising with x, so the uphill crossing is exact arithmetic, closed
    // downhill by a wall at x = -6. The wall is not decoration: with nothing on
    // that side the water would run off down the ramp, and the region would end
    // where the containment test puts it rather than where the iso does.
    TestGround ground(81u, 21u, 0.5f, -20.0f, -5.0f,
                      [](float32 x, float32) { return x <= -6.0f ? 10.0f : x; });

    const std::vector<SplineStripStation> stations = StraightRun(0.0f, -3.0f, 3.0f, 0.5f);
    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.EdgeDrop = 0.25f;

    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    // waterline 0, ground = x, so phi = -x. The iso phi = -EdgeDrop sits at
    // x = +0.25: every corner below it is in, every corner above is out.
    for (uint32 cx = 0; cx < ground.Grid.CountX; ++cx)
    {
        const float32 x = ground.Grid.WorldX(cx);
        if (x > -6.0f && x < 0.2f)
            EXPECT_TRUE(At(field, ground.Grid, cx, 10u).Wet) << "x = " << x;
        if (x > 0.3f)
            EXPECT_FALSE(At(field, ground.Grid, cx, 10u).Wet) << "x = " << x;
    }
}

// The same ramp WITHOUT the downhill wall: her report's case, reduced. A flat
// waterline over ground that keeps falling has nothing holding it up, so the
// region must end on the ground rather than run out to the corridor and hang
// there in mid-air.
TEST(SplineFillField, AWaterlineOverGroundThatKeepsFallingStopsShortOfTheCorridor)
{
    TestGround ground(81u, 21u, 0.5f, -20.0f, -5.0f,
                      [](float32 x, float32) { return x * 1.0f; });

    const std::vector<SplineStripStation> stations = StraightRun(0.0f, -3.0f, 3.0f, 0.5f);
    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.EdgeDrop = 0.25f;

    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    for (uint32 cz = 0; cz < ground.Grid.CountZ; ++cz)
        for (uint32 cx = 0; cx < ground.Grid.CountX; ++cx)
        {
            const SplineFillCorner& corner = At(field, ground.Grid, cx, cz);
            if (corner.Wet)
                EXPECT_LT(corner.Distance, params.MaxHalfWidth - 1.0f)
                    << "water reached the corridor over open falling ground at x = "
                    << ground.Grid.WorldX(cx);
        }
    EXPECT_GT(field.Diagnostics.OverBankCorners, 0u)
        << "the ramp gave the water nothing to stand behind, yet nothing was refused";
}

// ---------------------------------------------------------------------------
// Defect class 2: the pocket inside a bend that no perpendicular ray reaches.
// ---------------------------------------------------------------------------

TEST(SplineFillField, AConcaveBendFillsThePocketNoPerpendicularRayCanReach)
{
    // An L-shaped trench: down +Z to the origin, then off along +X, plus a lobe
    // of bed hanging off the bend.
    //
    // The lobe is placed where the perpendicular march is BLIND. The Z leg's
    // rays sweep the lines z = const for z <= 0; the X leg's sweep x = const for
    // x >= 0. Their union misses the quadrant x < 0, z > 0 entirely — no reach
    // reaches it, because what is missing is a DIRECTION, not a distance.
    TestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float32 x, float32 z)
    {
        const bool inLegZ = std::abs(x) <= 2.0f && z <= 0.0f;
        const bool inLegX = std::abs(z) <= 2.0f && x >= 0.0f;
        const bool lobe = x >= -4.0f && x <= 0.0f && z >= 0.0f && z <= 4.0f;
        return (inLegZ || inLegX || lobe) ? 0.0f : 10.0f;
    });

    std::vector<V3> points;
    for (float32 z = -15.0f; z <= 0.0f; z += 0.5f)
        points.push_back(V3(0.0f, 0.5f, z));
    for (float32 x = 0.5f; x <= 15.0f; x += 0.5f)
        points.push_back(V3(x, 0.5f, 0.0f));
    const std::vector<SplineStripStation> stations = MakeStations(points);

    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    // A point well inside the lobe.
    const uint32 pocketX = 34u; // world x = -3
    const uint32 pocketZ = 44u; // world z = +2
    ASSERT_NEAR(ground.Grid.WorldX(pocketX), -3.0f, 1.0e-4f);
    ASSERT_NEAR(ground.Grid.WorldZ(pocketZ), 2.0f, 1.0e-4f);
    ASSERT_EQ(ground.Heights[ground.Grid.Index(pocketX, pocketZ)], 0.0f)
        << "the pocket must be bed, or the test is not testing the pocket";

    // It is genuinely off every station's perpendicular: a perpendicular march
    // only ever samples points whose ALONG-TRACK offset from some station is
    // zero, and no station puts this point on its own perpendicular line.
    float32 closestAlong = 1.0e9f;
    for (const SplineStripStation& s : stations)
    {
        const float32 dx = ground.Grid.WorldX(pocketX) - s.Position.x;
        const float32 dz = ground.Grid.WorldZ(pocketZ) - s.Position.z;
        const float32 along = std::abs(dx * s.Forward.x + dz * s.Forward.z);
        closestAlong = std::min(closestAlong, along);
    }
    EXPECT_GT(closestAlong, 0.2f)
        << "the pocket lies on some station's perpendicular, so it is not a class-2 case";
    // And it is well within the reach, so a bigger MaxHalfWidth would not have
    // found it either — the ray fit's failure here is directional.
    EXPECT_LT(At(field, ground.Grid, pocketX, pocketZ).Distance, params.MaxHalfWidth);

    EXPECT_TRUE(At(field, ground.Grid, pocketX, pocketZ).Wet)
        << "the fill must reach the pocket the perpendicular march shadows past";
}

// ---------------------------------------------------------------------------
// Defect class 3, first costume: the detached tongue over a ridge.
// ---------------------------------------------------------------------------

TEST(SplineFillField, ABasinBehindARidgeStaysDryEvenThoughItIsBelowTheWaterline)
{
    // Trench at |x| <= 2; a ridge at 3 <= x <= 4; a second basin beyond it that
    // is LOWER than the waterline and well within MaxHalfWidth.
    TestGround ground(81u, 41u, 0.5f, -10.0f, -10.0f, [](float32 x, float32)
    {
        if (std::abs(x) <= 2.0f)
            return 0.0f;      // the channel
        if (x > 4.0f && x < 9.0f)
            return -1.0f;     // the detached basin, BELOW the waterline
        return 5.0f;          // banks and the ridge between them
    });

    const std::vector<SplineStripStation> stations = StraightRun(0.5f, -8.0f, 8.0f, 0.5f);
    SplineFillParams params;
    params.MaxHalfWidth = 12.0f; // the basin is inside the corridor
    params.EdgeDrop = 0.1f;

    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    const uint32 basinX = 32u; // world x = 6
    const uint32 z = 20u;
    ASSERT_NEAR(ground.Grid.WorldX(basinX), 6.0f, 1.0e-4f);

    const SplineFillCorner& basin = At(field, ground.Grid, basinX, z);
    // A THRESHOLD would take it: it is below the waterline and inside the reach.
    EXPECT_GT(basin.Inside, 0.0f)
        << "the basin must pass the per-corner test, or the flood is not what excluded it";
    // The FLOOD does not, because the ridge between is dry.
    EXPECT_FALSE(basin.Wet) << "a puddle over a ridge is the bug34 tongue";
    EXPECT_EQ(CountWetComponents(field, ground.Grid), 1u);
}

// ---------------------------------------------------------------------------
// Defect class 3, second costume: the hairpin that became a lake.
// ---------------------------------------------------------------------------

TEST(SplineFillField, AHairpinOverOneBasinYieldsOneRegionWithAnHonestStep)
{
    // One wide basin; the run enters along +Z at x = -3, turns, and comes back
    // along -Z at x = +3, dropping 1 m over the run.
    TestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float32 x, float32 z)
    {
        return (std::abs(x) <= 8.0f && std::abs(z) <= 8.0f) ? -1.0f : 10.0f;
    });

    std::vector<V3> points;
    for (float32 z = -7.0f; z <= 6.0f; z += 0.5f)
        points.push_back(V3(-3.0f, 0.5f, z));
    for (float32 x = -2.5f; x <= 3.0f; x += 0.5f)
        points.push_back(V3(x, 0.5f, 6.5f));
    for (float32 z = 6.0f; z >= -7.0f; z -= 0.5f)
        points.push_back(V3(3.0f, 0.5f, z));
    // Drop the whole run 1 m from source to mouth, linearly in index.
    for (size_t i = 0; i < points.size(); ++i)
        points[i].y -= 1.0f * static_cast<float32>(i) / static_cast<float32>(points.size() - 1u);
    const std::vector<SplineStripStation> stations = MakeStations(points);

    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    ASSERT_GT(field.Diagnostics.WetCorners, 0u);
    // ONE region, visited once, meshed once — there is no second surface to
    // z-fight, which is what the two independently-solved ribbons produced.
    EXPECT_EQ(CountWetComponents(field, ground.Grid), 1u);

    // The step down the middle is REPORTED rather than flattened away: between
    // the two legs the waterline field is discontinuous by the along-run drop.
    EXPECT_GT(field.Diagnostics.MedialStepMetres, 0.1f)
        << "the medial step is the honest cost of the closest-point field, and must be visible";
    EXPECT_LT(field.Diagnostics.MedialStepMetres, 1.01f)
        << "no step can exceed the run's own total drop";
}

// ---------------------------------------------------------------------------
// 4-connectivity, seeds, spill, the sea floor, and the budget.
// ---------------------------------------------------------------------------

TEST(SplineFillField, WaterDoesNotLeakThroughAOneCellDiagonalGap)
{
    // Two basins touching only at a diagonal: an 8-connected flood would leak.
    // The upper one is CLOSED -- a pocket, not an open quadrant -- so that it is
    // genuinely capable of holding water and connectivity is the only thing that
    // can keep it dry. An open one would drain outward, and this test would then
    // pass without exercising the flood at all.
    const float32 spacing = 1.0f;
    TestGround ground(21u, 21u, spacing, -10.0f, -10.0f, [](float32 x, float32 z)
    {
        const bool lower = x <= 0.0f && z <= 0.0f;
        const bool upper = x >= 1.0f && x <= 6.0f && z >= 1.0f && z <= 6.0f;
        return (lower || upper) ? 0.0f : 10.0f;
    });

    // A run confined to the lower basin.
    std::vector<V3> points;
    for (float32 z = -6.0f; z <= 0.0f; z += 1.0f)
        points.push_back(V3(-3.0f, 0.5f, z));
    const std::vector<SplineStripStation> stations = MakeStations(points);

    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    // The upper basin shares only the (0,0)/(1,1) diagonal with the lower one.
    const uint32 upperX = 15u; // world x = 5
    const uint32 upperZ = 15u; // world z = 5
    EXPECT_GT(At(field, ground.Grid, upperX, upperZ).Inside, 0.0f);
    EXPECT_FALSE(At(field, ground.Grid, upperX, upperZ).Wet)
        << "an 8-connected flood leaks here; water is conservative and must not";
    EXPECT_EQ(CountWetComponents(field, ground.Grid), 1u);
}

TEST(SplineFillField, ASeedAboveItsOwnBedIsReportedRatherThanFilled)
{
    // Ground above the waterline everywhere: nothing to fill.
    TestGround ground(41u, 41u, 0.5f, -10.0f, -10.0f,
                      [](float32, float32) { return 5.0f; });

    const std::vector<SplineStripStation> stations = StraightRun(0.5f, -5.0f, 5.0f, 0.5f);
    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    EXPECT_EQ(field.Diagnostics.WetCorners, 0u);
    EXPECT_GT(field.Diagnostics.DrySeeds, 0u);
    EXPECT_EQ(field.Diagnostics.DrySeeds, field.Diagnostics.Seeds)
        << "every seed is dry on ground this high";
    EXPECT_NEAR(field.Diagnostics.FirstDrySeed.y, 0.5f, 1.0e-4f);
}

TEST(SplineFillField, WaterWithNoBankInsideTheReachIsRefusedAndNamed)
{
    // Flat ground far wider than the reach. There is no bank anywhere the fill
    // can see, so nothing holds this water: it builds NOTHING and says which
    // knob is wrong, rather than laying a disc of water with a vertical face
    // where the corridor happens to end.
    TestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f,
                      [](float32, float32) { return -1.0f; });

    const std::vector<SplineStripStation> stations = StraightRun(0.5f, -10.0f, 10.0f, 0.5f);
    SplineFillParams params;
    params.MaxHalfWidth = 4.0f;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    EXPECT_EQ(field.Diagnostics.WetCorners, 0u);
    EXPECT_EQ(field.Diagnostics.DrySeeds, 0u) << "the bed IS below the waterline; that is not why";
    EXPECT_GT(field.Diagnostics.OverBankCorners, 0u);
    // Every refusal here is for want of a bank inside the reach, not for a bank
    // that is genuinely too low: the two are opposite instructions to an author.
    EXPECT_EQ(field.Diagnostics.UnseenBankCorners, field.Diagnostics.OverBankCorners);
}

TEST(SplineFillField, TheSameRunWithBanksInsideTheReachFillsAndNamesNothing)
{
    // The control for the test above: banks the fill can see, so the GROUND is
    // what stops the water and no sample is refused for want of evidence.
    // Without this, "UnseenBankCorners > 0" above could be an artefact of the
    // diagnostic rather than a fact about the run.
    TestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float32 x, float32 z)
    {
        return std::abs(x) <= 2.0f && std::abs(z) <= 14.0f ? -1.0f : 10.0f;
    });

    const std::vector<SplineStripStation> stations = StraightRun(0.5f, -10.0f, 10.0f, 0.5f);
    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    ASSERT_GT(field.Diagnostics.WetCorners, 0u);
    EXPECT_EQ(field.Diagnostics.OverBankCorners, 0u);
    EXPECT_EQ(field.Diagnostics.UnseenBankCorners, 0u);
}

// A refusal's WHERE, not only its how-many. Two leaks in one field, each
// confined to a single lattice row, so the worst sample of each kind is a
// corner this test can name outright:
//
//   z = -4  the ground past the bank drops to 0.80 m and stays there out beyond
//           the corridor, so the rim the water leaves over is the limit of the
//           SEARCH, not a bank -- refused by 0.20 m, and UNSEEN.
//   z = +4  the bank crests at 0.60 m one corner INSIDE the corridor before the
//           ground falls away, so the rim is terrain the run can see -- refused
//           by 0.40 m, and SEEN.
//
// The unseen row comes FIRST in lattice order and carries the SMALLER
// overshoot, so a "worst" that is really "the first one found" names the wrong
// row, and a field that reports one position for both kinds names the wrong
// fix.
TEST(SplineFillField, TheWorstRefusalOfEachKindNamesItsOwnCorner)
{
    constexpr float32 kWaterline = 1.0f;
    constexpr float32 kBedHalfWidth = 3.0f;
    // The channel is CLOSED at both ends, just past the last station. Past the
    // ends, distance-to-the-run grows along the channel rather than across it,
    // so a bed that simply ran off the grid would be refused up- and
    // downstream as well -- honest (the run ends there), and enough of it to
    // bury the two refusals this test is about.
    constexpr float32 kBedHalfLength = 13.0f;
    constexpr float32 kBankTop = 5.0f;
    constexpr float32 kSpacing = 0.5f;
    constexpr float32 kOriginZ = -15.0f;
    // The two leaking rows, and the run's first station.
    constexpr float32 kUnseenRowZ = -4.0f;
    constexpr float32 kSeenRowZ = 4.0f;
    constexpr float32 kRunStartZ = -12.0f;
    // Ground in the unseen row past the bed: below the water all the way out.
    constexpr float32 kUnseenLeak = 0.8f;
    // The seen row: a crest on the first corner past the bed, then lower ground
    // that lies AT or beyond the corridor edge and so is never tallied.
    constexpr float32 kCrestX = 3.5f;
    constexpr float32 kSeenCrest = 0.6f;
    constexpr float32 kPastCrest = 0.2f;
    constexpr float32 kReach = 4.0f;

    const auto sameRow = [](float32 a, float32 b) { return std::abs(a - b) < kSpacing * 0.5f; };
    TestGround ground(41u, 61u, kSpacing, -10.0f, kOriginZ, [&](float32 x, float32 z)
    {
        if (x > kBedHalfWidth)
        {
            if (sameRow(z, kUnseenRowZ))
                return kUnseenLeak;
            if (sameRow(z, kSeenRowZ))
                return sameRow(x, kCrestX) ? kSeenCrest : kPastCrest;
        }
        return std::abs(x) <= kBedHalfWidth && std::abs(z) <= kBedHalfLength ? 0.0f : kBankTop;
    });

    const std::vector<SplineStripStation> stations =
        StraightRun(kWaterline, kRunStartZ, -kRunStartZ, kSpacing);
    SplineFillParams params;
    params.MaxHalfWidth = kReach;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);
    const SplineFillDiagnostics& d = field.Diagnostics;

    ASSERT_GT(d.WetCorners, 0u);
    // Eight corners per row (x = 0 .. 3.5): the bed from the centreline out,
    // plus the crest itself. Corners at x >= 4.0 are at or past the corridor
    // edge and are not the author's to fix.
    EXPECT_EQ(d.OverBankCorners, 16u);
    EXPECT_EQ(d.UnseenBankCorners, 8u);
    EXPECT_NEAR(d.MaxOverBankMetres, kWaterline - kSeenCrest, 1.0e-5f);

    EXPECT_FLOAT_EQ(d.WorstOverBank.X, 0.0f);
    EXPECT_FLOAT_EQ(d.WorstOverBank.Z, kSeenRowZ);
    EXPECT_FLOAT_EQ(d.WorstOverBank.ArcDistance, kSeenRowZ - kRunStartZ);

    EXPECT_FLOAT_EQ(d.WorstUnseenBank.X, 0.0f);
    EXPECT_FLOAT_EQ(d.WorstUnseenBank.Z, kUnseenRowZ);
    EXPECT_FLOAT_EQ(d.WorstUnseenBank.ArcDistance, kUnseenRowZ - kRunStartZ);

    // Read the field back AT the coordinates it reported: the position has to
    // land on a corner that really was refused, and really is the worst of its
    // kind. Without this the numbers above could agree with a hand-derivation
    // and still not name a refusal.
    const auto cornerAt = [&](const SplineFillWorstSample& sample) -> const SplineFillCorner&
    {
        const uint32 cx = static_cast<uint32>(
            std::lround((sample.X - ground.Grid.OriginX) / ground.Grid.SpacingX));
        const uint32 cz = static_cast<uint32>(
            std::lround((sample.Z - ground.Grid.OriginZ) / ground.Grid.SpacingZ));
        return At(field, ground.Grid, cx, cz);
    };

    const SplineFillCorner& overBank = cornerAt(d.WorstOverBank);
    EXPECT_EQ(overBank.Wet, 0u);
    EXPECT_NE(overBank.Station, kNoStation);
    EXPECT_FLOAT_EQ(overBank.ArcDistance, d.WorstOverBank.ArcDistance);
    EXPECT_NEAR(overBank.Waterline - overBank.Escape, d.MaxOverBankMetres, 1.0e-5f);
    EXPECT_EQ(overBank.EscapeUnseen, 0u) << "the deeper refusal is the one the run CAN see";

    const SplineFillCorner& unseen = cornerAt(d.WorstUnseenBank);
    EXPECT_EQ(unseen.Wet, 0u);
    EXPECT_NE(unseen.Station, kNoStation);
    EXPECT_FLOAT_EQ(unseen.ArcDistance, d.WorstUnseenBank.ArcDistance);
    EXPECT_NE(unseen.EscapeUnseen, 0u)
        << "the unseen-bank warning is pointing at a bank the run could see";
    EXPECT_NEAR(unseen.Waterline - unseen.Escape, kWaterline - kUnseenLeak, 1.0e-5f);
}

TEST(SplineFillField, TheSeaLevelFloorLiftsTheWaterlineAndNeverLowersIt)
{
    // A run descending below y = 0 into a basin that continues below sea level.
    TestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f, [](float32 x, float32)
    {
        return std::abs(x) <= 3.0f ? -5.0f : 10.0f;
    });

    std::vector<V3> points;
    for (float32 z = -15.0f; z <= 15.0f; z += 0.5f)
        points.push_back(V3(0.0f, 2.0f - (z + 15.0f) * 0.1f, z)); // 2 m down to -1 m
    const std::vector<SplineStripStation> stations = MakeStations(points);

    SplineFillParams unclamped;
    unclamped.MaxHalfWidth = 12.0f;
    unclamped.EdgeDrop = 0.1f;
    const SplineFillResult without = BuildSplineFillField(stations, ground.Grid, unclamped);

    SplineFillParams clamped = unclamped;
    clamped.SeaLevelFloor = 0.5f;
    const SplineFillResult with = BuildSplineFillField(stations, ground.Grid, clamped);

    bool anyLifted = false;
    for (size_t i = 0; i < with.Corners.size(); ++i)
    {
        if (with.Corners[i].Station == kNoStation)
            continue;
        EXPECT_GE(with.Corners[i].Waterline, 0.5f - 1.0e-4f) << "the floor must hold";
        EXPECT_GE(with.Corners[i].Waterline, without.Corners[i].Waterline - 1.0e-4f)
            << "a floor may only raise the waterline, never lower it";
        if (with.Corners[i].Waterline > without.Corners[i].Waterline + 1.0e-3f)
            anyLifted = true;
    }
    EXPECT_TRUE(anyLifted) << "this run drops below the floor, so something must have been lifted";
}

TEST(SplineFillField, TheDefaultSeaLevelFloorChangesNothing)
{
    TestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f, [](float32 x, float32)
    {
        return std::abs(x) <= 3.0f ? -5.0f : 10.0f;
    });
    std::vector<V3> points;
    for (float32 z = -15.0f; z <= 15.0f; z += 0.5f)
        points.push_back(V3(0.0f, 2.0f - (z + 15.0f) * 0.1f, z));
    const std::vector<SplineStripStation> stations = MakeStations(points);

    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    const SplineFillResult defaulted = BuildSplineFillField(stations, ground.Grid, params);

    SplineFillParams explicitly = params;
    explicitly.SeaLevelFloor = kNoSeaLevelFloor;
    const SplineFillResult same = BuildSplineFillField(stations, ground.Grid, explicitly);

    ASSERT_EQ(defaulted.Corners.size(), same.Corners.size());
    for (size_t i = 0; i < same.Corners.size(); ++i)
        EXPECT_EQ(defaulted.Corners[i].Waterline, same.Corners[i].Waterline);
    EXPECT_EQ(defaulted.Diagnostics.WetCorners, same.Diagnostics.WetCorners);
}

TEST(SplineFillField, ARegionOverTheBudgetBuildsNothingAndSaysSo)
{
    // A closed basin the run sits in the middle of, so it genuinely floods and
    // the budget is what stops it -- flat open ground would now be refused for
    // having no bank, and this test would pass for the wrong reason.
    TestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float32 x, float32 z)
    {
        return std::abs(x) <= 8.0f && std::abs(z) <= 14.0f ? -1.0f : 10.0f;
    });

    const std::vector<SplineStripStation> stations = StraightRun(0.5f, -10.0f, 10.0f, 0.5f);
    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.MaxWetCorners = 64u; // far below what this basin fills

    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);
    EXPECT_TRUE(field.Diagnostics.ExceededBudget);
    EXPECT_EQ(field.Diagnostics.WetCorners, 0u);
    EXPECT_FALSE(field.HasRegion()) << "over budget must build NOTHING, not a coarser surface";
}

TEST(SplineFillField, AWaterlineThatRisesDownstreamIsReportedRatherThanClamped)
{
    TestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f,
                      [](float32, float32) { return -1.0f; });

    std::vector<V3> points;
    for (float32 z = -10.0f; z <= 10.0f; z += 0.5f)
        points.push_back(V3(0.0f, z > 0.0f ? 1.0f : 0.0f, z)); // a 1 m step UP mid-run
    const std::vector<SplineStripStation> stations = MakeStations(points);

    SplineFillParams params;
    params.MaxHalfWidth = 6.0f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    EXPECT_GT(field.Diagnostics.WaterlineRiseMetres, kWaterlineRiseToleranceMetres);
    // Not clamped: the water the author placed is still where they placed it.
    EXPECT_NEAR(field.Diagnostics.WaterlineRiseMetres, 1.0f, 1.0e-4f);
}

// ---------------------------------------------------------------------------
// The invariant the mesher depends on.
// ---------------------------------------------------------------------------

TEST(SplineFillField, EveryBoundaryEdgeBracketsTheIso)
{
    // The mesher interpolates Inside across any lattice edge with exactly one
    // wet endpoint, and needs that edge to bracket zero. It always does: two
    // 4-adjacent corners with Inside > 0 are in the SAME component, so a wet
    // corner's dry 4-neighbour cannot also be positive.
    TestGround ground(81u, 81u, 0.5f, -20.0f, -20.0f, [](float32 x, float32 z)
    {
        // Deliberately awkward: a winding channel with side pockets and a ridge.
        const float32 bed = std::sin(z * 0.35f) * 3.0f;
        const float32 across = std::abs(x - bed);
        if (across <= 2.5f)
            return -0.5f;
        if (across > 6.0f && across < 8.0f)
            return -0.8f; // detached pockets, below the waterline
        return 4.0f;
    });

    std::vector<V3> points;
    for (float32 z = -15.0f; z <= 15.0f; z += 0.5f)
        points.push_back(V3(std::sin(z * 0.35f) * 3.0f, 0.2f, z));
    const std::vector<SplineStripStation> stations = MakeStations(points);

    SplineFillParams params;
    params.MaxHalfWidth = 9.0f;
    params.EdgeDrop = 0.15f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);
    ASSERT_GT(field.Diagnostics.WetCorners, 0u);

    uint32 boundaryEdges = 0;
    const auto check = [&](uint32 a, uint32 b)
    {
        const bool wetA = field.Corners[a].Wet != 0u;
        const bool wetB = field.Corners[b].Wet != 0u;
        if (wetA == wetB)
            return;
        ++boundaryEdges;
        const SplineFillCorner& in = wetA ? field.Corners[a] : field.Corners[b];
        const SplineFillCorner& out = wetA ? field.Corners[b] : field.Corners[a];
        EXPECT_GT(in.Inside, 0.0f);
        EXPECT_LE(out.Inside, 0.0f)
            << "a dry 4-neighbour of a wet corner must not be inside — the crossing would "
               "have no bracket";
    };
    for (uint32 cz = 0; cz < ground.Grid.CountZ; ++cz)
    {
        for (uint32 cx = 0; cx < ground.Grid.CountX; ++cx)
        {
            const uint32 index = ground.Grid.Index(cx, cz);
            if (cx + 1u < ground.Grid.CountX)
                check(index, index + 1u);
            if (cz + 1u < ground.Grid.CountZ)
                check(index, index + ground.Grid.CountX);
        }
    }
    EXPECT_GT(boundaryEdges, 50u) << "this scene must actually have a boundary to check";
}

namespace
{

// Dry corners the wet region encloses on every side, whose own ground is below
// the water standing around them. Water cannot drain through water, so this
// count is the model's own contradiction and must be zero: any such corner was
// refused an escape route that runs through the very water holding it in.
//
// Ground ABOVE the waterline is excluded on purpose — that is an islet, and an
// islet is meant to stay dry-topped.
uint32 CountEnclosedDryBelowWaterline(const SplineFillResult& field,
                                      const SplineGroundGrid& grid)
{
    std::vector<uint8> reachable(field.Corners.size(), 0u);
    std::vector<uint32> stack;
    const auto visit = [&](uint32 index)
    {
        if (reachable[index] || field.Corners[index].Wet)
            return;
        reachable[index] = 1u;
        stack.push_back(index);
    };
    for (uint32 cx = 0; cx < grid.CountX; ++cx)
    {
        visit(grid.Index(cx, 0u));
        visit(grid.Index(cx, grid.CountZ - 1u));
    }
    for (uint32 cz = 0; cz < grid.CountZ; ++cz)
    {
        visit(grid.Index(0u, cz));
        visit(grid.Index(grid.CountX - 1u, cz));
    }
    while (!stack.empty())
    {
        const uint32 index = stack.back();
        stack.pop_back();
        const uint32 cx = index % grid.CountX;
        const uint32 cz = index / grid.CountX;
        if (cx > 0u)
            visit(index - 1u);
        if (cx + 1u < grid.CountX)
            visit(index + 1u);
        if (cz > 0u)
            visit(index - grid.CountX);
        if (cz + 1u < grid.CountZ)
            visit(index + grid.CountX);
    }

    uint32 enclosed = 0;
    for (uint32 index = 0; index < field.Corners.size(); ++index)
    {
        const SplineFillCorner& corner = field.Corners[index];
        if (corner.Wet || reachable[index] || corner.Station == kNoStation)
            continue;
        if (corner.Waterline - grid.Heights[index] > 0.0f)
            ++enclosed;
    }
    return enclosed;
}

// A run that doubles back on itself, over ground whose inner bank carries a low
// col. The bend is what makes a pocket possible at all: on a straight run the
// escape chain is a single lateral ray, so the corners it refuses always reach
// the corridor edge and drain. A chain that turns can bottom out in the middle
// of the region instead.
std::vector<SplineStripStation> HairpinRun(float32 waterline)
{
    std::vector<V3> points;
    for (float32 z = -18.0f; z <= 0.0f; z += 0.5f)
        points.push_back(V3(-6.0f, waterline, z));
    for (float32 a = 0.0f; a <= 3.14159f; a += 0.15f)
        points.push_back(V3(-6.0f + 6.0f * (1.0f - std::cos(a)), waterline, 6.0f * std::sin(a)));
    for (float32 z = 0.0f; z >= -18.0f; z -= 0.5f)
        points.push_back(V3(6.0f, waterline, z));
    return MakeStations(points);
}

} // namespace

// A property the settled region must have: no dry corner below the water
// standing around it, ringed by that water on every side. Water cannot drain
// through water, so such a corner would be the model contradicting itself.
//
// This hairpin does not produce one on its own: at a CONSTANT waterline every
// containment refusal keeps a dry 4-neighbour chain of increasing Distance out
// of the corridor, so it drains. The chain breaks only where the waterline
// varies between neighbours — a descending river over irregular banks — and no
// synthetic configuration reproduces that here: sweeps over constant-waterline
// hairpins (168 configurations) and descending hairpins with sinusoidal and
// tilted inner banks (1,800+ configurations) found no containment-class pocket.
// LIMITATION: deleting the seal leaves this whole suite green. The sole red arm
// for the seal is WaterFillLiftRepro against ComposedIsland (env-gated, content
// repo); this test guards the invariant itself, not the seal's presence.
TEST(SplineFillField, WaterThatEnclosesDryGroundBelowItsOwnSurfaceLeavesNoPocket)
{
    // Inner bank of the hairpin (the strip between the two legs) carries a col
    // low enough to sit under a lifted waterline.
    TestGround ground(81u, 101u, 0.5f, -20.0f, -20.0f, [](float32 x, float32 z)
    {
        const float32 bed = -4.0f;
        if (x <= -3.0f && x >= -9.0f && z <= 1.0f)
            return bed; // left leg
        if (x >= 3.0f && x <= 9.0f && z <= 1.0f)
            return bed; // right leg
        const float32 r = std::sqrt(x * x + z * z);
        if (z >= 0.0f && r >= 3.0f && r <= 9.0f)
            return bed; // the bend
        if (x > -3.0f && x < 3.0f && z > -14.0f && z < 0.0f)
            return 1.2f; // the inner bank between the legs, with a low crest
        return 12.0f;
    });

    // Waterline ABOVE the inner bank's crest: the lift that exposed the defect.
    const std::vector<SplineStripStation> stations = HairpinRun(2.5f);

    SplineFillParams params;
    params.MaxHalfWidth = 14.0f;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    ASSERT_GT(field.Diagnostics.WetCorners, 0u) << "the run must actually hold water to test";
    EXPECT_EQ(CountEnclosedDryBelowWaterline(field, ground.Grid), 0u)
        << "a dry corner the water surrounds has nowhere to drain to";
}

// The other half of the same rule: ground standing ABOVE the water it is
// surrounded by is an island, and sealing must not drown it.
TEST(SplineFillField, AnIsletStandingAboveTheWaterlineKeepsItsDryTop)
{
    // A straight channel with a rock in the middle of it, well clear of the
    // surface.
    TestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f, [](float32 x, float32 z)
    {
        if (std::abs(x) > 4.0f)
            return 12.0f;
        const float32 r = std::sqrt(x * x + z * z);
        return r <= 2.0f ? 4.0f : -4.0f; // rock top at 4 m, bed at -4 m
    });
    const std::vector<SplineStripStation> stations = StraightRun(0.0f, -15.0f, 15.0f, 0.5f);

    SplineFillParams params;
    params.MaxHalfWidth = 8.0f;
    params.EdgeDrop = 0.1f;
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    const SplineFillCorner& rockTop = At(field, ground.Grid, 20u, 40u); // x = 0, z = 0
    EXPECT_EQ(rockTop.Wet, 0u) << "ground above the waterline is an island, not a pocket";
    EXPECT_EQ(field.Diagnostics.SealedPocketCorners, 0u)
        << "an islet is not a pocket: nothing here should have been sealed";
    EXPECT_EQ(CountEnclosedDryBelowWaterline(field, ground.Grid), 0u);
}

// Defect: with the floor set to the sea's own altitude the mouth was EXACTLY
// coplanar with the sea, and the junction read as lattice-aligned stripes.
TEST(SplineFillField, TheSeaLevelFloorStandsClearOfTheLevelItNames)
{
    TestGround ground(41u, 81u, 0.5f, -10.0f, -20.0f, [](float32 x, float32)
    {
        return std::abs(x) <= 3.0f ? -5.0f : 10.0f;
    });
    std::vector<V3> points;
    for (float32 z = -15.0f; z <= 15.0f; z += 0.5f)
        points.push_back(V3(0.0f, 2.0f - (z + 15.0f) * 0.1f, z));
    const std::vector<SplineStripStation> stations = MakeStations(points);

    SplineFillParams params;
    params.MaxHalfWidth = 12.0f;
    params.SeaLevelFloor = 3.0f; // the sea plane's own altitude
    const SplineFillResult field = BuildSplineFillField(stations, ground.Grid, params);

    bool anyOnTheFloor = false;
    for (const SplineFillCorner& corner : field.Corners)
    {
        if (corner.Station == kNoStation)
            continue;
        EXPECT_GT(corner.Waterline, 3.0f)
            << "water sitting EXACTLY on the sea plane z-fights with it";
        if (corner.Waterline < 3.0f + kSeaLevelFloorLiftMetres * 2.0f)
            anyOnTheFloor = true;
    }
    EXPECT_TRUE(anyOnTheFloor) << "this run drops below the floor, so the floor must bind";
    EXPECT_LT(kSeaLevelFloorLiftMetres, 0.05f) << "a lift big enough to see is too big";
}

// The disabled floor must survive the lift with no branch guarding it: at
// kNoSeaLevelFloor's magnitude the addition is below one ULP.
TEST(SplineFillField, TheLiftCannotDisturbTheDisabledFloor)
{
    EXPECT_EQ(kNoSeaLevelFloor + kSeaLevelFloorLiftMetres, kNoSeaLevelFloor);
}

