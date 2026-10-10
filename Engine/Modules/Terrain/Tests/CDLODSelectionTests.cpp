#include "Terrain/CDLODQuadtree.h"
#include "Terrain/CDLODSelection.h"
#include "Terrain/Heightfield.h"
#include "Terrain/TerrainTypes.h"

#include <gtest/gtest.h>
#include <limits>

using namespace GameEngine;
using namespace GameEngine::Terrain;

class CDLODSelectionFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Create a flat 513x513 heightfield
        m_Heightfield = HeightfieldData(513, 513, 0.0f);
        m_Quadtree.Build(m_Heightfield, 6);
    }

    HeightfieldData m_Heightfield;
    CDLODQuadtree m_Quadtree;
};

TEST_F(CDLODSelectionFixture, Quadtree_BuildSucceeds)
{
    EXPECT_TRUE(m_Quadtree.IsBuilt());
    EXPECT_EQ(m_Quadtree.GetNumLevels(), 6u);
    EXPECT_GT(m_Quadtree.GetTotalNodeCount(), 0u);
}

TEST_F(CDLODSelectionFixture, Quadtree_FlatTerrain_MinMaxZero)
{
    const auto& root = m_Quadtree.GetNode(m_Quadtree.GetNumLevels() - 1, 0, 0);
    EXPECT_FLOAT_EQ(root.MinHeight, 0.0f);
    EXPECT_FLOAT_EQ(root.MaxHeight, 0.0f);
}

TEST_F(CDLODSelectionFixture, Quadtree_NonFlat_MinMaxCorrect)
{
    // Rebuild with some height variation
    m_Heightfield.SetSample(100, 100, 0.8f);
    m_Heightfield.SetSample(200, 200, -0.2f);
    m_Quadtree.Build(m_Heightfield, 6);

    const auto& root = m_Quadtree.GetNode(m_Quadtree.GetNumLevels() - 1, 0, 0);
    EXPECT_LE(root.MinHeight, -0.2f);
    EXPECT_GE(root.MaxHeight, 0.8f);
}

TEST_F(CDLODSelectionFixture, Select_CameraAtCenter_ProducesPatches)
{
    CDLODSelection selection;
    CDLODSelectionParams params{};
    params.CameraPosition = Mathematics::Vector3(512, 50, 512);
    params.WorldSizeX = 1024.0f;
    params.WorldSizeZ = 1024.0f;
    params.HeightScale = 256.0f;
    params.LODRangeScale = 2.0f;

    std::vector<CDLODPatch> patches;
    selection.Select(m_Quadtree, params, patches);

    EXPECT_GT(patches.size(), 0u);
}

TEST_F(CDLODSelectionFixture, Select_AllPatchesHaveValidScale)
{
    CDLODSelection selection;
    CDLODSelectionParams params{};
    params.CameraPosition = Mathematics::Vector3(512, 50, 512);
    params.WorldSizeX = 1024.0f;
    params.WorldSizeZ = 1024.0f;
    params.HeightScale = 256.0f;
    params.LODRangeScale = 2.0f;

    std::vector<CDLODPatch> patches;
    selection.Select(m_Quadtree, params, patches);

    for (const auto& p : patches)
    {
        EXPECT_GT(p.Scale, 0.0f);
        EXPECT_LT(p.LODLevel, 6u);
        EXPECT_GT(p.MorphEndDist, 0.0f);
        EXPECT_GE(p.MorphEndDist, p.MorphStartDist);
        EXPECT_NE(p.SubQuadFlags, 0u);
    }
}

TEST_F(CDLODSelectionFixture, Select_NearCamera_HasFinePatches)
{
    CDLODSelection selection;
    CDLODSelectionParams params{};
    params.CameraPosition = Mathematics::Vector3(512, 50, 512);
    params.WorldSizeX = 1024.0f;
    params.WorldSizeZ = 1024.0f;
    params.HeightScale = 256.0f;
    params.LODRangeScale = 2.0f;

    std::vector<CDLODPatch> patches;
    selection.Select(m_Quadtree, params, patches);

    bool hasLOD0 = false;
    for (const auto& p : patches)
    {
        if (p.LODLevel == 0)
        {
            hasLOD0 = true;
            break;
        }
    }
    EXPECT_TRUE(hasLOD0) << "Should have LOD 0 patches near the camera";
}

TEST_F(CDLODSelectionFixture, Select_CameraOutside_StillProducesPatches)
{
    // Camera outside the terrain bounds — should still select coarse patches
    // (no frustum culling = all nodes visible regardless of camera position).
    CDLODSelection selection;
    CDLODSelectionParams params{};
    params.CameraPosition = Mathematics::Vector3(-500, 50, -500);
    params.WorldSizeX = 1024.0f;
    params.WorldSizeZ = 1024.0f;
    params.HeightScale = 256.0f;
    params.LODRangeScale = 2.0f;

    std::vector<CDLODPatch> patches;
    selection.Select(m_Quadtree, params, patches);

    EXPECT_GT(patches.size(), 0u)
        << "Even with camera outside terrain, patches should be selected (no frustum cull)";
}

TEST_F(CDLODSelectionFixture, Select_SubQuadFlags_PartialPatchesExist)
{
    CDLODSelection selection;
    CDLODSelectionParams params{};
    params.CameraPosition = Mathematics::Vector3(512, 50, 512);
    params.WorldSizeX = 1024.0f;
    params.WorldSizeZ = 1024.0f;
    params.HeightScale = 256.0f;
    params.LODRangeScale = 2.0f;

    std::vector<CDLODPatch> patches;
    selection.Select(m_Quadtree, params, patches);

    uint32 fullCount = 0;
    uint32 partialCount = 0;
    for (const auto& p : patches)
    {
        if (p.SubQuadFlags == 0xF)
            ++fullCount;
        else
            ++partialCount;
    }

    // With mixed LODs, there should be both full and partial patches
    EXPECT_GT(fullCount, 0u);
    EXPECT_GT(partialCount, 0u) << "LOD boundary patches should have partial sub-quad flags";
}

TEST(CDLODSelectionStatic, ComputeLODRange_Increases)
{
    const float32 base = 64.0f;
    const float32 scale = 2.0f;

    float32 prev = 0;
    for (uint32 i = 0; i < 6; ++i)
    {
        float32 range = CDLODSelection::ComputeLODRange(i, base, scale);
        EXPECT_GT(range, prev) << "LOD range should increase with level";
        prev = range;
    }
}

TEST_F(CDLODSelectionFixture, Select_PrecomputedRanges_SameAsDefault)
{
    CDLODSelection selection;

    // First: select without precomputed ranges
    CDLODSelectionParams params{};
    params.CameraPosition = Mathematics::Vector3(512, 50, 512);
    params.WorldSizeX = 1024.0f;
    params.WorldSizeZ = 1024.0f;
    params.HeightScale = 256.0f;
    params.LODRangeScale = 2.0f;

    std::vector<CDLODPatch> patchesA;
    selection.Select(m_Quadtree, params, patchesA);

    // Precompute ranges
    const uint32 numLevels = 6;
    const float32 baseRange = 1024.0f / static_cast<float32>(m_Quadtree.GetNodesPerAxisAtLevel(0)) * 2.0f;
    float32 visRanges[kMaxLODLevels]{};
    float32 morphStart[kMaxLODLevels]{};
    float32 morphEnd[kMaxLODLevels]{};

    for (uint32 i = 0; i < numLevels; ++i)
        visRanges[i] = CDLODSelection::ComputeLODRange(i, baseRange, 2.0f);

    float32 prevStart = 0;
    for (uint32 i = 0; i < numLevels; ++i)
    {
        morphEnd[i] = visRanges[i];
        morphStart[i] = prevStart + (morphEnd[i] - prevStart) * 0.66f;
        prevStart = morphStart[i];
    }

    // Second: select with precomputed ranges
    params.PrecomputedVisRanges = visRanges;
    params.PrecomputedMorphStart = morphStart;
    params.PrecomputedMorphEnd = morphEnd;

    std::vector<CDLODPatch> patchesB;
    selection.Select(m_Quadtree, params, patchesB);

    // Should produce the same number of patches
    EXPECT_EQ(patchesA.size(), patchesB.size());
}

// Verify that CDLOD selection produces gapless spatial coverage.
// Rasterizes all patches (full + partial) into a 2D grid and checks that
// every cell within the terrain bounds is covered exactly once.
// A gap would manifest as a cell with coverage count 0.
TEST_F(CDLODSelectionFixture, Select_SpatialCoverage_NoGaps)
{
    CDLODSelection selection;
    CDLODSelectionParams params{};
    params.CameraPosition = Mathematics::Vector3(512, 50, 512);
    params.WorldSizeX = 1024.0f;
    params.WorldSizeZ = 1024.0f;
    params.HeightScale = 256.0f;
    params.LODRangeScale = 2.0f;

    std::vector<CDLODPatch> patches;
    selection.Select(m_Quadtree, params, patches);
    ASSERT_GT(patches.size(), 0u);

    // Rasterize into a grid at TWICE the finest patch resolution.
    // This ensures each cell is smaller than a single quadrant at the finest LOD,
    // so a full LOD=0 patch's 4 quadrants map to 4 different cells, not the same one.
    const uint32 finestNodesPerAxis = m_Quadtree.GetNodesPerAxisAtLevel(0);
    const uint32 gridRes = finestNodesPerAxis * 2; // 2x resolution
    const float32 cellSizeX = params.WorldSizeX / static_cast<float32>(gridRes);
    const float32 cellSizeZ = params.WorldSizeZ / static_cast<float32>(gridRes);

    std::vector<uint32> coverage(gridRes * gridRes, 0);

    for (const auto& patch : patches)
    {
        // Determine which quadrants this patch draws.
        // For each drawn quadrant, mark the covered cells.
        const float32 halfScale = patch.Scale * 0.5f;

        // Quadrant world-space bounds: TL, TR, BL, BR
        struct Quad { float32 minX, minZ, maxX, maxZ; };
        Quad quads[4] = {
            {patch.WorldX,              patch.WorldZ,              patch.WorldX + halfScale, patch.WorldZ + halfScale}, // TL (bit 0)
            {patch.WorldX + halfScale,  patch.WorldZ,              patch.WorldX + patch.Scale, patch.WorldZ + halfScale}, // TR (bit 1)
            {patch.WorldX,              patch.WorldZ + halfScale,  patch.WorldX + halfScale, patch.WorldZ + patch.Scale}, // BL (bit 2)
            {patch.WorldX + halfScale,  patch.WorldZ + halfScale,  patch.WorldX + patch.Scale, patch.WorldZ + patch.Scale}, // BR (bit 3)
        };

        for (uint32 q = 0; q < 4; ++q)
        {
            if (!(patch.SubQuadFlags & (1u << q)))
                continue;

            const auto& quad = quads[q];

            // Convert to grid cell range (clamp to terrain bounds).
            int32 startCellX = static_cast<int32>(std::floor((quad.minX - params.WorldOriginX) / cellSizeX));
            int32 startCellZ = static_cast<int32>(std::floor((quad.minZ - params.WorldOriginZ) / cellSizeZ));
            int32 endCellX = static_cast<int32>(std::ceil((quad.maxX - params.WorldOriginX) / cellSizeX));
            int32 endCellZ = static_cast<int32>(std::ceil((quad.maxZ - params.WorldOriginZ) / cellSizeZ));

            startCellX = std::max(startCellX, 0);
            startCellZ = std::max(startCellZ, 0);
            endCellX = std::min(endCellX, static_cast<int32>(gridRes));
            endCellZ = std::min(endCellZ, static_cast<int32>(gridRes));

            for (int32 z = startCellZ; z < endCellZ; ++z)
            {
                for (int32 x = startCellX; x < endCellX; ++x)
                {
                    coverage[z * gridRes + x]++;
                }
            }
        }
    }

    // Check: every cell should have coverage == 1 (no gaps, no overlaps).
    uint32 gapCount = 0;
    uint32 overlapCount = 0;
    for (uint32 z = 0; z < gridRes; ++z)
    {
        for (uint32 x = 0; x < gridRes; ++x)
        {
            uint32 c = coverage[z * gridRes + x];
            if (c == 0)
            {
                ++gapCount;
                if (gapCount <= 5)
                {
                    float32 worldX = params.WorldOriginX + x * cellSizeX;
                    float32 worldZ = params.WorldOriginZ + z * cellSizeZ;
                    ADD_FAILURE() << "Gap at cell (" << x << ", " << z
                                  << ") worldPos (" << worldX << ", " << worldZ << ")";
                }
            }
            else if (c > 1)
            {
                ++overlapCount;
                if (overlapCount <= 10)
                {
                    float32 worldX = params.WorldOriginX + x * cellSizeX;
                    float32 worldZ = params.WorldOriginZ + z * cellSizeZ;

                    // Find ALL patches covering this cell
                    std::string info;
                    for (uint32 pi = 0; pi < patches.size(); ++pi)
                    {
                        const auto& p = patches[pi];
                        float32 halfS = p.Scale * 0.5f;
                        struct Quad { float32 mnX, mnZ, mxX, mxZ; };
                        Quad qs[4] = {
                            {p.WorldX, p.WorldZ, p.WorldX+halfS, p.WorldZ+halfS},
                            {p.WorldX+halfS, p.WorldZ, p.WorldX+p.Scale, p.WorldZ+halfS},
                            {p.WorldX, p.WorldZ+halfS, p.WorldX+halfS, p.WorldZ+p.Scale},
                            {p.WorldX+halfS, p.WorldZ+halfS, p.WorldX+p.Scale, p.WorldZ+p.Scale},
                        };
                        for (uint32 q = 0; q < 4; ++q)
                        {
                            if (!(p.SubQuadFlags & (1u << q)))
                                continue;
                            if (worldX >= qs[q].mnX && worldX < qs[q].mxX &&
                                worldZ >= qs[q].mnZ && worldZ < qs[q].mxZ)
                            {
                                char hex[16];
                                snprintf(hex, sizeof(hex), "0x%X", p.SubQuadFlags);
                                info += "\n    patch[" + std::to_string(pi) + "] LOD=" +
                                        std::to_string(p.LODLevel) + " flags=" + hex +
                                        " quad=" + std::to_string(q) + " Scale=" +
                                        std::to_string(p.Scale) +
                                        " origin=(" + std::to_string(p.WorldX) + "," +
                                        std::to_string(p.WorldZ) + ")";
                            }
                        }
                    }
                    ADD_FAILURE() << "Overlap (coverage=" << c << ") at cell ("
                                  << x << ", " << z << ") worldPos (" << worldX
                                  << ", " << worldZ << "):" << info;
                }
            }
        }
    }

    EXPECT_EQ(gapCount, 0u) << "Found " << gapCount << " uncovered cells";
    EXPECT_EQ(overlapCount, 0u) << "Found " << overlapCount << " overlapping cells (double-rendered areas)";
}

// ---------------------------------------------------------------------------
// TerrainConfig::FromSamplesPerMeter tests
// ---------------------------------------------------------------------------

TEST(TerrainConfigTests, FromSamplesPerMeter_DefaultMatchesLegacy)
{
    // 1 sample/meter on a 1024m terrain with grid=32 should give
    // the same result as the old default: res=1025, LODLevels=6.
    auto config = TerrainConfig::FromSamplesPerMeter(1024.0f, 1024.0f, 256.0f, 1.0f, 32);
    EXPECT_EQ(config.HeightmapWidth, 1025u);
    EXPECT_EQ(config.HeightmapHeight, 1025u);
    EXPECT_EQ(config.LODLevels, 6u);
    EXPECT_EQ(config.PatchGridSize, 32u);
}

TEST(TerrainConfigTests, FromSamplesPerMeter_HigherDensity)
{
    // 2 samples/meter on 1024m terrain → ~2049 desired → grid=32, LOD=7 → 2049
    auto config = TerrainConfig::FromSamplesPerMeter(1024.0f, 1024.0f, 256.0f, 2.0f, 32);
    EXPECT_EQ(config.HeightmapWidth, 2049u);
    EXPECT_EQ(config.LODLevels, 7u);
}

TEST(TerrainConfigTests, FromSamplesPerMeter_LowerDensity)
{
    // 0.5 samples/meter on 1024m terrain → ~513 desired → grid=32, LOD=5 → 513
    auto config = TerrainConfig::FromSamplesPerMeter(1024.0f, 1024.0f, 256.0f, 0.5f, 32);
    EXPECT_EQ(config.HeightmapWidth, 513u);
    EXPECT_EQ(config.LODLevels, 5u);
}

TEST(TerrainConfigTests, FromSamplesPerMeter_LargerGrid)
{
    // 1 sample/meter, 1024m, grid=64 → desired=1025, 64*2^(N-1)+1 >= 1025 → N=5 → 1025
    auto config = TerrainConfig::FromSamplesPerMeter(1024.0f, 1024.0f, 256.0f, 1.0f, 64);
    EXPECT_EQ(config.HeightmapWidth, 1025u);
    EXPECT_EQ(config.LODLevels, 5u);
    EXPECT_EQ(config.PatchGridSize, 64u);
}

TEST(TerrainConfigTests, FromSamplesPerMeter_SmallTerrain)
{
    // 1 sample/meter, 128m terrain, grid=32 → desired=129, LOD=3 → 32*4+1=129
    auto config = TerrainConfig::FromSamplesPerMeter(128.0f, 128.0f, 64.0f, 1.0f, 32);
    EXPECT_EQ(config.HeightmapWidth, 129u);
    EXPECT_EQ(config.LODLevels, 3u);
}

TEST(TerrainConfigTests, FromSamplesPerMeter_LargeTerrain)
{
    // 1 sample/meter, 8192m terrain, grid=32 → desired=8193, LOD=9 → 32*256+1=8193
    auto config = TerrainConfig::FromSamplesPerMeter(8192.0f, 8192.0f, 512.0f, 1.0f, 32);
    EXPECT_EQ(config.HeightmapWidth, 8193u);
    EXPECT_EQ(config.LODLevels, 9u);
}

TEST(TerrainConfigTests, FromSamplesPerMeter_ClampsToMaxResolution)
{
    // Very high density on large terrain would exceed 8193 — should clamp.
    auto config = TerrainConfig::FromSamplesPerMeter(16384.0f, 16384.0f, 512.0f, 2.0f, 32);
    EXPECT_LE(config.HeightmapWidth, kMaxHeightmapDimension);
    EXPECT_LE(config.LODLevels, kMaxLODLevels);
}

TEST(TerrainConfigTests, FromSamplesPerMeter_ClampsMinDensity)
{
    // Very small density should still produce a valid config.
    auto config = TerrainConfig::FromSamplesPerMeter(1024.0f, 1024.0f, 256.0f, 0.001f, 32);
    EXPECT_GE(config.HeightmapWidth, 3u);
    EXPECT_GE(config.LODLevels, 1u);
}

TEST(TerrainConfigTests, FromSamplesPerMeter_NonSquareTerrain)
{
    // Non-square: 2048x512. Resolution is based on the larger axis (2048m).
    auto config = TerrainConfig::FromSamplesPerMeter(2048.0f, 512.0f, 256.0f, 1.0f, 32);
    EXPECT_EQ(config.HeightmapWidth, 2049u);
    EXPECT_EQ(config.HeightmapHeight, 2049u);
    EXPECT_EQ(config.WorldSizeX, 2048.0f);
    EXPECT_EQ(config.WorldSizeZ, 512.0f);
}

// ---------------------------------------------------------------------------
// TerrainNeedsTiling — the ONE tiling predicate
//
// Editor provisioning and the extraction system each used to carry their own. Provisioning
// tested the raw product (size * spm > 1024) while extraction tested the DERIVED resolution,
// which floors that product before adding the shared edge sample. On the interval where the two
// disagree, provisioning classified the terrain as tiled and therefore withheld both the eager
// heightfield AND its HeightFieldColliderShape, while extraction built a single terrain that
// rendered — a terrain you could see and walk through.
// ---------------------------------------------------------------------------

// The predicate provisioning used to carry, kept only so the discriminator below is legible.
static bool LegacyProvisioningWillTile(float32 sizeX, float32 sizeZ, float32 spm)
{
    return sizeX * spm > 1024.0f || sizeZ * spm > 1024.0f;
}

TEST(TerrainNeedsTiling, ExactlyAtThePerTileCapDoesNotTile)
{
    // 1024 m at 1 spm derives 1025 samples — exactly the per-tile ceiling, so one tile holds it.
    EXPECT_FALSE(TerrainNeedsTiling(1024.0f, 1024.0f, 1.0f));
    EXPECT_EQ(TerrainConfig::FromSamplesPerMeter(1024.0f, 1024.0f, 0.0f, 1.0f).HeightmapWidth,
              kMaxTileResolution);
}

// DISCRIMINATOR for the split predicate. 1024.5 m at 1 spm derives floor(1024.5) + 1 = 1025
// samples, which one tile holds — but the raw product is 1024.5, which the old provisioning
// predicate read as "will tile". That terrain rendered untiled and unprovisioned: no collider.
TEST(TerrainNeedsTiling, RoundingWindowDoesNotTileEvenThoughTheRawProductExceedsTheCap)
{
    EXPECT_FALSE(TerrainNeedsTiling(1024.5f, 1024.5f, 1.0f))
        << "the derived resolution is 1025 — one tile holds it";
    EXPECT_TRUE(LegacyProvisioningWillTile(1024.5f, 1024.5f, 1.0f))
        << "the retired predicate disagreed here; that disagreement is the defect";
}

TEST(TerrainNeedsTiling, JustPastTheCapTiles)
{
    // 1025 m at 1 spm derives 1026 samples, one past the ceiling.
    EXPECT_TRUE(TerrainNeedsTiling(1025.0f, 1025.0f, 1.0f));
}

TEST(TerrainNeedsTiling, DensityScalesTheThresholdNotJustSize)
{
    EXPECT_FALSE(TerrainNeedsTiling(512.0f, 512.0f, 2.0f)); // 1025 samples
    EXPECT_TRUE(TerrainNeedsTiling(513.0f, 513.0f, 2.0f));  // 1027 samples
}

TEST(TerrainNeedsTiling, TheLongerAxisDecides)
{
    EXPECT_TRUE(TerrainNeedsTiling(4096.0f, 128.0f, 1.0f));
    EXPECT_TRUE(TerrainNeedsTiling(128.0f, 4096.0f, 1.0f));
}

// The predicate must stay pinned to the resolution the renderer actually derives, so it can
// never drift back into being a second, independently-written rule.
//
// The regular sweep alone is NOT sufficient: its 37.5 m step lands 956 points and not one of
// them falls in the (1024, 1025) product window where the derived resolution and the raw product
// disagree, so the sweep stays green even against the predicate this replaced. The window cases
// are therefore seeded explicitly, and the second loop proves those seeds discriminate.
TEST(TerrainNeedsTiling, AgreesWithTheDerivedResolutionAcrossTheLadder)
{
    const std::pair<float32, float32> windowCases[] = {
        {1024.5f, 1.0f}, {1024.1f, 1.0f}, {1024.9f, 1.0f}, // product inside (1024, 1025)
        {512.25f, 2.0f},                                   // 1024.5 at double density
        {256.125f, 4.0f},                                  // 1024.5 at quadruple density
        {2049.0f, 0.5f},                                   // 1024.5 at half density
    };
    for (const auto& [size, spm] : windowCases)
    {
        const uint32 derived =
            TerrainConfig::FromSamplesPerMeter(size, size, 0.0f, spm).HeightmapWidth;
        EXPECT_EQ(TerrainNeedsTiling(size, size, spm), derived > kMaxTileResolution)
            << "seeded window case size=" << size << " spm=" << spm << " derived=" << derived;
    }

    // RED ARM for the seeds: every one must be a case the retired raw-product predicate gets
    // WRONG. If this stops holding, the seeds have drifted out of the window and the sweep
    // below is back to proving nothing.
    for (const auto& [size, spm] : windowCases)
        EXPECT_NE(TerrainNeedsTiling(size, size, spm), LegacyProvisioningWillTile(size, size, spm))
            << "seed size=" << size << " spm=" << spm << " no longer discriminates";

    for (const float32 spm : {0.5f, 1.0f, 2.0f, 4.0f})
        for (float32 size = 64.0f; size <= 9000.0f; size += 37.5f)
        {
            const uint32 derived =
                TerrainConfig::FromSamplesPerMeter(size, size, 0.0f, spm).HeightmapWidth;
            EXPECT_EQ(TerrainNeedsTiling(size, size, spm), derived > kMaxTileResolution)
                << "size=" << size << " spm=" << spm << " derived=" << derived;
        }
}

// Non-square terrains are the class a tile-geometry regression was already self-caught in. The
// predicate resolves on the LONGER axis, and that must hold whichever axis is longer.
TEST(TerrainNeedsTiling, AgreesWithTheDerivedResolutionForNonSquareTerrains)
{
    const std::pair<float32, float32> extents[] = {
        {2048.0f, 128.0f}, {128.0f, 2048.0f}, {1024.5f, 64.0f}, {64.0f, 1024.5f},
        {1024.0f, 1024.5f}, {8192.0f, 512.0f}, {300.0f, 900.0f},
    };
    for (const float32 spm : {0.5f, 1.0f, 2.0f})
        for (const auto& [sx, sz] : extents)
        {
            const uint32 derived =
                TerrainConfig::FromSamplesPerMeter(sx, sz, 0.0f, spm).HeightmapWidth;
            EXPECT_EQ(TerrainNeedsTiling(sx, sz, spm), derived > kMaxTileResolution)
                << "sx=" << sx << " sz=" << sz << " spm=" << spm << " derived=" << derived;
            EXPECT_EQ(TerrainNeedsTiling(sx, sz, spm), TerrainNeedsTiling(sz, sx, spm))
                << "orientation changed the answer: sx=" << sx << " sz=" << sz << " spm=" << spm;
        }
}

TEST(TerrainConfigTests, FromSamplesPerMeter_ResolutionMatchesGridTimesLOD)
{
    // Verify the fundamental CDLOD constraint: res = grid * 2^(LOD-1) + 1
    for (float32 spm : {0.25f, 0.5f, 1.0f, 2.0f, 4.0f})
    {
        auto config = TerrainConfig::FromSamplesPerMeter(1024.0f, 1024.0f, 256.0f, spm, 32);
        uint32 expected = config.PatchGridSize * (1u << (config.LODLevels - 1)) + 1;
        EXPECT_EQ(config.HeightmapWidth, expected)
            << "At SamplesPerMeter=" << spm << ": resolution should be grid * 2^(LOD-1) + 1";
    }
}

// Verify that selection still produces gapless coverage with configs derived
// from FromSamplesPerMeter at various density settings.
TEST(TerrainConfigTests, FromSamplesPerMeter_SelectionCoverage)
{
    for (float32 spm : {0.5f, 1.0f, 2.0f})
    {
        auto config = TerrainConfig::FromSamplesPerMeter(1024.0f, 1024.0f, 256.0f, spm, 32);

        HeightfieldData hf(config.HeightmapWidth, config.HeightmapHeight, 0.0f);
        CDLODQuadtree quadtree;
        quadtree.Build(hf, config.LODLevels);

        CDLODSelection selection;
        CDLODSelectionParams params{};
        params.CameraPosition = Mathematics::Vector3(512, 50, 512);
        params.WorldSizeX = config.WorldSizeX;
        params.WorldSizeZ = config.WorldSizeZ;
        params.HeightScale = config.HeightScale;
        params.LODRangeScale = config.LODRangeScale;

        std::vector<CDLODPatch> patches;
        selection.Select(quadtree, params, patches);
        ASSERT_GT(patches.size(), 0u) << "SamplesPerMeter=" << spm;

        // Verify no gaps using simplified coverage check
        const uint32 finestNodes = quadtree.GetNodesPerAxisAtLevel(0);
        const uint32 gridRes = finestNodes * 2;
        const float32 cellSizeX = config.WorldSizeX / static_cast<float32>(gridRes);
        const float32 cellSizeZ = config.WorldSizeZ / static_cast<float32>(gridRes);

        std::vector<uint32> coverage(gridRes * gridRes, 0);
        for (const auto& patch : patches)
        {
            const float32 halfScale = patch.Scale * 0.5f;
            struct Quad { float32 mnX, mnZ, mxX, mxZ; };
            Quad quads[4] = {
                {patch.WorldX, patch.WorldZ, patch.WorldX+halfScale, patch.WorldZ+halfScale},
                {patch.WorldX+halfScale, patch.WorldZ, patch.WorldX+patch.Scale, patch.WorldZ+halfScale},
                {patch.WorldX, patch.WorldZ+halfScale, patch.WorldX+halfScale, patch.WorldZ+patch.Scale},
                {patch.WorldX+halfScale, patch.WorldZ+halfScale, patch.WorldX+patch.Scale, patch.WorldZ+patch.Scale},
            };
            for (uint32 q = 0; q < 4; ++q)
            {
                if (!(patch.SubQuadFlags & (1u << q))) continue;
                int32 sx = std::max(0, static_cast<int32>(std::floor(quads[q].mnX / cellSizeX)));
                int32 sz = std::max(0, static_cast<int32>(std::floor(quads[q].mnZ / cellSizeZ)));
                int32 ex = std::min(static_cast<int32>(gridRes), static_cast<int32>(std::ceil(quads[q].mxX / cellSizeX)));
                int32 ez = std::min(static_cast<int32>(gridRes), static_cast<int32>(std::ceil(quads[q].mxZ / cellSizeZ)));
                for (int32 z = sz; z < ez; ++z)
                    for (int32 x = sx; x < ex; ++x)
                        coverage[z * gridRes + x]++;
            }
        }

        uint32 gaps = 0;
        for (auto c : coverage)
            if (c == 0) ++gaps;
        EXPECT_EQ(gaps, 0u) << "Found " << gaps << " gaps at SamplesPerMeter=" << spm;
    }
}

// ---------------------------------------------------------------------------
// CDLODQuadtree patchable API tests (AllocateLevels + SetNodeMinMax)
// ---------------------------------------------------------------------------

TEST(CDLODQuadtreePatchable, AllocateLevels_StructureCorrect)
{
    CDLODQuadtree qt;
    qt.AllocateLevels(4);

    EXPECT_TRUE(qt.IsBuilt());
    EXPECT_EQ(qt.GetNumLevels(), 4u);
    EXPECT_EQ(qt.GetNodesPerAxisAtLevel(0), 8u);  // 2^(4-1) = 8
    EXPECT_EQ(qt.GetNodesPerAxisAtLevel(1), 4u);
    EXPECT_EQ(qt.GetNodesPerAxisAtLevel(2), 2u);
    EXPECT_EQ(qt.GetNodesPerAxisAtLevel(3), 1u);

    // All nodes should be initialized to (0, 0).
    for (uint32 x = 0; x < 8; ++x)
    {
        for (uint32 z = 0; z < 8; ++z)
        {
            const auto& node = qt.GetNode(0, x, z);
            EXPECT_FLOAT_EQ(node.MinHeight, 0.0f);
            EXPECT_FLOAT_EQ(node.MaxHeight, 0.0f);
        }
    }
}

TEST(CDLODQuadtreePatchable, SetNodeMinMax_RetrievesCorrectly)
{
    CDLODQuadtree qt;
    qt.AllocateLevels(3);

    qt.SetNodeMinMax(0, 2, 3, -0.5f, 1.0f);

    const auto& node = qt.GetNode(0, 2, 3);
    EXPECT_FLOAT_EQ(node.MinHeight, -0.5f);
    EXPECT_FLOAT_EQ(node.MaxHeight, 1.0f);
}

TEST(CDLODQuadtreePatchable, BuildCoarseLevelsFromChildren_MergesCorrectly)
{
    CDLODQuadtree qt;
    qt.AllocateLevels(3);
    // Finest level: 4x4 nodes.

    // Set some varied heights at the finest level.
    qt.SetNodeMinMax(0, 0, 0, 0.0f, 1.0f);
    qt.SetNodeMinMax(0, 1, 0, 0.5f, 2.0f);
    qt.SetNodeMinMax(0, 0, 1, -1.0f, 0.0f);
    qt.SetNodeMinMax(0, 1, 1, 0.0f, 3.0f);

    qt.BuildCoarseLevelsFromChildren();

    // Level 1 node (0,0) covers finest (0,0), (1,0), (0,1), (1,1).
    const auto& parent = qt.GetNode(1, 0, 0);
    EXPECT_FLOAT_EQ(parent.MinHeight, -1.0f);
    EXPECT_FLOAT_EQ(parent.MaxHeight, 3.0f);
}

TEST(CDLODQuadtreePatchable, BuildMatchesHeightfieldBuild)
{
    // Build a quadtree from a heightfield, then build one patchably and compare.
    HeightfieldData hf(65, 65, 0.0f);
    hf.FillWithNoise(4.0f, 1.0f, 3, 99);

    CDLODQuadtree qtFromHF;
    qtFromHF.Build(hf, 3);

    // Patchable build: manually populate finest level and build coarse.
    CDLODQuadtree qtPatchable;
    qtPatchable.AllocateLevels(3);

    const uint32 finestNodes = qtPatchable.GetNodesPerAxisAtLevel(0); // 4
    const uint32 samplesPerNode = (hf.GetWidth() - 1) / finestNodes;

    for (uint32 z = 0; z < finestNodes; ++z)
    {
        for (uint32 x = 0; x < finestNodes; ++x)
        {
            float32 minH, maxH;
            hf.GetMinMax(static_cast<int32>(x * samplesPerNode),
                         static_cast<int32>(z * samplesPerNode),
                         static_cast<int32>(samplesPerNode + 1),
                         static_cast<int32>(samplesPerNode + 1),
                         minH, maxH);
            qtPatchable.SetNodeMinMax(0, x, z, minH, maxH);
        }
    }

    qtPatchable.BuildCoarseLevelsFromChildren();

    // Compare all nodes.
    for (uint32 level = 0; level < 3; ++level)
    {
        const uint32 n = qtFromHF.GetNodesPerAxisAtLevel(level);
        for (uint32 z = 0; z < n; ++z)
        {
            for (uint32 x = 0; x < n; ++x)
            {
                const auto& a = qtFromHF.GetNode(level, x, z);
                const auto& b = qtPatchable.GetNode(level, x, z);
                EXPECT_FLOAT_EQ(a.MinHeight, b.MinHeight)
                    << "Mismatch at level=" << level << " (" << x << "," << z << ")";
                EXPECT_FLOAT_EQ(a.MaxHeight, b.MaxHeight)
                    << "Mismatch at level=" << level << " (" << x << "," << z << ")";
            }
        }
    }
}

// Verify that CDLOD selection works correctly on a patchably-built quadtree
// (simulating a 2x2 tile grid with a global quadtree).
TEST(CDLODQuadtreePatchable, GlobalQuadtreeSelection_GaplessCoverage)
{
    // Simulate a 2x2 tile grid. Each tile has LODLevels=3, grid=32 -> 4 finest nodes.
    // Global: 8 finest nodes per axis, 4 LOD levels.
    const uint32 tileLODLevels = 3;
    const uint32 tilesPerAxis = 2;
    const uint32 tileFinestNodes = 1u << (tileLODLevels - 1); // 4
    const uint32 globalFinestNodes = tileFinestNodes * tilesPerAxis; // 8
    const float32 tileWorldSize = 128.0f;
    const float32 totalWorldSize = static_cast<float32>(globalFinestNodes) *
        (tileWorldSize / static_cast<float32>(tileFinestNodes)); // 256

    // Build patchable global quadtree.
    uint32 globalLevels = 1;
    while ((1u << (globalLevels - 1)) < globalFinestNodes)
        ++globalLevels;

    CDLODQuadtree globalQT;
    globalQT.AllocateLevels(globalLevels);

    // Fill each "tile" region with some height data.
    HeightfieldData tileHF(33, 33, 0.0f); // 32 grid + 1
    tileHF.FillWithNoise(8.0f, 1.0f, 3, 42);

    const uint32 samplesPerNode = (tileHF.GetWidth() - 1) / tileFinestNodes;

    for (uint32 tz = 0; tz < tilesPerAxis; ++tz)
    {
        for (uint32 tx = 0; tx < tilesPerAxis; ++tx)
        {
            for (uint32 nz = 0; nz < tileFinestNodes; ++nz)
            {
                for (uint32 nx = 0; nx < tileFinestNodes; ++nx)
                {
                    float32 minH, maxH;
                    tileHF.GetMinMax(static_cast<int32>(nx * samplesPerNode),
                                     static_cast<int32>(nz * samplesPerNode),
                                     static_cast<int32>(samplesPerNode + 1),
                                     static_cast<int32>(samplesPerNode + 1),
                                     minH, maxH);
                    globalQT.SetNodeMinMax(0, tx * tileFinestNodes + nx,
                                              tz * tileFinestNodes + nz,
                                              minH, maxH);
                }
            }
        }
    }

    globalQT.BuildCoarseLevelsFromChildren();

    // Run selection with tile-clamped vis ranges.
    float32 visRanges[kMaxLODLevels]{};
    float32 morphStart[kMaxLODLevels]{};
    float32 morphEnd[kMaxLODLevels]{};
    {
        const float32 patchSize = totalWorldSize / static_cast<float32>(globalFinestNodes);
        const float32 baseRange = patchSize * 2.0f;
        constexpr float32 kMorphStartRatio = 0.66f;

        for (uint32 i = 0; i + 1 < tileLODLevels && i < globalLevels; ++i)
            visRanges[i] = CDLODSelection::ComputeLODRange(i, baseRange, 2.0f);

        for (uint32 i = (tileLODLevels > 0 ? tileLODLevels - 1 : 0); i < globalLevels; ++i)
            visRanges[i] = std::numeric_limits<float32>::max();

        float32 prevStart = 0.0f;
        for (uint32 i = 0; i < globalLevels; ++i)
        {
            if (visRanges[i] < 1e30f)
            {
                morphEnd[i] = visRanges[i];
                morphStart[i] = prevStart + (morphEnd[i] - prevStart) * kMorphStartRatio;
                prevStart = morphStart[i];
            }
            else
            {
                morphStart[i] = std::numeric_limits<float32>::max();
                morphEnd[i] = std::numeric_limits<float32>::max();
            }
        }
    }

    CDLODSelection selection;
    CDLODSelectionParams params{};
    params.CameraPosition = Mathematics::Vector3(128, 50, 128);
    params.WorldSizeX = totalWorldSize;
    params.WorldSizeZ = totalWorldSize;
    params.HeightScale = 128.0f;
    params.LODRangeScale = 2.0f;
    params.PatchGridSize = 32;
    params.PrecomputedVisRanges = visRanges;
    params.PrecomputedMorphStart = morphStart;
    params.PrecomputedMorphEnd = morphEnd;

    std::vector<CDLODPatch> patches;
    selection.Select(globalQT, params, patches);
    ASSERT_GT(patches.size(), 0u);

    // Verify every patch fits within one tile (no multi-tile patches).
    const float32 maxPatchScale = tileWorldSize + 0.01f;
    for (const auto& p : patches)
    {
        EXPECT_LE(p.Scale, maxPatchScale)
            << "Patch at LOD=" << p.LODLevel << " has scale=" << p.Scale
            << " which exceeds tile size " << tileWorldSize;
    }

    // Verify gapless coverage.
    const uint32 gridRes = globalFinestNodes * 2;
    const float32 cellSize = totalWorldSize / static_cast<float32>(gridRes);
    std::vector<uint32> coverage(gridRes * gridRes, 0);
    for (const auto& patch : patches)
    {
        const float32 halfScale = patch.Scale * 0.5f;
        struct Quad { float32 mnX, mnZ, mxX, mxZ; };
        Quad quads[4] = {
            {patch.WorldX, patch.WorldZ, patch.WorldX+halfScale, patch.WorldZ+halfScale},
            {patch.WorldX+halfScale, patch.WorldZ, patch.WorldX+patch.Scale, patch.WorldZ+halfScale},
            {patch.WorldX, patch.WorldZ+halfScale, patch.WorldX+halfScale, patch.WorldZ+patch.Scale},
            {patch.WorldX+halfScale, patch.WorldZ+halfScale, patch.WorldX+patch.Scale, patch.WorldZ+patch.Scale},
        };
        for (uint32 q = 0; q < 4; ++q)
        {
            if (!(patch.SubQuadFlags & (1u << q))) continue;
            int32 sx = std::max(0, static_cast<int32>(std::floor(quads[q].mnX / cellSize)));
            int32 sz = std::max(0, static_cast<int32>(std::floor(quads[q].mnZ / cellSize)));
            int32 ex = std::min(static_cast<int32>(gridRes), static_cast<int32>(std::ceil(quads[q].mxX / cellSize)));
            int32 ez = std::min(static_cast<int32>(gridRes), static_cast<int32>(std::ceil(quads[q].mxZ / cellSize)));
            for (int32 z = sz; z < ez; ++z)
                for (int32 x = sx; x < ex; ++x)
                    coverage[z * gridRes + x]++;
        }
    }

    uint32 gaps = 0;
    for (auto c : coverage)
        if (c == 0) ++gaps;
    EXPECT_EQ(gaps, 0u) << "Found " << gaps << " uncovered cells in global quadtree selection";
}

// ---------------------------------------------------------------------------
// CDLODQuadtree::PatchRegion + TryGetGlobalHeightRange (region-scoped rebake)
// ---------------------------------------------------------------------------

// After a sub-rect of the heightfield changes, PatchRegion must produce a tree
// bit-identical to a fresh full Build of the mutated field: only the finest
// nodes over the rect are re-scanned, and the coarse levels are re-derived, so
// untouched nodes keep their (still-correct) values.
TEST(CDLODQuadtreePatchable, PatchRegionMatchesFullBuildAfterEdit)
{
    HeightfieldData hf(129, 129, 0.0f);
    hf.FillWithNoise(4.0f, 1.0f, 4, 7);

    CDLODQuadtree qtPatched;
    qtPatched.Build(hf, 4); // finest 8x8, samplesPerNode 16

    // Mutate an interior rect with divergent per-sample content (a tilted bump),
    // so the touched nodes' min/max genuinely move in both directions.
    const int32 rMinX = 40, rMinZ = 50, rMaxX = 70, rMaxZ = 78;
    for (int32 z = rMinZ; z <= rMaxZ; ++z)
        for (int32 x = rMinX; x <= rMaxX; ++x)
            hf.SetSample(static_cast<uint32>(x), static_cast<uint32>(z),
                         5.0f + 0.01f * static_cast<float32>(x + z));

    qtPatched.PatchRegion(hf, rMinX, rMinZ, rMaxX, rMaxZ);

    CDLODQuadtree qtFull;
    qtFull.Build(hf, 4);

    for (uint32 level = 0; level < qtFull.GetNumLevels(); ++level)
    {
        const uint32 n = qtFull.GetNodesPerAxisAtLevel(level);
        for (uint32 z = 0; z < n; ++z)
            for (uint32 x = 0; x < n; ++x)
            {
                const auto& a = qtFull.GetNode(level, x, z);
                const auto& b = qtPatched.GetNode(level, x, z);
                EXPECT_FLOAT_EQ(a.MinHeight, b.MinHeight)
                    << "min mismatch at level " << level << " (" << x << "," << z << ")";
                EXPECT_FLOAT_EQ(a.MaxHeight, b.MaxHeight)
                    << "max mismatch at level " << level << " (" << x << "," << z << ")";
            }
    }
}

// A finest node covers [n*spn, n*spn+spn] inclusive, so it SHARES its edge
// samples with the adjacent node. An edit to a single boundary sample must
// update BOTH nodes that share it — real divergent content (a spike well above
// the noise band), no analytic agreement — and the patched tree must match a
// full rebuild exactly, spike reaching the root.
TEST(CDLODQuadtreePatchable, PatchRegionUpdatesBoundarySharedNodes)
{
    HeightfieldData hf(65, 65, 0.0f);
    hf.FillWithNoise(6.0f, 1.0f, 3, 11);

    CDLODQuadtree qtPatched;
    qtPatched.Build(hf, 4); // finest 8x8, samplesPerNode 8 -> node edges at x=8,16,24,...

    // Sample x=16 is shared by finest nodes nx=1 ([8,16]) and nx=2 ([16,24]).
    const int32 bx = 16, bz = 16;
    hf.SetSample(static_cast<uint32>(bx), static_cast<uint32>(bz), 9.0f);

    qtPatched.PatchRegion(hf, bx, bz, bx, bz);

    CDLODQuadtree qtFull;
    qtFull.Build(hf, 4);

    for (uint32 level = 0; level < qtFull.GetNumLevels(); ++level)
    {
        const uint32 n = qtFull.GetNodesPerAxisAtLevel(level);
        for (uint32 z = 0; z < n; ++z)
            for (uint32 x = 0; x < n; ++x)
                EXPECT_FLOAT_EQ(qtFull.GetNode(level, x, z).MaxHeight,
                                qtPatched.GetNode(level, x, z).MaxHeight)
                    << "boundary spike missed at level " << level << " (" << x << "," << z << ")";
    }

    float32 rmin, rmax;
    ASSERT_TRUE(qtPatched.TryGetGlobalHeightRange(hf.GetWidth(), hf.GetHeight(), rmin, rmax));
    EXPECT_FLOAT_EQ(rmax, 9.0f) << "the coarse propagation did not carry the spike to the root";
}

// The root node's min/max equals a full-heightfield scan when the finest level
// tiles the field exactly, before AND after a region edit; and the coverage
// guard returns false for a resolution the finest level can't tile (so the
// caller falls back to a full scan rather than trust an edge-omitting root).
TEST(CDLODQuadtreePatchable, TryGetGlobalHeightRangeMatchesScanAndGuardsCoverage)
{
    HeightfieldData hf(129, 129, 0.0f);
    hf.FillWithNoise(4.0f, 1.0f, 4, 3);

    CDLODQuadtree qt;
    qt.Build(hf, 4);

    auto fullScan = [&](float32& mn, float32& mx) {
        hf.GetMinMax(0, 0, static_cast<int32>(hf.GetWidth()), static_cast<int32>(hf.GetHeight()), mn, mx);
    };

    float32 scanMin, scanMax, rMin, rMax;
    fullScan(scanMin, scanMax);
    ASSERT_TRUE(qt.TryGetGlobalHeightRange(hf.GetWidth(), hf.GetHeight(), rMin, rMax));
    EXPECT_FLOAT_EQ(rMin, scanMin);
    EXPECT_FLOAT_EQ(rMax, scanMax);

    // Region edit that pushes the global min below the noise band.
    for (int32 z = 20; z <= 30; ++z)
        for (int32 x = 20; x <= 30; ++x)
            hf.SetSample(static_cast<uint32>(x), static_cast<uint32>(z), -2.0f);
    qt.PatchRegion(hf, 20, 20, 30, 30);
    fullScan(scanMin, scanMax);
    ASSERT_TRUE(qt.TryGetGlobalHeightRange(hf.GetWidth(), hf.GetHeight(), rMin, rMax));
    EXPECT_FLOAT_EQ(rMin, scanMin);
    EXPECT_FLOAT_EQ(rMax, scanMax);
    EXPECT_FLOAT_EQ(rMin, -2.0f);

    // Coverage guard: finest=8, (101-1)/8=12, 8*12=96 != 100 -> incomplete tiling.
    EXPECT_FALSE(qt.TryGetGlobalHeightRange(101, 101, rMin, rMax));
}
