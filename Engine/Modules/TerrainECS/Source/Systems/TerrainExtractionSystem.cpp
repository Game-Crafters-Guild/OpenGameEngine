#include "TerrainECS/Systems/TerrainExtractionSystem.h"
#include "TerrainECS/TerrainHandleHygiene.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainGrassFieldUploader.h"
#include "TerrainECS/TerrainGrassAlpha.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TileStreamingManager.h"
#include "TerrainECS/AtlasResidencyController.h"
#include "TerrainECS/TerrainSizingPlan.h"
#include "TerrainECS/PlanarHeightQuery.h"
#include "TerrainECS/TerrainHeightPageDriver.h"
#include "TerrainECS/TerrainHeightPageFeature.h"
#include "CBTTerrain/CBTLayout.h" // kAtlasMaxTiles (F2 cap warn)
#include "AssetCore/GUID.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "TerrainECS/TerrainMaterialAuthoring.h"
#include "TerrainECS/TerrainMaterialLibraryCache.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Transform.h"
#include "Components/Name.h"
#include "ECS/Components.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/SunGlareRenderFeature.h"
#include "Engine/Rendering/RenderOrigin.h" // kSectorSize (world-anchoring UV phase)
#include "Core/Engine.h"
#include "ECS/ECS.h"
#include "ECS/WorldTemplateImplementations.inl"
#include "ECS/ECSTemplates.h"
#include "Rendering/Core/Device.h"
#include "Terrain/CDLODSelection.h"
#include "Logger/Logger.h"
#include "Rendering/CameraTypes.h"
#include "Core/CpuProfiler.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

using GameEngine::TerrainECS::ExtractCameraPosition;

// Explicit cross-DLL instantiation for the Terrain component handlers.
// See ECS/ECSTemplates.h for the rationale (GE_INSTANTIATE_ENGINE_COMPONENT).
// TerrainPlanetRelief is the planet's base-noise companion, split out of Terrain.
namespace GameEngine::ECS
{
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::Terrain);
    GE_INSTANTIATE_ENGINE_COMPONENT(Components::TerrainPlanetRelief);
} // namespace GameEngine::ECS

namespace GameEngine::TerrainECS
{
namespace
{

uint32 ResolveTerrainTextureBindless(Engine::Renderer::RenderServices* renderServices,
                                          const GUID& guid)
{
    if (!renderServices)
        return 0u;
    if (guid.IsNull())
        return 0u;
    const auto texture = renderServices->Textures().GetOrUpload(guid);
    if (!texture.IsValid())
        return 0u;
    return renderServices->Textures().GetBindlessIndex(texture);
}

// Carries each terrain texture kind's classification to the texture system, once per library parse
// so a map is classified before anything uploads it. ClassifyTerrainTexture holds the mapping and
// the reasoning behind it.
TerrainTextureDeclarer MakeTerrainTextureDeclarer(Engine::Renderer::RenderServices* renderServices)
{
    return [renderServices](const GUID& guid, TerrainTextureKind kind)
    {
        if (!renderServices)
            return;
        const TerrainTextureClassification classification = ClassifyTerrainTexture(kind);
        renderServices->Textures().DeclareTextureClassification(
            guid, classification.ColorSpace, classification.Usage);
    };
}

// The rate cbt_surface.glsl falls back to when a terrain reports no global tiling, mirrored here
// so the CPU's UV phase is computed against the same divisor the fragment tiles with.
constexpr double kFallbackMaterialTiling = 10.0;

// Author a terrain's four channel-role materials into this frame's table and point its params
// at them. Each terrain APPENDS kTerrainLayerRoleCount consecutive records and stores their
// ABSOLUTE table indices, so a terrain that is later dropped from the params array strands its own
// records instead of shifting everyone else's — the index is never re-derived from a terrain's
// position in either array.
//
// `library` is the terrain's parsed material library, or nullptr for a terrain that has none (or
// whose library has not arrived yet). Role r takes the entry holding the slot ID the terrain binds
// to that role. A role whose slot no entry holds falls back to the built-in material for that role.
//
// This is the single resolve of a terrain's authored materials. Both terrain domains and the grass
// read the record it produces, so a texture cannot be bound for one consumer and missing for
// another, which is exactly what two independent resolves used to allow.
void AppendTerrainMaterials(Terrain::TerrainGPUParams& params,
                            std::vector<Terrain::TerrainMaterialRecord>& table,
                            const Components::Terrain& terrain,
                            const std::vector<TerrainMaterialEntry>* library,
                            Engine::Renderer::RenderServices* renderServices,
                            const int32 (&renderOriginSector)[4])
{
    const TerrainTextureResolver resolveTexture = [renderServices](const GUID& guid) -> uint32
    { return ResolveTerrainTextureBindless(renderServices, guid); };

    // The compat profile has no binding arrays and cannot reach a texture through an index, so
    // the first two role slots' textures are captured as handles on the way past. Two because
    // cbt_surface's compat arm binds two layers and wraps the rest onto the last; role slot ->
    // material is fixed per terrain (params.LayerRole below), so a fixed binding is the right
    // shape. The profile that indexes a bindless array needs none of this and does not pay for it.
    const bool compat = renderServices != nullptr && renderServices->GetProfile().IsCompat();
    const TerrainTextureAccess access =
        compat ? TerrainTextureAccess::NamedBinding : TerrainTextureAccess::BindlessIndex;
    const auto resolveHandle = [renderServices](const GUID& guid) -> Rendering::TextureHandle {
        if (!renderServices || guid.IsNull())
            return {};
        return renderServices->Textures().GetOrUpload(guid);
    };
    TerrainRenderFeature::CompatLayerTextures compatLayers{};

    const double materialTiling = params.MaterialTiling > 0.0f
                                      ? static_cast<double>(params.MaterialTiling)
                                      : kFallbackMaterialTiling;
    const uint32 base = static_cast<uint32>(table.size());
    for (uint32 role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
    {
        const TerrainMaterialEntry* entry = FindTerrainRoleMaterial(terrain, library, role);
        Terrain::TerrainMaterialRecord record =
            entry ? AuthorTerrainMaterialRecord(*entry, resolveTexture, access)
                  : (library ? Terrain::kDefaultTerrainMaterials[role]
                             : AuthorLegacyTerrainMaterialRecord(terrain, role, resolveTexture,
                                                                 access));

        // World-anchoring UV phase, per world axis. The fragment tiles from the render-origin-
        // relative position; adding fract(originWorld * effectiveTiling) shifts each projection's
        // UV by a WHOLE number of texture repeats, which a REPEAT sampler cannot see, so the tap
        // stays anchored to the world across a sector rebase. Computed in DOUBLE because fp32
        // cannot hold sector * kSectorSize * tiling at planetary magnitude. A hex-tiled material is
        // NOT anchored by it: the lattice ids re-hash under a whole-repeat shift. Zero while the
        // render origin is inactive, which is what keeps near-origin scenes byte-identical.
        const double effTiling = static_cast<double>(record.Tiling) / materialTiling;
        const auto wrapPhase = [&](int32 sector) -> float32 {
            const double p = static_cast<double>(sector) *
                             static_cast<double>(Engine::Renderer::kSectorSize) * effTiling;
            return static_cast<float32>(p - std::floor(p));
        };
        record.UVPhaseX = wrapPhase(renderOriginSector[0]);
        record.UVPhaseY = wrapPhase(renderOriginSector[1]);
        record.UVPhaseZ = wrapPhase(renderOriginSector[2]);
        // params carries the extent the terrain's textures cover (the tile grid's, for a tiled
        // terrain); a Planar material spans the authored size inside it.
        SetPlanarFootprintScale(record, params.WorldSizeX, params.WorldSizeZ, terrain.SizeX,
                                terrain.SizeZ);

        if (compat && role < 2u)
        {
            const GUID albedo = entry ? entry->AlbedoTexture
                                      : terrain.LayerAlbedoTexture[role].ToGuid();
            compatLayers.Albedo[role] = resolveHandle(albedo);
            compatLayers.Normal[role] = resolveHandle(entry ? entry->NormalTexture : GUID{});
            compatLayers.Orm[role] = resolveHandle(entry ? entry->OrmTexture : GUID{});
        }

        table.push_back(record);
        params.LayerRole[role] = base + role;
    }

    if (auto* feature = compat ? renderServices->GetFeature<TerrainRenderFeature>() : nullptr)
        feature->SetCompatLayerTextures(compatLayers);
}

void ApplyGrassTextureParams(Terrain::TerrainGPUParams& params,
                             const Components::TerrainGrass* grass,
                             Engine::Renderer::RenderServices* renderServices)
{
    if (!grass)
        return;

    if (grass->TextureGrass)
    {
        params.GrassAlbedoBindless = ResolveTerrainTextureBindless(renderServices, grass->AlbedoTextureAssetGuid.ToGuid());
        params.GrassAlphaBindless = ResolveTerrainTextureBindless(renderServices, grass->AlphaTextureAssetGuid.ToGuid());
        params.GrassNormalBindless = ResolveTerrainTextureBindless(renderServices, grass->NormalTextureAssetGuid.ToGuid());
        // Carried as handles too on the compat profile, which has no binding array to index (the
        // grass surface reads these as named bindings there).
        const bool compat = renderServices != nullptr && renderServices->GetProfile().IsCompat();
        if (auto* feature = compat ? renderServices->GetFeature<TerrainRenderFeature>() : nullptr)
        {
            const auto handleOf = [renderServices](const GUID& guid) -> Rendering::TextureHandle {
                if (guid.IsNull())
                    return {};
                return renderServices->Textures().GetOrUpload(guid);
            };
            feature->SetCompatGrassTextures(handleOf(grass->AlbedoTextureAssetGuid.ToGuid()),
                                            handleOf(grass->AlphaTextureAssetGuid.ToGuid()),
                                            handleOf(grass->NormalTextureAssetGuid.ToGuid()));
        }

        // ORed rather than assigned: this runs after GrassEnabled is composed, and is the one
        // place both extraction paths share, so the bit is set once instead of at each packing.
        //
        // With no services nothing bound a texture either, so the blade is alpha 1.0 outright.
        if (TerrainGrassNeedsAlpha(renderServices, *grass, /*answerWithoutServices=*/false))
            params.GrassEnabled |= Terrain::kTerrainGrassBitAlphaNeeded;
    }
    params.GrassAtlasColumns = std::max(1u, grass->AtlasColumns);
    params.GrassAtlasRows = std::max(1u, grass->AtlasRows);
    params.GrassAtlasTileCount = std::clamp(
        std::max(1u, grass->AtlasTileCount),
        1u,
        std::max(1u, params.GrassAtlasColumns * params.GrassAtlasRows));
    params.GrassAlphaCutoff = std::clamp(grass->AlphaCutoff, 0.0f, 1.0f);
    params.GrassNormalStrength = std::max(0.0f, grass->NormalStrength);
    params.GrassTextureCardsPerSquareMeter = std::max(0.0f, grass->TextureCardsPerSquareMeter);
    params.GrassTextureSize = std::max(0.0f, grass->TextureSize);
}

// A row that places nothing must not leave the enabled bit set: the GPU classify admits a cell on
// that bit alone, while the CPU budget summary reduces only over rows that place. Identical sets
// are what makes the plan kernel's slot bound cover every cell the emit kernel is handed, so the
// emit can spend its range without a runtime clamp. Runs after ApplyGrassTextureParams, which
// resolves the mode's density.
void ClearGrassEnabledIfNothingToPlace(Terrain::TerrainGPUParams& params)
{
    if (!Terrain::TerrainGrassRowPlaces(params))
        params.GrassEnabled &= ~Terrain::kTerrainGrassBitEnabled;
}

// The quarantine depth must be >= the frames the GPU can have in flight (triple-buffered worst
// case), so a slot freed this frame is never handed to a new tile while an in-flight frame still
// samples it (Risk 2). The slot budget itself is no longer a fixed 36 — DeriveAtlasSlotCount scales
// the resident window to the terrain's tile grid (VRAM-capped), so a large world (her 400-tile
// 10240 m terrain) gets a proportionally larger detail window instead of a small island in a coarse
// sea (design §6, revised for large terrains). GE_TERRAIN_ATLAS_SLOTS still overrides outright.

// Out-of-window fallback height (normalized [0,1], before HeightScale) for a tile with no
// resident slot (design §8 Risk 3). A flat representative plane — never a hole, continuous,
// TDR-safe. The accurate coarse-FIELD fallback (tight horizon-seam continuity) is a
// documented follow-up; this mid-height plane is the sound first cut for the dark-ship slice.
constexpr float32 kAtlasCoarseFallbackNormalized = 0.5f;

// Mirror the streaming manager's resident tile set into the atlas residency model,
// driving slot assignment/eviction and emitting the Terrain.Atlas* signals. Phase E:
// the resulting slot assignments + upload requests + indirection table drive the GPU
// atlas source (the texture patch + FrameParams binding happen in the caller / render
// thread). framesInFlight sets the Risk-2 quarantine depth.
void DriveAtlasResidency(AtlasResidencyController& ctrl, const TiledTerrainData& tiled,
                         const TiledRenderExtent& extent,
                         const std::vector<Mathematics::Vector3>& cameras,
                         uint32 slotOverride, uint32 framesInFlight, uint64 frame,
                         float32 deltaSeconds, float32 fadeSeconds)
{
    const uint32 tileRes = tiled.Config.TileConfig.HeightmapWidth;
    const uint32 tilesX = extent.TilesPerAxisX;
    const uint32 tilesZ = extent.TilesPerAxisZ;
    const uint32 totalTiles = std::max(1u, tilesX * tilesZ);
    uint32 slots = slotOverride != 0u
                       ? slotOverride
                       : DeriveAtlasSlotCount(tilesX, tilesZ, tileRes, kAtlasVramBudgetBytes,
                                              kAtlasSlotBytesPerTexel + (tiled.GrassRegionsActive ? 2u : 0u));
    slots = std::clamp(slots, 1u, totalTiles);
    ctrl.Configure(tileRes, slots, framesInFlight, tilesX, tilesZ, /*logSignals*/ true, fadeSeconds);

    const float32 tileSize = tiled.Config.TileWorldSize;
    std::vector<AtlasResidentTile> resident;
    resident.reserve(tiled.Tiles.size());
    for (const auto& [coord, tilePtr] : tiled.Tiles)
    {
        if (!tilePtr || tilePtr->LodState == TileLodState::Empty)
            continue;
        if (coord.X < 0 || coord.Z < 0 ||
            coord.X >= static_cast<int32>(tilesX) || coord.Z >= static_cast<int32>(tilesZ))
            continue;
        const float32 cx = tiled.WorldOriginX + (static_cast<float32>(coord.X) + 0.5f) * tileSize;
        const float32 cz = tiled.WorldOriginZ + (static_cast<float32>(coord.Z) + 0.5f) * tileSize;
        float32 bestSq = 0.0f;
        bool haveCam = false;
        for (const auto& cam : cameras)
        {
            const float32 dx = cx - cam.x;
            const float32 dz = cz - cam.z;
            const float32 d = dx * dx + dz * dz;
            bestSq = haveCam ? std::min(bestSq, d) : d;
            haveCam = true;
        }
        resident.push_back({coord, bestSq, tilePtr->LodState == TileLodState::Full});
    }
    // Bound the per-frame slot-upload spike (the large-terrain movement hiccup): fill at most
    // kAtlasMaxAssignsPerFrame new slots per frame; deferred tiles stay in the coarse base relief.
    ctrl.Update(frame, deltaSeconds, resident, kAtlasMaxAssignsPerFrame);
}

} // namespace

TerrainExtractionSystem::TerrainExtractionSystem(Engine::Renderer::RenderServices* renderServices)
    : m_RenderServices(renderServices), m_GrassFieldUploader(std::make_unique<TerrainGrassFieldUploader>())
{
    // GE_TERRAIN_ATLAS_SLOTS overrides the derived resident-slot budget — the soak lever that
    // forces eviction churn by shrinking the window below the tile count.
    if (const char* slots = std::getenv("GE_TERRAIN_ATLAS_SLOTS"))
        m_AtlasSlotOverride = static_cast<uint32>(std::strtoul(slots, nullptr, 10));
    // GE_TERRAIN_ATLAS_FADE overrides the coarse->slot crossfade window (seconds); 0 disables it
    // (instant swap = the pre-fade behavior, for the before/after capture and as a runtime kill switch).
    if (const char* fade = std::getenv("GE_TERRAIN_ATLAS_FADE"))
        m_AtlasFadeSeconds = std::max(0.0f, static_cast<float32>(std::strtod(fade, nullptr)));
    // GE_TERRAIN_ATLAS_NO_REGION forces every content edit through the whole-slot path (the
    // pre-region-scoping behavior): the before/after measurement lever and the runtime kill switch
    // if the region fast-path ever misbehaves. Default: region patching ON.
    m_AtlasRegionPatch = std::getenv("GE_TERRAIN_ATLAS_NO_REGION") == nullptr;
    // GE_TERRAIN_UNIFIED_NO_REGION forces every unified-path (default tiled) content edit
    // through the whole-tile upload + whole-tile normal regen (the pre-fix ~50 ms/dab
    // sculpt hitch): the before/after measurement lever and the runtime kill switch if the
    // region fast-path ever misbehaves. Default: region patching ON.
    m_UnifiedRegionPatch = std::getenv("GE_TERRAIN_UNIFIED_NO_REGION") == nullptr;
}

TerrainExtractionSystem::~TerrainExtractionSystem() = default;

bool TerrainExtractionSystem::TryGetAtlasResidency(uint32 tiledTerrainIndex,
                                                   AtlasResidencySnapshot& outSnapshot) const
{
    const auto it = m_AtlasByTiled.find(tiledTerrainIndex);
    if (it == m_AtlasByTiled.end() || !it->second || !it->second->IsConfigured())
        return false;

    const AtlasResidencyController& atlas = *it->second;
    outSnapshot.SlotCount = atlas.Geometry().SlotCount;
    outSnapshot.ResidentCount = atlas.ResidentCount();
    outSnapshot.TileRes = atlas.Geometry().TileRes;
    outSnapshot.TableVersion = atlas.TableVersion();
    outSnapshot.AssignsThisFrame = atlas.AssignsThisFrame();
    outSnapshot.EvictionsThisFrame = atlas.EvictionsThisFrame();
    outSnapshot.UploadsThisFrame = atlas.UploadsThisFrame();
    outSnapshot.FallbacksThisFrame = atlas.FallbacksThisFrame();
    outSnapshot.ActiveFadesThisFrame = atlas.ActiveFadesThisFrame();
    return true;
}

void TerrainExtractionSystem::ForgetTiledTerrain(uint32 tiledIndex)
{
    if (m_StreamingManager)
        m_StreamingManager->Forget(tiledIndex);
    m_AtlasByTiled.erase(tiledIndex);
    m_AtlasCoarseBuilt.erase(tiledIndex);
    m_AtlasOverCapWarnedTiled.erase(tiledIndex);
    m_ReprovisionDebounce.erase(tiledIndex);
    m_TiledSlotGeneration.erase(tiledIndex);
}

void TerrainExtractionSystem::ForgetRetiredTiledTerrains(TerrainService& service)
{
    std::vector<uint32> retired;
    for (const auto& [index, generation] : m_TiledSlotGeneration)
    {
        if (!service.GetTiledTerrainData(TiledTerrainHandle{index, generation}))
            retired.push_back(index);
    }

    if (retired.empty())
        return;

    // In-flight tile jobs write only into their own TileGenerationResult, so cancelling
    // here (rather than at the teardown itself) risks nothing beyond the work already
    // done — but their results must never integrate into the terrain that reuses the
    // slot, and this runs before any integration for the frame. Every other tiled
    // terrain keeps streaming.
    for (uint32 index : retired)
        ForgetTiledTerrain(index);
}

// How far the skyline march looks and how far apart its stations are. 8 km is past the visible
// horizon for any eye a scene puts a camera at, and the step is floored to the terrain's own
// lattice inside the march -- finer would interpolate between the same two corners twice.
static constexpr float32 kSkylineMarchDistance = 8000.0f;
static constexpr float32 kSkylineMarchStep = 4.0f;

// The terrain skyline along the sun's azimuth, per view, handed to the sun glare.
//
// This lives on the TERRAIN side because it is the only side that can see both halves: the
// renderer cannot include TerrainECS (TerrainECS owns a render feature, so the dependency runs
// the other way), and the glare's render node has a camera but no ECS world. Here there is a
// world, a RenderServices, and the heightfield.
//
// Why the glare needs it at all: its screen-space depth probe is gated on the sun projecting
// inside the viewport, so a sun one degree outside the frame edge cannot be occluded by
// anything, and the halo switched off the frame its centre crossed in. A skyline is a property
// of the terrain and the eye, so it holds on- and off-frame and past any cascade distance.
void TerrainExtractionSystem::PublishSunGlareSkyline(ECS::World& world)
{
    GE_CPU_PROFILE_SCOPE("Terrain.SunGlareSkyline");

    auto* glare = m_RenderServices->GetFeature<Engine::Renderer::SunGlareRenderFeature>();
    if (!glare)
        return;

    const auto* sky = m_RenderServices->GetFeature<Engine::Renderer::SkyRenderFeature>();
    if (!sky || !sky->HasActiveSettings())
        return;
    const Rendering::SkySettings& settings = sky->GetSettings();
    if (!settings.showSunDisk)
        return;

    // Resolved every frame rather than cached: the query holds pointers into terrain residency
    // that streaming moves, and a stale one would march over freed tiles.
    const PlanarHeightQuery ground = ResolvePlanarHeightQuery(world);

    // The WHOLE set, replaced each frame. Scratch reused so the refill does not allocate.
    m_SkylineScratch.clear();
    for (const Rendering::ViewDesc& view : m_RenderServices->Views().GetViews())
    {
        float32 tangent = Engine::Renderer::SunGlareRenderFeature::kNoSkyline;
        if (ground.IsValid())
        {
            const Rendering::CameraData* camera =
                m_RenderServices->Views().FindCameraData(view.cameraId);
            if (camera)
            {
                const float32 marched = TerrainHorizonTangent(
                    ground, camera->cameraPos[0], camera->cameraPos[1], camera->cameraPos[2],
                    settings.scatteringSunDir[0], settings.scatteringSunDir[2],
                    kSkylineMarchDistance, kSkylineMarchStep);
                if (marched != kNoTerrainHorizon)
                    tangent = marched;
            }
        }
        m_SkylineScratch.push_back({view.id, tangent});
    }
    glare->SetSkylines(m_SkylineScratch.data(), m_SkylineScratch.size());
}

void TerrainExtractionSystem::Update(ECS::World& world, float32 deltaTime)
{
    GE_CPU_PROFILE_SCOPE("Terrain.Extraction");
    if (!m_RenderServices)
        return;

    auto* terrainService = TerrainService::TryGet();
    if (!terrainService)
        return;

    // Before anything reads or integrates into a tiled slot: a teardown outside this
    // system (scene close, entity delete, component remove) recycles the slot index
    // its per-terrain trackers key on.
    ForgetRetiredTiledTerrains(*terrainService);

    PublishSunGlareSkyline(world);

    auto* feature = m_RenderServices->GetFeature<TerrainRenderFeature>();
    if (!feature || !feature->IsInitialized())
        return;

    auto* device = m_RenderServices->GetDevice();
    if (!device)
        return;

    // A device rebuild left the feature with no GPU resources (TerrainRenderFeature::
    // OnDeviceRebuilt). Every terrain uploads again this tick from the terrain service's CPU
    // data: single terrains their heightfield, splat and normal maps; tiled terrains every
    // resident tile, through a fresh atlas residency; and every grass field.
    const uint64 deviceResourceEpoch = feature->GetDeviceResourceEpoch();
    const bool reuploadAll = deviceResourceEpoch != m_UploadedDeviceResourceEpoch;
    if (reuploadAll)
    {
        m_UploadedDeviceResourceEpoch = deviceResourceEpoch;
        m_GrassFieldUploader = std::make_unique<TerrainGrassFieldUploader>();
        m_AtlasByTiled.clear();
        m_AtlasCoarseBuilt.clear();
    }

    // A CHANGE TOKEN for the params ring, not an element index — UploadTerrainParamsArray
    // unwraps it so all kMaxFrames elements rotate (see its declaration).
    const uint32 deviceFrameIndex = device->GetFrameIndex();

    // One monotonic atlas clock per REAL frame, shared by every tiled terrain's
    // controller. Incremented once here (not per terrain) so the Risk 2 quarantine
    // spans framesInFlight REAL frames regardless of how many tiled terrains exist
    // — a per-terrain increment would shrink it to ceil(framesInFlight / N).
    // Real-frame clock for the slot quarantine (Risk 2). Free to advance unconditionally, and
    // advancing it always keeps the quarantine depth meaningful for a terrain that engages the
    // atlas mid-session (a resize past the unified ceiling).
    const uint64 atlasFrame = ++m_AtlasFrameCounter;

    // Collect camera positions from all active views for tile streaming.
    // Tiles within streaming radius of ANY camera are loaded.
    // Also extract the maximum far clip across all views for LOD range clamping.
    std::vector<Mathematics::Vector3> viewCameraPositions;
    Mathematics::Vector3 viewDirection{0, 0, 1}; // Default forward
    float32 maxViewFarClip = 1000.0f; // Conservative default
    // The largest 1 / tan(vfov / 2) of the views: the height pages' level rule (PageLevelRule.h)
    // serves the narrowest field of view, the one that asks for the finest level at a distance.
    float32 pageFocalScale = 0.0f;
    // Render-origin sector for the material table's world-anchoring UV phase. The table is shared
    // by every view, so it takes the FIRST camera's sector — the same "first camera wins" rule the
    // direction above uses. Two views far enough apart to sit in different sectors would anchor the
    // table to one of them; near-origin scenes (sector 0) are unaffected either way.
    int32 renderOriginSector[4] = {};
    {
        const auto& views = m_RenderServices->Views().GetViews();
        for (const auto& view : views)
        {
            const auto* camData = m_RenderServices->Views().FindCameraData(view.cameraId);
            if (camData)
            {
                viewCameraPositions.push_back(ExtractCameraPosition(*camData));
                pageFocalScale = std::max(pageFocalScale, std::abs(camData->proj[5]));
                // First camera XZ look for tile in-front priority. LH view row 2
                // is forward (MakeLookAtLH); do not negate — that was the RH
                // "looks along -Z in view space" fork and contradicted the
                // {0,0,1} default above.
                if (viewCameraPositions.size() == 1)
                {
                    viewDirection = TilePriorityViewDirectionXZ(camData->view);
                    // ResolveCameraData, NOT the camData above: the sector is derived on read by
                    // ComputeRebasedView and is zero in the registry's stored CameraData.
                    const auto resolved = m_RenderServices->Views().ResolveCameraData(view.id);
                    std::memcpy(renderOriginSector, resolved.renderOriginSector,
                                sizeof(renderOriginSector));
                }

                // Extract far clip from perspective projection (LH, ZO):
                // GLM perspectiveLH_ZO:
                //   proj[10] = zFar / (zFar - zNear)
                //   proj[14] = -(zNear * zFar) / (zFar - zNear)
                // => zFar = -proj[14] / (proj[10] - 1)
                const float32 p10 = camData->proj[10];
                const float32 p14 = camData->proj[14];
                if (std::abs(p10 - 1.0f) > 1e-6f)
                {
                    const float32 farClip = -p14 / (p10 - 1.0f);
                    if (farClip > 0.0f)
                        maxViewFarClip = std::max(maxViewFarClip, farClip);
                }
            }
        }
    }

    // Assert that the modifier system is not concurrently accessing terrain data.
    terrainService->AssertNoModifierAccess();

    std::vector<TerrainInstanceInfo> activeInfos;
    std::vector<Terrain::TerrainGPUParams> paramsArray;
    // Every terrain's four channel-role materials, concatenated. Params entries name records in
    // here by absolute index, so the two are built and published as one unit.
    std::vector<Terrain::TerrainMaterialRecord> materialTable;

    world.Query<ECS::Write<Components::Terrain>, ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle entity,
                  Components::Terrain& terrain,
                  const Components::WorldTransform& worldXf)
        {
            // Grass placement is planar-only (TerrainGrassSupportsDomain): the compute roots blades
            // on a flat XZ grid, which floats off a cube-sphere planet's curved surface. Source no
            // grass for a spherical terrain until radial placement lands, nor for grass that is
            // switched off — both params blocks below emit the disabled/zeroed grass state when
            // this is null.
            const auto* grassComp = world.GetComponent<Components::TerrainGrass>(entity);
            const bool grassOn = grassComp && ECS::Entity(&world, entity).IsEnabled<Components::TerrainGrass>();
            const auto* grass =
                grassOn && Components::TerrainGrassSupportsDomain(terrain.Domain) ? grassComp : nullptr;

            // Entity position is the center of the terrain.
            // Compute the corner origin for GPU params and patch selection.
            const float32 centerX = worldXf.matrix[12];
            const float32 centerZ = worldXf.matrix[14];
            const float32 worldX = centerX - terrain.SizeX * 0.5f;
            const float32 worldZ = centerZ - terrain.SizeZ * 0.5f;

            // Derive config from user-facing parameters.
            const auto desired = Terrain::TerrainConfig::FromSamplesPerMeter(
                terrain.SizeX, terrain.SizeZ, terrain.HeightScale,
                terrain.SamplesPerMeter, Terrain::kDefaultGridSize, Terrain::kDefaultLODRangeScale);

            // Auto-detect tiling: if the derived resolution exceeds the per-tile
            // cap, the terrain is automatically split into tiles.
            const bool needsTiling = Terrain::TerrainNeedsTiling(
                terrain.SizeX, terrain.SizeZ, terrain.SamplesPerMeter);

            // ---- Tiled terrain path ----
            if (needsTiling)
            {
                // Clean up non-tiled TerrainData when transitioning to tiled.
                if (terrain.TerrainDataHandle != 0 || terrain.TerrainDataGeneration != 0)
                {
                    TerrainHandle oldH{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
                    feature->ReleaseTerrainResources(oldH);
                    terrainService->DestroyTerrain(oldH);
                    terrain.TerrainDataHandle = 0;
                    terrain.TerrainDataGeneration = 0;
                }

                TiledTerrainHandle tiledHandle{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
                auto* tiled = terrainService->GetTiledTerrainData(tiledHandle);

                // Detect config changes and handle them with minimal disruption.
                // Patch grid size is a service-internal constant now, so only a
                // resolution, size or base change forces a rebuild.
                if (tiled)
                {
                    // A base change is a new base source, heightmap asset or heightmap content
                    // (every eviction of the heightmap bumps its content version: a hot reload,
                    // and any other content event for that asset). Every resident and streaming
                    // tile was filled from the old base, and a coarse tile is never re-baked in
                    // place, so the tile set is rebuilt from the new one. A discrete edit: no
                    // settle debounce.
                    //
                    // A resolution or size edit is coalesced instead (an inspector SamplesPerMeter /
                    // size drag emits a distinct value almost every frame) into ONE re-provision once
                    // the value settles. Until then keep rendering the existing terrain untouched — a
                    // per-tick destroy+recreate rebuilds the whole tile set + unified/atlas textures
                    // and restarts streaming, thrashing VRAM and (with the bindless teardown) faulting
                    // the device. The debounce key is the live TiledTerrainHandle index, stable because
                    // we do not destroy while settling; it is cleared in ForgetTiledTerrain on teardown.
                    // StepTiledReprovision is the rule (tested in TerrainRegionBakeTests).
                    const TiledTerrainEdit edit = terrainService->CompareTiledTerrainConfig(tiled->Config, terrain);
                    const bool baseChanged = edit.BaseChanged;
                    if (StepTiledReprovision(m_ReprovisionDebounce, tiledHandle.Index, edit,
                                             terrain.SamplesPerMeter, terrain.SizeX, terrain.SizeZ,
                                             kReprovisionSettleFrames))
                    {
                        // Full recreation needed — resolution changes require new
                        // heightfields, size changes alter the tile grid layout, base
                        // changes alter every tile's starting ground. Cancel
                        // in-flight CPU streaming jobs (they only produce host data now)
                        // and release the unified GPU textures for the old grid before
                        // recreating; the deferred-destroy keeps them alive until the GPU
                        // drains the frames that still reference the retired size.
                        //
                        // Grep-able transition marker: a re-provision reallocates the whole unified
                        // texture set at the new density while the old set is still quarantined, and
                        // re-streams/re-bakes every resident tile — the dominant frame-cost of an
                        // SPM/size edit and the correlation point for a transition-window device-lost.
                        LOG_INFO("TiledTerrain: re-provision (edit-driven) old grid {}x{} -> new spm={} "
                                 "size=({},{}) baseChanged={} - retiring unified set, rebuilding",
                                 tiled->Config.TilesPerAxisX, tiled->Config.TilesPerAxisZ,
                                 terrain.SamplesPerMeter, terrain.SizeX, terrain.SizeZ, baseChanged);

                        feature->ReleaseTerrainResources(
                            TerrainHandle{tiled->GlobalGpuHandleIndex, tiled->GlobalGpuHandleGeneration});

                        // Drop the atlas trackers for the retired handle so a new terrain
                        // reusing this slot index starts with a fresh pool + diff state (a stale
                        // controller would suppress the new terrain's assign/upload signals — the
                        // m_AtlasByTiled bug; a stale coarse-built flag would skip its first build).
                        ForgetTiledTerrain(tiledHandle.Index);

                        terrainService->DestroyTiledTerrain(tiledHandle);
                        terrain.TiledTerrainHandle = 0;
                        terrain.TiledTerrainGeneration = 0;
                        tiled = nullptr;
                        tiledHandle = {};
                    }
                }

                // Create tiled terrain if it doesn't exist.
                if (!tiled)
                {
                    // Same stale-handle rule as the single-terrain path: the handle
                    // named nothing, so provision fresh instead of stranding the entity.
                    ClearUnresolvedTerrainHandles(terrain);

                    // The component's size, density and base (BuildTiledTerrainConfig, tested in
                    // TerrainRegionBakeTests).
                    const auto newHandle =
                        terrainService->CreateTiledTerrain(terrainService->BuildTiledTerrainConfig(terrain));
                    terrain.TiledTerrainHandle = newHandle.Index;
                    terrain.TiledTerrainGeneration = newHandle.Generation;

                    tiled = terrainService->GetTiledTerrainData(newHandle);
                    if (!tiled)
                        return;

                    tiled->WorldOriginX = worldX;
                    tiled->WorldOriginZ = worldZ;
                    tiledHandle = newHandle;

                    LOG_INFO("TiledTerrain: origin=({},{}) tileSize={} tiles={}x{} res={} lod={} streamR={}",
                             worldX, worldZ, tiled->Config.TileWorldSize,
                             tiled->Config.TilesPerAxisX, tiled->Config.TilesPerAxisZ,
                             tiled->Config.TileConfig.HeightmapWidth,
                             tiled->Config.TileConfig.LODLevels,
                             tiled->Config.StreamingRadius);
                }

                // Record the live generation of this slot index so a teardown that does
                // not run through this system is still detected on a later tick.
                m_TiledSlotGeneration[tiledHandle.Index] = tiledHandle.Generation;

                // Update world origin if entity moved.
                tiled->WorldOriginX = worldX;
                tiled->WorldOriginZ = worldZ;

                if (reuploadAll)
                    for (auto& [coord, tile] : tiled->Tiles)
                        if (tile)
                        {
                            tile->HeightfieldDirty = true;
                            tile->SplatmapDirty = true;
                        }

                // ---- Async tile streaming via TileStreamingManager ----

                const float32 tileSize = tiled->Config.TileWorldSize;

                // Unified render resolution (option a): tilesPerAxis * tileInterior + 1, the same
                // grid an untiled terrain of equal content uses. The unified source is
                // full-terrain resolution, so it is the measure of when one texture stops being
                // able to carry the world — which is what the engagement rule below tests.
                const TiledRenderExtent extent = ComputeTiledRenderExtent(tiled->Config);
                const uint32 tilesX = extent.TilesPerAxisX;
                const uint32 tilesZ = extent.TilesPerAxisZ;
                const uint32 tileInteriorX = extent.TileInteriorX;
                const uint32 tileInteriorZ = extent.TileInteriorZ;
                const uint32 unifiedW = extent.UnifiedWidth;
                const uint32 unifiedH = extent.UnifiedHeight;
                // Height-source engagement rule. The unified texture IS the whole terrain, so it
                // stops being viable exactly at the point where the world outgrows one texture
                // (a 50000^2 source is ~30 GB). The resident-window atlas is sized to the
                // streaming window instead — fixed VRAM regardless of world size — so it takes
                // over from precisely that threshold upward, and there is no size an author can
                // dial in that renders nothing.
                const bool useAtlas = unifiedW > kMaxUnifiedTiledResolution ||
                                      unifiedH > kMaxUnifiedTiledResolution;

                // Streaming radius must cover at least the far clip plus one tile
                // of margin so tiles are pre-loaded before they enter the visible
                // range. If the user-configured streaming radius is larger, use that.
                const float32 streamR = std::max(tiled->Config.StreamingRadius,
                                                  maxViewFarClip + tileSize);

                auto streamCenters = viewCameraPositions;
                if (streamCenters.empty())
                    streamCenters.push_back(Mathematics::Vector3(centerX, 0, centerZ));

                // Create streaming manager lazily on first tiled terrain use. It loads
                // CPU tile data only (heightfield/splat/normal); the unified GPU textures
                // are owned by the feature and patched here on the graphics queue. One
                // manager streams every tiled terrain, each through its own state.
                if (!m_StreamingManager)
                {
                    auto* jobSystem = &EngineCore::GetInstance().GetJobSystem();
                    m_StreamingManager = std::make_unique<TileStreamingManager>(jobSystem);
                }

                const std::vector<TileCoord>* integratedTilesThisFrame = nullptr;
                {
                    GE_CPU_PROFILE_SCOPE("Terrain.Streaming");
                    integratedTilesThisFrame = &m_StreamingManager->Update(
                        tiledHandle, *tiled, *terrainService, streamCenters, viewDirection,
                        streamR, deltaTime);
                }

                // ---- Resident-window atlas residency model ----
                // Mirrors the streamed resident set into the atlas slot pool + indirection table
                // (assign/evict + upload requests), emits the Terrain.Atlas* signals, and DRIVES
                // the GPU source — the slot texture patches happen in the atlas branch below and
                // the indirection table rides TerrainInstanceInfo to CBT.
                if (useAtlas)
                {
                    GE_CPU_PROFILE_SCOPE("Terrain.AtlasResidency");
                    // F2: the indirection table has one row per whole-terrain tile, capped at
                    // kAtlasMaxTiles (CBTResources clamps the upload). Past the cap the tail tiles
                    // never get a row, so they resolve through the coarse fallback forever — warn
                    // once instead of silently degrading a slice of the world.
                    if (static_cast<uint64>(tilesX) * static_cast<uint64>(tilesZ) >
                            static_cast<uint64>(CBTTerrain::kAtlasMaxTiles) &&
                        m_AtlasOverCapWarnedTiled.insert(tiledHandle.Index).second)
                    {
                        LOG_WARNING("TiledTerrain: {}x{} tiles ({}) exceeds the atlas indirection cap "
                                    "{} — tail tiles resolve through the coarse fallback (never a hole)",
                                    tilesX, tilesZ, static_cast<uint64>(tilesX) * tilesZ,
                                    CBTTerrain::kAtlasMaxTiles);
                    }
                    auto& ctrl = m_AtlasByTiled[tiledHandle.Index];
                    if (!ctrl)
                        ctrl = std::make_unique<AtlasResidencyController>();
                    // Quarantine depth = the renderer's real frames-in-flight (Risk 2): an
                    // evicted texture slot must not be reused while an in-flight frame still
                    // samples it. Floored at 2 and naturally capped at the ring depth (4), so
                    // it never exceeds the indirection SSBO's double-buffering window.
                    const uint32 framesInFlight = std::max(2u, device->GetFramesInFlight());
                    DriveAtlasResidency(*ctrl, *tiled, extent, streamCenters,
                                        m_AtlasSlotOverride, framesInFlight, atlasFrame,
                                        deltaTime, m_AtlasFadeSeconds);
                }

                // ---- Batched quadtree update for tiles integrated this frame ----

                const std::vector<TileCoord>& integratedTiles = *integratedTilesThisFrame;
                if (!integratedTiles.empty() || tiled->QuadtreeDirty ||
                    !tiled->DirtyQuadtreeTiles.empty())
                {
                    GE_CPU_PROFILE_SCOPE("Terrain.QuadtreeSync");
                    // Falsifiable idle-cost signal: the global-quadtree finalize walks
                    // per-tile node min/max (O(nodes), or O(samples) on the sample-scan
                    // fallback path) and is INVISIBLE to Terrain.TileUpload/TileBake — so
                    // if this fires every frame on an idle tiled terrain, it is the hidden
                    // per-frame cost. Silent unless it actually runs (gated on integrated
                    // tiles or a dirty quadtree).
                    static const bool kTileDebug = std::getenv("GE_TERRAIN_TILE_DEBUG") != nullptr;
                    if (kTileDebug)
                        Logger::Log::Info("Terrain.QuadtreeSync integrated={} quadtreeDirty={}",
                                          integratedTiles.size(), tiled->QuadtreeDirty ? 1 : 0);
                    for (const auto& intCoord : integratedTiles)
                    {
                        auto tileIt = tiled->Tiles.find(intCoord);
                        if (tileIt != tiled->Tiles.end() && tileIt->second &&
                            !tileIt->second->PrecomputedQuadtreeNodes.empty())
                        {
                            // Use pre-computed min/max from job thread (no heightfield scan).
                            terrainService->PatchTileInGlobalQuadtreePrecomputed(
                                tiledHandle, intCoord,
                                tileIt->second->PrecomputedQuadtreeNodes.data(),
                                tileIt->second->PrecomputedQuadtreeNodesPerAxis);
                        }
                        else
                        {
                            // Fallback: scan heightfield on main thread.
                            terrainService->PatchTileInGlobalQuadtree(tiledHandle, intCoord);
                        }
                    }
                    terrainService->FinalizeGlobalQuadtreeUpdates(tiledHandle);
                }

                if (!tiled->GlobalQuadtree.IsBuilt())
                {
                    return; // No tiles loaded yet.
                }

                // ---- Global GPU TerrainData (holds the global quadtree for render node) ----

                TerrainHandle globalGpuHandle{tiled->GlobalGpuHandleIndex, tiled->GlobalGpuHandleGeneration};
                auto* globalData = terrainService->GetTerrainData(globalGpuHandle);
                if (!globalData)
                {
                    if (tiled->GlobalGpuHandleIndex != 0 || tiled->GlobalGpuHandleGeneration != 0)
                        return; // Stale handle

                    // Create a lightweight TerrainData for the global quadtree.
                    // Minimal heightfield (3x3); only the quadtree and morph ranges matter.
                    Terrain::TerrainConfig globalConfig{};
                    globalConfig.HeightmapWidth = 3;
                    globalConfig.HeightmapHeight = 3;
                    globalConfig.WorldSizeX = terrain.SizeX;
                    globalConfig.WorldSizeZ = terrain.SizeZ;
                    globalConfig.HeightScale = terrain.HeightScale;
                    globalConfig.LODLevels = tiled->GlobalQuadtree.GetNumLevels();
                    globalConfig.LODRangeScale = Terrain::kDefaultLODRangeScale;
                    globalConfig.PatchGridSize = Terrain::kDefaultGridSize;

                    const auto h = terrainService->CreateTerrain(globalConfig);
                    tiled->GlobalGpuHandleIndex = h.Index;
                    tiled->GlobalGpuHandleGeneration = h.Generation;
                    globalGpuHandle = h;

                    globalData = terrainService->GetTerrainData(globalGpuHandle);
                    if (!globalData)
                        return;
                }

                // The height pages of this terrain, when its height source is its pages.
                {
                    GE_CPU_PROFILE_SCOPE("Terrain.HeightPages");
                    const auto* name = world.GetComponent<Components::Name>(entity);
                    RequestTiledTerrainHeightPages(*terrainService, *device, *tiled, extent, globalGpuHandle,
                                                   name ? name->View() : std::string_view{}, worldXf.matrix[13],
                                                   terrain.HeightScale, terrain.TargetPixelError, streamCenters,
                                                   pageFocalScale);
                }

                // Sync global quadtree to the GPU slot.
                globalData->Quadtree = tiled->GlobalQuadtree;
                globalData->Config.LODLevels = tiled->GlobalQuadtree.GetNumLevels();
                globalData->Config.WorldSizeX = terrain.SizeX;
                globalData->Config.WorldSizeZ = terrain.SizeZ;
                globalData->Config.HeightScale = terrain.HeightScale;
                globalData->Config.LODRangeScale = Terrain::kDefaultLODRangeScale;
                globalData->Config.PatchGridSize = Terrain::kDefaultGridSize;

                // ---- Compute morph ranges for the global quadtree ----
                // Within-tile levels use normal chained ranges.
                // Tile-level and above use FLT_MAX to force subdivision,
                // preventing multi-tile patches that can't be assigned to one tile.

                if (!tiled->MorphRangesComputed ||
                    tiled->CachedLODRangeScale != Terrain::kDefaultLODRangeScale ||
                    tiled->CachedFarClip != maxViewFarClip)
                {
                    const uint32 globalLevels = tiled->GlobalQuadtree.GetNumLevels();
                    const uint32 tileLevels = tiled->Config.TileConfig.LODLevels;
                    const uint32 finestNodesPerAxis = tiled->GlobalQuadtree.GetNodesPerAxisAtLevel(0);
                    const float32 patchSizeX = terrain.SizeX / static_cast<float32>(finestNodesPerAxis);
                    const float32 patchSizeZ = terrain.SizeZ / static_cast<float32>(finestNodesPerAxis);
                    const float32 baseRange = std::max(patchSizeX, patchSizeZ) * Terrain::kDefaultLODRangeScale;
                    constexpr float32 kMorphStartRatio = 0.66f;

                    // The coarsest renderable LOD range is clamped to the far clip
                    // plane so we never select patches beyond what the camera can see.
                    // Use the lesser of streaming radius and far clip.
                    const float32 maxRenderDist = std::min(streamR, maxViewFarClip);

                    // Within-tile levels (0 to tileLevels-3): normal LOD ranges.
                    for (uint32 i = 0; i + 2 < tileLevels && i < globalLevels; ++i)
                        tiled->VisRanges[i] = Terrain::CDLODSelection::ComputeLODRange(i, baseRange, Terrain::kDefaultLODRangeScale);

                    // Coarsest within-tile level: extend to cover the visible distance
                    // so far tiles render coarse patches instead of disappearing.
                    if (tileLevels >= 2 && tileLevels - 2 < globalLevels)
                    {
                        tiled->VisRanges[tileLevels - 2] = std::max(
                            Terrain::CDLODSelection::ComputeLODRange(tileLevels - 2, baseRange, Terrain::kDefaultLODRangeScale),
                            maxRenderDist);
                    }

                    // Tile-level and above: always enter, never produce multi-tile patches.
                    for (uint32 i = (tileLevels > 0 ? tileLevels - 1 : 0); i < globalLevels; ++i)
                        tiled->VisRanges[i] = std::numeric_limits<float32>::max();

                    // Compute chained morph start/end.
                    float32 prevMorphStart = 0.0f;
                    for (uint32 i = 0; i < globalLevels; ++i)
                    {
                        if (tiled->VisRanges[i] < 1e30f)
                        {
                            tiled->MorphEnd[i] = tiled->VisRanges[i];
                            tiled->MorphStart[i] = prevMorphStart + (tiled->MorphEnd[i] - prevMorphStart) * kMorphStartRatio;
                            prevMorphStart = tiled->MorphStart[i];
                        }
                        else
                        {
                            // Tile-level and above: these nodes always subdivide, so
                            // morph must be 0. With FLT_MAX for both, the shader computes
                            // (dist - FLT_MAX) / max(0, 0.001) → clamped to 0.
                            tiled->MorphStart[i] = std::numeric_limits<float32>::max();
                            tiled->MorphEnd[i] = std::numeric_limits<float32>::max();
                        }
                    }

                    tiled->MorphRangesComputed = true;
                    tiled->CachedLODRangeScale = Terrain::kDefaultLODRangeScale;
                    tiled->CachedFarClip = maxViewFarClip;
                }

                // Sync morph ranges to global GPU data.
                std::memcpy(globalData->VisRanges, tiled->VisRanges, sizeof(tiled->VisRanges));
                std::memcpy(globalData->MorphStart, tiled->MorphStart, sizeof(tiled->MorphStart));
                std::memcpy(globalData->MorphEnd, tiled->MorphEnd, sizeof(tiled->MorphEnd));
                globalData->MorphRangesComputed = true;

                // Grid mesh is now updated on the render thread (BuildForView)
                // to avoid races between extraction and rendering.

                // Shared GPU-source results the params/info build below reads. The unified
                // (default) and resident-window-atlas (Phase E) paths fill them differently:
                // the unified path registers bindless indices for one terrain-sized texture
                // set; the atlas path leaves them 0 (CBT binds the atlas directly, not
                // bindless) and fills the atlas geometry + indirection snapshot instead.
                uint32 splatmapBindless = 0, heightmapBindless = 0, normalmapBindless = 0;
                Rendering::TextureHandle infoHeightTex{}, infoSplatTex{};
                bool atlasBacked = false;
                AtlasGeometry atlasGeo{};
                Rendering::TextureHandle atlasHeightTex{};
                Rendering::TextureHandle atlasCoarseTex{};
                uint32 atlasCoarseDim = 0;
                uint32 atlasSplatBindless = 0, atlasNormalBindless = 0;
                uint32 atlasSplatCoarseBindless = 0, atlasNormalCoarseBindless = 0;
                uint32 atlasHeightBindless = 0, atlasCoarseBindless = 0;
                uint64 atlasTableVersion = 0;
                std::vector<TileAtlasSlot> atlasRows;

                if (!useAtlas)
                {
                    // ---- Unified GPU heightmap/splatmap/normalmap for the tiled terrain (C8) ----
                    //
                    // A tiled terrain renders through ONE unified texture set sized to the
                    // full terrain (dims computed + cap-gated above, before streaming): each
                    // resident Full tile patches its own sub-rect via the graphics-queue band
                    // path (content-preserving), so CBT + the surface sample it exactly like a
                    // single (untiled) terrain and stay tile-unaware. Adjacent tiles share their
                    // boundary sample (the base resolves per world position), so tile (tx,tz)
                    // lands at texel origin (tx*interior, tz*interior).
                    const bool unifiedTexturesRecreated =
                        feature->EnsureUnifiedTiledTextures(globalGpuHandle, unifiedW, unifiedH);

                    // A re-provision retires the old unified set and, under the VRAM staging barrier,
                    // withholds the new one for several frames. A tile that integrates during that
                    // withhold reaches the upload loop below, has its Upload*Region silently dropped
                    // (the valid-texture guard), yet is still ClearDirty'd — so it never re-uploads
                    // once the set allocates, freezing its patch at the fresh texture's zero-cleared
                    // content (the re-provision "stuck tile" artifact; a disable/enable healed it only
                    // because a from-scratch create allocates the set immediately, so its tiles upload
                    // as they integrate). The frame the set is (re)created, re-mark every resident tile
                    // for a full height+splat+normal re-upload so the loop below refills the whole
                    // texture this same frame — converging the re-provision path onto the exact tile
                    // content a from-scratch create produces (the recreate/re-provision unification).
                    if (unifiedTexturesRecreated)
                    {
                        for (auto& tileEntry : tiled->Tiles)
                        {
                            auto& tilePtr = tileEntry.second;
                            if (!tilePtr || tilePtr->LodState == TileLodState::Empty)
                                continue;
                            tilePtr->MarkFullDirty();
                            tilePtr->SplatmapDirty = true;
                        }
                    }

                    // GE_TERRAIN_TILE_DEBUG: one Terrain.TileUpload line per tile patched
                    // into the unified source this frame; silent on idle (no dirty tiles).
                    static const bool kTileUploadDebug = std::getenv("GE_TERRAIN_TILE_DEBUG") != nullptr;

                    // Iterate tiles in deterministic (coord-sorted) order for stable logs.
                    std::vector<TileCoord> sortedCoords;
                    sortedCoords.reserve(tiled->Tiles.size());
                    for (auto& [coord, tilePtr] : tiled->Tiles)
                        sortedCoords.push_back(coord);
                    std::sort(sortedCoords.begin(), sortedCoords.end(),
                        [](const TileCoord& a, const TileCoord& b) {
                            return (a.Z != b.Z) ? (a.Z < b.Z) : (a.X < b.X);
                        });

                    // The unified texel span one tile occupies (shared-edge inclusive).
                    // A Full tile's own heightfield is exactly this wide; a Coarse tile's
                    // is smaller and gets bilinear-upsampled into it (below).
                    const uint32 tileRegionDim = tileInteriorX + 1;
                    // Is the tile at (nx,nz) resident AND at Full LOD? A coarse patch must
                    // not overwrite an edge texel shared with a Full neighbor (the Full side
                    // owns the exact + modifier-applied edge — a coarse write is a seam).
                    auto neighborFull = [&](int32 nx, int32 nz) -> bool {
                        if (nx < 0 || nz < 0 ||
                            nx >= static_cast<int32>(tilesX) || nz >= static_cast<int32>(tilesZ))
                            return false;
                        auto it = tiled->Tiles.find(TileCoord{nx, nz});
                        return it != tiled->Tiles.end() && it->second &&
                               it->second->LodState == TileLodState::Full;
                    };
                    GE_CPU_PROFILE_SCOPE("Terrain.TileUpload");
                    for (const auto& coord : sortedCoords)
                    {
                        auto* tile = tiled->Tiles[coord].get();
                        // Empty tiles have no data yet; Coarse and Full both patch (Coarse
                        // as a smooth low-frequency approximation, upgraded in place when
                        // Full arrives — the streamed tile re-flags HeightfieldDirty).
                        if (!tile || tile->LodState == TileLodState::Empty)
                            continue;
                        if (coord.X < 0 || coord.Z < 0 ||
                            coord.X >= static_cast<int32>(tilesX) || coord.Z >= static_cast<int32>(tilesZ))
                            continue;

                        const uint32 dstX = static_cast<uint32>(coord.X) * tileInteriorX;
                        const uint32 dstY = static_cast<uint32>(coord.Z) * tileInteriorZ;
                        const bool coarse = tile->LodState != TileLodState::Full;

                        // Region fast-path (sculpt real-time on the default unified tiled
                        // terrain): a dab into an already-resident Full tile touched only a
                        // small rect. The persistent normal is seeded and the tile's dirty-
                        // region journal says exactly which texels changed since the normal
                        // was generated — regenerate + re-upload only that rect (+ the
                        // central-difference / region-splat footprint) instead of the whole
                        // tile's ~1M-texel normal regen + ~12 MB height/normal/splat upload
                        // (the ~50 ms/dab sculpt hitch). Byte-identical to the whole-tile
                        // path below: the region normal generator matches the whole-tile
                        // generator (no neighbours, border-clamped) and the untouched texels
                        // are unchanged since the last upload. First upload / stream-in /
                        // coarse / range-shift (whole-tile MarkFullDirty, or a rect spanning
                        // the whole tile) fall through to the whole-tile path. Mirrors the
                        // atlas patchSlotRegion fast-path (#528).
                        if (m_UnifiedRegionPatch && !coarse && tile->HeightfieldDirty &&
                            tile->HeightfieldVersion > tile->NormalmapVersion)
                        {
                            const uint32 res = tile->Heightfield.GetWidth();
                            const bool nativeSquare = res >= 2 && tile->Heightfield.GetHeight() == res;
                            const bool normalSeeded = tile->Normalmap.size() ==
                                                          static_cast<size_t>(res) * res * 4u &&
                                                      tile->NormalmapWidth == res &&
                                                      tile->NormalmapHeight == res;
                            const bool splatNative = tile->Splatmap.size() ==
                                                         static_cast<size_t>(res) * res * 4u &&
                                                     tile->SplatmapWidth == res &&
                                                     tile->SplatmapHeight == res;
                            DirtyRegionLog::Region dirty;
                            if (nativeSquare && normalSeeded && splatNative &&
                                tile->HeightfieldDirtyLog.CollectSince(tile->NormalmapVersion, dirty) &&
                                !dirty.IsEmpty())
                            {
                                constexpr int32 kEditRegionPad = 2;
                                const int32 last = static_cast<int32>(res) - 1;
                                const int32 x0 = std::max(dirty.MinX - kEditRegionPad, 0);
                                const int32 z0 = std::max(dirty.MinZ - kEditRegionPad, 0);
                                const int32 x1 = std::min(dirty.MaxX - 1 + kEditRegionPad, last);
                                const int32 z1 = std::min(dirty.MaxZ - 1 + kEditRegionPad, last);
                                const bool wholeTile = x0 == 0 && z0 == 0 && x1 == last && z1 == last;
                                if (!wholeTile)
                                {
                                    GenerateNormalmapRegionFromHeightfield(
                                        tile->Heightfield, tileSize, tileSize, terrain.HeightScale,
                                        tile->Normalmap, x0, z0, x1, z1);
                                    tile->NormalmapVersion = tile->HeightfieldVersion;
                                    const uint32 ux0 = static_cast<uint32>(x0);
                                    const uint32 uz0 = static_cast<uint32>(z0);
                                    const uint32 rw = static_cast<uint32>(x1 - x0 + 1);
                                    const uint32 rh = static_cast<uint32>(z1 - z0 + 1);
                                    ExtractSubBlock(tile->Heightfield.GetRawSamples(), res,
                                                    ux0, uz0, rw, rh, m_UnifiedHeightRegionScratch);
                                    feature->UploadHeightmapRegion(globalGpuHandle,
                                                                   m_UnifiedHeightRegionScratch.data(),
                                                                   rw, rh, dstX + ux0, dstY + uz0);
                                    ExtractSubBlock(reinterpret_cast<const uint32*>(tile->Normalmap.data()),
                                                    res, ux0, uz0, rw, rh, m_UnifiedSurfaceRegionScratch);
                                    feature->UploadNormalmapRegion(globalGpuHandle,
                                        reinterpret_cast<const uint8*>(m_UnifiedSurfaceRegionScratch.data()),
                                        rw, rh, dstX + ux0, dstY + uz0);
                                    ExtractSubBlock(reinterpret_cast<const uint32*>(tile->Splatmap.data()),
                                                    res, ux0, uz0, rw, rh, m_UnifiedSurfaceRegionScratch);
                                    feature->UploadSplatmapRegion(globalGpuHandle,
                                        reinterpret_cast<const uint8*>(m_UnifiedSurfaceRegionScratch.data()),
                                        rw, rh, dstX + ux0, dstY + uz0);
                                    tile->ClearDirty();
                                    tile->SplatmapDirty = false;
                                    if (kTileUploadDebug)
                                        Logger::Log::Info(
                                            "Terrain.TileUpload tile=({},{}) rect=({},{},{},{}) lod=full "
                                            "src=region",
                                            coord.X, coord.Z, dstX + ux0, dstY + uz0,
                                            dstX + static_cast<uint32>(x1) + 1,
                                            dstY + static_cast<uint32>(z1) + 1);
                                    continue;
                                }
                            }
                        }

                        bool patched = false;
                        if (tile->HeightfieldDirty)
                        {
                            if (coarse)
                            {
                                // Upsample the coarse heightfield to the full tile region so
                                // the tile shows relief (not a flat hole) while its Full detail
                                // streams. Exclude any edge shared with a resident-Full neighbor
                                // (the Full side owns that exact + modifier-applied texel; a
                                // coarse write there is a persistent seam). Endpoints are
                                // preserved, so coarse<->coarse shared edges stay crack-free.
                                CoarsePatchNeighbors nb;
                                nb.LeftFull = neighborFull(coord.X - 1, coord.Z);
                                nb.RightFull = neighborFull(coord.X + 1, coord.Z);
                                nb.TopFull = neighborFull(coord.X, coord.Z - 1);
                                nb.BottomFull = neighborFull(coord.X, coord.Z + 1);
                                const CoarsePatchRect pr = ComputeCoarsePatchRect(tileRegionDim, nb);
                                if (pr.Width > 0 && pr.Height > 0)
                                {
                                    UpsampleHeightfieldBilinear(tile->Heightfield.GetRawSamples(),
                                                                tile->Heightfield.GetWidth(),
                                                                m_CoarseHeightScratch, tileRegionDim);
                                    ExtractSubBlock(m_CoarseHeightScratch.data(), tileRegionDim,
                                                    pr.X0, pr.Z0, pr.Width, pr.Height, m_CoarseHeightPatch);
                                    feature->UploadHeightmapRegion(globalGpuHandle,
                                                                   m_CoarseHeightPatch.data(),
                                                                   pr.Width, pr.Height,
                                                                   dstX + pr.X0, dstY + pr.Z0);
                                    // Splat rides the same coarse patch (upsampled + clamped) so
                                    // the tile is textured, not black.
                                    if (!tile->Splatmap.empty())
                                    {
                                        UpsampleSplatmapBilinear(tile->Splatmap.data(), tile->SplatmapWidth,
                                                                 m_CoarseSplatScratch, tileRegionDim);
                                        ExtractSubBlock(reinterpret_cast<const uint32*>(m_CoarseSplatScratch.data()),
                                                        tileRegionDim, pr.X0, pr.Z0, pr.Width, pr.Height,
                                                        m_CoarseSplatPatch);
                                        feature->UploadSplatmapRegion(globalGpuHandle,
                                            reinterpret_cast<const uint8*>(m_CoarseSplatPatch.data()),
                                            pr.Width, pr.Height, dstX + pr.X0, dstY + pr.Z0);
                                    }
                                    // Real normals from the upsampled coarse heights (was ZEROED, which
                                    // shaded coarse tiles flat-lit — the tiled dark-shard gap). Generate
                                    // over the full region then extract the same interior-clamped sub-block
                                    // so it never stomps a resident-Full neighbour's edge. Full re-uploads
                                    // its high-detail normal on upgrade.
                                    if (m_CoarseNormalHf.GetWidth() != tileRegionDim ||
                                        m_CoarseNormalHf.GetHeight() != tileRegionDim)
                                        m_CoarseNormalHf.Resize(tileRegionDim, tileRegionDim, 0.0f);
                                    std::memcpy(m_CoarseNormalHf.GetMutableSamples(),
                                                m_CoarseHeightScratch.data(),
                                                static_cast<size_t>(tileRegionDim) * tileRegionDim * sizeof(float32));
                                    uint32 cnW = 0, cnH = 0;
                                    GenerateNormalmapFromHeightfield(m_CoarseNormalHf, tileSize, tileSize,
                                                                     terrain.HeightScale,
                                                                     m_CoarseNormalScratch, cnW, cnH);
                                    if (m_CoarseNormalScratch.size() ==
                                        static_cast<size_t>(tileRegionDim) * tileRegionDim * 4u)
                                    {
                                        ExtractSubBlock(reinterpret_cast<const uint32*>(m_CoarseNormalScratch.data()),
                                                        tileRegionDim, pr.X0, pr.Z0, pr.Width, pr.Height,
                                                        m_CoarseNormalPatch);
                                        feature->UploadNormalmapRegion(globalGpuHandle,
                                            reinterpret_cast<const uint8*>(m_CoarseNormalPatch.data()),
                                            pr.Width, pr.Height, dstX + pr.X0, dstY + pr.Z0);
                                    }
                                }
                                tile->ClearDirty();
                                tile->SplatmapDirty = false;
                                patched = true;
                            }
                            else
                            {
                                feature->UploadHeightmapRegion(globalGpuHandle,
                                                               tile->Heightfield.GetRawSamples(),
                                                               tile->Heightfield.GetWidth(),
                                                               tile->Heightfield.GetHeight(),
                                                               dstX, dstY);
                                tile->ClearDirty();
                                patched = true;
                            }
                        }
                        if (!coarse && tile->SplatmapDirty && !tile->Splatmap.empty())
                        {
                            feature->UploadSplatmapRegion(globalGpuHandle,
                                                          tile->Splatmap.data(),
                                                          tile->SplatmapWidth, tile->SplatmapHeight,
                                                          dstX, dstY);
                            // Regenerate the tile normal ONLY when a modifier bake changed the heights since
                            // the normal was generated (HeightfieldVersion moved past NormalmapVersion). The
                            // cook bakes a valid normal on the JOB thread and integration sets the versions
                            // equal, so a freshly-streamed Full tile SKIPS this O(tileRes^2) regen (R3: it
                            // must not stall the extraction thread per integration) and just re-uploads the
                            // cook normal. A modifier edit (heights changed) regenerates from the post-modifier
                            // heightfield — the base-noise-normal dark-shard fix, now off the streaming path.
                            if (tile->HeightfieldVersion != tile->NormalmapVersion)
                            {
                                GE_CPU_PROFILE_SCOPE("Terrain.NormalRegen");
                                GenerateNormalmapFromHeightfield(tile->Heightfield, tileSize, tileSize,
                                                                 terrain.HeightScale, tile->Normalmap,
                                                                 tile->NormalmapWidth, tile->NormalmapHeight);
                                tile->NormalmapVersion = tile->HeightfieldVersion;
                            }
                            if (!tile->Normalmap.empty())
                                feature->UploadNormalmapRegion(globalGpuHandle,
                                                               tile->Normalmap.data(),
                                                               tile->NormalmapWidth, tile->NormalmapHeight,
                                                               dstX, dstY);
                            tile->SplatmapDirty = false;
                            patched = true;
                        }

                        if (patched && kTileUploadDebug)
                        {
                            const uint32 uploadedDim = coarse ? tileRegionDim : tile->Heightfield.GetWidth();
                            Logger::Log::Info("Terrain.TileUpload tile=({},{}) rect=({},{},{},{}) lod={}",
                                              coord.X, coord.Z, dstX, dstY,
                                              dstX + uploadedDim, dstY + uploadedDim,
                                              coarse ? "coarse" : "full");
                            // The coarse patch is the tile's first on-screen appearance while
                            // streaming; the modifier bake emits the matching bake=region line.
                            if (coarse)
                                Logger::Log::Info("Terrain.TileStream in tile=({},{}) bake=coarse",
                                                  coord.X, coord.Z);
                        }
                    }

                    // Register the unified textures with the global bindless set once.
                    feature->RegisterHeightmapBindless(globalGpuHandle, *m_RenderServices);
                    feature->RegisterSplatmapBindless(globalGpuHandle, *m_RenderServices);
                    feature->RegisterNormalmapBindless(globalGpuHandle, *m_RenderServices);

                    splatmapBindless = feature->GetSplatmapBindlessIndex(globalGpuHandle);
                    heightmapBindless = feature->GetHeightmapBindlessIndex(globalGpuHandle);
                    normalmapBindless = feature->GetNormalmapBindlessIndex(globalGpuHandle);
                    infoHeightTex = feature->GetInitializedCbtHeightTexture(globalGpuHandle, TerrainCbtHeightTexture::Heightmap);
                    infoSplatTex = feature->GetSplatmapTexture(globalGpuHandle);
                } // end unified-texture path
                else
                {
                    // ---- Phase E resident-window atlas patch ----
                    // The atlas replaces the unified textures for this terrain: the height
                    // source is sized to the streaming window (AtlasDim^2), not the world.
                    // Each slot that was (re)assigned or whose tile content changed is packed
                    // (apron + self-edges; PackTileHeightIntoSlot, the oracle-locked pack) into
                    // a single-slot scratch and uploaded to its atlas rect on the graphics queue
                    // (no transfer queue — C8). The indirection snapshot + geometry ride the
                    // TerrainInstanceInfo to the render thread, which binds them into CBT.
                    AtlasResidencyController* ctrl = m_AtlasByTiled[tiledHandle.Index].get();
                    if (ctrl && ctrl->IsConfigured())
                    {
                        const AtlasGeometry& geo = ctrl->Geometry();
                        feature->EnsureAtlasHeightTexture(globalGpuHandle, geo.AtlasDim);
                        // Splat (RGBA8) + normal (R16G16F) surface atlases share the height slot geometry.
                        feature->EnsureAtlasSplatTexture(globalGpuHandle, geo.AtlasDim);
                        feature->EnsureAtlasNormalTexture(globalGpuHandle, geo.AtlasDim);

                        // Single-slot pack geometry: AtlasDim == SlotStride, so packing tile
                        // into slot 0 yields exactly the slotStride^2 block to upload.
                        const AtlasGeometry slotGeo = MakeAtlasGeometry(geo.TileRes, 1u, 1u, 1u);
                        const size_t slotTexels = static_cast<size_t>(slotGeo.AtlasDim) * slotGeo.AtlasDim;
                        if (m_AtlasSlotScratch.size() != slotTexels)
                            m_AtlasSlotScratch.assign(slotTexels, 0.0f);
                        // RGBA8 splat + R16G16F normal are both 4 bytes/texel.
                        if (m_AtlasSplatSlotScratch.size() != slotTexels * 4u)
                            m_AtlasSplatSlotScratch.assign(slotTexels * 4u, 0);
                        if (m_AtlasNormalSlotScratch.size() != slotTexels * 4u)
                            m_AtlasNormalSlotScratch.assign(slotTexels * 4u, 0);

                        const float32 invTilesX = 1.0f / static_cast<float32>(std::max(1u, geo.TilesPerAxisX));
                        const float32 invTilesZ = 1.0f / static_cast<float32>(std::max(1u, geo.TilesPerAxisZ));
                        const float32 padU = geo.TileRes > 1u ? invTilesX / static_cast<float32>(geo.TileRes - 1u) : 0.0f;
                        const float32 padV = geo.TileRes > 1u ? invTilesZ / static_cast<float32>(geo.TileRes - 1u) : 0.0f;

                        // Set whenever a slot's content was (re)uploaded this frame (assign, LOD
                        // flip, streamed detail, or a sculpt/zone EDIT into a resident tile). The
                        // coarse field is a downsample of the same tile heightfields, so any content
                        // change makes it stale — rebuild it below when this is set (D3: an edit that
                        // only patched a slot must not leave the fallback rendering pre-edit relief).
                        bool anySlotPatched = false;

                        // THE single re-evaluation publish for the atlas path: "the height a bisector
                        // would sample changed over this tile's terrain-UV rect." EVERY height-source
                        // mutation must route through it so CBT re-evaluates the affected bisectors —
                        // (1) slot texel upload (patchSlot), (2) residency row transition (R2),
                        // (3) coarse-field content change (R3: an edit/integration into a non-resident
                        // tile). A path that mutates the source without calling this reopens the
                        // stale-corner seam/sliver class. Feeds the one accumulator
                        // (AccumulateAtlasHeightDirtyUV, +1-texel apron); change-gated per caller so a
                        // parked frame publishes nothing.
                        auto publishHeightSourceChanged = [&](const TileCoord& coord) {
                            const float32 minU = static_cast<float32>(coord.X) * invTilesX - padU;
                            const float32 maxU = static_cast<float32>(coord.X + 1) * invTilesX + padU;
                            const float32 minV = static_cast<float32>(coord.Z) * invTilesZ - padV;
                            const float32 maxV = static_cast<float32>(coord.Z + 1) * invTilesZ + padV;
                            feature->AccumulateAtlasHeightDirtyUV(globalGpuHandle, minU, minV, maxU, maxV);
                        };

                        static const bool kAtlasSurfDebug = std::getenv("GE_TERRAIN_ATLAS_DEBUG") != nullptr;

                        // An axis neighbour's heightfield, but ONLY when it is Full at tileRes so its
                        // boundary samples align exactly with this tile's. Adjacent slots then compute the
                        // shared boundary normal from the SAME samples and agree bit-for-bit — no lighting
                        // seam grid (R2). A coarse / mismatched / absent neighbour passes null (the border
                        // central-difference clamps; the coarse tile is transient and upgrades to Full).
                        auto neighborHf = [&](int32 nx, int32 nz) -> const Terrain::HeightfieldData* {
                            auto it = tiled->Tiles.find(TileCoord{nx, nz});
                            if (it == tiled->Tiles.end() || !it->second)
                                return nullptr;
                            auto* n = it->second.get();
                            if (n->LodState != TileLodState::Full || n->Heightfield.GetWidth() != geo.TileRes)
                                return nullptr;
                            return &n->Heightfield;
                        };

                        // Pack + upload this tile's RGBA8 splat into its slot (upsampled if coarse). A tile
                        // with NO splatmap (shouldn't happen for a cooked tile, but defensive) gets an
                        // EXPLICIT neutral all-layer-0 (grass) slot rather than relying on the create-time
                        // zero-clear — a Resident slot never samples uninitialised VRAM (R5).
                        auto uploadSplatSlot = [&](auto* tp, uint32 slot) {
                            const uint8* splatSrc = nullptr;
                            if (!tp->Splatmap.empty())
                            {
                                if (tp->SplatmapWidth == geo.TileRes && tp->SplatmapHeight == geo.TileRes)
                                    splatSrc = tp->Splatmap.data();
                                else
                                {
                                    UpsampleSplatmapBilinear(tp->Splatmap.data(), tp->SplatmapWidth,
                                                             m_AtlasSplatTileScratch, geo.TileRes);
                                    splatSrc = m_AtlasSplatTileScratch.data();
                                }
                            }
                            if (splatSrc)
                            {
                                PackTileBytesIntoSlot(m_AtlasSplatSlotScratch.data(), slotGeo, 0u, splatSrc, 4u);
                            }
                            else
                            {
                                // Neutral all-grass fill (layer 0 = 255), uniform so the apron is grass too.
                                for (size_t i = 0; i + 3 < m_AtlasSplatSlotScratch.size(); i += 4)
                                {
                                    m_AtlasSplatSlotScratch[i] = 255u;
                                    m_AtlasSplatSlotScratch[i + 1] = 0u;
                                    m_AtlasSplatSlotScratch[i + 2] = 0u;
                                    m_AtlasSplatSlotScratch[i + 3] = 0u;
                                }
                            }
                            feature->UploadAtlasSplatSlot(globalGpuHandle, m_AtlasSplatSlotScratch.data(),
                                                          geo.SlotStride, geo.SlotOriginTexelX(slot),
                                                          geo.SlotOriginTexelY(slot));
                        };

                        // Region (sub-rect) patch of a resident Full slot: a sculpt dab changed only
                        // [x0,x1]x[z0,z1] (tile-local, inclusive, already padded by the filter footprint).
                        // Region-regenerate the PERSISTENT tile normal (neighbour-aware at edges, so a
                        // boundary dab keeps the shared-edge normal bit-equal with the neighbour slot's) and
                        // upload only that sub-rect of height/normal/splat. The pre-change path paid a
                        // whole-tile normal regen + whole-slot (slotStride^2) re-upload PER DAB — the ~6 fps
                        // editing hitch. Settles byte-identical to the whole-slot pack (the same tile bytes
                        // packed into the same slot), locked by RegionPatchSettlesToWholeSlotBitEqual.
                        auto patchSlotRegion = [&](const TileCoord& coord, TerrainTileData& tp, uint32 slot,
                                                   int32 x0, int32 z0, int32 x1, int32 z1) {
                            const float32 tileWorld = tiled->Config.TileWorldSize;
                            const Terrain::HeightfieldData* nL = neighborHf(coord.X - 1, coord.Z);
                            const Terrain::HeightfieldData* nR = neighborHf(coord.X + 1, coord.Z);
                            const Terrain::HeightfieldData* nU = neighborHf(coord.X, coord.Z - 1);
                            const Terrain::HeightfieldData* nD = neighborHf(coord.X, coord.Z + 1);
                            {
                                GE_CPU_PROFILE_SCOPE("Terrain.NormalRegen.Region");
                                GenerateNormalmapRegionFromHeightfield(tp.Heightfield, tileWorld, tileWorld,
                                                                       terrain.HeightScale, tp.Normalmap,
                                                                       x0, z0, x1, z1, nL, nR, nU, nD);
                            }

                            // Height (R32F), normal (R16G16F), splat (RGBA8) are all 4 B/texel; pack each
                            // map's sub-rect (apron-extended at tile edges) and region-upload it. The scratch
                            // is reused between the three — each upload copies into its own staging buffer.
                            const auto* heightBytes =
                                reinterpret_cast<const uint8*>(tp.Heightfield.GetRawSamples());
                            const AtlasSlotUploadRect hRect = PackTileEditRegionIntoSlot(
                                m_AtlasEditScratch, heightBytes, sizeof(float32), geo, slot, x0, z0, x1, z1);
                            if (hRect.Width > 0)
                                feature->UploadAtlasHeightSlotRegion(globalGpuHandle, m_AtlasEditScratch.data(),
                                                                     hRect.Width, hRect.Height,
                                                                     hRect.DstTexelX, hRect.DstTexelY);
                            const AtlasSlotUploadRect nRect = PackTileEditRegionIntoSlot(
                                m_AtlasEditScratch, tp.Normalmap.data(), 4u, geo, slot, x0, z0, x1, z1);
                            if (nRect.Width > 0)
                                feature->UploadAtlasNormalSlotRegion(globalGpuHandle, m_AtlasEditScratch.data(),
                                                                     nRect.Width, nRect.Height,
                                                                     nRect.DstTexelX, nRect.DstTexelY);
                            const AtlasSlotUploadRect sRect = PackTileEditRegionIntoSlot(
                                m_AtlasEditScratch, tp.Splatmap.data(), 4u, geo, slot, x0, z0, x1, z1);
                            if (sRect.Width > 0)
                                feature->UploadAtlasSplatSlotRegion(globalGpuHandle, m_AtlasEditScratch.data(),
                                                                    sRect.Width, sRect.Height,
                                                                    sRect.DstTexelX, sRect.DstTexelY);

                            publishHeightSourceChanged(coord);
                            anySlotPatched = true;
                            tp.NormalmapVersion = tp.HeightfieldVersion;
                            tp.ClearDirty();
                            tp.SplatmapDirty = false;
                            if (kAtlasSurfDebug)
                                Logger::Log::Info("Terrain.AtlasSurface tile=({},{}) slot={} src=region "
                                                  "rect=({},{},{},{})",
                                                  coord.X, coord.Z, slot, x0, z0, x1, z1);
                        };

                        // Full slot patch: height (displaces geometry -> publish for CBT re-eval) +
                        // neighbour-aware normal (from POST-modifier heights) + splat. Clears BOTH dirty
                        // flags (all three sources re-uploaded). wholeSlot forces the whole path (a fresh
                        // slot assign / LOD flip whose entire content is stale); a content edit into an
                        // already-resident Full slot takes the region fast-path above when the dab is a
                        // strict sub-rect.
                        auto patchSlot = [&](const TileCoord& coord, uint32 slot, bool isFull, bool wholeSlot) {
                            auto tileIt = tiled->Tiles.find(coord);
                            if (tileIt == tiled->Tiles.end() || !tileIt->second)
                                return;
                            auto* tp = tileIt->second.get();
                            if (tp->LodState == TileLodState::Empty)
                                return;
                            const uint32 res = geo.TileRes;
                            const bool fullNative = isFull && tp->Heightfield.GetWidth() == res;

                            // Region fast-path: a sculpt dab into an already-resident Full slot only touched
                            // a small rect. Its persistent normal is already seeded (a prior whole patch /
                            // the cook), and the tile's dirty-region journal (the #505 dirty-rect the physics
                            // collider also reads) says exactly which texels changed since the normal was last
                            // generated — regenerate + re-upload only that rect (+ the central-difference /
                            // region-splat footprint). Assign / LOD-flip / non-Full / first-patch tiles fall
                            // through to the whole path, which re-seeds the persistent normal.
                            const bool normalSeeded = tp->Normalmap.size() ==
                                                          static_cast<size_t>(res) * res * 4u &&
                                                      tp->NormalmapWidth == res && tp->NormalmapHeight == res;
                            const bool splatNative = tp->Splatmap.size() ==
                                                         static_cast<size_t>(res) * res * 4u &&
                                                     tp->SplatmapWidth == res && tp->SplatmapHeight == res;
                            if (m_AtlasRegionPatch && !wholeSlot && fullNative && normalSeeded &&
                                splatNative && tp->HeightfieldVersion > tp->NormalmapVersion)
                            {
                                DirtyRegionLog::Region dirty;
                                if (tp->HeightfieldDirtyLog.CollectSince(tp->NormalmapVersion, dirty) &&
                                    !dirty.IsEmpty())
                                {
                                    // Pad the changed-height rect by the filter footprint (#505): the normal
                                    // central difference reaches ±1 sample and the region splat regen ±2.
                                    constexpr int32 kEditRegionPad = 2;
                                    const int32 last = static_cast<int32>(res) - 1;
                                    const int32 x0 = std::max(dirty.MinX - kEditRegionPad, 0);
                                    const int32 z0 = std::max(dirty.MinZ - kEditRegionPad, 0);
                                    const int32 x1 = std::min(dirty.MaxX - 1 + kEditRegionPad, last);
                                    const int32 z1 = std::min(dirty.MaxZ - 1 + kEditRegionPad, last);
                                    const bool wholeTile = x0 == 0 && z0 == 0 && x1 == last && z1 == last;
                                    if (!wholeTile)
                                    {
                                        patchSlotRegion(coord, *tp, slot, x0, z0, x1, z1);
                                        return;
                                    }
                                }
                            }

                            const float32* heights = nullptr;
                            if (fullNative)
                            {
                                heights = tp->Heightfield.GetRawSamples();
                            }
                            else
                            {
                                // Coarse (or mismatched-res) tile: bilinear-upsample to tileRes^2.
                                UpsampleHeightfieldBilinear(tp->Heightfield.GetRawSamples(),
                                                            tp->Heightfield.GetWidth(),
                                                            m_CoarseHeightScratch, geo.TileRes);
                                heights = m_CoarseHeightScratch.data();
                            }
                            PackTileHeightIntoSlot(m_AtlasSlotScratch.data(), slotGeo, 0u, heights,
                                                   isFull, AtlasTileNeighbors{}, AtlasNeighborEdges{});
                            feature->UploadAtlasHeightSlot(globalGpuHandle, m_AtlasSlotScratch.data(),
                                                           geo.SlotStride, geo.SlotOriginTexelX(slot),
                                                           geo.SlotOriginTexelY(slot));

                            // Neighbour-aware normal (R2): a Full tile passes its 4 axis Full neighbours so
                            // adjacent slots' shared boundary texel is bit-equal (no seam); a coarse tile
                            // uses the upsampled heights single-tile (neighbours skipped — transient). A Full
                            // tile's normal is regenerated into its PERSISTENT store (tp->Normalmap) so the
                            // region fast-path above can update it in place next dab; a coarse tile's is
                            // transient (upsample scratch), never persisted.
                            const float32 tileWorld = tiled->Config.TileWorldSize;
                            const Terrain::HeightfieldData* normalHf = nullptr;
                            const Terrain::HeightfieldData* nL = nullptr;
                            const Terrain::HeightfieldData* nR = nullptr;
                            const Terrain::HeightfieldData* nU = nullptr;
                            const Terrain::HeightfieldData* nD = nullptr;
                            if (fullNative)
                            {
                                normalHf = &tp->Heightfield;
                                nL = neighborHf(coord.X - 1, coord.Z);
                                nR = neighborHf(coord.X + 1, coord.Z);
                                nU = neighborHf(coord.X, coord.Z - 1);
                                nD = neighborHf(coord.X, coord.Z + 1);
                            }
                            else
                            {
                                if (m_AtlasNormalTileHf.GetWidth() != geo.TileRes ||
                                    m_AtlasNormalTileHf.GetHeight() != geo.TileRes)
                                    m_AtlasNormalTileHf.Resize(geo.TileRes, geo.TileRes, 0.0f);
                                std::memcpy(m_AtlasNormalTileHf.GetMutableSamples(), heights,
                                            static_cast<size_t>(geo.TileRes) * geo.TileRes * sizeof(float32));
                                normalHf = &m_AtlasNormalTileHf;
                            }
                            uint32 nW = 0, nH = 0;
                            std::vector<uint8>& normalOut =
                                fullNative ? tp->Normalmap : m_AtlasNormalTileScratch;
                            {
                                GE_CPU_PROFILE_SCOPE("Terrain.NormalRegen.Whole");
                                GenerateNormalmapFromHeightfield(*normalHf, tileWorld, tileWorld,
                                                                 terrain.HeightScale, normalOut,
                                                                 nW, nH, nL, nR, nU, nD);
                            }
                            if (fullNative)
                            {
                                tp->NormalmapWidth = nW;
                                tp->NormalmapHeight = nH;
                            }
                            if (normalOut.size() ==
                                static_cast<size_t>(geo.TileRes) * geo.TileRes * 4u)
                            {
                                PackTileBytesIntoSlot(m_AtlasNormalSlotScratch.data(), slotGeo, 0u,
                                                      normalOut.data(), 4u);
                                feature->UploadAtlasNormalSlot(globalGpuHandle, m_AtlasNormalSlotScratch.data(),
                                                               geo.SlotStride, geo.SlotOriginTexelX(slot),
                                                               geo.SlotOriginTexelY(slot));
                            }

                            uploadSplatSlot(tp, slot);

                            if (kAtlasSurfDebug)
                                Logger::Log::Info("Terrain.AtlasSurface tile=({},{}) slot={} "
                                                  "src=height+splat+normal lod={}",
                                                  coord.X, coord.Z, slot, isFull ? "full" : "coarse");

                            publishHeightSourceChanged(coord);
                            anySlotPatched = true;
                            // Only a fullNative pack WROTE the persistent normal (tp->Normalmap); a coarse
                            // pack generated a transient normal (m_AtlasNormalTileScratch) and left the
                            // persistent store untouched. Advancing NormalmapVersion on the coarse branch
                            // would falsely mark that store current — so a later coarse->Full transition
                            // that reaches this tile via the height-dirty route (not the residency LOD-flip
                            // UploadRequest) could take the region fast-path against a persistent normal that
                            // is still coarse. Advance the version only when the store was actually written,
                            // so the invariant "a tile at Full carries a full-res normal in its slot" holds
                            // regardless of which path drives the upgrade (the flat-slot-normal class the
                            // AtlasFullTileNormalIsDetailedWhileCoarseFallbackIsFlat oracle guards). A
                            // fullNative pack makes the region collect (CollectSince) see only the next dab's rect.
                            if (fullNative)
                                tp->NormalmapVersion = tp->HeightfieldVersion;
                            tp->ClearDirty();
                            tp->SplatmapDirty = false; // the full patch re-uploaded splat too
                        };

                        // Splat-only slot patch (R1): an interactive paint edit changed ONLY the splatmap.
                        // Re-upload the slot's splat and nothing else — NO height/normal, and NO
                        // publishHeightSourceChanged (splat is sampled per-pixel fresh; no CBT bisector
                        // cache to invalidate, and no VertexEval re-evaluation). Clears SplatmapDirty.
                        auto patchSlotSplatOnly = [&](const TileCoord& coord, uint32 slot) {
                            auto tileIt = tiled->Tiles.find(coord);
                            if (tileIt == tiled->Tiles.end() || !tileIt->second)
                                return;
                            auto* tp = tileIt->second.get();
                            if (tp->LodState == TileLodState::Empty)
                                return;
                            uploadSplatSlot(tp, slot);
                            if (kAtlasSurfDebug)
                                Logger::Log::Info("Terrain.AtlasSurface tile=({},{}) slot={} src=splat",
                                                  coord.X, coord.Z, slot);
                            tp->SplatmapDirty = false;
                        };

                        // Newly assigned + in-place LOD changes (slot content stale).
                        std::unordered_set<uint64> patched;
                        auto key = [](const TileCoord& c) {
                            return (static_cast<uint64>(static_cast<uint32>(c.X)) << 32) |
                                   static_cast<uint32>(c.Z);
                        };
                        for (const AtlasUploadRequest& req : ctrl->UploadRequestsThisFrame())
                        {
                            // Assign / LOD flip: the whole slot content is stale (fresh atlas location or
                            // a resolution change), so upload the whole slot and re-seed the persistent normal.
                            patchSlot(req.Coord, req.Slot, req.IsFull, /*wholeSlot*/ true);
                            patched.insert(key(req.Coord));
                        }
                        // Content-dirty tiles the residency diff did not already cover: streamed
                        // detail / an edit into a resident slot, OR (R3) an edit or integration into a
                        // CPU-loaded tile that has NO atlas slot and renders through the coarse field.
                        // The modifier bake writes every loaded tile's heightfield immediately
                        // regardless of atlas residency, and the coarse rebuild below folds those
                        // heightfields in — so a non-resident dirty tile's fallback bisectors must be
                        // re-published or they keep pre-edit heights while the resident side drops:
                        // a seam at the residency boundary inside the edit footprint.
                        // Two independent dirty channels: HEIGHT (re-patch height+normal+splat, publish for
                        // CBT re-eval, rebuild coarse height/normal) and SPLAT-only (R1: an interactive
                        // paint bake sets ONLY SplatmapDirty — locked by TiledPaintOnlyEditDoesNotBumpHeight;
                        // it must re-upload the slot's splat + refresh the coarse splat, WITHOUT touching
                        // height/normal or re-evaluating any bisector). Without the splat channel a paint is
                        // invisible on atlas terrains until unrelated churn re-patches the slot.
                        bool coarseContentEdited = false; // a non-resident tile's HEIGHT changed
                        bool splatCoarseEdited = false;   // any tile's SPLAT changed -> coarse splat rebuild
                        for (const auto& [coord, tilePtr] : tiled->Tiles)
                        {
                            if (!tilePtr || tilePtr->LodState == TileLodState::Empty)
                                continue;
                            const bool heightDirty = tilePtr->HeightfieldDirty;
                            const bool splatDirty = tilePtr->SplatmapDirty;
                            if (!heightDirty && !splatDirty)
                                continue;
                            if (patched.count(key(coord)))
                                continue; // the request loop already did a full re-patch (both flags cleared)
                            const AtlasSlotRef ref = ctrl->Pool().Find(coord);
                            switch (ResolveAtlasTileDirtyRoute(heightDirty, splatDirty, ref.IsResident()))
                            {
                            case AtlasTileDirtyRoute::ResidentFull:
                                // Content edit into a resident slot: the region fast-path inside patchSlot
                                // takes over when the dab is a strict sub-rect (the real-time sculpt case).
                                patchSlot(coord, ref.Slot, tilePtr->LodState == TileLodState::Full,
                                          /*wholeSlot*/ false);
                                break;
                            case AtlasTileDirtyRoute::NonResidentHeight:
                                // No slot -> renders through the coarse field. Publish its rect so the
                                // fallback bisectors re-evaluate against the rebuilt coarse, flag both coarse
                                // rebuilds (a height edit re-bakes the procedural splat too), and clear both
                                // flags — this atlas frame consumed the edit. A later assign re-uploads the
                                // current heightfield, so nothing is lost.
                                publishHeightSourceChanged(coord);
                                coarseContentEdited = true;
                                splatCoarseEdited = true;
                                tilePtr->ClearDirty();
                                tilePtr->SplatmapDirty = false;
                                break;
                            case AtlasTileDirtyRoute::ResidentSplatOnly:
                                patchSlotSplatOnly(coord, ref.Slot);
                                splatCoarseEdited = true;
                                break;
                            case AtlasTileDirtyRoute::NonResidentSplatOnly:
                                tilePtr->SplatmapDirty = false; // folded into the coarse splat rebuild
                                splatCoarseEdited = true;
                                break;
                            case AtlasTileDirtyRoute::None:
                                break;
                            }
                        }

                        // Residency transitions flip a tile's effective height source (slot <->
                        // coarse field). The patch loops above only cover tiles that GAINED or
                        // re-uploaded a slot; a tile that LOST its slot (evict-to-fallback / moved
                        // out of window) keeps its bisector corners cached under the OLD slot until
                        // an unrelated split/merge re-flags them — the #505 dirty-rect law lifted to
                        // indirection-row changes. Publish each transitioned tile's UV rect so those
                        // bisectors re-evaluate against the now-current source (the coarse field),
                        // killing the frontier sky-slivers. Edge-triggered in the controller, so a
                        // parked stable window publishes nothing (union is idempotent for the
                        // assign/upgrade tiles the patch loops already flagged).
                        for (const TileCoord& coord : ctrl->RowTransitionsThisFrame())
                            publishHeightSourceChanged(coord);

                        // ---- GPU height bake dispatch (slice-1b, GE_TERRAIN_GPU_BAKE) ----
                        // Mark this terrain GPU-bake-eligible next frame only when the whole terrain
                        // fits the resident window (every tile always resident -> no slot-eviction race
                        // between the modifier system's skip decision and this dispatch). Then drain any
                        // batch the modifier system recorded this frame: resolve each baked tile's atlas
                        // slot and dispatch the SAME height evaluation through the indirection, so the
                        // sampled atlas height is GPU-produced. The CPU bake already wrote the
                        // authoritative heightfield (physics / normal / splat). Publish is already done
                        // by the CPU patch loops above for the same tiles.
                        {
                            const uint32 tilesTotal = geo.TilesPerAxisX * geo.TilesPerAxisZ;
                            tiled->AtlasGpuBakeEligible =
                                IsGpuHeightBakeEnabled() && tilesTotal <= geo.SlotCount;

                            auto& batch = tiled->GpuBakeBatch;
                            if (batch.Pending && !batch.Tiles.empty())
                            {
                                std::vector<GpuHeightBakeDispatch> dispatches;
                                dispatches.reserve(batch.Tiles.size());
                                // Shared by every tile in the bake: the authored row count the
                                // kernel loops over, from the same function that sizes the row
                                // binding in TerrainRenderFeature (see ModifierBindingSizes).
                                const ModifierBindingSizes modSizes =
                                    ComputeModifierBindingSizes(batch.Modifiers.size());
                                for (const auto& rq : batch.Tiles)
                                {
                                    const TileCoord coord{rq.TileX, rq.TileZ};
                                    const AtlasSlotRef ref = ctrl->Pool().Find(coord);
                                    if (!ref.IsResident())
                                        continue; // not resident this frame -> CPU upload covered it
                                    GpuHeightBakeDispatch d{};
                                    TerrainHeightBakePush& p = d.Push;
                                    p.RectMinX = rq.RectMinX;
                                    p.RectMinZ = rq.RectMinZ;
                                    p.RectW = static_cast<uint32>(rq.RectMaxX - rq.RectMinX + 1);
                                    p.RectH = static_cast<uint32>(rq.RectMaxZ - rq.RectMinZ + 1);
                                    p.OriginX = rq.OriginX;
                                    p.OriginZ = rq.OriginZ;
                                    p.TileSize = rq.TileSize;
                                    p.HeightScale = batch.HeightScale;
                                    p.TerrainOriginY = batch.TerrainOriginY;
                                    p.BaseFreq = batch.BaseFreq;
                                    p.BaseAmp = batch.BaseAmp;
                                    p.BaseOctaves = batch.BaseOctaves;
                                    p.BaseSeed = batch.BaseSeed;
                                    p.Slot = ref.Slot;
                                    p.ModifierCount = modSizes.KernelModifierCount;
                                    p.TileRes = geo.TileRes;
                                    p.SlotStride = geo.SlotStride;
                                    p.SlotsPerRow = geo.SlotsPerRow;
                                    d.TileX = rq.TileX;
                                    d.TileZ = rq.TileZ;
                                    dispatches.push_back(d);
                                }
                                if (!dispatches.empty())
                                {
                                    static const bool kGpuBakeDebug =
                                        std::getenv("GE_TERRAIN_GPU_BAKE_DEBUG") != nullptr;
                                    if (kGpuBakeDebug)
                                        Logger::Log::Info(
                                            "Terrain.GpuBake tiles={} mods={} rules={} slot0={}",
                                            dispatches.size(), batch.Modifiers.size(),
                                            batch.SurfaceRules.size(),
                                            dispatches.front().Push.Slot);
                                    feature->QueueGpuHeightBake(globalGpuHandle, batch.Modifiers,
                                                                std::move(dispatches), batch.Settle,
                                                                batch.SplatMinH, batch.SplatMaxH,
                                                                batch.SplatEligible,
                                                                batch.SurfaceRules,
                                                                batch.SurfaceRuleConditions);
                                }
                            }
                            batch.Pending = false;

                            // Settle-readback adopt (slice-1c): apply completed GPU->CPU readbacks
                            // into the CPU heightfield, then MarkRegionDirty + schedule the deferred
                            // splat renormalize — the SAME settle machinery a CPU bake drives, so
                            // physics / normal / splat catch up from the exact texels the GPU
                            // rendered (closes the physics<->render epsilon to bit-level at settle).
                            feature->DrainReadyHeightReadbacks(globalGpuHandle,
                                [&](int32 tileX, int32 tileZ, const float32* samples, uint32 rectW,
                                    uint32 rectH, int32 rectMinX, int32 rectMinZ)
                                {
                                    auto it = tiled->Tiles.find(TileCoord{tileX, tileZ});
                                    if (it == tiled->Tiles.end() || !it->second)
                                        return;
                                    TerrainTileData& tp = *it->second;
                                    const uint32 w = tp.Heightfield.GetWidth();
                                    const uint32 h = tp.Heightfield.GetHeight();
                                    if (w < 2 || h < 2)
                                        return;
                                    for (uint32 j = 0; j < rectH; ++j)
                                    {
                                        const uint32 z = static_cast<uint32>(rectMinZ) + j;
                                        if (z >= h) break;
                                        for (uint32 i = 0; i < rectW; ++i)
                                        {
                                            const uint32 x = static_cast<uint32>(rectMinX) + i;
                                            if (x >= w) break;
                                            tp.Heightfield.SetSample(x, z, samples[j * rectW + i]);
                                        }
                                    }
                                    // Refresh the tile's cached height range (matches the modifier
                                    // system's RefreshTileHeightRange: a plain min/max scan).
                                    float32 lo = 1e30f, hi = -1e30f;
                                    const float32* s = tp.Heightfield.GetRawSamples();
                                    const size_t count = tp.Heightfield.GetSampleCount();
                                    for (size_t i = 0; i < count; ++i)
                                    {
                                        lo = std::min(lo, s[i]);
                                        hi = std::max(hi, s[i]);
                                    }
                                    tp.CachedMinH = lo;
                                    tp.CachedMaxH = hi;
                                    // GPU readback replaced heights outside the modifier
                                    // system's block-grid bookkeeping; invalidate the grid so
                                    // the next region range refresh rebuilds from these heights.
                                    tp.HeightBlockDim = 0;
                                    tp.MarkRegionDirty(rectMinX, rectMinZ,
                                                       rectMinX + static_cast<int32>(rectW),
                                                       rectMinZ + static_cast<int32>(rectH));
                                    tiled->SplatResplatPending = true;
                                    tiled->SplatResplatIdleFrames = 0;
                                    ++tiled->Revision;
                                    tiled->QuadtreeDirty = true;
                                    tiled->GlobalHeightRangeDirty = true;
                                    terrainService->MarkGpuReadbackSplatPending();
                                    static const bool kGpuBakeDebug =
                                        std::getenv("GE_TERRAIN_GPU_BAKE_DEBUG") != nullptr;
                                    if (kGpuBakeDebug)
                                        Logger::Log::Info("Terrain.GpuReadbackAdopt tile=({},{}) "
                                                          "rect=({},{},{},{}) minH={:.4f} maxH={:.4f}",
                                                          tileX, tileZ, rectMinX, rectMinZ, rectW, rectH,
                                                          lo, hi);
                                });
                        }

                        // ---- Out-of-window coarse height field (design §8 Risk 3) ----
                        // The small always-resident downsample an out-of-window tile resolves
                        // through, so the fallback tracks resident relief at the streaming-window
                        // edge (a bounded low-frequency skirt, not a flat cliff / sky-gap; this also
                        // relaxes — not eliminates — the frontier tessellation pressure a flat cliff
                        // drove to max depth). Built by
                        // the shared BuildCoarseHeightField — the SAME terrain-UV -> tile mapping the
                        // atlas resolve uses, so it is continuous with resident tiles by construction
                        // and the frontier oracle runs the exact math. Rebuilt only on the first atlas
                        // frame, a frame that integrated new tile detail, a frame that patched a
                        // resident slot (D3), or a frame that edited a NON-resident tile (R3:
                        // coarseContentEdited — else the fallback renders pre-edit relief). A parked,
                        // unedited camera rebuilds nothing; un-streamed texels keep the whole-terrain
                        // base relief (below), continuous with resident tiles rather than a flat sheet.
                        const uint32 cd = kAtlasCoarseFieldDim;
                        feature->EnsureAtlasCoarseTexture(globalGpuHandle, cd);
                        feature->EnsureAtlasSplatCoarseTexture(globalGpuHandle, cd);
                        feature->EnsureAtlasNormalCoarseTexture(globalGpuHandle, cd);
                        const bool coarseFirst = m_AtlasCoarseBuilt.insert(tiledHandle.Index).second;
                        if (coarseFirst || !integratedTiles.empty() || anySlotPatched ||
                            coarseContentEdited || splatCoarseEdited)
                        {
                            // Whole-terrain base relief from the SAME deterministic world-space source
                            // the tiles use (FillTiledBaseRegion over the terrain's base, as every
                            // streamed tile is filled), so an out-of-window tile that never streamed
                            // resolves to real low-frequency relief continuous with resident tiles —
                            // not the flat 0.5 plane that made a large terrain dissolve into a sheet at
                            // altitude. Streamed tiles still overlay their exact (modifier-inclusive)
                            // height in BuildCoarseHeightField. The field spans the tile grid's extent,
                            // the rect BuildCoarseHeightField maps its texels over (the grid can
                            // overhang the authored size): FillAtlasCoarseBaseField, tested in
                            // TerrainRegionBakeTests.
                            if (m_AtlasCoarseBaseHf.GetWidth() != cd || m_AtlasCoarseBaseHf.GetHeight() != cd)
                                m_AtlasCoarseBaseHf.Resize(cd, cd, kAtlasCoarseFallbackNormalized);
                            FillAtlasCoarseBaseField(m_AtlasCoarseBaseHf, *tiled);
                            BuildCoarseHeightField(
                                cd, tilesX, tilesZ, kAtlasCoarseFallbackNormalized,
                                [tiled](int32 tx, int32 tz) -> CoarseTileSource {
                                    auto it = tiled->Tiles.find(TileCoord{tx, tz});
                                    if (it == tiled->Tiles.end() || !it->second ||
                                        it->second->LodState == TileLodState::Empty)
                                        return {};
                                    const auto& hf = it->second->Heightfield;
                                    return {hf.GetRawSamples(), hf.GetWidth(), hf.GetHeight()};
                                },
                                m_AtlasCoarseScratch, m_AtlasCoarseBaseHf.GetRawSamples());
                            if (!m_AtlasCoarseScratch.empty())
                                feature->UploadAtlasCoarse(globalGpuHandle, m_AtlasCoarseScratch.data(), cd);

                            // Coarse SPLAT: downsample the streamed tile splatmaps via the SAME UV->tile
                            // mapping the atlas resolve uses, so the out-of-window fallback is continuous
                            // with resident splat at the frontier. Un-streamed texels take layer-0 (grass).
                            const uint8 kCoarseSplatFallback[4] = {255u, 0u, 0u, 0u};
                            BuildCoarseFieldU8(
                                cd, tilesX, tilesZ, 4u, kCoarseSplatFallback,
                                [tiled](int32 tx, int32 tz) -> CoarseTileSourceU8 {
                                    auto it = tiled->Tiles.find(TileCoord{tx, tz});
                                    if (it == tiled->Tiles.end() || !it->second ||
                                        it->second->LodState == TileLodState::Empty ||
                                        it->second->Splatmap.empty())
                                        return {};
                                    return {it->second->Splatmap.data(), it->second->SplatmapWidth,
                                            it->second->SplatmapHeight, 4u};
                                },
                                m_AtlasCoarseSplatScratch);
                            if (!m_AtlasCoarseSplatScratch.empty())
                                feature->UploadAtlasSplatCoarse(globalGpuHandle, m_AtlasCoarseSplatScratch.data(), cd);

                            // Coarse NORMAL: derive from the coarse HEIGHT field so distant shading matches
                            // the coarse geometry an out-of-window tile displaces to (no lit bumps on smooth
                            // terrain). worldSize = the whole terrain (extent), heightScale = the terrain's.
                            if (!m_AtlasCoarseScratch.empty())
                            {
                                if (m_AtlasCoarseNormalHf.GetWidth() != cd || m_AtlasCoarseNormalHf.GetHeight() != cd)
                                    m_AtlasCoarseNormalHf.Resize(cd, cd, 0.0f);
                                std::memcpy(m_AtlasCoarseNormalHf.GetMutableSamples(), m_AtlasCoarseScratch.data(),
                                            static_cast<size_t>(cd) * cd * sizeof(float32));
                                uint32 cnW = 0, cnH = 0;
                                GenerateNormalmapFromHeightfield(m_AtlasCoarseNormalHf, extent.WorldSizeX,
                                                                 extent.WorldSizeZ, terrain.HeightScale,
                                                                 m_AtlasCoarseNormalScratch, cnW, cnH);
                                if (!m_AtlasCoarseNormalScratch.empty())
                                    feature->UploadAtlasNormalCoarse(globalGpuHandle,
                                                                     m_AtlasCoarseNormalScratch.data(), cd);
                            }
                        }

                        // Register the four surface atlas textures with the global bindless set (once
                        // per (re)creation) and capture their GE_BTEX indices for the surface params.
                        feature->RegisterAtlasSurfaceBindless(globalGpuHandle, *m_RenderServices);
                        atlasSplatBindless = feature->GetAtlasSplatBindlessIndex(globalGpuHandle);
                        atlasNormalBindless = feature->GetAtlasNormalBindlessIndex(globalGpuHandle);
                        atlasSplatCoarseBindless = feature->GetAtlasSplatCoarseBindlessIndex(globalGpuHandle);
                        atlasNormalCoarseBindless = feature->GetAtlasNormalCoarseBindlessIndex(globalGpuHandle);
                        atlasHeightBindless = feature->GetAtlasHeightBindlessIndex(globalGpuHandle);
                        atlasCoarseBindless = feature->GetAtlasCoarseBindlessIndex(globalGpuHandle);

                        atlasBacked = true;
                        atlasGeo = geo;
                        atlasHeightTex = feature->GetInitializedCbtHeightTexture(globalGpuHandle, TerrainCbtHeightTexture::AtlasHeight);
                        atlasCoarseTex = feature->GetInitializedCbtHeightTexture(globalGpuHandle, TerrainCbtHeightTexture::AtlasCoarse);
                        atlasCoarseDim = cd;
                        atlasTableVersion = ctrl->TableVersion();
                        // The indirection snapshot rides TerrainInstanceInfo to the render thread
                        // EVERY frame (a small vector, KiBs). The render side gates the GPU upload on
                        // a per-ring-slot version so a parked camera writes nothing — parking the CPU
                        // copy too would leave a ring slot that missed the window with no rows to
                        // recover from (F1's dirty reset then re-uploads stale rows). The GPU
                        // quiescence that matters is the render-side gate, not this copy.
                        atlasRows = ctrl->Table().Rows;
                    }
                }

                // ---- ONE TerrainGPUParams + TerrainInstanceInfo (mirrors the single path) ----
                //
                // The tiled terrain now presents a single unified texture set, so its
                // params/instance are shaped exactly like an untiled terrain: CBT binds
                // one heightmap, the surface reads terrains[0].
                //
                // The render extent is the REAL area the unified heightmap covers
                // (extent.WorldSizeX/Z = tilesPerAxis * tileWorldSize), computed above —
                // NOT the power-of-2 square the global quadtree rounds up to, which would
                // stretch a non-square / non-pow2-tile-count terrain. CBT maps
                // world = origin + uv*size and samples the same uv, so size must match it.
                Terrain::TerrainGPUParams params{};
                params.WorldOriginX = worldX;
                params.WorldOriginZ = worldZ;
                params.WorldSizeX = extent.WorldSizeX;
                params.WorldSizeZ = extent.WorldSizeZ;
                params.HeightScale = terrain.HeightScale;
                params.InvHeightmapWidth = 1.0f / static_cast<float32>(unifiedW);
                params.InvHeightmapHeight = 1.0f / static_cast<float32>(unifiedH);
                params.TexelSize = 1.0f / static_cast<float32>(unifiedW - 1);
                params.WorldOriginY = worldXf.matrix[13];
                params.SplatmapBindless = splatmapBindless;
                params.NormalmapBindless = normalmapBindless;
                params.HeightmapBindless = heightmapBindless;
                params.MaterialTiling = terrain.MaterialTiling;
                params.LayerCount = Terrain::kMaxTerrainMaterialLayers;
                AppendTerrainMaterials(params, materialTable, terrain,
                                       feature->MaterialLibraries().Get(
                                           terrain.MaterialLibraryGuid.ToGuid(),
                                           MakeTerrainTextureDeclarer(m_RenderServices)),
                                       m_RenderServices, renderOriginSector);
                params.GridDimHalf = static_cast<float32>(Terrain::kDefaultGridSize) * 0.5f;
                params.Flags = (terrain.ReceiveShadows ? Terrain::kTerrainFlagReceiveShadows : 0u)
                    | (atlasBacked ? Terrain::kTerrainFlagAtlasBacked : 0u)
                    | (feature->GetHeightmapTexture(globalGpuHandle).IsValid()
                           ? Terrain::kTerrainFlagHasHeightmap : 0u)
                    | (feature->GetNormalmapTexture(globalGpuHandle).IsValid()
                           ? Terrain::kTerrainFlagHasNormalmap : 0u)
                    | (feature->GetSplatmapTexture(globalGpuHandle).IsValid()
                           ? Terrain::kTerrainFlagHasSplatmap : 0u);
                params.GrassEnabled = (grass ? Terrain::kTerrainGrassBitEnabled : 0u)
                    | (grass && grass->UseSplatRootColor ? Terrain::kTerrainGrassBitSplatRootColor : 0u)
                    | (grass && grass->TextureGrass ? Terrain::kTerrainGrassBitTextureCard : 0u)
                    | (grass && grass->RenderMode == Components::TerrainGrassRenderMode::Blend
                           ? Terrain::kTerrainGrassBitBlendMode : 0u);
                params.GrassDensity = grass ? std::max(0.0f, grass->BladesPerSquareMeter) : 0.0f;
                params.GrassBladeHeight = grass ? std::max(0.0f, grass->BladeHeight) : 0.0f;
                params.GrassBladeWidth = grass ? std::max(0.0f, grass->BladeWidth) : 0.0f;
                params.GrassMaxWidthRatio = grass ? std::max(0.0f, grass->MaxWidthRatio) : 0.0f;
                params.GrassBladeSegments = grass ? std::clamp(grass->BladeSegments,
                    Components::kMinTerrainGrassBladeSegments,
                    Components::kMaxTerrainGrassBladeSegments) : 5u;
                params.GrassMaskThreshold = grass ? std::clamp(grass->MaskThreshold, 0.0f, 1.0f) : 1.0f;
                params.GrassDensityFalloff = grass ? std::max(0.0f, grass->DensityFalloff) : 0.0f;
                params.GrassRange = grass ? std::max(0.0f, grass->Range) : 0.0f;
                params.GrassPlacementSeed = grass ? grass->PlacementSeed : 0.0f;
                params.GrassWindStrength = grass ? std::max(0.0f, grass->WindStrength) : 0.0f;
                params.GrassLayerIndex = grass ? std::min(grass->LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u) : 0u;
                params.GrassBrightness = grass ? std::max(0.0f, grass->Brightness) : 1.0f;
                params.GrassGroundingStrength = grass ? std::clamp(grass->GroundingStrength, 0.0f, 1.0f) : 0.0f;
                params.GrassRandomScale = grass ? std::max(0.0f, grass->RandomScale) : 0.0f;
                params.GrassRandomBrightness = grass ? std::max(0.0f, grass->RandomBrightness) : 0.0f;
                params.GrassTranslucency = grass ? std::max(0.0f, grass->Translucency) : 0.0f;
                params.GrassRootColor = grass ? grass->RootColor : 0xFF335F1Au;
                params.GrassTipColor = grass ? grass->TipColor : 0xFFB3DB4Du;
                params.GrassBacklightColor = grass ? grass->BacklightColor : 0xFFE6F06Au;
                params.GrassWindDirection = grass ? grass->WindDirection : -2.5f;
                params.GrassWindGustSpeed = grass ? grass->WindGustSpeed : 0.9f;
                params.GrassWindGustScale = grass ? grass->WindGustScale : 0.05f;
                params.GrassWindRestingLean = grass ? grass->WindRestingLean : 0.12f;
                params.GrassWindFlutterAmount = grass ? grass->WindFlutterAmount : 0.16f;
                params.GrassWindFlutterSpeed = grass ? grass->WindFlutterSpeed : 2.2f;
                params.GrassWindSeed = grass ? grass->WindSeed : 3.0f;
                params.GrassClumpSize = grass ? std::max(0.0f, grass->ClumpSize) : 0.0f;
                params.GrassClumpHeightVariance = grass ? std::clamp(grass->ClumpHeightVariance, 0.0f, 1.0f) : 0.0f;
                params.GrassClumpAlignment = grass ? std::clamp(grass->ClumpAlignment, 0.0f, 1.0f) : 0.0f;
                params.GrassClumpGather = grass ? std::clamp(grass->ClumpGather, 0.0f, 1.0f) : 0.0f;
                params.GrassHueVariation = grass ? std::clamp(grass->HueVariation, 0.0f, 1.0f) : 0.0f;
                params.GrassRootShade = grass ? std::clamp(grass->RootShade, 0.0f, 1.0f) : 1.0f;
                params.GrassBladeNormalForm = grass ? std::clamp(grass->BladeNormalForm, 0.0f, 1.0f) : 0.0f;
                params.GrassBladeScatterGain = grass ? std::max(0.0f, grass->BladeScatterGain) : 0.0f;
                params.GrassRootFadeStart = grass ? std::clamp(grass->RootFadeStart, Components::kMinTerrainGrassRootFadeStart, 1.0f - Components::kMinTerrainGrassRootFadeSpan) : 0.0f;
                params.GrassRootFadeEnd = grass ? std::clamp(grass->RootFadeEnd, params.GrassRootFadeStart + Components::kMinTerrainGrassRootFadeSpan, 1.0f) : 1.0f;
                ApplyGrassTextureParams(params, grass, m_RenderServices);
                ClearGrassEnabledIfNothingToPlace(params);

                // Atlas-backed terrains have no unified heightmap (HeightmapBindless == 0). The grass
                // placement compute reads the kTerrainFlagAtlasBacked bit (set above) and routes its
                // height/normal/mask sampling through the resident-window atlas resolve instead — the
                // atlas geometry + bindless height/normal/splat indices + indirection rows ride to the
                // grass feature via TerrainRenderFeature::TryGetAtlasGrassSource. Near-camera blades sit
                // on resident tiles by construction, so they snap at full precision; the coarse fields
                // keep any out-of-window blade height-continuous (never a floater into garbage). Grass
                // is therefore left ENABLED on atlas terrains.
                bool grassControlsActive = false;
                bool grassControlsSuppressed = false;
                if (grass)
                {
                    grassControlsActive = atlasBacked
                        ? m_GrassFieldUploader->UpdateAtlas(*feature, globalGpuHandle, *tiled, atlasGeo, atlasRows)
                        : m_GrassFieldUploader->UpdateUnified(*feature, globalGpuHandle, tiled->GrassUnifiedField);
                    feature->RegisterGrassFieldsBindless(globalGpuHandle, *m_RenderServices);
                    const bool required = atlasBacked ? tiled->GrassRegionsActive : tiled->GrassUnifiedField.IsActive();
                    const bool namedMaps = m_RenderServices->GetProfile().IsCompat();
                    const bool sampleable = atlasBacked
                        ? feature->GrassFieldCanBeSampled(globalGpuHandle, TerrainGrassMap::Atlas)
                          && feature->GrassFieldCanBeSampled(globalGpuHandle, TerrainGrassMap::Coarse)
                          && (namedMaps || (feature->GetGrassFieldBindlessIndex(globalGpuHandle, TerrainGrassMap::Atlas) != 0
                                           && feature->GetGrassFieldBindlessIndex(globalGpuHandle, TerrainGrassMap::Coarse) != 0))
                        : feature->GrassFieldCanBeSampled(globalGpuHandle, TerrainGrassMap::Unified)
                          && (namedMaps || feature->GetGrassFieldBindlessIndex(globalGpuHandle, TerrainGrassMap::Unified) != 0);
                    // A defined white clear is not yet authored content. Keep
                    // pending fields out of placement until upload + publication.
                    if (required && (!grassControlsActive || !sampleable))
                    {
                        params.GrassEnabled &= ~Terrain::kTerrainGrassBitEnabled;
                        grassControlsSuppressed = true;
                        ++m_GrassControlSuppressedTicks;
                    }
                }
                else
                    feature->ReleaseGrassFieldResources(globalGpuHandle);
                if (grassControlsActive && !atlasBacked)
                {
                    params.Flags |= Terrain::kTerrainFlagHasGrassControls;
                    params.GrassControlBindless = feature->GetGrassFieldBindlessIndex(globalGpuHandle, TerrainGrassMap::Unified);
                }
                paramsArray.push_back(params);

                // GE_TERRAIN_GRASS_DEBUG: one line of the tiled terrain's grass-relevant params +
                // resolved bindless indices, so a runtime tiled-grass absence can be pinned to the
                // upstream binding without a capture — the device/bake oracles proved the compact
                // places on these params, so the live question is whether these values are the ones
                // the compute receives (a zero height index roots blades at the base plane -> buried;
                // a zero splat index takes the procedural mask; disabled/zero grass scale -> no draw).
                static const bool kGrassDebug = std::getenv("GE_TERRAIN_GRASS_DEBUG") != nullptr;
                // Throttle to on-change: the grass params are steady per frame, so a per-frame line is
                // pure spam. Key on the fields that actually gate/route grass.
                static uint32 s_LastKey = 0xFFFFFFFFu;
                const uint32 changeKey = params.GrassEnabled ^ (params.HeightmapBindless << 1)
                    ^ (params.SplatmapBindless << 2) ^ (atlasHeightBindless << 3)
                    ^ (static_cast<uint32>(params.WorldSizeX) << 4) ^ (atlasBacked ? 0x40000000u : 0u);
                if (kGrassDebug && changeKey != s_LastKey)
                {
                    s_LastKey = changeKey;
                    LOG_INFO("Terrain.GrassParams tiled atlas={} worldSize=({:.0f},{:.0f}) originY={:.1f} "
                             "heightScale={:.1f} heightBindless={} splatBindless={} normalBindless={} "
                             "grassEnabled={} density={:.2f} bladeH={:.3f} bladeW={:.3f} "
                             "mask={:.2f} layer={} atlasHeightBindless={} atlasSplatBindless={} atlasRows={}",
                             atlasBacked, params.WorldSizeX, params.WorldSizeZ, params.WorldOriginY,
                             params.HeightScale, params.HeightmapBindless, params.SplatmapBindless,
                             params.NormalmapBindless, params.GrassEnabled,
                             params.GrassDensity, params.GrassBladeHeight, params.GrassBladeWidth,
                             params.GrassMaskThreshold, params.GrassLayerIndex,
                             atlasHeightBindless, atlasSplatBindless, static_cast<uint32>(atlasRows.size()));
                }

                TerrainInstanceInfo info{};
                info.WorldOriginX = worldX;
                info.WorldOriginY = worldXf.matrix[13];
                info.WorldOriginZ = worldZ;
                info.SizeX = extent.WorldSizeX;
                info.SizeZ = extent.WorldSizeZ;
                info.HeightScale = terrain.HeightScale;
                info.LODRangeScale = Terrain::kDefaultLODRangeScale;
                info.LODLevels = tiled->GlobalQuadtree.GetNumLevels();
                info.MaterialTiling = terrain.MaterialTiling;
                info.Handle = globalGpuHandle;
                info.HeightmapTexture = infoHeightTex;
                info.HeightmapBindlessIndex = heightmapBindless;
                info.SplatmapTexture = infoSplatTex;
                info.SplatmapBindlessIndex = splatmapBindless;
                info.NormalmapBindlessIndex = normalmapBindless;
                info.IsTiled = true;
                info.CastShadows = terrain.CastShadows;
                // Phase E: the CBT render feature reads these to bind the atlas height texture
                // + indirection SSBO and set the atlas-enabled flag in the frame params.
                info.AtlasBacked = atlasBacked;
                info.AtlasHeightTexture = atlasHeightTex;
                info.AtlasCoarseTexture = atlasCoarseTex;
                info.AtlasCoarseDim = atlasCoarseDim;
                info.AtlasSplatBindlessIndex = atlasSplatBindless;
                info.AtlasNormalBindlessIndex = atlasNormalBindless;
                info.AtlasSplatCoarseBindlessIndex = atlasSplatCoarseBindless;
                info.AtlasNormalCoarseBindlessIndex = atlasNormalCoarseBindless;
                info.AtlasHeightBindlessIndex = atlasHeightBindless;
                info.AtlasCoarseBindlessIndex = atlasCoarseBindless;
                info.GrassRegionsActive = grassControlsActive;
                info.AtlasGrassBindlessIndex = feature->GetGrassFieldBindlessIndex(globalGpuHandle, TerrainGrassMap::Atlas);
                info.AtlasGrassCoarseBindlessIndex = feature->GetGrassFieldBindlessIndex(globalGpuHandle, TerrainGrassMap::Coarse);
                info.GrassControlBindlessIndex = params.GrassControlBindless;
                info.GrassControlsSuppressedPlacement = grassControlsSuppressed;
                info.AtlasDim = atlasGeo.AtlasDim;
                info.AtlasSlotStride = atlasGeo.SlotStride;
                info.AtlasSlotsPerRow = atlasGeo.SlotsPerRow;
                info.AtlasTileRes = atlasGeo.TileRes;
                info.AtlasTilesPerAxisX = atlasGeo.TilesPerAxisX;
                info.AtlasTilesPerAxisZ = atlasGeo.TilesPerAxisZ;
                info.AtlasTableVersion = atlasTableVersion;
                static_assert(sizeof(TileAtlasSlot) == 16, "indirection row must be 16 B");
                info.AtlasRowCount = static_cast<uint32>(atlasRows.size());
                info.AtlasRowBytes.resize(atlasRows.size() * sizeof(TileAtlasSlot));
                if (!atlasRows.empty())
                    std::memcpy(info.AtlasRowBytes.data(), atlasRows.data(),
                                info.AtlasRowBytes.size());
                info.Quadtree = tiled->GlobalQuadtree;
                info.PatchGridSize = Terrain::kDefaultGridSize;
                info.MorphRangesComputed = tiled->MorphRangesComputed;
                if (tiled->MorphRangesComputed)
                {
                    std::memcpy(info.VisRanges, tiled->VisRanges, sizeof(info.VisRanges));
                    std::memcpy(info.MorphStart, tiled->MorphStart, sizeof(info.MorphStart));
                    std::memcpy(info.MorphEnd, tiled->MorphEnd, sizeof(info.MorphEnd));
                }
                activeInfos.push_back(std::move(info));

                // Tile unloading is handled by TileStreamingManager::UnloadDistantTiles.

                return; // Skip single-terrain path below.
            }

            // ---- Single-terrain path (non-tiled) ----
            // If switching from tiled to non-tiled, release the tiled terrain's unified
            // GPU textures (keyed by its global handle) and destroy the tiled data.
            if (terrain.TiledTerrainHandle != 0 || terrain.TiledTerrainGeneration != 0)
            {
                TiledTerrainHandle oldTiled{terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
                if (auto* oldTiledData = terrainService->GetTiledTerrainData(oldTiled))
                {
                    feature->ReleaseTerrainResources(TerrainHandle{
                        oldTiledData->GlobalGpuHandleIndex, oldTiledData->GlobalGpuHandleGeneration});
                }
                ForgetTiledTerrain(oldTiled.Index); // fresh trackers if this index is reused
                terrainService->DestroyTiledTerrain(oldTiled);
                terrain.TiledTerrainHandle = 0;
                terrain.TiledTerrainGeneration = 0;
            }
            TerrainHandle handle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
            auto* data = terrainService->GetTerrainData(handle);

            // Check if structural config changed (requires full terrain recreation).
            if (data)
            {
                const bool structuralChange =
                    data->Config.HeightmapWidth != desired.HeightmapWidth ||
                    data->Config.HeightmapHeight != desired.HeightmapHeight ||
                    data->Config.PatchGridSize != desired.PatchGridSize ||
                    data->Config.LODLevels != desired.LODLevels;

                if (structuralChange)
                {
                    feature->ReleaseTerrainResources(handle);
                    terrainService->DestroyTerrain(handle);
                    terrain.TerrainDataHandle = 0;
                    terrain.TerrainDataGeneration = 0;
                    data = nullptr;
                    handle = {};
                }
            }

            // Check if lightweight config changed (morph ranges need recomputation).
            if (data)
            {
                bool morphDirty = false;
                if (data->Config.WorldSizeX != terrain.SizeX)
                {
                    data->Config.WorldSizeX = terrain.SizeX;
                    morphDirty = true;
                }
                if (data->Config.WorldSizeZ != terrain.SizeZ)
                {
                    data->Config.WorldSizeZ = terrain.SizeZ;
                    morphDirty = true;
                }
                if (data->Config.LODRangeScale != Terrain::kDefaultLODRangeScale)
                {
                    data->Config.LODRangeScale = Terrain::kDefaultLODRangeScale;
                    morphDirty = true;
                }
                if (data->Config.HeightScale != terrain.HeightScale)
                    data->Config.HeightScale = terrain.HeightScale;

                if (morphDirty)
                    data->MorphRangesComputed = false;
            }

            // Create terrain data if it doesn't exist yet.
            if (!data)
            {
                // The handle named nothing, so it is stale: drop it and provision
                // fresh. Refusing to recreate would leave an undone terrain-entity
                // delete permanently terrainless.
                ClearUnresolvedTerrainHandles(terrain);

                const auto newHandle = terrainService->CreateTerrain(desired);
                terrain.TerrainDataHandle = newHandle.Index;
                terrain.TerrainDataGeneration = newHandle.Generation;

                data = terrainService->GetTerrainData(newHandle);
                if (!data)
                    return;

                // Stage-A base fill (design §3.1): noise / imported heightmap
                // / flat, so the heightfield has data for both rendering and
                // physics before any modifier bake. Modifier re-bakes reset
                // through the same helper, keeping both paths per-sample
                // identical.
                std::shared_ptr<const Terrain::HeightfieldData> baseHeightmap;
                if (terrain.BaseSource == Components::TerrainBaseSource::HeightmapAsset
                    && !terrain.TerrainAssetGuid.IsNull())
                {
                    baseHeightmap = terrainService->ResolveHeightmapAsset(terrain.TerrainAssetGuid.ToGuid());
                }
                FillHeightfieldBaseRegion(data->Heightfield, terrain.BaseSource, baseHeightmap.get(),
                                          0, 0,
                                          static_cast<int32>(data->Heightfield.GetWidth()) - 1,
                                          static_cast<int32>(data->Heightfield.GetHeight()) - 1);
                data->MarkFullDirty();
                terrainService->RebuildQuadtree(newHandle);

                // Size the splatmap and commit the bake range. It stays unbaked (reads as
                // channel 0) until the modifier system composites the surface rules over
                // it — a new terrain's materials come from its rules, not from creation.
                data->ResetSplatmapAndCommitRange();

                handle = TerrainHandle{terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
            }

            // Grid mesh is now updated on the render thread (BuildForView)
            // to avoid races between extraction and rendering.

            // Compute and cache morph ranges once (they only depend on config)
            if (!data->MorphRangesComputed)
            {
                const uint32 numLevels = data->Config.LODLevels;
                const uint32 finestNodesPerAxis = data->Quadtree.IsBuilt()
                    ? data->Quadtree.GetNodesPerAxisAtLevel(0) : 1;
                const float32 patchSizeX = data->Config.WorldSizeX / static_cast<float32>(finestNodesPerAxis);
                const float32 patchSizeZ = data->Config.WorldSizeZ / static_cast<float32>(finestNodesPerAxis);
                const float32 baseRange = std::max(patchSizeX, patchSizeZ) * data->Config.LODRangeScale;
                constexpr float32 kMorphStartRatio = 0.66f;

                for (uint32 i = 0; i < numLevels; ++i)
                    data->VisRanges[i] = Terrain::CDLODSelection::ComputeLODRange(i, baseRange, data->Config.LODRangeScale);

                float32 prevMorphStart = 0.0f;
                for (uint32 i = 0; i < numLevels; ++i)
                {
                    data->MorphEnd[i] = data->VisRanges[i];
                    data->MorphStart[i] = prevMorphStart + (data->MorphEnd[i] - prevMorphStart) * kMorphStartRatio;
                    prevMorphStart = data->MorphStart[i];
                }

                data->MorphRangesComputed = true;
            }

            // Upload dirty heightfield data to GPU, consuming this system's
            // own cursor into the dirty-region log (physics keeps its own —
            // nobody clears shared state). When the dirty rect is a sub-region
            // the upload is a horizontal band (full width, rows [minZ,maxZ)):
            // the device sub-region copy writes only those rows and preserves
            // the rest of the texture. The same rect drives the CPU normal regen.
            const uint32 hfWidth = data->Heightfield.GetWidth();
            const uint32 hfHeight = data->Heightfield.GetHeight();
            bool hfWasDirty = false;
            bool hfDirtyIsRegion = false;
            int32 hfDirtyMinX = 0, hfDirtyMinZ = 0, hfDirtyMaxX = 0, hfDirtyMaxZ = 0;
            auto& hfCursor = m_HeightfieldUploadCursors[handle.Index];
            if (hfCursor.Generation != handle.Generation)
                hfCursor = {handle.Generation, 0};
            DirtyRegionLog::Region hfRegion;
            if (reuploadAll)
            {
                hfRegion.MinX = hfRegion.MinZ = 0;
                hfRegion.MaxX = static_cast<int32>(hfWidth);
                hfRegion.MaxZ = static_cast<int32>(hfHeight);
                data->SplatmapDirty = true;
                data->SplatmapFullDirty = true;
            }
            if (reuploadAll || data->HeightfieldDirtyLog.CollectSince(hfCursor.Version, hfRegion))
            {
                hfWasDirty = true;
                hfDirtyMinX = std::max(hfRegion.MinX, 0);
                hfDirtyMinZ = std::max(hfRegion.MinZ, 0);
                hfDirtyMaxX = std::min(hfRegion.MaxX, static_cast<int32>(hfWidth));  // exclusive
                hfDirtyMaxZ = std::min(hfRegion.MaxZ, static_cast<int32>(hfHeight)); // exclusive
                hfDirtyIsRegion = hfDirtyMinX > 0 || hfDirtyMinZ > 0 ||
                                  hfDirtyMaxX < static_cast<int32>(hfWidth) ||
                                  hfDirtyMaxZ < static_cast<int32>(hfHeight);

                TerrainRenderFeature::UploadBand hfBand{};
                if (hfDirtyIsRegion)
                {
                    hfBand.DstRow = static_cast<uint32>(hfDirtyMinZ);
                    hfBand.RowCount = static_cast<uint32>(hfDirtyMaxZ - hfDirtyMinZ);
                }
                feature->UploadHeightmap(handle,
                                          data->Heightfield.GetRawSamples(),
                                          hfWidth,
                                          hfHeight,
                                          hfBand);
                hfCursor.Version = data->HeightfieldVersion;
            }

            // Upload dirty splatmap data to GPU
            if (data->SplatmapDirty && !data->Splatmap.empty())
            {
                // Regenerate the normal map (no neighbors for single terrain).
                // Normals only depend on heights: when this frame's heightfield
                // dirty rect was a sub-region, recompute just that region
                // padded by the central-difference radius; when heights didn't
                // change at all, the existing normalmap is already correct.
                const bool normalmapSized = !data->Normalmap.empty()
                    && data->NormalmapWidth == hfWidth && data->NormalmapHeight == hfHeight;
                const bool normalFullRegen = !normalmapSized || (hfWasDirty && !hfDirtyIsRegion);
                // Region regen implies normalmapSized && hfWasDirty && hfDirtyIsRegion.
                const bool normalRegionRegen = !normalFullRegen && hfWasDirty;
                if (normalFullRegen)
                {
                    GenerateNormalmapFromHeightfield(
                        data->Heightfield, data->Config.WorldSizeX, data->Config.WorldSizeZ,
                        terrain.HeightScale,
                        data->Normalmap, data->NormalmapWidth, data->NormalmapHeight);
                }
                else if (normalRegionRegen)
                {
                    GenerateNormalmapRegionFromHeightfield(
                        data->Heightfield, data->Config.WorldSizeX, data->Config.WorldSizeZ,
                        terrain.HeightScale, data->Normalmap,
                        hfDirtyMinX - 1, hfDirtyMinZ - 1,
                        hfDirtyMaxX, hfDirtyMaxZ); // dirty max is exclusive → +1 pad inclusive
                }

                // Splat has no independent dirty rect. A REGION splat regen is
                // co-located with the height rect, so the band rides it — but a
                // FULL regen (SplatmapFullDirty: global height range shifted,
                // every texel renormalized) must upload full-texture, and
                // paint-only edits (no height rect) upload full too.
                TerrainRenderFeature::UploadBand splatBand{};
                if (!data->SplatmapFullDirty && hfWasDirty && hfDirtyIsRegion &&
                    data->SplatmapWidth == hfWidth && data->SplatmapHeight == hfHeight)
                {
                    splatBand.DstRow = static_cast<uint32>(hfDirtyMinZ);
                    splatBand.RowCount = static_cast<uint32>(hfDirtyMaxZ - hfDirtyMinZ);
                }
                feature->UploadSplatmap(handle,
                                         data->Splatmap.data(),
                                         data->SplatmapWidth,
                                         data->SplatmapHeight,
                                         splatBand);

                if (!data->Normalmap.empty())
                {
                    // Band the upload only when a region regen ran — a full regen
                    // rewrote the whole map (and left dims matching), so re-deriving
                    // the band from dimensions would wrongly skip the outside rows.
                    // Central-difference normals change one row beyond the height
                    // rect on each side; match GenerateNormalmapRegion's padded
                    // footprint so the band covers every regenerated row.
                    TerrainRenderFeature::UploadBand normalBand{};
                    if (normalRegionRegen)
                    {
                        const int32 nMinZ = std::max(hfDirtyMinZ - 1, 0);
                        const int32 nMaxZ = std::min(hfDirtyMaxZ + 1, static_cast<int32>(hfHeight));
                        normalBand.DstRow = static_cast<uint32>(nMinZ);
                        normalBand.RowCount = static_cast<uint32>(nMaxZ - nMinZ);
                    }
                    feature->UploadNormalmap(handle,
                                              data->Normalmap.data(),
                                              data->NormalmapWidth,
                                              data->NormalmapHeight,
                                              normalBand);
                }

                data->SplatmapDirty = false;
                data->SplatmapFullDirty = false;
            }

            // Lazily register textures with the global bindless texture set.
            feature->RegisterHeightmapBindless(handle, *m_RenderServices);
            feature->RegisterSplatmapBindless(handle, *m_RenderServices);
            feature->RegisterNormalmapBindless(handle, *m_RenderServices);

            const uint32 splatmapBindless = feature->GetSplatmapBindlessIndex(handle);
            const uint32 heightmapBindless = feature->GetHeightmapBindlessIndex(handle);
            const uint32 normalmapBindless = feature->GetNormalmapBindlessIndex(handle);

            // Build TerrainGPUParams for this terrain
            Terrain::TerrainGPUParams params{};
            params.WorldOriginX = worldX;
            params.WorldOriginZ = worldZ;
            params.WorldSizeX = terrain.SizeX;
            params.WorldSizeZ = terrain.SizeZ;
            params.HeightScale = terrain.HeightScale;
            params.InvHeightmapWidth = 1.0f / static_cast<float32>(data->Heightfield.GetWidth());
            params.InvHeightmapHeight = 1.0f / static_cast<float32>(data->Heightfield.GetHeight());
            params.TexelSize = 1.0f / static_cast<float32>(data->Heightfield.GetWidth() - 1);
            params.WorldOriginY = worldXf.matrix[13];
            params.SplatmapBindless = splatmapBindless;
            params.NormalmapBindless = normalmapBindless;
            params.HeightmapBindless = heightmapBindless;
            params.MaterialTiling = terrain.MaterialTiling;
            params.LayerCount = Terrain::kMaxTerrainMaterialLayers;
            AppendTerrainMaterials(params, materialTable, terrain,
                                   feature->MaterialLibraries().Get(
                                       terrain.MaterialLibraryGuid.ToGuid(),
                                       MakeTerrainTextureDeclarer(m_RenderServices)),
                                   m_RenderServices, renderOriginSector);
            params.GridDimHalf = static_cast<float32>(data->Config.PatchGridSize) * 0.5f;
            params.Flags = (terrain.ReceiveShadows ? Terrain::kTerrainFlagReceiveShadows : 0u)
                | (feature->GetHeightmapTexture(handle).IsValid()
                       ? Terrain::kTerrainFlagHasHeightmap : 0u)
                | (feature->GetNormalmapTexture(handle).IsValid()
                       ? Terrain::kTerrainFlagHasNormalmap : 0u)
                | (feature->GetSplatmapTexture(handle).IsValid()
                       ? Terrain::kTerrainFlagHasSplatmap : 0u);
            params.GrassEnabled = (grass ? Terrain::kTerrainGrassBitEnabled : 0u)
                | (grass && grass->UseSplatRootColor ? Terrain::kTerrainGrassBitSplatRootColor : 0u)
                | (grass && grass->TextureGrass ? Terrain::kTerrainGrassBitTextureCard : 0u)
                | (grass && grass->RenderMode == Components::TerrainGrassRenderMode::Blend
                       ? Terrain::kTerrainGrassBitBlendMode : 0u);
            params.GrassDensity = grass ? std::max(0.0f, grass->BladesPerSquareMeter) : 0.0f;
            params.GrassBladeHeight = grass ? std::max(0.0f, grass->BladeHeight) : 0.0f;
            params.GrassBladeWidth = grass ? std::max(0.0f, grass->BladeWidth) : 0.0f;
            params.GrassMaxWidthRatio = grass ? std::max(0.0f, grass->MaxWidthRatio) : 0.0f;
            params.GrassBladeSegments = grass ? std::clamp(grass->BladeSegments,
                Components::kMinTerrainGrassBladeSegments,
                Components::kMaxTerrainGrassBladeSegments) : 5u;
            params.GrassMaskThreshold = grass ? std::clamp(grass->MaskThreshold, 0.0f, 1.0f) : 1.0f;
            params.GrassDensityFalloff = grass ? std::max(0.0f, grass->DensityFalloff) : 0.0f;
            params.GrassRange = grass ? std::max(0.0f, grass->Range) : 0.0f;
            params.GrassPlacementSeed = grass ? grass->PlacementSeed : 0.0f;
            params.GrassWindStrength = grass ? std::max(0.0f, grass->WindStrength) : 0.0f;
            params.GrassLayerIndex = grass ? std::min(grass->LayerIndex, Terrain::kMaxTerrainMaterialLayers - 1u) : 0u;
            params.GrassBrightness = grass ? std::max(0.0f, grass->Brightness) : 1.0f;
            params.GrassGroundingStrength = grass ? std::clamp(grass->GroundingStrength, 0.0f, 1.0f) : 0.0f;
            params.GrassRandomScale = grass ? std::max(0.0f, grass->RandomScale) : 0.0f;
            params.GrassRandomBrightness = grass ? std::max(0.0f, grass->RandomBrightness) : 0.0f;
            params.GrassTranslucency = grass ? std::max(0.0f, grass->Translucency) : 0.0f;
            params.GrassRootColor = grass ? grass->RootColor : 0xFF335F1Au;
            params.GrassTipColor = grass ? grass->TipColor : 0xFFB3DB4Du;
            params.GrassBacklightColor = grass ? grass->BacklightColor : 0xFFE6F06Au;
            params.GrassWindDirection = grass ? grass->WindDirection : -2.5f;
            params.GrassWindGustSpeed = grass ? grass->WindGustSpeed : 0.9f;
            params.GrassWindGustScale = grass ? grass->WindGustScale : 0.05f;
            params.GrassWindRestingLean = grass ? grass->WindRestingLean : 0.12f;
            params.GrassWindFlutterAmount = grass ? grass->WindFlutterAmount : 0.16f;
            params.GrassWindFlutterSpeed = grass ? grass->WindFlutterSpeed : 2.2f;
            params.GrassWindSeed = grass ? grass->WindSeed : 3.0f;
            params.GrassClumpSize = grass ? std::max(0.0f, grass->ClumpSize) : 0.0f;
            params.GrassClumpHeightVariance = grass ? std::clamp(grass->ClumpHeightVariance, 0.0f, 1.0f) : 0.0f;
            params.GrassClumpAlignment = grass ? std::clamp(grass->ClumpAlignment, 0.0f, 1.0f) : 0.0f;
            params.GrassClumpGather = grass ? std::clamp(grass->ClumpGather, 0.0f, 1.0f) : 0.0f;
            params.GrassHueVariation = grass ? std::clamp(grass->HueVariation, 0.0f, 1.0f) : 0.0f;
            params.GrassRootShade = grass ? std::clamp(grass->RootShade, 0.0f, 1.0f) : 1.0f;
            params.GrassBladeNormalForm = grass ? std::clamp(grass->BladeNormalForm, 0.0f, 1.0f) : 0.0f;
            params.GrassBladeScatterGain = grass ? std::max(0.0f, grass->BladeScatterGain) : 0.0f;
            params.GrassRootFadeStart = grass ? std::clamp(grass->RootFadeStart, Components::kMinTerrainGrassRootFadeStart, 1.0f - Components::kMinTerrainGrassRootFadeSpan) : 0.0f;
            params.GrassRootFadeEnd = grass ? std::clamp(grass->RootFadeEnd, params.GrassRootFadeStart + Components::kMinTerrainGrassRootFadeSpan, 1.0f) : 1.0f;
            ApplyGrassTextureParams(params, grass, m_RenderServices);
            ClearGrassEnabledIfNothingToPlace(params);
            bool grassControlsSuppressed = false;
            if (grass)
            {
                const bool ready = m_GrassFieldUploader->UpdateUnified(*feature, handle, data->GrassField);
                if (ready) params.Flags |= Terrain::kTerrainFlagHasGrassControls;
                feature->RegisterGrassFieldsBindless(handle, *m_RenderServices);
                params.GrassControlBindless = feature->GetGrassFieldBindlessIndex(handle, TerrainGrassMap::Unified);
                const bool sampleable = feature->GrassFieldCanBeSampled(handle, TerrainGrassMap::Unified)
                    && (m_RenderServices->GetProfile().IsCompat() || params.GrassControlBindless != 0);
                if (data->GrassField.IsActive() && (!ready || !sampleable))
                {
                    params.GrassEnabled &= ~Terrain::kTerrainGrassBitEnabled;
                    grassControlsSuppressed = true;
                    ++m_GrassControlSuppressedTicks;
                }
            }
            else
                feature->ReleaseGrassFieldResources(handle);
            paramsArray.push_back(params);

            // Cache terrain instance info for render node.
            // Snapshot the quadtree and morph ranges so the render thread
            // never accesses TerrainData directly (avoids cross-thread race
            // when the modifier system rebuilds the quadtree).
            TerrainInstanceInfo info{};
            info.WorldOriginX = worldX;
            info.WorldOriginY = worldXf.matrix[13];
            info.WorldOriginZ = worldZ;
            info.SizeX = terrain.SizeX;
            info.SizeZ = terrain.SizeZ;
            info.HeightScale = terrain.HeightScale;
            info.LODRangeScale = Terrain::kDefaultLODRangeScale;
            info.LODLevels = data->Config.LODLevels;
            info.MaterialTiling = terrain.MaterialTiling;
            info.Handle = handle;
            info.CastShadows = terrain.CastShadows;
            info.HeightmapTexture = feature->GetInitializedCbtHeightTexture(handle, TerrainCbtHeightTexture::Heightmap);
            info.HeightmapBindlessIndex = feature->GetHeightmapBindlessIndex(handle);
            info.SplatmapTexture = feature->GetSplatmapTexture(handle);
            info.SplatmapBindlessIndex = splatmapBindless;
            info.NormalmapBindlessIndex = normalmapBindless;
            info.GrassControlBindlessIndex = params.GrassControlBindless;
            info.GrassControlsSuppressedPlacement = grassControlsSuppressed;
            info.GrassRegionsActive = (params.Flags & Terrain::kTerrainFlagHasGrassControls) != 0;
            info.Quadtree = data->Quadtree;
            info.PatchGridSize = data->Config.PatchGridSize;
            info.MorphRangesComputed = data->MorphRangesComputed;
            if (data->MorphRangesComputed)
            {
                std::memcpy(info.VisRanges, data->VisRanges, sizeof(info.VisRanges));
                std::memcpy(info.MorphStart, data->MorphStart, sizeof(info.MorphStart));
                std::memcpy(info.MorphEnd, data->MorphEnd, sizeof(info.MorphEnd));
            }

            activeInfos.push_back(std::move(info));
        });

    // Upload the params array (one entry per terrain) and the material table its LayerRole
    // indices address, in one call so both land on the same ring element. Called even when empty
    // so per-frame counts do not retain stale terrain/grass state.
    feature->UploadTerrainParamsArray(paramsArray.empty() ? nullptr : paramsArray.data(),
                                      static_cast<uint32>(paramsArray.size()),
                                      materialTable.empty() ? nullptr : materialTable.data(),
                                      static_cast<uint32>(materialTable.size()),
                                      deviceFrameIndex);
    // Same published slot as the params, so a grass draw binds the volumes extracted with
    // the params it reads.
    const auto& windVolumes = m_GrassWindVolumes.Extract(world);
    feature->GrassWindVolumes().Upload(*device, feature->GetLastTerrainParamsSlot(),
                                       windVolumes.data(), static_cast<uint32>(windVolumes.size()));

    m_GrassFieldUploader->ForgetRetired(*terrainService);
    {
        GE_CPU_PROFILE_SCOPE("Terrain.HeightPages");
        UpdateHeightPages(*terrainService, *feature, m_RenderServices->EnsureFeature<TerrainHeightPageFeature>(),
                          atlasFrame, deltaTime, std::max(2u, device->GetFramesInFlight()));
    }
    feature->SetActiveTerrains(std::move(activeInfos));
}

} // namespace GameEngine::TerrainECS
