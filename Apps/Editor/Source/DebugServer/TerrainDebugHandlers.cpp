#include "DebugServer/TerrainDebugHandlers.h"

#include "AssetCore/GUID.h"
#include "CBTTerrain/CBTPoolHealth.h"
#include "CBTTerrain/CBTTreeAudit.h"
#include "CBTTerrainECS/CBTRenderFeature.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "DebugServer/TerrainSplitThresholdReport.h"
#include "DebugServer/TerrainStrokeTarget.h"
#include "Core/Engine.h"
#include "Components/Name.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Terrain/TerrainMaterialRecord.h" // kTerrainLayerRoleNames (channelOrder binding)
#include "ECS/ECSTemplates.h"
#include "ECS/Systems.h"
#include "ECSModules/Rendering/RenderingLoop.h"
#include "EditorApplication.h"
#include "Engine/Rendering/RenderServices.h"
#include "Mathematics/Vector3.h"
#include "SceneView/TerrainBrushTool.h"
#include "SceneViewController.h"
#include "TerrainECS/PlanarHeightQuery.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainRenderPublication.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/Systems/TerrainExtractionSystem.h"
#include "TerrainGrass/TerrainGrassPlacementStats.h"
#include "TerrainGrass/TerrainGrassRenderFeature.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

std::string EntityNameFromWorld(ECS::World* world, ECS::EntityHandle entity)
{
    if (!world || !entity.IsValid())
        return {};

    auto* name = world->GetComponent<Components::Name>(entity);
    if (name)
        return std::string(name->View());
    return {};
}

json HeightStats(const Terrain::HeightfieldData& hf)
{
    if (hf.IsEmpty())
        return json{{"empty", true}};

    float32 minH = std::numeric_limits<float32>::max();
    float32 maxH = std::numeric_limits<float32>::lowest();
    double sum = 0.0;
    const auto* samples = hf.GetRawSamples();
    const size_t count = hf.GetSampleCount();
    for (size_t i = 0; i < count; ++i)
    {
        const float32 h = samples[i];
        minH = std::min(minH, h);
        maxH = std::max(maxH, h);
        sum += static_cast<double>(h);
    }

    return json{
        {"empty", false},
        {"width", hf.GetWidth()},
        {"height", hf.GetHeight()},
        {"sampleCount", count},
        {"min", minH},
        {"max", maxH},
        {"range", maxH - minH},
        {"mean", count > 0 ? sum / static_cast<double>(count) : 0.0}
    };
}

// Running per-channel summary of RGBA8 splat texels. An accumulator rather than a
// one-shot function because a TILED terrain's splat lives in per-tile buffers, and a
// caller asking what the surface looks like wants ONE answer for the terrain, not a
// number per tile (or, as before, nothing at all).
struct SplatmapAccumulator
{
    size_t PixelCount = 0;
    uint32 Sources = 0;
    std::array<uint8, 4> MinC = {255, 255, 255, 255};
    std::array<uint8, 4> MaxC = {0, 0, 0, 0};
    std::array<double, 4> SumC = {0.0, 0.0, 0.0, 0.0};
    std::array<size_t, 4> DominantCount = {0, 0, 0, 0};

    void Add(const std::vector<uint8>& splatmap, uint32 width, uint32 height)
    {
        if (splatmap.empty() || width == 0 || height == 0)
            return;
        const size_t pixels = std::min(static_cast<size_t>(width) * height, splatmap.size() / 4);
        for (size_t i = 0; i < pixels; ++i)
        {
            const uint8* p = &splatmap[i * 4];
            uint32 dominant = 0;
            for (uint32 c = 0; c < 4; ++c)
            {
                MinC[c] = std::min(MinC[c], p[c]);
                MaxC[c] = std::max(MaxC[c], p[c]);
                SumC[c] += static_cast<double>(p[c]) / 255.0;
                if (p[c] > p[dominant])
                    dominant = c;
            }
            ++DominantCount[dominant];
        }
        PixelCount += pixels;
        ++Sources;
    }

    json ToJson() const
    {
        if (PixelCount == 0)
            return json{{"empty", true}};

        json mean = json::array();
        json minValues = json::array();
        json maxValues = json::array();
        json dominant = json::array();
        for (uint32 c = 0; c < 4; ++c)
        {
            minValues.push_back(static_cast<double>(MinC[c]) / 255.0);
            maxValues.push_back(static_cast<double>(MaxC[c]) / 255.0);
            mean.push_back(SumC[c] / static_cast<double>(PixelCount));
            dominant.push_back(DominantCount[c]);
        }

        // Derived from the shipped channel->role binding, never a literal: a caller reading these
        // stats has to know which channel each number belongs to, and a second copy of the order
        // here would silently disagree the moment the binding changes.
        json channelOrder = json::array();
        for (uint32_t role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
            channelOrder.push_back(Terrain::kTerrainLayerRoleNames[role]);

        return json{
            {"empty", false},
            {"pixelCount", PixelCount},
            {"channelOrder", std::move(channelOrder)},
            {"min", std::move(minValues)},
            {"max", std::move(maxValues)},
            {"mean", std::move(mean)},
            {"dominantPixelCount", std::move(dominant)}
        };
    }
};

json SplatmapStats(const std::vector<uint8>& splatmap, uint32 width, uint32 height)
{
    SplatmapAccumulator acc;
    acc.Add(splatmap, width, height);
    json out = acc.ToJson();
    out["width"] = width;
    out["height"] = height;
    return out;
}

// Summed magnitude of a zone's written texels — |offset| for a sculpt payload, weight for
// a paint mask. An effect-side scalar that is 0 for an untouched zone and moves with every
// dab, so a caller can prove texels were written rather than trusting a success code.
double ZoneWrittenMagnitude(const TerrainECS::TerrainZonePayload& payload)
{
    double sum = 0.0;
    for (const float32 v : payload.Offsets)
        sum += std::abs(static_cast<double>(v));
    for (const uint8 v : payload.Mask)
        sum += static_cast<double>(v);
    return sum;
}

// Effect-side proof for a planar stroke: which zone the brush wrote and how its payload
// moved. The before-values only describe the zone the stroke actually landed in — a stroke
// that auto-created one reports a zero baseline (which is what a fresh payload has) rather
// than the previous zone's numbers, so before/after is always a like-for-like comparison.
// Null json when the stroke wrote no zone (nothing was under the target).
json PlanarZoneReport(const Editor::SceneTools::TerrainBrushTool& brush, TerrainECS::TerrainService& svc,
                      const GUID& payloadBefore, uint64 versionBefore, double magnitudeBefore)
{
    const GUID payloadGuid = brush.GetActivePayload();
    if (payloadGuid.IsNull())
        return json{};
    const TerrainECS::TerrainZonePayload* payload = svc.GetZonePayload(payloadGuid);
    if (!payload)
        return json{};
    const bool sameZone = payloadGuid == payloadBefore;
    return json{
        {"entityId", brush.GetActiveZone().id},
        {"payload", payloadGuid.ToString()},
        {"createdThisStroke", !sameZone},
        {"format", payload->IsSculpt() ? "sculptOffsetR32F" : "paintMaskR8"},
        {"width", payload->Width},
        {"height", payload->Height},
        {"dataVersion", {{"before", sameZone ? versionBefore : 0ull},
                         {"after", payload->DataVersion}}},
        {"writtenMagnitude", {{"before", sameZone ? magnitudeBefore : 0.0},
                             {"after", ZoneWrittenMagnitude(*payload)}}},
        {"needsSave", payload->NeedsSave}
    };
}

// One sampled corner of the corner-height audit.
struct CornerHeightError
{
    uint64_t HeapId = 0;
    float X = 0.0f;
    float Z = 0.0f;
    float CachedY = 0.0f;
    float CpuY = 0.0f;
    float Error = 0.0f;
};

bool LargerError(const CornerHeightError& a, const CornerHeightError& b)
{
    return a.Error > b.Error;
}

// Keeps the `capacity` largest errors seen so far, in no order.
void KeepIfAmongWorst(std::vector<CornerHeightError>& worst, size_t capacity, const CornerHeightError& e)
{
    if (worst.size() < capacity)
    {
        worst.push_back(e);
        return;
    }
    const auto smallest = std::min_element(worst.begin(), worst.end(),
                                           [](const CornerHeightError& a, const CornerHeightError& b) {
                                               return a.Error < b.Error;
                                           });
    if (e.Error > smallest->Error)
        *smallest = e;
}

// Cached corner HEIGHTS against the CPU composed heightfield, on a stride of the live VISIBLE
// bisectors (planar, world-corner storage). The topology audit cannot see a corner whose y froze
// while the height texture moved under it (a streamed or re-baked region the dirty-rect channel
// never flagged): that corner draws a floater or a shard, and Classify's frustum test on it can
// drop a facet that is in view. The GPU samples the height texture at the corner UV; the CPU query
// reads the composed heightfield, so the two agree to a lattice cell, and a stale corner is
// metres off.
json CornerHeightAudit(const CBTTerrain::CBTTreeSnapshot& snapshot,
                       const TerrainECS::PlanarHeightQuery& query)
{
    constexpr uint32_t kMaxSamples = 8192u;
    constexpr size_t kWorstRows = 8u;
    constexpr float kStaleOver1M = 1.0f;
    constexpr float kStaleOver2M = 2.0f;
    std::vector<float> errors;
    errors.reserve(kMaxSamples);
    std::vector<CornerHeightError> worst;
    worst.reserve(kWorstRows);
    float worstErr = 0.0f;
    uint32_t overOneMetre = 0, overTwoMetres = 0, unsampled = 0;
    const uint32_t stride = std::max(1u, (snapshot.VisibleCount * 3u + kMaxSamples - 1u) / kMaxSamples);
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < snapshot.VisibleCount && errors.size() < kMaxSamples; ++i)
    {
        const uint32_t slot = snapshot.IndicesVisible[i];
        if (slot >= snapshot.PoolSize || snapshot.HeapIds[slot] == CBTTerrain::kFreeSlotHeapID)
            continue;
        const CBTTerrain::CBTVertexData& v = snapshot.Vertices[slot];
        if (v.DeepTag[0] != 0u)
            continue; // (sector, local) storage, not a world corner
        const float* corners[3] = {v.Corner0, v.Corner1, v.Corner2};
        for (uint32_t k = 0; k < 3u; ++k, ++cursor)
        {
            if (cursor % stride != 0u)
                continue;
            float cpuY = 0.0f;
            if (!query.SampleHeight(corners[k][0], corners[k][2], cpuY))
            {
                ++unsampled;
                continue;
            }
            const float err = std::fabs(corners[k][1] - cpuY);
            errors.push_back(err);
            if (err > kStaleOver1M)
                ++overOneMetre;
            if (err > kStaleOver2M)
                ++overTwoMetres;
            worstErr = std::max(worstErr, err);
            KeepIfAmongWorst(worst, kWorstRows,
                             {snapshot.HeapIds[slot], corners[k][0], corners[k][2], corners[k][1], cpuY, err});
        }
    }
    std::sort(worst.begin(), worst.end(), LargerError);
    json worstRows = json::array();
    for (const CornerHeightError& e : worst)
        worstRows.push_back({{"heapId", e.HeapId}, {"x", e.X}, {"z", e.Z}, {"cachedY", e.CachedY},
                             {"cpuY", e.CpuY}, {"err", e.Error}});
    std::sort(errors.begin(), errors.end());
    const float p99 =
        errors.empty() ? 0.0f : errors[static_cast<size_t>(static_cast<double>(errors.size()) * 0.99)];
    return json{{"sampled", errors.size()}, {"unsampled", unsampled}, {"maxAbsErrM", worstErr},
                {"p99AbsErrM", p99},         {"over1m", overOneMetre},         {"over2m", overTwoMetres},
                {"worst", std::move(worstRows)}};
}

} // namespace

void RegisterTerrainDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    server.RegisterHandler("get_terrain_debug", [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
    {
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        auto* terrainService = TerrainECS::TerrainService::TryGet();
        // The atlas controllers live on the extraction system, which is reachable only through the
        // rendering loop's system manager. Null in a headless or pre-init editor; the residency
        // field then reports null rather than a fabricated zero.
        auto* loop = EngineCore::GetInstance().GetRenderingLoop();
        auto* systems = loop ? loop->GetSystemManager() : nullptr;
        auto* extraction = systems ? systems->GetSystem<TerrainECS::TerrainExtractionSystem>()
                                   : nullptr;
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        auto* feature = rs ? rs->GetFeature<TerrainECS::TerrainRenderFeature>() : nullptr;
        auto* grassFeature = rs ? rs->GetFeature<TerrainGrass::TerrainGrassRenderFeature>() : nullptr;

        const auto activeTerrains = feature ? feature->GetActiveTerrains()
                                            : std::vector<TerrainECS::TerrainInstanceInfo>{};

        json terrains = json::array();
        for (auto entity : world->GetAliveEntitiesSnapshot())
        {
            auto* terrain = world->GetComponent<Components::Terrain>(entity);
            if (!terrain)
                continue;
            const auto* grass = world->GetComponent<Components::TerrainGrass>(entity);

            json item;
            item["entityId"] = entity.id;
            item["name"] = EntityNameFromWorld(world, entity);
            item["component"] = {
                {"enabled", ECS::Entity(world, entity).IsEnabled<Components::Terrain>()},
                {"sizeX", terrain->SizeX},
                {"sizeZ", terrain->SizeZ},
                {"heightScale", terrain->HeightScale},
                {"samplesPerMeter", terrain->SamplesPerMeter},
                {"domain", static_cast<uint32_t>(terrain->Domain)},
                {"planetRadius", terrain->PlanetRadius},
                {"terrainDataHandle", terrain->TerrainDataHandle},
                {"terrainDataGeneration", terrain->TerrainDataGeneration},
                {"tiledTerrainHandle", terrain->TiledTerrainHandle},
                {"tiledTerrainGeneration", terrain->TiledTerrainGeneration}
            };
            if (grass)
            {
                item["grass"] = {
                    {"enabled", ECS::Entity(world, entity).IsEnabled<Components::TerrainGrass>()},
                    {"renderMode",
                     grass->RenderMode == Components::TerrainGrassRenderMode::Blend ? "Blend"
                                                                                    : "Dither"},
                    {"bladesPerSquareMeter", grass->BladesPerSquareMeter},
                    {"range", grass->Range},
                    {"densityFalloff", grass->DensityFalloff},
                    {"placementSeed", grass->PlacementSeed},
                    {"clumpSize", grass->ClumpSize},
                    {"clumpHeightVariance", grass->ClumpHeightVariance},
                    {"clumpAlignment", grass->ClumpAlignment},
                    {"clumpGather", grass->ClumpGather},
                    {"bladeHeight", grass->BladeHeight},
                    {"bladeWidth", grass->BladeWidth},
                    {"maxWidthRatio", grass->MaxWidthRatio},
                    {"bladeSegments", grass->BladeSegments},
                    {"randomScale", grass->RandomScale},
                    {"layerIndex", grass->LayerIndex},
                    {"maskThreshold", grass->MaskThreshold},
                    {"windDirection", grass->WindDirection},
                    {"windGustSpeed", grass->WindGustSpeed},
                    {"windGustScale", grass->WindGustScale},
                    {"windStrength", grass->WindStrength},
                    {"windRestingLean", grass->WindRestingLean},
                    {"windFlutterAmount", grass->WindFlutterAmount},
                    {"windFlutterSpeed", grass->WindFlutterSpeed},
                    {"windSeed", grass->WindSeed},
                    {"brightness", grass->Brightness},
                    {"randomBrightness", grass->RandomBrightness},
                    {"hueVariation", grass->HueVariation},
                    {"rootShade", grass->RootShade},
                    {"rootFadeStart", grass->RootFadeStart},
                    {"rootFadeEnd", grass->RootFadeEnd},
                    {"bladeNormalForm", grass->BladeNormalForm},
                    {"bladeScatterGain", grass->BladeScatterGain},
                    {"groundingStrength", grass->GroundingStrength},
                    {"translucency", grass->Translucency},
                    {"rootColor", grass->RootColor},
                    {"tipColor", grass->TipColor},
                    {"backlightColor", grass->BacklightColor},
                    {"albedoTexture", grass->AlbedoTextureAssetGuid.ToGuid().ToString()},
                    {"alphaTexture", grass->AlphaTextureAssetGuid.ToGuid().ToString()},
                    {"normalTexture", grass->NormalTextureAssetGuid.ToGuid().ToString()},
                    {"atlasColumns", grass->AtlasColumns},
                    {"atlasRows", grass->AtlasRows},
                    {"atlasTileCount", grass->AtlasTileCount},
                    {"alphaCutoff", grass->AlphaCutoff},
                    {"normalStrength", grass->NormalStrength},
                    {"textureGrass", grass->TextureGrass},
                    {"textureCardsPerSquareMeter", grass->TextureCardsPerSquareMeter},
                    {"textureSize", grass->TextureSize},
                    {"useSplatRootColor", grass->UseSplatRootColor}
                };

                // No per-terrain candidate arithmetic here any more: density is authored in
                // blades/m2 at the camera and placement is camera-relative, so a terrain's own
                // size no longer bounds or scales what it grows. What the terrain contributes is
                // its authored request, reported above; what actually got placed is per VIEW and
                // lives in grassPlacement.
            }

            if (terrainService)
            {
                TerrainECS::TerrainHandle handle{terrain->TerrainDataHandle, terrain->TerrainDataGeneration};
                if (const auto* data = terrainService->GetTerrainData(handle))
                {
                    item["terrainData"] = {
                        {"heightfieldVersion", data->HeightfieldVersion},
                        {"heightfieldDirtyLogEntries", data->HeightfieldDirtyLog.EntryCount()},
                        {"splatmapDirty", data->SplatmapDirty},
                        {"splatmapWidth", data->SplatmapWidth},
                        {"splatmapHeight", data->SplatmapHeight},
                        {"splatmap", SplatmapStats(data->Splatmap, data->SplatmapWidth, data->SplatmapHeight)},
                        {"quadtreeBuilt", data->Quadtree.IsBuilt()},
                        {"heightfield", HeightStats(data->Heightfield)}
                    };
                }
                else
                {
                    item["terrainData"] = nullptr;
                }

                TerrainECS::TiledTerrainHandle tiledHandle{terrain->TiledTerrainHandle,
                                                           terrain->TiledTerrainGeneration};
                if (const auto* tiled = terrainService->GetTiledTerrainData(tiledHandle))
                {
                    // Splat summary aggregated over the resident tiles. A tiled terrain keeps its
                    // splat per tile, so without this the one terrain shape the GPU splat bake
                    // actually runs on reported no splat state at all — and "did the surface rules
                    // land?" had no answer short of a screenshot.
                    SplatmapAccumulator splat;
                    uint32 summarizedTiles = 0;
                    for (const auto& [coord, tilePtr] : tiled->Tiles)
                    {
                        if (!tilePtr || tilePtr->LodState != TerrainECS::TileLodState::Full)
                            continue;
                        splat.Add(tilePtr->Splatmap, tilePtr->SplatmapWidth, tilePtr->SplatmapHeight);
                        ++summarizedTiles;
                    }

                    // Atlas residency, from the extraction system's controller for this terrain.
                    // Before this the only evidence of what the resident-slot window was doing was
                    // the Terrain.AtlasMap/AtlasUpload/AtlasEvict log signals, which have to be
                    // grepped out of a running editor's log and are edge-triggered, so a question
                    // as basic as "how many slots are resident right now" had no direct answer.
                    // null means this terrain has no controller — the unified height path — which
                    // is a different answer from an atlas with nothing resident.
                    json atlasResidency = json(nullptr);
                    if (extraction)
                    {
                        TerrainECS::AtlasResidencySnapshot snap;
                        if (extraction->TryGetAtlasResidency(terrain->TiledTerrainHandle, snap))
                        {
                            atlasResidency = {
                                {"slotCount", snap.SlotCount},
                                {"residentCount", snap.ResidentCount},
                                {"tileRes", snap.TileRes},
                                {"tableVersion", snap.TableVersion},
                                // Edge-triggered, reset every atlas update: all zero on a settled
                                // window is the quiescent steady state, not a stalled atlas.
                                {"assignsThisFrame", snap.AssignsThisFrame},
                                {"evictionsThisFrame", snap.EvictionsThisFrame},
                                {"uploadsThisFrame", snap.UploadsThisFrame},
                                {"fallbacksThisFrame", snap.FallbacksThisFrame},
                                {"activeFadesThisFrame", snap.ActiveFadesThisFrame}
                            };
                        }
                    }

                    item["tiledTerrain"] = {
                        {"atlasResidency", std::move(atlasResidency)},
                        {"revision", tiled->Revision},
                        {"tileCount", tiled->Tiles.size()},
                        {"tilesPerAxisX", tiled->Config.TilesPerAxisX},
                        {"tilesPerAxisZ", tiled->Config.TilesPerAxisZ},
                        {"tileWorldSize", tiled->Config.TileWorldSize},
                        {"quadtreeBuilt", tiled->GlobalQuadtree.IsBuilt()},
                        {"quadtreeDirty", tiled->QuadtreeDirty},
                        // Tiles WALKED, which is not tiles that contributed: a Full tile with an
                        // unallocated splat buffer adds nothing, and splat.empty says so.
                        {"splatTilesWalked", summarizedTiles},
                        {"splatTilesContributing", splat.Sources},
                        {"splat", splat.ToJson()}
                    };
                }
                else
                {
                    item["tiledTerrain"] = nullptr;
                }
            }

            json active = json::array();
            TerrainECS::TerrainHandle terrainHandle{terrain->TerrainDataHandle, terrain->TerrainDataGeneration};
            TerrainECS::TerrainHandle tiledGlobalHandle{0, 0};
            if (terrainService)
            {
                TerrainECS::TiledTerrainHandle tiledHandle{terrain->TiledTerrainHandle,
                                                           terrain->TiledTerrainGeneration};
                if (const auto* tiled = terrainService->GetTiledTerrainData(tiledHandle))
                    tiledGlobalHandle = {tiled->GlobalGpuHandleIndex, tiled->GlobalGpuHandleGeneration};
            }

            for (const auto& info : activeTerrains)
            {
                if (info.Handle != terrainHandle && info.Handle != tiledGlobalHandle)
                    continue;

                active.push_back({
                    {"handle", {{"index", info.Handle.Index}, {"generation", info.Handle.Generation}}},
                    {"worldOriginX", info.WorldOriginX},
                    {"worldOriginY", info.WorldOriginY},
                    {"worldOriginZ", info.WorldOriginZ},
                    {"sizeX", info.SizeX},
                    {"sizeZ", info.SizeZ},
                    {"heightScale", info.HeightScale},
                    {"isTiled", info.IsTiled},
                    {"heightmapTextureValid", info.HeightmapTexture.IsValid()},
                    {"heightmapBindlessIndex", info.HeightmapBindlessIndex},
                    {"splatmapTextureValid", info.SplatmapTexture.IsValid()},
                    {"splatmapBindlessIndex", info.SplatmapBindlessIndex},
                    {"normalmapBindlessIndex", info.NormalmapBindlessIndex},
                    // Regional grass controls: whether a control field is live for this terrain,
                    // the bindless slot placement reads it through (0 on an atlas terrain, which
                    // uses the two atlas indices), and whether the admission gate refused to place
                    // grass this tick because that field was not yet uploaded and published.
                    {"grassControlsActive", info.GrassRegionsActive},
                    {"grassControlBindlessIndex", info.GrassControlBindlessIndex},
                    {"atlasGrassBindlessIndex", info.AtlasGrassBindlessIndex},
                    {"atlasGrassCoarseBindlessIndex", info.AtlasGrassCoarseBindlessIndex},
                    {"grassControlsSuppressedPlacement", info.GrassControlsSuppressedPlacement},
                    {"quadtreeBuilt", info.Quadtree.IsBuilt()},
                    {"lodLevels", info.LODLevels},
                    {"morphRangesComputed", info.MorphRangesComputed}
                });
            }
            item["activeRenderInfos"] = std::move(active);

            terrains.push_back(std::move(item));
        }

        // What placement actually PRODUCED, per view. Grass is placed on the GPU
        // from a stochastic candidate grid, so this is the only answer to "are
        // there blades, and how many" that is not a screenshot. Counts are per
        // VIEW: one dispatch covers every terrain into one indirect draw, so they
        // cannot be split per terrain entity.
        json grassPlacement = json::array();
        uint32 grassFrameIndex = 0;
        if (grassFeature)
        {
            auto& stats = grassFeature->PlacementStats();
            stats.Poll(rs->GetDevice());
            grassFrameIndex = stats.FrameIndex();
            for (const auto& row : stats.LatchedPlacements())
            {
                const auto& placement = row.Placed;
                const auto& d = placement.Dispatched;
                grassPlacement.push_back({
                    {"viewId", static_cast<uint32>(row.View)},
                    // READ THIS BEFORE THE COUNTS. A view that stops being declared keeps its last
                    // sample — nothing refreshes it and nothing zeroes it — so placedInstances is
                    // only a statement about now while isCurrent holds.
                    {"isCurrent", row.IsCurrent},
                    {"framesSinceDeclared", row.FramesSinceDeclared},
                    {"lastDeclaredFrameIndex", row.LastDeclaredFrameIndex},
                    // False ⇒ the counts below are defaults, not a measurement: this row exists
                    // only because dropouts were recorded for a view that never resolved a sample.
                    {"hasPlacementSample", row.HasSample},
                    // The node declared no grass at all (grass off, no terrain, or a broken frame),
                    // so a zero here is a measured zero rather than a dropped frame.
                    {"idle", row.Idle},
                    {"placedInstances", placement.PlacedInstances},
                    {"placedLod0", placement.PlacedPerLod[0]},
                    {"placedLod1", placement.PlacedPerLod[1]},
                    // One pool for both LODs: placedLod0 + placedLod1 <= capacity by construction.
                    {"capacity", d.Capacity},
                    // Of the reserved slots, how many produced a blade. The gap is what the splat
                    // mask and the growth floor rejected, standing in the pool as zero-size
                    // instances — the ratio is the pool efficiency on this content.
                    {"acceptedBlades", placement.AcceptedBlades},
                    // Nothing is ever refused: the per-view budget is spent by shortening the
                    // field, so a full pool shows up as a scale below 1 instead of as dropped
                    // blades. 1.0 means the budget cost this view no range at all.
                    {"rangeReducedByBudget", placement.RangeReducedByBudget},
                    {"fittedRangeScale", placement.FittedRangeScale},
                    // GPU truth: slots reserved by the cells that survived the frustum test, after
                    // the GPU re-fit — so it is bounded by capacity by construction and measures
                    // how much of the pool this view spends, not how much it wanted.
                    {"candidatesConsidered", placement.CandidatesConsidered},
                    {"cellsVisible", placement.CellsVisible},
                    // Frames whose placement dispatch bailed out, monotonic per class. staleArgs
                    // is the one no count-shaped detector can find: the indirect args survive from
                    // a full ring ago, so the draw runs at a healthy count against a placement made
                    // for a camera several frames stale. zeroGrass frames simply have no blades.
                    {"zeroGrassFrames", row.ZeroGrassFrames},
                    {"staleArgFrames", row.StaleArgFrames},
                    {"lastDropoutFrameIndex", row.LastDropoutFrameIndex},
                    // What the dispatch was asked for: one workgroup per cell of the camera-relative
                    // window, and the density/range the budget fit settled on.
                    {"cellCount", d.CellCount},
                    {"plannedCandidates", d.PlannedCandidates},
                    {"nearDensity", d.NearDensity},
                    {"range", d.Range},
                    {"terrainParamsCount", d.TerrainParamsCount},
                    {"activeGrassTerrains", d.ActiveGrassTerrains}
                });
            }
        }

        return json{
            {"terrainCount", terrains.size()},
            {"terrainServiceAvailable", terrainService != nullptr},
            {"terrainRenderFeatureAvailable", feature != nullptr},
            {"activeRenderTerrainCount", activeTerrains.size()},
            {"terrains", std::move(terrains)},
            // Absent feature vs present-but-nothing-placed are different answers:
            // false means grass never declared in this process, an empty array
            // means it did and no sample has resolved yet.
            {"grassRenderFeatureAvailable", grassFeature != nullptr},
            // "Now" in the units every per-row frame index is stamped in, so
            // grassFrameIndex - lastDropoutFrameIndex is frames since the last dropout. One step
            // per submitted render-graph frame stream, so a multi-window editor steps it once per
            // window per app frame — it is not an app frame count.
            {"grassFrameIndex", grassFrameIndex},
            // Extraction ticks on which regional grass controls existed but were not yet
            // sampleable, so placement was refused. Monotonic: sample it, change the region,
            // sample again, and the delta is exactly how many ticks the edit blanked grass for.
            {"grassControlSuppressedTicks", extraction ? extraction->GrassControlSuppressedTicks() : 0},
            {"grassPlacement", std::move(grassPlacement)}
        };
    });

    // Streaming-corruption discriminator (E6): force ONE full-pool CBT VertexEval on the
    // next recorded update. On a corrupted parked scene, a heal proves the corruption is
    // stale cached bisector corners (the streaming re-eval bug) rather than texture
    // corruption. No params; no-op-safe when no CBT terrain is live.
    server.RegisterHandler("cbt_force_vertex_eval",
                           [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
    {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        auto* cbt = rs ? rs->GetFeature<CBTTerrainECS::CBTRenderFeature>() : nullptr;
        if (!cbt)
            return Editor::RefuseRequest("no CBTRenderFeature (no active CBT terrain?)");
        cbt->RequestForceVertexEval();
        return json{{"ok", true},
                    {"forced", "gateVertexEval=0 on the next recorded CBT.Update"}};
    });

    // CBT watertightness audit: the whole tree read back (HeapIDs, the CURRENT neighbour
    // records, flags, occupancy, the corner cache, the index streams) and audited on the CPU
    // (CBTTreeAudit) — every live leaf decoded from its HeapID, the domain tiled exactly, every
    // interior edge shared by exactly two leaves, links reciprocal, corner UVs fresh, the draw
    // streams exactly the live and VISIBLE sets. The mechanical answer to "is there a hole",
    // independent of what the GPU cached. A one-shot WaitForIdle readback (~160 MiB at the 1M
    // pool) plus a sort of three edges per leaf: a diagnostic to query at a pose, never per frame.
    server.RegisterHandler("get_cbt_tree_audit",
                           [](const EditorDebugServer::RequestContext& /*ctx*/) -> json
    {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        auto* cbt = rs ? rs->GetFeature<CBTTerrainECS::CBTRenderFeature>() : nullptr;
        if (!cbt)
            return Editor::RefuseRequest("no CBTRenderFeature (no active CBT terrain?)");
        if (!cbt->IsInitialized())
            return Editor::RefuseRequest("CBT feature not yet initialized (no live update)");
        if (cbt->UsesNarrowHeap())
            return Editor::RefuseRequest("the audit reads the wide-heap layout; this device took the narrow arm");

        const CBTTerrain::CBTTreeSnapshot snapshot =
            CBTTerrain::ReadTreeSnapshot(cbt->GetInstance(), true);
        const CBTTerrain::CBTTreeAuditResult audit = CBTTerrain::AuditTree(snapshot);
        json sample = json::array();
        for (uint64_t heapId : audit.SampleBadHeapIds)
            sample.push_back(heapId);

        json heights;
        if (snapshot.DomainMode == CBTTerrain::kDomainPlanar && !snapshot.Vertices.empty())
            if (auto* world = EngineCore::GetInstance().GetPrimaryWorld())
                heights = CornerHeightAudit(snapshot, TerrainECS::ResolvePlanarHeightQuery(*world));
        return json{
            {"heights", std::move(heights)},
            {"domain", snapshot.DomainMode == CBTTerrain::kDomainSpherical ? "spherical" : "planar"},
            {"poolSize", snapshot.PoolSize},
            {"liveCount", audit.LiveCount},
            {"visibleCount", snapshot.VisibleCount},
            {"modifiedCount", snapshot.ModifiedCount},
            {"maxDepth", audit.MaxDepth},
            {"nonConformingEdges", audit.NonConformingEdges},
            {"overlappingLeaves", audit.OverlappingLeaves},
            {"incompleteRoots", audit.IncompleteRoots},
            {"malformedLeaves", audit.MalformedLeaves},
            {"nonReciprocalLinks", audit.NonReciprocalLinks},
            {"staleCornerUVs", audit.StaleCornerUVs},
            {"zombies", audit.Zombies},
            {"streamAllErrors", audit.StreamAllErrors},
            {"streamVisibleErrors", audit.StreamVisibleErrors},
            {"vertexEvalCounter", cbt->GetInstance().ReadVertexEvalCount()},
            {"watertight", audit.Watertight()},
            {"clean", audit.Clean()},
            // The logical tree's signature (sorted live HeapIDs): two captures of one pose agree
            // iff the tessellation is the same, whatever slots it landed in.
            {"treeSignature", CBTTerrain::TreeSignature(snapshot)},
            {"sampleBadHeapIds", std::move(sample)}};
    });

    // CBT tessellation-health report (round-8e confirmation tooling). Answers "is the triangulation
    // converged and correct, or is it saturated/deadlocked?" at any moment. The saturation-deadlock
    // fingerprint is mergeDemand >> mergeServed with liveCount == poolSize: the topology is frozen, so
    // a static-camera edit / TargetPixelError change cannot re-tessellate until the camera moves. Does
    // a self-contained GPU readback (WaitForIdle) — a one-frame hitch when queried, never per-frame.
    server.RegisterHandler("get_terrain_stats",
                           [](const EditorDebugServer::RequestContext& ctx) -> json
    {
        auto* rs = EngineCore::GetInstance().GetRenderServices();
        auto* cbt = rs ? rs->GetFeature<CBTTerrainECS::CBTRenderFeature>() : nullptr;
        if (!cbt)
            return json{{"active", false},
                        {"note", "no CBTRenderFeature — no active CBT terrain in this scene"}};

        const auto& domain = cbt->GetDomainConfig();
        const bool spherical = domain.DomainMode == CBTTerrain::kDomainSpherical;
        json out;
        // `active` reads the ECS component's handles, which are written BEFORE extraction decides
        // whether the terrain can render — so it is true for a terrain that draws nothing. Report
        // it alongside what the renderer was actually handed, and lead with the reconciliation, so
        // no reader mistakes "a terrain exists" for "a terrain is on screen".
        uint32_t liveTerrainCount = 0;
        if (auto* world = EngineCore::GetInstance().GetPrimaryWorld())
            liveTerrainCount = TerrainECS::CountLiveTerrains(*world);
        auto* terrainFeature = rs ? rs->GetFeature<TerrainECS::TerrainRenderFeature>() : nullptr;
        const uint32_t publishedCount =
            terrainFeature ? static_cast<uint32_t>(terrainFeature->GetActiveTerrains().size()) : 0u;
        const TerrainECS::TerrainPublicationState publication =
            TerrainECS::DiagnoseTerrainPublication(liveTerrainCount, publishedCount);
        out["render"] = {
            {"liveTerrainCount", liveTerrainCount},
            {"publishedTerrainCount", publishedCount},
            {"rendering", publication == TerrainECS::TerrainPublicationState::Published},
            {"diagnosis", TerrainECS::TerrainPublicationDescription(publication)}
        };
        out["active"] = cbt->IsActive();
        out["initialized"] = cbt->IsInitialized();
        out["domain"] = spherical ? "spherical" : "planar";
        out["planetRadius"] = domain.PlanetRadius;
        out["reliefAmplitude"] = domain.ReliefAmplitude;
        out["reliefOctaves"] = domain.ReliefOctaves;
        // The authored target and the threshold the kernels applied at the view's render height.
        const json splitThreshold = Editor::DescribeTerrainSplitThreshold(
            cbt->GetTargetPixelError(), cbt->GetAppliedSplitThresholdPx(), cbt->GetAppliedRenderHeightPx());
        out["targetPixelError"] = splitThreshold["targetPixelError"];
        out["splitThresholdPx"] = splitThreshold["splitThresholdPx"];
        out["renderHeightPx"] = splitThreshold["renderHeightPx"];
        out["targetReferenceHeightPx"] = splitThreshold["targetReferenceHeightPx"];
        out["maxDepth"] = cbt->GetClassify().TargetDepth;
        // Which CBT kernel arm this device took, and the subdivision ceiling that arm's
        // representation can carry (maxDepth is derived under it). A device without 64-bit
        // shader integers lands on "narrow" by fallback, which is the attribution for a
        // terrain that refines less than its size would suggest.
        out["heapArm"] = cbt->UsesNarrowHeap() ? "narrow" : "wide";
        out["subdivCeiling"] = CBTTerrainECS::SubdivCapFor(
            spherical ? Components::TerrainDomain::Spherical : Components::TerrainDomain::Planar,
            cbt->GetClassify().DeepDecode != 0u, cbt->UsesNarrowHeap());
        out["nearFieldGate"] = cbt->GetClassify().NearFieldGate != 0u; // the round-8e fix state
        out["editRetess"] = cbt->GetClassify().EditRetessEnabled != 0u;
        // Whether the planar pool-pressure controller is armed (GE_CBT_POOL_PRESSURE). Without it a
        // pressureStep of 0 below reads the same whether the controller is off or simply not engaged.
        out["poolPressure"] = cbt->GetClassify().PoolPressure != 0u;
        out["updateRecordCount"] = static_cast<uint64_t>(cbt->GetUpdateRecordCount());
        // Sphere sculpt store state (persistence verification: a loaded scene's restored
        // sculpt shows version > 0 and needsSave false without any dab).
        if (spherical)
        {
            if (auto* svc = TerrainECS::TerrainService::TryGet())
            {
                std::array<CBTTerrain::SphereAnalyticDab,
                           CBTTerrain::kMaxSphereAnalyticModifiers> transient{};
                out["sculpt"] = {
                    {"version", svc->SphereSculptVersion()},
                    {"virtualDim", svc->GetPlanetSculptGeometry().VirtualDim},
                    {"hasEdits", svc->HasSphereSculptEdits()},
                    {"needsSave", svc->SphereSculptNeedsSave()},
                    // S3 analytic-while-stroking: live transient placements (non-zero only while
                    // a brush stroke is held) — the mid-stroke observability seam.
                    {"transientDabs", svc->CopyPlanetTransientDabs(transient)},
                    // S4 adaptive page levels: pool slots claimed (a level-L page counts 4^L)
                    // and virtual pages hosted above base resolution — the escalation oracle.
                    {"allocatedSlots", svc->SphereSculptAllocatedPages()},
                    {"escalatedPages", svc->SphereSculptEscalatedPages()}
                };
                // Optional composed-height probe: probe_dir [x,y,z] samples the CPU-authoritative
                // composed sculpt (store + analytic modifiers + transient dabs, relief passed 0)
                // at that direction — the direct oracle for "is the held stroke in the composition".
                if (ctx.params.contains("probe_dir") && ctx.params["probe_dir"].is_array() &&
                    ctx.params["probe_dir"].size() == 3)
                {
                    const float px = ctx.params["probe_dir"][0].get<float>();
                    const float py = ctx.params["probe_dir"][1].get<float>();
                    const float pz = ctx.params["probe_dir"][2].get<float>();
                    out["sculpt"]["probeComposed"] = svc->SampleSphereSculptHeight(px, py, pz, 0.0f);
                }
            }
        }

        if (!cbt->IsInitialized())
        {
            out["note"] = "CBT feature not yet initialized (no live update) — GPU stats unavailable";
            return out;
        }

        const CBTTerrain::CBTTessellationStats st = cbt->GetInstance().ReadTessellationStats();
        out["poolSize"] = st.PoolSize;
        out["liveCount"] = st.LiveCount;
        out["occupancy"] = st.PoolSize ? static_cast<double>(st.LiveCount) / st.PoolSize : 0.0;
        out["freeCount"] = st.FreeCount;
        out["splitDemand"] = st.SplitDemand;
        out["splitServed"] = st.SplitServed;
        out["mergeDemand"] = st.MergeDemand;
        out["mergeServed"] = st.MergeServed;
        out["overflowTotal"] = st.OverflowTotal;
        // Since-init count of GPU-written dispatch widths clamped to the pool bound. Non-zero
        // means a corrupted count (sum-tree root / work-queue counter) was produced and caught
        // before it could become an unbounded indirect dispatch — the first-load device-loss
        // discriminator: query this after any CBT-adjacent device loss.
        out["dispatchClampTotal"] = st.DispatchClampTotal;
        // Planar pool-pressure scale: the threshold multiplier the pool can afford (1.0 = at rest).
        out["pressureStep"] = st.PressureStep;
        out["pressureScale"] = std::exp2(static_cast<double>(st.PressureStep) /
                                         CBTTerrain::kPressureStepsPerOctave);
        // Planar off-frustum keep step: 0 keeps the whole off-frustum field at depth; higher steps
        // narrow the band of off-frustum geometry kept while the pool is under pressure.
        out["offFrustumKeepStep"] = st.OffFrustumKeepStep;

        // Interpret the numbers so the answer is legible without knowing the internals.
        // CBTPoolHealth.h owns the classification and the reasoning behind it.
        const CBTTerrain::CBTPoolHealth health = CBTTerrain::DiagnoseCBTPool(st, publishedCount > 0);
        out["rootCount"] = st.RootCount;
        out["saturated"] = health.Saturated;
        out["nearCapacity"] = health.NearCapacity;
        out["mergeStalled"] = health.MergeStalled;
        out["splitsStarved"] = health.SplitsStarved;
        out["inert"] = health.Inert;
        out["diagnosis"] = std::string(CBTTerrain::CBTPoolStateDescription(health.State)) + " " +
                           splitThreshold["summary"].get<std::string>();
        return out;
    });

    // Sculpt stroke driver for BOTH terrain domains, through the EXACT interactive brush
    // path: TerrainBrushTool::ProbeSurfaceAt runs the same picks the viewport ray runs, and
    // TerrainBrushTool::ApplyStrokePhase is the same body OnPointerEvent executes once it
    // has a surface point. A planet routes to the sphere sculpt layer, a planar terrain to
    // the zone brush (auto-creating and growing a TerrainSculptZone exactly as a viewport
    // stroke does) — so an IPC stroke matches a viewport stroke in falloff, dirty-region
    // signal, retess trigger and undo granularity (one stroke = one undo entry) by
    // construction rather than by a parallel implementation.
    //
    // Params:
    //   pos  [x,y,z]      world position of the dab. Planar: the XZ column is probed down
    //                     onto the terrain (the Y you pass is ignored — the stroke lands on
    //                     the surface). Planet: the direction from the world origin.
    //   dir  [x,y,z]      planet surface direction — spherical terrains only.
    //   end_pos / end_dir optional stroke end; `count` dabs interpolate start -> end
    //                     (planar: lerp of positions, planet: slerp of directions)
    //   count             dabs to apply (default 1, max 256)
    //   radius            brush radius, world metres (optional; persists on the tool)
    //   strength          brush strength, metres/dab (optional; persists on the tool)
    //   mode              "raise" | "lower" (optional; persists on the tool)
    //   hold              true = leave the stroke open (mouse still down: the planet's
    //                     transient preview stays live; no undo entry yet). Default false.
    //   release_only      true = no dabs, just release a held stroke (commit + one undo entry)
    //   probe_only        true = resolve `pos`/`dir` onto the surface and report the point
    //                     WITHOUT sculpting. The surface-height query automation otherwise
    //                     has no way to ask: it answers "where is the terrain here", which
    //                     is what a physics contact has to be compared against to tell a
    //                     live heightfield collider from a stale one.
    server.RegisterHandler("terrain_sculpt_dab",
                           [&app](const EditorDebugServer::RequestContext& ctx) -> json
    {
        constexpr int kMaxStrokeDabs = 256;
        using Phase = Editor::SceneTools::TerrainStrokePhase;

        if (app.m_Windows.empty() || !app.m_Windows[0]->scene)
            return Editor::RefuseRequest("No scene view controller");
        auto* brush = dynamic_cast<Editor::SceneTools::TerrainBrushTool*>(
            app.m_Windows[0]->scene->GetRegisteredTool("terrainBrush"));
        if (!brush)
            return Editor::RefuseRequest("No terrain brush tool");
        auto* svc = TerrainECS::TerrainService::TryGet();
        if (!svc)
            return Editor::RefuseRequest("No terrain service");

        Components::Terrain active{};
        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        const bool haveActive = world && CBTTerrainECS::FindActiveTerrain(*world, active);
        if (!haveActive)
            return Editor::RefuseRequest("No active terrain in the scene — create one first "
                                         "(execute_command create_terrain / create_planet_5km)");
        const bool spherical = active.Domain == Components::TerrainDomain::Spherical;

        // Optional brush parameter overrides (same persistence semantics as set_terrain_brush).
        if (ctx.params.contains("radius"))
            brush->SetRadius(ctx.params["radius"].get<float>());
        if (ctx.params.contains("strength"))
            brush->SetStrength(ctx.params["strength"].get<float>());
        if (ctx.params.contains("mode"))
        {
            using Mode = Editor::SceneTools::TerrainBrushMode;
            const std::string m = ctx.params["mode"].get<std::string>();
            if (m == "raise")      brush->SetMode(Mode::Raise);
            else if (m == "lower") brush->SetMode(Mode::Lower);
            else if (m == "paint" && !spherical) brush->SetMode(Mode::Paint);
            else if (m == "paint") return Editor::RefuseRequest("paint is not supported on a planet "
                                                                "(planet sculpt v1 is raise|lower)");
            else return Editor::RefuseRequest("mode must be raise|lower, or paint on a planar terrain");
        }
        if (spherical && brush->GetMode() == Editor::SceneTools::TerrainBrushMode::Paint)
            return Editor::RefuseRequest("brush mode is 'paint' — planet sculpt supports raise|lower only; "
                                         "pass mode:\"raise\" or mode:\"lower\"");

        const bool releaseOnly = ctx.params.value("release_only", false);
        const bool probeOnly = ctx.params.value("probe_only", false);

        std::string err;
        Mathematics::Vector3 startTarget{};
        if (probeOnly)
        {
            if (!Editor::ResolveStrokeTarget(ctx.params, "pos", "dir", spherical, startTarget, err))
                return Editor::RefuseRequest(err.empty() ? "probe_only needs 'pos' (or 'dir' on a planet)"
                                                         : err);
            Mathematics::Vector3 hit{};
            Mathematics::Vector3 normal{};
            if (!brush->ProbeSurfaceAt(startTarget, hit, normal))
                return Editor::RefuseRequest("No terrain surface at the requested target",
                                             json{{"target", {startTarget.x, startTarget.y, startTarget.z}}});
            return json{{"ok", true},
                        {"domain", spherical ? "spherical" : "planar"},
                        {"probeOnly", true},
                        {"point", {hit.x, hit.y, hit.z}},
                        {"normal", {normal.x, normal.y, normal.z}}};
        }
        if (!releaseOnly &&
            !Editor::ResolveStrokeTarget(ctx.params, "pos", "dir", spherical, startTarget, err))
            return Editor::RefuseRequest(err.empty()
                                             ? (spherical ? "Provide 'pos' or 'dir' as a [x,y,z] array"
                                                          : "Provide 'pos' as a [x,y,z] world position")
                                             : err);

        Mathematics::Vector3 endTarget = startTarget;
        const bool hasEnd =
            Editor::ResolveStrokeTarget(ctx.params, "end_pos", "end_dir", spherical, endTarget, err);
        if (!hasEnd && !err.empty())
            return Editor::RefuseRequest(err);

        const int count = ctx.params.value("count", 1);
        if (count < 1 || count > kMaxStrokeDabs)
            return Editor::RefuseRequest("count must be 1..256");

        const uint64 sculptVersionBefore = svc->SphereSculptVersion();
        const GUID zonePayloadBefore = brush->GetActivePayload();
        uint64 zoneVersionBefore = 0;
        double zoneMagnitudeBefore = 0.0;
        if (const auto* p = svc->GetZonePayload(zonePayloadBefore))
        {
            zoneVersionBefore = p->DataVersion;
            zoneMagnitudeBefore = ZoneWrittenMagnitude(*p);
        }

        // One IPC call = one stroke = one undo entry (the same boundary a viewport
        // mouse-down -> mouse-up drives). `hold: true` leaves the stroke OPEN after the dabs
        // (mouse still down); the next call continues the SAME stroke, and a default
        // (non-hold) call or `release_only: true` releases it — everything since the first
        // held call commits + records as ONE undo entry, the viewport-faithful boundary.
        const bool hold = ctx.params.value("hold", false);
        int applied = 0;
        json dabs = json::array();
        if (!releaseOnly)
        {
            for (int i = 0; i < count; ++i)
            {
                const float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
                Mathematics::Vector3 target = startTarget;
                if (hasEnd)
                {
                    target = spherical
                                 ? Editor::SlerpDir(startTarget.Normalize(), endTarget.Normalize(), t)
                                 : startTarget + (endTarget - startTarget) * t;
                }

                Mathematics::Vector3 hit{};
                Mathematics::Vector3 normal{};
                if (!brush->ProbeSurfaceAt(target, hit, normal))
                {
                    // Close whatever this call opened so an aborted stroke cannot leak an
                    // armed capture into the next one.
                    brush->ApplyStrokePhase(Phase::End, hit);
                    return Editor::RefuseRequest(spherical
                                           ? "The planet has no positive radius — nothing to sculpt"
                                           : "No terrain surface under the requested position — the XZ "
                                             "column is outside the active terrain's footprint",
                                                 json{{"target", {target.x, target.y, target.z}},
                                                      {"dabIndex", i},
                                                      {"dabsApplied", applied}});
                }

                const Phase phase = brush->IsStroking() ? Phase::Continue : Phase::Begin;
                if (!brush->ApplyStrokePhase(phase, hit))
                {
                    brush->ApplyStrokePhase(Phase::End, hit);
                    return Editor::RefuseRequest("Brush stroke did not start — the terrain service could "
                                                 "not provide a zone payload for this position",
                                                 json{{"target", {hit.x, hit.y, hit.z}},
                                                      {"dabIndex", i},
                                                      {"dabsApplied", applied}});
                }
                dabs.push_back({hit.x, hit.y, hit.z});
                ++applied;
            }
        }
        const bool undoRecorded = hold ? false : brush->ApplyStrokePhase(Phase::End, Mathematics::Vector3{});

        json out;
        out["ok"] = true;
        out["domain"] = spherical ? "spherical" : "planar";
        out["dabsApplied"] = applied;
        out["dabPoints"] = std::move(dabs);
        out["brush"] = {
            {"mode", brush->GetMode() == Editor::SceneTools::TerrainBrushMode::Lower  ? "lower"
                     : brush->GetMode() == Editor::SceneTools::TerrainBrushMode::Paint ? "paint"
                                                                                       : "raise"},
            {"radius", brush->GetRadius()},
            {"strength", brush->GetStrength()}
        };
        if (spherical)
        {
            // Effect-side proof + sub-texel diagnosis data (the same maths the brush uses).
            const CBTTerrain::SphereSculptGeometry geom = svc->GetPlanetSculptGeometry();
            const float angularRadius =
                active.PlanetRadius > 0.0f ? brush->GetRadius() / active.PlanetRadius : 0.0f;
            out["planet"] = {
                {"radius", active.PlanetRadius},
                {"angularRadius", angularRadius},
                {"virtualDim", geom.VirtualDim},
                {"footprintTexels", TerrainECS::TerrainService::SphereSculptDabFootprintTexels(
                                        angularRadius, geom.VirtualDim)}
            };
            out["sculptVersion"] = {{"before", sculptVersionBefore},
                                    {"after", svc->SphereSculptVersion()}};
        }
        else
        {
            out["zone"] = PlanarZoneReport(*brush, *svc, zonePayloadBefore, zoneVersionBefore,
                                           zoneMagnitudeBefore);
        }
        out["strokeOpen"] = brush->IsStroking();
        out["undo"] = {
            {"recorded", undoRecorded},
            {"entry", undoRecorded ? (spherical ? "Planet Sculpt Stroke" : "Terrain Brush Stroke") : ""},
            {"granularity", hold ? "stroke held open (hold:true) — a later non-hold or "
                                   "release_only call records the whole held stroke as one entry"
                                 : "one entry per stroke (held calls fold into the releasing call)"}
        };
        return out;
    });
}

} // namespace GameEngine
