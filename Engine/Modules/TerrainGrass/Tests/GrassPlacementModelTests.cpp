#include "TerrainGrass/GrassClumpModel.h"
#include "TerrainGrass/GrassPlacementModel.h"
#include "TerrainGrass/TerrainGrassPlacementStats.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

// The placement model is the arithmetic the GPU runs, mirrored on the CPU so the budget fit and the
// dispatch extent are derived from the same slot counts the compute spawns. These tests pin the
// determinism contract, the scale-invariance claim, and the golden hash values a GLSL drift would
// break (grass_placement.glsl is the mirror; a change to one without the other reds this suite).

namespace GameEngine::TerrainGrass
{
namespace
{

// Defaults read from the struct rather than repeating literals: a bare 8.0f here read as a
// statement about the shipped density, and went stale the moment that changed.
GrassPlacementParams MakeParams(float32 nearDensity = GrassPlacementParams{}.NearDensity,
                                float32 range = GrassPlacementParams{}.FarRadius,
                                float32 falloff = GrassPlacementParams{}.Falloff,
                                float32 seed = GrassPlacementParams{}.Seed)
{
    GrassPlacementParams p;
    p.NearDensity = nearDensity;
    p.FarRadius = range;
    p.Falloff = falloff;
    p.Seed = seed;
    return p;
}

} // namespace

// --- Determinism --------------------------------------------------------------------------------

TEST(GrassPlacementModel, SameSiteAndSeedProduceBitIdenticalBlades)
{
    for (int32 cellX : {-2048, -7, 0, 13, 100000})
    {
        for (uint32 slot : {0u, 1u, 63u, 511u})
        {
            const GrassSlotSample a = GrassSampleSlot(cellX, cellX / 3, slot, 3u);
            const GrassSlotSample b = GrassSampleSlot(cellX, cellX / 3, slot, 3u);
            EXPECT_EQ(a.CellU, b.CellU);
            EXPECT_EQ(a.CellV, b.CellV);
            EXPECT_EQ(a.Yaw01, b.Yaw01);
            EXPECT_EQ(a.ScaleRand, b.ScaleRand);
            EXPECT_EQ(a.WidthRand, b.WidthRand);
            EXPECT_EQ(a.MaskRand, b.MaskRand);
        }
    }
}

// The control for the test above: identical output for identical input is only meaningful if a
// different seed actually reshuffles the field.
TEST(GrassPlacementModel, ADifferentSeedMovesEssentiallyEveryBlade)
{
    constexpr uint32 kSamples = 512;
    uint32 moved = 0;
    for (uint32 slot = 0; slot < kSamples; ++slot)
    {
        const GrassSlotSample a = GrassSampleSlot(4, -9, slot, 3u);
        const GrassSlotSample b = GrassSampleSlot(4, -9, slot, 4u);
        if (a.CellU != b.CellU || a.CellV != b.CellV)
            ++moved;
    }
    EXPECT_EQ(moved, kSamples);
}

TEST(GrassPlacementModel, NeighbouringCellsDoNotShareTheirBladePattern)
{
    uint32 differing = 0;
    for (uint32 slot = 0; slot < 128; ++slot)
    {
        const GrassSlotSample a = GrassSampleSlot(0, 0, slot, 3u);
        const GrassSlotSample b = GrassSampleSlot(1, 0, slot, 3u);
        if (a.CellU != b.CellU || a.CellV != b.CellV)
            ++differing;
    }
    // The cells share a Halton sequence but not the per-cell rotation, so every slot lands
    // somewhere else. Identical cells would read as a tiled pattern at the cell size.
    EXPECT_EQ(differing, 128u);
}

// The anti-teleport property the fixed lattice could not have: a blade's identity is (cell, slot),
// so how many slots the cell currently wants — which moves with the camera every frame — cannot
// disturb a blade that already exists.
TEST(GrassPlacementModel, ABladeDoesNotMoveWhenTheCellsSlotCountChanges)
{
    const GrassPlacementParams params = MakeParams();
    const std::array<float32, 4> distances = {0.0f, 50.0f, 200.0f, 400.0f};

    std::vector<float32> slotCounts;
    for (float32 d : distances)
        slotCounts.push_back(GrassSlotsWantedForCell(params, d));
    // Guard the premise: the slot count really does change over these distances.
    EXPECT_GT(slotCounts.front(), slotCounts.back());

    const GrassSlotSample reference = GrassSampleSlot(11, -3, 7u, 3u);
    for (size_t i = 0; i < distances.size(); ++i)
    {
        if (GrassSlotsToIterate(slotCounts[i]) <= 7u)
            continue; // the cell no longer reaches slot 7 at this distance
        const GrassSlotSample s = GrassSampleSlot(11, -3, 7u, 3u);
        EXPECT_EQ(s.CellU, reference.CellU);
        EXPECT_EQ(s.CellV, reference.CellV);
    }
}

// Golden values. These are the exact numbers grass_placement.glsl must produce; they exist so a
// drift between the two mirrors is a red test rather than a subtly different field of grass.
TEST(GrassPlacementModel, HashGoldenValues)
{
    EXPECT_EQ(GrassHashSite(0, 0, 0u, 3u), 0x826E2BE1u);
    EXPECT_EQ(GrassHashSite(0, 0, 1u, 3u), 0xC87A800Cu);
    EXPECT_EQ(GrassHashSite(-7, 13, 5u, 3u), 0xC262619Cu);
    EXPECT_EQ(GrassHashSite(1000, -1000, 63u, 777u), 0xF0C98D84u);
    EXPECT_EQ(GrassHashNext(GrassHashSite(0, 0, 0u, 3u)), 0xCBD0E7E7u);
}

TEST(GrassPlacementModel, RadicalInverseGoldenValues)
{
    EXPECT_FLOAT_EQ(GrassRadicalInverse2(0u), 0.0f);
    EXPECT_FLOAT_EQ(GrassRadicalInverse2(1u), 0.5f);
    EXPECT_FLOAT_EQ(GrassRadicalInverse2(2u), 0.25f);
    EXPECT_FLOAT_EQ(GrassRadicalInverse2(3u), 0.75f);
    EXPECT_FLOAT_EQ(GrassRadicalInverse3(1u), 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(GrassRadicalInverse3(2u), 2.0f / 3.0f);
    EXPECT_FLOAT_EQ(GrassRadicalInverse3(3u), 1.0f / 9.0f);
}

// --- Scale invariance ---------------------------------------------------------------------------

// The headline claim of the rewrite. Nothing about a terrain's size enters the density model, so
// the authored blades/m² is what a cell at the camera spawns, at every terrain scale.
TEST(GrassPlacementModel, DensityAtTheCameraIsTheAuthoredBladesPerSquareMetre)
{
    for (float32 authored : {2.0f, 8.0f, 32.0f, 64.0f})
    {
        const GrassPlacementParams params = MakeParams(authored);
        const float32 slots = GrassSlotsWantedForCell(params, 0.0f);
        EXPECT_NEAR(slots / kGrassCellArea, authored, 1e-4f) << "authored=" << authored;
    }
}

TEST(GrassPlacementModel, DensityFallsToZeroExactlyAtTheRange)
{
    const GrassPlacementParams params = MakeParams(8.0f, 500.0f, 2.0f);
    EXPECT_NEAR(GrassDensityAtDistance(params, 500.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(GrassDensityAtDistance(params, 600.0f), 0.0f, 1e-6f);
    EXPECT_GT(GrassDensityAtDistance(params, 499.0f), 0.0f);
}

TEST(GrassPlacementModel, DensityIsMonotonicallyDecreasingWithDistance)
{
    const GrassPlacementParams params = MakeParams();
    float32 previous = GrassDensityAtDistance(params, 0.0f);
    for (float32 d = 1.0f; d <= 500.0f; d += 1.0f)
    {
        const float32 current = GrassDensityAtDistance(params, d);
        EXPECT_LE(current, previous) << "d=" << d;
        previous = current;
    }
}

TEST(GrassPlacementModel, PerCellSlotsAreCappedSoOneCellCannotStallTheDispatch)
{
    const GrassPlacementParams params = MakeParams(10000.0f);
    EXPECT_LE(GrassSlotsWantedForCell(params, 0.0f), static_cast<float32>(kGrassMaxSlotsPerCell));
    EXPECT_LE(GrassSlotsToIterate(GrassSlotsWantedForCell(params, 0.0f)), kGrassMaxSlotsPerCell);
}

// --- Growth -------------------------------------------------------------------------------------

TEST(GrassPlacementModel, TheNewestSlotsScaleInInsteadOfPopping)
{
    constexpr float32 kSlotsWanted = 512.0f;
    // Fully grown well below the top of the range.
    EXPECT_FLOAT_EQ(GrassGrowthForSlot(kSlotsWanted, 0u), 1.0f);
    EXPECT_FLOAT_EQ(GrassGrowthForSlot(kSlotsWanted, 100u), 1.0f);
    // The topmost slot is the one mid-growth, and the slot past the count is absent.
    EXPECT_GT(GrassGrowthForSlot(kSlotsWanted, 511u), 0.0f);
    EXPECT_LT(GrassGrowthForSlot(kSlotsWanted, 511u), 1.0f);
    EXPECT_FLOAT_EQ(GrassGrowthForSlot(kSlotsWanted, 512u), 0.0f);
    EXPECT_FLOAT_EQ(GrassGrowthForSlot(kSlotsWanted, 900u), 0.0f);
}

TEST(GrassPlacementModel, GrowthIsContinuousAsTheCameraApproaches)
{
    const GrassPlacementParams params = MakeParams();
    constexpr uint32 kSlot = 300u;
    // Walk a cell from the far range to the camera; a blade's scale must never jump.
    float32 previous = 0.0f;
    float32 largestStep = 0.0f;
    for (float32 d = 500.0f; d >= 0.0f; d -= 0.25f)
    {
        const float32 growth = GrassGrowthForSlot(GrassSlotsWantedForCell(params, d), kSlot);
        largestStep = std::max(largestStep, std::abs(growth - previous));
        previous = growth;
    }
    // A pop would be a step of order 1. A quarter-metre of camera travel must not change a blade's
    // scale by more than a few percent.
    EXPECT_LT(largestStep, 0.05f);
}

TEST(GrassPlacementModel, EverySlotWithVisibleGrowthIsIterated)
{
    for (float32 wanted : {0.5f, 1.0f, 25.6f, 511.5f, 4096.0f})
    {
        const uint32 iterated = GrassSlotsToIterate(wanted);
        // No slot beyond the iteration bound may still be visible.
        EXPECT_LE(GrassGrowthForSlot(wanted, iterated), kGrassMinGrowth) << "wanted=" << wanted;
    }
}

// --- Halton distribution ------------------------------------------------------------------------

// The pre-registered R2 risk was that a clean per-cell hash reads as a grid. Halton is prefix
// uniform, so ANY slot count fills the cell evenly — this is the property that both removes the
// lattice artefact and keeps blades still while the count changes.
//
// The bounds below are MEASURED worst cases over the swept cells, not chosen thresholds: a cell
// covers its quadrants at every slot count, covers a 4x4 grid from 64 slots, and its deviation
// from a perfectly even fill shrinks as the count rises. A hash that degenerated — aliasing at
// large cell coordinates, or collapsing the rotation — breaks these by a wide margin.
namespace
{

struct CellFillStats
{
    uint32 EmptyBuckets = 0;
    float32 WorstRelativeDeviation = 0.0f;
};

CellFillStats MeasureCellFill(uint32 buckets, uint32 slotCount)
{
    CellFillStats worst{};
    for (int32 cell = 0; cell < 40; ++cell)
    {
        std::vector<uint32> histogram(buckets * buckets, 0u);
        for (uint32 slot = 0; slot < slotCount; ++slot)
        {
            const GrassSlotSample s = GrassSampleSlot(cell, cell * 7 + 3, slot, 3u);
            const uint32 bx = std::min(static_cast<uint32>(s.CellU * buckets), buckets - 1);
            const uint32 by = std::min(static_cast<uint32>(s.CellV * buckets), buckets - 1);
            ++histogram[by * buckets + bx];
        }
        const float32 ideal = static_cast<float32>(slotCount) / (buckets * buckets);
        for (uint32 h : histogram)
        {
            if (h == 0u)
                ++worst.EmptyBuckets;
            worst.WorstRelativeDeviation = std::max(
                worst.WorstRelativeDeviation, std::abs(static_cast<float32>(h) - ideal) / ideal);
        }
    }
    return worst;
}

} // namespace

TEST(GrassPlacementModel, EverySlotPrefixCoversAllFourQuadrantsOfItsCell)
{
    // Even the sparsest cell the far field produces must reach every quadrant; a prefix that
    // clumped into one corner is the visible artefact this sequence exists to prevent.
    for (uint32 count : {16u, 25u, 64u, 256u, 512u})
        EXPECT_EQ(MeasureCellFill(2u, count).EmptyBuckets, 0u) << "count=" << count;
}

TEST(GrassPlacementModel, CellFillEvensOutAsTheSlotCountRises)
{
    const CellFillStats quadrantsSparse = MeasureCellFill(2u, 16u);
    const CellFillStats quadrantsDense = MeasureCellFill(2u, 512u);
    EXPECT_LT(quadrantsSparse.WorstRelativeDeviation, 0.6f);
    EXPECT_LT(quadrantsDense.WorstRelativeDeviation, 0.10f);
    EXPECT_LT(quadrantsDense.WorstRelativeDeviation, quadrantsSparse.WorstRelativeDeviation);

    // A finer grid needs more slots before every bucket is reached — base 3 does not align with a
    // power-of-two grid — but the fill still converges.
    EXPECT_EQ(MeasureCellFill(4u, 64u).EmptyBuckets, 0u);
    EXPECT_LT(MeasureCellFill(4u, 64u).WorstRelativeDeviation, 0.9f);
    EXPECT_LT(MeasureCellFill(4u, 512u).WorstRelativeDeviation, 0.20f);
}

// --- Budget fit ---------------------------------------------------------------------------------

TEST(GrassPlacementModel, AFittingRequestIsLeftAlone)
{
    const GrassPlacementParams params = MakeParams(8.0f, 500.0f, 2.0f);
    const GrassPlacementPlan plan = GrassFitPlacementToBudget(params, 524288u);
    EXPECT_FALSE(plan.RangeReduced);
    EXPECT_FLOAT_EQ(plan.Params.NearDensity, params.NearDensity);
    EXPECT_FLOAT_EQ(plan.Params.FarRadius, params.FarRadius);
}

// The budget is spent near first: blades per m² at the camera is what the author specified and what
// they can see, so over-subscription costs range and never near-field density.
TEST(GrassPlacementModel, OverSubscriptionShortensRangeAndHoldsNearDensity)
{
    const GrassPlacementParams params = MakeParams(50.0f, 500.0f, 2.0f);
    const GrassPlacementPlan plan = GrassFitPlacementToBudget(params, 524288u);
    EXPECT_TRUE(plan.RangeReduced);
    EXPECT_FLOAT_EQ(plan.Params.NearDensity, 50.0f);
    EXPECT_LT(plan.Params.FarRadius, params.FarRadius);
    EXPECT_GT(plan.Params.FarRadius, 0.0f);
    EXPECT_LE(plan.PlannedCandidates, 524288u);
}

TEST(GrassPlacementModel, TheFitNeverCollapsesTheNearField)
{
    const GrassPlacementParams params = MakeParams(64.0f, 2000.0f, 1.0f);
    const GrassPlacementPlan plan = GrassFitPlacementToBudget(params, 4096u);
    EXPECT_GE(plan.Params.FarRadius, 32.0f);
    EXPECT_FLOAT_EQ(plan.Params.NearDensity, 64.0f);
}

// The whole budget is one pool the two LOD ranges fill from opposite ends, so neither range can be
// starved by a share that a different camera pitch would have wanted the other way.
TEST(GrassPlacementModel, TheBudgetIsOnePoolForBothLodRanges)
{
    constexpr uint32 kBudget = 524288u;
    const GrassPlacementPlan plan = GrassFitPlacementToBudget(MakeParams(), kBudget);
    EXPECT_EQ(plan.Capacity, kBudget);
    EXPECT_GT(plan.Lod1StartDistance, 0.0f);
    EXPECT_LT(plan.Lod1StartDistance, plan.Params.FarRadius);
}

TEST(GrassPlacementModel, TheCellWindowCoversTheFittedRange)
{
    const GrassPlacementPlan plan = GrassFitPlacementToBudget(MakeParams(8.0f, 500.0f), 524288u);
    const uint32 halfSpan = (plan.CellSpan - 1u) / 2u;
    EXPECT_GE(static_cast<float32>(halfSpan) * kGrassCellSize, plan.Params.FarRadius);
    EXPECT_EQ(plan.CellCount, plan.CellSpan * plan.CellSpan);
}

TEST(GrassPlacementModel, AZeroBudgetOrZeroDensityPlansNothing)
{
    EXPECT_EQ(GrassFitPlacementToBudget(MakeParams(), 0u).CellCount, 0u);
    EXPECT_EQ(GrassFitPlacementToBudget(MakeParams(0.0f), 524288u).CellCount, 0u);
    EXPECT_EQ(GrassFitPlacementToBudget(MakeParams(8.0f, 0.0f), 524288u).CellCount, 0u);
}

// The scale-invariance claim itself is carried by the GPU oracle
// (RenderPipelineDeclareTests.GrassPlacementIsIndependentOfTerrainSize), which places on two
// terrain sizes and compares. What the model can assert on its own is the property that makes it
// possible: the hash stays well-distributed at world-scale cell coordinates. A float hash chain
// aliases out there, which would show up as a distant cell degenerating toward the origin cell's
// pattern — the reason this uses an integer hash.
TEST(GrassPlacementModel, SlotSamplingStaysWellFormedAtWorldScaleCoordinates)
{
    for (int32 cell : {0, 2000, -2000, 1000000, -1000000})
    {
        const GrassSlotSample s = GrassSampleSlot(cell, cell / 3 - 11, 42u, 3u);
        EXPECT_GE(s.CellU, 0.0f) << "cell=" << cell;
        EXPECT_LT(s.CellU, 1.0f) << "cell=" << cell;
        EXPECT_GE(s.CellV, 0.0f) << "cell=" << cell;
        EXPECT_LT(s.CellV, 1.0f) << "cell=" << cell;
    }
    const GrassSlotSample atOrigin = GrassSampleSlot(0, 0, 42u, 3u);
    const GrassSlotSample farOut = GrassSampleSlot(1000000, -1000000, 42u, 3u);
    EXPECT_NE(atOrigin.CellU, farOut.CellU);
    EXPECT_NE(atOrigin.CellV, farOut.CellV);
}

TEST(GrassPlacementModel, TheAuthoredSeedReachesTheGpuAsTheSameBits)
{
    // The component stores a float and the shader hashes floatBitsToUint of it; this is the CPU
    // mirror of that reinterpretation, so a test can reason about the same seed the GPU uses.
    EXPECT_EQ(GrassSeedBits(3.0f), GrassSeedBits(3.0f));
    EXPECT_NE(GrassSeedBits(3.0f), GrassSeedBits(4.0f));
    EXPECT_NE(GrassSampleSlot(0, 0, 0u, GrassSeedBits(3.0f)).CellU,
              GrassSampleSlot(0, 0, 0u, GrassSeedBits(4.0f)).CellU);
}

TEST(GrassPlacementModel, TheDispatchNeverExceedsTheGuaranteedWorkgroupCount)
{
    // One workgroup per cell against Vulkan's guaranteed 65535 per dimension.
    for (float32 range : {500.0f, 1200.0f, 2000.0f, 16000.0f})
    {
        const GrassPlacementPlan plan =
            GrassFitPlacementToBudget(MakeParams(0.5f, range, 1.0f), 524288u);
        EXPECT_LE(plan.CellCount, 65535u) << "range=" << range;
        EXPECT_LE(plan.CellSpan, kGrassMaxCellHalfSpan * 2u + 1u) << "range=" << range;
    }
}


// --- Clumping ------------------------------------------------------------------------------------

// The clump lattice is the second world lattice, the one that groups blades into tufts. It carries
// the SAME determinism contract as the placement lattice — a pure function of (worldX, worldZ,
// clumpSize, seed) — and grass_clump.glsl is its GPU mirror. The golden values below were derived
// from an independent reimplementation of the hash rather than from this header, and that
// reimplementation was validated by reproducing HashGoldenValues above before any clump number was
// read off it.

namespace
{

float32 ClumpDistance(float32 worldX, float32 worldZ, const GrassClumpSample& c)
{
    const float32 dx = worldX - c.CenterX;
    const float32 dz = worldZ - c.CenterZ;
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

TEST(GrassClumpModel, SameSiteAndSeedProduceBitIdenticalClumps)
{
    for (float32 x : {-4096.5f, -7.25f, 0.0f, 13.75f, 100000.5f})
    {
        for (float32 z : {-311.5f, 0.0f, 42.25f})
        {
            const GrassClumpSample a = GrassSampleClump(x, z, 1.1f, 3u);
            const GrassClumpSample b = GrassSampleClump(x, z, 1.1f, 3u);
            EXPECT_EQ(a.CenterX, b.CenterX);
            EXPECT_EQ(a.CenterZ, b.CenterZ);
            EXPECT_EQ(a.ColorRand, b.ColorRand);
            EXPECT_EQ(a.HeightRand, b.HeightRand);
            EXPECT_EQ(a.FacingRand, b.FacingRand);
            EXPECT_EQ(a.CellX, b.CellX);
            EXPECT_EQ(a.CellZ, b.CellZ);
        }
    }
}

// The control for the test above: identical output for identical input only means something if a
// different seed actually relays the tufts.
TEST(GrassClumpModel, ADifferentSeedRelaysTheTufts)
{
    constexpr uint32 kSamples = 256;
    uint32 moved = 0;
    for (uint32 i = 0; i < kSamples; ++i)
    {
        const float32 x = static_cast<float32>(i) * 0.37f;
        const GrassClumpSample a = GrassSampleClump(x, -x * 0.5f, 1.1f, 3u);
        const GrassClumpSample b = GrassSampleClump(x, -x * 0.5f, 1.1f, 4u);
        if (a.CenterX != b.CenterX || a.CenterZ != b.CenterZ)
            ++moved;
    }
    EXPECT_EQ(moved, kSamples);
}

// The exact numbers grass_clump.glsl must produce. A drift between the two mirrors is a red test
// rather than a subtly differently-clumped field.
TEST(GrassClumpModel, ClumpGoldenValues)
{
    const GrassClumpSample origin = GrassSampleClump(0.0f, 0.0f, 1.1f, 3u);
    EXPECT_EQ(origin.CellX, 0);
    EXPECT_EQ(origin.CellZ, 0);
    EXPECT_FLOAT_EQ(origin.ColorRand, 0.377299845f);
    EXPECT_FLOAT_EQ(origin.HeightRand, 0.655676484f);
    EXPECT_FLOAT_EQ(origin.FacingRand, 0.298224688f);
    EXPECT_FLOAT_EQ(origin.CenterX, 0.0516854376f);
    EXPECT_FLOAT_EQ(origin.CenterZ, 0.0312289894f);

    const GrassClumpSample offset = GrassSampleClump(3.7f, -2.4f, 1.1f, 3u);
    EXPECT_EQ(offset.CellX, 2);
    EXPECT_EQ(offset.CellZ, -3);
    EXPECT_FLOAT_EQ(offset.ColorRand, 0.771366894f);
    EXPECT_FLOAT_EQ(offset.HeightRand, 0.279884219f);
    EXPECT_FLOAT_EQ(offset.FacingRand, 0.344435632f);

    // Far from the origin: the cell-local distance form must not degrade out here.
    const GrassClumpSample farOut = GrassSampleClump(1000.5f, -999.25f, 1.1f, 3u);
    EXPECT_EQ(farOut.CellX, 909);
    EXPECT_EQ(farOut.CellZ, -909);
    EXPECT_FLOAT_EQ(farOut.ColorRand, 0.201008916f);
    EXPECT_FLOAT_EQ(farOut.HeightRand, 0.528636038f);
    EXPECT_FLOAT_EQ(farOut.FacingRand, 0.710813403f);

    const GrassClumpSample reseeded = GrassSampleClump(0.0f, 0.0f, 1.1f, 4u);
    EXPECT_EQ(reseeded.CellX, 0);
    EXPECT_EQ(reseeded.CellZ, -1);
    EXPECT_FLOAT_EQ(reseeded.ColorRand, 0.291421115f);
}

// The property the whole slice exists for: blades standing next to each other mostly belong to the
// same tuft, and blades a few clumps apart mostly do not. Both halves are needed — a function that
// returned a constant would pass the first half on its own.
TEST(GrassClumpModel, NeighbouringBladesShareATuftAndDistantOnesDoNot)
{
    constexpr float32 kClumpSize = 1.1f;
    constexpr uint32 kSamples = 2000;
    uint32 nearShared = 0;
    uint32 farShared = 0;
    for (uint32 i = 0; i < kSamples; ++i)
    {
        // A deterministic sweep rather than a PRNG, so a failure is reproducible from the index.
        const float32 x = -50.0f + static_cast<float32>(i) * 0.05f;
        const float32 z = 37.0f - static_cast<float32>(i) * 0.031f;
        const GrassClumpSample a = GrassSampleClump(x, z, kClumpSize, 3u);
        const GrassClumpSample nearBlade = GrassSampleClump(x + 0.1f, z, kClumpSize, 3u);
        const GrassClumpSample farBlade = GrassSampleClump(x + 4.0f * kClumpSize, z, kClumpSize, 3u);
        if (a.CellX == nearBlade.CellX && a.CellZ == nearBlade.CellZ)
            ++nearShared;
        if (a.CellX == farBlade.CellX && a.CellZ == farBlade.CellZ)
            ++farShared;
    }
    // 10 cm apart at a 1.1 m clump: such a pair straddles a boundary only near one, so the large
    // majority share. Measured 89 % on the shipped hash; the bound is loose enough to survive the
    // sample choice and tight enough that a per-blade (unclumped) assignment fails it outright.
    EXPECT_GT(nearShared, kSamples * 3u / 4u);
    // Four clumps apart, sharing is the coincidence it should be.
    EXPECT_LT(farShared, kSamples / 20u);
}

// Nearest-of-nine always has the centre cell's own site among its candidates, and that site is at
// most one cell away on each axis. A blade therefore can never be adopted by a tuft further off
// than the cell diagonal — the bound that makes "clump size" mean something in metres.
TEST(GrassClumpModel, TheAdoptingTuftIsNeverFurtherThanTheCellDiagonal)
{
    for (float32 clumpSize : {0.4f, 1.1f, 3.0f})
    {
        const float32 bound = clumpSize * 1.41421356f;
        for (uint32 i = 0; i < 500; ++i)
        {
            const float32 x = -120.0f + static_cast<float32>(i) * 0.47f;
            const float32 z = 61.0f - static_cast<float32>(i) * 0.29f;
            const GrassClumpSample c = GrassSampleClump(x, z, clumpSize, 3u);
            EXPECT_LE(ClumpDistance(x, z, c), bound * 1.0001f)
                << "clumpSize=" << clumpSize << " i=" << i;
        }
    }
}

TEST(GrassClumpModel, ClumpAssignmentStaysWellFormedAtWorldScaleCoordinates)
{
    for (float32 world : {0.0f, 2000.0f, -2000.0f, 100000.0f, -100000.0f})
    {
        const float32 z = world * 0.5f - 11.0f;
        const GrassClumpSample c = GrassSampleClump(world, z, 1.1f, 3u);
        EXPECT_GE(c.ColorRand, 0.0f) << "world=" << world;
        EXPECT_LT(c.ColorRand, 1.0f) << "world=" << world;
        EXPECT_GE(c.HeightRand, 0.0f) << "world=" << world;
        EXPECT_LT(c.HeightRand, 1.0f) << "world=" << world;
        EXPECT_GE(c.FacingRand, 0.0f) << "world=" << world;
        EXPECT_LT(c.FacingRand, 1.0f) << "world=" << world;
        // The winning site really is the one the cell index claims, at every scale.
        EXPECT_LE(ClumpDistance(world, z, c), 1.1f * 1.4143f) << "world=" << world;
    }
}

// Clump size is a metre quantity, not a shape knob: halving it must roughly quadruple how many
// tufts a fixed patch of ground holds.
TEST(GrassClumpModel, ClumpSizeSetsHowManyTuftsAPatchHolds)
{
    auto tuftsInPatch = [](float32 clumpSize) {
        std::set<std::pair<int32, int32>> tufts;
        for (uint32 i = 0; i < 200u; ++i)
        {
            for (uint32 j = 0; j < 200u; ++j)
            {
                const GrassClumpSample c = GrassSampleClump(
                    static_cast<float32>(i) * 0.1f, static_cast<float32>(j) * 0.1f, clumpSize, 3u);
                tufts.insert({c.CellX, c.CellZ});
            }
        }
        return tufts.size();
    };
    // A 20 m x 20 m patch: the tuft count tracks (20 / clumpSize)^2 to within the boundary cells
    // the window clips. Measured 373 at 1.1 m against the 368 lattice cells the patch spans.
    const size_t coarse = tuftsInPatch(2.2f);
    const size_t fine = tuftsInPatch(1.1f);
    EXPECT_GT(fine, coarse * 3u);
    EXPECT_LT(fine, coarse * 5u);
    EXPECT_GT(coarse, 60u);
    EXPECT_LT(coarse, 130u);
}

// Zero variance is EXACTLY the unclumped field, not merely close to it. "Inert at its off setting"
// has to mean bit-identical, or an author who dials a knob back to zero still gets a different
// field than the one they had before they touched it.
TEST(GrassClumpModel, ZeroVarianceIsExactlyTheAuthoredHeight)
{
    for (float32 heightRand : {0.0f, 0.25f, 0.5f, 0.9999f})
    {
        EXPECT_EQ(GrassClumpHeightMultiplier(heightRand, 0.0f), 1.0f);
        for (float32 height : {0.24f, 0.72f, 3.1415f})
            EXPECT_EQ(height * GrassClumpHeightMultiplier(heightRand, 0.0f), height);
    }
    // A live variance is symmetric about the authored height.
    EXPECT_FLOAT_EQ(GrassClumpHeightMultiplier(0.5f, 0.3f), 1.0f);
    EXPECT_FLOAT_EQ(GrassClumpHeightMultiplier(0.0f, 0.3f), 0.7f);
    EXPECT_FLOAT_EQ(GrassClumpHeightMultiplier(1.0f, 0.3f), 1.3f);
    // An out-of-range variance cannot invert a blade.
    EXPECT_GE(GrassClumpHeightMultiplier(0.0f, 5.0f), 0.0f);
}

// The clump code and the minimum-width scale ride in the instance's Flags word beside the LOD
// index, which is the whole reason both cost zero bytes per blade. No field may corrupt another.
TEST(GrassClumpModel, TheFlagsWordCarriesLodClumpCodeAndMinWidthIndependently)
{
    for (uint32 lod = 0; lod < 4u; ++lod)
    {
        for (uint32 code = 0; code <= 255u; ++code)
        {
            for (float32 scale : {0.0f, 0.5f, 1.0f, 3.25f, kGrassMaxWidthExpansion})
            {
                const uint32 flags = GrassPackFlags(lod, code, scale);
                EXPECT_EQ(GrassLodFromFlags(flags), lod) << "code=" << code << " scale=" << scale;
                EXPECT_EQ(GrassClumpCodeFromFlags(flags), code) << "lod=" << lod
                                                                << " scale=" << scale;
                // One quantization step over the encoded range, which is what the 16 bits buy.
                EXPECT_NEAR(GrassMinWidthScaleFromFlags(flags), scale,
                            kGrassMaxWidthExpansion / static_cast<float32>(kGrassMinWidthScaleMask))
                    << "lod=" << lod << " code=" << code;
            }
        }
    }
    // Out-of-range input is masked or clamped, never allowed to bleed into a neighbouring field.
    EXPECT_EQ(GrassLodFromFlags(GrassPackFlags(0u, 0xFFFFFFFFu, 0.0f)), 0u);
    EXPECT_EQ(GrassClumpCodeFromFlags(GrassPackFlags(0xFFFFFFFFu, 0u, 0.0f)), 0u);
    EXPECT_EQ(GrassLodFromFlags(GrassPackFlags(0u, 0u, 1.0e30f)), 0u);
    EXPECT_EQ(GrassClumpCodeFromFlags(GrassPackFlags(0u, 0u, 1.0e30f)), 0u);
    EXPECT_FLOAT_EQ(GrassMinWidthScaleFromFlags(GrassPackFlags(0u, 0u, 1.0e30f)),
                    kGrassMaxWidthExpansion);
    EXPECT_FLOAT_EQ(GrassMinWidthScaleFromFlags(GrassPackFlags(0u, 0u, -5.0f)), 0.0f);
    // Bits 26-31 are still unused, so nothing the packer writes may reach them.
    EXPECT_EQ(GrassPackFlags(3u, 255u, kGrassMaxWidthExpansion) >> 26u, 0u);
}

// The substitution the S3b bench rests on: main's vertex modifier reads Flags only through the LOD
// and clump-code accessors, so a placement compute that also writes the min-width field must be
// invisible to it. If either accessor could see those bits, running one binary's compute under the
// other binary's material would not be a controlled arm.
TEST(GrassClumpModel, TheLodAndClumpAccessorsAreBlindToTheMinWidthField)
{
    for (uint32 lod = 0; lod < 4u; ++lod)
    {
        for (uint32 code : {0u, 1u, 127u, 254u, 255u})
        {
            const uint32 without = GrassPackFlags(lod, code, 0.0f);
            for (float32 scale : {0.25f, 1.0f, 2.5f, kGrassMaxWidthExpansion})
            {
                const uint32 with = GrassPackFlags(lod, code, scale);
                EXPECT_EQ(GrassLodFromFlags(with), GrassLodFromFlags(without));
                EXPECT_EQ(GrassClumpCodeFromFlags(with), GrassClumpCodeFromFlags(without));
            }
        }
    }
}

// The floor is inert unless it exceeds the width it is clamped against, and inertness must be
// EXACT rather than close: clamp returns its `width` operand itself, so a blade that already
// clears the floor renders bit-identically to a build without the feature. Mirrors the vertex
// stage's `clamp(minWidth, width, width * kGrassMaxWidthExpansion)`.
TEST(GrassClumpModel, AMinWidthBelowTheBladesOwnWidthIsBitIdenticallyInert)
{
    auto clampWidth = [](float32 minWidth, float32 width) {
        return std::min(std::max(minWidth, width), width * kGrassMaxWidthExpansion);
    };
    for (float32 width : {0.0f, 1.0e-6f, 0.0123f, 0.043f, 0.05f, 1.7f})
    {
        // No floor at all: the encoded 0 must not perturb a single bit.
        const float32 noFloor = 0.05f * GrassMinWidthScaleFromFlags(GrassPackFlags(0u, 0u, 0.0f));
        EXPECT_EQ(clampWidth(noFloor, width), width) << "width=" << width;
        // A floor strictly under the width is equally inert.
        EXPECT_EQ(clampWidth(width * 0.5f, width), width) << "width=" << width;
    }
    // And where it does bind, it binds to the floor, not past the ceiling.
    EXPECT_FLOAT_EQ(clampWidth(0.02f, 0.01f), 0.02f);
    EXPECT_FLOAT_EQ(clampWidth(1.0f, 0.01f), 0.01f * kGrassMaxWidthExpansion);
}

TEST(GrassClumpModel, TheClumpCodeSpansItsEightBitsAcrossTheField)
{
    std::set<uint32> codes;
    for (uint32 i = 0; i < 4000u; ++i)
    {
        const GrassClumpSample c = GrassSampleClump(static_cast<float32>(i) * 1.7f,
                                                    static_cast<float32>(i) * -0.9f, 1.1f, 3u);
        codes.insert(GrassClumpCodeFromRand(c.ColorRand));
        EXPECT_LE(GrassClumpCodeFromRand(c.ColorRand), 255u);
    }
    // A quantizer that collapsed, or a hash that banded, would show up as a handful of codes.
    EXPECT_GT(codes.size(), 200u);
    // The round trip stays inside one quantization step.
    for (uint32 code : {0u, 1u, 127u, 254u, 255u})
        EXPECT_NEAR(static_cast<float32>(GrassClumpCodeFromRand(GrassClumpRandFromCode(code))),
                    static_cast<float32>(code), 1.0f);
}


// --- Site purity of the shader seed paths --------------------------------------------------------

// gl_InstanceIndex is the compacted slot a blade landed in, and the placement compute reallocates
// those slots with a subgroup atomicAdd every frame — workgroup completion order decides who gets
// which. A blade's slot is therefore NOT a stable identity: anything that seeds an attribute from
// it re-rolls every frame and reads as field-wide jitter on a perfectly static camera. That defect
// shipped once (the blade-lean jitter, fixed on probe/grass-churn) and clumping adds a second,
// larger family of per-blade attributes with exactly the same temptation.
//
// So this is a source sweep, not a value test: every seed path in the grass shaders must draw from
// the blade's SITE. It reads the shipped GLSL and fails on the pattern rather than waiting for a
// human to notice a shimmer. Each assertion carries a positive control — a scan that matched
// nothing would otherwise pass green, which is the failure mode this class of test is prone to.

#if defined(TERRAIN_GRASS_SHADER_SOURCE_DIR)

namespace
{

// COMMENTS ARE STRIPPED before any scan below. The rule is about what the shader COMPUTES, and a
// test that also matched prose would forbid the files from documenting the very rule they keep —
// which is exactly what this test did on its first run, firing on grass_clump.glsl's own header.
std::string StripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();)
    {
        if (src.compare(i, 2, "//") == 0)
        {
            while (i < src.size() && src[i] != '\n')
                ++i;
        }
        else if (src.compare(i, 2, "/*") == 0)
        {
            i += 2;
            while (i + 1 < src.size() && src.compare(i, 2, "*/") != 0)
                ++i;
            i = std::min(i + 2, src.size());
        }
        else
        {
            out.push_back(src[i]);
            ++i;
        }
    }
    return out;
}

std::string ReadShaderSourceRaw(const std::string& name)
{
    const std::string path = std::string(TERRAIN_GRASS_SHADER_SOURCE_DIR) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "could not open " << path;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string ReadShaderSource(const std::string& name)
{
    return StripComments(ReadShaderSourceRaw(name));
}

// The argument text of every call to `callee(`, brace-matched so nested calls come back whole.
std::vector<std::string> CallArguments(const std::string& src, const std::string& callee)
{
    std::vector<std::string> args;
    const std::string needle = callee + "(";
    size_t at = 0;
    while ((at = src.find(needle, at)) != std::string::npos)
    {
        // Skip the declaration/definition site and any identifier that merely ENDS with the name.
        const bool boundedLeft = at == 0 || (!std::isalnum(static_cast<unsigned char>(src[at - 1])) &&
                                             src[at - 1] != '_');
        size_t i = at + needle.size();
        int depth = 1;
        const size_t start = i;
        for (; i < src.size() && depth > 0; ++i)
        {
            if (src[i] == '(')
                ++depth;
            else if (src[i] == ')')
                --depth;
        }
        if (boundedLeft && depth == 0)
            args.push_back(src.substr(start, i - 1 - start));
        at = at + needle.size();
    }
    return args;
}

bool MentionsInstanceSlot(const std::string& text)
{
    return text.find("gl_InstanceIndex") != std::string::npos ||
           text.find("instanceIdx") != std::string::npos;
}

} // namespace

// The two files this slice adds or extends are pure site functions end to end: the instance slot
// must not appear in them at all.
TEST(GrassShaderSitePurity, TheClumpAndPlacementMirrorsNeverSeeTheInstanceSlot)
{
    for (const char* name : {"grass_clump.glsl", "grass_placement.glsl"})
    {
        const std::string src = ReadShaderSource(name);
        // Positive control: prove we read the file we think we did, and that the comment stripper
        // left the code behind, before trusting the file's silence about the instance slot.
        ASSERT_GT(src.size(), 200u) << name << " read back empty, truncated, or stripped to nothing";
        EXPECT_NE(src.find("GrassHashSite"), std::string::npos)
            << name << " does not look like the placement/clump mirror";
        EXPECT_EQ(src.find("gl_InstanceIndex"), std::string::npos)
            << name << " seeds from the instance slot, which is reallocated every frame";
    }
    // The clump mirror specifically must expose the sampler this suite pins.
    EXPECT_NE(ReadShaderSource("grass_clump.glsl").find("GrassSampleClump"), std::string::npos);
}

// The placement compute has no such builtin, and must not grow one: every blade attribute it
// writes has to be derivable before the blade is allocated a slot at all.
TEST(GrassShaderSitePurity, ThePlacementComputeNeverSeedsFromAnInstanceSlot)
{
    const std::string src = ReadShaderSource("terrain_grass_place.comp");
    ASSERT_NE(src.find("GrassSampleClump"), std::string::npos)
        << "positive control failed: the compute no longer samples the clump lattice";
    EXPECT_EQ(src.find("gl_InstanceIndex"), std::string::npos);
}

// The vertex modifier legitimately uses gl_InstanceIndex to SUBSCRIPT the instance buffer — that is
// what it is for. What it must never do is feed it to a hash: the subscript reads a blade's data,
// a hash of the subscript invents data that changes when the slot changes.
TEST(GrassShaderSitePurity, NoHashInTheVertexModifierIsSeededFromTheInstanceSlot)
{
    const std::string src = ReadShaderSource("terrain_grass_vertex_modifier.glsl");

    const std::vector<std::string> hashArgs = CallArguments(src, "hash11");
    const std::vector<std::string> noiseArgs = CallArguments(src, "snoise");
    // Positive controls: a scanner that matched nothing would pass every assertion below.
    ASSERT_FALSE(hashArgs.empty()) << "found no hash11 call sites - the scan is not working";
    ASSERT_FALSE(noiseArgs.empty()) << "found no snoise call sites - the scan is not working";

    for (const std::string& arg : hashArgs)
        EXPECT_FALSE(MentionsInstanceSlot(arg))
            << "hash11(" << arg << ") is seeded from the instance slot, which is reallocated by a "
               "subgroup atomicAdd every frame";
    for (const std::string& arg : noiseArgs)
        EXPECT_FALSE(MentionsInstanceSlot(arg))
            << "snoise(" << arg << ") is seeded from the instance slot";

    // The control that keeps the scan honest in the other direction: the file really does use the
    // instance slot somewhere (to subscript the buffer), so "no mentions in hash arguments" is a
    // statement about WHERE it is used, not about the file being empty of it.
    EXPECT_NE(src.find("gl_InstanceIndex"), std::string::npos)
        << "the vertex modifier no longer reads the instance buffer at all - this test's premise "
           "has changed and its scan needs revisiting";
}

// Wind phase must come from the ENGINE animation clock and from nothing else, and it must reach the
// modifier as its endpoint argument (inst.deformationTimeSeconds, stamped by the vertex adapter from
// Light.uTimeParams.x) rather than through the per-view global: a global cannot be evaluated at a
// second endpoint, which is what motion vectors for deformed geometry need. That clock's per-frame
// advance is capped (Time.cpp kMaxAnimationDeltaSeconds), which is what bounds the phase a stalled
// frame loop can render in a single resume frame; a second, wall-derived clock summed alongside it
// is unreachable by the cap and also double-rates every authored speed. The wall clock this pins out
// arrived as a `uTimeSeconds` field in a grass-owned uniform block.
TEST(GrassWindClock, WindTimeComesOnlyFromTheEngineAnimationClock)
{
    const std::string modifier = ReadShaderSource("terrain_grass_vertex_modifier.glsl");
    // Positive controls: prove the file was read, survived the comment strip, and still contains
    // the wind path this test is about, before trusting any file's silence below.
    ASSERT_GT(modifier.size(), 200u) << "the vertex modifier read back empty or stripped to nothing";
    ASSERT_NE(modifier.find("sampleWindField"), std::string::npos)
        << "the vertex modifier no longer samples a wind field - this test's premise has changed";
    EXPECT_NE(modifier.find("inst.deformationTimeSeconds"), std::string::npos)
        << "the vertex modifier reads no endpoint deformation clock at all";
    EXPECT_EQ(modifier.find("Light.uTimeParams"), std::string::npos)
        << "the vertex modifier reads the per-view global clock, which cannot be evaluated at a "
           "second endpoint";

    for (const char* name : {"terrain_grass_vertex_modifier.glsl", "terrain_grass_surface.glsl",
                             "terrain_grass_place.comp", "grass_placement.glsl", "grass_clump.glsl"})
    {
        const std::string src = ReadShaderSource(name);
        ASSERT_GT(src.size(), 200u) << name << " read back empty, truncated, or stripped to nothing";
        EXPECT_EQ(src.find("uTimeSeconds"), std::string::npos)
            << name << " carries a second, wall-derived clock alongside Light.uTimeParams: summing "
                       "them defeats the animation clock's hitch cap and doubles the wind rate";
    }
}

// The ground splat must be resolved in ONE place. GrassAtlasSplatSource tests what that place
// DECIDES; this tests that both stages actually go through it, which no behavioural test of a
// shared function can see — a shader that quietly kept its own tap would leave those tests green
// while the blade and its ground disagreed again.
//
// That is not hypothetical: it is the state this slice found the surface in. It sampled
// tp.SplatmapBindless directly, which an atlas-backed terrain leaves at 0, so every blade on a
// >4 km terrain took its ground colour from channel 0 whatever was painted underneath.
TEST(GrassSplatResolveSharing, BothStagesResolveTheGroundSplatThroughTheSharedInclude)
{
    for (const char* name : {"terrain_grass_place.comp", "terrain_grass_surface.glsl"})
    {
        const std::string src = ReadShaderSource(name);
        ASSERT_GT(src.size(), 200u) << name << " read back empty, truncated, or stripped to nothing";

        EXPECT_NE(src.find("TerrainGrass/grass_atlas_splat.glsl"), std::string::npos)
            << name << " does not include the shared ground splat resolve";
        EXPECT_NE(src.find("GrassAtlas_ResolveSplat"), std::string::npos)
            << name << " includes the shared resolve but never calls it";

        // The declarations the shared include owns must exist in exactly one file, so neither
        // stage can drift a second copy of the bindings or the selection back into itself.
        EXPECT_EQ(src.find("uniform GrassAtlasParamsBuffer"), std::string::npos)
            << name << " re-declares the atlas params block the shared include owns";
        EXPECT_EQ(src.find("buffer GrassAtlasRowsBuffer"), std::string::npos)
            << name << " re-declares the atlas rows block the shared include owns";
    }

    // The surface must reach the splat ONLY through the resolve. A direct tap on
    // tp.SplatmapBindless is the exact regression this slice removed, and it is silent: an atlas
    // terrain carries 0 there, so the shader compiles, runs, and answers channel 0 everywhere.
    const std::string surface = ReadShaderSource("terrain_grass_surface.glsl");
    for (const std::string& arg : CallArguments(surface, "GE_BTEX"))
        EXPECT_EQ(arg.find("SplatmapBindless"), std::string::npos)
            << "terrain_grass_surface.glsl samples the splatmap directly (GE_BTEX(" << arg
            << ")) instead of through GrassAtlas_ResolveSplat, which is blind to the atlas";

    // Positive control: the scan above is only meaningful if the surface samples SOMETHING through
    // GE_BTEX at all — a broken reader would otherwise report an empty list and pass.
    ASSERT_FALSE(CallArguments(surface, "GE_BTEX").empty())
        << "found no GE_BTEX call sites in the grass surface - the scan is not working";

    // The argument scan is defeatable by an alias (uint sm = tp.SplatmapBindless; GE_BTEX(sm, ...)),
    // so pin the identifier's total occurrence count in the stripped source: the struct field
    // declaration plus the one argument handed to GrassAtlas_ResolveSplat. Any third mention is a
    // new read of the handle, and every new read belongs inside the shared resolve.
    size_t splatmapMentions = 0;
    for (size_t at = surface.find("SplatmapBindless"); at != std::string::npos;
         at = surface.find("SplatmapBindless", at + 1))
        ++splatmapMentions;
    EXPECT_EQ(splatmapMentions, 2u)
        << "terrain_grass_surface.glsl mentions SplatmapBindless " << splatmapMentions
        << " times; only the struct field and the GrassAtlas_ResolveSplat argument are allowed - "
           "an extra mention is a direct or aliased tap the atlas path cannot see";

    // The extraction markers are COMMENTS, so this reads the raw file: the stripped copy the scans
    // above use would never contain them, and asserting on it would pass only by accident.
    const std::string sharedRaw = ReadShaderSourceRaw("grass_atlas_splat.glsl");
    ASSERT_GT(sharedRaw.size(), 200u) << "the shared resolve read back empty or truncated";
    for (const char* marker : {"// GE_SHARED_GRASS_SPLAT_SOURCE_BEGIN",
                               "// GE_SHARED_GRASS_SPLAT_SOURCE_END"})
        EXPECT_NE(sharedRaw.find(marker), std::string::npos)
            << "the extraction marker " << marker << " GrassAtlasSplatSourceExtracted.h is "
               "generated from is gone; the behavioural suite would be built from nothing";
    // The selection the extracted block must actually contain, in the code rather than in prose.
    const std::string shared = ReadShaderSource("grass_atlas_splat.glsl");
    EXPECT_NE(shared.find("GrassAtlas_SelectSplatSource"), std::string::npos)
        << "the shared include no longer defines the selection the host suite executes";
    EXPECT_NE(shared.find("GrassAtlas_SlotUpgradeWeight"), std::string::npos)
        << "the shared include no longer defines the upgrade-crossfade rule the host suite executes";
}

// The two stages share the resolve and must DISAGREE about one thing inside it: whether a tile
// still crossfading from the coarse field into its slot is shown crossfading. Colour shows it, so a
// blade's ground colour arrives at full detail with the ground beside it; a placement mask does
// not, because a refusal that slid between two sources across the window would spawn and unspawn
// blades along the residency frontier.
//
// Scanned rather than measured, because GrassAtlasSlotUpgrade already executes the WEIGHT rule on
// the host and cannot see which answer each stage asks for. Swapping these two literals compiles,
// runs, and is invisible in every settled still — the transient is bounded by the fade window.
TEST(GrassSplatResolveSharing, ColourShowsTheUpgradeCrossfadeAndPlacementDoesNot)
{
    struct Consumer
    {
        const char* File;
        const char* ShowsFade;
    };
    for (const Consumer consumer : {Consumer{"terrain_grass_surface.glsl", "true"},
                                    Consumer{"terrain_grass_place.comp", "false"}})
    {
        const std::string src = ReadShaderSource(consumer.File);
        ASSERT_GT(src.size(), 200u) << consumer.File << " read back empty or stripped to nothing";

        const std::vector<std::string> calls = CallArguments(src, "GrassAtlas_ResolveSplat");
        ASSERT_EQ(calls.size(), 1u)
            << consumer.File << " calls GrassAtlas_ResolveSplat " << calls.size()
            << " times; one call site per stage is what makes its fade answer scannable here";

        // The last argument, however the earlier ones are spelled. A literal is required rather
        // than a named constant: the two sites differ on purpose, and a shared name for the
        // difference is a place for them to quietly become the same.
        const size_t comma = calls[0].rfind(',');
        ASSERT_NE(comma, std::string::npos)
            << consumer.File << " passes no crossfade argument: " << calls[0];
        std::string last = calls[0].substr(comma + 1);
        last.erase(0, last.find_first_not_of(" \t\r\n"));
        if (const size_t end = last.find_last_not_of(" \t\r\n"); end != std::string::npos)
            last.erase(end + 1);
        EXPECT_EQ(last, consumer.ShowsFade)
            << consumer.File << " asks for showUpgradeFade=" << last << ", expected "
            << consumer.ShowsFade << " — colour must track the ground's crossfade and a placement "
               "mask must not";
    }
}

// The surface's ground colour reaches the atlas TWICE: the splat weights through the shared resolve
// above, and the terrain normal that supplies the triplanar projection weights the shared albedo
// expression blends its taps by. cbt_surface.glsl crossfades BOTH on upgrade, so fading only the
// splat would still step the blend the moment a tile went resident.
TEST(GrassSplatResolveSharing, TheSurfacesGroundNormalTakesTheSameUpgradeWeight)
{
    const std::string surface = ReadShaderSource("terrain_grass_surface.glsl");
    ASSERT_GT(surface.size(), 200u) << "the grass surface read back empty or stripped to nothing";

    const std::vector<std::string> calls = CallArguments(surface, "GrassAtlas_SlotUpgradeWeight");
    ASSERT_EQ(calls.size(), 1u)
        << "terrain_grass_surface.glsl calls GrassAtlas_SlotUpgradeWeight " << calls.size()
        << " times; the ground normal is the one site that needs it directly (the splat reaches it "
           "through GrassAtlas_ResolveSplat)";
    // The weight must come from the row THIS resolve read — a second clock, or a constant, would
    // put the blade's projection weights on a different schedule from the ground's.
    EXPECT_NE(calls[0].find("r.Fade"), std::string::npos)
        << "the ground normal's upgrade weight is not derived from the resolved row's Fade: "
        << calls[0];
}

// Deriving the weight is not applying it: deleting just the mix() while keeping the weight call
// leaves every scan above green. Pin the APPLICATION — each consumer must hand its slotWeight to
// a mix() — so the crossfade cannot be severed from its weight without a test going red.
TEST(GrassSplatResolveSharing, TheUpgradeWeightIsActuallyMixedNotJustDerived)
{
    struct MixSite
    {
        const char* File;
        size_t ExpectedMixes;
    };
    for (const MixSite& site : {MixSite{"grass_atlas_splat.glsl", 1u},
                                MixSite{"terrain_grass_surface.glsl", 1u}})
    {
        const std::string src = ReadShaderSource(site.File);
        ASSERT_GT(src.size(), 200u) << site.File << " read back empty or stripped to nothing";

        size_t mixesTakingSlotWeight = 0;
        for (const std::string& arg : CallArguments(src, "mix"))
            if (arg.find("slotWeight") != std::string::npos) ++mixesTakingSlotWeight;
        EXPECT_EQ(mixesTakingSlotWeight, site.ExpectedMixes)
            << site.File << " mixes by slotWeight " << mixesTakingSlotWeight
            << " times, expected " << site.ExpectedMixes
            << " - a derived-but-unapplied weight reintroduces the coarse/slot step this "
               "crossfade exists to kill";
    }
}

// A blade's grounded end is the terrain's own resolved colour, UNSCALED. Any factor on it is a
// value step at the contact line, and a blade that reads darker than the ground it grows out of is
// the wrong way round — the base is supposed to disappear into its surroundings. The contact is
// structural rather than authored, so nothing outside this expression can express that factor, and
// the expression is therefore what holds the invariant.
TEST(GrassContactIdentity, TheGroundedEndTakesTheTerrainColourUnscaled)
{
    const std::string surface = ReadShaderSource("terrain_grass_surface.glsl");
    ASSERT_GT(surface.size(), 200u) << "the grass surface read back empty or stripped to nothing";

    // The one mix that spans the blade's colour schedule: grounded end to canopy end, weighted by
    // the schedule itself.
    std::vector<std::string> scheduleMixes;
    for (const std::string& arg : CallArguments(surface, "mix"))
        if (arg.find("terrainColor") != std::string::npos &&
            arg.find("bladeRise") != std::string::npos)
            scheduleMixes.push_back(arg);
    ASSERT_EQ(scheduleMixes.size(), 1u)
        << "expected exactly one terrainColor-to-canopy schedule mix in the grass surface, found "
        << scheduleMixes.size() << " — re-point this scan before trusting it";

    std::string grounded = scheduleMixes[0].substr(0, scheduleMixes[0].find(','));
    grounded.erase(std::remove_if(grounded.begin(), grounded.end(),
                                  [](unsigned char c) { return std::isspace(c) != 0; }),
                   grounded.end());
    EXPECT_EQ(grounded, "terrainColor")
        << "the grounded end of the blade's colour schedule is `" << grounded
        << "`, not the terrain colour itself — a scaled contact reads as a colour mismatch at the "
           "root rather than as the blade being seated in its ground";
}

// The plan step WRITES the indirect block the placement then adds to, each through its own copy
// of the layout, and the CPU reads the same block back. A field added to one copy without the
// others would put LOD 1's first instance in the wrong word, so the three are pinned against each
// other here rather than discovered as duplicated blades.
TEST(GrassIndirectBlockMirror, ThePlanStepWritesTheBlockThePlacementReads)
{
    // One span of a shader with comments and whitespace stripped, from `from` to the first `to`
    // after it. Used for the draw-record struct and for the block separately: the record layout
    // is part of what the plan writes, so a reordered field there is as wrong as one in the
    // counters, but the two copies sit in different surroundings.
    auto spanOf = [](const std::string& source, const char* from, const char* to) {
        const size_t begin = source.find(from);
        const size_t end = begin == std::string::npos ? begin : source.find(to, begin);
        if (begin == std::string::npos || end == std::string::npos)
            return std::string{};
        std::string out;
        std::istringstream lines(source.substr(begin, end - begin));
        for (std::string line; std::getline(lines, line);)
        {
            const size_t comment = line.find("//");
            if (comment != std::string::npos)
                line.erase(comment);
            line.erase(std::remove_if(line.begin(), line.end(),
                                      [](unsigned char c) { return std::isspace(c) != 0; }),
                       line.end());
            out += line;
        }
        return out;
    };
    const std::string placementSource = ReadShaderSource("terrain_grass_place.comp");
    const std::string planSource = ReadShaderSource("terrain_grass_plan.comp");
    const std::string placementRecord = spanOf(placementSource, "struct DrawIndexedIndirectCommand", "};");
    const std::string planRecord = spanOf(planSource, "struct DrawIndexedIndirectCommand", "};");
    ASSERT_FALSE(placementRecord.empty()) << "the placement compute lost its DrawIndexedIndirectCommand";
    ASSERT_FALSE(planRecord.empty()) << "the plan compute lost its DrawIndexedIndirectCommand";
    EXPECT_EQ(placementRecord, planRecord);
    const std::string placement = spanOf(placementSource, "buffer TerrainGrassIndirectArgs", "} Args;");
    const std::string plan = spanOf(planSource, "buffer TerrainGrassIndirectArgs", "} Args;");
    ASSERT_FALSE(placement.empty()) << "the placement compute lost its TerrainGrassIndirectArgs block";
    ASSERT_FALSE(plan.empty()) << "the plan compute lost its TerrainGrassIndirectArgs block";
    EXPECT_EQ(placement, plan);

    // The classify kernel writes no counter and no draw record, so it declares no copy of that
    // block at all — a copy it never reads would be a layout to keep in step for nothing.
    const std::string classifySource = ReadShaderSource("terrain_grass_classify.comp");
    EXPECT_TRUE(spanOf(classifySource, "buffer TerrainGrassIndirectArgs", "} Args;").empty())
        << "the classify compute declares the indirect args block it does not use";

    // All three kernels bind the CELL PLAN at the same binding, and it carries a ring histogram
    // ahead of the cell array. A kernel that declares the array without the header still compiles
    // and still reads plausible numbers — it just reads them from the wrong cell, one header's
    // length away. The counters come from the kernel that declares the header, so they read
    // healthy while the field is empty: only the picture shows that failure.
    const std::string placeCells = spanOf(placementSource, "buffer TerrainGrassCellPlanBuffer", "} CellPlan;");
    const std::string planCells = spanOf(planSource, "buffer TerrainGrassCellPlanBuffer", "} CellPlan;");
    const std::string classifyCells = spanOf(classifySource, "buffer TerrainGrassCellPlanBuffer", "} CellPlan;");
    ASSERT_FALSE(placeCells.empty()) << "the placement compute lost its cell plan block";
    ASSERT_FALSE(planCells.empty()) << "the plan compute lost its cell plan block";
    ASSERT_FALSE(classifyCells.empty()) << "the classify compute lost its cell plan block";
    EXPECT_EQ(placeCells, planCells);
    EXPECT_EQ(placeCells, classifyCells);

    // The counters after the draw records are plain uints in both copies; their count is the
    // C++ mirror's tail, word for word.
    size_t counterWords = 0;
    for (size_t at = placement.find("uint"); at != std::string::npos; at = placement.find("uint", at + 4))
        ++counterWords;
    EXPECT_EQ(counterWords,
              (sizeof(GrassIndirectBlockGPU) - kGrassLodCount * sizeof(GrassIndirectDrawGPU)) / sizeof(uint32));
}

#endif // TERRAIN_GRASS_SHADER_SOURCE_DIR


// The vertex stage hands the surface the terrain row and the clump code in ONE float, because
// custom0 is the only channel the surface contract carries and grass already spends the other
// three. That float is a reinterpreted integer, and a reinterpreted small integer is a DENORMAL —
// precisely the class of value a driver is permitted to flush to zero. A flushed payload does not
// look like a failure: it reads back as terrain row 0 and clump 0.
TEST(GrassClumpModel, TheCustomVaryingPayloadIsAlwaysANormalFloat)
{
    for (uint32 terrain : {0u, 1u, 2u, 7u, 255u, 4096u, 32767u})
    {
        for (uint32 code = 0; code <= 255u; ++code)
        {
            const uint32 bits = GrassPackTerrainAndClumpBits(terrain, code);
            EXPECT_EQ(GrassUnpackTerrainIndexBits(bits), terrain) << "code=" << code;
            EXPECT_EQ(GrassUnpackClumpCodeBits(bits), code) << "terrain=" << terrain;
            const float32 f = GrassPackedCustomAsFloat(bits);
            EXPECT_TRUE(std::isnormal(f)) << "terrain=" << terrain << " code=" << code;
            EXPECT_GE(f, 1.0f) << "terrain=" << terrain << " code=" << code;
            EXPECT_LT(f, 2.0f) << "terrain=" << terrain << " code=" << code;
        }
    }
    // The control that makes the assertion above mean anything: the SAME payloads without the
    // exponent bias really are denormal, so the test is measuring the bias and not measuring
    // nothing. (0 is a special case - it is zero, not denormal - so it is not asserted here.)
    EXPECT_FALSE(std::isnormal(GrassPackedCustomAsFloat(1u)));
    EXPECT_FALSE(std::isnormal(GrassPackedCustomAsFloat(255u << kGrassCustomClumpShift)));
    EXPECT_FALSE(std::isnormal(GrassPackedCustomAsFloat(kGrassTerrainIndexMask)));

    // The two fields cannot reach each other, and neither can reach the exponent.
    EXPECT_EQ(GrassUnpackClumpCodeBits(GrassPackTerrainAndClumpBits(0xFFFFFFFFu, 0u)), 0u);
    EXPECT_EQ(GrassUnpackTerrainIndexBits(GrassPackTerrainAndClumpBits(0u, 0xFFFFFFFFu)), 0u);
    EXPECT_TRUE(std::isnormal(GrassPackedCustomAsFloat(
        GrassPackTerrainAndClumpBits(0xFFFFFFFFu, 0xFFFFFFFFu))));
}

// custom0.x carries two [0,1] scalars whose quantization has to stay under an 8-bit output level,
// and the same denormal hazard applies to it as to the lane above.
TEST(GrassClumpModel, TheFarLodAndGustLaneRoundTripsWithinAnOutputLevel)
{
    // 1/255 is the coarsest an 8-bit output can resolve; both fields must land well inside it.
    constexpr float32 kOutputLevel = 1.0f / 255.0f;
    for (int i = 0; i <= 200; ++i)
    {
        const float32 v = static_cast<float32>(i) / 200.0f;
        for (int j = 0; j <= 200; ++j)
        {
            const float32 w = static_cast<float32>(j) / 200.0f;
            const uint32 bits = GrassPackFarLodAndGustBits(v, w);
            ASSERT_NEAR(GrassUnpackFarLodBits(bits), v, kOutputLevel * 0.5f);
            ASSERT_NEAR(GrassUnpackGustBits(bits), w, kOutputLevel * 0.5f);
            ASSERT_TRUE(std::isnormal(GrassPackedCustomAsFloat(bits)));
        }
    }
    // Endpoints are exact, so a still field and a fully gusting one are not merely close.
    EXPECT_EQ(GrassUnpackFarLodBits(GrassPackFarLodAndGustBits(0.0f, 0.0f)), 0.0f);
    EXPECT_EQ(GrassUnpackFarLodBits(GrassPackFarLodAndGustBits(1.0f, 0.0f)), 1.0f);
    EXPECT_EQ(GrassUnpackGustBits(GrassPackFarLodAndGustBits(0.0f, 1.0f)), 1.0f);
    // Neither field reaches the other or the exponent.
    EXPECT_EQ(GrassUnpackGustBits(GrassPackFarLodAndGustBits(1.0f, 0.0f)), 0.0f);
    EXPECT_EQ(GrassUnpackFarLodBits(GrassPackFarLodAndGustBits(0.0f, 1.0f)), 0.0f);
}

// custom0.z carries the ground's own normal at the blade's foot, which is what the blade's grounded
// end shades on. FLAT terrain must decode to EXACTLY (0, 1, 0): a settle target a fraction off
// vertical is the defect this lane exists to remove, and it costs the most under a low sun.
TEST(GrassClumpModel, TheTerrainNormalLaneIsExactlyVerticalOnFlatGround)
{
    const uint32 flat = GrassPackTerrainNormalBits(0.0f, 0.0f);
    EXPECT_EQ(GrassUnpackTerrainNormalXBits(flat), 0.0f);
    EXPECT_EQ(GrassUnpackTerrainNormalZBits(flat), 0.0f);
    EXPECT_TRUE(std::isnormal(GrassPackedCustomAsFloat(flat)));

    // Endpoints are exact too, so a wall-steep normal is not quantized inward.
    EXPECT_EQ(GrassUnpackTerrainNormalXBits(GrassPackTerrainNormalBits(1.0f, -1.0f)), 1.0f);
    EXPECT_EQ(GrassUnpackTerrainNormalZBits(GrassPackTerrainNormalBits(1.0f, -1.0f)), -1.0f);

    // Angular error of the round trip. Y is REBUILT from X and Z, so the error grows as the
    // surface approaches vertical and Y approaches zero: two bars, one over the slopes grass
    // actually stands on and a looser one that pins the steep tail rather than leaving it unstated.
    // Both are measured maxima (0.054 and 0.259 degrees) with margin, not guesses.
    constexpr float32 kPi = 3.14159265358979323846f;
    constexpr float32 kMaxAngleErrorDegrees = 0.08f;      // slopes to 45 degrees
    constexpr float32 kMaxSteepAngleErrorDegrees = 0.35f; // out to 82 degrees
    constexpr float32 kGentleSlopeXZ = 0.7071f;
    constexpr float32 kSteepestSampledXZ = 0.99f;
    for (int ix = -99; ix <= 99; ++ix)
    {
        for (int iz = -99; iz <= 99; ++iz)
        {
            const float32 nx = static_cast<float32>(ix) / 100.0f;
            const float32 nz = static_cast<float32>(iz) / 100.0f;
            const float32 lenXZ = std::sqrt(nx * nx + nz * nz);
            if (lenXZ > kSteepestSampledXZ)
                continue;
            const float32 ny = std::sqrt(std::max(1.0f - nx * nx - nz * nz, 0.0f));
            const uint32 bits = GrassPackTerrainNormalBits(nx, nz);
            const float32 dx = GrassUnpackTerrainNormalXBits(bits);
            const float32 dz = GrassUnpackTerrainNormalZBits(bits);
            const float32 dy = std::sqrt(std::max(1.0f - dx * dx - dz * dz, 0.0f));
            const float32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float32 cosine = std::clamp((nx * dx + ny * dy + nz * dz) / len, -1.0f, 1.0f);
            const float32 degrees = std::acos(cosine) * (180.0f / kPi);
            const float32 bar = lenXZ <= kGentleSlopeXZ ? kMaxAngleErrorDegrees
                                                        : kMaxSteepAngleErrorDegrees;
            ASSERT_LT(degrees, bar) << "nx=" << nx << " nz=" << nz;
            ASSERT_TRUE(std::isnormal(GrassPackedCustomAsFloat(bits)));
        }
    }

    // Neither component reaches the other or the exponent.
    EXPECT_EQ(GrassUnpackTerrainNormalZBits(GrassPackTerrainNormalBits(1.0f, 0.0f)), 0.0f);
    EXPECT_EQ(GrassUnpackTerrainNormalXBits(GrassPackTerrainNormalBits(0.0f, 1.0f)), 0.0f);
    EXPECT_TRUE(std::isnormal(GrassPackedCustomAsFloat(GrassPackTerrainNormalBits(1.0f, 1.0f))));
}

} // namespace GameEngine::TerrainGrass
