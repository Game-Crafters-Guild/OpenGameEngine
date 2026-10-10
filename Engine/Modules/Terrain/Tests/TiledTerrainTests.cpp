#include "TerrainECS/TerrainService.h"
#include "Terrain/TerrainTypes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <utility>

using namespace GameEngine;
using namespace GameEngine::Terrain;
using namespace GameEngine::TerrainECS;

class TiledTerrainFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TerrainService::Initialize();
    }

    void TearDown() override
    {
        TerrainService::Shutdown();
    }
};

// ---------------------------------------------------------------------------
// TiledTerrainConfig derivation
// ---------------------------------------------------------------------------

TEST_F(TiledTerrainFixture, CreateTiledTerrain_ComputesTileConfig)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 256.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    ASSERT_NE(handle, (TiledTerrainHandle{}));

    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    EXPECT_GT(data->Config.TileWorldSize, 0.0f);
    EXPECT_GT(data->Config.TilesPerAxisX, 0u);
    EXPECT_GT(data->Config.TilesPerAxisZ, 0u);
    EXPECT_GT(data->Config.TileConfig.LODLevels, 0u);
    EXPECT_GT(data->Config.TileConfig.HeightmapWidth, 3u);
    EXPECT_GT(data->Config.StreamingRadius, 0.0f);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, CreateTiledTerrain_MultiTile)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 8192.0f;
    config.WorldSizeZ = 8192.0f;
    config.HeightScale = 256.0f;
    config.SamplesPerMeter = 2.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    EXPECT_GE(data->Config.TilesPerAxisX, 2u);
    EXPECT_GE(data->Config.TilesPerAxisZ, 2u);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// ---------------------------------------------------------------------------
// Tile loading / unloading
// ---------------------------------------------------------------------------

TEST_F(TiledTerrainFixture, LoadTile_CreatesHeightfield)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    TileCoord coord{0, 0};
    EXPECT_FALSE(TerrainService::Get().IsTileLoaded(handle, coord));

    auto* tile = TerrainService::Get().LoadTile(handle, coord);
    ASSERT_NE(tile, nullptr);
    EXPECT_TRUE(TerrainService::Get().IsTileLoaded(handle, coord));

    EXPECT_FALSE(tile->Heightfield.IsEmpty());
    EXPECT_TRUE(tile->HeightfieldDirty);
    EXPECT_GT(tile->SplatmapWidth, 0u);
    EXPECT_FALSE(tile->Splatmap.empty());

    // Tile world origin should match its grid position.
    EXPECT_FLOAT_EQ(tile->WorldOriginX, data->WorldOriginX);
    EXPECT_FLOAT_EQ(tile->WorldOriginZ, data->WorldOriginZ);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, RevisionTracksTileMutations)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->Revision, 0u);

    auto* tile = TerrainService::Get().LoadTile(handle, {0, 0});
    ASSERT_NE(tile, nullptr);
    const uint64 afterLoad = data->Revision;
    EXPECT_GT(afterLoad, 0u);

    HeightfieldData replacement;
    replacement.Resize(tile->Config.HeightmapWidth, tile->Config.HeightmapHeight, 1.0f);
    TerrainService::Get().SetTileHeightfield(handle, {0, 0}, std::move(replacement), 1.0f, 1.0f);
    EXPECT_GT(data->Revision, afterLoad);
    const uint64 afterHeightfield = data->Revision;

    TerrainService::Get().UnloadTile(handle, {0, 0});
    EXPECT_GT(data->Revision, afterHeightfield);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, LoadTile_DifferentCoords_DifferentOrigins)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 8192.0f;
    config.WorldSizeZ = 8192.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 2.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_GE(data->Config.TilesPerAxisX, 2u);

    auto* tile00 = TerrainService::Get().LoadTile(handle, {0, 0});
    auto* tile10 = TerrainService::Get().LoadTile(handle, {1, 0});
    ASSERT_NE(tile00, nullptr);
    ASSERT_NE(tile10, nullptr);

    // Tile (1,0) should be offset by one tile world size in X.
    EXPECT_FLOAT_EQ(tile10->WorldOriginX, tile00->WorldOriginX + data->Config.TileWorldSize);
    EXPECT_FLOAT_EQ(tile10->WorldOriginZ, tile00->WorldOriginZ);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, UnloadTile_RemovesTile)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    TerrainService::Get().LoadTile(handle, {0, 0});
    EXPECT_TRUE(TerrainService::Get().IsTileLoaded(handle, {0, 0}));

    TerrainService::Get().UnloadTile(handle, {0, 0});
    EXPECT_FALSE(TerrainService::Get().IsTileLoaded(handle, {0, 0}));

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, LoadTile_OutOfBounds_ReturnsNull)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    EXPECT_EQ(TerrainService::Get().LoadTile(handle, {-1, 0}), nullptr);
    EXPECT_EQ(TerrainService::Get().LoadTile(handle, {static_cast<int32>(data->Config.TilesPerAxisX), 0}), nullptr);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, LoadTile_DuplicateLoad_ReturnsSamePointer)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* tile1 = TerrainService::Get().LoadTile(handle, {0, 0});
    auto* tile2 = TerrainService::Get().LoadTile(handle, {0, 0});

    EXPECT_EQ(tile1, tile2) << "Loading same tile twice should return the same pointer";

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// ---------------------------------------------------------------------------
// Global quadtree build from tiles
// ---------------------------------------------------------------------------

TEST_F(TiledTerrainFixture, RebuildGlobalQuadtree_ProducesValidQuadtree)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 4096.0f;
    config.WorldSizeZ = 4096.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    // Load all tiles (4x4 grid).
    for (uint32 z = 0; z < data->Config.TilesPerAxisZ; ++z)
        for (uint32 x = 0; x < data->Config.TilesPerAxisX; ++x)
            ASSERT_NE(TerrainService::Get().LoadTile(handle, {static_cast<int32>(x), static_cast<int32>(z)}), nullptr);

    TerrainService::Get().RebuildGlobalQuadtree(handle);
    EXPECT_TRUE(data->GlobalQuadtree.IsBuilt());

    // Global LOD levels should be more than per-tile levels.
    EXPECT_GT(data->GlobalQuadtree.GetNumLevels(), data->Config.TileConfig.LODLevels);

    // Root node should span the entire terrain's height range.
    const uint32 rootLevel = data->GlobalQuadtree.GetNumLevels() - 1;
    const auto& root = data->GlobalQuadtree.GetNode(rootLevel, 0, 0);
    EXPECT_LE(root.MinHeight, root.MaxHeight);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// Verify that adjacent tiles produce identical heightfield values at their
// shared boundary. World-space noise is deterministic per world position,
// so the last column of tile (0,0) must match the first column of tile (1,0).
TEST_F(TiledTerrainFixture, AdjacentTiles_HeightfieldBoundaryMatch)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 2048.0f;
    config.WorldSizeZ = 2048.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 2.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_GE(data->Config.TilesPerAxisX, 2u);

    auto* tile00 = TerrainService::Get().LoadTile(handle, {0, 0});
    auto* tile10 = TerrainService::Get().LoadTile(handle, {1, 0});
    ASSERT_NE(tile00, nullptr);
    ASSERT_NE(tile10, nullptr);

    const uint32 w = tile00->Heightfield.GetWidth();
    const uint32 h = tile00->Heightfield.GetHeight();

    // Right edge of tile(0,0) == left edge of tile(1,0)
    uint32 mismatches = 0;
    for (uint32 z = 0; z < h; ++z)
    {
        const float32 a = tile00->Heightfield.GetSample(w - 1, z);
        const float32 b = tile10->Heightfield.GetSample(0, z);
        if (std::abs(a - b) > 1e-6f)
        {
            ++mismatches;
            if (mismatches <= 3)
                ADD_FAILURE() << "Height mismatch at z=" << z << ": tile(0,0)[" << (w-1) << "]="
                              << a << " vs tile(1,0)[0]=" << b;
        }
    }
    EXPECT_EQ(mismatches, 0u) << "Found " << mismatches << " boundary height mismatches";

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// Verify that adjacent tiles also match on the Z boundary.
TEST_F(TiledTerrainFixture, AdjacentTiles_HeightfieldBoundaryMatch_Z)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 2048.0f;
    config.WorldSizeZ = 2048.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 2.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_GE(data->Config.TilesPerAxisZ, 2u);

    auto* tile00 = TerrainService::Get().LoadTile(handle, {0, 0});
    auto* tile01 = TerrainService::Get().LoadTile(handle, {0, 1});
    ASSERT_NE(tile00, nullptr);
    ASSERT_NE(tile01, nullptr);

    const uint32 w = tile00->Heightfield.GetWidth();
    const uint32 h = tile00->Heightfield.GetHeight();

    // Bottom edge of tile(0,0) == top edge of tile(0,1)
    uint32 mismatches = 0;
    for (uint32 x = 0; x < w; ++x)
    {
        const float32 a = tile00->Heightfield.GetSample(x, h - 1);
        const float32 b = tile01->Heightfield.GetSample(x, 0);
        if (std::abs(a - b) > 1e-6f)
            ++mismatches;
    }
    EXPECT_EQ(mismatches, 0u) << "Found " << mismatches << " Z-boundary height mismatches";

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// Verify deterministic noise: same tile coord -> same heightfield data.
TEST_F(TiledTerrainFixture, LoadTile_DeterministicNoise)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 128.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);

    auto* tile1 = TerrainService::Get().LoadTile(handle, {0, 0});
    ASSERT_NE(tile1, nullptr);
    float32 sample1 = tile1->Heightfield.GetSample(10, 10);

    // Unload and reload — should get the same noise.
    TerrainService::Get().UnloadTile(handle, {0, 0});
    auto* tile2 = TerrainService::Get().LoadTile(handle, {0, 0});
    ASSERT_NE(tile2, nullptr);
    float32 sample2 = tile2->Heightfield.GetSample(10, 10);

    EXPECT_FLOAT_EQ(sample1, sample2) << "Noise should be deterministic per tile coord";

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// ---------------------------------------------------------------------------
// C8: tiled height-query parity (SampleTiledHeightNormalized)
//
// The editor terrain brush/zone ray-march samples height through this helper for
// tiled terrains. It must select the resident tile containing a world point and
// return its heightfield value, matching CPU truth and staying continuous across
// tile boundaries — otherwise brush/zone strokes never hit a tiled terrain.
// ---------------------------------------------------------------------------

namespace
{
// A 2x2-resident tiled terrain: 4096m @ 1 sample/m -> 4x4 grid of 1025-sample,
// 1024m tiles. Loads the (0,0)..(1,1) block and marks it Full (LoadTile leaves
// LodState Empty; production streams tiles to Full — mirror that here so the
// resident-tile gates apply).
TiledTerrainHandle MakeResident2x2(float32& outTileWorld, uint32& outTileRes)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = 4096.0f;
    config.WorldSizeZ = 4096.0f;
    config.HeightScale = 200.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    if (!data)
        return {};
    outTileWorld = data->Config.TileWorldSize;
    outTileRes = data->Config.TileConfig.HeightmapWidth;
    for (int32 tz = 0; tz < 2; ++tz)
    for (int32 tx = 0; tx < 2; ++tx)
    {
        auto* tile = TerrainService::Get().LoadTile(handle, {tx, tz});
        if (tile)
            tile->LodState = TileLodState::Full;
    }
    return handle;
}
} // namespace

TEST_F(TiledTerrainFixture, SampleTiledHeight_MatchesTileCpuTruth)
{
    float32 tileWorld = 0.0f;
    uint32 tileRes = 0;
    auto handle = MakeResident2x2(tileWorld, tileRes);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    const float32 spacing = tileWorld / static_cast<float32>(tileRes - 1);

    // Sample at grid-aligned world points (bilinear collapses to GetSample there)
    // spanning tiles (0,0) and (1,1); compare to the owning tile's CPU sample.
    struct Probe { int32 tx, tz, sx, sz; };
    const Probe probes[] = {{0, 0, 0, 0}, {0, 0, 100, 40}, {1, 0, 200, 10},
                            {0, 1, 5, 300}, {1, 1, 512, 512}};
    for (const auto& p : probes)
    {
        auto* tile = TerrainService::Get().LoadTile(handle, {p.tx, p.tz});
        ASSERT_NE(tile, nullptr);
        const float32 worldX = tile->WorldOriginX + static_cast<float32>(p.sx) * spacing;
        const float32 worldZ = tile->WorldOriginZ + static_cast<float32>(p.sz) * spacing;

        float32 h = -1.0f;
        ASSERT_TRUE(SampleTiledHeightNormalized(*data, worldX, worldZ, h))
            << "tile (" << p.tx << "," << p.tz << ") sample (" << p.sx << "," << p.sz << ")";
        EXPECT_FLOAT_EQ(h, tile->Heightfield.GetSample(p.sx, p.sz));
    }

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, SampleTiledHeight_ContinuousAcrossTileBoundary)
{
    float32 tileWorld = 0.0f;
    uint32 tileRes = 0;
    auto handle = MakeResident2x2(tileWorld, tileRes);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    // At the X boundary between tile (0,0) and (1,0): the query resolves to tile
    // (1,0) local u=0, whose edge samples equal tile (0,0)'s far edge by
    // world-space noise coherence. Sampling just-inside each side must agree.
    const float32 boundaryX = data->WorldOriginX + tileWorld;
    const float32 spacing = tileWorld / static_cast<float32>(tileRes - 1);
    for (uint32 sz = 0; sz < tileRes; sz += 128)
    {
        const float32 worldZ = data->WorldOriginZ + static_cast<float32>(sz) * spacing;

        float32 hAt = -1.0f, hLeft = -1.0f;
        ASSERT_TRUE(SampleTiledHeightNormalized(*data, boundaryX, worldZ, hAt));
        ASSERT_TRUE(SampleTiledHeightNormalized(*data, boundaryX - spacing * 0.001f, worldZ, hLeft));
        // Continuous: a hair to the left (still tile (0,0)) is ~equal to the boundary.
        EXPECT_NEAR(hAt, hLeft, 1e-3f) << "z sample " << sz;
    }

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// C8 render-extent oracle: the CBT render size (params/info WorldSize) must be the REAL
// per-axis extent (tilesPerAxis * tileWorldSize), NOT the power-of-2 square the global
// quadtree rounds up to. A non-square / non-pow2-tile-count terrain rendered with the
// square would stretch (CBT maps world = origin + uv*size over the unified heightmap).
TEST_F(TiledTerrainFixture, RenderExtent_NonSquareUsesRealPerAxisExtent)
{
    // 4096 x 2048 @ 1 sample/m -> 1024m tiles -> 4 x 2 tiles (a non-square grid).
    TiledTerrainConfig config{};
    config.WorldSizeX = 4096.0f;
    config.WorldSizeZ = 2048.0f;
    config.HeightScale = 200.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->Config.TilesPerAxisX, data->Config.TilesPerAxisZ) << "expected a non-square tile grid";

    const TiledRenderExtent e = ComputeTiledRenderExtent(data->Config);
    // Real extent = tilesPerAxis * tileWorldSize, DISTINCT per axis (the bug collapsed
    // both to one square virtual size).
    EXPECT_FLOAT_EQ(e.WorldSizeX, data->Config.TilesPerAxisX * data->Config.TileWorldSize);
    EXPECT_FLOAT_EQ(e.WorldSizeZ, data->Config.TilesPerAxisZ * data->Config.TileWorldSize);
    EXPECT_NE(e.WorldSizeX, e.WorldSizeZ) << "render extent collapsed a non-square terrain to a square";
    // Unified resolution matches an untiled terrain of equal content, per axis.
    EXPECT_EQ(e.UnifiedWidth, data->Config.TilesPerAxisX * (data->Config.TileConfig.HeightmapWidth - 1) + 1);
    EXPECT_EQ(e.UnifiedHeight, data->Config.TilesPerAxisZ * (data->Config.TileConfig.HeightmapHeight - 1) + 1);
    EXPECT_NE(e.UnifiedWidth, e.UnifiedHeight);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, RenderExtent_NonPow2TileCountIsNotRoundedUp)
{
    // 3072 x 3072 @ 1 sample/m -> 1024m tiles -> 3 x 3 tiles. The global quadtree rounds
    // 3 up to 4 finest-node blocks; the render extent must stay 3*tileWorldSize, not 4.
    TiledTerrainConfig config{};
    config.WorldSizeX = 3072.0f;
    config.WorldSizeZ = 3072.0f;
    config.HeightScale = 200.0f;
    config.SamplesPerMeter = 1.0f;
    config.PatchGridSize = 32;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);
    ASSERT_EQ(data->Config.TilesPerAxisX, 3u);

    const TiledRenderExtent e = ComputeTiledRenderExtent(data->Config);
    const float32 realX = 3.0f * data->Config.TileWorldSize; // 3072
    EXPECT_FLOAT_EQ(e.WorldSizeX, realX);
    // The pow2-rounded quadtree square (4 tiles wide) would be 4/3 too large — reject it.
    EXPECT_LT(e.WorldSizeX, 4.0f * data->Config.TileWorldSize);

    TerrainService::Get().DestroyTiledTerrain(handle);
}

TEST_F(TiledTerrainFixture, SampleTiledHeight_MissesOutsideAndUnloaded)
{
    float32 tileWorld = 0.0f;
    uint32 tileRes = 0;
    auto handle = MakeResident2x2(tileWorld, tileRes);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    ASSERT_NE(data, nullptr);

    float32 h = 0.0f;
    // Left/below the terrain origin: outside the footprint.
    EXPECT_FALSE(SampleTiledHeightNormalized(*data, data->WorldOriginX - 10.0f,
                                             data->WorldOriginZ + 10.0f, h));
    // Inside the grid but in a tile that was never loaded (tile (3,3)).
    const float32 farX = data->WorldOriginX + 3.5f * tileWorld;
    const float32 farZ = data->WorldOriginZ + 3.5f * tileWorld;
    EXPECT_FALSE(SampleTiledHeightNormalized(*data, farX, farZ, h));

    TerrainService::Get().DestroyTiledTerrain(handle);
}

// ---------------------------------------------------------------------------
// Auto streaming radius is anchored in world metres, independent of texel density
// ---------------------------------------------------------------------------

namespace
{
// Create a tiled terrain of the given world size + SamplesPerMeter, read back the
// auto-derived streaming radius, and destroy it. StreamingRadius left 0 => auto.
float32 AutoStreamingRadius(float32 worldSize, float32 samplesPerMeter)
{
    TiledTerrainConfig config{};
    config.WorldSizeX = worldSize;
    config.WorldSizeZ = worldSize;
    config.HeightScale = 256.0f;
    config.SamplesPerMeter = samplesPerMeter;
    config.PatchGridSize = kDefaultGridSize;

    auto handle = TerrainService::Get().CreateTiledTerrain(config);
    auto* data = TerrainService::Get().GetTiledTerrainData(handle);
    EXPECT_NE(data, nullptr);
    const float32 radius = data ? data->Config.StreamingRadius : 0.0f;
    TerrainService::Get().DestroyTiledTerrain(handle);
    return radius;
}
} // namespace

// Fails before the fix: the old derivation scaled the window with tileWorld
// (= (kMaxTileResolution-1)/spm), so a higher SPM HALVED the auto radius on a
// fixed-size terrain (e.g. 4096 m: 3072 -> 1536 m). The window is a view-distance
// concept and must not move with texel density.
TEST_F(TiledTerrainFixture, AutoStreamingRadius_IndependentOfSamplesPerMeter)
{
    for (const float32 worldSize : {2048.0f, 4096.0f, 6144.0f})
    {
        const float32 r1 = AutoStreamingRadius(worldSize, 1.0f);
        const float32 r2 = AutoStreamingRadius(worldSize, 2.0f);
        const float32 r4 = AutoStreamingRadius(worldSize, 4.0f);
        EXPECT_GT(r1, 0.0f) << "worldSize=" << worldSize;
        EXPECT_FLOAT_EQ(r1, r2) << "worldSize=" << worldSize << " spm 1 vs 2";
        EXPECT_FLOAT_EQ(r1, r4) << "worldSize=" << worldSize << " spm 1 vs 4";
    }
}

// Residency / no-tear oracle for a 4096 m terrain with a centered camera.
// The auto window must reach every corner of the terrain at BOTH SPM so raising
// SamplesPerMeter never drops a previously-resident band (the sky-through-terrain
// tear). Before the fix, SPM 2 => radius 1536 m < 2896 m half-diagonal (fails).
TEST_F(TiledTerrainFixture, AutoStreamingRadius_CoversTerrainDiagonalAcrossSpm)
{
    constexpr float32 kWorldSize = 4096.0f;
    const float32 halfDiagonal = 0.5f * kWorldSize * std::sqrt(2.0f); // ~2896 m from centre
    const float32 r1 = AutoStreamingRadius(kWorldSize, 1.0f);
    const float32 r2 = AutoStreamingRadius(kWorldSize, 2.0f);
    EXPECT_GE(r1, halfDiagonal);
    EXPECT_GE(r2, halfDiagonal); // the residency band that tore stays covered at SPM 2
}
