#include "TerrainECS/TerrainSizingPlan.h"

#include "CBTTerrain/CBTLayout.h"
#include "TerrainECS/TerrainAtlas.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS
{

static_assert(kMaxTerrainTilesPerAxis * kMaxTerrainTilesPerAxis == CBTTerrain::kAtlasMaxTiles,
              "kMaxTerrainTilesPerAxis must cover exactly the CBT atlas's per-tile table");

namespace
{

// The sparsest density a terrain is derived at: below it the tile's world size runs away.
constexpr float32 kMinSamplesPerMeter = 0.01f;

} // namespace

float32 MaxTerrainExtentMetres(float32 samplesPerMeter)
{
    return static_cast<float32>(kMaxTerrainTilesPerAxis) *
           static_cast<float32>(Terrain::kMaxTileResolution - 1) / std::max(samplesPerMeter, kMinSamplesPerMeter);
}

float32 MaxTerrainSamplesPerMeter(float32 sizeX, float32 sizeZ)
{
    return static_cast<float32>(kMaxTerrainTilesPerAxis) *
           static_cast<float32>(Terrain::kMaxTileResolution - 1) / std::max(sizeX, sizeZ);
}

TerrainSizingPlan DeriveTerrainSizingPlan(float32 worldSizeX, float32 worldSizeZ,
                                          float32 samplesPerMeter, uint32 patchGridSize,
                                          float32 lodRangeScale)
{
    TerrainSizingPlan plan;
    if (worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
        return plan;

    samplesPerMeter = std::max(samplesPerMeter, kMinSamplesPerMeter);
    patchGridSize = std::clamp(patchGridSize, Terrain::kMinPatchSize, Terrain::kMaxPatchSize);

    // Tile geometry is derived UNCONDITIONALLY, even for a terrain that will not tile: it is the
    // grid TerrainService::CreateTiledTerrain builds, and that entry point is reachable with a
    // sub-cap config (the tiled-terrain tests call it directly). Deriving it only on the tiling
    // branch would have silently changed a non-square sub-cap terrain's tile size from the
    // shorter axis to the longer one. Square tiles, capped at the per-tile sample ceiling, so the
    // tile's world size falls out of the cap and the density.
    const float32 maxTileWorld =
        static_cast<float32>(Terrain::kMaxTileResolution - 1) / samplesPerMeter;
    const float32 tileWorld = std::min({worldSizeX, worldSizeZ, maxTileWorld});
    const Terrain::TerrainConfig tile = Terrain::TerrainConfig::FromSamplesPerMeter(
        tileWorld, tileWorld, /*heightScale*/ 0.0f, samplesPerMeter, patchGridSize, lodRangeScale);

    plan.TileWorldSize = tileWorld;
    plan.TileResolution = tile.HeightmapWidth;
    // Counted in float and saturated before the conversion: a side the float range allows but no
    // tile grid does (a scene file's, a component write's, a NaN) would overflow the cast and the
    // products.
    const auto tilesAlong = [tileWorld](float32 side) {
        const float32 tiles = std::ceil(side / tileWorld);
        return tiles < static_cast<float32>(kMaxDerivedTilesPerAxis) ? static_cast<uint32>(tiles)
                                                                     : kMaxDerivedTilesPerAxis;
    };
    plan.TilesPerAxisX = tilesAlong(worldSizeX);
    plan.TilesPerAxisZ = tilesAlong(worldSizeZ);
    plan.TotalTiles = std::max(1u, plan.TilesPerAxisX * plan.TilesPerAxisZ);

    const uint32 tileInterior = plan.TileResolution > 0 ? plan.TileResolution - 1u : 0u;
    plan.UnifiedWidth = plan.TilesPerAxisX * tileInterior + 1u;
    plan.UnifiedHeight = plan.TilesPerAxisZ * tileInterior + 1u;

    // Both of these MUST come from the tile, not from a whole-terrain config: past
    // kMaxHeightmapDimension the whole-terrain derivation clamps, which would under-report a
    // large terrain's true sample count and its actual near-field detail (an 8 km terrain at
    // 2 samples/m clamps to 8193 and reads 1 m/texel, when the tiles really deliver 0.5 m).
    plan.SampleResolution = std::max(plan.UnifiedWidth, plan.UnifiedHeight);
    plan.MetresPerTexelNear = tileWorld / static_cast<float32>(std::max(1u, tileInterior));

    if (!Terrain::TerrainNeedsTiling(worldSizeX, worldSizeZ, samplesPerMeter))
    {
        // One heightmap over the whole terrain, always live. Its resolution follows the LONGER
        // axis (FromSamplesPerMeter derives one square grid from it), which is not what the tile
        // grid above describes for a non-square terrain — so these are re-derived rather than
        // inherited.
        const Terrain::TerrainConfig whole = Terrain::TerrainConfig::FromSamplesPerMeter(
            worldSizeX, worldSizeZ, /*heightScale*/ 0.0f, samplesPerMeter, patchGridSize,
            lodRangeScale);
        plan.Source = TerrainHeightSource::Single;
        plan.SampleResolution = whole.HeightmapWidth;
        plan.MetresPerTexelNear = std::max(worldSizeX, worldSizeZ) /
                                  static_cast<float32>(std::max(1u, plan.SampleResolution - 1u));
        plan.ResidentSlots = plan.TotalTiles;
        plan.ResidentWindowMetres = std::max(worldSizeX, worldSizeZ);
        plan.MetresPerTexelFar = plan.MetresPerTexelNear;
        plan.PredictedVramBytes = static_cast<uint64>(plan.SampleResolution) *
                                  plan.SampleResolution * kAtlasSlotBytesPerTexel;
        return plan;
    }

    if (plan.UnifiedWidth <= kMaxUnifiedTiledResolution &&
        plan.UnifiedHeight <= kMaxUnifiedTiledResolution)
    {
        plan.Source = TerrainHeightSource::Unified;
        plan.ResidentSlots = plan.TotalTiles; // the whole terrain is one texture: all of it is live
        plan.ResidentWindowMetres = std::max(worldSizeX, worldSizeZ);
        plan.MetresPerTexelFar = plan.MetresPerTexelNear;
        plan.PredictedVramBytes = static_cast<uint64>(plan.UnifiedWidth) * plan.UnifiedHeight *
                                  kAtlasSlotBytesPerTexel;
        return plan;
    }

    plan.Source = TerrainHeightSource::Atlas;
    // The plan is derived from the terrain's own config, before any modifier volume exists,
    // so it prices the ground maps alone. A terrain that later carries a regional grass
    // control atlas is charged 2 more bytes per texel at residency time
    // (TerrainExtractionSystem), which buys fewer slots than this notice reports.
    plan.ResidentSlots = std::min(
        plan.TotalTiles,
        DeriveAtlasSlotCount(plan.TilesPerAxisX, plan.TilesPerAxisZ, plan.TileResolution,
                             kAtlasVramBudgetBytes, kAtlasSlotBytesPerTexel));
    // The window is square-packed, so its side in tiles is the floor of the slot count's root.
    const uint32 windowTiles = static_cast<uint32>(
        std::floor(std::sqrt(static_cast<double>(plan.ResidentSlots))));
    plan.ResidentWindowMetres = static_cast<float32>(windowTiles) * tileWorld;
    plan.MetresPerTexelFar = std::max(worldSizeX, worldSizeZ) /
                             static_cast<float32>(kAtlasCoarseFieldDim - 1u);

    const uint64 slotStride = plan.TileResolution + 2ull * kAtlasApron;
    const uint32 slotsPerRow = static_cast<uint32>(
        std::ceil(std::sqrt(static_cast<double>(std::max(1u, plan.ResidentSlots)))));
    const uint64 atlasDim = static_cast<uint64>(slotsPerRow) * slotStride;
    plan.PredictedVramBytes =
        atlasDim * atlasDim * kAtlasSlotBytesPerTexel +
        static_cast<uint64>(kAtlasCoarseFieldDim) * kAtlasCoarseFieldDim * kAtlasSlotBytesPerTexel;
    return plan;
}

} // namespace GameEngine::TerrainECS
