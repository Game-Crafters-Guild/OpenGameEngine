#pragma once

#include "ECS/Systems.h"
#include "Engine/Rendering/SunGlareRenderFeature.h" // ViewSkyline (per-frame scratch)
#include "TerrainECS/GrassWindVolumeExtractor.h"
#include "Terrain/Heightfield.h" // HeightfieldData scratch for atlas normal regen
#include "TerrainECS/TerrainAtlas.h" // kAtlasUpgradeFadeSeconds (device-free, pure)
#include "TerrainECS/TerrainReprovisionDebounce.h" // coalesce SamplesPerMeter/size drags
#include "Types/Types.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::TerrainECS
{

class TileStreamingManager;
class AtlasResidencyController;
class TerrainService;
class TerrainGrassFieldUploader;

// ECS system that handles terrain data lifecycle and heightmap/splat/normal GPU upload.
// The CBT renderer consumes the extracted params + bindless textures; the upload flush
// runs in TerrainUploadNode.
// One tiled terrain's atlas residency, flattened for readers outside this module (the debug
// server). Reported rather than the controller itself so its internals stay private.
//
// The two groups have DIFFERENT temporal meanings and a reader that conflates them will misread a
// healthy frame as a broken one: SlotCount/ResidentCount/TableVersion are LEVEL — they describe the
// window as it stands — while the *ThisFrame counters are EDGE-triggered and reset every update, so
// a parked camera over a settled window correctly reports zeros for all of them.
struct AtlasResidencySnapshot
{
    uint32 SlotCount = 0;     // configured slot budget
    uint32 ResidentCount = 0; // slots currently holding a tile
    uint32 TileRes = 0;
    uint64 TableVersion = 0;

    uint32 AssignsThisFrame = 0;
    uint32 EvictionsThisFrame = 0;
    uint32 UploadsThisFrame = 0;
    // Tiles inside the window that wanted a slot and did not get one, so they render coarse. A
    // standing non-zero here is the signal that the slot budget is short for the window.
    uint32 FallbacksThisFrame = 0;
    uint32 ActiveFadesThisFrame = 0;
};

class TerrainExtractionSystem : public ECS::ISystem
{
public:
    explicit TerrainExtractionSystem(Engine::Renderer::RenderServices* renderServices);
    ~TerrainExtractionSystem() override;

    const char* GetName() const override { return "TerrainExtractionSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Atlas residency for a tiled terrain, keyed by TiledTerrainHandle INDEX. False when that
    // terrain has no atlas controller at all — the unified height path allocates none and does zero
    // atlas work — which is a different answer from "an atlas with nothing resident".
    //
    // Index only, no generation: the controller map is itself keyed that way, and the index is
    // recycled. ForgetTiledTerrain / ForgetRetiredTiledTerrains drop a retired index's controller,
    // so a recycled index does not normally resolve to its predecessor's state — but nothing here
    // re-checks the generation, so treat this as a diagnostic read rather than an identity-safe
    // one. Threading the generation through m_AtlasByTiled is the fix if a correctness path ever
    // wants it.
    bool TryGetAtlasResidency(uint32 tiledTerrainIndex, AtlasResidencySnapshot& outSnapshot) const;

    // Extraction ticks on which a terrain's regional grass controls existed but were not yet
    // sampleable, so placement was refused for that terrain. Monotonic across the session, and
    // the only frame-exact answer to "how long does adding a region blank the grass". A single
    // step is the one tick a freshly created control texture needs; a climbing count is the
    // gate firing repeatedly, which would be a defect rather than a transient.
    uint64 GrassControlSuppressedTicks() const { return m_GrassControlSuppressedTicks; }

private:
    uint64 m_GrassControlSuppressedTicks = 0;

    // Marches the terrain skyline along the sun's azimuth for each view and leaves the answer
    // on the sun-glare feature. Here rather than in the renderer because the renderer cannot
    // depend on TerrainECS (the dependency runs the other way), and the glare's render node has
    // a camera but no ECS world.
    void PublishSunGlareSkyline(ECS::World& world);
    // Reused across frames so the per-frame refill does not allocate.
    std::vector<Engine::Renderer::SunGlareRenderFeature::ViewSkyline> m_SkylineScratch;
    GrassWindVolumeExtractor m_GrassWindVolumes;

    // Drop every per-terrain tracker (tile streaming state, atlas controller, coarse-build/
    // row-publish/warn state, re-provision debounce) for a retired TiledTerrainHandle index, so a
    // new terrain reusing the index starts fresh.
    void ForgetTiledTerrain(uint32 tiledIndex);

    // Forget the trackers of every tiled slot whose recorded generation the service no
    // longer resolves. The trackers key on the slot INDEX, which TerrainService recycles,
    // so a teardown outside this system (scene close, entity delete, component remove)
    // would otherwise hand a reused index the previous terrain's state.
    void ForgetRetiredTiledTerrains(TerrainService& service);

    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    std::unique_ptr<TileStreamingManager> m_StreamingManager;
    std::unique_ptr<TerrainGrassFieldUploader> m_GrassFieldUploader;

    // This system's cursor into each terrain's HeightfieldDirtyLog: the
    // HeightfieldVersion whose data was last uploaded to the GPU. Keyed by
    // slot index; a generation mismatch (slot reused) resets the cursor to 0,
    // which re-reads the log from the CreateTerrain full-dirty entry.
    struct HeightfieldUploadCursor
    {
        uint32 Generation = 0;
        uint64 Version = 0;
    };
    std::unordered_map<uint32, HeightfieldUploadCursor> m_HeightfieldUploadCursors;
    // The TerrainRenderFeature::GetDeviceResourceEpoch every terrain was last uploaded for.
    uint64 m_UploadedDeviceResourceEpoch = 0;

    // Reusable scratch for upsampling a Coarse (not-yet-Full) streamed tile's
    // heightfield/splatmap to the unified tile-region resolution so it renders a
    // smooth low-frequency approximation of its Full bake instead of a flat/black
    // hole. Re-derived only when a coarse tile is dirty (once per stream-in), so
    // a parked camera pays nothing. The *Patch buffers hold the interior-clamped
    // sub-block actually uploaded (boundary texels shared with a resident-Full
    // neighbor are excluded so a coarse write never stomps the Full edge).
    std::vector<float32> m_CoarseHeightScratch;
    std::vector<uint8> m_CoarseSplatScratch;
    std::vector<float32> m_CoarseHeightPatch;
    std::vector<uint32> m_CoarseSplatPatch;
    // Normals for a coarse (not-yet-Full) tile region are regenerated from its upsampled heights
    // (was ZEROED — the tiled coarse dark-shard gap): the full region normal + the interior-clamped
    // sub-block actually uploaded, plus a HeightfieldData wrap of the upsampled scratch.
    std::vector<uint8> m_CoarseNormalScratch;
    std::vector<uint32> m_CoarseNormalPatch;
    Terrain::HeightfieldData m_CoarseNormalHf;

    // Phase E: single-slot pack scratch (slotStride^2 floats) reused across the atlas slot
    // patches so each tile upload does not reallocate. Resized on a tileRes change.
    std::vector<float32> m_AtlasSlotScratch;

    // Region-edit scratch (editing real-time part 2): the tightly-packed apron-extended sub-rect a
    // brush dab uploads into a resident slot, reused across the height/normal/splat region uploads of
    // one patch (each upload copies into its own staging buffer, so the scratch is free to reuse). A
    // dab is ~16 texels, so this stays small; a whole-tile fallback resizes it once.
    std::vector<uint8> m_AtlasEditScratch;

    // Unified-path region-edit scratch (sculpt real-time on the DEFAULT tiled terrain,
    // which renders through one unified texture set sized to the whole terrain, not the
    // resident-window atlas): the tightly-packed sub-rect a brush dab uploads into a
    // tile's unified sub-window, reused across the height/normal/splat region uploads of
    // one patch. Height is R32F (float32); normal (R16G16F) + splat (RGBA8) are 4-byte
    // texels packed as uint32. Small (a dab is ~tens of texels); a whole-tile fallback
    // resizes them once.
    std::vector<float32> m_UnifiedHeightRegionScratch;
    std::vector<uint32> m_UnifiedSurfaceRegionScratch;

    // Quality-sweep slice 1: atlas SPLAT (RGBA8) + NORMAL (R16G16F) slot sources ride the same
    // slot geometry as height. Reused per-patch scratch so streaming does not reallocate. The
    // *SlotScratch hold the packed slotStride^2 block (apron included) uploaded to the atlas; the
    // *TileScratch hold the per-tile source at tileRes (upsampled splat / freshly regenerated
    // normal). The normal is regenerated from the tile's POST-modifier heights every patch (fixing
    // the tiled base-noise-normal gap on atlas terrains too), via GenerateNormalmapFromHeightfield
    // over m_AtlasNormalTileHf (a coarse tile's upsampled heights; a Full tile uses its own field).
    std::vector<uint8> m_AtlasSplatSlotScratch;
    std::vector<uint8> m_AtlasNormalSlotScratch;
    std::vector<uint8> m_AtlasSplatTileScratch;
    std::vector<uint8> m_AtlasNormalTileScratch;
    Terrain::HeightfieldData m_AtlasNormalTileHf;
    // Out-of-window coarse splat (RGBA8, downsampled tile splats) + coarse normal (R16G16F, derived
    // from the coarse HEIGHT field so distant shading matches the coarse geometry — no lit bumps on
    // smooth terrain). Rebuilt with the coarse height field (same gate), so a parked camera pays nothing.
    std::vector<uint8> m_AtlasCoarseSplatScratch;
    std::vector<uint8> m_AtlasCoarseNormalScratch;
    Terrain::HeightfieldData m_AtlasCoarseNormalHf;
    // Whole-terrain base relief (coarseDim^2) from the terrain's base (FillTiledBaseRegion), so a coarse
    // texel with no streamed tile resolves to real low-frequency relief continuous with resident
    // tiles instead of a flat mid-plane (the large-terrain "dissolve into a flat sheet" at altitude).
    Terrain::HeightfieldData m_AtlasCoarseBaseHf;

    // Region-scope a resident-slot content edit to the dab's sub-rect (editing real-time part 2).
    // GE_TERRAIN_ATLAS_NO_REGION clears it -> every edit takes the whole-slot path (the pre-change
    // behavior); the before/after measurement lever + runtime kill switch.
    bool m_AtlasRegionPatch = true;
    // Region-scope a resident tile's UNIFIED-texture content edit to the dab's sub-rect,
    // mirroring m_AtlasRegionPatch for the default (non-atlas) tiled path. Pre-fix the
    // unified path re-uploaded the whole tile (height+splat+normal, ~12 MB @1025^2) and
    // regenerated the whole-tile normal (~1M texels) per dab — the ~50 ms/dab sculpt
    // hitch. GE_TERRAIN_UNIFIED_NO_REGION forces the whole-tile path (the before/after
    // measurement lever + runtime kill switch). Default: region patching ON.
    bool m_UnifiedRegionPatch = true;
    uint32 m_AtlasSlotOverride = 0; // GE_TERRAIN_ATLAS_SLOTS; 0 = default budget
    // Coarse->slot upgrade crossfade window in seconds (kAtlasUpgradeFadeSeconds). GE_TERRAIN_ATLAS_FADE
    // overrides it; setting it to 0 restores the pre-fade instant swap (before/after + kill switch).
    float32 m_AtlasFadeSeconds = kAtlasUpgradeFadeSeconds;
    uint64 m_AtlasFrameCounter = 0; // monotonic (device frame index cycles)
    std::unordered_map<uint32, std::unique_ptr<AtlasResidencyController>> m_AtlasByTiled;

    // Out-of-window coarse height field (design §8 Risk 3): the small always-resident
    // downsample an out-of-window tile resolves through so the fallback is height-continuous
    // with resident relief at the streaming-window edge. Rebuilt (BuildCoarseHeightField) only
    // on the first atlas frame or a frame that integrated new tile detail — a parked camera
    // rebuilds nothing. Reusable build scratch; the built-once set is keyed by
    // TiledTerrainHandle index and cleared on that terrain's teardown so a reused slot rebuilds.
    std::vector<float32> m_AtlasCoarseScratch;
    std::unordered_set<uint32> m_AtlasCoarseBuilt;

    // Warn-once (keyed by TiledTerrainHandle index): the atlas indirection table silently caps at
    // kAtlasMaxTiles.
    std::unordered_set<uint32> m_AtlasOverCapWarnedTiled;

    // Settle gate for a tiled terrain's structural re-provision (SamplesPerMeter / world size),
    // keyed by TiledTerrainHandle index. A resolution/size change from an inspector drag emits a
    // distinct value almost every frame; without a settle window each one tore the whole terrain
    // down and rebuilt it (VRAM churn + dangling bindless descriptors -> device-lost). The key
    // stays valid across the settle because the terrain is NOT destroyed until it fires. Cleared
    // in ForgetTiledTerrain (every tiled teardown) so a reused slot index starts fresh.
    std::unordered_map<uint32, TerrainReprovisionDebounce> m_ReprovisionDebounce;

    // Last generation this system saw live for each tiled slot index — the input
    // ForgetRetiredTiledTerrains tests the service against.
    std::unordered_map<uint32, uint32> m_TiledSlotGeneration;
};

} // namespace GameEngine::TerrainECS
