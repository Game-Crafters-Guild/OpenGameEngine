#pragma once

#include "TerrainECS/DirtyRegionLog.h"
#include "TerrainECS/TerrainBakeCache.h"
#include "TerrainECS/TerrainZonePayload.h"
#include "TerrainECS/TerrainGpuBake.h"
#include "TerrainECS/TerrainGrassField.h"
#include "TerrainECS/TerrainReprovisionDebounce.h" // TiledTerrainEdit
#include "CBTTerrain/SphereAnalyticModifiers.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "Terrain/TerrainTypes.h"
#include "Terrain/Heightfield.h"
#include "Terrain/CDLODQuadtree.h"
#include "AssetCore/AssetReloadInvalidator.h"
#include "AssetCore/GUID.h"
#include "PageStreaming/HeightPageOverlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace JobSystem
{
class JobChannel;
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine::Components
{
enum class TerrainBaseSource : uint8;
struct Terrain;
}

namespace GameEngine::TerrainECS
{

// Pre-computed quadtree node min/max for a single finest-level node.
// Computed on job threads to avoid scanning heightfield on the main thread.
struct NodeMinMax
{
    float32 MinH = 0.0f;
    float32 MaxH = 0.0f;
};

// Noise parameters of a tiled terrain's ProceduralNoise base: FillTiledBaseRegion's
// noise branch and the GPU height bake's base (TerrainModifierSystem), which must agree.
static constexpr float32 kTileNoiseFrequency = 0.004f;
static constexpr float32 kTileNoiseAmplitude = 1.0f;
static constexpr uint32  kTileNoiseOctaves   = 5;
static constexpr uint32  kTileNoiseSeed      = 42;

// Base noise parameters for single (non-tiled) terrain procedural fills.
// Shared by FillHeightfieldBaseRegion's noise branch so the modifier bake's
// region fills and the extraction creation fill stay per-sample identical.
static constexpr float32 kBaseNoiseFrequency = 4.0f;
static constexpr float32 kBaseNoiseAmplitude = 1.0f;
static constexpr uint32  kBaseNoiseOctaves   = 5;
static constexpr uint32  kBaseNoiseSeed      = 42;

// All CPU-side data for a single terrain.
struct TerrainData
{
    TerrainGrassField GrassField;
    Terrain::TerrainConfig Config;
    Terrain::HeightfieldData Heightfield;
    Terrain::CDLODQuadtree Quadtree;

    // ---- Splatmap (RGBA8: 4 material layer weights per texel) ----
    std::vector<uint8> Splatmap;          // RGBA8 interleaved, row-major
    uint32 SplatmapWidth = 0;
    uint32 SplatmapHeight = 0;
    bool SplatmapDirty = false;

    // Set by a full splat re-bake (ResetSplatmapAndCommitRange): every texel was
    // reset against the global height range, so the GPU upload must be
    // full-texture. A height-rect band is only valid for region re-bakes.
    // Cleared by extraction together with SplatmapDirty.
    bool SplatmapFullDirty = false;

    // Height range the current splatmap was generated with. Height-based layer
    // weights depend on the GLOBAL heightfield min/max, so a region splat regen
    // is only exact while that range is unchanged; when it changes, the whole
    // splatmap must be regenerated (TerrainModifierSystem checks this).
    float32 SplatBakeMinH = 0.0f;
    float32 SplatBakeMaxH = 0.0f;
    bool SplatBakeRangeValid = false;

    // Deferred whole-terrain splat renormalize (#526 contract, single-terrain
    // analogue of TiledTerrainData's fields). A height edit that shifts the
    // global range would, done eagerly, re-generate the entire splatmap every
    // dab (O(all-samples) — the dominant single-heightfield edit hitch). Instead
    // the touched region splats against the still-committed range (a consistent
    // preview) and this flags a pending flush; FlushDeferredSplatResplat runs the
    // one full renormalize against the FINAL range once editing goes quiescent,
    // so the settled splat is byte-identical to a fresh full bake.
    bool SplatResplatPending = false;
    uint32 SplatResplatIdleFrames = 0;

    // ---- Normal map (R16G16_FLOAT: XZ normal components, row-major) ----
    std::vector<uint8> Normalmap;
    uint32 NormalmapWidth = 0;
    uint32 NormalmapHeight = 0;

    // Reset the splatmap to unbaked (sized to the heightfield, all zero) and commit
    // the heightfield's global range as the splat bake range. The range is what the
    // HeightNormalized rule conditions normalize against, so it must be current
    // before the caller composites surface rules over the reset base.
    void ResetSplatmapAndCommitRange();

    // Cached per-LOD morph ranges (only depend on config, not per-frame data).
    float32 VisRanges[Terrain::kMaxLODLevels] = {};
    float32 MorphStart[Terrain::kMaxLODLevels] = {};
    float32 MorphEnd[Terrain::kMaxLODLevels] = {};
    bool MorphRangesComputed = false;

    // Monotonic counter incremented on each heightfield modification.
    // Used by PhysicsInitSystem (via HeightFieldColliderShape.lastBuiltVersion)
    // to detect when the collision shape needs rebuilding.
    uint64 HeightfieldVersion = 0;

    // Per-version dirty-region journal. Consumers (render extraction, the
    // physics heightfield gather) each keep their own cursor and call
    // CollectSince — nobody clears shared state, so consumers can't starve
    // each other regardless of frame order.
    DirtyRegionLog HeightfieldDirtyLog;

    // Mark the entire heightfield as needing re-upload.
    void MarkFullDirty()
    {
        ++HeightfieldVersion;
        HeightfieldDirtyLog.Append(HeightfieldVersion,
                                   {0, 0,
                                    static_cast<int32>(Heightfield.GetWidth()),
                                    static_cast<int32>(Heightfield.GetHeight())});
    }

    // Mark a sub-region as dirty. Max bounds are exclusive.
    void MarkRegionDirty(int32 minX, int32 minZ, int32 maxX, int32 maxZ)
    {
        ++HeightfieldVersion;
        HeightfieldDirtyLog.Append(HeightfieldVersion, {minX, minZ, maxX, maxZ});
    }
};

// ---- Tile types for tiled terrain ----

struct TileCoord
{
    int32 X = 0;
    int32 Z = 0;

    bool operator==(const TileCoord& o) const { return X == o.X && Z == o.Z; }
    bool operator!=(const TileCoord& o) const { return !(*this == o); }
    // Lexicographic order (Z major, X minor) so a baked-tile list can be
    // sorted + de-duplicated. Not a spatial ordering — just a total order.
    bool operator<(const TileCoord& o) const { return Z != o.Z ? Z < o.Z : X < o.X; }
};

struct TileCoordHash
{
    size_t operator()(const TileCoord& c) const
    {
        // Combine X and Z into a single hash (good for moderate grid sizes).
        auto h1 = std::hash<int32>{}(c.X);
        auto h2 = std::hash<int32>{}(c.Z);
        return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

// LOD state for async-streamed tiles.
enum class TileLodState : uint8
{
    Empty,   // Tile slot exists but has no heightfield data yet
    Coarse,  // Coarse (low-res) heightfield loaded, awaiting full detail
    Full     // Full-resolution heightfield + splatmap + normalmap loaded
};

// Per-tile data within a tiled terrain. Each tile has its own heightfield
// and splatmap, but shares the single global CDLOD quadtree for selection.
struct TerrainTileData
{
    TerrainGrassField GrassField;
    TileCoord Coord;
    float32 WorldOriginX = 0.0f;    // world-space corner of this tile
    float32 WorldOriginZ = 0.0f;

    Terrain::TerrainConfig Config;   // per-tile config (same resolution for all tiles)
    Terrain::HeightfieldData Heightfield;

    std::vector<uint8> Splatmap;
    uint32 SplatmapWidth = 0;
    uint32 SplatmapHeight = 0;
    bool SplatmapDirty = false;

    std::vector<uint8> Normalmap;
    uint32 NormalmapWidth = 0;
    uint32 NormalmapHeight = 0;
    // The HeightfieldVersion the Normalmap was generated for. The cook bakes the normal on the JOB
    // thread and integration sets this equal to HeightfieldVersion, so a freshly-streamed tile does
    // NOT re-generate the normal on the extraction thread (it only re-generates when a modifier bumps
    // HeightfieldVersion — heights actually changed). Guards the O(tileRes^2) extraction-thread stall.
    uint64 NormalmapVersion = 0;

    bool HeightfieldDirty = true;
    uint64 HeightfieldVersion = 0;

    // Per-version dirty-region journal for this tile's heightfield, consumed by
    // the per-tile physics collider (E6). Mirrors TerrainData::HeightfieldDirtyLog:
    // the collider keeps its own cursor and unions everything newer, so a region
    // bake that touches only this tile drives a region-scoped in-place collider
    // update. HeightfieldDirty (above) stays the whole-tile GPU-upload flag.
    DirtyRegionLog HeightfieldDirtyLog;

    // Cached per-tile height range for incremental global min/max tracking.
    // Updated by TileStreamingManager when heightfield data is integrated.
    float32 CachedMinH = 0.0f;
    float32 CachedMaxH = 0.0f;

    // Coarse per-block height min/max grid backing the incremental per-tile range.
    // A region edit rescans only the blocks its dirty rect overlaps (O(region)) and
    // the tile CachedMin/MaxH is the aggregate of every block (O(blocks)); a full
    // O(tile-samples) rescan is paid only when the grid is (re)built (first edit,
    // heightfield resize, streamed-in tile). This retires the ~4.6 ms/touched-tile
    // full rescan RefreshTileHeightRange ran per dab (the dominant sculpt-stroke
    // main-thread cost). Byte-identical to a full scan: blocks partition the tile
    // exactly, so unedited blocks keep their exact cached extremes and the aggregate
    // equals a full min/max. HeightBlockDim == 0 means "not built" (rebuild on next
    // range refresh). Mirrors the single-terrain PatchQuadtreeRegion incremental path.
    std::vector<float32> HeightBlockMinH;
    std::vector<float32> HeightBlockMaxH;
    uint32 HeightBlockDim = 0; // blocks per axis (0 = grid not built)

    // Pre-computed quadtree node min/max (populated by job threads via streaming manager).
    std::vector<NodeMinMax> PrecomputedQuadtreeNodes;
    uint32 PrecomputedQuadtreeNodesPerAxis = 0;

    // Async streaming LOD state. Only Full tiles patch the unified GPU source.
    TileLodState LodState = TileLodState::Empty;

    // False until the modifier bake has applied the authored modifiers to this
    // tile. A tile arrives from streaming with the terrain's base only (the streaming
    // job never sees modifiers), so the modifier system bakes it ONCE, scoped to
    // this tile (BakeTiledStreamedTiles) — rather than re-baking (and version-
    // bumping, hence collider re-cooking) every resident tile whenever one tile
    // streams in. Reset to false whenever streaming (re)sets this tile's
    // heightfield content (SetTileHeightfield); set true by the modifier bake.
    bool ModifiersApplied = false;

    // False until the surface rules have been composited over this tile's COARSE splat.
    // A coarse tile is the distant, still-streaming LOD, and every full-detail splat path
    // gates on LodState == Full — so without a coarse pass the tile shows the unbaked base
    // (channel 0) and then POPS to the ruled surface the moment it upgrades. Separate from
    // ModifiersApplied because that flag gates the FULL bake, which must still run on
    // upgrade. Reset with ModifiersApplied whenever streaming (re)sets this tile's content.
    bool CoarseSplatBaked = false;

    void MarkFullDirty()
    {
        ++HeightfieldVersion;
        HeightfieldDirty = true;
        HeightfieldDirtyLog.Append(HeightfieldVersion,
                                   {0, 0,
                                    static_cast<int32>(Heightfield.GetWidth()),
                                    static_cast<int32>(Heightfield.GetHeight())});
    }

    // Mark a sub-region of this tile's heightfield dirty (E6 region bake). Max
    // bounds are exclusive. Bumps the version + logs the rect for the collider,
    // and sets the whole-tile GPU-upload flag (tile textures upload whole today).
    void MarkRegionDirty(int32 minX, int32 minZ, int32 maxX, int32 maxZ)
    {
        ++HeightfieldVersion;
        HeightfieldDirty = true;
        HeightfieldDirtyLog.Append(HeightfieldVersion, {minX, minZ, maxX, maxZ});
    }

    void ClearDirty() { HeightfieldDirty = false; }
};

// The ground every tile of a tiled terrain starts from before the modifier stack
// (bake stage A): the terrain's Terrain.BaseSource and, for HeightmapAsset, the
// decoded heightmap. The heightmap is shared with TerrainService's decode cache, so
// a streaming job that captured it keeps it alive across a hot-reload eviction.
// TerrainService::ResolveTiledTerrainBase builds one, and
// TerrainService::IsTiledTerrainBaseCurrent tells when the authored base has moved on.
struct TiledTerrainBase
{
    Components::TerrainBaseSource Source{}; // value-initialized: ProceduralNoise
    // Null for HeightmapAsset when the asset is missing or failed to decode: the
    // base is then flat, as it is on an untiled terrain.
    std::shared_ptr<const Terrain::HeightfieldData> Heightmap;
    // What Heightmap was resolved from (HeightmapAsset only; null and 0 otherwise):
    // the asset and its content version (TerrainService::GetHeightmapContentVersion),
    // so a hot reload is seen without resolving the heightmap again.
    GUID HeightmapGuid;
    uint64 HeightmapContentVersion = 0;
};

// Configuration for a tiled terrain.
struct TiledTerrainConfig
{
    float32 WorldSizeX = 1024.0f;
    float32 WorldSizeZ = 1024.0f;
    float32 HeightScale = 256.0f;
    float32 SamplesPerMeter = 1.0f;
    uint32 PatchGridSize = Terrain::kDefaultGridSize;
    float32 LODRangeScale = Terrain::kDefaultLODRangeScale;
    float32 StreamingRadius = 0.0f;   // 0 = auto

    // Fixed for the tiled terrain's lifetime: the extraction system re-provisions
    // the terrain when the authored base or its decoded heightmap changes, the same
    // way it does for a size or density change, so every tile (streamed before or
    // after the change, coarse or full) starts from one base.
    TiledTerrainBase Base;

    // Derived per-tile config (computed at creation time).
    Terrain::TerrainConfig TileConfig;
    float32 TileWorldSize = 0.0f;     // world-space size of each tile
    uint32 TilesPerAxisX = 0;
    uint32 TilesPerAxisZ = 0;
};

// All data for a tiled terrain. Owns the tile grid, the single global
// CDLOD quadtree, and a stitched heightfield used to build it.
struct TiledTerrainData
{
    // Analytic whole-terrain fallback includes regions on never-streamed tiles.
    TerrainGrassField GrassCoarseField;
    TerrainGrassField GrassUnifiedField;
    bool GrassRegionsActive = false;
    TiledTerrainConfig Config;
    float32 WorldOriginX = 0.0f;     // world-space corner of tile (0,0)
    float32 WorldOriginZ = 0.0f;

    // Active (loaded) tiles, keyed by TileCoord.
    std::unordered_map<TileCoord, std::unique_ptr<TerrainTileData>, TileCoordHash> Tiles;

    // Monotonic counter for structural and tile-content changes. Systems that
    // need to notice tiled terrain changes can hash this instead of walking the
    // full tile map each frame.
    uint64 Revision = 0;

    // Single global CDLOD quadtree spanning the entire terrain.
    // Built from per-tile min/max data (no stitched heightfield needed);
    // rebuilt when tiles load/unload.
    Terrain::CDLODQuadtree GlobalQuadtree;

    // Tracks whether the global quadtree needs a full rebuild (structural change:
    // tile loaded/unloaded, first bake, terrain-set change).
    bool QuadtreeDirty = true;

    // Tiles whose heightfields were edited since the last quadtree sync. Their
    // finest-level node min/max are patched into the global quadtree incrementally
    // (FinalizeGlobalQuadtreeUpdates), avoiding the O(all-samples) full rebuild a
    // brush stroke would otherwise force every dab. Cleared once consumed.
    std::vector<TileCoord> DirtyQuadtreeTiles;

    // Global GPU terrain handle (holds the global quadtree, accessed by render node).
    // Created once by the extraction system; quadtree is synced when dirty.
    uint32 GlobalGpuHandleIndex = 0;
    uint32 GlobalGpuHandleGeneration = 0;

    // Morph ranges for the global quadtree. Within-tile levels use normal
    // chained ranges. Tile-level and above use FLT_MAX to prevent multi-tile patches.
    float32 VisRanges[Terrain::kMaxLODLevels] = {};
    float32 MorphStart[Terrain::kMaxLODLevels] = {};
    float32 MorphEnd[Terrain::kMaxLODLevels] = {};
    bool MorphRangesComputed = false;
    float32 CachedLODRangeScale = 0.0f;
    float32 CachedFarClip = 0.0f;

    // Cached global height range that splat normalization is keyed on (the only
    // consumer). Seeded to the deterministic procedural-noise output range
    // [0, kTileNoiseAmplitude] rather than to the first tile's observed range, so
    // every tile normalizes its splat against a fixed range regardless of which
    // tiles are resident. The streaming job path snapshots this seed directly; the
    // modifier bakes re-derive it from ComputeResidentGlobalHeightRange, which is
    // floored to the SAME band, so a height edit no longer discards the seed. The
    // band is widened (never shrunk) only by authored edits that push samples
    // outside it; those widen sites are no-ops for pure noise. Seeding it to (0,0)
    // instead made the first-streamed tile fall back to a per-tile local range and
    // every later tile widen the aggregate, so the SAME tile's splat drifted across
    // stream-out/in cycles (bytes depended on stream order).
    bool GlobalHeightRangeDirty = true;
    float32 CachedGlobalMinH = 0.0f;
    float32 CachedGlobalMaxH = kTileNoiseAmplitude;

    // Global height range the current tile splatmaps were generated with (E6).
    // Height-based splat layers depend on the range shared across ALL tiles, so
    // a region splat regen on one tile is only bit-exact while that range is
    // unchanged; when it shifts, every tile's splatmap must be regenerated.
    // Mirrors TerrainData::SplatBake{MinH,MaxH,RangeValid}.
    float32 SplatBakeMinH = 0.0f;
    float32 SplatBakeMaxH = 0.0f;
    bool SplatBakeRangeValid = false;

    // Deferred whole-terrain splat renormalize (edit-realtime). A height edit that
    // moves the global range would otherwise renormalize EVERY resident tile's
    // splatmap every dab (O(all tiles) procedural-splat regen — the dominant
    // brush-stroke hitch). Instead, an active stroke region-splats only the touched
    // tiles against the still-committed range and sets this flag; the modifier
    // system flushes the one full renormalize (against the final range) once the
    // stroke settles, so the SETTLED splat is byte-identical to a full bake.
    bool SplatResplatPending = false;
    uint32 SplatResplatIdleFrames = 0;

    // Spread the deferred whole-terrain renormalize across several settle frames so
    // no single settle frame regenerates every resident tile's splat at once (the
    // ~1.4s stroke-release spike on large tiled terrains). Once the stroke goes
    // quiescent the flush snapshots the resident-Full tiles + freezes the final
    // global range here, then renormalizes a bounded number of texel rows per frame
    // against that frozen range until the queue drains. The settled result stays
    // byte-identical to a one-shot renormalize (frozen range, each texel visited
    // exactly once); a tile is marked SplatmapDirty only once its LAST row is done,
    // so the GPU sees no torn tile. A fresh range-shifting dab mid-spread cancels it
    // (SplatRenormalizeActive=false) so the spread re-freezes against the new range.
    bool SplatRenormalizeActive = false;
    float32 SplatRenormalizeMinH = 0.0f;
    float32 SplatRenormalizeMaxH = 0.0f;
    std::vector<TileCoord> SplatRenormalizeQueue;
    std::size_t SplatRenormalizeTileCursor = 0;
    int32 SplatRenormalizeRowCursor = 0;

    // ---- GPU height bake (slice-1b, GE_TERRAIN_GPU_BAKE) ----
    // Set by TerrainExtractionSystem each frame: true when the atlas is backing this terrain,
    // the GPU-bake flag is on, and the whole terrain fits the resident window (every tile always
    // resident — no eviction race). The modifier system reads last frame's value to decide
    // whether the height pass can run on the GPU.
    bool AtlasGpuBakeEligible = false;
    // Filled by TerrainModifierSystem when it routes the height pass to the GPU (skipping the CPU
    // eval); drained by TerrainExtractionSystem, which resolves each tile's atlas slot and hands
    // the dispatches to TerrainRenderFeature. Transient per bake.
    GpuHeightBakeBatch GpuBakeBatch;

    // ---- Height page overlay (TerrainModifierSystem, BakeHeightPageOverlay) ----
    // What the modifiers add to this terrain's base height pages: the baked minus the base over the
    // rect the height modifiers reach, on the tile lattice (level 0 of the terrain's pages). Null
    // when no height modifier reaches the terrain. PageOverlayVersion takes a new process-wide
    // value on every change (zero until the first: two terrains' data never share a non-zero
    // version) and PageOverlayChanged is the level-0 rect that change touched relative to version
    // PageOverlayChangedSince; PageOverlayCurrent is set once the
    // overlay reflects the gathered modifier set, so the terrain pages only with its modifiers.
    // PageOverlayByteCap is set by the height page driver each frame: the platform's overlay cap
    // (HeightPageProfile::OverlayByteCap) when the terrain would page but for its modifiers, zero
    // when it keeps its height texture: only a terrain with a cap holds an overlay.
    // PageOverlayRefusedBytes is what the overlay would hold when it is over the cap, zero
    // otherwise: such a terrain keeps its texture and the driver logs OverlayRefusal.
    // PageOverlayOffGrid (set by the driver, PageOverlayAllowance) says the terrain's pages are not
    // on the tile lattice the overlay is baked on; PageOverlayRefusedOffGrid is set when a height
    // modifier reaches such a terrain: it keeps its texture and the driver logs OverlayGridRefusal.
    // The modifier system owns PageOverlay and rebakes it in place; the height pages hold it read-only.
    std::shared_ptr<PageStreaming::HeightPageOverlay> PageOverlay;
    uint64 PageOverlayVersion = 0;
    PageStreaming::PageSampleRect PageOverlayChanged;
    uint64 PageOverlayChangedSince = 0;
    bool PageOverlayCurrent = false;
    uint64 PageOverlayByteCap = 0;
    uint64 PageOverlayRefusedBytes = 0;
    bool PageOverlayOffGrid = false;
    bool PageOverlayRefusedOffGrid = false;

    /// True when the terrain would page and its overlay is neither built nor refused: the
    /// modifier system bakes it at its next update.
    bool AwaitsPageOverlay() const
    {
        return PageOverlayByteCap != 0 && !PageOverlayCurrent && PageOverlayRefusedBytes == 0 &&
               !PageOverlayRefusedOffGrid;
    }
};

// Handle to a tiled terrain within TerrainService.
struct TiledTerrainHandle
{
    uint32 Index = 0;
    uint32 Generation = 0;

    bool operator==(const TiledTerrainHandle& o) const { return Index == o.Index && Generation == o.Generation; }
    bool operator!=(const TiledTerrainHandle& o) const { return !(*this == o); }
};

// Handle to a terrain data instance within TerrainService.
struct TerrainHandle
{
    uint32 Index = 0;
    uint32 Generation = 0;

    bool operator==(const TerrainHandle& other) const
    {
        return Index == other.Index && Generation == other.Generation;
    }

    bool operator!=(const TerrainHandle& other) const { return !(*this == other); }
};

class TerrainHeightPages;
class HeightPageStoreLoader;
struct HeightPageStoreLocation;

// Singleton service owning all terrain data. Follows the PhysicsWorldService /
// NavigationService pattern: static Initialize/Shutdown/Get.
class TerrainService
{
public:
    TerrainService();
    ~TerrainService();

    static void Initialize();
    static void Shutdown();
    static TerrainService& Get();
    static TerrainService* TryGet();
    static bool IsInitialized();
    static uint64 GetGeneration();

    // Create a new terrain and return its handle.
    TerrainHandle CreateTerrain(Terrain::TerrainConfig config);

    // Destroy a terrain by handle.
    void DestroyTerrain(TerrainHandle handle);

    // Access terrain data. Returns nullptr if handle is invalid/stale.
    TerrainData* GetTerrainData(TerrainHandle handle);
    const TerrainData* GetTerrainData(TerrainHandle handle) const;
    // Shares ownership of a terrain's data, so a job off the update can read it after
    // DestroyTerrain retires the slot. Null if the handle is invalid/stale. The data is
    // still written in place by TerrainModifierSystem's bakes: a reader that runs
    // concurrently must be ordered before the next one.
    std::shared_ptr<const TerrainData> ShareTerrainData(TerrainHandle handle) const;

    // Iterate all active terrains.
    uint32 GetActiveTerrainCount() const;
    uint32 GetActiveTiledTerrainCount() const;

    // Size of the slot tables, which only ever grows: a slot is returned to the
    // free list on destroy and reused, so a table larger than the high-water
    // mark of concurrently-live terrains means some teardown is not releasing.
    // That makes the per-open leak countable in one poll — the active counts
    // return to their steady value either way.
    uint32 GetTerrainSlotCount() const;
    uint32 GetTiledTerrainSlotCount() const;

    // Rebuild the CDLOD quadtree for a terrain (call after heightfield changes).
    void RebuildQuadtree(TerrainHandle handle);

    // Region-scoped analogue of RebuildQuadtree: re-scan only the quadtree's
    // finest nodes over the sample rect [minSampleX, maxSampleX] x
    // [minSampleZ, maxSampleZ] (inclusive) from the current heights, then
    // re-derive coarse levels — avoiding the O(all-samples) finest-level scan a
    // full rebuild pays every modifier edit. Also returns the resulting global
    // height range (the root node's min/max) in outMinH/outMaxH; the return
    // value is true when that range is exact (the finest level tiles the field),
    // false when the caller must scan the heightfield itself for the range.
    // Locked like RebuildQuadtree so a concurrent render read / DestroyTerrain
    // stays safe. Falls back to a full Build when the tree isn't built yet.
    bool PatchQuadtreeRegion(TerrainHandle handle,
                             int32 minSampleX, int32 minSampleZ,
                             int32 maxSampleX, int32 maxSampleZ,
                             float32& outMinH, float32& outMaxH);

    // ---- Tiled terrain API ----

    // Create a tiled terrain and return its handle. Computes per-tile config
    // from the tiled config, but does NOT create any tiles — tiles are loaded
    // on demand via LoadTile().
    TiledTerrainHandle CreateTiledTerrain(TiledTerrainConfig config);

    // Destroy a tiled terrain and all its loaded tiles.
    void DestroyTiledTerrain(TiledTerrainHandle handle);

    // Access tiled terrain data. Returns nullptr if handle is invalid/stale.
    TiledTerrainData* GetTiledTerrainData(TiledTerrainHandle handle);

    // Load a tile at the given coordinate. Creates heightfield, builds quadtree.
    // The tile is filled from the terrain's base (FillTiledBaseRegion).
    TerrainTileData* LoadTile(TiledTerrainHandle handle, TileCoord coord);

    // Unload a tile. Frees CPU data. Caller is responsible for GPU cleanup.
    void UnloadTile(TiledTerrainHandle handle, TileCoord coord);

    // Check if a tile is loaded.
    bool IsTileLoaded(TiledTerrainHandle handle, TileCoord coord) const;

    // Rebuild the single global CDLOD quadtree from all loaded tiles.
    // Called after tiles are loaded or unloaded.
    void RebuildGlobalQuadtree(TiledTerrainHandle handle);

    // ---- Per-tile physics collider addressing (E6) ----
    //
    // A tiled terrain is one entity but needs one Jolt heightfield body per
    // tile. The physics HeightFieldDataProvider is keyed by a single
    // (handle, generation) pair, so each tile gets a stable generational
    // "tile-physics handle" that resolves to its live heightfield. The top bit
    // (kTilePhysicsHandleBit) marks a tile handle vs a single-terrain slot
    // index; the physics provider routes on it. Single-terrain slot indices are
    // tiny, so the top bit is always free.

    // dataHandle discriminator: set on HeightFieldColliderShape.dataHandle to
    // mark a tile-physics handle. Cleared to reach the raw slot index.
    static constexpr uint32 kTilePhysicsHandleBit = 0x80000000u;

    struct TilePhysicsHandle
    {
        uint32 Index = 0;      // slot index OR'd with kTilePhysicsHandleBit
        uint32 Generation = 0;
    };

    // Acquire a stable handle addressing one tile's heightfield for physics.
    // TerrainPhysicsSystem owns the (tile -> handle) mapping via the
    // TerrainTileCollider component, so this is a plain slot allocation (not
    // idempotent per coord); the caller stores Index directly in
    // HeightFieldColliderShape.dataHandle.
    TilePhysicsHandle AcquireTilePhysicsHandle(TiledTerrainHandle tiled, TileCoord coord);

    // Release a tile-physics handle (tile unloaded / collider torn down).
    // handleIndex includes kTilePhysicsHandleBit; generation must match the
    // slot's current generation or the release is refused and logged (a stale
    // index alone would silently free whichever slot now owns it). No-op for
    // non-tile handles.
    void ReleaseTilePhysicsHandle(uint32 handleIndex, uint32 generation);

    // Release every tile-physics slot. Called at world-transition seams
    // (Play-exit shuts physics down and snapshot-restore destroys the collider
    // entities without the per-tile teardown sweep ever running, so the slots
    // would otherwise leak across Play sessions). Safe any time — a released
    // handle simply resolves to nullptr. The service outlives Play sessions.
    void ReleaseAllTilePhysicsHandles();

    // Resolve a tile-physics handle to its live tile, or nullptr when the slot
    // is stale, the tiled terrain is gone, or the tile is no longer loaded.
    // handleIndex includes kTilePhysicsHandleBit. Used by the physics provider.
    TerrainTileData* ResolveTilePhysicsTile(uint32 handleIndex, uint32 generation);

    // Test seam: size of the tile-physics slot pool (live + free). Tests assert
    // it stays flat across acquire/release churn (no per-session leak).
    uint32 GetTilePhysicsSlotCountForTests() const;

    // ---- Per-face planet collider addressing (planet-collider slice) ----
    //
    // A spherical (planet) terrain is one entity but needs one Jolt heightfield body per
    // cube face (6). Each face gets a stable generational "planet-face physics handle" that
    // the physics HeightFieldDataProvider resolves to its live sample buffer. Like tiles,
    // the handle carries a discriminator bit (kPlanetFacePhysicsHandleBit) so the provider
    // routes single-terrain / tile / planet-face handles apart. TerrainService owns the
    // sample buffers (regenerated by TerrainPhysicsSystem from the planet height field);
    // the provider returns raw samples + version + a dirty-region log for the E1 two-tier
    // in-place/rebuild path — identical machinery to single terrains and tiles.

    // dataHandle discriminator marking a planet-face handle. Distinct from the tile bit;
    // single-terrain slot indices are tiny, so neither top bit collides with them.
    static constexpr uint32 kPlanetFacePhysicsHandleBit = 0x40000000u;

    struct PlanetFacePhysicsHandle
    {
        uint32 Index = 0;      // slot index OR'd with kPlanetFacePhysicsHandleBit
        uint32 Generation = 0;
    };

    // A planet face's collider height data: the square sample grid (metres, in the face's
    // local height axis), its version, and a per-version dirty-region journal the physics
    // collider consumes (mirrors TerrainData::HeightfieldDirtyLog).
    struct PlanetFaceColliderData
    {
        std::vector<float32> Samples;
        uint32 Dim = 0;
        uint64 Version = 0;
        DirtyRegionLog DirtyLog;
    };

    // Acquire a slot for one planet face's collider heightfield. Plain slot allocation
    // (TerrainPhysicsSystem owns the face -> handle mapping via the
    // TerrainPlanetFaceCollider component).
    PlanetFacePhysicsHandle AcquirePlanetFacePhysicsHandle();

    // Release a planet-face handle (terrain destroyed / switched to Planar / collider torn
    // down). handleIndex includes kPlanetFacePhysicsHandleBit; generation must match or the
    // release is refused and logged (a stale index alone would silently free the current
    // owner). No-op for non-planet-face handles.
    void ReleasePlanetFacePhysicsHandle(uint32 handleIndex, uint32 generation);

    // Release every planet-face slot. Called at world-transition seams (Play-exit), where
    // the snapshot restore destroys the collider entities without running the per-face
    // teardown sweep — the slots would otherwise leak across Play sessions.
    void ReleaseAllPlanetFacePhysicsHandles();

    // Resolve a planet-face handle to its live collider data (mutable) for the physics
    // system to (re)generate samples into. Resizes the buffer to dim*dim on demand.
    // Returns nullptr when the slot is stale. handleIndex includes the discriminator bit.
    PlanetFaceColliderData* ResolvePlanetFaceColliderForWrite(uint32 handleIndex, uint32 generation,
                                                              uint32 dim);

    // Resolve a planet-face handle to its live collider data (read-only) for the provider.
    // Returns nullptr when the slot is stale or its buffer is empty.
    const PlanetFaceColliderData* ResolvePlanetFaceCollider(uint32 handleIndex,
                                                            uint32 generation) const;

    // Commit a full (re)generation: bump the version + log a full-dirty rect so the collider
    // (re)builds from the whole grid.
    void CommitPlanetFaceFull(uint32 handleIndex, uint32 generation);

    // Commit a region regeneration (sculpt edit): bump the version and log
    // [minX, minZ, maxX, maxZ) (exclusive) so the provider reports it to the E1 in-place path.
    void CommitPlanetFaceRegion(uint32 handleIndex, uint32 generation, int32 minX, int32 minZ,
                                int32 maxX, int32 maxZ);

    // Test seam: size of the planet-face slot pool (live + free). Tests assert it stays
    // flat across acquire/release churn (no per-session leak).
    uint32 GetPlanetFacePhysicsSlotCountForTests() const;

    // ---- Editable spherical sculpt (planet editing) ----
    //
    // TerrainService owns the AUTHORITATIVE sphere sculpt layer (dab layer + baked modifier
    // stack, SphereSculptLayer). It is the sphere analogue of the planar heightfield the
    // service already owns, and lives here — not in the CBT render feature — because the two
    // writers span modules the feature cannot: the editor brush (ApplySphereSculptDab) and
    // TerrainModifierSystem (BakePlanetModifierRegions, TerrainECS, which cannot reach the
    // feature in CBTTerrainECS). The CBT feature reads CopySphereSculptUpload + version to
    // upload the SSBO ring and drives Classify from SphereSculptDirtyRegions; the physics
    // colliders sample the same atlas (GetPlanetSculptMirror). One mutex guards the layer so
    // the render-thread upload never tears against a main-thread edit (edits are rare).
    //
    // The dirty rects are accumulated PER FACE, not just the primary face of the last edit: a
    // dab or modifier near a cube edge writes both adjacent faces' atlas bands (the #488
    // crack-free contract), and several edits can land between two collider consumes. Each
    // face's rect is the union of everything since that face was last consumed;
    // TerrainPhysicsSystem refreshes every touched face and clears it, so no neighbour-face
    // collider is left with pre-edit heights.
    static constexpr uint32 kPlanetSculptFaceCount = 6u; // the six cube faces

    struct PlanetSculptFaceRect
    {
        bool Touched = false;
        float32 MinU = 0.0f, MinV = 0.0f, MaxU = 0.0f, MaxV = 0.0f;
    };

    struct PlanetSculptMirror
    {
        // A sampler VIEW over the paged sculpt store (pool + page table + geometry), or an invalid
        // view (Valid() == false) before the first edit. Points into service-owned storage.
        CBTTerrain::SphereSculptSampler Sampler{};
        uint64 Version = 0;
        std::array<PlanetSculptFaceRect, kPlanetSculptFaceCount> Faces{};
        // The analytic modifier placement set (sculpt shape-accuracy S2) — copied by value so the
        // physics collider composes the SAME closed forms the GPU samples (parity by shared math).
        std::array<CBTTerrain::SphereAnalyticFlatten, CBTTerrain::kMaxSphereAnalyticModifiers>
            Analytic{};
        uint32 AnalyticCount = 0u;
        // The transient brush-dab set (S3 analytic-while-stroking) — non-empty only while a
        // stroke is held. Carried so the collider tracks the growing stroke exactly like the
        // pre-S3 per-dab store writes did (consistency by the shared chokepoint).
        std::array<CBTTerrain::SphereAnalyticDab, CBTTerrain::kMaxSphereAnalyticModifiers>
            TransientDabs{};
        uint32 TransientDabCount = 0u;
    };

    // Size the sculpt page store's VIRTUAL resolution from the planet radius (a metres-per-texel
    // budget -> Dv). Idempotent while the radius-derived geometry is unchanged, so calling it each
    // frame from the modifier / render path is cheap. When a LIVE radius edit re-derives a
    // different Dv over authored content, the store REMAPS the content onto the new grid (angular
    // position kept, amplitudes stay absolute metres) and the remapped regions feed the same
    // dirty / re-tess / physics unions a dab does — or the store keeps the old resolution when the
    // remap cannot fit the physical pool (see SphereSculptLayer::Configure). Must run before the
    // first dab / modifier bake so every consumer (GPU, physics, CPU cursor) derives the same Dv.
    void ConfigurePlanetSculpt(float32 planetRadius);

    // Drop the sculpt store's content + geometry (and the per-face dirty accumulators + one-shot
    // sub-texel warning) so the NEXT ConfigurePlanetSculpt re-derives freely. The CBT update system
    // calls this when the active spherical terrain is deleted or switches domain away — the sculpt
    // is per-planet authored data, so a create->edit->delete->recreate sequence must not carry the
    // old planet's frozen dim + stale content into the new planet. Content loss is correct here.
    void ResetPlanetSculpt();

    // Apply an interactive brush dab into the sculpt layer's DAB contribution (editor brush,
    // main thread). Returns the touched (face, UV rect) regions. Accumulates them into the
    // per-face physics dirty set and advances the sculpt version.
    CBTTerrain::SphereEditRegions ApplySphereSculptDab(float32 cx, float32 cy, float32 cz,
                                                       float32 angularRadius, float32 strength,
                                                       bool lower);

    // Diameter of a dab's angular footprint in VIRTUAL sculpt texels: a cube face spans a quarter
    // turn (pi/2 rad) across `virtualDim` texels, so texels = (2*angularRadius)/(pi/2)*virtualDim.
    // Below ~1 the brush writes fewer than one texel. With the radius-scaled page-table dim this
    // should rarely fire at sane radii (that is the point). Pure + static so it is unit-tested.
    static float32 SphereSculptDabFootprintTexels(float32 angularRadius, uint32 virtualDim);

    // Re-derive the sculpt layer's MODIFIER contribution over `regions` (TerrainModifierSystem,
    // main thread): each texel in the regions is set to evalOffset(worldDirX,Y,Z) metres — the
    // terrain-modifier stack's total height offset at that texel's world direction (0 outside
    // every footprint). `regions` is the union of the old ∪ new face footprints of every
    // modifier whose state changed this bake (the six full faces for a first / full bake), so a
    // modifier that moved has its vacated footprint re-derived to 0 — the sphere analogue of
    // the planar "reset region to base, re-apply the stack". Accumulates the touched faces for
    // physics and advances the version. Templated so the per-texel loop is a direct call.
    template <typename EvalFn>
    CBTTerrain::SphereEditRegions BakePlanetModifierRegions(const CBTTerrain::SphereEditRegions& regions,
                                                            EvalFn&& evalOffset, bool fullBake = false)
    {
        std::lock_guard<std::mutex> lock(m_SphereSculptMutex);
        // A full bake clears the whole modifier layer first (bounded by the pool, not the virtual
        // face area) so a removed/moved modifier's vacated pages are reset; a region bake only
        // re-derives `regions`. Both preserve the freehand dab layer.
        CBTTerrain::SphereEditRegions baked =
            fullBake ? m_SphereSculpt.BakeModifierFull(regions, std::forward<EvalFn>(evalOffset))
                     : m_SphereSculpt.BakeModifierLayer(regions, std::forward<EvalFn>(evalOffset));
        AccumulateSphereSculptDirtyFaces(baked);
        return baked;
    }

    // Copy the paged sculpt store's published page pool + page table + geometry under the sculpt
    // mutex (CBT feature GPU upload, render thread). `pool` / `pageTable` are resized to the store's
    // sizes; empty until the first Configure. Returns the sculpt version.
    uint64 CopySphereSculptUpload(std::vector<float32>& pool, std::vector<uint32>& pageTable,
                                  CBTTerrain::SphereSculptGeometry& geom) const;

    // Monotonic sculpt edit counter (0 = no edits) — the store layer's version PLUS the analytic
    // placement-set version, so an analytic-modifier edit re-arms the same upload / Classify /
    // physics gates a store write does (an edit IS an edit). HasSphereSculptEdits() gates the
    // shader sample: store edits OR a non-empty analytic set.
    uint64 SphereSculptVersion() const;
    bool HasSphereSculptEdits() const;

    // S4 adaptive page levels, observability (get_terrain_stats): pool slots claimed (a level-L
    // page counts its whole 4^L block) and virtual pages hosted above the base resolution.
    uint32 SphereSculptAllocatedPages() const;
    uint32 SphereSculptEscalatedPages() const;

    // ---- Analytic sphere modifiers (sculpt shape-accuracy S2) ----
    // The bounded closed-form placement set TerrainModifierSystem publishes each sphere bake when
    // GE_TERRAIN_ANALYTIC_MODIFIERS is on (flag off / no eligible modifiers = empty). Compared
    // by value: an unchanged publish is a no-op; a changed one advances the combined sculpt
    // version above. Consumers: CBTRenderFeature (frame + surface params upload), the physics
    // mirror, and the brush-cursor sample below.
    void SetPlanetAnalyticModifiers(const CBTTerrain::SphereAnalyticFlatten* items, uint32 count);
    uint32 CopyPlanetAnalyticModifiers(
        std::array<CBTTerrain::SphereAnalyticFlatten,
                   CBTTerrain::kMaxSphereAnalyticModifiers>& out) const;

    // ---- Analytic-while-stroking (sculpt shape-accuracy S3) ----
    // While an interactive brush stroke is held, its dabs are NOT written to the store: each dab
    // queues (FIFO) and is mirrored into a bounded TRANSIENT analytic set the sampling
    // chokepoints compose (exact circles at any radius while the mouse is down). Consecutive
    // dabs at the bitwise-same centre/radius MERGE into one transient (amplitudes sum — exact,
    // the falloff is a pure function of angle), so a press-and-hold stays one placement. The
    // transient budget is kMaxSphereAnalyticModifiers minus the published modifier count; when a
    // new centre would exceed it, the OLDEST half of the transients commits to the store
    // mid-stroke (commit-as-you-go chunks, FIFO order preserved). CommitSphereSculptStroke
    // (mouse-up) replays the remaining queue through the normal ApplyDab path — the store then
    // holds byte-identical content to a pre-S3 stroke of the same dab sequence, #630's stroke
    // undo captures exactly those writes, and the transient set clears (it is never persisted,
    // never undone). Flag off (GE_TERRAIN_ANALYTIC_MODIFIERS unset) routes straight to
    // ApplySphereSculptDab — the pre-S3 path, byte-identical.
    static bool AnalyticModifiersEnabled();
    CBTTerrain::SphereEditRegions ApplySphereSculptDabStroked(float32 cx, float32 cy, float32 cz,
                                                              float32 angularRadius,
                                                              float32 strength, bool lower);
    void CommitSphereSculptStroke();
    uint32 CopyPlanetTransientDabs(
        std::array<CBTTerrain::SphereAnalyticDab, CBTTerrain::kMaxSphereAnalyticModifiers>& out)
        const;

    // The resolved page geometry (virtual dim / pages / pool count) for the GPU frame + surface
    // params. Reflects the last ConfigurePlanetSculpt (or defaults before one).
    CBTTerrain::SphereSculptGeometry GetPlanetSculptGeometry() const;

    // Drain (clear-on-read) the render-side per-face dirty accumulator into `out`: the union of
    // EVERY sphere sculpt edit (dab + modifier bake) since the last drain, as (face, UV rect)
    // regions. Returns false (out unchanged) when nothing is dirty. Draining a UNION — not reading
    // the layer's single most-recent LastRegions — is what stops several dabs that land between two
    // CBT updates (a fast stroke at 3-10 fps drops multiple mouse-move dabs per frame) from
    // collapsing to only the last edit's footprint, which would leave the earlier dabs' geometry
    // stale until a later edit overlaps them (the same neighbour-stale seam class as the cross-face
    // fix). Separate from the physics accumulator (m_PlanetSculptFaces) so the two consumers drain
    // at their own rates. Non-const — it clears what it returns.
    bool ConsumeSphereSculptDirtyRegions(CBTTerrain::SphereEditRegions& out);

    // The CPU-authoritative sculpt height (metres) at a world direction — the editable layer
    // the GPU VertexEval also samples, COMPOSED with the analytic modifier set (S2). The editor
    // brush adds this to the procedural relief so its cursor tracks tall sculpt instead of
    // drifting on the base sphere. `reliefAtDir` is the closed-form base relief at the direction
    // (the caller has it; an analytic flatten cancels it — pass 0 when no relief context exists,
    // which only mis-levels analytic pads, never the store). `dir` need not be unit.
    float32 SampleSphereSculptHeight(float32 dx, float32 dy, float32 dz, float32 reliefAtDir) const;

    // Read the current sculpt mirror (TerrainPhysicsSystem). Atlas is null until the first edit.
    PlanetSculptMirror GetPlanetSculptMirror() const;

    // Clear a face's accumulated dirty rect after its collider region was refreshed.
    void ClearPlanetSculptDirtyFace(uint32 face);

    // ---- Sphere sculpt stroke undo (editor brush) ----
    // One interactive stroke (mouse-down -> mouse-up; one terrain_sculpt_dab IPC call) brackets
    // its dabs in Begin/Take: the layer captures each touched page's pre-image lazily on first
    // write (memory-bounded — only pages the stroke wrote). The editor pushes the (before, after)
    // page sets as ONE undo entry (SphereSculptStrokeCommand); undo/redo restore through
    // RestoreSphereSculptPages, which re-enters the SAME edit path a dab does — version advance,
    // per-face physics + render dirty unions, and the edit-driven re-tess / forced VertexEval
    // those feed. An undo IS an edit.
    void BeginSphereSculptStrokeCapture();
    std::vector<CBTTerrain::SphereSculptPageState> TakeSphereSculptStrokeCapture();

    // Current state of the pages keyed by `keys` (the stroke's post-images, for redo).
    std::vector<CBTTerrain::SphereSculptPageState> SnapshotSphereSculptPages(
        const std::vector<CBTTerrain::SphereSculptPageState>& keys) const;

    // Restore captured page states (undo -> pre-images, redo -> post-images) and feed the
    // restored regions into both dirty accumulators, exactly like ApplySphereSculptDab.
    void RestoreSphereSculptPages(const std::vector<CBTTerrain::SphereSculptPageState>& pages);

    // Test seam: apply a raw dab into the sculpt layer without a render feature.
    void SeedPlanetSculptMirrorForTests(float32 cx, float32 cy, float32 cz, float32 angularRadius,
                                        float32 strength, bool lower);

    // ---- Sphere sculpt persistence (.tsculpt save/load) ----
    // The sculpt page store persists as a .tsculpt sidecar next to the scene (the planet
    // analogue of the zones' .tzone), GUID-referenced from the Terrain component's
    // SphereSculptGuid. The editor's save flush encodes + writes it (SphereSculptSaver);
    // CBTUpdateSystem restores it once per planet via EnsureSphereSculptLoaded.

    // True when the DAB layer changed since the last save/load (drives the editor's save
    // prompts — brush strokes touch no ECS state, so the document flag alone misses
    // stroke-only sessions, exactly like AnyZonePayloadNeedsSave for zones). Keyed on dab
    // mutations, NOT the sculpt version: modifier bakes and radius remaps advance the version
    // on every provision (an untouched loaded planet would read phantom-dirty forever), and
    // their content re-derives/remaps from the serialized components on the next load anyway —
    // only the freehand dab layer is unrecoverable, so only it warrants the prompt.
    bool SphereSculptNeedsSave() const;

    // Encode the current page store to the .tsculpt blob under the sculpt mutex.
    // `outVersion` receives the encoded sculpt version so the saver can mark it saved (or
    // ledger-dedupe an autosave staging) without racing a later edit.
    std::vector<uint8> EncodeSphereSculptBlob(uint64& outVersion) const;

    // Record a completed save: `guid` is the file-backed payload identity (the next
    // EnsureSphereSculptLoaded with it is a no-op — the content IS the file's) and `version`
    // the encoded sculpt version (needs-save clears while no newer edit exists).
    void MarkSphereSculptSaved(const GUID& guid, uint64 version);

    // Restore a .tsculpt blob: configure the store at the blob's SAVED grid (with the LIVE
    // pool count), import the pages, and feed the covering regions into both dirty unions —
    // a LOAD is an edit, so re-tess + physics engage without camera motion. When the live
    // planet radius derives a different grid, the next ConfigurePlanetSculpt remaps the
    // restored content via the normal resize path (no separate load-remap exists). Returns
    // false (store untouched) on a malformed blob. Public as the headless test seam.
    bool RestoreSphereSculptFromBlob(const uint8* data, std::size_t size, const GUID& guid);

    // Resolve `guid` to its .tsculpt asset and restore it unless it is already the loaded
    // (or just-saved) content. Called per frame by CBTUpdateSystem for the active planet;
    // guid-keyed, so it is once-per-planet and re-fires after a planet-identity reset.
    // Load failures warn once per guid until a different guid is requested.
    void EnsureSphereSculptLoaded(const GUID& guid);

    // ---- Async tile API (used by TileStreamingManager) ----

    // Create an empty tile slot at the given coordinate (no heightfield data).
    TerrainTileData* CreateEmptyTile(TiledTerrainHandle handle, TileCoord coord);

    // Set the tile's heightfield data (coarse or full). Moves the heightfield
    // into the tile and updates CachedMinH/MaxH and LodState.
    void SetTileHeightfield(TiledTerrainHandle handle, TileCoord coord,
                            Terrain::HeightfieldData&& heightfield,
                            float32 minH, float32 maxH);

    // Set the tile's splatmap and normalmap data. Moves the data into the tile.
    void SetTileMaps(TiledTerrainHandle handle, TileCoord coord,
                     std::vector<uint8>&& splatmap, uint32 splatW, uint32 splatH,
                     std::vector<uint8>&& normalmap, uint32 normW, uint32 normH);

    // Patch finest-level quadtree nodes for a single tile (no coarse propagation).
    // Call FinalizeGlobalQuadtreeUpdates() after patching all tiles in a batch.
    void PatchTileInGlobalQuadtree(TiledTerrainHandle handle, TileCoord coord);

    // Patch finest-level nodes from pre-computed min/max data (avoids heightfield scan).
    // The nodes array has nodesPerAxis^2 entries in row-major order.
    void PatchTileInGlobalQuadtreePrecomputed(TiledTerrainHandle handle, TileCoord coord,
                                              const NodeMinMax* nodes, uint32 nodesPerAxis);

    // Propagate coarse levels from children after a batch of PatchTileInGlobalQuadtree calls.
    void FinalizeGlobalQuadtreeUpdates(TiledTerrainHandle handle);

    // ---- Base-source heightmap assets + asset-reload lane ----

    // Decoded base-source heightmap cache, keyed by asset GUID. Decodes once
    // via the AssetManager (16-bit PNG through stb's 16-bit path; .r16/.r32
    // raw on the grid its import settings declare, RawHeightmap.h) and caches until a
    // hot-reload eviction. Returns null when the GUID can't be resolved or
    // decoded (negative-cached until eviction, the reason in
    // GetHeightmapDecodeError). Main-thread only — the
    // modifier and extraction systems that consume it run sequentially. The
    // result is shared: an eviction drops the cache's reference, and a holder
    // (a tiled terrain's base, a streaming job) keeps reading the content it
    // was given until it lets go.
    std::shared_ptr<const Terrain::HeightfieldData> ResolveHeightmapAsset(const GUID& guid);

    // Why the heightmap `guid` last failed to decode, worded as the fix ("set the heightmap's
    // width and height in its import settings"); empty when it decoded or has not been resolved.
    std::string GetHeightmapDecodeError(const GUID& guid) const;

    // ---- Baked-terrain cache (TerrainBakeCache.h) ----

    // Where a single terrain's full modifier bake is looked up before it runs and,
    // when writable, stored after. Set at startup by the engine: the workspace's
    // .Cache/TerrainBake in the editor and dev runs, the build's staged copy
    // (read-only) in a packaged game. Off while the directory is empty.
    void SetBakeCache(TerrainBakeCacheConfig config) { m_BakeCache = std::move(config); }
    const TerrainBakeCacheConfig& GetBakeCache() const { return m_BakeCache; }

    // The process's one "Terrain bake store" channel (cap 1) on `jobSystem`'s blocking
    // threads, created on the first call: a save's bake-cache copy, writes and prune run as
    // its jobs, scheduled one store after another across every world (the cap bounds
    // concurrency until the pool's shutdown gate; it is not a lock), and never hold a
    // compute worker.
    // Every call passes the same job system, which outlives this service.
    JobSystem::JobChannel& BakeStoreChannel(JobSystem::WorkStealingThreadPool& jobSystem);

    // ---- Height pages (TerrainHeightPages.h; design page-streaming D-c) ----

    // The process's one height-page residency, created on first use with the engine's job pool.
    TerrainHeightPages& GetHeightPages();
    // The cooked height stores of the heightmaps paged terrains read.
    HeightPageStoreLoader& GetHeightPageStores();
    // Where height stores are found or cooked; set at startup by the engine, like SetBakeCache.
    void SetHeightPageStoreLocation(HeightPageStoreLocation location);
    // Set by the modifier system at each gather: whether any modifier is authored. Until the first
    // gather the set is unknown and HeightModifiersMayApply answers true.
    void SetHeightModifiersPresent(bool present)
    {
        m_HeightModifiersGathered = true;
        m_HeightModifiersPresent = present;
    }
    bool HeightModifiersMayApply() const { return !m_HeightModifiersGathered || m_HeightModifiersPresent; }

    // Content version for a heightmap GUID: bumped on each eviction so
    // terrain-state hashes treat reloaded content as a change. 0 until the
    // first eviction; never requires (or triggers) a decode.
    uint64 GetHeightmapContentVersion(const GUID& guid) const;

    // Drop the decoded entry and bump its content version. Returns true when
    // the GUID was referenced (entry or version existed) — i.e. a re-bake is
    // warranted.
    bool EvictDecodedHeightmap(const GUID& guid);

    // Test seam: unit tests run without an AssetManager and inject decoded
    // heightfields directly into the same cache the decode path fills.
    void SeedDecodedHeightmapForTests(const GUID& guid, Terrain::HeightfieldData heightfield);

    // The base a tiled terrain's tiles start from, for a terrain authored with `source`
    // and the heightmap asset `heightmapGuid` (read for HeightmapAsset only). The
    // heightmap comes from the decode cache above. Main-thread only, like the cache.
    TiledTerrainBase ResolveTiledTerrainBase(Components::TerrainBaseSource source,
                                             const GUID& heightmapGuid);

    // True while `base` is what ResolveTiledTerrainBase would return for `source` and
    // `heightmapGuid` now: the same source and, for HeightmapAsset, the same asset at
    // the same content version. Lookups only, never a decode, so the extraction system
    // asks every frame and resolves only on a change.
    //
    // The content version moves on every EvictDecodedHeightmap of that GUID, and the
    // asset-reload lane evicts on each ContentEvents event for it, not only a hot reload:
    // Modified and Created from the file watcher, Reloaded, Unloaded and Destroyed. Each
    // of those makes the base stale, and a stale base costs a full re-provision of every
    // tiled terrain on that heightmap (the whole tile set re-streamed and re-baked).
    bool IsTiledTerrainBaseCurrent(const TiledTerrainBase& base, Components::TerrainBaseSource source,
                                   const GUID& heightmapGuid) const;

    // The config the extraction system provisions a tiled terrain with for the component
    // `terrain`: its size, height scale, samples per metre and streaming radius, the
    // default patch grid and LOD range, and its base (ResolveTiledTerrainBase over its
    // baseSource and heightmap asset). Main-thread only, like ResolveTiledTerrainBase.
    TiledTerrainConfig BuildTiledTerrainConfig(const Components::Terrain& terrain);

    // How the live tiled terrain's `config` differs from the component `terrain` now: its
    // density, its size and its base (IsTiledTerrainBaseCurrent). Lookups only; the
    // extraction system asks every frame and passes the answer to StepTiledReprovision.
    TiledTerrainEdit CompareTiledTerrainConfig(const TiledTerrainConfig& config,
                                               const Components::Terrain& terrain) const;

    // Asset-reload lane (design §9.1/§9.2). The invalidator handlers only
    // ENQUEUE here — they can fire on the file-watcher thread.
    // TerrainModifierSystem drains on the main thread at Update start and
    // evicts the decoded caches there.
    void EnqueueAssetInvalidation(const GUID& guid);
    std::vector<GUID> TakePendingAssetInvalidations();

    // ---- Zone payload store (edit-pipeline design §9.3) ----
    //
    // Brush-authored sculpt/paint payloads keyed by the zone component's
    // payload GUID. The bake resolves payloads here; the brush writes here.
    // Main-thread only, like the heightmap cache — the modifier/extraction/tool
    // paths that touch it run sequentially.

    // Fetch an already-resident payload (no asset load). Null when absent.
    TerrainZonePayload* GetZonePayload(const GUID& guid);

    // Any resident payload with unsaved brush edits. Brush strokes never touch
    // ECS state, so the editor's dirty prompts must ask here — the scene
    // document's own flag misses stroke-only sessions.
    bool AnyZonePayloadNeedsSave() const;

    // Drop every resident payload (scene replace / new scene). Payloads are
    // per-scene authored data: a replace-open must not leak the previous
    // session's edits — a discarded stroke left resident would keep the
    // document dirty forever and would be silently persisted by the next
    // explicit Save. The new scene's zones re-resolve lazily from their
    // .tzone files. Bumps the edit epoch so the modifier gate re-bakes.
    void ClearZonePayloads();

    // Resolve a payload: return the resident one, or decode it from its .tzone
    // asset on a miss. Null when the GUID can't be resolved/decoded.
    const TerrainZonePayload* ResolveZonePayload(const GUID& guid);

    // Create (or reset) a resident payload for a new zone. Bumps the edit epoch
    // so the modifier gate wakes and bakes the fresh zone.
    TerrainZonePayload& EnsureZonePayload(const GUID& guid, ZonePayloadFormat format,
                                          uint32 width, uint32 height);

    // Record a brush write: bump DataVersion, accumulate the dirty texel rect,
    // bump the global edit epoch (a payload write changes no ECS state, so the
    // modifier gate has nothing else to wake on), and flag NeedsSave. Max
    // bounds are exclusive. No-op when the GUID isn't resident.
    void NotifyZonePayloadEdited(const GUID& guid, int32 minX, int32 minZ, int32 maxX, int32 maxZ);

    // Clear a payload's accumulated dirty rect after the bake consumed it.
    void ClearZonePayloadDirty(const GUID& guid);

    // Drop a resident payload. Returns true when one existed (a re-bake is
    // warranted); bumps the edit epoch so the gate re-reads.
    bool EvictZonePayload(const GUID& guid);

    // Monotonic epoch bumped on any zone-payload edit/create/evict/restore.
    // TerrainModifierSystem's change gate compares it to detect brush strokes.
    uint64 GetZonePayloadEditEpoch() const { return m_ZonePayloadEditEpoch; }

    // Replace a resident payload wholesale (undo/redo restore path). Bumps
    // DataVersion + the edit epoch but records NO dirty rect, so the next bake
    // re-applies the full zone footprint (a stroke undo reverts the whole zone).
    void SetZonePayload(const GUID& guid, TerrainZonePayload payload);

    // Test seam: inject a resident payload directly (no AssetManager).
    void SeedZonePayloadForTests(const GUID& guid, TerrainZonePayload payload);

    // Play-mode snapshot/restore (design §7/§10): zone payloads mutated during
    // Play restore on Play-exit by default. Snapshot deep-copies the whole
    // store on Play-enter; restore swaps it back and bumps the edit epoch so
    // the next bake reflects the reverted payloads.
    void SnapshotZonePayloadsForPlay();
    void RestoreZonePayloadsFromPlaySnapshot();

    // ---- Thread safety guards ----
    // The modifier and extraction systems both mutate terrain data (heightfields,
    // quadtrees, splatmaps) and MUST NOT run concurrently. Today they run
    // sequentially via system dependencies (Modifiers → Extraction). These guards
    // assert that invariant at runtime so a future parallelization mistake is
    // caught immediately.
    //
    // Future async support: to allow concurrent modifier + extraction, adopt a
    // staging pattern — the modifier writes to a staging heightfield, and
    // extraction atomically swaps the staging data into the live slot. This
    // eliminates shared mutable state entirely. See the design doc's caching
    // strategy (Section 3) for the conceptual foundation.
    void BeginModifierAccess()  { assert(!m_ModifierAccessActive.exchange(true)); }
    void EndModifierAccess()    { m_ModifierAccessActive.store(false); }
    void AssertNoModifierAccess() const { assert(!m_ModifierAccessActive.load()); }

    // ---- Interactive modifier-drag signal ----
    // Set by the editor while an interactive edit is in progress.
    // TerrainModifierSystem coalesces the expensive per-frame full-footprint
    // re-bake while this is set, deferring to one accurate settle bake the moment
    // the edit ends — a gizmo drag on a large atlas terrain re-baked old∪new every
    // frame otherwise (single-digit fps). Defaults off, so headless runs, gameplay,
    // and tests keep the immediate per-frame bake.
    //
    // The signal is the OR of independent SOURCES, not one bool. The scene view
    // writes its source EVERY frame from the gizmo's live state, so a second
    // writer sharing one bool would have its arming cleared on the very next
    // frame — an inspector drag would arm the throttle and lose it before the
    // modifier system ever coalesced anything.
    enum class InteractiveEditSource : uint32
    {
        TransformGizmo = 0, // a modifier/zone gizmo being moved, rotated or scaled
        InspectorDrag = 1,  // an inspector control being dragged (surface-rule bands)
        Count
    };

    void SetInteractiveModifierEdit(InteractiveEditSource source, bool active)
    {
        const uint32 bit = 1u << static_cast<uint32>(source);
        m_InteractiveModifierEditSources =
            active ? (m_InteractiveModifierEditSources | bit)
                   : (m_InteractiveModifierEditSources & ~bit);
    }
    bool IsInteractiveModifierEdit() const { return m_InteractiveModifierEditSources != 0; }

    // ---- GPU height-bake settle-readback -> splat handoff (slice-1c) ----
    // The extraction system's settle-readback adopt refreshed a tiled terrain's heightfield from
    // the GPU and set its SplatResplatPending; this one-shot signal tells the modifier system's
    // next Update to run the deferred splat renormalize (which it gates on m_AnyResplatPending),
    // so the readback drives the SAME #526 settle machinery a CPU bake would.
    void MarkGpuReadbackSplatPending() { m_GpuReadbackSplatPending = true; }
    bool TakeGpuReadbackSplatPending()
    {
        const bool v = m_GpuReadbackSplatPending;
        m_GpuReadbackSplatPending = false;
        return v;
    }

private:
    // Subscribe the texture + heightmap reload invalidators. No-op until the
    // engine (and its AssetManager) is initialized — unit tests construct the
    // service standalone and the lane stays inactive there.
    void InstallAssetReloadInvalidators();

    std::atomic<bool> m_ModifierAccessActive{false};
    TerrainBakeCacheConfig m_BakeCache;
    // BakeStoreChannel's channel and the job system it was created on.
    std::unique_ptr<JobSystem::JobChannel> m_BakeStoreChannel;
    JobSystem::WorkStealingThreadPool* m_BakeStoreJobSystem = nullptr;

    // One bit per InteractiveEditSource, held for the duration of that source's
    // gesture (see SetInteractiveModifierEdit). Single-threaded main-thread state
    // — the editor writes it and the modifier system reads it in the same frame's
    // system pass.
    uint32 m_InteractiveModifierEditSources = 0;
    bool m_GpuReadbackSplatPending = false;

    struct DecodedHeightmapEntry
    {
        // Null when the decode failed: a negative cache entry, so a failing
        // source is not re-decoded every bake (until its eviction).
        std::shared_ptr<const Terrain::HeightfieldData> Heightfield;
        uint64 ContentVersion = 0;
        std::string DecodeError; // why Heightfield is null; empty when it decoded
    };
    std::unordered_map<GUID, DecodedHeightmapEntry> m_HeightmapCache;
    // Content versions survive eviction (see GetHeightmapContentVersion).
    std::unordered_map<GUID, uint64> m_HeightmapVersions;

    std::mutex m_PendingAssetInvalidationsMutex;
    std::vector<GUID> m_PendingAssetInvalidations;
    AssetReloadInvalidator m_TextureReloadInvalidator;
    AssetReloadInvalidator m_HeightmapReloadInvalidator;
    AssetReloadInvalidator m_ZonePayloadReloadInvalidator;

    // Zone payload store (see the public zone API above).
    std::unordered_map<GUID, TerrainZonePayload> m_ZonePayloads;
    uint64 m_ZonePayloadEditEpoch = 0;
    std::unordered_map<GUID, TerrainZonePayload> m_ZonePayloadPlaySnapshot;
    bool m_HasZonePayloadPlaySnapshot = false;

    struct Slot
    {
        std::shared_ptr<TerrainData> Data;
        uint32 Generation = 0;
        bool Active = false;
    };

    std::vector<Slot> m_Slots;
    std::vector<uint32> m_FreeList;

    // Tiled terrain slots (separate pool from single-terrain slots).
    struct TiledSlot
    {
        std::unique_ptr<TiledTerrainData> Data;
        uint32 Generation = 0;
        bool Active = false;
    };
    std::vector<TiledSlot> m_TiledSlots;
    std::vector<uint32> m_TiledFreeList;

    // Per-tile physics handle slots (E6). Each maps a stable generational
    // handle to a (tiled terrain, tile coord); the physics provider resolves
    // through it to the live tile heightfield.
    struct TilePhysicsSlot
    {
        TiledTerrainHandle Tiled;
        TileCoord Coord;
        uint32 Generation = 0;
        bool Active = false;
    };
    std::vector<TilePhysicsSlot> m_TilePhysicsSlots;
    std::vector<uint32> m_TilePhysicsFreeList;

    // Per-face planet collider slots (planet-collider slice). Each holds one cube face's
    // collider sample buffer + version + dirty log; the physics provider resolves through
    // the generational handle to the live buffer.
    struct PlanetFacePhysicsSlot
    {
        PlanetFaceColliderData Data;
        uint32 Generation = 0;
        bool Active = false;
    };
    std::vector<PlanetFacePhysicsSlot> m_PlanetFacePhysicsSlots;
    std::vector<uint32> m_PlanetFacePhysicsFreeList;

    // The authoritative editable sphere sculpt layer (dab + baked modifier stack). Guarded so
    // the render-thread GPU upload (CopySphereSculptUpload) never tears against a main-thread
    // edit. m_PlanetSculptFaces accumulates the per-face dirty rects the physics colliders
    // consume (union across every edit since a face was last cleared).
    CBTTerrain::SphereSculptLayer m_SphereSculpt;
    mutable std::mutex m_SphereSculptMutex;
    // Physics per-face dirty union, drained face-by-face by TerrainPhysicsSystem (ClearPlanetSculptDirtyFace).
    std::array<PlanetSculptFaceRect, kPlanetSculptFaceCount> m_PlanetSculptFaces{};
    // Render per-face dirty union, drained whole (clear-on-read) by ConsumeSphereSculptDirtyRegions.
    // Separate from the physics set so the GPU re-classify and the colliders drain independently —
    // and so several dabs between two CBT updates accumulate here instead of collapsing to the
    // layer's single LastRegions.
    std::array<PlanetSculptFaceRect, kPlanetSculptFaceCount> m_RenderSculptFaces{};
    // One-shot honest gate: a dab whose angular footprint is smaller than one sculpt texel
    // writes nothing visible (the atlas is a fixed dim/face, so a huge planet radius makes the
    // per-texel spacing coarse). Warn once instead of letting the brush silently do nothing —
    // radius-scaled sculpt resolution is the tracked follow-up (sparse page-table design).
    bool m_SphereSculptSubTexelWarned = false;

    // Analytic sphere modifiers (sculpt shape-accuracy S2; guarded by m_SphereSculptMutex).
    // The placement set published by TerrainModifierSystem; the version advances on every set
    // change and folds into the combined SphereSculptVersion so the render upload / Classify /
    // physics refresh gates re-arm exactly like a store write.
    std::array<CBTTerrain::SphereAnalyticFlatten, CBTTerrain::kMaxSphereAnalyticModifiers>
        m_SphereAnalytic{};
    uint32 m_SphereAnalyticCount = 0u;
    uint64 m_SphereAnalyticVersion = 0;

    // Analytic-while-stroking (S3; guarded by m_SphereSculptMutex). The FIFO queue holds every
    // uncommitted dab of the held stroke exactly as ApplySphereSculptDab would receive it; the
    // transient array is its folded analytic mirror (bitwise-equal consecutive centres merged),
    // each entry recording one past its last queue index so a mid-stroke overflow can commit an
    // oldest-first prefix without reordering. Empty outside strokes (flag-off dark-ship: never
    // touched).
    struct PendingSphereDab
    {
        float32 Cx, Cy, Cz;
        float32 AngularRadius;
        float32 Strength;
        bool Lower;
    };
    std::vector<PendingSphereDab> m_SphereStrokeQueue;
    std::array<CBTTerrain::SphereAnalyticDab, CBTTerrain::kMaxSphereAnalyticModifiers>
        m_SphereTransientDabs{};
    std::array<uint32, CBTTerrain::kMaxSphereAnalyticModifiers> m_SphereTransientQueueEnd{};
    uint32 m_SphereTransientDabCount = 0u;
    // The radius the sculpt store was last configured for (ConfigurePlanetSculpt) — the transient
    // dab's tangent-plane frame needs metres, and the store geometry doesn't carry the radius.
    float32 m_PlanetSculptRadius = 0.0f;

    // Persistence bookkeeping (guarded by m_SphereSculptMutex): the payload GUID whose content
    // currently backs the store (loaded or saved — EnsureSphereSculptLoaded no-ops on it) and
    // the sculpt version at the last save/load (needs-save = live version differs). The failed
    // guid is a warn-once negative cache (main-thread only, like the heightmap cache).
    GUID m_SphereSculptLoadedGuid{};
    uint64 m_SphereSculptSavedVersion = 0;
    GUID m_SphereSculptLoadFailedGuid{};
    // Unsaved-DAB flag behind SphereSculptNeedsSave: set by dab strokes and stroke undo/redo
    // restores, cleared by save/load/reset. See SphereSculptNeedsSave for why version deltas
    // (which every modifier bake / remap advances) are the wrong dirty signal.
    bool m_SphereSculptDabDirty = false;

    // Union every touched (face, UV rect) of an edit into the per-face physics dirty set.
    void AccumulateSphereSculptDirtyFaces(const CBTTerrain::SphereEditRegions& regions);

    // The store-dab body of ApplySphereSculptDab without the lock (the S3 commit replays queued
    // dabs through it under one lock hold — identical writes, order preserved).
    CBTTerrain::SphereEditRegions ApplySphereSculptDabLocked(float32 cx, float32 cy, float32 cz,
                                                             float32 angularRadius,
                                                             float32 strength, bool lower);
    // Commit the oldest `entries` transient entries' queued dabs to the store (FIFO), erase them
    // from the queue, and compact the transient arrays. Lock held by the caller.
    void FlushSphereStrokePrefixLocked(uint32 entries);

    std::unique_ptr<TerrainHeightPages> m_HeightPages;
    std::unique_ptr<HeightPageStoreLoader> m_HeightPageStores;
    bool m_HeightModifiersGathered = false;
    bool m_HeightModifiersPresent = false;

    mutable std::mutex m_Mutex;

    static std::unique_ptr<TerrainService> s_Instance;
    static uint64 s_Generation;
};

// Real render geometry for a tiled terrain's unified GPU source. WorldSizeX/Z is the
// REAL per-axis extent (tilesPerAxis * tileWorldSize) the tile grid covers — NOT the
// power-of-2 square the global CDLOD quadtree rounds up to. CBT maps
// world = origin + uv*size and samples the unified heightmap at that uv 1:1, so the
// squared/pow2 quadtree size would stretch a non-square or non-pow2-tile-count terrain
// (a 3-tile axis would render 4/3 too wide with a phantom clamped-edge band). Unified
// resolution = tilesPerAxis * (tileRes-1) + 1 — the grid an untiled terrain of equal
// content uses. Pure function of the config so it is directly unit-testable.
struct TiledRenderExtent
{
    float32 WorldSizeX = 0.0f;
    float32 WorldSizeZ = 0.0f;
    uint32 TilesPerAxisX = 0;
    uint32 TilesPerAxisZ = 0;
    uint32 TileInteriorX = 0;  // tileRes - 1 (shared-edge spacing)
    uint32 TileInteriorZ = 0;
    uint32 UnifiedWidth = 0;
    uint32 UnifiedHeight = 0;
};
TiledRenderExtent ComputeTiledRenderExtent(const TiledTerrainConfig& config);

// Sample a tiled terrain's normalized height (nominally [0,1]; modifier bakes
// accumulate unclamped, so values outside that range are real terrain — before
// HeightScale/WorldOriginY) at world XZ: select the resident tile containing the
// point and bilinearly sample its heightfield at the tile-local UV. Returns false
// when the point is outside the terrain footprint or the containing tile is not
// resident (Empty/missing). The editor terrain ray-march (TerrainPicking) uses this
// for tiled terrains, mirroring HeightfieldData::SampleBilinear for the single path,
// so brush/zone strokes hit tiled terrains too. Caller must hold the tiled data
// alive for the call (extraction/modifier/tool paths run sequentially).
bool SampleTiledHeightNormalized(const TiledTerrainData& tiled,
                                 float32 worldX, float32 worldZ, float32& outHeight);

// Fill heightfield samples in [minX, maxX] x [minZ, maxZ] (inclusive,
// clamped) from a terrain's base source (bake stage A, design §3.1):
// procedural noise (kBaseNoise*), a decoded heightmap (bilinear over global
// UV when resolutions differ), or flat zero. Deterministic per-sample, so a
// region fill is an exact subset of a full fill — region re-bakes rely on
// this. `heightmap` is the decoded source for HeightmapAsset (from
// TerrainService::ResolveHeightmapAsset); null/empty falls back to flat zero
// so a missing asset is visually obvious rather than silently noise.
void FillHeightfieldBaseRegion(Terrain::HeightfieldData& heightfield,
                               Components::TerrainBaseSource source,
                               const Terrain::HeightfieldData* heightmap,
                               int32 minX, int32 minZ, int32 maxX, int32 maxZ);

// The tiled analogue of FillHeightfieldBaseRegion: fill samples in [minX, maxX] x
// [minZ, maxZ] (inclusive, clamped) of `field`, a grid laid over the world rect from
// (fieldOriginX, fieldOriginZ) to + (fieldSizeX, fieldSizeZ) of a tiled terrain whose
// corner is (terrainOriginX, terrainOriginZ), from that terrain's `config.Base`:
//   ProceduralNoise  world-space tile noise (kTileNoise*);
//   HeightmapAsset   the heightmap spread over the authored footprint (config.WorldSizeX
//                    x config.WorldSizeZ from the corner) and sampled bilinearly at each
//                    sample's position in it, so a tiled terrain shows the same relief an
//                    untiled terrain of that size shows; positions past the footprint (the
//                    last tile column and row can overhang it) take the heightmap's edge;
//   Flat             zero, and so is a heightmap that is missing or failed to decode.
// Every value depends only on the sample's world position, so tiles agree on their
// shared edge and a region fill is an exact subset of a full fill.
void FillTiledBaseRegion(Terrain::HeightfieldData& field, const TiledTerrainConfig& config,
                         float32 terrainOriginX, float32 terrainOriginZ,
                         float32 fieldOriginX, float32 fieldOriginZ,
                         float32 fieldSizeX, float32 fieldSizeZ,
                         int32 minX, int32 minZ, int32 maxX, int32 maxZ);

// The resident-window atlas's out-of-window coarse base field (BuildCoarseHeightField's
// `baseField`): every sample of `field` from `tiled`'s base (FillTiledBaseRegion), with
// the field laid over the tile grid's extent (ComputeTiledRenderExtent) from the terrain's
// corner. That is the rect BuildCoarseHeightField maps its texels over, and the grid can
// overhang the authored size, so a field over the authored size would put a heightmap
// base's relief at the wrong texels.
void FillAtlasCoarseBaseField(Terrain::HeightfieldData& field, const TiledTerrainData& tiled);

// Reset a splatmap to the unbaked state: sized to the heightfield, RGBA8, all zero.
// Surface rules composite the material placement on top (ApplySplatModifiers).
//
// ZERO IS THE ONLY CORRECT BASE, for two independent reasons. CompositeSplatTexel
// adds a row's weight then renormalizes the texel, so a channel-0-full base would
// pin channel 0 at 1.0 in the accumulator and cap any row targeting another channel
// at 50%. And an all-zero texel is not "no data" — the surface resolves it to
// channel 0 (cbt_surface.glsl's wSum guard, normalizeLayerWeights in the grass
// shaders), so "unbaked reads as the first material" is the shipped convention and
// a texel no rule claims already renders correctly.
void ResetSplatmap(const Terrain::HeightfieldData& heightfield,
                   std::vector<uint8>& outSplatmap,
                   uint32& outWidth, uint32& outHeight);

// Region-scoped variant: zero only the texels in [minX, maxX] x [minZ, maxZ]
// (inclusive, clamped), leaving the rest of the splatmap untouched. The splatmap
// must already be sized to the heightfield.
void ResetSplatmapRegion(const Terrain::HeightfieldData& heightfield,
                         std::vector<uint8>& splatmap,
                         int32 minX, int32 minZ, int32 maxX, int32 maxZ);

// Generate an R16G16_FLOAT normal map from a heightfield using central differences.
// Stores XZ normal components as float16; Y reconstructed in shader.
// Optional neighbor heightfields provide correct cross-tile edge normals.
void GenerateNormalmapFromHeightfield(
    const Terrain::HeightfieldData& heightfield,
    float32 worldSizeX, float32 worldSizeZ,
    float32 heightScale,
    std::vector<uint8>& outNormalmap,
    uint32& outWidth, uint32& outHeight,
    const Terrain::HeightfieldData* neighborLeft = nullptr,
    const Terrain::HeightfieldData* neighborRight = nullptr,
    const Terrain::HeightfieldData* neighborUp = nullptr,
    const Terrain::HeightfieldData* neighborDown = nullptr);

// Region-scoped variant: recompute only normal texels in
// [minX, maxX] x [minZ, maxZ] (inclusive, clamped), leaving the rest of the
// normalmap untouched. The normalmap must already be sized to the heightfield.
// Callers must pad the region by the central-difference radius (1 sample)
// beyond the changed heightfield samples. Optional neighbor heightfields give
// bit-identical cross-tile edge normals to the full generator, so a region
// edit that touches a tile boundary keeps the two slots' shared-edge normal
// equal (the atlas boundary-seam invariant).
void GenerateNormalmapRegionFromHeightfield(
    const Terrain::HeightfieldData& heightfield,
    float32 worldSizeX, float32 worldSizeZ,
    float32 heightScale,
    std::vector<uint8>& normalmap,
    int32 minX, int32 minZ, int32 maxX, int32 maxZ,
    const Terrain::HeightfieldData* neighborLeft = nullptr,
    const Terrain::HeightfieldData* neighborRight = nullptr,
    const Terrain::HeightfieldData* neighborUp = nullptr,
    const Terrain::HeightfieldData* neighborDown = nullptr);

// Bilinear-upsample a square R32F field from srcDim x srcDim to dstDim x dstDim,
// writing dstDim*dstDim floats into `out` (resized as needed). The endpoint texels
// (u,v in {0,1}) sample the source corners exactly, so two adjacent tiles' shared
// edge upsamples to bit-identical values, as the terrain's base already agrees there
// (it resolves per world position): that keeps tile seams crack-free. Used to render a Coarse (not-yet-Full) streamed tile as a
// smooth low-frequency approximation of its Full bake instead of a flat/black hole
// (planet-streaming coarse-tile rendering). No-op when srcDim < 2 or dstDim < 2.
void UpsampleHeightfieldBilinear(const float32* src, uint32 srcDim,
                                 std::vector<float32>& out, uint32 dstDim);

// Bilinear-upsample a square RGBA8 splatmap from srcDim x srcDim to dstDim x dstDim
// (per-channel bilinear, round-to-nearest). Endpoints exact, like the height variant.
void UpsampleSplatmapBilinear(const uint8* src, uint32 srcDim,
                              std::vector<uint8>& out, uint32 dstDim);

// Which sides of a coarse tile's unified patch are shared with a resident-Full
// neighbor. In the unified texture adjacent tiles overlap by one texel (tile
// (tx,tz) starts at tx*interior), so a coarse tile's boundary row/column IS the
// Full neighbor's edge. A coarse write there would stomp the Full side's exact +
// modifier-applied edge with approximate, modifier-free data — a persistent 1-2
// texel seam trench until the coarse tile upgrades. The Full side owns those
// shared texels; a coarse↔coarse edge upsamples bit-identically (endpoints exact)
// so it needs no exclusion.
struct CoarsePatchNeighbors
{
    bool LeftFull = false;   // -X neighbor is resident Full
    bool RightFull = false;  // +X neighbor is resident Full
    bool TopFull = false;    // -Z neighbor is resident Full
    bool BottomFull = false; // +Z neighbor is resident Full
};

// The sub-rect (in a coarse tile's regionDim x regionDim upsample) that should be
// uploaded, excluding the 1-texel boundary shared with a resident-Full neighbor.
struct CoarsePatchRect
{
    uint32 X0 = 0, Z0 = 0, Width = 0, Height = 0;
};

// Pure function: the interior-clamped upload rect for a coarse tile given which
// neighbors are resident Full. Directly unit-testable (the coarse↔Full shared-edge
// oracle) — the discriminating check the coarse↔coarse oracle could not see.
CoarsePatchRect ComputeCoarsePatchRect(uint32 regionDim, const CoarsePatchNeighbors& neighbors);

// Copy the [x0,z0]+[width,height] sub-block out of a regionDim x regionDim row-major
// source into a tightly-packed destination (resized as needed). T is the per-texel
// unit: float32 for the R32F heightmap, uint32 for a packed RGBA8 splat texel.
template<typename T>
void ExtractSubBlock(const T* src, uint32 regionDim, uint32 x0, uint32 z0,
                     uint32 width, uint32 height, std::vector<T>& out)
{
    out.resize(static_cast<std::size_t>(width) * height);
    for (uint32 z = 0; z < height; ++z)
        for (uint32 x = 0; x < width; ++x)
            out[static_cast<std::size_t>(z) * width + x] =
                src[static_cast<std::size_t>(z0 + z) * regionDim + (x0 + x)];
}

} // namespace GameEngine::TerrainECS
