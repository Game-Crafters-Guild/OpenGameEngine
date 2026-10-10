// Resident-window atlas oracles (design §5): slot-pool churn/leak/generation +
// frames-in-flight quarantine (Risk 2), sampling parity + shared-edge continuity
// across disjoint slots, mixed-LOD edge ownership (Risk 1), and out-of-window
// fallback + horizon-seam continuity (Risk 3). Every risk mitigation has a
// discriminating test that fails when the mitigation is disabled — the disable
// step is documented in each test's comment.
#include "TerrainECS/AtlasSlotPool.h"
#include "TerrainECS/TerrainAtlas.h"
#include "TerrainECS/TerrainSizingPlan.h" // height-source engagement rule + author-facing sizing
#include "TerrainECS/TerrainRenderPublication.h" // did the terrain reach the renderer at all
#include "ECS/Components.h" // ECS::Disabled (the extraction filter the live count must match)
#include "ECS/ECSTemplates.h" // World::Create and AddComponentImmediate, instantiated here
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "TerrainECS/AtlasResidencyController.h"
#include "TerrainECS/TerrainReprovisionDebounce.h" // SamplesPerMeter/size re-provision settle gate
#include "TerrainECS/TileStreamingManager.h" // TileStreamingPriority (lever 3 ordering oracle)
#include "TerrainECS/TerrainRenderFeature.h" // AtlasGrassSource
#include "TerrainECS/TerrainService.h" // ResetSplatmap
#include "TerrainECS/TerrainDefaultSurfaceRules.h" // MakeDefaultTerrainSurfaceRules
#include "TerrainECS/TerrainRuleNoise.h" // SurfaceRuleNoiseSample
#include "TerrainECS/TerrainSplatComposite.h" // CompositeSplatTexel
#include "TerrainECS/TerrainSurfaceRuleEval.h" // EvaluateTerrainSurfaceRuleWeight
#include "Components/Terrain/TerrainSurfaceRules.h" // kMaxTerrainSurfaceRules (binding-size sweep)
#include "TerrainGrass/TerrainGrassRenderFeature.h" // GrassAtlasParamsGPU + BuildAtlasParams
#include "Terrain/Heightfield.h"
#include "Mathematics/MatrixOps.h" // MakeLookAtLH (tile-priority +Z look pin)

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;

namespace
{
// Residency-mechanics tests below drive the controller with the upgrade crossfade DISABLED
// (fadeSeconds 0) so they exercise the pre-fade slot/version semantics in isolation; the crossfade
// itself has its own dedicated oracles (AtlasUpgradeCrossfade suite). With fade off the delta is inert.
constexpr float32 kNoFade = 0.0f;
constexpr float32 kNoFadeDt = 0.0f;

// A deterministic, smooth per-GLOBAL-sample height so adjacent tiles share their
// boundary samples exactly (endpoint-exact, like the engine's world-space noise).
float32 WorldHeight(int32 gx, int32 gz)
{
    return 0.5f + 0.25f * std::sin(gx * 0.31f) * std::cos(gz * 0.27f);
}

// Fill a tileRes*tileRes Full tile at grid (tx,tz) from WorldHeight over the
// stitched global grid (shared edges collapse to identical samples).
std::vector<float32> MakeFullTile(uint32 tileRes, int32 tx, int32 tz)
{
    std::vector<float32> h(static_cast<size_t>(tileRes) * tileRes);
    const int32 interior = static_cast<int32>(tileRes) - 1;
    for (uint32 z = 0; z < tileRes; ++z)
        for (uint32 x = 0; x < tileRes; ++x)
            h[static_cast<size_t>(z) * tileRes + x] =
                WorldHeight(tx * interior + static_cast<int32>(x), tz * interior + static_cast<int32>(z));
    return h;
}

// A deliberately-different coarse approximation of a tile (a smooth ramp), so its
// shared edge genuinely diverges from the Full neighbour's exact edge — the
// mixed-LOD seam the yield rule must close.
std::vector<float32> MakeCoarseTile(uint32 tileRes)
{
    std::vector<float32> h(static_cast<size_t>(tileRes) * tileRes);
    for (uint32 z = 0; z < tileRes; ++z)
        for (uint32 x = 0; x < tileRes; ++x)
            h[static_cast<size_t>(z) * tileRes + x] =
                0.2f + 0.6f * (static_cast<float32>(x) / static_cast<float32>(tileRes - 1));
    return h;
}
} // namespace

// ---------------------------------------------------------------------------
// Geometry (Risk 3 correction: slotStride = tileRes + 2, not tileRes)
// ---------------------------------------------------------------------------
TEST(TerrainAtlasGeometry, SlotStrideIncludesApronAndSquarePacks)
{
    const AtlasGeometry g = MakeAtlasGeometry(/*tileRes*/ 1025, /*slots*/ 36, /*tilesX*/ 50, /*tilesZ*/ 50);
    EXPECT_TRUE(g.IsValid());
    EXPECT_EQ(g.SlotStride, 1025u + 2u * kAtlasApron);
    EXPECT_EQ(g.SlotStride, 1027u);
    EXPECT_GE(g.SlotsPerRow * g.SlotsPerRow, g.SlotCount);
    EXPECT_EQ(g.AtlasDim, g.SlotsPerRow * g.SlotStride);
    // The interior origin is past the apron on both axes.
    EXPECT_EQ(g.SlotInteriorTexelX(0), kAtlasApron);
    EXPECT_EQ(g.SlotInteriorTexelY(0), kAtlasApron);
}

// ---------------------------------------------------------------------------
// AtlasSlotPool — churn / leak / generation
// ---------------------------------------------------------------------------
TEST(AtlasSlotPool, AcquireAssignsAndFinds)
{
    AtlasSlotPool pool;
    pool.Initialize(/*slots*/ 4, /*framesInFlight*/ 2);
    pool.BeginFrame(1);
    const AtlasSlotRef ref = pool.Acquire({0, 0}, 1.0f);
    ASSERT_TRUE(ref.IsResident());
    EXPECT_EQ(pool.Find({0, 0}).Slot, ref.Slot);
    EXPECT_EQ(pool.ResidentCount(), 1u);
    EXPECT_EQ(pool.AssignsThisFrame(), 1u);
    EXPECT_FALSE(pool.Find({9, 9}).IsResident());
}

TEST(AtlasSlotPool, NoLeakUnderChurn)
{
    AtlasSlotPool pool;
    pool.Initialize(/*slots*/ 8, /*framesInFlight*/ 2);
    uint64 frame = 0;
    for (int32 i = 0; i < 5000; ++i)
    {
        pool.BeginFrame(++frame);
        const TileCoord c{i % 20, (i / 20) % 20};
        const AtlasSlotRef ref = pool.Acquire(c, static_cast<float32>(i % 7));
        if (ref.IsResident() && (i % 3 == 0))
            pool.Release(c, ref.Generation);
        // Every slot is in exactly one state at all times: no leak.
        EXPECT_EQ(pool.FreeCount() + pool.ResidentCount() + pool.QuarantinedCount(), pool.SlotCount());
        EXPECT_LE(pool.ResidentCount(), pool.SlotCount());
    }
}

TEST(AtlasSlotPool, StaleReleaseRefused)
{
    AtlasSlotPool pool;
    pool.Initialize(4, 2);
    pool.BeginFrame(1);
    const AtlasSlotRef ref = pool.Acquire({2, 3}, 1.0f);
    ASSERT_TRUE(ref.IsResident());
    // A stale generation must NOT free the live slot (E6 / #495 discriminator bit).
    EXPECT_FALSE(pool.Release({2, 3}, ref.Generation + 7));
    EXPECT_TRUE(pool.Find({2, 3}).IsResident());
    // The correct generation frees it.
    EXPECT_TRUE(pool.Release({2, 3}, ref.Generation));
    EXPECT_FALSE(pool.Find({2, 3}).IsResident());
}

TEST(AtlasSlotPool, ResetSweepFreesAllAndBumpsGeneration)
{
    AtlasSlotPool pool;
    pool.Initialize(4, 2);
    pool.BeginFrame(1);
    const AtlasSlotRef a = pool.Acquire({0, 0}, 1.0f);
    const AtlasSlotRef b = pool.Acquire({1, 0}, 1.0f);
    ASSERT_TRUE(a.IsResident());
    ASSERT_TRUE(b.IsResident());
    pool.Reset();
    EXPECT_EQ(pool.ResidentCount(), 0u);
    EXPECT_EQ(pool.FreeCount(), pool.SlotCount());
    EXPECT_FALSE(pool.Find({0, 0}).IsResident());
    // A handle held across the sweep is staled (generation bumped).
    EXPECT_FALSE(pool.Release({0, 0}, a.Generation));
}

// ---------------------------------------------------------------------------
// Risk 2 — eviction vs frames-in-flight (quarantine)
//
// DISCRIMINATOR: a released slot must not be handed to a new tile for
// framesInFlight frames. Re-run with Initialize(..., framesInFlight=0) and the
// "not reused while quarantined" assertion fails (the wrong-terrain flash).
// ---------------------------------------------------------------------------
TEST(AtlasSlotPool, Risk2_QuarantineDefersReuse)
{
    constexpr uint32 kSlots = 2;
    constexpr uint32 kFIF = 3;
    AtlasSlotPool pool;
    pool.Initialize(kSlots, kFIF);

    // Frame 1: fill the pool.
    pool.BeginFrame(1);
    const AtlasSlotRef s0 = pool.Acquire({0, 0}, 1.0f);
    const AtlasSlotRef s1 = pool.Acquire({1, 0}, 1.0f);
    ASSERT_TRUE(s0.IsResident());
    ASSERT_TRUE(s1.IsResident());
    const uint32 freedSlot = s0.Slot;

    // Frame 2: release {0,0}. Its slot is quarantined, NOT free.
    pool.BeginFrame(2);
    ASSERT_TRUE(pool.Release({0, 0}, s0.Generation));
    EXPECT_EQ(pool.QuarantinedCount(), 1u);

    // Frames 2..(2+kFIF-1): a new tile must NOT reuse the quarantined slot.
    for (uint64 f = 2; f < 2 + kFIF; ++f)
    {
        pool.BeginFrame(f);
        const AtlasSlotRef n = pool.Acquire({5, 5}, 1.0f);
        EXPECT_NE(n.Slot, freedSlot) << "quarantined slot reused at frame " << f
                                     << " (framesInFlight guard defeated)";
        if (n.IsResident())
            pool.Release({5, 5}, n.Generation); // keep the pool churning
    }

    // Once the quarantine expires the slot is reusable again.
    pool.BeginFrame(2 + kFIF);
    const AtlasSlotRef reuse = pool.Acquire({6, 6}, 1.0f);
    ASSERT_TRUE(reuse.IsResident());
    EXPECT_EQ(reuse.Slot, freedSlot) << "slot never returned to the free list after quarantine";
}

// Locks the disabled behaviour so the discriminator above is demonstrably
// falsifiable: framesInFlight=0 hands the freed slot straight back the same frame.
TEST(AtlasSlotPool, Risk2_QuarantineDisabledReusesImmediately)
{
    AtlasSlotPool pool;
    pool.Initialize(/*slots*/ 2, /*framesInFlight*/ 0);
    pool.BeginFrame(1);
    const AtlasSlotRef s0 = pool.Acquire({0, 0}, 1.0f);
    pool.Acquire({1, 0}, 1.0f);
    const uint32 freed = s0.Slot;
    pool.BeginFrame(2);
    ASSERT_TRUE(pool.Release({0, 0}, s0.Generation));
    EXPECT_EQ(pool.QuarantinedCount(), 0u);
    const AtlasSlotRef n = pool.Acquire({5, 5}, 1.0f);
    EXPECT_EQ(n.Slot, freed) << "with the quarantine disabled the slot is reused immediately";
}

// ---------------------------------------------------------------------------
// Sampling parity + shared-edge continuity across DISJOINT slots
// ---------------------------------------------------------------------------
namespace
{
// Build a 2x2 Full-tile atlas and its indirection table. Slot s holds tile
// (s%2, s/2). Returns the atlas texels + table for the sampler oracles.
struct BuiltAtlas
{
    AtlasGeometry Geo;
    std::vector<float32> Atlas;
    AtlasIndirectionTable Table;
};

BuiltAtlas Build2x2FullAtlas(uint32 tileRes)
{
    BuiltAtlas b;
    b.Geo = MakeAtlasGeometry(tileRes, /*slots*/ 4, /*tilesX*/ 2, /*tilesZ*/ 2);
    b.Atlas.assign(static_cast<size_t>(b.Geo.AtlasDim) * b.Geo.AtlasDim, 0.0f);
    b.Table.Resize(b.Geo);
    uint32 slot = 0;
    for (int32 tz = 0; tz < 2; ++tz)
        for (int32 tx = 0; tx < 2; ++tx)
        {
            const std::vector<float32> heights = MakeFullTile(tileRes, tx, tz);
            PackTileHeightIntoSlot(b.Atlas.data(), b.Geo, slot, heights.data(),
                                   /*tileIsFull*/ true, AtlasTileNeighbors{}, AtlasNeighborEdges{});
            TileAtlasSlot& row = b.Table.Row(tx, tz);
            row.Slot = slot;
            row.Generation = 1;
            row.LodBias = kAtlasLodBiasFull;
            ++slot;
        }
    return b;
}
} // namespace

TEST(AtlasSampler, ParityWithDirectTileSampleAtGridPoints)
{
    const uint32 tileRes = 9;
    BuiltAtlas b = Build2x2FullAtlas(tileRes);
    AtlasHeightSampler s{b.Geo, b.Table.Rows.data(), b.Atlas.data(), nullptr};

    const int32 interior = static_cast<int32>(tileRes) - 1;
    const int32 globalW = interior * 2 + 1; // 17
    // At every stitched grid sample, the atlas sample must equal world truth,
    // bit-exact (bilinear collapses at a texel centre).
    for (int32 gz = 0; gz < globalW; ++gz)
        for (int32 gx = 0; gx < globalW; ++gx)
        {
            const float32 u = static_cast<float32>(gx) / static_cast<float32>(globalW - 1);
            const float32 v = static_cast<float32>(gz) / static_cast<float32>(globalW - 1);
            EXPECT_FLOAT_EQ(s.SampleHeightNormalized(u, v), WorldHeight(gx, gz))
                << "grid (" << gx << "," << gz << ")";
        }
}

// ---------------------------------------------------------------------------
// Sampling-convention parity between the two height sources.
//
// The atlas resolves terrain UV to absolute texel u*(W-1) (endpoint-exact), which is the
// convention HeightfieldData::SampleBilinear uses — so the atlas agrees with the CPU authority
// that physics colliders, height queries and picking are built from. The unified source is fed
// straight to a linear sampler as textureLod(gHeight, uv), which lands on u*W - 0.5.
//
// The two therefore differ by (u - 0.5) texels: zero at the terrain centre, half a texel at the
// edges. This oracle pins WHICH convention matches CPU truth, so that the divergence is a
// recorded, quantified property rather than a surprise — and so that unifying the conventions
// later fails here loudly instead of silently shifting every terrain.
// ---------------------------------------------------------------------------
TEST(AtlasSampler, TheAtlasMatchesCpuTruthWhileTheUnifiedConventionIsHalfATexelOff)
{
    const uint32 tileRes = 9;
    BuiltAtlas b = Build2x2FullAtlas(tileRes);
    AtlasHeightSampler s{b.Geo, b.Table.Rows.data(), b.Atlas.data(), nullptr};

    const int32 interior = static_cast<int32>(tileRes) - 1;
    const uint32 globalW = static_cast<uint32>(interior * 2 + 1); // 17
    std::vector<float32> global(static_cast<size_t>(globalW) * globalW);
    for (uint32 gz = 0; gz < globalW; ++gz)
        for (uint32 gx = 0; gx < globalW; ++gx)
            global[gz * globalW + gx] = WorldHeight(static_cast<int32>(gx), static_cast<int32>(gz));

    const float32 span = static_cast<float32>(globalW - 1);
    float32 worstAtlas = 0.0f;
    float32 worstUnified = 0.0f;
    for (int32 i = 0; i <= 64; ++i)
    {
        const float32 u = static_cast<float32>(i) / 64.0f;
        const float32 v = 0.5f;

        // CPU authority: endpoint-exact, u -> texel u*(W-1).
        const float32 truth = SampleGridBilinearTexel(global.data(), globalW, globalW,
                                                      u * span, v * span);
        // The unified source's convention: a raw normalized UV into a linear sampler.
        const float32 unified = SampleGridBilinearTexel(
            global.data(), globalW, globalW,
            u * static_cast<float32>(globalW) - 0.5f, v * static_cast<float32>(globalW) - 0.5f);

        worstAtlas = std::max(worstAtlas, std::fabs(s.SampleHeightNormalized(u, v) - truth));
        worstUnified = std::max(worstUnified, std::fabs(unified - truth));
    }

    EXPECT_LT(worstAtlas, 1e-6f)
        << "the atlas must agree with the CPU heightfield the colliders are built from";
    EXPECT_GT(worstUnified, 1e-3f)
        << "if this stops diverging the two conventions have been unified — update this oracle "
           "and the height-source notes deliberately, do not just relax the bound";
}

TEST(AtlasSampler, SharedEdgeBitEqualBetweenAdjacentFullSlots)
{
    const uint32 tileRes = 9;
    BuiltAtlas b = Build2x2FullAtlas(tileRes);

    // Tile (0,0) slot 0, tile (1,0) slot 1: their +X / -X interior edges are the
    // same world samples and must be bit-equal in the two disjoint slots.
    std::vector<float32> rightOf00, leftOf10;
    ReadSlotInteriorEdge(b.Atlas.data(), b.Geo, /*slot*/ 0, AtlasEdgeSide::PosX, rightOf00);
    ReadSlotInteriorEdge(b.Atlas.data(), b.Geo, /*slot*/ 1, AtlasEdgeSide::NegX, leftOf10);
    ASSERT_EQ(rightOf00.size(), leftOf10.size());
    for (size_t i = 0; i < rightOf00.size(); ++i)
        EXPECT_FLOAT_EQ(rightOf00[i], leftOf10[i]) << "shared-edge texel " << i;

    // And the sampler is continuous across the boundary UV.
    AtlasHeightSampler s{b.Geo, b.Table.Rows.data(), b.Atlas.data(), nullptr};
    const float32 boundaryU = 0.5f; // between tile 0 and tile 1 on a 2-wide grid
    for (float32 v = 0.0f; v <= 1.0f; v += 0.1f)
    {
        const float32 hAt = s.SampleHeightNormalized(boundaryU, v);
        const float32 hLeft = s.SampleHeightNormalized(boundaryU - 1e-4f, v);
        EXPECT_NEAR(hAt, hLeft, 1e-3f) << "v=" << v;
    }
}

// ---------------------------------------------------------------------------
// Risk 1 — mixed-LOD shared edges across slots (edge ownership)
// ---------------------------------------------------------------------------
TEST(AtlasEdgeOwnership, CoarseYieldsToFullNeighborOnly)
{
    // Only (coarse self, resident Full neighbour) yields; every other combo keeps Self.
    EXPECT_EQ(ResolveEdgeOwnership(/*selfFull*/ false, /*nbrResident*/ true, /*nbrFull*/ true),
              AtlasEdgeSource::Neighbor);
    EXPECT_EQ(ResolveEdgeOwnership(false, true, false), AtlasEdgeSource::Self); // both coarse
    EXPECT_EQ(ResolveEdgeOwnership(true, true, true), AtlasEdgeSource::Self);   // both Full
    EXPECT_EQ(ResolveEdgeOwnership(true, true, false), AtlasEdgeSource::Self);  // self Full owns
    EXPECT_EQ(ResolveEdgeOwnership(false, false, false), AtlasEdgeSource::Self); // no neighbour (horizon)
}

// DISCRIMINATOR: a coarse tile beside a Full neighbour must copy the Full edge
// into its own slot so the two physical copies agree. Disable the yield (pack
// with default AtlasTileNeighbors{} so ResolveEdgeOwnership returns Self) and the
// coarse approximation diverges from the Full edge — the seam trench.
TEST(AtlasEdgeOwnership, Risk1_CoarseSlotSharedEdgeMatchesFullNeighbor)
{
    const uint32 tileRes = 9;
    AtlasGeometry geo = MakeAtlasGeometry(tileRes, /*slots*/ 2, /*tilesX*/ 2, /*tilesZ*/ 1);
    std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim, 0.0f);

    // Slot 1 = Full tile (1,0). Slot 0 = coarse tile (0,0) to its left.
    const std::vector<float32> full = MakeFullTile(tileRes, /*tx*/ 1, /*tz*/ 0);
    PackTileHeightIntoSlot(atlas.data(), geo, /*slot*/ 1, full.data(),
                           /*tileIsFull*/ true, AtlasTileNeighbors{}, AtlasNeighborEdges{});
    std::vector<float32> fullLeftEdge; // the Full tile's -X interior edge (shared with the coarse tile)
    ReadSlotInteriorEdge(atlas.data(), geo, /*slot*/ 1, AtlasEdgeSide::NegX, fullLeftEdge);

    const std::vector<float32> coarse = MakeCoarseTile(tileRes);

    // WITH the mitigation: the coarse tile's +X (right) edge yields to the Full neighbour.
    AtlasTileNeighbors nb{};
    nb.RightResident = true;
    nb.RightFull = true;
    AtlasNeighborEdges edges{};
    edges.Right = fullLeftEdge; // the Full neighbour's shared edge
    PackTileHeightIntoSlot(atlas.data(), geo, /*slot*/ 0, coarse.data(),
                           /*tileIsFull*/ false, nb, edges);
    std::vector<float32> coarseRightEdge;
    ReadSlotInteriorEdge(atlas.data(), geo, /*slot*/ 0, AtlasEdgeSide::PosX, coarseRightEdge);
    ASSERT_EQ(coarseRightEdge.size(), fullLeftEdge.size());
    for (size_t i = 0; i < coarseRightEdge.size(); ++i)
        EXPECT_FLOAT_EQ(coarseRightEdge[i], fullLeftEdge[i]) << "yielded edge texel " << i;

    // WITHOUT the mitigation (default neighbours -> Self): the coarse approximation
    // diverges from the Full edge. Proves the test is falsifiable.
    PackTileHeightIntoSlot(atlas.data(), geo, /*slot*/ 0, coarse.data(),
                           /*tileIsFull*/ false, AtlasTileNeighbors{}, AtlasNeighborEdges{});
    std::vector<float32> coarseRightNoYield;
    ReadSlotInteriorEdge(atlas.data(), geo, /*slot*/ 0, AtlasEdgeSide::PosX, coarseRightNoYield);
    uint32 diffs = 0;
    for (size_t i = 0; i < coarseRightNoYield.size(); ++i)
        if (std::abs(coarseRightNoYield[i] - fullLeftEdge[i]) > 1e-4f)
            ++diffs;
    EXPECT_GT(diffs, 0u) << "coarse edge should diverge from Full without the yield (test not discriminating)";
}

// A coarse tile beside a coarse neighbour that upgrades coarse->Full has a STALE
// shared edge until it is refreshed. Honest discriminator: re-pack ONLY the
// upgraded tile, assert the shared-edge oracle FAILS, then re-pack the neighbours
// NeighborsNeedingApronRefresh returns and assert it PASSES.
TEST(AtlasEdgeOwnership, Risk1_ApronRefreshDiscriminates)
{
    const uint32 tileRes = 9;
    AtlasGeometry geo = MakeAtlasGeometry(tileRes, /*slots*/ 2, /*tilesX*/ 2, /*tilesZ*/ 1);
    std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim, 0.0f);
    AtlasIndirectionTable table;
    table.Resize(geo);
    table.Row(0, 0).Slot = 0; // T = coarse tile (0,0)
    table.Row(1, 0).Slot = 1; // N = tile (1,0), coarse then upgraded

    // T coarse; N starts coarse. Neither Full -> T keeps its own +X edge.
    const std::vector<float32> coarse = MakeCoarseTile(tileRes);
    PackTileHeightIntoSlot(atlas.data(), geo, /*slot*/ 0, coarse.data(),
                           /*tileIsFull*/ false, AtlasTileNeighbors{}, AtlasNeighborEdges{});
    PackTileHeightIntoSlot(atlas.data(), geo, /*slot*/ 1, coarse.data(),
                           /*tileIsFull*/ false, AtlasTileNeighbors{}, AtlasNeighborEdges{});

    // N upgrades coarse->Full: re-pack ONLY N (the streamed-in Full detail).
    const std::vector<float32> full = MakeFullTile(tileRes, /*tx*/ 1, /*tz*/ 0);
    PackTileHeightIntoSlot(atlas.data(), geo, /*slot*/ 1, full.data(),
                           /*tileIsFull*/ true, AtlasTileNeighbors{}, AtlasNeighborEdges{});
    std::vector<float32> nLeft, tRight;
    ReadSlotInteriorEdge(atlas.data(), geo, /*slot*/ 1, AtlasEdgeSide::NegX, nLeft);
    ReadSlotInteriorEdge(atlas.data(), geo, /*slot*/ 0, AtlasEdgeSide::PosX, tRight);

    // WITHOUT the refresh: T's edge is the stale coarse value -> diverges from N's Full edge.
    uint32 stale = 0;
    for (size_t i = 0; i < nLeft.size(); ++i)
        if (std::abs(nLeft[i] - tRight[i]) > 1e-4f)
            ++stale;
    EXPECT_GT(stale, 0u) << "shared edge should be stale before the refresh (test not discriminating)";

    // The refresh set names T; re-pack it, yielding its +X edge to N's new Full edge.
    const std::vector<TileCoord> refresh = NeighborsNeedingApronRefresh(table, /*tx*/ 1, /*tz*/ 0);
    ASSERT_FALSE(refresh.empty());
    EXPECT_TRUE(std::find(refresh.begin(), refresh.end(), TileCoord{0, 0}) != refresh.end());
    AtlasTileNeighbors nb{};
    nb.RightResident = true;
    nb.RightFull = true;
    AtlasNeighborEdges edges{};
    edges.Right = nLeft;
    PackTileHeightIntoSlot(atlas.data(), geo, /*slot*/ 0, coarse.data(),
                           /*tileIsFull*/ false, nb, edges);
    ReadSlotInteriorEdge(atlas.data(), geo, /*slot*/ 0, AtlasEdgeSide::PosX, tRight);
    for (size_t i = 0; i < nLeft.size(); ++i)
        EXPECT_FLOAT_EQ(tRight[i], nLeft[i]) << "edge still stale after refresh, texel " << i;
}

// The refresh set covers axis AND diagonal resident neighbours (a diagonal
// neighbour shares only the corner texel — omitting it is the 4-tile corner bug).
TEST(AtlasEdgeOwnership, Risk1_ApronRefreshSetIncludesDiagonals)
{
    const uint32 tileRes = 9;
    AtlasGeometry geo = MakeAtlasGeometry(tileRes, 4, /*tilesX*/ 2, /*tilesZ*/ 2);
    AtlasIndirectionTable table;
    table.Resize(geo);
    for (int32 tz = 0; tz < 2; ++tz)
        for (int32 tx = 0; tx < 2; ++tx)
        {
            table.Row(tx, tz).Slot = static_cast<uint32>(tz * 2 + tx);
            table.Row(tx, tz).Generation = 1;
        }
    // Tile (1,0): resident axis neighbours (0,0),(1,1) + resident DIAGONAL (0,1).
    const std::vector<TileCoord> refresh = NeighborsNeedingApronRefresh(table, /*tx*/ 1, /*tz*/ 0);
    EXPECT_EQ(refresh.size(), 3u);
    EXPECT_TRUE(std::find(refresh.begin(), refresh.end(), TileCoord{0, 1}) != refresh.end())
        << "diagonal neighbour omitted -> the 4-tile corner never refreshes";
    // A non-resident diagonal drops out of the set.
    table.Row(0, 1).Slot = kAtlasNoSlot;
    EXPECT_EQ(NeighborsNeedingApronRefresh(table, 1, 0).size(), 2u);
}

// DISCRIMINATOR (4-tile mixed-LOD junction): the corner texel shared by four tiles
// must be bit-equal across all four. With C Full and A/B/D coarse around the
// centre corner, A is DIAGONAL to C, so no axis edge carries C's corner to A —
// only the corner-ownership rule does. Disable A's corner yield and A's copy
// diverges from C's.
TEST(AtlasEdgeOwnership, Risk1_FourTileCornerBitEqualAtMixedLodJunction)
{
    const uint32 tileRes = 9;
    AtlasGeometry geo = MakeAtlasGeometry(tileRes, 4, /*tilesX*/ 2, /*tilesZ*/ 2);
    std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim, 0.0f);
    // slot = tz*2 + tx: A=(0,0)=0, B=(1,0)=1, D=(0,1)=2, C=(1,1)=3.
    const uint32 sA = 0, sB = 1, sD = 2, sC = 3;

    // C Full; A/B/D coarse. The centre corner = A.BR = B.BL = D.TR = C.TL.
    const std::vector<float32> cFull = MakeFullTile(tileRes, /*tx*/ 1, /*tz*/ 1);
    const std::vector<float32> coarse = MakeCoarseTile(tileRes);
    PackTileHeightIntoSlot(atlas.data(), geo, sC, cFull.data(), /*tileIsFull*/ true,
                           AtlasTileNeighbors{}, AtlasNeighborEdges{});

    // The authoritative corner value: C is the only Full tile among the four.
    const bool sharersFull[4] = {false /*A*/, false /*B*/, false /*D*/, true /*C*/};
    EXPECT_EQ(ResolveCornerOwner(sharersFull, 4), 3u);
    std::vector<float32> cTop, cLeft;
    ReadSlotInteriorEdge(atlas.data(), geo, sC, AtlasEdgeSide::NegZ, cTop);  // C top row; [0] = C.TL
    ReadSlotInteriorEdge(atlas.data(), geo, sC, AtlasEdgeSide::NegX, cLeft); // C left col; [0] = C.TL
    const float32 cornerAuthoritative = cTop[0];

    // B's +Z (bottom) edge yields to C (below B); D's +X (right) edge yields to C
    // (right of D). Those edge copies carry the corner into B.BL and D.TR.
    AtlasTileNeighbors nbB{};
    nbB.BottomResident = true; nbB.BottomFull = true;
    AtlasNeighborEdges edB{}; edB.Bottom = cTop;
    PackTileHeightIntoSlot(atlas.data(), geo, sB, coarse.data(), /*tileIsFull*/ false, nbB, edB);

    AtlasTileNeighbors nbD{};
    nbD.RightResident = true; nbD.RightFull = true;
    AtlasNeighborEdges edD{}; edD.Right = cLeft;
    PackTileHeightIntoSlot(atlas.data(), geo, sD, coarse.data(), /*tileIsFull*/ false, nbD, edD);

    auto texel = [&](uint32 slot, uint32 lx, uint32 lz) {
        return atlas[static_cast<size_t>(geo.SlotInteriorTexelY(slot) + lz) * geo.AtlasDim +
                     (geo.SlotInteriorTexelX(slot) + lx)];
    };
    const uint32 last = tileRes - 1u;

    // A is DIAGONAL to C (its axis neighbours B/D are coarse), so only the corner
    // rule fixes A.BR. WITH the corner yield:
    AtlasCornerYields cornersA{};
    cornersA.BottomRight = {true, cornerAuthoritative};
    PackTileHeightIntoSlot(atlas.data(), geo, sA, coarse.data(), /*tileIsFull*/ false,
                           AtlasTileNeighbors{}, AtlasNeighborEdges{}, cornersA);
    EXPECT_FLOAT_EQ(texel(sA, last, last), cornerAuthoritative); // A.BR
    EXPECT_FLOAT_EQ(texel(sB, 0, last), cornerAuthoritative);    // B.BL
    EXPECT_FLOAT_EQ(texel(sD, last, 0), cornerAuthoritative);    // D.TR
    EXPECT_FLOAT_EQ(texel(sC, 0, 0), cornerAuthoritative);       // C.TL

    // WITHOUT A's corner yield: A.BR keeps its own coarse value -> diverges.
    PackTileHeightIntoSlot(atlas.data(), geo, sA, coarse.data(), /*tileIsFull*/ false,
                           AtlasTileNeighbors{}, AtlasNeighborEdges{});
    EXPECT_NE(texel(sA, last, last), cornerAuthoritative)
        << "diagonal corner not reconciled without the corner rule (test not discriminating)";
}

// ---------------------------------------------------------------------------
// Risk 3 — out-of-window fallback + horizon-seam continuity
// ---------------------------------------------------------------------------
namespace
{
AtlasCoarseField MakeCoarseField(uint32 dim)
{
    AtlasCoarseField f;
    f.Dim = dim;
    f.Heights.resize(static_cast<size_t>(dim) * dim);
    for (uint32 z = 0; z < dim; ++z)
        for (uint32 x = 0; x < dim; ++x)
            // A smooth low-frequency field over the whole terrain [0,1]^2.
            f.Heights[static_cast<size_t>(z) * dim + x] =
                0.4f + 0.2f * std::sin(x * 0.5f) + 0.1f * std::cos(z * 0.4f);
    return f;
}
} // namespace

// DISCRIMINATOR: a non-resident tile must resolve to the coarse field, never a
// hole. With Coarse=nullptr the fallback returns 0 — the "≈ coarse value"
// assertion fails (garbage/hole).
TEST(AtlasSampler, Risk3_NonResidentTileFallsBackToCoarseNotGarbage)
{
    AtlasGeometry geo = MakeAtlasGeometry(/*tileRes*/ 9, /*slots*/ 4, /*tilesX*/ 4, /*tilesZ*/ 4);
    AtlasIndirectionTable table;
    table.Resize(geo);
    table.Clear(); // NO tile resident -> every lookup is out-of-window

    const AtlasCoarseField coarse = MakeCoarseField(16);

    AtlasHeightSampler withCoarse{geo, table.Rows.data(), nullptr, &coarse};
    AtlasHeightSampler noCoarse{geo, table.Rows.data(), nullptr, nullptr};

    for (float32 v = 0.05f; v < 1.0f; v += 0.2f)
        for (float32 u = 0.05f; u < 1.0f; u += 0.2f)
        {
            const float32 expected = SampleGridBilinearTexel(
                coarse.Heights.data(), coarse.Dim, coarse.Dim,
                u * static_cast<float32>(coarse.Dim - 1), v * static_cast<float32>(coarse.Dim - 1));
            EXPECT_FLOAT_EQ(withCoarse.SampleHeightNormalized(u, v), expected);
            // Disabled fallback: a hole at 0 (the discriminating divergence).
            EXPECT_FLOAT_EQ(noCoarse.SampleHeightNormalized(u, v), 0.0f);
            EXPECT_NE(expected, 0.0f);
        }
}

// The innermost out-of-window tile (fallback) sits against the outermost
// in-window tile. When the in-window edge tile is coarse (packed FROM the same
// coarse field), the horizon shared edge matches to within coarse tolerance —
// no ring seam as tiles cross the window.
TEST(AtlasSampler, Risk3_HorizonSeamContinuity)
{
    const uint32 tileRes = 9;
    const uint32 tilesPerAxis = 4;
    // Resident window = the tx in {0,1} column across every tz, so the +X horizon
    // sits at u = 2/tilesPerAxis for all v. 2 * tilesPerAxis slots.
    AtlasGeometry geo = MakeAtlasGeometry(tileRes, /*slots*/ 2 * tilesPerAxis, tilesPerAxis, tilesPerAxis);
    std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim, 0.0f);
    AtlasIndirectionTable table;
    table.Resize(geo);
    table.Clear();

    const AtlasCoarseField coarse = MakeCoarseField(tilesPerAxis * (tileRes - 1) + 1);
    const int32 interior = static_cast<int32>(tileRes) - 1;

    // Pack the horizon-edge tiles as COARSE, heights sampled from the same coarse
    // field the out-of-window fallback uses — so the shared horizon edge matches.
    auto coarseTileFromField = [&](int32 tx, int32 tz)
    {
        std::vector<float32> h(static_cast<size_t>(tileRes) * tileRes);
        for (uint32 z = 0; z < tileRes; ++z)
            for (uint32 x = 0; x < tileRes; ++x)
                h[static_cast<size_t>(z) * tileRes + x] = SampleGridBilinearTexel(
                    coarse.Heights.data(), coarse.Dim, coarse.Dim,
                    static_cast<float32>(tx * interior + static_cast<int32>(x)),
                    static_cast<float32>(tz * interior + static_cast<int32>(z)));
        return h;
    };
    uint32 slot = 0;
    for (int32 tz = 0; tz < static_cast<int32>(tilesPerAxis); ++tz)
        for (int32 tx = 0; tx <= 1; ++tx)
        {
            const std::vector<float32> h = coarseTileFromField(tx, tz);
            PackTileHeightIntoSlot(atlas.data(), geo, slot, h.data(),
                                   /*tileIsFull*/ false, AtlasTileNeighbors{}, AtlasNeighborEdges{});
            TileAtlasSlot& row = table.Row(tx, tz);
            row.Slot = slot;
            row.Generation = 1;
            row.LodBias = kAtlasLodBiasCoarse;
            ++slot;
        }

    AtlasHeightSampler s{geo, table.Rows.data(), atlas.data(), &coarse};

    // Horizon boundary between tile (1,0) [resident] and tile (2,0) [fallback]:
    // u = 2/tilesPerAxis. Sample just inside (atlas) and just outside (fallback).
    const float32 horizonU = 2.0f / static_cast<float32>(tilesPerAxis);
    EXPECT_TRUE(s.IsResidentAt(horizonU - 1e-4f, 0.5f));
    EXPECT_FALSE(s.IsResidentAt(horizonU + 1e-4f, 0.5f));
    for (float32 v = 0.0f; v <= 1.0f; v += 0.1f)
    {
        const float32 hInside = s.SampleHeightNormalized(horizonU - 1e-4f, v);
        const float32 hOutside = s.SampleHeightNormalized(horizonU + 1e-4f, v);
        // Both derive from the coarse field near the edge -> continuous within tolerance.
        EXPECT_NEAR(hInside, hOutside, 2e-2f) << "horizon v=" << v;
    }
}

// DISCRIMINATOR (PR #506 R1 fix): the out-of-window fallback must be HEIGHT-CONTINUOUS with the
// resident relief at the streaming-window edge — the live frontier. The dark-ship sampled a FLAT
// representative height there, so where fallback tiles met resident relief the surface cracked /
// sky-gapped (the eviction-pressure leg). BuildCoarseHeightField (the shipped fallback source, the
// SAME UV->tile mapping the atlas resolve uses) closes it. Oracle: a bisector straddling the window
// edge samples continuous height through the coarse field, and the SAME straddle CRACKS with the
// flat fallback (coarse=nullptr -> 0). Disabling the coarse field is the falsifiable divergence.
TEST(AtlasSampler, Risk3_CoarseFieldFrontierContinuous_FlatFallbackCracks)
{
    constexpr uint32 tileRes = 17;
    constexpr uint32 tilesPerAxis = 4;
    const int32 interior = static_cast<int32>(tileRes) - 1;

    // Smooth low-frequency relief (real terrain, faithfully captured by the coarse downsample so
    // the frontier stays within coarse tolerance). Centered ~0.72 so it stays clear of the shipped
    // flat fallback value (0.5) at the frontier — a flat 0.5 plane is then a visible cliff there.
    auto H = [](int32 gx, int32 gz) {
        return 0.62f + 0.18f * std::sin(gx * 0.045f) + 0.10f * std::cos(gz * 0.05f);
    };
    auto fullTile = [&](int32 tx, int32 tz) {
        std::vector<float32> h(static_cast<size_t>(tileRes) * tileRes);
        for (uint32 z = 0; z < tileRes; ++z)
            for (uint32 x = 0; x < tileRes; ++x)
                h[static_cast<size_t>(z) * tileRes + x] =
                    H(tx * interior + static_cast<int32>(x), tz * interior + static_cast<int32>(z));
        return h;
    };

    // Resident window = tx in {0,1} across every tz; the +X frontier is at u = 2/tilesPerAxis. Every
    // tile is streamed on the CPU (the coarse field is the whole-terrain downsample), only the window
    // subset is atlas-resident.
    AtlasGeometry geo = MakeAtlasGeometry(tileRes, /*slots*/ 2 * tilesPerAxis, tilesPerAxis, tilesPerAxis);
    std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim, 0.0f);
    AtlasIndirectionTable table;
    table.Resize(geo);
    table.Clear();
    std::vector<std::vector<float32>> tileHeights(static_cast<size_t>(tilesPerAxis) * tilesPerAxis);
    uint32 slot = 0;
    for (int32 tz = 0; tz < static_cast<int32>(tilesPerAxis); ++tz)
        for (int32 tx = 0; tx < static_cast<int32>(tilesPerAxis); ++tx)
        {
            tileHeights[static_cast<size_t>(tz) * tilesPerAxis + tx] = fullTile(tx, tz);
            if (tx <= 1) // resident window
            {
                PackTileHeightIntoSlot(atlas.data(), geo, slot,
                                       tileHeights[static_cast<size_t>(tz) * tilesPerAxis + tx].data(),
                                       /*tileIsFull*/ true, AtlasTileNeighbors{}, AtlasNeighborEdges{});
                TileAtlasSlot& row = table.Row(tx, tz);
                row.Slot = slot;
                row.Generation = 1;
                row.LodBias = kAtlasLodBiasFull;
                ++slot;
            }
        }

    // The coarse field: the whole-terrain downsample the shipped fallback resolves through, built by
    // the SAME BuildCoarseHeightField the extraction runs (single-sourced math).
    AtlasCoarseField coarse;
    coarse.Dim = kAtlasCoarseFieldDim;
    BuildCoarseHeightField(coarse.Dim, tilesPerAxis, tilesPerAxis, /*fallback*/ 0.5f,
                           [&](int32 tx, int32 tz) -> CoarseTileSource {
                               const auto& h = tileHeights[static_cast<size_t>(tz) * tilesPerAxis + tx];
                               return {h.data(), tileRes, tileRes};
                           },
                           coarse.Heights);
    ASSERT_TRUE(coarse.IsValid());

    // The disable leg is the ACTUAL shipped flat fallback: a constant plane at the representative
    // height (kAtlasCoarseFallbackNormalized == 0.5), not a 0 stand-in — so this reproduces exactly
    // what dark-shipped and cracked.
    constexpr float32 kShippedFlatFallback = 0.5f;
    AtlasCoarseField flat;
    flat.Dim = 2u;
    flat.Heights.assign(4u, kShippedFlatFallback);

    AtlasHeightSampler withCoarse{geo, table.Rows.data(), atlas.data(), &coarse};
    AtlasHeightSampler flatFallback{geo, table.Rows.data(), atlas.data(), &flat}; // the shipped 0.5 plane

    const float32 frontierU = 2.0f / static_cast<float32>(tilesPerAxis);
    ASSERT_TRUE(withCoarse.IsResidentAt(frontierU - 1e-4f, 0.5f));  // inside the window: resident Full
    ASSERT_FALSE(withCoarse.IsResidentAt(frontierU + 1e-4f, 0.5f)); // outside: fallback

    uint32 samples = 0, coarseContinuous = 0, flatCracks = 0;
    for (float32 v = 0.05f; v <= 0.95f; v += 0.1f)
    {
        const float32 hInside = withCoarse.SampleHeightNormalized(frontierU - 1e-4f, v); // resident Full
        const float32 hOutsideCoarse = withCoarse.SampleHeightNormalized(frontierU + 1e-4f, v);
        const float32 hOutsideFlat = flatFallback.SampleHeightNormalized(frontierU + 1e-4f, v); // 0.5
        ++samples;
        EXPECT_NEAR(hInside, hOutsideCoarse, 2e-2f) << "frontier cracks WITH the coarse field, v=" << v;
        if (std::abs(hInside - hOutsideCoarse) < 2e-2f)
            ++coarseContinuous;
        if (std::abs(hInside - hOutsideFlat) > 1e-1f)
            ++flatCracks;
    }
    EXPECT_EQ(coarseContinuous, samples) << "coarse field must be continuous at every frontier sample";
    EXPECT_EQ(flatCracks, samples)
        << "the shipped flat 0.5 fallback must crack at every frontier sample (test not discriminating)";
}

// Large-terrain coarse fallback (the "dissolve into a flat sheet at altitude" fix): an out-of-window
// tile that never streamed must resolve to the whole-terrain BASE relief (continuous with resident
// tiles, same deterministic source) rather than a flat 0.5 plane. At her 400-tile / 36-slot scale
// only a fraction of tiles ever stream, so the coarse field is almost entirely fallback — the flat
// 0.5 is exactly what made the far field collapse to a sheet. Oracle: with nothing streamed and a
// base field supplied, every coarse texel carries the base relief; the SAME build with
// baseField==nullptr flattens them to the fallback constant. Passing nullptr is the falsifiable
// divergence (the shipped-before behaviour).
TEST(AtlasSampler, Risk3_CoarseBaseFieldReplacesFlatForUnstreamedTiles)
{
    constexpr uint32 tilesPerAxis = 4;
    constexpr uint32 cd = kAtlasCoarseFieldDim;

    // Whole-terrain base relief, clearly off the flat fallback (0.5) so the divergence is unambiguous.
    std::vector<float32> baseField(static_cast<size_t>(cd) * cd);
    const float32 invCd = cd > 1u ? 1.0f / static_cast<float32>(cd - 1u) : 0.0f;
    for (uint32 z = 0; z < cd; ++z)
        for (uint32 x = 0; x < cd; ++x)
            baseField[static_cast<size_t>(z) * cd + x] =
                0.30f + 0.40f * (static_cast<float32>(x) * invCd) +
                0.15f * std::sin(static_cast<float32>(z) * invCd * 6.0f);

    // Nothing streamed — a large terrain viewed from altitude, far tiles never loaded.
    auto noTiles = [](int32, int32) -> CoarseTileSource { return {}; };

    std::vector<float32> withBase, flat;
    BuildCoarseHeightField(cd, tilesPerAxis, tilesPerAxis, /*fallback*/ 0.5f, noTiles, withBase,
                           baseField.data());
    BuildCoarseHeightField(cd, tilesPerAxis, tilesPerAxis, /*fallback*/ 0.5f, noTiles, flat,
                           /*baseField*/ nullptr);

    ASSERT_EQ(withBase.size(), baseField.size());
    ASSERT_EQ(flat.size(), baseField.size());

    uint32 followsBase = 0, flatCracks = 0, total = 0;
    for (size_t i = 0; i < baseField.size(); ++i)
    {
        ++total;
        if (std::abs(withBase[i] - baseField[i]) < 1e-6f)
            ++followsBase; // base build keeps real relief where no tile streamed
        if (std::abs(flat[i] - 0.5f) < 1e-6f && std::abs(baseField[i] - 0.5f) > 1e-2f)
            ++flatCracks;  // the SAME texel collapses to the flat fallback with no base field
    }
    EXPECT_EQ(followsBase, total) << "unstreamed coarse texels must carry the whole-terrain base relief";
    EXPECT_GT(flatCracks, total * 3u / 4u)
        << "with no base field the SAME texels flatten to 0.5 (the large-terrain dissolve)";
}

// Resident-window slot budget scales with the terrain's tile grid, capped by a VRAM budget
// (DeriveAtlasSlotCount). The historical fixed 36 slots left a large terrain (her 400-tile 10240 m
// world) a small resident detail island in a coarse sea; deriving the budget from the tile count
// grows the window with the world while the square-padded atlas VRAM stays within the budget.
TEST(TerrainAtlasSlotBudget, ScalesWithTileGridCappedByVram)
{
    // A world smaller than the VRAM budget is fully resident: every tile gets a slot.
    EXPECT_EQ(DeriveAtlasSlotCount(4, 4, /*tileRes*/ 1025, kAtlasVramBudgetBytes), 16u);
    EXPECT_EQ(DeriveAtlasSlotCount(6, 6, 1025, kAtlasVramBudgetBytes), 36u);

    // Her config: 20x20 = 400 tiles at 1025-res. Well past the old fixed 36, never above the tile
    // count, and the square-padded footprint stays within the budget.
    const uint32 hers = DeriveAtlasSlotCount(20, 20, 1025, kAtlasVramBudgetBytes);
    EXPECT_GT(hers, 36u) << "a large terrain must get a bigger resident window than the old fixed 36";
    EXPECT_LE(hers, 400u) << "never more than the terrain's tile count";

    const uint64 slotStride = 1025ull + 2ull * kAtlasApron;
    const uint64 bytesPerSlot = slotStride * slotStride * kAtlasSlotBytesPerTexel;
    auto squarePadded = [](uint32 s) {
        uint32 r = static_cast<uint32>(std::ceil(std::sqrt(static_cast<double>(s))));
        while (r * r < s)
            ++r;
        return static_cast<uint64>(r) * r;
    };
    EXPECT_LE(squarePadded(hers) * bytesPerSlot, kAtlasVramBudgetBytes)
        << "square-padded atlas VRAM must stay within the budget (VRAM-honest cap)";

    // Monotonic in world size (up to the cap): a bigger world never yields a smaller window.
    EXPECT_GE(DeriveAtlasSlotCount(20, 20, 1025, kAtlasVramBudgetBytes),
              DeriveAtlasSlotCount(8, 8, 1025, kAtlasVramBudgetBytes));

    // The #508 VRAM lever is real: a smaller budget windows fewer slots.
    EXPECT_LT(DeriveAtlasSlotCount(20, 20, 1025, kAtlasVramBudgetBytes / 4u),
              DeriveAtlasSlotCount(20, 20, 1025, kAtlasVramBudgetBytes));

    // Bigger tiles cost more VRAM per slot, so the same budget windows no more of them.
    EXPECT_LE(DeriveAtlasSlotCount(20, 20, 2049, kAtlasVramBudgetBytes),
              DeriveAtlasSlotCount(20, 20, 1025, kAtlasVramBudgetBytes));
}

// ---------------------------------------------------------------------------
// Render publication — "did the terrain reach the renderer", which no other surface answered.
//
// The ECS component carries live handles from provisioning, which happens BEFORE extraction
// decides whether it can render the terrain, so component-derived "active" is true for a terrain
// that draws nothing. get_terrain_stats reported HEALTHY on exactly that state.
// ---------------------------------------------------------------------------
// DISCRIMINATOR for the counting filter. Extraction's query excludes disabled rows, so a
// hierarchy-disabled terrain is never visited and never published. Counting it as live made the
// reconciliation report "extraction skipped it" for a terrain nothing skipped — a fabricated
// fault produced by one disable, on the instrument whose entire purpose is telling the truth
// about render state.
TEST(TerrainRenderPublication, ADisabledTerrainIsNotCountedLiveSoNoFaultIsFabricated)
{
    ECS::World world;

    Components::Terrain live{};
    live.TiledTerrainHandle = 7;
    live.TiledTerrainGeneration = 1;
    world.Create<Components::Terrain>(live);

    EXPECT_EQ(CountLiveTerrains(world), 1u);

    // A second terrain, identical but disabled through the hierarchy.
    const ECS::EntityHandle disabled = world.CreateEntity();
    world.AddComponentImmediate(disabled, live);
    world.AddComponentImmediate(disabled, ECS::Disabled{});

    EXPECT_EQ(CountLiveTerrains(world), 1u)
        << "the disabled terrain must not count: extraction never visits it";
    EXPECT_EQ(DiagnoseTerrainPublication(CountLiveTerrains(world), /*published*/ 1),
              TerrainPublicationState::Published)
        << "one enabled terrain published = healthy; counting the disabled one reports a skip "
           "that never happened";
}

// A switched-off Terrain component is not the same thing as the hierarchy disable, and the query
// the count shares with extraction has to skip both.
TEST(TerrainRenderPublication, AComponentDisabledTerrainIsAlsoNotCountedLive)
{
    ECS::World world;
    Components::Terrain off{};
    off.TiledTerrainHandle = 3;
    world.Create<Components::Terrain, ECS::ComponentDisabled<Components::Terrain>>(off, {});

    EXPECT_EQ(CountLiveTerrains(world), 0u);
    EXPECT_EQ(DiagnoseTerrainPublication(CountLiveTerrains(world), 0),
              TerrainPublicationState::NoTerrain);
}

// A terrain with no handle at all has not been provisioned yet; it is not a skip either.
TEST(TerrainRenderPublication, AHandlelessTerrainIsNotCountedLive)
{
    ECS::World world;
    Components::Terrain fresh{};
    world.Create<Components::Terrain>(fresh);

    EXPECT_EQ(CountLiveTerrains(world), 0u);
}

TEST(TerrainRenderPublication, LiveTerrainThatReachedNoRendererIsNotPublished)
{
    EXPECT_EQ(DiagnoseTerrainPublication(/*live*/ 1, /*published*/ 0),
              TerrainPublicationState::AllSkipped);
    EXPECT_EQ(DiagnoseTerrainPublication(1, 1), TerrainPublicationState::Published);
}

TEST(TerrainRenderPublication, AnEmptySceneIsNotAFault)
{
    EXPECT_EQ(DiagnoseTerrainPublication(0, 0), TerrainPublicationState::NoTerrain);
}

TEST(TerrainRenderPublication, APartialPublishIsItsOwnStateNotSilentSuccess)
{
    EXPECT_EQ(DiagnoseTerrainPublication(3, 1), TerrainPublicationState::PartiallySkipped);
    EXPECT_NE(DiagnoseTerrainPublication(3, 1), TerrainPublicationState::Published);
}

// ---------------------------------------------------------------------------
// Height-source engagement — which source an authored size + density resolves through.
//
// This used to be an undocumented environment variable that nothing in the tree set, so the
// only reachable behaviour past the unified ceiling was a silent render skip: the terrain
// vanished and the sole signal was a LOG_WARNING. The rule is now derived, and these oracles
// pin BOTH halves of it — where the atlas engages, and that no authored size renders nothing.
// ---------------------------------------------------------------------------

TEST(TerrainHeightSourceRule, BelowThePerTileCapStaysASingleUntiledTerrain)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(1024.0f, 1024.0f, 1.0f);

    EXPECT_EQ(plan.Source, TerrainHeightSource::Single);
    EXPECT_EQ(plan.SampleResolution, 1025u);
    EXPECT_EQ(plan.TotalTiles, 1u);
}

// TerrainService::CreateTiledTerrain consumes this plan's tile geometry, and it is reachable
// with a SUB-CAP config (the tiled-terrain tests call it directly). Tiles are square and sized
// from the SHORTER axis, so the geometry must be derived for a non-tiling terrain too — deriving
// it only on the tiling branch silently resized those tiles to the longer axis.
TEST(TerrainHeightSourceRule, TileGeometryIsDerivedEvenWhenTheTerrainDoesNotTile)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(512.0f, 128.0f, 1.0f);

    ASSERT_EQ(plan.Source, TerrainHeightSource::Single);
    EXPECT_FLOAT_EQ(plan.TileWorldSize, 128.0f)
        << "square tiles take the shorter axis, exactly as CreateTiledTerrain computes them";
    // The single heightmap still resolves from the LONGER axis, which is a different question.
    EXPECT_EQ(plan.SampleResolution, 513u);
}

TEST(TerrainHeightSourceRule, ATiledTerrainUnderTheUnifiedCeilingUsesTheUnifiedSource)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(2048.0f, 2048.0f, 1.0f);

    EXPECT_EQ(plan.Source, TerrainHeightSource::Unified);
    EXPECT_EQ(plan.TilesPerAxisX, 2u);
    EXPECT_EQ(plan.UnifiedWidth, 2049u);
}

// The exact threshold, from both sides. 4096 m at 2 spm derives 8 tiles and a 8193-sample
// unified source — the largest terrain one texture set can carry, and therefore the only size
// at which both sources are viable (the honest A/B point).
TEST(TerrainHeightSourceRule, TheAtlasEngagesExactlyWhereTheUnifiedSourceStops)
{
    const TerrainSizingPlan atCap = DeriveTerrainSizingPlan(4096.0f, 4096.0f, 2.0f);
    EXPECT_EQ(atCap.UnifiedWidth, kMaxUnifiedTiledResolution);
    EXPECT_EQ(atCap.Source, TerrainHeightSource::Unified);

    const TerrainSizingPlan pastCap = DeriveTerrainSizingPlan(4097.0f, 4097.0f, 2.0f);
    EXPECT_GT(pastCap.UnifiedWidth, kMaxUnifiedTiledResolution);
    EXPECT_EQ(pastCap.Source, TerrainHeightSource::Atlas);
}

// THE DEFECT, as an oracle. Every legal size/density an author can dial in must resolve to a
// source that renders. Before the rule this swept range produced terrains that were skipped
// outright above the ceiling.
// Square AND axis-asymmetric extents. A square-only sweep leaves every non-square derivation
// unpinned, which is the exact class the tile-geometry regression hid in.
const std::pair<float32, float32> kAuthorableExtents[] = {
    {64.0f, 64.0f},      {512.0f, 512.0f},    {1024.0f, 1024.0f},   {2048.0f, 2048.0f},
    {4096.0f, 4096.0f},  {4097.0f, 4097.0f},  {8192.0f, 8192.0f},   {16384.0f, 16384.0f},
    {65536.0f, 65536.0f},{2048.0f, 128.0f},   {128.0f, 2048.0f},    {8192.0f, 512.0f},
    {512.0f, 8192.0f},   {16384.0f, 1024.0f}, {1024.0f, 16384.0f},  {1024.5f, 64.0f},
    {64.0f, 1024.5f},    {300.0f, 900.0f},
};

TEST(TerrainHeightSourceRule, NoAuthoredSizeResolvesToNothing)
{
    for (const float32 spm : {0.5f, 1.0f, 2.0f, 4.0f})
        for (const auto& [sx, sz] : kAuthorableExtents)
        {
            const TerrainSizingPlan plan = DeriveTerrainSizingPlan(sx, sz, spm);
            const std::string at = "sx=" + std::to_string(sx) + " sz=" + std::to_string(sz) +
                                   " spm=" + std::to_string(spm);
            EXPECT_GT(plan.SampleResolution, 0u) << at;
            EXPECT_GT(plan.TotalTiles, 0u) << at;
            EXPECT_GT(plan.ResidentSlots, 0u) << at << " resolves to no live source";
            EXPECT_GT(plan.PredictedVramBytes, 0u) << at;
            EXPECT_GT(plan.TileWorldSize, 0.0f) << at;
            EXPECT_GT(plan.MetresPerTexelNear, 0.0f) << at;
            // The tile grid must cover the authored extent on BOTH axes, or a strip of the
            // terrain has no tile to stream and simply is not there.
            EXPECT_GE(static_cast<float32>(plan.TilesPerAxisX) * plan.TileWorldSize, sx) << at;
            EXPECT_GE(static_cast<float32>(plan.TilesPerAxisZ) * plan.TileWorldSize, sz) << at;
        }
}

// The load-bearing fact behind engaging the atlas only at the ceiling rather than always: across
// the ENTIRE range the unified source supports, the atlas would hold every tile at full detail
// simultaneously. The coarse far field never engages below the threshold, so the threshold costs
// no detail — it is not a quality compromise, only a source choice.
TEST(TerrainHeightSourceRule, EveryUnifiedSupportedTerrainWouldBeFullyResidentInTheAtlas)
{
    for (const float32 spm : {0.5f, 1.0f, 2.0f, 4.0f})
        for (float32 size = 128.0f; size <= 32768.0f; size *= 2.0f)
        {
            const TerrainSizingPlan plan = DeriveTerrainSizingPlan(size, size, spm);
            if (plan.Source != TerrainHeightSource::Unified)
                continue;
            const uint32 slots = DeriveAtlasSlotCount(plan.TilesPerAxisX, plan.TilesPerAxisZ,
                                                      plan.TileResolution, kAtlasVramBudgetBytes);
            EXPECT_GE(slots, plan.TotalTiles)
                << "size=" << size << " spm=" << spm << " tiles=" << plan.TotalTiles
                << " would stream rather than hold — the threshold WOULD cost detail here";
        }
}

// Past the ceiling the resident window is a genuine subset, and the far field is the coarse
// whole-terrain source. This is the island-scale limitation, stated as an oracle so a future
// change to the budget or the coarse dim cannot quietly alter what the author was promised.
TEST(TerrainHeightSourceRule, IslandScaleIsPartiallyResidentWithACoarseFarField)
{
    const TerrainSizingPlan plan = DeriveTerrainSizingPlan(8192.0f, 8192.0f, 2.0f);

    EXPECT_EQ(plan.Source, TerrainHeightSource::Atlas);
    EXPECT_EQ(plan.TotalTiles, 256u);
    EXPECT_FALSE(plan.FullyResident());
    EXPECT_LT(plan.MetresPerTexelNear, 1.0f) << "full detail inside the window";
    EXPECT_GT(plan.MetresPerTexelFar, plan.MetresPerTexelNear * 10.0f)
        << "the far field is dramatically coarser — the call the author has to make";
}

// Atlas VRAM is a function of the WINDOW, not the world: past the ceiling, doubling the world
// must not double the footprint. That inversion is the whole reason the atlas exists.
TEST(TerrainHeightSourceRule, AtlasFootprintIsBoundedWhileUnifiedFootprintTracksTheWorld)
{
    const TerrainSizingPlan small = DeriveTerrainSizingPlan(8192.0f, 8192.0f, 2.0f);
    const TerrainSizingPlan huge = DeriveTerrainSizingPlan(65536.0f, 65536.0f, 2.0f);

    ASSERT_EQ(small.Source, TerrainHeightSource::Atlas);
    ASSERT_EQ(huge.Source, TerrainHeightSource::Atlas);
    EXPECT_EQ(huge.PredictedVramBytes, small.PredictedVramBytes)
        << "an 8x larger world must cost the same resident window";
    EXPECT_LE(huge.PredictedVramBytes, kAtlasVramBudgetBytes + (1ull << 20))
        << "and must stay inside the VRAM budget";
}

// ---------------------------------------------------------------------------
// AtlasResidencyController — camera-soak / quiescence / no-leak / wrong-terrain
// ---------------------------------------------------------------------------
TEST(AtlasResidencyController, ParkedCameraIsQuiescent)
{
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 65, /*slots*/ 16, /*framesInFlight*/ 2,
                   /*tilesX*/ 8, /*tilesZ*/ 8, /*logSignals*/ false, kNoFade);

    std::vector<AtlasResidentTile> desired;
    for (int32 tz = 0; tz < 3; ++tz)
        for (int32 tx = 0; tx < 3; ++tx)
            desired.push_back({{tx, tz}, static_cast<float32>(tx * tx + tz * tz), true});

    // First few frames settle residency; afterwards a still camera must be silent.
    for (uint64 f = 1; f <= 6; ++f)
        ctrl.Update(f, kNoFadeDt, desired);

    for (uint64 f = 7; f <= 20; ++f)
    {
        ctrl.Update(f, kNoFadeDt, desired);
        EXPECT_EQ(ctrl.AssignsThisFrame(), 0u) << "assign on a parked frame " << f;
        EXPECT_EQ(ctrl.EvictionsThisFrame(), 0u) << "evict on a parked frame " << f;
        EXPECT_EQ(ctrl.UploadsThisFrame(), 0u) << "upload on a parked frame " << f;
        EXPECT_EQ(ctrl.FallbacksThisFrame(), 0u) << "fallback on a parked frame " << f;
        EXPECT_TRUE(ctrl.RowTransitionsThisFrame().empty()) << "source-flip publish on a parked frame " << f;
    }
    EXPECT_EQ(ctrl.ResidentCount(), 9u);
}

// DISCRIMINATOR (PR #506 R2 — the frontier sky-sliver fix): a residency transition flips a tile's
// effective height SOURCE (slot <-> coarse field), and the extraction path publishes that tile's
// terrain-UV rect so CBT bisectors cached under the old source re-evaluate (the #505 dirty-rect law
// lifted to indirection-row changes). The evict-to-fallback direction is the one the slot-patch
// loops miss (no upload request), so without a RowTransition the evicted tile's corners stay frozen
// at the stale slot height = the scattered sky-slivers. Assert the evicted tile appears in
// RowTransitionsThisFrame (assign publishes one too) and a parked window emits none. DISABLE: drop
// the m_RowTransitions.push_back on the !isResident&&prevResident branch -> the evicted tile is
// absent -> this fails. (The rect->re-eval mechanism this publish then drives — a gated bisector
// re-samples the current source only when its UV rect is published, else it stays frozen — is
// locked device-side by CBTScreenSpaceTest.GatedRegionEditRefreshesOnlyWithDirtyRect.)
TEST(AtlasResidencyController, Risk3_EvictionPublishesRowTransitionForReeval)
{
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 33, /*slots*/ 1, /*framesInFlight*/ 2, /*tilesX*/ 8, /*tilesZ*/ 8, false, kNoFade);
    auto has = [](const std::vector<TileCoord>& v, TileCoord c) {
        return std::find(v.begin(), v.end(), c) != v.end();
    };

    // Frame 1: tile A resident (assign is a coarse->slot transition -> published).
    ctrl.Update(1, kNoFadeDt, {{{0, 0}, 100.0f, true}});
    EXPECT_TRUE(has(ctrl.RowTransitionsThisFrame(), TileCoord{0, 0})) << "assign must publish a transition";
    ASSERT_NE(ctrl.Table().Row(0, 0).Slot, kAtlasNoSlot);

    // Frame 2: parked -> quiescent, no transitions.
    ctrl.Update(2, kNoFadeDt, {{{0, 0}, 100.0f, true}});
    EXPECT_TRUE(ctrl.RowTransitionsThisFrame().empty()) << "a parked window must publish no transition";

    // Frame 3: a NEARER tile B reclaims the single slot; A stays desired but loses its slot (its
    // source flips slot->coarse). B's dist (0) must be < A's recorded dist (100) for the preemption.
    ctrl.Update(3, kNoFadeDt, {{{0, 0}, 100.0f, true}, {{1, 0}, 0.0f, true}});
    EXPECT_EQ(ctrl.Table().Row(0, 0).Slot, kAtlasNoSlot) << "A must lose its slot to nearer B";
    EXPECT_TRUE(has(ctrl.RowTransitionsThisFrame(), TileCoord{0, 0}))
        << "evict-to-fallback must publish the evicted tile — else its bisectors keep the stale slot "
           "height (the frontier sky-slivers)";
}

// Camera-flight soak with continuous eviction + reassignment. Asserts (a) the
// pool never leaks (state partition holds every frame), and (b) the Risk 2
// wrong-terrain guard: a slot's indirection-table tile never changes to a NEW
// tile until >= framesInFlight frames after it last held a different tile.
TEST(AtlasResidencyController, EvictionChurnNoLeakNoWrongTerrain)
{
    constexpr uint32 kSlots = 12;
    constexpr uint32 kFIF = 2;
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 33, kSlots, kFIF, /*tilesX*/ 64, /*tilesZ*/ 64, false, kNoFade);

    std::unordered_map<uint32, TileCoord> slotTile;   // slot -> current table tile
    std::unordered_map<uint32, uint64> slotLastFrame; // slot -> frame it last held a tile

    for (uint64 f = 1; f <= 3000; ++f)
    {
        // A window of 4x4 tiles sliding diagonally across the 64x64 grid.
        const int32 cx = static_cast<int32>((f / 3) % 55);
        const int32 cz = static_cast<int32>((f / 5) % 55);
        std::vector<AtlasResidentTile> desired;
        for (int32 dz = 0; dz < 4; ++dz)
            for (int32 dx = 0; dx < 4; ++dx)
            {
                const int32 tx = cx + dx;
                const int32 tz = cz + dz;
                desired.push_back({{tx, tz}, static_cast<float32>(dx * dx + dz * dz), true});
            }

        ctrl.Update(f, kNoFadeDt, desired);

        const AtlasSlotPool& pool = ctrl.Pool();
        ASSERT_EQ(pool.FreeCount() + pool.ResidentCount() + pool.QuarantinedCount(), pool.SlotCount())
            << "slot-state partition broken at frame " << f;
        ASSERT_LE(pool.ResidentCount(), pool.SlotCount());

        // Walk the indirection table; verify no slot's tile changed to a new tile
        // within the frames-in-flight window.
        const AtlasIndirectionTable& table = ctrl.Table();
        for (const AtlasResidentTile& t : desired)
        {
            const TileAtlasSlot& row = table.Row(t.Coord.X, t.Coord.Z);
            if (row.Slot == kAtlasNoSlot)
                continue;
            auto it = slotTile.find(row.Slot);
            if (it != slotTile.end() && !(it->second == t.Coord))
            {
                const uint64 gap = f - slotLastFrame[row.Slot];
                ASSERT_GE(gap, static_cast<uint64>(kFIF))
                    << "slot " << row.Slot << " reassigned from (" << it->second.X << ","
                    << it->second.Z << ") to (" << t.Coord.X << "," << t.Coord.Z
                    << ") after only " << gap << " frames (< framesInFlight)";
            }
            slotTile[row.Slot] = t.Coord;
            slotLastFrame[row.Slot] = f;
        }
    }
}

// Over-budget: more in-window tiles than slots. The nearest tiles get slots; the
// farthest have no slot (kAtlasNoSlot -> coarse fallback), never a crash/hole.
TEST(AtlasResidencyController, OverBudgetFarthestTilesRenderFallback)
{
    constexpr uint32 kSlots = 4;
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 33, kSlots, /*framesInFlight*/ 2, /*tilesX*/ 8, /*tilesZ*/ 8, false, kNoFade);

    std::vector<AtlasResidentTile> desired;
    // 9 in-window tiles competing for 4 slots; nearer = smaller distance.
    for (int32 tz = 0; tz < 3; ++tz)
        for (int32 tx = 0; tx < 3; ++tx)
            desired.push_back({{tx, tz}, static_cast<float32>(tx * tx + tz * tz), true});

    for (uint64 f = 1; f <= 8; ++f)
        ctrl.Update(f, kNoFadeDt, desired);

    const AtlasIndirectionTable& table = ctrl.Table();
    EXPECT_EQ(ctrl.ResidentCount(), kSlots);
    // The nearest tile (0,0) must be resident; a far corner must be fallback.
    EXPECT_NE(table.Row(0, 0).Slot, kAtlasNoSlot);
    EXPECT_EQ(table.Row(2, 2).Slot, kAtlasNoSlot);
}

// Assign budget: a large resident window that could fill in one frame instead commits at most
// kBudget new slots per frame, bounding the per-frame slot-upload spike (each assign re-uploads a
// full slot — the large-terrain movement hiccup). Deferred tiles stay in the coarse fallback until a
// later frame; over enough frames every wanted tile becomes resident. DISCRIMINATOR: the unbudgeted
// path assigns them ALL in one frame — the upload cliff this bounds.
TEST(AtlasResidencyController, AssignBudgetBoundsPerFrameAssignsAndFillsOverFrames)
{
    constexpr uint32 kSlots = 16;
    constexpr uint32 kBudget = 3;
    std::vector<AtlasResidentTile> desired;
    for (int32 tz = 0; tz < 4; ++tz)
        for (int32 tx = 0; tx < 4; ++tx)
            desired.push_back({{tx, tz}, static_cast<float32>(tx * tx + tz * tz), true}); // 16 tiles

    AtlasResidencyController budgeted;
    budgeted.Configure(/*tileRes*/ 33, kSlots, /*framesInFlight*/ 2, /*tilesX*/ 4, /*tilesZ*/ 4, false, kNoFade);
    budgeted.Update(1, kNoFadeDt, desired, kBudget);
    EXPECT_EQ(budgeted.AssignsThisFrame(), kBudget) << "first frame must not exceed the assign budget";
    EXPECT_EQ(budgeted.ResidentCount(), kBudget);

    for (uint64 f = 2; f <= 16; ++f)
    {
        budgeted.Update(f, kNoFadeDt, desired, kBudget);
        EXPECT_LE(budgeted.AssignsThisFrame(), kBudget) << "no frame exceeds the assign budget";
    }
    EXPECT_EQ(budgeted.ResidentCount(), kSlots) << "every wanted tile eventually becomes resident";

    AtlasResidencyController unbudgeted;
    unbudgeted.Configure(33, kSlots, 2, 4, 4, false, kNoFade);
    unbudgeted.Update(1, kNoFadeDt, desired, kAtlasNoAssignBudget);
    EXPECT_EQ(unbudgeted.AssignsThisFrame(), kSlots)
        << "without a budget the whole window fills in one frame (the upload cliff this bounds)";
}

// DISCRIMINATOR (frame-clock, the m_AtlasFrameCounter fix): the Risk 2 quarantine
// must span framesInFlight REAL frames, so the atlas clock advances ONCE per real
// frame regardless of how many tiled terrains exist. A freed slot's real-frame
// reuse gap is measured under a shared per-real-frame clock (step 1) vs the buggy
// per-terrain clock (step = terrain count) — the buggy clock collapses the window.
TEST(AtlasResidencyController, Risk2_QuarantineIsInRealFramesNotPerTerrainTicks)
{
    constexpr uint32 kFIF = 2;
    // Real frames between a slot's release and its reuse, given a clock that
    // advances by `clockStep` per real frame (1 = shared per-frame; 2 = two
    // terrains each ticking the counter, as the pre-fix code did).
    auto reuseGapRealFrames = [](uint64 clockStep) -> uint32 {
        AtlasResidencyController a;
        a.Configure(/*tileRes*/ 33, /*slots*/ 1, kFIF, /*tilesX*/ 8, /*tilesZ*/ 8, false, kNoFade);
        uint64 frame = 0;
        a.Update(frame += clockStep, kNoFadeDt, {{{0, 0}, 0.0f, true}}); // real frame 1: hold (0,0)
        uint32 realFrame = 1;
        for (;;) // real frames >= 2: want (1,0); (0,0) released at real frame 2
        {
            a.Update(frame += clockStep, kNoFadeDt, {{{1, 0}, 0.0f, true}});
            ++realFrame;
            if (a.Table().Row(1, 0).Slot != kAtlasNoSlot)
                return realFrame - 2; // real frames elapsed since the release
        }
    };
    EXPECT_EQ(reuseGapRealFrames(1), kFIF) << "shared per-real-frame clock must hold the full quarantine";
    EXPECT_LT(reuseGapRealFrames(2), kFIF) << "a per-terrain clock collapses the quarantine (the bug)";
}

// DISCRIMINATOR (the m_AtlasByTiled reuse fix): a destroyed+recreated same-config
// terrain must start from a FRESH controller (the extraction erase), or its tiles
// never re-emit assign/upload signals. A settled controller reused across the
// swap stays silent; a fresh one fires assigns again.
TEST(AtlasResidencyController, FreshControllerAfterTeardownReemitsAssigns)
{
    const std::vector<AtlasResidentTile> desired = {{{0, 0}, 0.0f, true}, {{1, 0}, 1.0f, true}};

    AtlasResidencyController settled;
    settled.Configure(/*tileRes*/ 33, /*slots*/ 16, /*framesInFlight*/ 2, 8, 8, false, kNoFade);
    settled.Update(1, kNoFadeDt, desired);
    EXPECT_GT(settled.AssignsThisFrame(), 0u);
    settled.Update(2, kNoFadeDt, desired);
    EXPECT_EQ(settled.AssignsThisFrame(), 0u) << "should be quiescent once settled";

    // The fix: teardown erases the controller, so the recreated terrain gets a fresh one.
    AtlasResidencyController fresh;
    fresh.Configure(/*tileRes*/ 33, /*slots*/ 16, /*framesInFlight*/ 2, 8, 8, false, kNoFade);
    fresh.Update(3, kNoFadeDt, desired);
    EXPECT_GT(fresh.AssignsThisFrame(), 0u) << "a fresh controller must re-emit assigns for the recreated terrain";

    // The bug: reusing the settled controller across the swap suppresses the signals.
    settled.Update(3, kNoFadeDt, desired);
    EXPECT_EQ(settled.AssignsThisFrame(), 0u) << "a reused (stale) controller stays silent — the m_AtlasByTiled bug";
}

// ---------------------------------------------------------------------------
// Phase E cutover — per-frame upload requests + indirection-table version
// (the GPU-side quiescence gate: an idle frame patches no slot and re-uploads no
// indirection rows — extends the #490 law to the atlas resources).
// ---------------------------------------------------------------------------
TEST(AtlasResidencyController, AssignEmitsUploadRequestAndBumpsTableVersion)
{
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 33, /*slots*/ 16, /*framesInFlight*/ 2, 8, 8, false, kNoFade);
    const uint64 v0 = ctrl.TableVersion();

    ctrl.Update(1, kNoFadeDt, {{{0, 0}, 0.0f, true}, {{1, 0}, 1.0f, true}});
    EXPECT_EQ(ctrl.UploadRequestsThisFrame().size(), 2u) << "each newly assigned slot needs a patch";
    EXPECT_GT(ctrl.TableVersion(), v0) << "a slot assignment changed the rows -> bump";
    // Every request names the slot the indirection row resolves to.
    for (const AtlasUploadRequest& r : ctrl.UploadRequestsThisFrame())
        EXPECT_EQ(ctrl.Table().Row(r.Coord.X, r.Coord.Z).Slot, r.Slot);

    // Parked (same desired set): no patch, no row re-upload.
    const uint64 v1 = ctrl.TableVersion();
    ctrl.Update(2, kNoFadeDt, {{{0, 0}, 0.0f, true}, {{1, 0}, 1.0f, true}});
    EXPECT_TRUE(ctrl.UploadRequestsThisFrame().empty()) << "quiescent frame patches nothing";
    EXPECT_EQ(ctrl.TableVersion(), v1) << "quiescent frame must not bump the row version";
}

// DISCRIMINATOR: an in-place coarse->Full upgrade re-patches the slot CONTENT (an upload
// request) but the slot assignment — the indirection ROW — is unchanged, so the table
// version must NOT bump (else the indirection SSBO is needlessly re-uploaded every upgrade,
// defeating the quiescence gate on a streaming frontier).
TEST(AtlasResidencyController, InPlaceLodUpgradeReUploadsSlotButNotRows)
{
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 33, /*slots*/ 16, /*framesInFlight*/ 2, 8, 8, false, kNoFade);
    ctrl.Update(1, kNoFadeDt, {{{0, 0}, 0.0f, false}}); // coarse
    ASSERT_FALSE(ctrl.UploadRequestsThisFrame().empty());
    const uint32 slot = ctrl.Table().Row(0, 0).Slot;
    const uint64 vAfterAssign = ctrl.TableVersion();

    ctrl.Update(2, kNoFadeDt, {{{0, 0}, 0.0f, true}}); // coarse -> Full, SAME tile/slot
    ASSERT_EQ(ctrl.UploadRequestsThisFrame().size(), 1u) << "the upgrade re-patches the slot content";
    EXPECT_EQ(ctrl.UploadRequestsThisFrame()[0].Slot, slot);
    EXPECT_TRUE(ctrl.UploadRequestsThisFrame()[0].IsFull);
    EXPECT_EQ(ctrl.Table().Row(0, 0).Slot, slot) << "slot unchanged on in-place upgrade";
    EXPECT_EQ(ctrl.TableVersion(), vAfterAssign)
        << "an in-place LOD upgrade must not bump the row version (the row is unchanged)";
}

// The upload-request list and the table version stay flat across a long parked hold — the
// GPU-resource quiescence oracle the orchestrator watches (zero atlas uploads / SSBO writes).
TEST(AtlasResidencyController, UploadsAndTableVersionQuiescentWhenParked)
{
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 65, /*slots*/ 16, /*framesInFlight*/ 2, 8, 8, false, kNoFade);
    std::vector<AtlasResidentTile> desired;
    for (int32 tz = 0; tz < 3; ++tz)
        for (int32 tx = 0; tx < 3; ++tx)
            desired.push_back({{tx, tz}, static_cast<float32>(tx * tx + tz * tz), true});

    for (uint64 f = 1; f <= 6; ++f)
        ctrl.Update(f, kNoFadeDt, desired);
    const uint64 settled = ctrl.TableVersion();
    for (uint64 f = 7; f <= 30; ++f)
    {
        ctrl.Update(f, kNoFadeDt, desired);
        EXPECT_TRUE(ctrl.UploadRequestsThisFrame().empty()) << "atlas upload on parked frame " << f;
        EXPECT_EQ(ctrl.TableVersion(), settled) << "table re-upload on parked frame " << f;
    }
}

// ---------------------------------------------------------------------------
// Coarse->slot upgrade crossfade (frontier-quality lever 1, the residency pop killer). The
// per-tile TileAtlasSlot::Fade the surface mixes coarse<->slot by, ramped CPU-side on assign.
// ---------------------------------------------------------------------------

// The state machine plumbs correctly: an assign starts the tile's row Fade at 0 (still the coarse
// field it rendered a frame ago), it ticks monotonically to 1 over ~fadeSeconds, then the fade
// state clears (settled). DISABLE (fadeSeconds 0) => the row is Fade=1 the instant it assigns (the
// pre-fade instant swap), which is exactly the pop this lever removes.
TEST(AtlasUpgradeCrossfade, RowFadeRampsZeroToOneThenSettles)
{
    constexpr float32 kFadeSeconds = 0.4f;
    constexpr float32 kDt = 0.1f; // 4 ticks to fully fade in
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 33, /*slots*/ 4, /*framesInFlight*/ 2, 8, 8, false, kFadeSeconds);

    const std::vector<AtlasResidentTile> desired = {{{0, 0}, 0.0f, true}};

    // Frame 1: assign. The row is resident but starts fully faded to the coarse field (Fade 0).
    ctrl.Update(1, kDt, desired);
    ASSERT_NE(ctrl.Table().Row(0, 0).Slot, kAtlasNoSlot) << "assigned tile must be resident";
    EXPECT_FLOAT_EQ(ctrl.Table().Row(0, 0).Fade, 0.0f) << "a just-assigned tile renders the coarse field";
    EXPECT_EQ(ctrl.ActiveFadesThisFrame(), 1u) << "the crossfade is active the frame it starts";

    // Frames 2..5: the fade ramps monotonically toward 1, never overshooting.
    float32 prev = ctrl.Table().Row(0, 0).Fade;
    for (uint64 f = 2; f <= 5; ++f)
    {
        ctrl.Update(f, kDt, desired);
        const float32 fade = ctrl.Table().Row(0, 0).Fade;
        EXPECT_GT(fade, prev - 1e-6f) << "Fade must be monotonically non-decreasing at frame " << f;
        EXPECT_LE(fade, 1.0f) << "Fade never exceeds 1 at frame " << f;
        prev = fade;
    }
    // After 4 ticks of 0.25 progress each it has reached full detail.
    EXPECT_FLOAT_EQ(ctrl.Table().Row(0, 0).Fade, 1.0f) << "the tile is fully faded in after ~fadeSeconds";

    // Once settled it does zero fade work — the crossfade ticks ONLY while active (quiescence law).
    ctrl.Update(6, kDt, desired);
    EXPECT_EQ(ctrl.ActiveFadesThisFrame(), 0u) << "a settled tile ticks no crossfade";
    EXPECT_FLOAT_EQ(ctrl.Table().Row(0, 0).Fade, 1.0f) << "a settled resident tile stays at full detail";
}

// DISABLE-AND-FAIL discriminator for the whole lever: with fadeSeconds 0 an assign is an instant
// swap (row Fade == 1 immediately, zero active fades) — the pre-fade behavior. Any nonzero window
// makes the same assign start at 0 instead. This is the runtime kill switch (GE_TERRAIN_ATLAS_FADE=0).
TEST(AtlasUpgradeCrossfade, DisabledFadeSwapsInstantly)
{
    AtlasResidencyController instant;
    instant.Configure(/*tileRes*/ 33, /*slots*/ 4, /*framesInFlight*/ 2, 8, 8, false, /*fadeSeconds*/ 0.0f);
    instant.Update(1, /*dt*/ 0.016f, {{{0, 0}, 0.0f, true}});
    EXPECT_FLOAT_EQ(instant.Table().Row(0, 0).Fade, 1.0f) << "fade disabled => instant full detail on assign";
    EXPECT_EQ(instant.ActiveFadesThisFrame(), 0u) << "fade disabled => no crossfade ever ticks";

    AtlasResidencyController fading;
    fading.Configure(/*tileRes*/ 33, /*slots*/ 4, /*framesInFlight*/ 2, 8, 8, false, /*fadeSeconds*/ 0.4f);
    fading.Update(1, /*dt*/ 0.016f, {{{0, 0}, 0.0f, true}});
    EXPECT_FLOAT_EQ(fading.Table().Row(0, 0).Fade, 0.0f) << "fade enabled => same assign starts at the coarse field";
    EXPECT_EQ(fading.ActiveFadesThisFrame(), 1u);
}

// Count/version quiescence: the crossfade bumps the table version EVERY frame it ticks (the row's
// Fade changed, so the SSBO must re-upload) and is otherwise flat. A parked+settled camera bumps
// nothing (the #490 law). DISABLE: drop the m_ActiveFadesThisFrame term from the version bump ->
// the ramping rows never re-upload -> the GPU freezes the fade at 0 (the tile is stuck coarse).
TEST(AtlasUpgradeCrossfade, TableVersionTicksWhileFadingThenGoesQuiescent)
{
    constexpr float32 kFadeSeconds = 0.4f;
    constexpr float32 kDt = 0.1f;
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 33, /*slots*/ 4, /*framesInFlight*/ 2, 8, 8, false, kFadeSeconds);
    const std::vector<AtlasResidentTile> desired = {{{0, 0}, 0.0f, true}};

    ctrl.Update(1, kDt, desired); // assign
    uint64 prevVersion = ctrl.TableVersion();
    // Frames 2..5: while the fade ramps, every frame is a row-content change -> a version bump.
    for (uint64 f = 2; f <= 5; ++f)
    {
        ctrl.Update(f, kDt, desired);
        EXPECT_GT(ctrl.TableVersion(), prevVersion) << "a ramping crossfade must re-upload the rows at frame " << f;
        prevVersion = ctrl.TableVersion();
    }
    // Settled: the version freezes and the fade counter is flat — parked = zero work.
    const uint64 settledVersion = ctrl.TableVersion();
    for (uint64 f = 6; f <= 20; ++f)
    {
        ctrl.Update(f, kDt, desired);
        EXPECT_EQ(ctrl.TableVersion(), settledVersion) << "a settled+parked camera re-uploads no rows at frame " << f;
        EXPECT_EQ(ctrl.ActiveFadesThisFrame(), 0u) << "no crossfade ticks once settled at frame " << f;
        EXPECT_EQ(ctrl.AssignsThisFrame(), 0u);
        EXPECT_EQ(ctrl.EvictionsThisFrame(), 0u);
    }
}

// Endpoint-exactness is orthogonal to the crossfade: mixing coarse<->slot cannot introduce a NEW
// shared-edge divergence, because a resident tile's Fade never leaks across a tile boundary (it is a
// per-tile uniform, one value for the whole tile). A settled resident tile carries Fade==1, so the
// row the samplers read is byte-identical to the pre-fade row — the #498 shared-edge oracles above
// (SharedEdgeBitEqualBetweenAdjacentFullSlots) sample settled rows and stay green unchanged. Here we
// pin the invariant the samplers rely on: a resident row's Fade is exactly 1 once settled, and an
// out-of-window row is left at its cleared default (also 1 -> the shader forces pure-coarse anyway).
TEST(AtlasUpgradeCrossfade, SettledResidentFadeIsExactlyOneAndClearedRowsDefaultOne)
{
    constexpr float32 kFadeSeconds = 0.4f;
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 33, /*slots*/ 4, /*framesInFlight*/ 2, 8, 8, false, kFadeSeconds);
    const std::vector<AtlasResidentTile> desired = {{{0, 0}, 0.0f, true}, {{1, 0}, 1.0f, true}};
    for (uint64 f = 1; f <= 20; ++f) // settle the fades with a large dt
        ctrl.Update(f, /*dt*/ 1.0f, desired);

    EXPECT_FLOAT_EQ(ctrl.Table().Row(0, 0).Fade, 1.0f);
    EXPECT_FLOAT_EQ(ctrl.Table().Row(1, 0).Fade, 1.0f);
    // An out-of-window tile (never desired) keeps the cleared-row default (1.0); the resolve forces
    // pure coarse for it regardless, so a spurious < 1 here could never fade a non-resident tile.
    EXPECT_EQ(ctrl.Table().Row(2, 2).Slot, kAtlasNoSlot);
    EXPECT_FLOAT_EQ(ctrl.Table().Row(2, 2).Fade, 1.0f);
}

// An eviction drops the crossfade state (upgrade-only fade this slice): the tile's row goes back to
// the coarse field with no lingering Fade, and re-assigning it later restarts the fade from 0 (it
// re-arrives from the coarse field, not from a stale mid-fade value).
TEST(AtlasUpgradeCrossfade, EvictionClearsFadeAndReassignRestartsFromZero)
{
    constexpr float32 kFadeSeconds = 0.4f;
    constexpr float32 kDt = 0.1f;
    AtlasResidencyController ctrl;
    ctrl.Configure(/*tileRes*/ 33, /*slots*/ 1, /*framesInFlight*/ 2, 8, 8, false, kFadeSeconds);

    ctrl.Update(1, kDt, {{{0, 0}, 100.0f, true}}); // assign A, fade starts at 0
    ctrl.Update(2, kDt, {{{0, 0}, 100.0f, true}}); // A ramps a little
    EXPECT_GT(ctrl.Table().Row(0, 0).Fade, 0.0f);

    // A nearer tile B reclaims the single slot; A loses its slot -> coarse, its crossfade is dropped.
    // B does NOT become resident this frame: A's slot is quarantined for framesInFlight frames (Risk 2),
    // so B renders the coarse field (no slot) until the slot clears — the reassign then starts fresh.
    const std::vector<AtlasResidentTile> aAndB = {{{0, 0}, 100.0f, true}, {{1, 0}, 0.0f, true}};
    ctrl.Update(3, kDt, aAndB);
    EXPECT_EQ(ctrl.Table().Row(0, 0).Slot, kAtlasNoSlot) << "A evicted to coarse (crossfade dropped)";

    // Advance until B finally gets the quarantined slot; the FIRST frame it is resident its fade must
    // start at the coarse field (0) — a fresh arrival, never inheriting A's mid-fade value.
    bool sawBResident = false;
    for (uint64 f = 4; f <= 10 && !sawBResident; ++f)
    {
        ctrl.Update(f, kDt, aAndB);
        if (ctrl.Table().Row(1, 0).Slot != kAtlasNoSlot)
        {
            sawBResident = true;
            EXPECT_FLOAT_EQ(ctrl.Table().Row(1, 0).Fade, 0.0f)
                << "the reclaiming tile fades in from the coarse field afresh, not from A's stale fade";
        }
    }
    EXPECT_TRUE(sawBResident) << "B must eventually acquire the quarantined slot";
}

// ---------------------------------------------------------------------------
// Streaming integration-queue priority (frontier-quality lever 3): the visible frontier upgrades
// first. VERIFICATION oracle — the ordering already exists (TileStreamingPriority::operator<); this
// pins the contract the dispatch/integration std::sort relies on so a regression is caught.
// ---------------------------------------------------------------------------

// At EQUAL distance, an in-frustum tile is strictly higher priority than an out-of-frustum one
// (operator< is a strict-weak "lower = sooner"): the camera-facing frontier streams before the tiles
// behind the camera. DISABLE: drop the InFrustum branch from operator< -> equal-distance tiles tie on
// distance and the in-front tile no longer wins -> this fails.
TEST(TileStreamingPriorityOrder, InFrustumBeatsOutOfFrustumAtEqualDistance)
{
    TileStreamingPriority inFront{};
    inFront.DistanceSq = 100.0f;
    inFront.InFrustum = true;

    TileStreamingPriority behind{};
    behind.DistanceSq = 100.0f; // SAME distance
    behind.InFrustum = false;

    EXPECT_TRUE(inFront < behind) << "at equal distance the in-frustum tile must sort first (upgrade first)";
    EXPECT_FALSE(behind < inFront) << "the out-of-frustum tile must never outrank the in-frustum one at equal distance";

    // The frustum test dominates distance: a FARTHER in-frustum tile still beats a NEARER behind-camera
    // tile, so the visible frontier is never starved by closer geometry behind the camera.
    TileStreamingPriority farInFront{};
    farInFront.DistanceSq = 10000.0f;
    farInFront.InFrustum = true;
    TileStreamingPriority nearBehind{};
    nearBehind.DistanceSq = 1.0f;
    nearBehind.InFrustum = false;
    EXPECT_TRUE(farInFront < nearBehind) << "in-frustum priority dominates distance — the visible frontier leads";

    // Within the same frustum class, nearer still wins (distance is the tiebreak after frustum + upgrade).
    TileStreamingPriority near{};
    near.DistanceSq = 25.0f;
    near.InFrustum = true;
    TileStreamingPriority far{};
    far.DistanceSq = 400.0f;
    far.InFrustum = true;
    EXPECT_TRUE(near < far) << "same frustum class => nearer tile streams first";
}

// LH MakeLookAtLH view row 2 is camera forward. Identity / look +Z must report
// +Z so tiles along +Z get InFrustum (the previous -row2 fork boosted -Z).
TEST(TilePriorityViewDirection, IdentityViewLooksAlongPlusZ)
{
    float view[16] = {};
    view[0] = view[5] = view[10] = view[15] = 1.0f;
    const Mathematics::Vector3 d = TilePriorityViewDirectionXZ(view);
    EXPECT_NEAR(d.x, 0.0f, 1e-6f);
    EXPECT_NEAR(d.y, 0.0f, 1e-6f);
    EXPECT_NEAR(d.z, 1.0f, 1e-6f);
}

TEST(TilePriorityViewDirection, LookAtLhTowardPlusZScoresPlusZTileInFront)
{
    using GameEngine::Mathematics::MakeLookAtLH;
    using GameEngine::Mathematics::Matrix4x4;
    using GameEngine::Mathematics::Vector3;
    const Vector3 eye(0.0f, 10.0f, 0.0f);
    const Matrix4x4 view = MakeLookAtLH(eye, eye + Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 1.0f, 0.0f));
    const Vector3 d = TilePriorityViewDirectionXZ(view.Data());
    EXPECT_GT(d.z, 0.0f);
    EXPECT_TRUE(TileIsInFrontOfCameraXZ(0.0f, 50.0f, eye.x, eye.z, d));
    EXPECT_FALSE(TileIsInFrontOfCameraXZ(0.0f, -50.0f, eye.x, eye.z, d));
}

// ---------------------------------------------------------------------------
// Multi-component slot sources (splat = 4, normal = 2) — quality-sweep slice 1.
//
// Splat/normal ride the SAME slot geometry as height (same apron, same edge/corner
// ownership, same indirection rows), differing only in components-per-texel. These
// oracles re-run the height sampling-parity / shared-edge / mixed-LOD-corner /
// out-of-window contracts over the generalized multi-component machinery, so the
// per-pixel splat + normal atlas resolve the surface shader runs is proven the same
// way the height resolve is. The GLSL resolve (CBT_AtlasResolve -> slotUV) is byte-
// shared with the height path and already device-proven by
// CBTScreenSpaceTest.AtlasResolveSamplesAtlasNotUnifiedHeight; these lock the payload.
// ---------------------------------------------------------------------------
namespace
{
// A deterministic, smooth per-GLOBAL-sample componentsPerTexel-vector so adjacent tiles
// share their boundary samples exactly (endpoint-exact, like the world-space source).
void WorldTexelN(int32 gx, int32 gz, uint32 components, float32* out)
{
    for (uint32 c = 0; c < components; ++c)
        out[c] = 0.5f + 0.35f * std::sin((gx + 7 * static_cast<int32>(c)) * 0.23f) *
                            std::cos((gz - 5 * static_cast<int32>(c)) * 0.19f);
}

std::vector<float32> MakeFullTileN(uint32 tileRes, int32 tx, int32 tz, uint32 components)
{
    std::vector<float32> d(static_cast<size_t>(tileRes) * tileRes * components);
    const int32 interior = static_cast<int32>(tileRes) - 1;
    for (uint32 z = 0; z < tileRes; ++z)
        for (uint32 x = 0; x < tileRes; ++x)
            WorldTexelN(tx * interior + static_cast<int32>(x), tz * interior + static_cast<int32>(z),
                        components, &d[(static_cast<size_t>(z) * tileRes + x) * components]);
    return d;
}

// A deliberately-different coarse approximation (a per-channel ramp) so its shared edge
// genuinely diverges from a Full neighbour's — the mixed-LOD seam the yield rule closes.
std::vector<float32> MakeCoarseTileN(uint32 tileRes, uint32 components)
{
    std::vector<float32> d(static_cast<size_t>(tileRes) * tileRes * components);
    for (uint32 z = 0; z < tileRes; ++z)
        for (uint32 x = 0; x < tileRes; ++x)
            for (uint32 c = 0; c < components; ++c)
                d[(static_cast<size_t>(z) * tileRes + x) * components + c] =
                    0.15f * static_cast<float32>(c + 1) +
                    0.55f * (static_cast<float32>(x) / static_cast<float32>(tileRes - 1));
    return d;
}
} // namespace

TEST(AtlasComponentSampler, ParityWithDirectTileSampleAtGridPoints)
{
    for (uint32 components : {2u, 4u}) // normal, splat
    {
        const uint32 tileRes = 9;
        const AtlasGeometry geo = MakeAtlasGeometry(tileRes, /*slots*/ 4, /*tilesX*/ 2, /*tilesZ*/ 2);
        std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim * components, 0.0f);
        AtlasIndirectionTable table;
        table.Resize(geo);
        uint32 slot = 0;
        for (int32 tz = 0; tz < 2; ++tz)
            for (int32 tx = 0; tx < 2; ++tx)
            {
                const std::vector<float32> t = MakeFullTileN(tileRes, tx, tz, components);
                PackTileComponentsIntoSlot(atlas.data(), geo, slot, t.data(), components,
                                           /*tileIsFull*/ true, AtlasTileNeighbors{}, AtlasNeighborEdgesN{});
                TileAtlasSlot& row = table.Row(tx, tz);
                row.Slot = slot;
                row.Generation = 1;
                ++slot;
            }
        AtlasComponentSampler s{geo, table.Rows.data(), atlas.data(), components, nullptr};

        const int32 interior = static_cast<int32>(tileRes) - 1;
        const int32 globalW = interior * 2 + 1;
        std::vector<float32> got(components), expected(components);
        for (int32 gz = 0; gz < globalW; ++gz)
            for (int32 gx = 0; gx < globalW; ++gx)
            {
                const float32 u = static_cast<float32>(gx) / static_cast<float32>(globalW - 1);
                const float32 v = static_cast<float32>(gz) / static_cast<float32>(globalW - 1);
                s.Sample(u, v, got.data());
                WorldTexelN(gx, gz, components, expected.data());
                for (uint32 c = 0; c < components; ++c)
                    EXPECT_FLOAT_EQ(got[c], expected[c])
                        << "components=" << components << " comp " << c << " grid (" << gx << "," << gz << ")";
            }
    }
}

TEST(AtlasComponentSampler, SharedEdgeBitEqualBetweenAdjacentFullSlots)
{
    for (uint32 components : {2u, 4u})
    {
        const uint32 tileRes = 9;
        const AtlasGeometry geo = MakeAtlasGeometry(tileRes, 4, 2, 2);
        std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim * components, 0.0f);
        uint32 slot = 0;
        for (int32 tz = 0; tz < 2; ++tz)
            for (int32 tx = 0; tx < 2; ++tx)
            {
                const std::vector<float32> t = MakeFullTileN(tileRes, tx, tz, components);
                PackTileComponentsIntoSlot(atlas.data(), geo, slot++, t.data(), components, true,
                                           AtlasTileNeighbors{}, AtlasNeighborEdgesN{});
            }
        // Tile (0,0) slot 0 +X edge == tile (1,0) slot 1 -X edge, per component, bit-equal.
        std::vector<float32> rightOf00, leftOf10;
        ReadSlotInteriorEdgeN(atlas.data(), geo, 0, AtlasEdgeSide::PosX, components, rightOf00);
        ReadSlotInteriorEdgeN(atlas.data(), geo, 1, AtlasEdgeSide::NegX, components, leftOf10);
        ASSERT_EQ(rightOf00.size(), leftOf10.size());
        ASSERT_EQ(rightOf00.size(), static_cast<size_t>(tileRes) * components);
        for (size_t i = 0; i < rightOf00.size(); ++i)
            EXPECT_FLOAT_EQ(rightOf00[i], leftOf10[i])
                << "components=" << components << " shared-edge element " << i;
    }
}

// DISCRIMINATOR: a coarse tile beside a Full neighbour must copy the Full edge (all
// components) into its own slot. Disable the yield (default AtlasTileNeighbors{}) and the
// coarse approximation diverges — the mixed-LOD splat/normal seam.
TEST(AtlasComponentEdgeOwnership, Risk1_CoarseSlotSharedEdgeMatchesFullNeighbor)
{
    for (uint32 components : {2u, 4u})
    {
        const uint32 tileRes = 9;
        const AtlasGeometry geo = MakeAtlasGeometry(tileRes, 2, 2, 1);
        std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim * components, 0.0f);

        const std::vector<float32> full = MakeFullTileN(tileRes, /*tx*/ 1, /*tz*/ 0, components);
        PackTileComponentsIntoSlot(atlas.data(), geo, /*slot*/ 1, full.data(), components, true,
                                   AtlasTileNeighbors{}, AtlasNeighborEdgesN{});
        std::vector<float32> fullLeftEdge;
        ReadSlotInteriorEdgeN(atlas.data(), geo, 1, AtlasEdgeSide::NegX, components, fullLeftEdge);

        const std::vector<float32> coarse = MakeCoarseTileN(tileRes, components);

        AtlasTileNeighbors nb{};
        nb.RightResident = true;
        nb.RightFull = true;
        AtlasNeighborEdgesN edges{};
        edges.Right = fullLeftEdge;
        PackTileComponentsIntoSlot(atlas.data(), geo, /*slot*/ 0, coarse.data(), components, false, nb, edges);
        std::vector<float32> coarseRightEdge;
        ReadSlotInteriorEdgeN(atlas.data(), geo, 0, AtlasEdgeSide::PosX, components, coarseRightEdge);
        ASSERT_EQ(coarseRightEdge.size(), fullLeftEdge.size());
        for (size_t i = 0; i < coarseRightEdge.size(); ++i)
            EXPECT_FLOAT_EQ(coarseRightEdge[i], fullLeftEdge[i]) << "components=" << components << " element " << i;

        // WITHOUT the mitigation: diverges (test is discriminating).
        PackTileComponentsIntoSlot(atlas.data(), geo, /*slot*/ 0, coarse.data(), components, false,
                                   AtlasTileNeighbors{}, AtlasNeighborEdgesN{});
        std::vector<float32> coarseRightNoYield;
        ReadSlotInteriorEdgeN(atlas.data(), geo, 0, AtlasEdgeSide::PosX, components, coarseRightNoYield);
        uint32 diffs = 0;
        for (size_t i = 0; i < coarseRightNoYield.size(); ++i)
            if (std::abs(coarseRightNoYield[i] - fullLeftEdge[i]) > 1e-4f)
                ++diffs;
        EXPECT_GT(diffs, 0u) << "components=" << components << " edge should diverge without the yield";
    }
}

// DISCRIMINATOR (4-tile mixed-LOD corner): a coarse tile DIAGONAL to the only Full tile
// learns the shared corner from no axis edge — only the corner-yield rule reconciles it.
// Disable the yield and the copy diverges (per component).
TEST(AtlasComponentEdgeOwnership, Risk1_FourTileCornerBitEqual)
{
    for (uint32 components : {2u, 4u})
    {
        const uint32 tileRes = 9;
        const AtlasGeometry geo = MakeAtlasGeometry(tileRes, 4, 2, 2);
        std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim * components, 0.0f);
        const uint32 sA = 0, sC = 3; // A=(0,0) coarse, C=(1,1) Full; corner A.BR == C.TL

        const std::vector<float32> cFull = MakeFullTileN(tileRes, 1, 1, components);
        const std::vector<float32> coarse = MakeCoarseTileN(tileRes, components);
        PackTileComponentsIntoSlot(atlas.data(), geo, sC, cFull.data(), components, true,
                                   AtlasTileNeighbors{}, AtlasNeighborEdgesN{});
        std::vector<float32> cTop;
        ReadSlotInteriorEdgeN(atlas.data(), geo, sC, AtlasEdgeSide::NegZ, components, cTop); // C.TL = element 0
        std::vector<float32> cornerAuthoritative(cTop.begin(), cTop.begin() + components);

        auto texel = [&](uint32 slot, uint32 lx, uint32 lz, uint32 c) {
            return atlas[(static_cast<size_t>(geo.SlotInteriorTexelY(slot) + lz) * geo.AtlasDim +
                          (geo.SlotInteriorTexelX(slot) + lx)) * components + c];
        };
        const uint32 last = tileRes - 1u;

        AtlasCornerYieldsN cornersA{};
        cornersA.BottomRight = {true, cornerAuthoritative};
        PackTileComponentsIntoSlot(atlas.data(), geo, sA, coarse.data(), components, false,
                                   AtlasTileNeighbors{}, AtlasNeighborEdgesN{}, cornersA);
        for (uint32 c = 0; c < components; ++c)
            EXPECT_FLOAT_EQ(texel(sA, last, last, c), cornerAuthoritative[c])
                << "components=" << components << " comp " << c;

        // WITHOUT A's corner yield: diverges.
        PackTileComponentsIntoSlot(atlas.data(), geo, sA, coarse.data(), components, false,
                                   AtlasTileNeighbors{}, AtlasNeighborEdgesN{});
        uint32 diffs = 0;
        for (uint32 c = 0; c < components; ++c)
            if (std::abs(texel(sA, last, last, c) - cornerAuthoritative[c]) > 1e-4f)
                ++diffs;
        EXPECT_GT(diffs, 0u) << "components=" << components << " corner should diverge without the rule";
    }
}

// DISCRIMINATOR: a non-resident tile resolves to the coarse field (per component), never
// garbage. With Coarse=nullptr the fallback returns zeros — the discriminating divergence.
TEST(AtlasComponentSampler, Risk3_NonResidentFallsBackToCoarseNotGarbage)
{
    for (uint32 components : {2u, 4u})
    {
        const AtlasGeometry geo = MakeAtlasGeometry(9, 4, 4, 4);
        AtlasIndirectionTable table;
        table.Resize(geo);
        table.Clear(); // none resident

        AtlasCoarseFieldN coarse;
        coarse.Dim = 16;
        coarse.Components = components;
        coarse.Data.resize(static_cast<size_t>(coarse.Dim) * coarse.Dim * components);
        for (uint32 z = 0; z < coarse.Dim; ++z)
            for (uint32 x = 0; x < coarse.Dim; ++x)
                for (uint32 c = 0; c < components; ++c)
                    coarse.Data[(static_cast<size_t>(z) * coarse.Dim + x) * components + c] =
                        0.4f + 0.2f * std::sin((x + c) * 0.5f) + 0.1f * std::cos(z * 0.4f);

        AtlasComponentSampler withCoarse{geo, table.Rows.data(), nullptr, components, &coarse};
        AtlasComponentSampler noCoarse{geo, table.Rows.data(), nullptr, components, nullptr};

        std::vector<float32> got(components), gotNo(components);
        for (float32 v = 0.05f; v < 1.0f; v += 0.2f)
            for (float32 u = 0.05f; u < 1.0f; u += 0.2f)
            {
                withCoarse.Sample(u, v, got.data());
                noCoarse.Sample(u, v, gotNo.data());
                for (uint32 c = 0; c < components; ++c)
                {
                    const float32 expected = SampleGridBilinearTexelN(
                        coarse.Data.data(), coarse.Dim, coarse.Dim, components, c,
                        u * static_cast<float32>(coarse.Dim - 1), v * static_cast<float32>(coarse.Dim - 1));
                    EXPECT_FLOAT_EQ(got[c], expected);
                    EXPECT_FLOAT_EQ(gotNo[c], 0.0f); // disabled fallback -> zeros (discriminator)
                    EXPECT_NE(expected, 0.0f);
                }
            }
    }
}

// The coarse splat field is a whole-terrain downsample built with the SAME UV->tile mapping the
// atlas resolve uses, so the out-of-window fallback is continuous with resident splat at the
// streaming-window frontier (no band). Mirror of the height Risk3 frontier oracle, multi-component.
TEST(AtlasComponentField, FrontierContinuousWithResident)
{
    constexpr uint32 components = 4; // splat
    constexpr uint32 tileRes = 17;
    constexpr uint32 tilesPerAxis = 4;

    std::vector<std::vector<float32>> tiles(static_cast<size_t>(tilesPerAxis) * tilesPerAxis);
    const AtlasGeometry geo = MakeAtlasGeometry(tileRes, 2 * tilesPerAxis, tilesPerAxis, tilesPerAxis);
    std::vector<float32> atlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim * components, 0.0f);
    AtlasIndirectionTable table;
    table.Resize(geo);
    table.Clear();
    uint32 slot = 0;
    for (int32 tz = 0; tz < static_cast<int32>(tilesPerAxis); ++tz)
        for (int32 tx = 0; tx < static_cast<int32>(tilesPerAxis); ++tx)
        {
            tiles[static_cast<size_t>(tz) * tilesPerAxis + tx] = MakeFullTileN(tileRes, tx, tz, components);
            if (tx <= 1) // resident window
            {
                PackTileComponentsIntoSlot(atlas.data(), geo, slot,
                                           tiles[static_cast<size_t>(tz) * tilesPerAxis + tx].data(),
                                           components, true, AtlasTileNeighbors{}, AtlasNeighborEdgesN{});
                TileAtlasSlot& row = table.Row(tx, tz);
                row.Slot = slot;
                row.Generation = 1;
                ++slot;
            }
        }

    AtlasCoarseFieldN coarse;
    coarse.Dim = kAtlasCoarseFieldDim;
    coarse.Components = components;
    const std::vector<float32> fallback(components, 0.5f);
    BuildCoarseComponentField(coarse.Dim, tilesPerAxis, tilesPerAxis, components, fallback.data(),
                              [&](int32 tx, int32 tz) -> CoarseTileSourceN {
                                  const auto& t = tiles[static_cast<size_t>(tz) * tilesPerAxis + tx];
                                  return {t.data(), tileRes, tileRes, components};
                              },
                              coarse.Data);
    ASSERT_TRUE(coarse.IsValid());

    AtlasComponentSampler s{geo, table.Rows.data(), atlas.data(), components, &coarse};
    const float32 frontierU = 2.0f / static_cast<float32>(tilesPerAxis);
    ASSERT_TRUE(s.IsResidentAt(frontierU - 1e-4f, 0.5f));
    ASSERT_FALSE(s.IsResidentAt(frontierU + 1e-4f, 0.5f));

    std::vector<float32> inside(components), outside(components);
    uint32 samples = 0, continuous = 0;
    for (float32 v = 0.05f; v <= 0.95f; v += 0.1f)
    {
        s.Sample(frontierU - 1e-4f, v, inside.data());
        s.Sample(frontierU + 1e-4f, v, outside.data());
        ++samples;
        bool ok = true;
        for (uint32 c = 0; c < components; ++c)
            if (std::abs(inside[c] - outside[c]) > 3e-2f)
                ok = false;
        if (ok)
            ++continuous;
    }
    EXPECT_EQ(continuous, samples) << "coarse splat field must be continuous at the frontier";
}

// LOCK: the extraction packs splat/normal slots with the BYTE packer PackTileBytesIntoSlot for speed
// (no float<->byte round-trip). With empty neighbours (the extraction's pack) it must be byte-for-byte
// PackTileComponentsIntoSlot over the same data reinterpreted as bytes — so the byte tool inherits the
// full float oracle suite (parity / shared-edge / apron). Runs for 1/2/4 components (height/normal/splat).
TEST(AtlasBytePacker, ByteSlotPackMatchesFloatPackEmptyNeighbors)
{
    for (uint32 components : {1u, 2u, 4u})
    {
        const uint32 tileRes = 9;
        const AtlasGeometry geo = MakeAtlasGeometry(tileRes, 4, 2, 2);
        const std::vector<float32> tile = MakeFullTileN(tileRes, 1, 0, components);

        std::vector<float32> floatAtlas(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim * components, 0.0f);
        PackTileComponentsIntoSlot(floatAtlas.data(), geo, /*slot*/ 2, tile.data(), components, true,
                                   AtlasTileNeighbors{}, AtlasNeighborEdgesN{});

        std::vector<uint8> byteAtlas(floatAtlas.size() * sizeof(float32), 0);
        PackTileBytesIntoSlot(byteAtlas.data(), geo, /*slot*/ 2,
                              reinterpret_cast<const uint8*>(tile.data()),
                              components * static_cast<uint32>(sizeof(float32)));
        ASSERT_EQ(byteAtlas.size(), floatAtlas.size() * sizeof(float32));
        EXPECT_EQ(0, std::memcmp(byteAtlas.data(), floatAtlas.data(), byteAtlas.size()))
            << "components=" << components << " byte pack diverged from float pack";
    }
}

// LOCK: the coarse SPLAT field is built by the byte downsampler BuildCoarseFieldU8; it must match the
// oracle-proven float BuildCoarseComponentField within uint8 rounding (+/-1), so its frontier continuity
// (proven for the float builder above) carries to the shipped byte path.
TEST(AtlasBytePacker, CoarseSplatByteMatchesFloatWithinRounding)
{
    constexpr uint32 channels = 4, coarseDim = 16, tilesX = 3, tilesZ = 3, tileRes = 9;
    std::vector<std::vector<uint8>> tilesU8(static_cast<size_t>(tilesX) * tilesZ);
    std::vector<std::vector<float32>> tilesF(static_cast<size_t>(tilesX) * tilesZ);
    for (int32 tz = 0; tz < static_cast<int32>(tilesZ); ++tz)
        for (int32 tx = 0; tx < static_cast<int32>(tilesX); ++tx)
        {
            auto& u8 = tilesU8[static_cast<size_t>(tz) * tilesX + tx];
            auto& f = tilesF[static_cast<size_t>(tz) * tilesX + tx];
            u8.resize(static_cast<size_t>(tileRes) * tileRes * channels);
            f.resize(u8.size());
            for (uint32 i = 0; i < tileRes * tileRes; ++i)
                for (uint32 c = 0; c < channels; ++c)
                {
                    const uint8 v = static_cast<uint8>((i * 7u + c * 53u + tx * 11 + tz * 17) & 0xFFu);
                    u8[i * channels + c] = v;
                    f[i * channels + c] = static_cast<float32>(v) / 255.0f;
                }
        }
    std::vector<uint8> outU8;
    const uint8 fbU8[4] = {128, 128, 128, 128};
    BuildCoarseFieldU8(coarseDim, tilesX, tilesZ, channels, fbU8,
                       [&](int32 tx, int32 tz) -> CoarseTileSourceU8 {
                           const auto& u8 = tilesU8[static_cast<size_t>(tz) * tilesX + tx];
                           return {u8.data(), tileRes, tileRes, channels};
                       },
                       outU8);
    std::vector<float32> outF;
    const float32 fbF[4] = {128 / 255.0f, 128 / 255.0f, 128 / 255.0f, 128 / 255.0f};
    BuildCoarseComponentField(coarseDim, tilesX, tilesZ, channels, fbF,
                              [&](int32 tx, int32 tz) -> CoarseTileSourceN {
                                  const auto& f = tilesF[static_cast<size_t>(tz) * tilesX + tx];
                                  return {f.data(), tileRes, tileRes, channels};
                              },
                              outF);
    ASSERT_EQ(outU8.size(), outF.size());
    for (size_t i = 0; i < outU8.size(); ++i)
        EXPECT_LE(std::abs(static_cast<int32>(outU8[i]) - static_cast<int32>(std::lround(outF[i] * 255.0f))), 1)
            << "coarse element " << i;
}

// DISCRIMINATOR (R1, splat-paint visibility): HEIGHT and SPLAT are independent dirty channels. An
// interactive paint bake sets ONLY SplatmapDirty; the pre-fix atlas loop handled only the height
// channel, so a splat-only tile routed to nothing and the paint was INVISIBLE on atlas terrains. The
// pure routing function must send a splat-only edit down the splat channel (resident -> re-upload the
// splat slot; non-resident -> rebuild the coarse splat), never None.
TEST(AtlasTileDirtyRoute, SplatOnlyEditRoutesToSplatChannelNotIgnored)
{
    using R = AtlasTileDirtyRoute;
    // The fix: splat-only edits reach the splat channel.
    EXPECT_EQ(ResolveAtlasTileDirtyRoute(/*height*/ false, /*splat*/ true, /*resident*/ true),
              R::ResidentSplatOnly);
    EXPECT_EQ(ResolveAtlasTileDirtyRoute(false, true, false), R::NonResidentSplatOnly);
    // Height edits still re-patch all three; height dominates a combined edit (full re-patch covers splat).
    EXPECT_EQ(ResolveAtlasTileDirtyRoute(true, false, true), R::ResidentFull);
    EXPECT_EQ(ResolveAtlasTileDirtyRoute(true, true, true), R::ResidentFull);
    EXPECT_EQ(ResolveAtlasTileDirtyRoute(true, false, false), R::NonResidentHeight);
    EXPECT_EQ(ResolveAtlasTileDirtyRoute(false, false, true), R::None);
    // The falsifiable core: a splat-only resident edit must NOT collapse to None (the pre-fix bug that
    // made paint invisible). A regression that drops the splat channel fails here.
    EXPECT_NE(ResolveAtlasTileDirtyRoute(false, true, true), R::None);
    EXPECT_NE(ResolveAtlasTileDirtyRoute(false, true, false), R::None);
}

// ---------------------------------------------------------------------------
// Grass-on-atlas placement (grass-on-atlas-terrains slice)
//
// The grass placement compute (terrain_grass_place.comp) snaps each blade's RootY through the
// SAME resident-window resolve CBT displaces the surface with: rootY = originY + h01(u,v)*scale,
// where h01 comes from the atlas (resident slot at full precision, coarse field out of window).
// These oracles lock that CPU-mirrored math at texel centres + slot edges, prove the atlas path is
// load-bearing (disabling it collapses roots to the base plane — the float/bury bug the #508 gate
// guarded against), and pin the compute->vertex-modifier normal handoff + the un-gate builder.
// ---------------------------------------------------------------------------
namespace
{
// CPU mirror of the GLSL grass rootY: originY + normalized atlas height * heightScale.
float32 GrassRootY(const AtlasHeightSampler& s, float32 u, float32 v,
                   float32 originY, float32 heightScale)
{
    return originY + s.SampleHeightNormalized(u, v) * heightScale;
}
} // namespace

TEST(AtlasGrassPlacement, RootSnapsToSurfaceAtTexelCentersAndSlotEdges)
{
    const uint32 tileRes = 9;
    const float32 originY = 40.0f;
    const float32 heightScale = 256.0f;
    BuiltAtlas b = Build2x2FullAtlas(tileRes);
    AtlasHeightSampler s{b.Geo, b.Table.Rows.data(), b.Atlas.data(), nullptr};

    const int32 interior = static_cast<int32>(tileRes) - 1;
    const int32 globalW = interior * 2 + 1; // 17
    // Texel centres (every stitched grid sample) — bilinear collapses, so the root sits EXACTLY on
    // the surface the CBT compute displaces to. A blade here neither floats nor buries.
    for (int32 gz = 0; gz < globalW; ++gz)
        for (int32 gx = 0; gx < globalW; ++gx)
        {
            const float32 u = static_cast<float32>(gx) / static_cast<float32>(globalW - 1);
            const float32 v = static_cast<float32>(gz) / static_cast<float32>(globalW - 1);
            const float32 surface = originY + WorldHeight(gx, gz) * heightScale;
            EXPECT_FLOAT_EQ(GrassRootY(s, u, v, originY, heightScale), surface)
                << "grid (" << gx << "," << gz << ")";
        }

    // Slot edge: the boundary UV between tile 0 and tile 1 is served by two disjoint slots whose
    // shared edge is bit-equal, so a blade straddling the seam snaps continuously (no seam crack).
    const float32 boundaryU = 0.5f;
    for (float32 v = 0.0f; v <= 1.0f; v += 0.1f)
    {
        const float32 rootAt = GrassRootY(s, boundaryU, v, originY, heightScale);
        const float32 rootLeft = GrassRootY(s, boundaryU - 1e-4f, v, originY, heightScale);
        EXPECT_NEAR(rootAt, rootLeft, heightScale * 1e-3f) << "seam v=" << v;
    }
}

TEST(AtlasGrassPlacement, RootFloatsWhenAtlasResolveDisabled)
{
    // Disable the atlas resolve (every row NO_SLOT, no coarse) — exactly the pre-slice state that
    // made the #508 gate disable grass: h01 collapses to 0, so rootY freezes on the base plane
    // while the CBT surface still has real relief. The blades would float over valleys and bury
    // under peaks. This is the disable-and-fail that proves the resolve is load-bearing.
    const uint32 tileRes = 9;
    const float32 originY = 40.0f;
    const float32 heightScale = 256.0f;
    BuiltAtlas b = Build2x2FullAtlas(tileRes);
    AtlasHeightSampler resolved{b.Geo, b.Table.Rows.data(), b.Atlas.data(), nullptr};

    AtlasIndirectionTable disabled;
    disabled.Resize(b.Geo);
    disabled.Clear(); // every row -> kAtlasNoSlot
    AtlasHeightSampler collapsed{b.Geo, disabled.Rows.data(), b.Atlas.data(), nullptr};

    const int32 interior = static_cast<int32>(tileRes) - 1;
    const int32 globalW = interior * 2 + 1;
    float32 maxErr = 0.0f;
    for (int32 gz = 0; gz < globalW; ++gz)
        for (int32 gx = 0; gx < globalW; ++gx)
        {
            const float32 u = static_cast<float32>(gx) / static_cast<float32>(globalW - 1);
            const float32 v = static_cast<float32>(gz) / static_cast<float32>(globalW - 1);
            // Collapsed root is the flat base plane; resolved root is on the surface.
            EXPECT_FLOAT_EQ(GrassRootY(collapsed, u, v, originY, heightScale), originY);
            maxErr = std::max(maxErr,
                std::abs(GrassRootY(resolved, u, v, originY, heightScale)
                         - GrassRootY(collapsed, u, v, originY, heightScale)));
        }
    // The relief the disabled path drops is tens of metres (0.25 amplitude * 256 scale = 64 m band):
    // far past any tolerable float/bury. If this ever shrank to ~0, the atlas resolve is a no-op.
    EXPECT_GT(maxErr, 10.0f);
}

TEST(AtlasGrassPlacement, OutOfWindowRootUsesCoarseNotGarbage)
{
    // A blade beyond the resident window must resolve to the coarse field (height-continuous with
    // resident relief), never garbage/base-plane. Near-camera grass never reaches here by
    // construction (its tiles are resident), so this is the safety net for any spill past the window.
    const uint32 tileRes = 9;
    const float32 originY = 12.0f;
    const float32 heightScale = 200.0f;
    AtlasGeometry geo = MakeAtlasGeometry(tileRes, /*slots*/ 4, /*tilesX*/ 4, /*tilesZ*/ 4);
    AtlasIndirectionTable table;
    table.Resize(geo);
    table.Clear(); // NOTHING resident -> every tile takes the coarse fallback

    AtlasCoarseField coarse;
    coarse.Dim = 8;
    coarse.Heights.assign(static_cast<size_t>(coarse.Dim) * coarse.Dim, 0.0f);
    for (uint32 z = 0; z < coarse.Dim; ++z)
        for (uint32 x = 0; x < coarse.Dim; ++x)
            coarse.Heights[static_cast<size_t>(z) * coarse.Dim + x] =
                0.3f + 0.4f * (static_cast<float32>(x) / static_cast<float32>(coarse.Dim - 1));

    AtlasHeightSampler withCoarse{geo, table.Rows.data(), nullptr, &coarse};
    AtlasHeightSampler noCoarse{geo, table.Rows.data(), nullptr, nullptr};
    for (float32 v = 0.05f; v < 1.0f; v += 0.17f)
        for (float32 u = 0.05f; u < 1.0f; u += 0.13f)
        {
            const float32 root = GrassRootY(withCoarse, u, v, originY, heightScale);
            // Coarse root lands within the terrain's height band (originY .. originY+scale), never NaN
            // or a wild value. Without any coarse field it is the base plane (never garbage either).
            EXPECT_GE(root, originY - 1.0f);
            EXPECT_LE(root, originY + heightScale + 1.0f);
            EXPECT_FLOAT_EQ(GrassRootY(noCoarse, u, v, originY, heightScale), originY);
        }
}

TEST(AtlasGrassPlacement, NormalPackReconstructRoundTrips)
{
    // The compute bakes the terrain normal as (nx, nz) into the blade instance; the vertex modifier
    // reconstructs ny = sqrt(1 - nx^2 - nz^2). Terrain surface normals always point up (ny >= 0), so
    // the round-trip is exact. A regression that stored ny instead of nx (axis swap) fails here.
    const float32 kNormals[][3] = {
        {0.0f, 1.0f, 0.0f}, {0.2f, 0.9539f, 0.22f}, {-0.35f, 0.9f, 0.25f}, {0.5f, 0.7071f, 0.5f},
    };
    for (const auto& n : kNormals)
    {
        const float32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float32 nx = n[0] / len, ny = n[1] / len, nz = n[2] / len;
        const float32 rny = std::sqrt(std::max(1.0f - nx * nx - nz * nz, 0.0f));
        EXPECT_NEAR(rny, ny, 1e-4f);
        EXPECT_NEAR(std::sqrt(nx * nx + rny * rny + nz * nz), 1.0f, 1e-5f);
    }
}

TEST(AtlasGrassParams, BuiltFromValidSourceEnablesGrassWithHeightSource)
{
    // Un-gate oracle: an atlas source produces params with Enabled != 0 AND a valid height source,
    // so the extraction leaves grass ENABLED (the #508 gate is gone). RowCount is clamped to the
    // ring capacity the shader indexes.
    using Grass = GameEngine::TerrainGrass::TerrainGrassRenderFeature;
    AtlasGrassSource src{};
    src.Valid = true;
    src.AtlasDim = 2048;
    src.AtlasSlotStride = 1027;
    src.AtlasSlotsPerRow = 2;
    src.AtlasTileRes = 1025;
    src.AtlasTilesPerAxisX = 6;
    src.AtlasTilesPerAxisZ = 6;
    src.AtlasCoarseDim = 64;
    src.HeightBindless = 17;
    src.HeightCoarseBindless = 18;
    src.NormalBindless = 19;
    src.SplatBindless = 20;
    src.RowCount = 36;

    const Grass::GrassAtlasParamsGPU p = Grass::BuildAtlasParams(src, /*rowCapacity*/ 36);
    EXPECT_NE(p.Enabled, 0u);
    EXPECT_NE(p.HeightBindless, 0u); // a valid height source exists -> grass will snap, not float
    EXPECT_EQ(p.TileRes, 1025u);
    EXPECT_EQ(p.TilesPerAxisX, 6u);
    EXPECT_EQ(p.RowCount, 36u);

    // RowCount is clamped to the ring capacity so the shader never reads past its bound rows.
    const Grass::GrassAtlasParamsGPU clamped = Grass::BuildAtlasParams(src, /*rowCapacity*/ 16);
    EXPECT_EQ(clamped.RowCount, 16u);
}

TEST(AtlasGrassParams, InvalidSourceDisablesAtlasPath)
{
    // No atlas terrain this frame -> Enabled 0, so the compute never takes the atlas path (and the
    // default 1-row NO_SLOT buffer keeps binding 5 valid). Guards the no-atlas / non-atlas terrains.
    using Grass = GameEngine::TerrainGrass::TerrainGrassRenderFeature;
    AtlasGrassSource src{}; // Valid == false
    const Grass::GrassAtlasParamsGPU p = Grass::BuildAtlasParams(src, /*rowCapacity*/ 36);
    EXPECT_EQ(p.Enabled, 0u);
    EXPECT_EQ(p.RowCount, 0u);
}

namespace
{
// Mirror of the splat-masked grass-placement gate in terrain_grass_place.comp (kept in lockstep):
// with a splatmap bound, a candidate places only where grass weight >= maskThreshold AND grass leads
// the next-strongest layer by >= dominanceFloor = min(0.18, maskThreshold). Before the fix the floor
// was a hardcoded 0.18 independent of maskThreshold, so a blended-but-grass splat (grass is the
// plurality, but not by 0.18) placed nothing however low the user set MaskThreshold — the tiled-grass
// invisibility on procedurally-blended splats.
bool GrassGatePassesSplat(float32 grassWeight, float32 otherMax, float32 maskThreshold)
{
    const float32 dominanceFloor = std::min(0.18f, maskThreshold);
    const float32 dominantGrass = grassWeight - otherMax;
    return !(grassWeight < maskThreshold || dominantGrass < dominanceFloor);
}
} // namespace

TEST(GrassSplatBake, GrassIsPluralityWhereSurfaceReadsGrassy)
{
    // The user-facing invariant: "terrain looks grassy -> grass can grow there". The grass
    // placement compute grows grass only where grass is the plurality splat layer, but the surface
    // shades a BLEND of all four layer colours — so gently-sloped ground can read grassy (grass is
    // a big green contributor) while a different layer holds the weight plurality, and no blades
    // place. Shading and placement must agree.
    //
    // Measured against the SHIPPED DEFAULT RULES over the unbaked base, which is what a new terrain
    // actually carries — so this asserts the agreement a user actually gets. Terrain shape is the
    // coordinator's repro: 1536 m with HeightScale-60 relief.
    using namespace GameEngine::TerrainECS;
    const uint32 dim = 129;
    const float32 worldSize = 1536.0f;
    const float32 heightScale = 60.0f;
    Terrain::HeightfieldData hf(dim, dim, 0.0f);
    hf.FillWithNoise(/*frequency*/ 2.5f, /*amplitude*/ heightScale, /*octaves*/ 4, /*seed*/ 1337);

    std::vector<uint8> splat;
    uint32 sw = 0, sh = 0;
    ResetSplatmap(hf, splat, sw, sh);
    ASSERT_EQ(splat.size(), static_cast<size_t>(dim) * dim * 4);

    // Bake the default rows exactly as ApplySplatModifiers does: measure the texel once, then
    // composite every row in authored order through the byte quantization.
    {
        const float32 spacing = worldSize / static_cast<float32>(dim - 1);
        float32 minH = 1e30f, maxH = -1e30f;
        for (uint32 z = 0; z < dim; ++z)
            for (uint32 x = 0; x < dim; ++x)
            {
                const float32 v = hf.GetSample(x, z);
                minH = std::min(minH, v);
                maxH = std::max(maxH, v);
            }
        const float32 range = std::max(maxH - minH, 0.001f);
        const auto rules = MakeDefaultTerrainSurfaceRules();
        for (uint32 z = 0; z < dim; ++z)
            for (uint32 x = 0; x < dim; ++x)
            {
                const auto n = hf.ComputeNormal(static_cast<int32>(x), static_cast<int32>(z),
                                                spacing, spacing);
                const TerrainRuleSample sample = MakeTerrainRuleSample(
                    n.x, n.y, n.z, hf.GetSample(x, z), heightScale, minH, range,
                    static_cast<float32>(x) * spacing, static_cast<float32>(z) * spacing);
                uint8* pixel = &splat[(static_cast<size_t>(z) * dim + x) * 4];
                for (uint32 r = 0; r < rules.RuleCount; ++r)
                {
                    const float32 wgt = EvaluateTerrainSurfaceRuleWeight(
                        rules.Rules[r], sample, SurfaceRuleNoiseSample);
                    if (wgt > 0.0f)
                        CompositeSplatTexel(pixel, rules.Rules[r].MaterialSlot, wgt,
                                            rules.Rules[r].Replace);
                }
            }
    }

    const float32 spacingX = worldSize / static_cast<float32>(dim - 1);
    uint32 grassPlurality = 0, rockPlurality = 0, dirtPlurality = 0, snowPlurality = 0, total = 0;
    // "Grassy" ground: gently sloped, low altitude, with grass a big colour contributor — exactly the
    // ground the user reads as grass. Count how much of it grass does NOT hold the plurality on.
    uint32 grassyTexels = 0, grassyButNotGrassPlurality = 0;
    for (uint32 z = 1; z + 1 < dim; ++z)
        for (uint32 x = 1; x + 1 < dim; ++x)
        {
            const size_t idx = (static_cast<size_t>(z) * dim + x) * 4;
            const float32 g = splat[idx] / 255.0f;
            const float32 r = splat[idx + 1] / 255.0f;
            const float32 d = splat[idx + 2] / 255.0f;
            const float32 s = splat[idx + 3] / 255.0f;
            const auto normal = hf.ComputeNormal(static_cast<int32>(x), static_cast<int32>(z),
                                                 spacingX, spacingX);
            const float32 slope = 1.0f - std::max(normal.y, 0.0f);

            ++total;
            int plurality = 0;
            float32 maxw = g;
            if (r > maxw) { maxw = r; plurality = 1; }
            if (d > maxw) { maxw = d; plurality = 2; }
            if (s > maxw) { maxw = s; plurality = 3; }
            grassPlurality += (plurality == 0);
            rockPlurality += (plurality == 1);
            dirtPlurality += (plurality == 2);
            snowPlurality += (plurality == 3);

            if (slope < 0.30f && g > 0.30f)
            {
                ++grassyTexels;
                if (plurality != 0)
                    ++grassyButNotGrassPlurality;
            }
        }

    auto pct = [](uint32 n, uint32 t) -> uint32 { return t ? n * 100u / t : 0u; };
    std::printf("[SplatBake] total=%u grass=%u%% rock=%u%% dirt=%u%% snow=%u%% | grassy=%u "
                "grassy-but-not-grass=%u (%u%%)\n",
                total, pct(grassPlurality, total), pct(rockPlurality, total),
                pct(dirtPlurality, total), pct(snowPlurality, total),
                grassyTexels, grassyButNotGrassPlurality, pct(grassyButNotGrassPlurality, grassyTexels));

    // The invariant + disable-and-fail: gently-sloped grass-tinted ground must be grass-plurality
    // (so grass can grow where the surface reads grassy).
    ASSERT_GT(grassyTexels, 0u) << "the noise terrain produced no gentle grass-tinted ground to test";
    ASSERT_GT(grassPlurality + rockPlurality + dirtPlurality + snowPlurality, 0u)
        << "the default rows placed nothing - every measure below would be vacuous";
    EXPECT_LT(pct(grassyButNotGrassPlurality, grassyTexels), 10u)
        << "grass-tinted gentle ground where grass is NOT the plurality: surface reads grass, "
           "placement rejects it (shading<->placement disagreement)";
}

TEST(GrassPlacementGate, BlendedGrassPlacesUnderLowThresholdButStrictByDefault)
{
    // A blended-but-grass splat: grass is the plurality (0.5) but leads the next layer (0.4) by only
    // 0.10 — the surface shades grassy, but the OLD hardcoded 0.18 floor rejected it at ANY threshold.
    const float32 grass = 0.5f, otherMax = 0.4f;

    // Default MaskThreshold (0.45): the floor stays 0.18, so this is still rejected — byte-identical
    // to the pre-fix behavior, so nothing that worked before changes.
    EXPECT_FALSE(GrassGatePassesSplat(grass, otherMax, /*maskThreshold*/ 0.45f));

    // A LOW MaskThreshold (the user asking for liberal grass, as in the repro): the floor drops to
    // 0.02, so the blended-but-grass splat now places. This is the fix — the disable-and-fail core:
    // revert dominanceFloor to a hardcoded 0.18 and this flips back to rejected.
    EXPECT_TRUE(GrassGatePassesSplat(grass, otherMax, /*maskThreshold*/ 0.02f));

    // Grass genuinely absent (snow/rock dominant): rejected at any threshold — grass is NOT the
    // plurality, so no-grass stays correct (the floor fix never forces grass onto non-grass terrain).
    EXPECT_FALSE(GrassGatePassesSplat(/*grass*/ 0.1f, /*otherMax*/ 0.8f, 0.02f));

    // Grass strongly dominant (the untiled-that-works shape): placed at the default threshold,
    // unchanged by the fix.
    EXPECT_TRUE(GrassGatePassesSplat(/*grass*/ 0.9f, /*otherMax*/ 0.05f, 0.45f));
}

// ---------------------------------------------------------------------------
// Region (sub-rect) slot patch — editing real-time part 2 (#526 remainder)
//
// A brush dab changed only a small rect of a resident Full tile. The extraction now regenerates +
// re-uploads ONLY that rect (+ the filter footprint / apron) instead of a whole-tile normal regen +
// whole-slot (slotStride^2) re-upload per dab. These oracles lock: (1) the region pack is byte-for-
// byte the whole-slot pack (apron duplicated at tile edges); (2) a settled dab sequence leaves the
// slot byte-identical to the whole path, on REAL divergent tiles; (3) the boundary-normal seam
// (#508 class) stays bit-equal across slots at region granularity.
// ---------------------------------------------------------------------------
namespace
{
// Copy a tightly-packed region (from PackTileEditRegionIntoSlot) into a CPU mirror of the atlas
// texture — the CPU stand-in for the render feature's content-preserving region upload.
void ApplyPackedRegion(uint8* atlas, uint32 atlasDim, uint32 bytesPerTexel,
                       const std::vector<uint8>& region, const AtlasSlotUploadRect& r)
{
    for (uint32 y = 0; y < r.Height; ++y)
        for (uint32 x = 0; x < r.Width; ++x)
        {
            const size_t dst =
                (static_cast<size_t>(r.DstTexelY + y) * atlasDim + (r.DstTexelX + x)) * bytesPerTexel;
            const size_t src = (static_cast<size_t>(y) * r.Width + x) * bytesPerTexel;
            std::memcpy(atlas + dst, &region[src], bytesPerTexel);
        }
}

// A "real" divergent tile: world-space fractal noise (the engine's actual tile source), NOT a smooth
// analytic function — so the byte-identity proof is payload-honest, not a symmetry artefact.
Terrain::HeightfieldData MakeNoiseTile(uint32 tileRes, float32 tileWorld, float32 originX,
                                       float32 originZ, uint32 seed)
{
    Terrain::HeightfieldData hf(tileRes, tileRes);
    hf.FillWithNoiseWorldSpace(0.01f, 1.0f, originX, originZ, tileWorld, tileWorld, 5, seed);
    return hf;
}
} // namespace

// (1) The region pack writes byte-for-byte what a whole-slot pack writes — for an interior rect AND
// an edge rect (whose apron gutter must duplicate the interior edge). Method: whole-pack a slot, then
// re-apply a region pack of the SAME tile over a copy; the copy must be UNCHANGED (identical bytes),
// and the returned rect must include the apron exactly when the region touches the tile edge.
TEST(AtlasRegionPatch, PackTileEditRegionByteEqualsWholeSlotPackWithApron)
{
    constexpr uint32 tileRes = 17;
    const AtlasGeometry geo = MakeAtlasGeometry(tileRes, /*slots*/ 4, /*tilesX*/ 2, /*tilesZ*/ 2);
    const uint32 slot = 1; // a non-zero slot to exercise the origin offset
    const int32 last = static_cast<int32>(tileRes) - 1;

    const Terrain::HeightfieldData hf = MakeNoiseTile(tileRes, 512.0f, 0.0f, 0.0f, 7);
    std::vector<uint8> whole(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim * sizeof(float32), 0u);
    PackTileHeightIntoSlot(reinterpret_cast<float32*>(whole.data()), geo, slot, hf.GetRawSamples(),
                           /*tileIsFull*/ true, AtlasTileNeighbors{}, AtlasNeighborEdges{});

    const auto* heightBytes = reinterpret_cast<const uint8*>(hf.GetRawSamples());

    // Interior rect: no apron extension. Re-applying it must not change a single byte.
    {
        std::vector<uint8> copy = whole;
        std::vector<uint8> scratch;
        const AtlasSlotUploadRect r =
            PackTileEditRegionIntoSlot(scratch, heightBytes, sizeof(float32), geo, slot, 4, 5, 9, 8);
        EXPECT_EQ(r.Width, 6u);  // 4..9 inclusive
        EXPECT_EQ(r.Height, 4u); // 5..8 inclusive
        EXPECT_EQ(r.DstTexelX, geo.SlotInteriorTexelX(slot) + 4u);
        EXPECT_EQ(r.DstTexelY, geo.SlotInteriorTexelY(slot) + 5u);
        ApplyPackedRegion(copy.data(), geo.AtlasDim, sizeof(float32), scratch, r);
        EXPECT_EQ(0, std::memcmp(copy.data(), whole.data(), whole.size()))
            << "interior region pack diverged from the whole-slot pack";
    }

    // Corner rect touching (0,0): the slot rect must extend into the top/left apron gutter, and the
    // apron texels must duplicate the interior edge — still byte-identical to the whole pack.
    {
        std::vector<uint8> copy = whole;
        std::vector<uint8> scratch;
        const AtlasSlotUploadRect r =
            PackTileEditRegionIntoSlot(scratch, heightBytes, sizeof(float32), geo, slot, 0, 0, 3, 3);
        EXPECT_EQ(r.DstTexelX, geo.SlotOriginTexelX(slot)) << "left apron column not included";
        EXPECT_EQ(r.DstTexelY, geo.SlotOriginTexelY(slot)) << "top apron row not included";
        EXPECT_EQ(r.Width, 3u + 1u + static_cast<uint32>(kAtlasApron));  // 0..3 + left apron
        ApplyPackedRegion(copy.data(), geo.AtlasDim, sizeof(float32), scratch, r);
        EXPECT_EQ(0, std::memcmp(copy.data(), whole.data(), whole.size()))
            << "edge region pack (apron) diverged from the whole-slot pack";
    }

    // Far corner touching (last,last): extends into the bottom/right apron.
    {
        std::vector<uint8> copy = whole;
        std::vector<uint8> scratch;
        const AtlasSlotUploadRect r = PackTileEditRegionIntoSlot(
            scratch, heightBytes, sizeof(float32), geo, slot, last - 3, last - 3, last, last);
        EXPECT_EQ(r.DstTexelX + r.Width, geo.SlotOriginTexelX(slot) + geo.SlotStride)
            << "right apron column not included";
        EXPECT_EQ(r.DstTexelY + r.Height, geo.SlotOriginTexelY(slot) + geo.SlotStride)
            << "bottom apron row not included";
        ApplyPackedRegion(copy.data(), geo.AtlasDim, sizeof(float32), scratch, r);
        EXPECT_EQ(0, std::memcmp(copy.data(), whole.data(), whole.size()))
            << "far-edge region pack (apron) diverged from the whole-slot pack";
    }
}

namespace
{
// Run the extraction's region flow on a dab rect and assert the settled slot (height + normal)
// is byte-identical to a whole-slot re-pack of the edited tile. Mirrors patchSlotRegion: pad the
// dab by the filter footprint (2), region-regen the persistent normal, and region-upload each map
// over a slot seeded by the pre-dab whole pack.
//
// The splat is NOT one of the maps here. It is a 4-byte-per-texel map packed by the same
// PackTileBytesIntoSlot / PackTileEditRegionIntoSlot / ApplyPackedRegion calls at the same stride
// as the normal map, so it exercised no packing path the normal arm does not; its only distinct
// content was material derived at bake time, which no longer exists; against an unbaked splat the arm
// would have compared zeros to zeros.
void RunAtlasRegionSettleOracle(int32 dx0, int32 dz0, int32 dx1, int32 dz1)
{
    constexpr uint32 tileRes = 17;
    constexpr int32 kPad = 2;
    const AtlasGeometry geo = MakeAtlasGeometry(tileRes, /*slots*/ 4, /*tilesX*/ 2, /*tilesZ*/ 2);
    const uint32 slot = 0;
    const float32 tileWorld = 512.0f, heightScale = 100.0f;
    const int32 last = static_cast<int32>(tileRes) - 1;

    const Terrain::HeightfieldData hf0 = MakeNoiseTile(tileRes, tileWorld, 0.0f, 0.0f, 4242);
    std::vector<uint8> n0; uint32 w = 0, h = 0;
    GenerateNormalmapFromHeightfield(hf0, tileWorld, tileWorld, heightScale, n0, w, h);

    auto packWhole = [&](const Terrain::HeightfieldData& hf, const std::vector<uint8>& n,
                         std::vector<uint8>& hAtlas, std::vector<uint8>& nAtlas) {
        hAtlas.assign(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim * sizeof(float32), 0u);
        nAtlas.assign(static_cast<size_t>(geo.AtlasDim) * geo.AtlasDim * 4u, 0u);
        PackTileHeightIntoSlot(reinterpret_cast<float32*>(hAtlas.data()), geo, slot, hf.GetRawSamples(),
                               true, AtlasTileNeighbors{}, AtlasNeighborEdges{});
        PackTileBytesIntoSlot(nAtlas.data(), geo, slot, n.data(), 4u);
    };

    // Seed = the resident slot before the dab.
    std::vector<uint8> hSeed, nSeed;
    packWhole(hf0, n0, hSeed, nSeed);

    // The dab: raise a mound confined to [dx0..dx1]x[dz0..dz1].
    Terrain::HeightfieldData hf1 = hf0;
    for (int32 z = dz0; z <= dz1; ++z)
        for (int32 x = dx0; x <= dx1; ++x)
            hf1.SetSample(static_cast<uint32>(x), static_cast<uint32>(z),
                          hf1.GetSample(static_cast<uint32>(x), static_cast<uint32>(z)) + 0.5f);

    // Whole path: repack everything from the edited tile.
    std::vector<uint8> n1;
    GenerateNormalmapFromHeightfield(hf1, tileWorld, tileWorld, heightScale, n1, w, h);
    std::vector<uint8> hWhole, nWhole;
    packWhole(hf1, n1, hWhole, nWhole);

    // The dab actually moved the height atlas — the oracle is not vacuous.
    ASSERT_NE(0, std::memcmp(hSeed.data(), hWhole.data(), hWhole.size()));

    // Region path: persistent normal/splat seeded pre-dab, region-regenerated over the padded rect.
    const int32 rx0 = std::max(dx0 - kPad, 0), rz0 = std::max(dz0 - kPad, 0);
    const int32 rx1 = std::min(dx1 + kPad, last), rz1 = std::min(dz1 + kPad, last);
    std::vector<uint8> nR = n0;
    GenerateNormalmapRegionFromHeightfield(hf1, tileWorld, tileWorld, heightScale, nR, rx0, rz0, rx1, rz1);

    std::vector<uint8> hReg = hSeed, nReg = nSeed, scratch;
    const auto* hf1Bytes = reinterpret_cast<const uint8*>(hf1.GetRawSamples());
    AtlasSlotUploadRect rr =
        PackTileEditRegionIntoSlot(scratch, hf1Bytes, sizeof(float32), geo, slot, rx0, rz0, rx1, rz1);
    ApplyPackedRegion(hReg.data(), geo.AtlasDim, sizeof(float32), scratch, rr);
    rr = PackTileEditRegionIntoSlot(scratch, nR.data(), 4u, geo, slot, rx0, rz0, rx1, rz1);
    ApplyPackedRegion(nReg.data(), geo.AtlasDim, 4u, scratch, rr);
    EXPECT_EQ(0, std::memcmp(hReg.data(), hWhole.data(), hWhole.size())) << "height slot not bit-equal";
    EXPECT_EQ(0, std::memcmp(nReg.data(), nWhole.data(), nWhole.size())) << "normal slot not bit-equal";
}
} // namespace

// (2) THE settle oracle: a region dab settles byte-identical to the whole-slot path, for an interior
// dab (no apron) AND a corner dab (apron on two sides). Real divergent noise tile, height + normal.
TEST(AtlasRegionPatch, RegionPatchSettlesToWholeSlotBitEqual)
{
    RunAtlasRegionSettleOracle(/*interior*/ 6, 7, 9, 10);
    RunAtlasRegionSettleOracle(/*corner*/ 0, 0, 3, 3);
    RunAtlasRegionSettleOracle(/*far corner*/ 12, 12, 16, 16);
}

// (3) #508 boundary-normal seam at REGION granularity: a dab straddling two tiles' shared edge must
// leave the shared boundary normal bit-equal across the two disjoint slots. The region regen is
// neighbour-aware (each side passes the other's edited heightfield), so the two copies of the shared
// boundary texel agree. DISCRIMINATOR: drop the neighbours and the boundary normals diverge.
TEST(AtlasRegionPatch, RegionNormalBoundaryBitEqualAcrossSlotsWithNeighbors)
{
    constexpr uint32 tileRes = 17;
    constexpr int32 kPad = 2;
    const float32 tileWorld = 512.0f, heightScale = 100.0f;
    const int32 last = static_cast<int32>(tileRes) - 1;

    // Adjacent tiles: T0 world [0..tileWorld], T1 [tileWorld..2*tileWorld]. Their shared column
    // (T0 x=last == T1 x=0, same world x) is bit-equal by world-space-noise construction.
    Terrain::HeightfieldData t0 = MakeNoiseTile(tileRes, tileWorld, 0.0f, 0.0f, 99);
    Terrain::HeightfieldData t1 = MakeNoiseTile(tileRes, tileWorld, tileWorld, 0.0f, 99);
    for (uint32 z = 0; z < tileRes; ++z)
        ASSERT_FLOAT_EQ(t0.GetSample(tileRes - 1, z), t1.GetSample(0, z)) << "shared column z=" << z;

    // Seed normals, neighbour-aware at the shared edge.
    std::vector<uint8> n0, n1; uint32 w = 0, h = 0;
    GenerateNormalmapFromHeightfield(t0, tileWorld, tileWorld, heightScale, n0, w, h,
                                     nullptr, &t1, nullptr, nullptr);
    GenerateNormalmapFromHeightfield(t1, tileWorld, tileWorld, heightScale, n1, w, h,
                                     &t0, nullptr, nullptr, nullptr);

    // Dab straddling the seam: raise both sides' boundary-adjacent columns by the SAME delta, so the
    // shared column stays bit-equal.
    for (uint32 z = 4; z <= 10; ++z)
    {
        for (int32 x = last - 4; x <= last; ++x)
            t0.SetSample(static_cast<uint32>(x), z, t0.GetSample(static_cast<uint32>(x), z) + 0.5f);
        for (int32 x = 0; x <= 4; ++x)
            t1.SetSample(static_cast<uint32>(x), z, t1.GetSample(static_cast<uint32>(x), z) + 0.5f);
    }
    for (uint32 z = 0; z < tileRes; ++z)
        ASSERT_FLOAT_EQ(t0.GetSample(tileRes - 1, z), t1.GetSample(0, z))
            << "dab broke shared-column equality at z=" << z;

    auto boundaryColumnByte = [&](const std::vector<uint8>& nm, uint32 col) {
        std::vector<uint8> out(static_cast<size_t>(tileRes) * 4u);
        for (uint32 z = 0; z < tileRes; ++z)
            std::memcpy(&out[static_cast<size_t>(z) * 4u],
                        &nm[(static_cast<size_t>(z) * tileRes + col) * 4u], 4u);
        return out;
    };

    // WITH neighbours: region-regen each tile's boundary region, passing the other's edited field.
    {
        std::vector<uint8> nr0 = n0, nr1 = n1;
        GenerateNormalmapRegionFromHeightfield(t0, tileWorld, tileWorld, heightScale, nr0,
                                               last - 4 - kPad, 4 - kPad, last, 10 + kPad,
                                               nullptr, &t1, nullptr, nullptr);
        GenerateNormalmapRegionFromHeightfield(t1, tileWorld, tileWorld, heightScale, nr1,
                                               0, 4 - kPad, 4 + kPad, 10 + kPad,
                                               &t0, nullptr, nullptr, nullptr);
        const std::vector<uint8> edge0 = boundaryColumnByte(nr0, tileRes - 1); // T0 +X edge
        const std::vector<uint8> edge1 = boundaryColumnByte(nr1, 0);           // T1 -X edge
        EXPECT_EQ(0, std::memcmp(edge0.data(), edge1.data(), edge0.size()))
            << "shared boundary normal not bit-equal across slots after a region dab";
    }

    // DISCRIMINATOR: region-regen WITHOUT neighbours -> the boundary normals diverge (edge-clamp vs
    // neighbour-aware), proving the neighbour pass is what keeps the seam closed.
    {
        std::vector<uint8> nr0 = n0, nr1 = n1;
        GenerateNormalmapRegionFromHeightfield(t0, tileWorld, tileWorld, heightScale, nr0,
                                               last - 4 - kPad, 4 - kPad, last, 10 + kPad);
        GenerateNormalmapRegionFromHeightfield(t1, tileWorld, tileWorld, heightScale, nr1,
                                               0, 4 - kPad, 4 + kPad, 10 + kPad);
        const std::vector<uint8> edge0 = boundaryColumnByte(nr0, tileRes - 1);
        const std::vector<uint8> edge1 = boundaryColumnByte(nr1, 0);
        EXPECT_NE(0, std::memcmp(edge0.data(), edge1.data(), edge0.size()))
            << "no-neighbour region regen should diverge at the seam (test not discriminating)";
    }
}

TEST(AtlasGrassParams, RowVersionCacheResetsOnIdentityChangeOrDisable)
{
    // D1: the per-slot row-version cache must be flushed on a destroy+recreate whose new controller's
    // table version can numerically equal a retained slot version (ABA). AtlasSlotsNeedReset is the
    // pure decision the ring uses; the falsifiable core is that a version-equal-but-identity-DIFFERENT
    // frame still resets (a regression that keyed reset on version alone would skip it and grass would
    // resolve through the DEAD terrain's rows — float/bury/flicker).
    using Grass = GameEngine::TerrainGrass::TerrainGrassRenderFeature;
    const uint64 idA = 0x0000000100000007ull; // (index 1, gen 7)
    const uint64 idB = 0x0000000200000007ull; // (index 2, gen 7) — same gen, different terrain

    // Steady state: same identity, atlas present -> keep the cache (parked-camera fast path).
    EXPECT_FALSE(Grass::AtlasSlotsNeedReset(/*hasAtlas*/ true, idA, /*last*/ idA));
    // Destroy+recreate (identity flips) -> reset, even though a version gate might see equal versions.
    EXPECT_TRUE(Grass::AtlasSlotsNeedReset(/*hasAtlas*/ true, idB, /*last*/ idA));
    // Atlas went away -> reset so a later re-enable re-uploads instead of trusting a stale slot.
    EXPECT_TRUE(Grass::AtlasSlotsNeedReset(/*hasAtlas*/ false, /*current*/ 0, /*last*/ idA));
    // Re-enable from a clean (identity 0) state -> reset, since the identity differs from 0.
    EXPECT_TRUE(Grass::AtlasSlotsNeedReset(/*hasAtlas*/ true, idA, /*last*/ 0));
}

// ---------------------------------------------------------------------------
// SamplesPerMeter / size re-provision settle gate (P0: SPM-change crash).
//
// The extraction system's tiled re-provision compare is a raw float !=, so an
// inspector SamplesPerMeter/size drag (a distinct value nearly every frame) fired
// a full terrain destroy+recreate PER TICK — VRAM churn + dangling bindless
// descriptors -> device-lost. TerrainReprovisionDebounce collapses a drag to ONE
// re-provision on settle. These oracles fix the coalescing law; the discriminator
// is the drag case (a value changing every frame must never fire).
// ---------------------------------------------------------------------------

TEST(TerrainReprovisionDebounce, HeldValueSettlesAfterExactlySettleFrames)
{
    // A single set (e.g. typing a value, or a mouse-up) holds one config: it must fire
    // exactly once, on the tick the hold reaches kReprovisionSettleFrames — not sooner
    // (would re-provision mid-interaction) and not never (the edit must take effect).
    TerrainReprovisionDebounce deb{};
    constexpr uint32 kSettle = 4;
    // Tick 0: first observation of the new value -> re-arm, no fire.
    EXPECT_FALSE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle));
    // Ticks 1..kSettle-1: still holding, accumulating -> no fire yet.
    for (uint32 i = 1; i < kSettle; ++i)
        EXPECT_FALSE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle)) << "fired early at hold " << i;
    // Tick kSettle: held long enough -> fire exactly once.
    EXPECT_TRUE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle));
}

TEST(TerrainReprovisionDebounce, DragNeverFiresWhileValueKeepsChanging)
{
    // THE discriminator (the P0 storm): a slider drag emits a new SamplesPerMeter every
    // frame. Every distinct value re-arms the settle counter, so the gate must NOT fire
    // for the whole drag — the pre-fix raw-float compare fired on every one of these.
    TerrainReprovisionDebounce deb{};
    constexpr uint32 kSettle = 4;
    uint32 fireCount = 0;
    for (uint32 i = 0; i <= 100; ++i)
    {
        const float32 spm = 1.0f + 0.01f * static_cast<float32>(i); // 1.00 -> 2.00, distinct each tick
        if (deb.Observe(spm, 2048.0f, 2048.0f, kSettle))
            ++fireCount;
    }
    EXPECT_EQ(0u, fireCount) << "a drag with a distinct value each tick must never re-provision";

    // Release: the value now holds at the final 2.00. The gate is level-triggered (stays true
    // while settled); the extraction caller erases the entry the tick it fires, so in practice it
    // re-provisions once. Mirror that here — the first fire must land once the value stops changing
    // (the exact settle-tick timing from a clean start is pinned by HeldValueSettles above).
    bool fired = false;
    for (uint32 i = 0; i < kSettle + 4 && !fired; ++i)
        fired = deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle);
    EXPECT_TRUE(fired) << "the settled value must re-provision after it stops changing";
}

TEST(TerrainReprovisionDebounce, ReArmsAfterFiringSoASecondEditStillReprovisions)
{
    // After a settled fire, the caller destroys the terrain and erases the entry; but even
    // a reused-index entry must treat a NEW config as a fresh edit (re-arm), so a second
    // SamplesPerMeter change is not swallowed by the post-fire state.
    TerrainReprovisionDebounce deb{};
    constexpr uint32 kSettle = 2;
    // First edit settles + fires.
    EXPECT_FALSE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle));
    EXPECT_FALSE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle));
    EXPECT_TRUE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle));
    // A DIFFERENT value arrives (second edit) -> must re-arm (no immediate fire on the flip).
    EXPECT_FALSE(deb.Observe(4.0f, 2048.0f, 2048.0f, kSettle));
    EXPECT_FALSE(deb.Observe(4.0f, 2048.0f, 2048.0f, kSettle));
    EXPECT_TRUE(deb.Observe(4.0f, 2048.0f, 2048.0f, kSettle));
}

TEST(TerrainReprovisionDebounce, SizeChangeAloneAlsoGatesAndSettles)
{
    // A world-size drag (SamplesPerMeter unchanged) drives the same storm; the gate keys on
    // size too, so a size-only change coalesces identically. A jitter on any tracked field
    // re-arms; only when all three hold does it fire.
    TerrainReprovisionDebounce deb{};
    constexpr uint32 kSettle = 3;
    EXPECT_FALSE(deb.Observe(2.0f, 1000.0f, 2048.0f, kSettle)); // SizeX moving
    EXPECT_FALSE(deb.Observe(2.0f, 1500.0f, 2048.0f, kSettle)); // still moving -> re-arm
    EXPECT_FALSE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle)); // settled value, hold 0
    EXPECT_FALSE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle)); // hold 1
    EXPECT_FALSE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle)); // hold 2
    EXPECT_TRUE(deb.Observe(2.0f, 2048.0f, 2048.0f, kSettle));  // hold 3 -> fire
}

// Pending-GPU-work-drained oracle (SPM re-provision × GE_TERRAIN_GPU_BAKE): a re-provision
// retires the terrain's atlas textures and recycles its slot index. Any queued GPU height bake
// or settle readback for that slot must be dropped with it — a bake left queued would dispatch
// against whatever terrain reuses the index (FlushPendingBakes matches on Index only → the old
// slot/rect imageStore lands in a possibly-smaller new atlas: out-of-bounds → device-lost), and
// a readback would copy from the destroyed image and, being generation-checked in the drain,
// never drain (leaking its buffer every re-provision). Device-less: only the CPU-side queues run.
TEST(TerrainReprovisionGpuBake, ReleaseDrainsPendingBakesAndReadbacksForRetiredSlot)
{
    TerrainRenderFeature feature;

    const TerrainHandle retiring{7u, 3u};
    const TerrainHandle other{9u, 1u}; // a different slot's work must survive

    std::vector<GpuHeightBakeDispatch> dispatches(1); // non-empty so QueueGpuHeightBake retains it
    feature.QueueGpuHeightBake(retiring, {}, dispatches, /*settle=*/true, 0.0f, 1.0f, false, {}, {});
    feature.SeedPendingReadbackForTests(retiring);
    feature.SeedPendingReadbackForTests(other);
    ASSERT_TRUE(feature.HasPendingBakes());
    ASSERT_EQ(feature.GetPendingReadbackCountForTests(), 2u);

    feature.ReleaseTerrainResources(retiring);

    EXPECT_FALSE(feature.HasPendingBakes());                  // the retired slot's bake is gone
    EXPECT_EQ(feature.GetPendingReadbackCountForTests(), 1u); // only the other slot's readback remains
}

// Grass placement is elided while its inputs hold still, and a heightmap has no CPU-side bytes to
// compare, so the gate consults a terrain CONTENT EPOCH instead. An atlas-backed terrain never
// touches the staged-upload queue: a sculpt stroke bakes height, normal and splat straight into the
// atlas from a compute kernel through QueueGpuHeightBake. A bake that does not bump the epoch
// leaves the blades standing at their pre-stroke root heights, on the pre-stroke mask, for as long
// as the stroke lasts. Device-less: both queues are CPU-side.
TEST(TerrainGrassContentEpoch, AQueuedAtlasBakeBumpsTheEpoch)
{
    TerrainRenderFeature feature;

    const TerrainHandle terrain{4u, 1u};
    const uint64 atRest = feature.GetGrassPlacementContentEpoch();

    // A bake with nothing to dispatch queues no write, so it must not claim a content change.
    feature.QueueGpuHeightBake(terrain, {}, {}, /*settle=*/false, 0.0f, 1.0f, false, {}, {});
    EXPECT_EQ(feature.GetGrassPlacementContentEpoch(), atRest);

    std::vector<GpuHeightBakeDispatch> dispatches(1);
    feature.QueueGpuHeightBake(terrain, {}, dispatches, /*settle=*/false, 0.0f, 1.0f, false, {}, {});
    const uint64 afterBake = feature.GetGrassPlacementContentEpoch();
    EXPECT_GT(afterBake, atRest);

    // Control on the instrument itself: a retirement, which has always bumped, still does.
    feature.ReleaseTerrainResources(terrain);
    EXPECT_GT(feature.GetGrassPlacementContentEpoch(), afterBake);
}

// The RULES-FREE binding decision — the commonest bake there is under the atlas opt-in, and the
// one the device oracles do NOT cover: they drive the test harness's own buffer setup, not
// FlushPendingBakes. What is coverable without a provisioned device atlas is the DECISION that
// path makes, which is where the trap lives, so the production code was routed through this
// function and the trap is asserted here rather than read.
//
// Two distinct failure modes, both silent:
//   * a zero-sized storage binding (invalid Vulkan) when a bake carries no rules;
//   * telling the kernel the zero-fill PADDING element is a real rule — harmless today only
//     because a zeroed row short-circuits at VolumeWeight <= 0; the authored count keeps that
//     correctness independent of the shader's short-circuit.
TEST(TerrainGpuSplatBindings, ARulesFreeBakeBindsPaddingButReportsNoRules)
{
    const SurfaceRuleBindingSizes empty = ComputeSurfaceRuleBindingSizes(0, 0);

    // Non-zero bindings, because a zero-sized one is invalid.
    EXPECT_EQ(empty.RuleBytes, sizeof(SurfaceRuleGpu));
    EXPECT_EQ(empty.ConditionBytes, sizeof(SurfaceRuleConditionGpu));
    EXPECT_GT(empty.RuleBytes, 0u);
    EXPECT_GT(empty.ConditionBytes, 0u);

    // ...but the kernel is told there are NO rules, so it reads neither buffer. This is the
    // assertion that fails if the padded element is ever reported as a row.
    EXPECT_EQ(empty.KernelRuleCount, 0u);
    EXPECT_TRUE(empty.RulesArePadding);
    EXPECT_TRUE(empty.ConditionsArePadding);
}

TEST(TerrainGpuSplatBindings, AuthoredRowsSizeTheBindingAndAreReportedInFull)
{
    // Discriminator for the case above: with real rows the padding branch must NOT engage, and
    // the reported count must be the authored count — otherwise "reports 0" would pass by always
    // reporting 0, and every rule on every terrain would silently stop compositing.
    for (size_t rules = 1; rules <= Components::kMaxTerrainSurfaceRules; ++rules)
    {
        const size_t conditions = rules * 2u;
        const SurfaceRuleBindingSizes s = ComputeSurfaceRuleBindingSizes(rules, conditions);
        EXPECT_EQ(s.RuleBytes, rules * sizeof(SurfaceRuleGpu));
        EXPECT_EQ(s.ConditionBytes, conditions * sizeof(SurfaceRuleConditionGpu));
        EXPECT_EQ(s.KernelRuleCount, rules);
        EXPECT_FALSE(s.RulesArePadding);
        EXPECT_FALSE(s.ConditionsArePadding);
    }

    // Rows with no conditions at all: the row buffer is real, the condition buffer is padding, and
    // the two decisions are independent. An unconditional rule row is legal and covers its volume.
    const SurfaceRuleBindingSizes mixed = ComputeSurfaceRuleBindingSizes(3, 0);
    EXPECT_EQ(mixed.RuleBytes, 3u * sizeof(SurfaceRuleGpu));
    EXPECT_EQ(mixed.ConditionBytes, sizeof(SurfaceRuleConditionGpu));
    EXPECT_EQ(mixed.KernelRuleCount, 3u);
    EXPECT_FALSE(mixed.RulesArePadding);
    EXPECT_TRUE(mixed.ConditionsArePadding);
}

// The same binding/count split one slice earlier, for the HEIGHT pass's modifier rows. The two
// numbers are produced in different files — the row buffer in TerrainRenderFeature::FlushPendingBakes,
// pc.ModifierCount in TerrainExtractionSystem — with only this function tying them, so what is
// asserted here is the decision both call sites now take.
//
// A base-fill-only re-bake (no modifiers) is a real, routine bake, and its two silent failure
// modes are a zero-sized storage binding, and telling the kernel the zero-fill element is a
// modifier row.
TEST(TerrainGpuHeightBakeBindings, AModifierFreeBakeBindsPaddingButReportsNoModifiers)
{
    const ModifierBindingSizes empty = ComputeModifierBindingSizes(0);

    // One padded element, because a zero-sized storage binding is invalid.
    EXPECT_EQ(empty.ModifierBytes, sizeof(ModifierGpu));
    EXPECT_GT(empty.ModifierBytes, 0u);

    // ...and the kernel's stack loop is told to run zero times, so the padded row is never read.
    EXPECT_EQ(empty.KernelModifierCount, 0u);
    EXPECT_TRUE(empty.ModifiersArePadding);
}

TEST(TerrainGpuHeightBakeBindings, AuthoredModifiersSizeTheBindingAndAreReportedInFull)
{
    // Discriminator for the case above: with real rows the padding branch must NOT engage and the
    // reported count must be the authored one, or "reports 0" would pass by always reporting 0 and
    // every sculpt would bake as bare base noise.
    //
    // The planar height stack has no authored cap (unlike kMaxTerrainSurfaceRules), so the sweep is
    // a dense low range — the depths a stroke actually produces — extended past any plausible stack.
    for (size_t mods = 1; mods <= 16; ++mods)
    {
        const ModifierBindingSizes s = ComputeModifierBindingSizes(mods);
        EXPECT_EQ(s.ModifierBytes, mods * sizeof(ModifierGpu));
        EXPECT_EQ(s.KernelModifierCount, mods);
        EXPECT_FALSE(s.ModifiersArePadding);
    }

    const ModifierBindingSizes deep = ComputeModifierBindingSizes(1024);
    EXPECT_EQ(deep.ModifierBytes, 1024u * sizeof(ModifierGpu));
    EXPECT_EQ(deep.KernelModifierCount, 1024u);
    EXPECT_FALSE(deep.ModifiersArePadding);
}

// Re-provision VRAM staging: releasing a unified set ARMS the feature-wide drain barrier so the
// next world-sized allocation (EnsureUnifiedTiledTextures) is withheld until the retired set
// clears its frames-in-flight quarantine — capping the transient peak at max(old,new) instead of
// old+new. The barrier is feature-wide (not per-slot) because a re-provision assigns the new
// global GPU handle a fresh index, so no per-slot key spans the old and new sets. Disable the
// arm in ReleaseTerrainResources and the barrier stays 0 (no staging), so old+new would coexist.
// Maturation needs per-frame render ticks (a device); this locks the arming half, device-free.
TEST(TerrainReprovisionVramStaging, ReleaseArmsDrainBarrier)
{
    TerrainRenderFeature feature;

    EXPECT_EQ(feature.GetReprovisionDrainFrameForTests(), 0u); // not yet armed

    feature.ReleaseTerrainResources(TerrainHandle{5u, 2u});

    EXPECT_NE(feature.GetReprovisionDrainFrameForTests(), 0u); // barrier armed by the retire
}
