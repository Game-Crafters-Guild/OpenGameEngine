#pragma once

// What an authored terrain size + sample density actually cost, derived once. The extraction
// system builds these resources, the inspector must show the author their consequence BEFORE
// they commit to a value, and the tests pin the engagement threshold — all three read this one
// derivation, so no surface can predict a geometry the renderer will not build.
//
// Pure: no service, no device, no GPU, no allocation.

#include "Terrain/TerrainTypes.h"
#include "Types/Types.h"

namespace GameEngine::TerrainECS
{

// Which height source a terrain resolves through. The unified texture IS the whole terrain, so
// it stops being viable once the world outgrows one texture; the resident-window atlas is sized
// to the streaming window instead and takes over from exactly that point upward.
enum class TerrainHeightSource : uint32
{
    Single,  // one heightmap, terrain below the per-tile cap — no tiling, no streaming
    Unified, // tiled, one whole-terrain texture set
    Atlas,   // tiled, resident-window slot atlas + coarse far field
};

// The unified texture set is allocated at the terrain's full sample resolution, so this cap is
// what one texture can carry: 8 tiles of 1024 interior samples plus the shared edge. Past it the
// source would be tens of gigabytes.
inline constexpr uint32 kMaxUnifiedTiledResolution = 8193;

// The most tiles a planar terrain spans along one axis: the CBT atlas's per-tile table holds
// kMaxTerrainTilesPerAxis^2 rows (CBTTerrain::kAtlasMaxTiles, asserted equal in
// TerrainSizingPlan.cpp). A tile past it never becomes resident and draws the coarse
// field only; far past it the tile counts overflow.
inline constexpr uint32 kMaxTerrainTilesPerAxis = 128;

// The widest a planar terrain of this sample density may be along either axis, in metres:
// kMaxTerrainTilesPerAxis tiles of the per-tile sample cap (131072 m at 1 sample per metre).
float32 MaxTerrainExtentMetres(float32 samplesPerMeter);

// The densest a planar terrain this wide and deep may be sampled, in samples per metre: the
// inverse of MaxTerrainExtentMetres for the longer side.
float32 MaxTerrainSamplesPerMeter(float32 sizeX, float32 sizeZ);

// The most tiles per axis DeriveTerrainSizingPlan counts. A terrain past kMaxTerrainTilesPerAxis
// is refused at creation but still reaches the derivation from a scene file or a component write;
// saturating here keeps the tile product and the unified width inside uint32.
inline constexpr uint32 kMaxDerivedTilesPerAxis = 65535;

struct TerrainSizingPlan
{
    TerrainHeightSource Source = TerrainHeightSource::Single;

    // ---- Sampling ----
    uint32 SampleResolution = 0;   // samples per axis across the whole terrain
    float32 MetresPerTexelNear = 0.0f; // detail actually resolved where the source is resident

    // ---- Tiling ----
    uint32 TilesPerAxisX = 0;
    uint32 TilesPerAxisZ = 0;
    uint32 TileResolution = 0;     // samples per tile axis (interior + shared edge)
    float32 TileWorldSize = 0.0f;

    // ---- Unified source (Source == Unified) ----
    uint32 UnifiedWidth = 0;
    uint32 UnifiedHeight = 0;

    // ---- Resident window (Source == Atlas) ----
    uint32 TotalTiles = 0;
    uint32 ResidentSlots = 0;      // tiles held at full detail at once
    // Side of the square resident window, in metres. What "full detail near the camera" buys.
    float32 ResidentWindowMetres = 0.0f;
    // Everything outside the window resolves through one coarse field spanning the WHOLE
    // terrain, so this degrades linearly with world size — the island-scale limitation.
    float32 MetresPerTexelFar = 0.0f;

    uint64 PredictedVramBytes = 0; // height + splat + normal for the chosen source

    bool FullyResident() const { return TotalTiles > 0 && ResidentSlots >= TotalTiles; }
};

// Derive the plan for an authored size + density. patchGridSize / lodRangeScale mirror
// TerrainConfig's, so a caller holding a live TiledTerrainConfig passes its own values and the
// authoring surface passes the engine defaults.
TerrainSizingPlan DeriveTerrainSizingPlan(float32 worldSizeX, float32 worldSizeZ,
                                          float32 samplesPerMeter,
                                          uint32 patchGridSize = Terrain::kDefaultGridSize,
                                          float32 lodRangeScale = Terrain::kDefaultLODRangeScale);

} // namespace GameEngine::TerrainECS
