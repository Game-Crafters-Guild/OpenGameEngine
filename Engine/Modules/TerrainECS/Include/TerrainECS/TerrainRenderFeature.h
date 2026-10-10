#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Terrain/CDLODQuadtree.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/GrassWindVolumeRing.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainGpuBake.h"
#include "TerrainECS/TerrainMaterialLibraryCache.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/DeviceFrameCounter.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::TerrainECS
{

// Extract camera world position from a column-major LH view matrix.
// Camera position = -transpose(R) * t, where R = upper-left 3x3, t = column 3.
inline Mathematics::Vector3 ExtractCameraPosition(const Rendering::CameraData& cam)
{
    const float* v = cam.view;
    return Mathematics::Vector3(
        -(v[0]*v[12] + v[1]*v[13] + v[2]*v[14]),
        -(v[4]*v[12] + v[5]*v[13] + v[6]*v[14]),
        -(v[8]*v[12] + v[9]*v[13] + v[10]*v[14]));
}

// Cached terrain instance data extracted by TerrainExtractionSystem and consumed by
// the CBT renderer (via GetActiveTerrains): world bounds, height scale, and the
// heightmap/splatmap/normalmap bindless indices CBT + grass sample. The quadtree
// snapshot remains for the service-side CDLOD LOD-range machinery (TerrainService
// builds it; nav/debug read it).
struct TerrainInstanceInfo
{
    float32 WorldOriginX;
    float32 WorldOriginY;
    float32 WorldOriginZ;
    float32 SizeX;
    float32 SizeZ;
    float32 HeightScale;
    float32 LODRangeScale;
    uint32 LODLevels;
    float32 MaterialTiling = 10.0f;
    TerrainHandle Handle;
    Rendering::TextureHandle HeightmapTexture; // cached from UploadHeightmap
    uint32 HeightmapBindlessIndex = 0;         // bindless descriptor index for current frame (0 = not registered)
    Rendering::TextureHandle SplatmapTexture;  // cached from UploadSplatmap
    uint32 SplatmapBindlessIndex = 0;          // bindless descriptor index (0 = not registered)
    uint32 NormalmapBindlessIndex = 0;         // bindless descriptor index for R16G16_FLOAT normal map

    bool IsTiled = false;
    // Terrain.CastShadows: whether the terrain shadows the directional light (its clearance map).
    bool CastShadows = true;

    // Phase E resident-window atlas (design §3). When AtlasBacked, the CBT height sample
    // resolves terrain UV -> tile -> slot through AtlasRows + AtlasHeightTexture instead of
    // sampling a unified heightmap (HeightmapBindlessIndex is 0 for these — no unified
    // texture is created, which is what removes the 8193/axis cap). The geometry mirrors the
    // AtlasGeometry the extraction system packed the slots with.
    bool AtlasBacked = false;
    Rendering::TextureHandle AtlasHeightTexture;
    Rendering::TextureHandle AtlasCoarseTexture; // out-of-window fallback field
    uint32 AtlasCoarseDim = 0;
    // Quality-sweep slice 1: the surface samples splat/normal per-pixel through the atlas resolve
    // via these bindless indices (0 = unregistered). Resident tiles tap AtlasSplat/AtlasNormal at
    // the resolved slot UV; out-of-window tiles tap AtlasSplatCoarse/AtlasNormalCoarse at terrain UV.
    uint32 AtlasSplatBindlessIndex = 0;
    uint32 AtlasNormalBindlessIndex = 0;
    uint32 AtlasSplatCoarseBindlessIndex = 0;
    uint32 AtlasNormalCoarseBindlessIndex = 0;
    // Grass placement samples the atlas height (R32F) + coarse-height fields BINDLESS (via the
    // same GE_BTEX path CBT's compute binds directly at binding 18): the grass compute has no
    // fixed sampler-ring binding, so height rides the bindless set exactly like splat/normal.
    uint32 AtlasHeightBindlessIndex = 0;
    uint32 AtlasCoarseBindlessIndex = 0;
    bool GrassRegionsActive = false;
    uint32 AtlasGrassBindlessIndex = 0;
    uint32 AtlasGrassCoarseBindlessIndex = 0;
    // The regional grass control field as extraction resolved it this tick. The bindless
    // index is the unified map's (0 on an atlas terrain, which reads the two above), and
    // SuppressedPlacement is the admission gate having refused to place grass because a
    // required field was not yet uploaded and published. Diagnostics only: the placement
    // path reads none of these, and without them "is the region live?" has no answer
    // short of a screenshot.
    uint32 GrassControlBindlessIndex = 0;
    bool GrassControlsSuppressedPlacement = false;
    uint32 AtlasDim = 0;
    uint32 AtlasSlotStride = 0;
    uint32 AtlasSlotsPerRow = 0;
    uint32 AtlasTileRes = 0;
    uint32 AtlasTilesPerAxisX = 0;
    uint32 AtlasTilesPerAxisZ = 0;
    uint64 AtlasTableVersion = 0;     // bumps on any slot (re)assignment (indirection upload gate)
    // Indirection table snapshot as opaque 16-B TileAtlasSlot rows (kept as bytes so this
    // widely-included header does not pull in TerrainAtlas.h). AtlasRowCount rows; the CBT
    // render feature uploads them verbatim to the indirection SSBO ring.
    std::vector<uint8_t> AtlasRowBytes;
    uint32 AtlasRowCount = 0;

    Terrain::CDLODQuadtree Quadtree;
    uint32 PatchGridSize = 32;
    bool MorphRangesComputed = false;
    float32 VisRanges[Terrain::kMaxLODLevels] = {};
    float32 MorphStart[Terrain::kMaxLODLevels] = {};
    float32 MorphEnd[Terrain::kMaxLODLevels] = {};
};

// The single atlas-backed terrain's resolve inputs the grass placement compute needs:
// the atlas geometry, the bindless height/normal/splat indices (resident + coarse), and a
// copy of the indirection rows for THIS frame. Only the atlas fields + rows are copied (never
// the heavy CDLODQuadtree on TerrainInstanceInfo), so a grass dispatch can pull it cheaply.
// The engine is single-atlas-terrain (CBT is single-terrain), so the FIRST atlas-backed
// terrain is THE one; a second would be an impossible config (guarded, not silently wrong).
enum class TerrainGrassMap : uint8 { Unified, Atlas, Coarse };

// The height textures the CBT renderer samples: the unified heightmap, or the resident atlas and
// its out-of-window coarse field for an atlas-backed terrain.
enum class TerrainCbtHeightTexture : uint8 { Heightmap, AtlasHeight, AtlasCoarse };

struct AtlasGrassSource
{
    bool Valid = false;
    // Version-independent identity of the atlas terrain (packed TerrainHandle): survives a
    // destroy+recreate as a DIFFERENT value even when the new controller's table version
    // numerically collides with the old one, so the grass row ring can force a fresh upload
    // instead of trusting a stale slot version (the ABA the plain version gate misses).
    uint64 Identity = 0;
    uint32 AtlasDim = 0;
    uint32 AtlasSlotStride = 0;
    uint32 AtlasSlotsPerRow = 0;
    uint32 AtlasTileRes = 0;
    uint32 AtlasTilesPerAxisX = 0;
    uint32 AtlasTilesPerAxisZ = 0;
    uint32 AtlasCoarseDim = 0;
    uint32 HeightBindless = 0;
    uint32 HeightCoarseBindless = 0;
    uint32 NormalBindless = 0;
    uint32 NormalCoarseBindless = 0;
    uint32 SplatBindless = 0;
    uint32 SplatCoarseBindless = 0;
    uint32 GrassBindless = 0;
    uint32 GrassCoarseBindless = 0;
    bool GrassRegionsActive = false;
    // The same eight maps as handles. A profile with no binding arrays (WebGPU) binds them by name
    // instead of indexing them, and one binding per map is exactly right here because grass
    // resolves a SINGLE atlas terrain — the multi-atlas case is warned about and unsupported.
    Rendering::TextureHandle HeightTexture;
    Rendering::TextureHandle HeightCoarseTexture;
    Rendering::TextureHandle NormalTexture;
    Rendering::TextureHandle NormalCoarseTexture;
    Rendering::TextureHandle SplatTexture;
    Rendering::TextureHandle SplatCoarseTexture;
    Rendering::TextureHandle GrassTexture;
    Rendering::TextureHandle GrassCoarseTexture;
    uint64 TableVersion = 0; // indirection upload gate: unchanged version -> parked, no re-upload
    uint32 RowCount = 0;
    std::vector<uint8_t> RowBytes; // AtlasRowCount * 16 B TileAtlasSlot rows
};

// Owns the GPU-side terrain textures + params SSBO the CBT renderer and grass consume.
// Created via RenderServices::EnsureFeature<TerrainRenderFeature>(). Renderer-agnostic:
// the CDLOD render core was deleted at the CBT cutover; this feature keeps the shared
// heightmap/normalmap/splatmap uploads, the per-terrain params SSBO, and the extracted
// ActiveTerrains list.
class TerrainRenderFeature : public Engine::Renderer::IRenderFeature
{
public:
    ~TerrainRenderFeature() override;

    bool Initialize(Rendering::IDevice* device);
    bool IsInitialized() const { return m_Initialized; }

    // Release GPU heightmap/splatmap/normalmap resources for a terrain handle.
    void ReleaseTerrainResources(TerrainHandle handle);

    // After an in-place device rebuild every texture, view, bindless slot, buffer, sampler and
    // pipeline this feature holds died with the old device. Forgets them without destroying them
    // (a stale handle could name a new resource), recreates the samplers and material tables
    // Initialize makes, and advances GetDeviceResourceEpoch so the extraction uploads every
    // terrain again from the terrain service's CPU data.
    void OnDeviceRebuilt(Rendering::IDevice* device) override;
    // Advances once per device rebuild (OnDeviceRebuilt). The extraction compares it with the
    // value it last restored for and, on a change, re-uploads every terrain's heights, surface
    // maps and grass fields from the CPU state the terrain service holds.
    uint64 GetDeviceResourceEpoch() const { return m_DeviceResourceEpoch.load(std::memory_order_acquire); }

    // Monotonic count of ReleaseTerrainResources calls — the "a terrain texture set was retired"
    // signal an in-place re-provision (SamplesPerMeter/size edit) raises. The CBT renderer samples
    // the unified heightmap through its OWN descriptor ring (a path bindless-slot invalidation does
    // not cover), so it polls this each frame and, on a bump, deterministically drops the retired
    // views from every ring element BEFORE this feature's frames-in-flight quarantine frees the
    // image. Cross-thread (extraction bumps, render reads); the value only needs to differ across
    // frames, so relaxed load/acquire is sufficient.
    uint64 GetTerrainTextureRetireGeneration() const
    {
        return m_TerrainTextureRetireGeneration.load(std::memory_order_acquire);
    }

    // Monotonic count of every change to the terrain TEXTURE content grass placement samples —
    // height, splat and normal, unified or atlas — plus every texture retirement. Bumped where the
    // write is QUEUED rather than where it is flushed, so a reader that consults it while declaring
    // this frame's render graph already sees a change the same frame's flush will apply.
    //
    // It exists because that content has no CPU-side bytes to compare: an idle-elision gate can
    // compare the params array byte for byte, but not a heightmap. Deliberately conservative — a
    // queued upload bumps it whether or not the texels differ — so it can only cost a recompute,
    // never hide one.
    uint64 GetGrassPlacementContentEpoch() const
    {
        return m_GrassContentEpoch.load(std::memory_order_acquire);
    }

    // The terrain params array as last published, for a caller that must compare content rather
    // than trust a version. Fills `out` rather than returning, so a per-frame caller can hoist the
    // buffer instead of allocating; empty when nothing has been uploaded yet.
    void CopyLastTerrainParamsBytes(std::vector<uint8>& out) const;

    // ---- Per-terrain GPU heightmap textures ----

    // Destination row band for a partial texture upload (full-width horizontal band).
    // RowCount == 0 means "upload the whole texture".
    struct UploadBand
    {
        uint32 DstRow = 0;
        uint32 RowCount = 0;
    };

    void UploadHeightmap(TerrainHandle handle,
                         const float32* samples, uint32 width, uint32 height);
    void UploadHeightmap(TerrainHandle handle,
                         const float32* samples, uint32 width, uint32 height,
                         const UploadBand& band);

    Rendering::TextureHandle GetHeightmapTexture(TerrainHandle handle) const;
    // The first active NON-atlas terrain's unified source maps, for consumers that bind them as
    // named textures (the compat grass placement; bindless targets read per-terrain indices
    // instead). Single-terrain shape, same contract as TryGetAtlasGrassSource. Returns false when
    // no non-atlas terrain is active; individual handles may still be invalid.
    bool TryGetUnifiedGrassMaps(Rendering::TextureHandle& height, Rendering::TextureHandle& normal,
                                Rendering::TextureHandle& splat, Rendering::TextureHandle* controls = nullptr) const;
    Rendering::SamplerHandle GetHeightmapSampler() const { return m_HeightmapSampler; }
    // The shared REPEAT sampler the compat profile binds for the layer maps (see CreateSamplers).
    Rendering::SamplerHandle GetLayerSampler() const { return m_LayerSampler; }
    uint32 GetHeightmapBindlessIndex(TerrainHandle handle) const;
    // Publishes the texture's global bindless slot — immediately when FlushPendingUploads has
    // recorded its GPU-side initialization OR is certain to record it this frame ahead of every
    // consumer, and not at all while neither holds (see RegisterBindless). Idempotent, and
    // designed to be re-called every frame: a registration refused because nothing is draining
    // the upload queue succeeds on the first frame after something does.
    void RegisterHeightmapBindless(TerrainHandle handle,
                                   Engine::Renderer::RenderServices& rs);

    // ---- Per-terrain GPU splatmap textures (RGBA8: 4 material layer weights) ----

    void UploadSplatmap(TerrainHandle handle,
                        const uint8* rgba8Data, uint32 width, uint32 height);
    void UploadSplatmap(TerrainHandle handle,
                        const uint8* rgba8Data, uint32 width, uint32 height,
                        const UploadBand& band);
    Rendering::TextureHandle GetSplatmapTexture(TerrainHandle handle) const;
    uint32 GetSplatmapBindlessIndex(TerrainHandle handle) const;
    void RegisterSplatmapBindless(TerrainHandle handle,
                                  Engine::Renderer::RenderServices& rs);

    // ---- Per-terrain GPU normal maps (R16G16_FLOAT: XZ normal components) ----

    void UploadNormalmap(TerrainHandle handle,
                         const uint8* rg8Data, uint32 width, uint32 height);
    void UploadNormalmap(TerrainHandle handle,
                         const uint8* rg8Data, uint32 width, uint32 height,
                         const UploadBand& band);
    uint32 GetNormalmapBindlessIndex(TerrainHandle handle) const;
    void RegisterNormalmapBindless(TerrainHandle handle,
                                   Engine::Renderer::RenderServices& rs);

    // ---- Unified tiled-terrain textures (C8) ----
    //
    // A tiled terrain renders through ONE unified heightmap/splatmap/normalmap
    // sized to the full terrain, keyed by its global GPU handle. Per-tile bakes
    // patch sub-rects via the graphics-queue band path (FlushPendingUploads) —
    // no transfer-queue uploads, so the transfer-queue barrier hazard is gone.
    // CBT and the surface sample it exactly like a single (untiled) terrain, so
    // the renderer stays completely tile-unaware.

    // Create/resize the unified textures to (width,height) and zero-initialize
    // them (a full discard upload, so subsequent region patches preserve). All
    // three share one resolution. Idempotent when already at the requested size.
    // Returns true only on a frame that (re)created the set — the caller re-marks
    // every resident tile dirty so the whole texture refills that frame. A
    // re-provision's VRAM staging barrier otherwise withholds the set for several
    // frames while tiles integrate; those uploads are dropped (no texture yet) but
    // the tiles are still cleared, so they never re-upload and freeze at the fresh
    // texture's zero-cleared content. Returns false while the set is withheld (or on
    // allocation failure): the caller must keep the tiles dirty and retry next frame.
    bool EnsureUnifiedTiledTextures(TerrainHandle handle, uint32 width, uint32 height);

    // Patch a tile's srcW*srcH block into the unified textures at destination
    // texel origin (dstX,dstY), preserving the rest (content-preserving copy).
    // No-op when the unified texture is missing or the rect exceeds it.
    void UploadHeightmapRegion(TerrainHandle handle, const float32* src,
                               uint32 srcW, uint32 srcH, uint32 dstX, uint32 dstY);
    void UploadSplatmapRegion(TerrainHandle handle, const uint8* src,
                              uint32 srcW, uint32 srcH, uint32 dstX, uint32 dstY);
    void UploadNormalmapRegion(TerrainHandle handle, const uint8* src,
                               uint32 srcW, uint32 srcH, uint32 dstX, uint32 dstY);

    // ---- Phase E resident-window atlas height texture (past the 8193 unified cap) ----
    //
    // One square R32_FLOAT atlas per tiled terrain, sized to the slot grid (AtlasDim^2),
    // NOT the world. The CBT compute binds it directly (binding 18) — never bindless — so
    // VertexEval resolves terrain UV -> tile -> slot -> atlas texel (design §3). A resident
    // tile's pre-packed slot block (apron + edge/corner ownership already applied CPU-side,
    // PackTileHeightIntoSlot) is uploaded via the same graphics-queue region path as the
    // unified textures (no transfer queue — C8). This scales to any world size: only the
    // window is resident, so a 50 km terrain fits the same fixed VRAM as an 8 km one.

    // Create/resize the atlas to atlasDim^2 and GPU-zero it (idempotent when already sized).
    void EnsureAtlasHeightTexture(TerrainHandle handle, uint32 atlasDim);
    // Patch one slot's slotStride^2 pre-packed block at atlas texel origin (dstX,dstY).
    void UploadAtlasHeightSlot(TerrainHandle handle, const float32* slotData,
                               uint32 slotStride, uint32 dstX, uint32 dstY);
    // Region variant: patch only a regionW*regionH sub-rect of a slot (a brush dab), content-
    // preserving, at absolute atlas texel origin (dstX,dstY). `data` is tightly-packed R32F bytes
    // (PackTileEditRegionIntoSlot). This is what makes a 16-texel edit cost a 16-texel upload
    // instead of a whole-slot (slotStride^2) re-upload.
    void UploadAtlasHeightSlotRegion(TerrainHandle handle, const uint8* data,
                                     uint32 regionW, uint32 regionH, uint32 dstX, uint32 dstY);
    // The layer-material textures a profile with no binding arrays binds by name. Only the first
    // two role slots: cbt_surface's compat arm clamps the layer ordinal to two bound layers and
    // wraps 2-3 onto the last, so these are the only ones a shader can address there. Role slot r
    // is a fixed per-terrain mapping (params.LayerRole[r]), which is what makes them bindable at
    // all — the blend ordinal is the role slot, not a per-pixel choice.
    struct CompatLayerTextures
    {
        Rendering::TextureHandle Albedo[2];
        Rendering::TextureHandle Normal[2];
        Rendering::TextureHandle Orm[2];
        // The blade's own maps. They ride here rather than on the grass feature because this is
        // where the extraction system already resolves terrain-adjacent texture GUIDs, and the
        // grass surface reads them from the same set the ground layers come from.
        Rendering::TextureHandle GrassAlbedo;
        Rendering::TextureHandle GrassAlpha;
        Rendering::TextureHandle GrassNormal;
    };
    void SetCompatLayerTextures(const CompatLayerTextures& textures);
    void SetCompatGrassTextures(Rendering::TextureHandle albedo, Rendering::TextureHandle alpha,
                                Rendering::TextureHandle normal);
    CompatLayerTextures GetCompatLayerTextures() const;

    // The maps a profile with no binding arrays binds by name rather than by bindless index.
    Rendering::TextureHandle GetNormalmapTexture(TerrainHandle handle) const;
    Rendering::TextureHandle GetAtlasNormalTexture(TerrainHandle handle) const;
    Rendering::TextureHandle GetAtlasNormalCoarseTexture(TerrainHandle handle) const;
    Rendering::TextureHandle GetAtlasSplatTexture(TerrainHandle handle) const;
    Rendering::TextureHandle GetAtlasSplatCoarseTexture(TerrainHandle handle) const;

    // The per-terrain coarse height field (design §8 Risk 3): a small square R32F downsample
    // of the whole terrain an out-of-window tile resolves to. Whole-texture upload (it is tiny),
    // done only when its content changed (extraction gates on a version).
    void EnsureAtlasCoarseTexture(TerrainHandle handle, uint32 coarseDim);
    void UploadAtlasCoarse(TerrainHandle handle, const float32* data, uint32 coarseDim);
    // Union a tile's TERRAIN-UV footprint into the shared height dirty rect so CBT bisectors
    // sampling through that slot re-evaluate when the slot content re-uploads (the #505
    // dirty-rect channel, mapped through the atlas indirection — the rect stays in terrain-UV
    // space, which is where Classify tests bisector UVs; the slot indirection is transparent
    // to it). Drained by the existing TakeUnifiedHeightDirtyRect path.
    void AccumulateAtlasHeightDirtyUV(TerrainHandle handle, float32 minU, float32 minV,
                                      float32 maxU, float32 maxV);

    // ---- Phase E atlas splat + normal surface sources (quality-sweep slice 1) ----
    //
    // Splat (RGBA8, 4 layer weights) and normal (R16G16_FLOAT, world nx/nz) ride the SAME slot
    // geometry / apron / edge ownership as the height atlas (AtlasDim^2), packed CPU-side by the
    // multi-component PackTileComponentsIntoSlot. Unlike the compute-bound height atlas, these are
    // sampled PER-PIXEL by the surface fragment, so they are registered BINDLESS (GE_BTEX) exactly
    // like the per-terrain splatmap/normalmap; the surface resolves terrain UV -> slot UV and taps
    // them there, or the coarse field below out of the resident window.
    void EnsureAtlasSplatTexture(TerrainHandle handle, uint32 atlasDim);
    void UploadAtlasSplatSlot(TerrainHandle handle, const uint8* rgba8Data,
                              uint32 slotStride, uint32 dstX, uint32 dstY);
    // Region variant (see UploadAtlasHeightSlotRegion): patch a RGBA8 sub-rect of a slot.
    void UploadAtlasSplatSlotRegion(TerrainHandle handle, const uint8* data,
                                    uint32 regionW, uint32 regionH, uint32 dstX, uint32 dstY);
    void EnsureAtlasNormalTexture(TerrainHandle handle, uint32 atlasDim);
    void UploadAtlasNormalSlot(TerrainHandle handle, const uint8* rg16Data,
                               uint32 slotStride, uint32 dstX, uint32 dstY);
    // Region variant (see UploadAtlasHeightSlotRegion): patch a R16G16F sub-rect of a slot.
    void UploadAtlasNormalSlotRegion(TerrainHandle handle, const uint8* data,
                                     uint32 regionW, uint32 regionH, uint32 dstX, uint32 dstY);
    // Out-of-window coarse splat (RGBA8) + normal (R16G16_FLOAT) fields — the frontier fallback,
    // continuous with resident content (design §8 Risk 3). Whole-texture uploads (tiny).
    void EnsureAtlasSplatCoarseTexture(TerrainHandle handle, uint32 coarseDim);
    void UploadAtlasSplatCoarse(TerrainHandle handle, const uint8* rgba8Data, uint32 coarseDim);
    void EnsureAtlasNormalCoarseTexture(TerrainHandle handle, uint32 coarseDim);
    void UploadAtlasNormalCoarse(TerrainHandle handle, const uint8* rg16Data, uint32 coarseDim);
    // Register all four atlas surface textures with the global bindless set (once-only per
    // (re)creation) and read their GE_BTEX indices (0 = unregistered). The surface reads these
    // from CBTSurfaceParams: resident slot taps use atlas splat/normal, out-of-window taps use coarse.
    // Also registers the atlas HEIGHT + coarse-height (R32F) textures bindless — grass samples
    // those (not just splat/normal), and the compute has no fixed sampler-ring binding for them.
    void RegisterAtlasSurfaceBindless(TerrainHandle handle, Engine::Renderer::RenderServices& rs);
    uint32 GetAtlasSplatBindlessIndex(TerrainHandle handle) const;
    uint32 GetAtlasNormalBindlessIndex(TerrainHandle handle) const;
    uint32 GetAtlasSplatCoarseBindlessIndex(TerrainHandle handle) const;
    uint32 GetAtlasNormalCoarseBindlessIndex(TerrainHandle handle) const;
    uint32 GetAtlasHeightBindlessIndex(TerrainHandle handle) const;
    uint32 GetAtlasCoarseBindlessIndex(TerrainHandle handle) const;

    // Copy the first atlas-backed terrain's grass resolve inputs (geometry + bindless indices +
    // this frame's indirection rows) into `out`, returning true when one exists. Copies only the
    // atlas fields + rows (never the Quadtree), so it is cheap enough for the per-frame grass path.
    bool TryGetAtlasGrassSource(AtlasGrassSource& out) const;

    // Optional RG8 regional grass controls. Clear to neutral (1,1), then patch
    // through TerrainUpload; publication follows the existing initialization gate.
    bool EnsureGrassFieldTexture(TerrainHandle handle, TerrainGrassMap map, uint32 width, uint32 height);
    bool UploadGrassField(TerrainHandle handle, TerrainGrassMap map, const uint8* data,
                          uint32 width, uint32 height, uint32 dstX = 0, uint32 dstY = 0);
    Rendering::TextureHandle GetGrassFieldTexture(TerrainHandle handle, TerrainGrassMap map) const;
    uint32 GetGrassFieldBindlessIndex(TerrainHandle handle, TerrainGrassMap map) const;
    void RegisterGrassFieldsBindless(TerrainHandle handle, Engine::Renderer::RenderServices& rs);
    bool GrassFieldCanBeSampled(TerrainHandle handle, TerrainGrassMap map) const;
    // A height texture the CBT renderer samples, once its zero-clear or first whole-texture copy
    // has been recorded; invalid until then. The CBT renderer binds these raw handles through its
    // own descriptor ring, which carries no render-graph edge from the upload pass, so a texture
    // published in the frame its initializer is queued is sampled ahead of it, in its undefined
    // layout. Every height texture published to the CBT renderer goes through this getter.
    Rendering::TextureHandle GetInitializedCbtHeightTexture(TerrainHandle handle, TerrainCbtHeightTexture source) const;
    void ReleaseGrassFieldResources(TerrainHandle handle);

    // ---- Unified-texture height dirty-region accumulator (CBT streaming re-eval) ----
    //
    // Every UploadHeightmapRegion unions its patched sub-rect (in UV [0,1], from the
    // unified texture dims) into a per-terrain dirty AABB and bumps a monotonic version.
    // CBTUpdateSystem drains this each frame into Classify's dirty rect so the CBT
    // bisectors overlapping a streamed / re-baked tile are flagged MODIFIED and VertexEval
    // re-samples their now-current heights. Without it the quiescence gate freezes those
    // bisectors' cached corners at whatever the texture held when they were last
    // split / merged / edited — the permanent floating-shard / sky-hole corruption. Height
    // only: gVertex caches geometry, which VertexEval writes from the height texture;
    // normals / splat are sampled fresh per-pixel every frame (no per-bisector cache), so
    // their re-uploads reach the screen without a reclassify.

    // Drain (and clear) the accumulated height dirty rect for `handle`. Returns true and
    // fills the rect when one was accumulated since the last drain; returns false (leaving
    // the rect outputs untouched) when quiescent. `version` — the monotonic upload counter
    // (diagnostics / tests) — is ALWAYS written, even on a false return.
    bool TakeUnifiedHeightDirtyRect(TerrainHandle handle, float32& minU, float32& minV,
                                    float32& maxU, float32& maxV, uint64& version);
    // Non-destructive read of the same accumulated rect (tests / future consumers). Same
    // contract: rect written only on true; `version` always written.
    bool PeekUnifiedHeightDirtyRect(TerrainHandle handle, float32& minU, float32& minV,
                                    float32& maxU, float32& maxV, uint64& version) const;

    // ---- Deferred texture uploads ----

    bool HasPendingUploads() const;

    // Returns true exactly once per distinct rgFrameIndex — the render graph's
    // MONOTONIC per-frame index (shared across every window's graph in one engine
    // frame, so the first graph claims the flush and the rest are refused), NOT the
    // device's cyclic slot. Records which graph won the flush so a consumer in a
    // different graph (the multi-graph ordering hazard) can be detected.
    bool TryClaimHeightmapUploadFrameRG(uint64_t rgFrameIndex, const void* graph);

    // Cross-graph ordering tripwire: true iff the heightmap upload flush was claimed
    // THIS frame (by rgFrameIndex) by a render graph OTHER than `graph`. The edge-less
    // upload carries no cross-graph GPU sync, so a CBT.Update that samples the
    // heightmap from a different graph than the one flushing the copy could commit its
    // dirty rect against pre-copy texels (freezing one-shot streamed uploads). The
    // monotonic index self-invalidates a stale stamp (a later frame never matches), so
    // an upload-less frame cannot mis-fire. Today this never fires: the terrain-owning
    // RenderServices builds exactly one graph per frame.
    bool HeightmapUploadClaimedInOtherGraph(uint64_t rgFrameIndex, const void* graph) const;

    // Records the "TerrainUpload" pass the claim winner actually declared. Ignored unless
    // (rgFrameIndex, graph) still match the live claim, so a losing graph cannot overwrite it.
    void RecordHeightmapUploadPassRG(uint64_t rgFrameIndex, const void* graph,
                                     Rendering::RenderGraph::RGPassId pass);

    // The "TerrainUpload" flush pass declared in `graph` for `rgFrameIndex`, or kInvalidId when
    // that graph declared none this frame — nothing was pending, or the claim went to another
    // graph, and a pass id only means anything inside the graph that produced it.
    //
    // Every consumer that samples the uploaded textures pulls this and adds an ordering edge
    // from it to its own pass. THAT EDGE IS THE ORDERING GUARANTEE. The upload pass shares no
    // declared resource access with its consumers (the textures are bindless, never
    // RenderGraph-imported), so without an edge it is its own one-pass component: the scheduler
    // ranks whole components by the minimum (phase, pass id) within them and emits a winning
    // component entirely before reconsidering, so a consumer component holding any earlier
    // kEarlySetup pass drains — consumers included — ahead of the upload, whatever its phase.
    Rendering::RenderGraph::RGPassId GetHeightmapUploadPassRG(uint64_t rgFrameIndex,
                                                             const void* graph) const;

    // Record barrier + copy commands for all pending uploads into the command list.
    void FlushPendingUploads(Rendering::CommandList* cl);

    // ---- Per-terrain GPU params SSBO + material table (shared across views) ----

    // Writes this frame's ring element and publishes it as GetLastTerrainParamsSlot, which is
    // how every reader below names the element to read — readers never derive it themselves.
    //
    // The params array and the material table are published TOGETHER, under one element, by this
    // one call: a params entry names its materials by absolute table index, so a reader that took
    // the slot must see both from the same frame. Two upload calls would let a table from frame N
    // be indexed by params from frame N-1 for the width of one draw, which resolves to another
    // material rather than to a detectable error.
    //
    // `deviceFrameIndex` is a CHANGE TOKEN, not the element: IDevice::GetFrameIndex() reports a
    // slot in [0, framesInFlight), a domain too narrow to name the last of kMaxFrames elements,
    // so reducing its value directly would cap the rotation at the device's pacing and leave
    // that element allocated but never written. The rotation is one element per device frame
    // instead (m_ParamsFrameCounter), which is what makes the reuse distance kMaxFrames: the
    // write to element k lands at frame F and k was last read at F-kMaxFrames. This call runs
    // in the extraction tick — the application UPDATE phase — where the newest frame the device
    // has PROVEN complete is only F-framesInFlight-1, not F-framesInFlight
    // (FrameBufferAllocator::BeginFrame states the rule). kMaxFrames must therefore EXCEED the
    // device's pacing; it is sized from kMaxSupportedFramesInFlight so the relation holds on a
    // backend that paces deeper than today's three, rather than by matching the current pacing.
    // The buffer carries no BufferCreateFlags::FrameSlotted: that flag diagnoses any write
    // outside an acquired frame, which is the right question only for a ring sized AT the
    // pacing, and would fire on every legitimate write to this one.
    void UploadTerrainParamsArray(const Terrain::TerrainGPUParams* params, uint32 count,
                                  const Terrain::TerrainMaterialRecord* materials,
                                  uint32 materialCount, uint32 deviceFrameIndex);
    // `paramsSlot` is a published element index (GetLastTerrainParamsSlot); the modulo is a
    // bounds guard, not a rotation.
    Rendering::BufferHandle GetTerrainParamsSSBO(uint32 paramsSlot) const { return m_TerrainParamsSSBO[paramsSlot % kMaxFrames]; }
    // Wind volumes the grass shaders sample, addressed by the same published slot as the
    // params. The extraction tick uploads them right after UploadTerrainParamsArray.
    GrassWindVolumeRing& GrassWindVolumes() { return m_GrassWindVolumes; }
    const GrassWindVolumeRing& GrassWindVolumes() const { return m_GrassWindVolumes; }
    // The material table every terrain surface indexes through TerrainGPUParams::LayerRole.
    // Valid from Initialize onward (a zero-filled minimum table is created there), because an
    // unbound set-2 SSBO is a null descriptor and a device fault rather than a missing texture.
    Rendering::BufferHandle GetTerrainMaterialTableSSBO(uint32 paramsSlot) const { return m_TerrainMaterialTableSSBO[paramsSlot % kMaxFrames]; }
    uint32 GetTerrainMaterialCount(uint32 paramsSlot) const { return m_TerrainMaterialCount[paramsSlot % kMaxFrames]; }
    // The parsed material libraries extraction authors that table from. Owned by the table's
    // owner; main-thread only, like the extraction pass that reads it.
    TerrainMaterialLibraryCache& MaterialLibraries() { return m_MaterialLibraries; }
    uint32 GetTerrainParamsCount(uint32 paramsSlot) const { return m_TerrainParamsCount[paramsSlot % kMaxFrames]; }
    uint32 GetTerrainGrassActiveCount(uint32 paramsSlot) const { return m_TerrainGrassActiveCount[paramsSlot % kMaxFrames]; }
    uint32 GetTerrainGrassMaxBladeSegments(uint32 paramsSlot) const { return m_TerrainGrassMaxBladeSegments[paramsSlot % kMaxFrames]; }
    // Aggregates over the grass-enabled rows of a params slot, reduced at upload time because the
    // params themselves live only on the GPU. Grass placement is camera-relative, so its CPU side
    // needs the WORST case across terrains to size one dispatch window and one instance budget:
    // the densest request, the longest range, the flattest falloff (which spends the most of the
    // budget far out), and the vertical extent every cell's frustum AABB must cover.
    struct GrassPlacementSummary
    {
        float32 MaxNearDensity = 0.0f; // blades per square metre at the camera
        float32 MaxRange = 0.0f;
        float32 MinFalloff = 0.0f;
        float32 MaxBladeHeight = 0.0f;
        float32 WorldMinY = 0.0f;
        float32 WorldMaxY = 0.0f;
        // Both flags are reduced once per params UPLOAD, over every ACTIVE grass row in the array.
        // That array is not frustum-filtered and every view reads the same published slot, so
        // these are world-wide facts, not per-view ones — a terrain no view can see still sets them.
        //
        // True when EVERY active grass row authored TerrainGrassRenderMode::Blend. Blades from
        // every terrain share one draw per LOD band, so the drawn mode is never per terrain:
        // mixed authoring resolves to Dither, the depth-correct choice.
        bool AllBlendMode = false;
        // True when ANY active grass row binds a blade texture whose alpha is not uniformly
        // opaque. Reduced the opposite way to AllBlendMode and for the same reason one draw
        // serves every terrain: the alpha path has to survive if a single row still needs it.
        // All-clear is what lets the draw take the opaque fast path (see ResolveGrassDrawMode).
        bool AnyAlphaNeeded = false;
    };
    GrassPlacementSummary GetTerrainGrassPlacementSummary(uint32 paramsSlot) const
    {
        return m_TerrainGrassPlacement[paramsSlot % kMaxFrames];
    }
    uint32 GetLastTerrainParamsSlot() const { return m_LastTerrainParamsSlot.load(std::memory_order_acquire); }

    // ---- Active terrain instances (extracted from ECS) ----

    void SetActiveTerrains(std::vector<TerrainInstanceInfo> terrains);
    std::vector<TerrainInstanceInfo> GetActiveTerrains() const;  // returns by value under lock
    // The height textures of the first active terrain (the one the CBT renderer draws), copied
    // under the lock without the rest of its info. Present is false when no terrain is active.
    struct ActiveHeightSource
    {
        bool Present = false;
        Rendering::TextureHandle HeightmapTexture;
        bool AtlasBacked = false;
        Rendering::TextureHandle AtlasHeightTexture;
    };
    ActiveHeightSource GetActiveHeightSource() const;

    // ---- Frame lifecycle ----

    // Retires deferred GPU-resource destroys at most once per frame (thread-safe), and stamps
    // the "the TerrainUpload pass is live" signal the bindless gate reads (see
    // m_UploadPassSeenExtractionFrame). Reaching this function IS that signal, because
    // TerrainUploadNode::DeclareForView is its only caller.
    // `rs` frees the global bindless slot of each retired terrain texture (the
    // heightmap/splat/normal + atlas sources are all bindless-registered) before the
    // underlying image is destroyed — without it the slot leaks and its descriptor
    // dangles at freed VRAM until reissued, the class of fault a resolution change's
    // texture teardown otherwise reopened.
    void TryCleanupStaleViews(uint32 frameIndex, Engine::Renderer::RenderServices& rs);

private:
    // Depth of the params ring: deepest pacing any backend reports, plus the
    // update-phase slot, because UploadTerrainParamsArray runs in the extraction
    // tick and so must write an element no frame in flight can still be reading.
    static constexpr uint32 kMaxFrames =
        Rendering::IDevice::kMaxSupportedFramesInFlight + 1u;
    static_assert(GrassWindVolumeRing::kSlots == kMaxFrames,
                  "wind volumes are addressed by the params slot");

    void CreateSamplers(Rendering::IDevice* device);

    Rendering::IDevice* m_Device = nullptr;
    bool m_Initialized = false;

    // One GPU terrain texture, indexed by terrain slot index. Shared shape for
    // the heightmap (R32F), splatmap (RGBA8), and normal map (R16G16F).
    struct TextureEntry
    {
        Rendering::TextureHandle Texture;
        uint32 Width = 0;
        uint32 Height = 0;
        uint32 BindlessIndex = 0;
        // GPU-allocated byte size of Texture (0 for entries not tracked by the unified/atlas
        // VRAM accounting). Carried into the deferred-destroy record so the running live total
        // is decremented only when the image is actually freed after the quarantine.
        size_t Bytes = 0;
        // Storage-image view for compute imageStore (atlas height GPU bake only; empty
        // for sampled-only entries). Created alongside the texture when UAV usage is set.
        Rendering::TextureViewHandle StorageView;
    };
    CompatLayerTextures m_CompatLayerTextures{};
    std::vector<TextureEntry> m_Heightmaps;
    Rendering::SamplerHandle m_HeightmapSampler;
    Rendering::SamplerHandle m_LayerSampler{};
    std::vector<TextureEntry> m_Splatmaps;
    std::vector<TextureEntry> m_Normalmaps;
    std::array<std::vector<TextureEntry>, 3> m_GrassFields;
    void ReleaseGrassFieldResourcesLocked(TerrainHandle handle);
    // Phase E: one square R32F atlas per terrain (bound directly by CBT compute, not bindless).
    std::vector<TextureEntry> m_AtlasHeights;
    // The per-terrain coarse height field (out-of-window fallback), one small square R32F each.
    std::vector<TextureEntry> m_AtlasCoarse;
    // Phase E atlas splat/normal surface sources (RGBA8 / R16G16F, AtlasDim^2) + their coarse
    // fields (small squares). Registered bindless — the surface samples them per-pixel.
    std::vector<TextureEntry> m_AtlasSplats;
    std::vector<TextureEntry> m_AtlasNormals;
    std::vector<TextureEntry> m_AtlasSplatCoarse;
    std::vector<TextureEntry> m_AtlasNormalCoarse;

    // Per-terrain accumulated unified-height dirty rect (see TakeUnifiedHeightDirtyRect),
    // indexed by handle slot. Max <= Min == empty; Version is monotonic across uploads.
    struct UnifiedDirtyRect
    {
        float32 MinU = 1.0f;
        float32 MinV = 1.0f;
        float32 MaxU = 0.0f;
        float32 MaxV = 0.0f;
        bool HasRect = false;
        uint64 Version = 0;
    };
    std::vector<UnifiedDirtyRect> m_UnifiedHeightDirty;

    // Union a patched texel sub-rect into the terrain's height dirty AABB (UV space) and
    // bump its version. Caller holds m_Mutex. The rect is padded one texel each side to
    // cover CBT's linear-clamp sample footprint (which reaches half a texel past the raw
    // texel rect), so every bisector that samples a re-uploaded texel is flagged.
    void AccumulateUnifiedHeightDirty(TerrainHandle handle, uint32 dstX, uint32 dstY,
                                      uint32 srcW, uint32 srcH, uint32 texW, uint32 texH);

    GrassWindVolumeRing m_GrassWindVolumes;
    // Per-frame terrain params SSBO (array of TerrainGPUParams, one per terrain).
    Rendering::BufferHandle m_TerrainParamsSSBO[kMaxFrames] = {};
    uint32 m_TerrainParamsCapacity[kMaxFrames] = {};
    uint32 m_TerrainParamsCount[kMaxFrames] = {};
    // Per-frame material table (array of TerrainMaterialRecord), rotating on the SAME element as
    // the params array above so the two are always read as a pair.
    Rendering::BufferHandle m_TerrainMaterialTableSSBO[kMaxFrames] = {};
    uint32 m_TerrainMaterialCapacity[kMaxFrames] = {};
    uint32 m_TerrainMaterialCount[kMaxFrames] = {};
    // Grows element `fi`'s material table to `capacity` records, zero-filling a freshly created
    // buffer so an unwritten entry shades black rather than sampling whatever the memory held.
    // Caller holds m_Mutex.
    void EnsureMaterialTableBuffer(uint32 fi, uint32 capacity);
    // The authored side of that table: parsed libraries, dropped on reload. Not guarded by
    // m_Mutex — extraction is its only reader and shares a thread with the reload dispatch.
    TerrainMaterialLibraryCache m_MaterialLibraries;
    uint32 m_TerrainGrassActiveCount[kMaxFrames] = {};
    uint32 m_TerrainGrassMaxBladeSegments[kMaxFrames] = {};
    GrassPlacementSummary m_TerrainGrassPlacement[kMaxFrames] = {};
    std::atomic<uint32> m_LastTerrainParamsSlot{0};
    // Unwraps the device's wrapped frame slot so all kMaxFrames params elements rotate.
    // Ticked only by UploadTerrainParamsArray — the ring's single writer, once per extraction
    // — so the rotation is exactly one element per frame.
    Rendering::DeviceFrameCounter m_ParamsFrameCounter;

    // Terrain instances extracted from ECS (written by extraction, read by the CBT renderer).
    std::vector<TerrainInstanceInfo> m_ActiveTerrains;

    std::atomic<uint32> m_LastCleanupFrame{UINT32_MAX};
    // Bumped by ReleaseTerrainResources (see GetTerrainTextureRetireGeneration): the re-provision
    // signal the CBT renderer polls to purge the retired heightmap from its own descriptor ring.
    std::atomic<uint64> m_TerrainTextureRetireGeneration{0};
    // Bumped wherever a terrain texture write is QUEUED, and by ReleaseTerrainResources (see
    // GetGrassPlacementContentEpoch). Two queues reach those textures: staged copies drain through
    // m_PendingUploads, which only QueueBandUpload and QueueRegionUpload fill, and atlas bakes
    // drain through m_PendingBakes, which only QueueGpuHeightBake fills. Those three, plus
    // EnsureUnifiedEntry's clear and the retirement, cover the whole surface rather than each
    // caller having to remember.
    std::atomic<uint64> m_GrassContentEpoch{0};
    // See GetDeviceResourceEpoch.
    std::atomic<uint64> m_DeviceResourceEpoch{0};
    // The params array as last published, kept so a reader can compare CONTENT instead of trusting
    // the ring slot, which rotates every frame whether or not anything changed. Guarded by m_Mutex.
    std::vector<uint8> m_LastTerrainParamsBytes;
    // Warn-once when more than one atlas-backed terrain is active (grass binds only the first).
    mutable std::atomic<bool> m_MultiAtlasGrassWarned{false};
    mutable std::mutex m_Mutex;

    // Monotonic frame counter for deferred destruction (device GetFrameIndex cycles).
    uint32 m_MonotonicFrame = 0;

    struct DeferredBufferDestroy
    {
        Rendering::BufferHandle Buffer;
        uint32 FrameRetired = 0;
    };
    std::vector<DeferredBufferDestroy> m_DeferredDestroys;
    void DeferBufferDestroy(Rendering::BufferHandle buf);

    struct DeferredTextureDestroy
    {
        Rendering::TextureHandle Texture;
        uint32 FrameRetired = 0;
        // Unified/atlas VRAM bytes to reclaim from the live total when this image is actually
        // freed (0 for textures outside the accounting, e.g. single-tile heightmaps).
        size_t Bytes = 0;
    };
    std::vector<DeferredTextureDestroy> m_DeferredTextureDestroys;
    void DeferTextureDestroy(Rendering::TextureHandle tex, size_t bytes = 0);

    // Running live + high-water byte totals for the unified/atlas texture sets (see the
    // GetUnifiedTexture*Bytes accessors). Written under m_Mutex on allocate and once-per-frame
    // on the deferred-destroy flush — the same non-overlapping extract/render phasing the
    // monotonic-frame counter already relies on. Not a per-frame hot path (re-provision only).
    size_t m_UnifiedTextureBytesLive = 0;
    size_t m_UnifiedTextureBytesPeak = 0;

    // Feature-wide re-provision VRAM staging barrier: monotonic frame past which the most
    // recently retired unified set has cleared its frames-in-flight quarantine (and been freed).
    // Set to (monotonic frame + kMaxFrames) whenever ReleaseTerrainResources retires a set;
    // EnsureUnifiedTiledTextures withholds the new world-sized allocation until the frame passes
    // it, so the old set is freed before the new one is allocated — capping the transient peak at
    // max(old, new) instead of old + new. Feature-wide (not per-slot) because a re-provision
    // recycles the tiled terrain but assigns the new global GPU handle a FRESH index, so there is
    // no stable per-slot key spanning the old and new sets. Zero = no re-provision pending. The
    // bindless-slot-0 sentinel covers the brief no-texture window (flat terrain, the same
    // transient the pre-existing clear-to-zero already shows on any re-provision).
    uint32 m_UnifiedReprovisionDrainFrame = 0;

    struct DeferredViewDestroy
    {
        Rendering::TextureViewHandle View;
        uint32 FrameRetired = 0;
    };
    std::vector<DeferredViewDestroy> m_DeferredViewDestroys;
    void DeferTextureViewDestroy(Rendering::TextureViewHandle view);

    void FlushDeferredDestroys(Engine::Renderer::RenderServices& rs);

    // Pending texture uploads queued by the Upload*/Upload*Region paths, flushed
    // into a render graph command list by FlushPendingUploads. A copy places a
    // tightly-packed CopyWidth*CopyHeight staging block at destination texel
    // origin (DstX,DstY). FullTexture drives the pre-copy layout: Undefined
    // (discard) for whole-texture rewrites, ShaderResource (preserve) for bands
    // and tile sub-rects.
    struct PendingTextureUpload
    {
        Rendering::BufferHandle StagingBuffer;
        Rendering::TextureHandle Texture;
        uint32 CopyWidth = 0;
        uint32 CopyHeight = 0;
        size_t RowPitchBytes = 0;
        uint32 DstX = 0;
        uint32 DstY = 0;
        bool FullTexture = true;
    };
    std::vector<PendingTextureUpload> m_PendingUploads;

    // Unified textures (re)created this frame that need a one-time GPU zero-clear in
    // FlushPendingUploads before their region patches — no CPU/staging buffer needed.
    struct PendingTextureClear { Rendering::TextureHandle Texture; float32 Value = 0.0f; };
    std::vector<PendingTextureClear> m_PendingClears;

    // ---- GPU-initialization gate ----
    //
    // A freshly created image holds no contents and sits in VK_IMAGE_LAYOUT_UNDEFINED. It
    // becomes samplable only when FlushPendingUploads records its zero-clear or its first
    // whole-texture copy, and that call has exactly one caller: the render graph's
    // "TerrainUpload" pass (TerrainUploadNode). A pipeline that binds terrain params without
    // declaring that pass would otherwise point shaders at an image nothing ever wrote —
    // undefined behaviour that fails silently and differently per driver (MoltenVK reads zero,
    // which reads downstream as flat terrain and zero grass blades rather than as an error).
    //
    // The gate narrows that hole to the BINDLESS taps: a texture is held here from creation
    // until its initialization is recorded, and RegisterBindless publishes its slot only once
    // that initialization is recorded or certain to be recorded this frame ahead of every
    // consumer (see UploadPassIsLive). Keyed by raw handle id (generation included), erased on
    // creation-time replacement and on deferred destroy.
    //
    // It does NOT cover the CBT height sample: CBTRenderFeature::BuildFrameParams binds the
    // heightmap into the CBT descriptor ring directly (CBTInstance::SetHeightSource), not
    // through a bindless index, so a pipeline with no "TerrainUpload" pass still samples an
    // image nothing has written there. WarnIfUploadPassMissing is what reports that case.
    std::unordered_set<Rendering::Detail::HandleType> m_TexturesAwaitingGpuInit;
    void MarkAwaitingGpuInit(Rendering::TextureHandle tex);
    bool AwaitsGpuInit(Rendering::TextureHandle tex) const
    {
        return m_TexturesAwaitingGpuInit.count(tex.id) != 0;
    }
    // True while `tex`'s zero-clear or whole-texture copy is queued and undrained — the
    // initializer a flush would record. Asked per texture rather than inferred from
    // HasPendingUploads() so a texture whose staging allocation failed (QueueBandUpload bails
    // without queuing) keeps its slot withheld instead of being published against an image
    // nothing is going to write. Pinned by TerrainBindlessSlotWaitsForTheUploadFlush's
    // staging-failure arm, which uses FailNextStagingAllocationForTests.
    bool HasQueuedGpuInit(Rendering::TextureHandle tex) const;

    // Set only by FailNextStagingAllocationForTests; consumed by the next QueueBandUpload.
    bool m_FailNextStagingAllocation = false;

    // Shared body of the three public Register*Bindless entry points.
    void RegisterBindless(std::vector<TextureEntry>& entries, TerrainHandle handle,
                          Engine::Renderer::RenderServices& rs);

    // ---- "the TerrainUpload pass is live" signal ----
    //
    // Frame order inside one engine frame: the ECS extraction wave runs first (Application's
    // loop calls EngineCore::Update before Render). TerrainExtractionSystem::Update creates the
    // textures, queues their initializers, calls Register*Bindless, and closes with
    // UploadTerrainParamsArray. The render graph is declared after the wave joins, and
    // TerrainUploadNode::DeclareForView — the only caller of TryCleanupStaleViews — adds the
    // "TerrainHeightmapUpload" pass whenever HasPendingUploads().
    //
    // That the flush then EXECUTES before its consumers is the ordering edge's doing, not the
    // phase's: the upload pass shares no declared access with them, so it is a scheduling
    // component of one, and components are emitted whole in rank order. The CBT update and the
    // grass placement each pull the pass (GetHeightmapUploadPassRG) and add an edge from it to
    // their own; the CBT surface and the world draws inherit the order through the buffers those
    // two produce. A consumer that samples these textures without adding that edge can and does
    // schedule ahead of the flush.
    //
    // So a slot may be published for a still-uninitialized texture exactly when the pass
    // declared since the previous extraction tick: it will declare again this frame and flush
    // before anything samples the image. m_ExtractionFrame is the READER's own clock and still
    // holds the previous tick's value while Register*Bindless runs, which is what makes equality
    // mean "declared since then". Deliberately NOT m_MonotonicFrame: that advances only inside
    // the declare that would stamp it, so a pipeline which LOSES the pass mid-session (a
    // rendergraph edit) freezes both and the stamp would keep matching forever, reopening the
    // hole this gate exists to close.
    //
    // Accepted residual, one frame wide: a blueprint change is applied in the declare step
    // (FrameOrchestrator::EnsureActiveRenderPipelineBlueprint, reached only from
    // BuildFrameGraph), which is AFTER that frame's extraction. A texture created in the very
    // frame the pass disappears therefore publishes under a stamp that still matches, and
    // RegisterBindless will not revisit it because a non-zero BindlessIndex returns early — so
    // that one texture's slot stays published until a pass returns. It heals rather than rots:
    // its initializer is still queued (only FlushPendingUploads drains the queues), so the flush
    // defines the image the moment a pass declares again. Every LATER creation re-arms, because
    // the extraction clock has moved past the frozen stamp by then.
    static constexpr uint32 kUploadPassNeverDeclared = UINT32_MAX;
    // Extraction frame TerrainUploadNode last declared on. Written by TryCleanupStaleViews off
    // m_Mutex (the deferred-destroy path deliberately runs unlocked) and read by the registrars
    // under it, hence atomic; relaxed because the ECS wave join already orders extraction against
    // the declare. A clock value can only collide with the sentinel after 2^32 pass-less
    // extraction ticks, and then for that one tick — the next value re-arms the gate.
    std::atomic<uint32> m_UploadPassSeenExtractionFrame{kUploadPassNeverDeclared};
    // Device-frame-idempotent extraction clock: the counter m_ParamsFrameCounter produces, so a
    // second world's UploadTerrainParamsArray inside one device frame does not advance it — and
    // it freezes exactly when the device stops advancing frames, which is the state in which
    // nothing declares (a minimized window skips the render half; extraction keeps ticking).
    std::atomic<uint32> m_ExtractionFrame{0};
    bool UploadPassIsLive() const
    {
        return m_UploadPassSeenExtractionFrame.load(std::memory_order_relaxed) ==
               m_ExtractionFrame.load(std::memory_order_relaxed);
    }

    // FlushPendingUploads calls that reached a command list. Zero forever means no pass is
    // draining the queue, which is the one condition the missing-pass diagnostic reports.
    uint64 m_FlushCount = 0;

    // Consecutive extraction frames (UploadTerrainParamsArray ticks) that found textures still
    // awaiting initialization with m_FlushCount == 0. A pipeline that declares TerrainUpload
    // flushes on its first declared frame and disarms this permanently, so the diagnostic
    // cannot false-fire on startup ordering or on a mid-session texture (re)creation.
    //
    // Keyed on m_FlushCount, NOT on m_UploadPassSeenExtractionFrame: extraction ticks with no
    // render behind it whenever the render half is skipped (a minimized editor window's 0x0
    // framebuffer, a swapchain acquire failure, a Player scene with no enabled camera), at
    // uncapped loop rate, so a stamp-keyed limit would report a healthy pipeline.
    uint32 m_FramesAwaitingUploadPass = 0;
    bool m_MissingUploadPassReported = false;
    // Reports the missing "TerrainUpload" pass once. Call with m_Mutex held, once per frame.
    void WarnIfUploadPassMissing();

    // Full-width horizontal band (or whole-texture) upload of a full-size source.
    void QueueBandUpload(Rendering::TextureHandle texture,
                         const uint8* srcBytes, uint32 width, uint32 height,
                         size_t rowPitchBytes, const UploadBand& band,
                         bool forceFullTexture, const char* stagingName);

    // Content-preserving 2D sub-rect upload of a standalone srcW*srcH block into
    // an existing texture at (dstX,dstY). Used for unified tiled-terrain patches.
    void QueueRegionUpload(Rendering::TextureHandle texture,
                           const uint8* srcBytes, uint32 srcW, uint32 srcH,
                           size_t bytesPerTexel, uint32 dstX, uint32 dstY,
                           const char* stagingName);

    // Shared body of EnsureUnifiedTiledTextures for one texture entry. When needsStorage,
    // the texture also carries UnorderedAccess usage + a storage view (for the compute
    // height bake's imageStore); sampled-only entries pass false. Returns true when it
    // allocated a NEW texture this call (create/resize), false when it reused the existing
    // one or the allocation failed.
    bool EnsureUnifiedEntry(std::vector<TextureEntry>& entries, TerrainHandle handle,
                            uint32 width, uint32 height, uint32 textureFormat,
                            size_t bytesPerTexel, const char* debugName,
                            bool needsStorage = false, float32 clearValue = 0.0f);

    // Once-per-frame stamp for the heightmap-upload pass. UINT64_MAX = none claimed yet.
    uint64_t m_RGHeightmapUploadFrame = UINT64_MAX;
    // The render graph (RGFrame) that won the current frame's upload-flush claim, so a
    // CBT.Update declared in a different graph can be caught (see the header methods).
    const void* m_RGHeightmapUploadGraph = nullptr;
    // The pass that graph declared for the claim, for consumers to order themselves after.
    // Reset by every claim, so last frame's id can never be handed out for this frame.
    Rendering::RenderGraph::RGPassId m_RGHeightmapUploadPass = Rendering::RenderGraph::kInvalidId;

    // ---- GPU height bake (slice-1b, GE_TERRAIN_GPU_BAKE) ----
public:
    // Storage-image view of the atlas height texture (empty if not created / not GPU-bake).
    Rendering::TextureViewHandle GetAtlasHeightStorageView(TerrainHandle handle) const;
    Rendering::TextureViewHandle GetAtlasNormalStorageView(TerrainHandle handle) const;
    Rendering::TextureViewHandle GetAtlasSplatStorageView(TerrainHandle handle) const;

    // Queue one bake's GPU work: the shared priority-sorted modifier rows + one dispatch per
    // baked tile (slot + rect + params + tile index). Recorded by the extraction system;
    // flushed by TerrainUploadNode before CBT VertexEval. When `settle`, each dispatched tile's
    // slot rect is copied GPU->CPU after the bake so the CPU heightfield can be refreshed. The
    // splat pass runs after the normal pass when `splatEligible`, normalizing altitude against the
    // committed range (`splatMinH`/`splatMaxH`) and compositing `surfaceRules` (whose conditions
    // are addressed into `surfaceRuleConditions`) on top; it is skipped for a paint-carrying bake
    // so the CPU-composited splat stands until settle.
    void QueueGpuHeightBake(TerrainHandle handle, std::vector<ModifierGpu> modifiers,
                            std::vector<GpuHeightBakeDispatch> dispatches, bool settle,
                            float splatMinH, float splatMaxH, bool splatEligible,
                            std::vector<SurfaceRuleGpu> surfaceRules,
                            std::vector<SurfaceRuleConditionGpu> surfaceRuleConditions);
    bool HasPendingBakes() const;
    // Record the bake dispatches (+ any settle readback copies) into the command list. Recorded
    // right after FlushPendingUploads in the same pass so the atlas clear/normal/splat upload
    // precede the height imageStore on the queue.
    void FlushPendingBakes(Rendering::CommandList* cl);

    // Drain settle readbacks for `handle` whose GPU copy has completed (>= kMaxFrames monotonic
    // ticks old). For each, invokes cb(tileX, tileZ, samples, rectW, rectH, rectMinX, rectMinZ)
    // with the read-back interior height rect so the caller can refresh the CPU heightfield.
    using ReadbackApply = std::function<void(int32 tileX, int32 tileZ, const float32* samples,
                                             uint32 rectW, uint32 rectH,
                                             int32 rectMinX, int32 rectMinZ)>;
    void DrainReadyHeightReadbacks(TerrainHandle handle, const ReadbackApply& cb);

    // Test seams for the re-provision drop-on-retire oracle. A settle readback is normally
    // created only inside FlushPendingBakes (needs a device); the seam injects one with an
    // invalid buffer handle so ReleaseTerrainResources' drain is observable without a GPU.
    void SeedPendingReadbackForTests(TerrainHandle handle);
    uint32 GetPendingReadbackCountForTests() const;

    // Live / peak byte accounting for the world-sized unified + resident-window atlas texture
    // sets (the sets a SamplesPerMeter/size re-provision reallocates). GPU-allocated bytes are
    // counted from CreateTexture until the deferred-destroy quarantine actually frees them, so
    // the peak captures the transient window where the old (quarantined) and new sets coexist.
    size_t GetUnifiedTextureLiveBytes() const;
    size_t GetUnifiedTexturePeakBytes() const;

    // Frame past which the most recently retired unified set has drained (re-provision VRAM
    // staging barrier). Zero before any re-provision. Test seam for the gate-arming oracle
    // (arming is device-free; maturation needs per-frame render ticks).
    uint32 GetReprovisionDrainFrameForTests() const;

    // Test seam (no production caller): sends the next QueueBandUpload down CreateUploadBuffer's
    // failure return. That path leaves a freshly created texture marked as awaiting GPU
    // initialization with no initializer queued FOR IT, which is the one case the bindless gate
    // must keep withholding even while the upload pass is live — otherwise the slot would be
    // published against an image no flush is ever going to write. One-shot.
    void FailNextStagingAllocationForTests();

private:
    bool EnsureGpuBakePipeline();

    Rendering::PipelineHandle m_GpuBakePipeline{};
    bool m_GpuBakePipelineAttempted = false;
    // Slice-2 normal-derive pipeline (KERNEL_NORMAL variant): reads the height atlas storage
    // image, writes the RG16F normal atlas. Attempted lazily alongside the height pipeline.
    Rendering::PipelineHandle m_GpuNormalPipeline{};
    // Slice-3 splat-derive pipeline (KERNEL_SPLAT variant): reads the height atlas storage image,
    // writes the RGBA8 splat atlas. Attempted lazily alongside the height/normal pipelines.
    Rendering::PipelineHandle m_GpuSplatPipeline{};

    struct PendingBake
    {
        TerrainHandle Handle;
        std::vector<ModifierGpu> Modifiers;
        std::vector<GpuHeightBakeDispatch> Dispatches;
        bool Settle = false;
        float SplatMinH = 0.0f;
        float SplatMaxH = 0.0f;
        bool SplatEligible = false;
        std::vector<SurfaceRuleGpu> SurfaceRules;
        std::vector<SurfaceRuleConditionGpu> SurfaceRuleConditions;
    };
    std::vector<PendingBake> m_PendingBakes;

    struct PendingReadback
    {
        Rendering::BufferHandle Buffer;
        TerrainHandle Handle;
        int32 TileX = 0;
        int32 TileZ = 0;
        uint32 RectW = 0;
        uint32 RectH = 0;
        int32 RectMinX = 0;
        int32 RectMinZ = 0;
        uint32 ReadyFrame = 0; // monotonic frame at/after which the copy is guaranteed complete
    };
    std::vector<PendingReadback> m_PendingReadbacks;
};

} // namespace GameEngine::TerrainECS
