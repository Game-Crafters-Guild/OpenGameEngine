#include "Engine/Rendering/OceanDepthCacheBaker.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "TerrainECS/TerrainService.h"

#include <cstring>
#include <utility>
#include <vector>

namespace GameEngine::Engine::Renderer
{
namespace
{

void SetError(std::string* error, const char* message)
{
    if (error)
        *error = message;
}

void MultiplyMatrices(const float32 a[16], const float32 b[16], float32 out[16])
{
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

struct SceneMeshCandidate
{
    GUID ModelGuid;
    uint32 MeshId = 0;
    uint32 RenderLayerMask = 1u;
    bool IsSkinnedRenderer = false;
    float32 WorldTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
};

void CollectSceneMeshCandidates(ECS::World& world,
                                const OceanSceneDepthBakeOptions& options,
                                std::vector<SceneMeshCandidate>& outCandidates,
                                OceanSceneDepthBakeStats& stats)
{
    auto query = world.Query<ECS::Read<Components::MeshRenderer>,
                             ECS::Read<Components::WorldTransform>,
                             ECS::Optional<Components::OceanMeshDepthContributor>,
                             ECS::Optional<ECS::ComponentDisabled<Components::OceanMeshDepthContributor>>>();
    // The bake option decides whether disabled renderers contribute at all, so
    // it decides whether the query visits them.
    if (options.IncludeDisabledRenderers)
        query.IncludeDisabled();
    query.Each([&](ECS::EntityHandle entity,
                   const Components::MeshRenderer& renderer,
                   const Components::WorldTransform& worldTransform,
                   const Components::OceanMeshDepthContributor* depthContributor,
                   const ECS::ComponentDisabled<Components::OceanMeshDepthContributor>* contributorOff)
        {
            if (options.OnlyOceanMeshDepthContributors)
            {
                // The opt-in above reaches disabled renderers, never a switched-off tag.
                if (!depthContributor || contributorOff)
                {
                    ++stats.SkippedUntagged;
                    return;
                }
                if ((renderer.renderLayerMask & depthContributor->RenderLayerMask) == 0u)
                {
                    ++stats.SkippedLayer;
                    return;
                }
            }
            if ((renderer.renderLayerMask & options.RenderLayerMask) == 0u)
            {
                ++stats.SkippedLayer;
                return;
            }
            if (renderer.modelAssetGuid.IsNull())
            {
                ++stats.SkippedAsset;
                return;
            }

            const bool isSkinned = world.GetComponent<Components::SkinnedMeshRenderer>(entity) != nullptr;
            if (isSkinned && !options.IncludeSkinnedMeshes)
            {
                ++stats.SkippedSkinned;
                return;
            }

            SceneMeshCandidate candidate{};
            candidate.ModelGuid = renderer.modelAssetGuid.ToGuid();
            candidate.MeshId = renderer.meshId;
            candidate.RenderLayerMask = renderer.renderLayerMask;
            candidate.IsSkinnedRenderer = isSkinned;
            std::memcpy(candidate.WorldTransform, worldTransform.matrix, sizeof(candidate.WorldTransform));
            outCandidates.push_back(candidate);
            ++stats.CandidateRenderers;
        });
}

void AppendTerrainHeightfieldSource(
    const Terrain::HeightfieldData& heightfield,
    float32 originX, float32 originY, float32 originZ,
    float32 sizeX, float32 sizeZ, float32 heightScale, uint32 layerMask,
    std::vector<Ocean::OceanDepthCacheHeightfieldSource>& outSources,
    OceanSceneDepthBakeStats& stats)
{
    if (heightfield.IsEmpty() || heightfield.GetWidth() == 0u || heightfield.GetHeight() == 0u ||
        sizeX <= 0.0f || sizeZ <= 0.0f)
    {
        ++stats.SkippedTerrain;
        return;
    }

    Ocean::OceanDepthCacheHeightfieldSource source{};
    source.Samples = heightfield.GetRawSamples();
    source.Width = heightfield.GetWidth();
    source.Height = heightfield.GetHeight();
    source.OriginX = originX;
    source.OriginZ = originZ;
    source.SizeX = sizeX;
    source.SizeZ = sizeZ;
    source.HeightScale = heightScale;
    source.WorldOriginY = originY;
    source.LayerMask = layerMask;
    outSources.push_back(source);
    ++stats.TerrainSources;
}

void CollectSceneTerrainSources(ECS::World& world,
                                const OceanSceneDepthBakeOptions& options,
                                std::vector<Ocean::OceanDepthCacheHeightfieldSource>& outSources,
                                OceanSceneDepthBakeStats& stats)
{
    if (!options.IncludeTerrainHeightfields)
        return;

    auto* terrainService = TerrainECS::TerrainService::TryGet();

    auto query = world.Query<ECS::Read<Components::Terrain>,
                             ECS::Read<Components::WorldTransform>>();
    if (options.IncludeDisabledRenderers)
        query.IncludeDisabled();
    query.Each([&](ECS::EntityHandle,
                   const Components::Terrain& terrain,
                   const Components::WorldTransform& worldTransform)
        {
            if (!terrainService)
            {
                ++stats.SkippedTerrain;
                return;
            }
            if ((terrain.RenderLayerMask & options.RenderLayerMask) == 0u)
            {
                ++stats.SkippedLayer;
                return;
            }

            const float32 originX = worldTransform.matrix[12];
            const float32 originY = worldTransform.matrix[13];
            const float32 originZ = worldTransform.matrix[14];
            const uint32 layerMask = terrain.RenderLayerMask;

            if (terrain.TiledTerrainHandle != 0u || terrain.TiledTerrainGeneration != 0u)
            {
                TerrainECS::TiledTerrainHandle handle{
                    terrain.TiledTerrainHandle, terrain.TiledTerrainGeneration};
                const auto* tiled = terrainService->GetTiledTerrainData(handle);
                if (!tiled)
                {
                    ++stats.SkippedTerrain;
                    return;
                }

                const float32 tileSize = tiled->Config.TileWorldSize;
                for (const auto& [coord, tilePtr] : tiled->Tiles)
                {
                    (void)coord;
                    if (!tilePtr)
                    {
                        ++stats.SkippedTerrain;
                        continue;
                    }
                    AppendTerrainHeightfieldSource(
                        tilePtr->Heightfield,
                        tilePtr->WorldOriginX,
                        originY,
                        tilePtr->WorldOriginZ,
                        tileSize,
                        tileSize,
                        terrain.HeightScale,
                        layerMask,
                        outSources,
                        stats);
                }
                return;
            }

            TerrainECS::TerrainHandle handle{
                terrain.TerrainDataHandle, terrain.TerrainDataGeneration};
            const auto* data = terrainService->GetTerrainData(handle);
            if (!data)
            {
                ++stats.SkippedTerrain;
                return;
            }

            // A single planar terrain is centred on WorldTransform; heightfield
            // samples start at its minimum corner. Sizes are already world units.
            AppendTerrainHeightfieldSource(
                data->Heightfield,
                originX - terrain.SizeX * 0.5f,
                originY,
                originZ - terrain.SizeZ * 0.5f,
                terrain.SizeX,
                terrain.SizeZ,
                terrain.HeightScale,
                layerMask,
                outSources,
                stats);
        });
}

SharedPtr<Asset> GetOrLoadAsset(AssetManager& assetManager,
                                const GUID& guid,
                                bool loadMissing)
{
    SharedPtr<Asset> asset = assetManager.GetAsset(guid);
    if ((!asset || !asset->IsLoaded()) && loadMissing)
        asset = assetManager.LoadAssetAsync(guid).get();
    return asset;
}

} // anonymous namespace

bool BakeOceanDepthCacheFromSceneMeshes(ECS::World& world,
                                        const Ocean::OceanDepthCacheBakeDesc& desc,
                                        const OceanSceneDepthBakeOptions& options,
                                        Ocean::OceanDepthCacheAsset& outCache,
                                        OceanSceneDepthBakeStats* outStats,
                                        std::string* error)
{
    OceanSceneDepthBakeStats stats{};
    std::vector<SceneMeshCandidate> candidates;
    CollectSceneMeshCandidates(world, options, candidates, stats);
    std::vector<Ocean::OceanDepthCacheHeightfieldSource> heightfieldSources;
    CollectSceneTerrainSources(world, options, heightfieldSources, stats);

    std::vector<Ocean::OceanDepthCacheMeshSource> sources;
    sources.reserve(candidates.size());
    std::vector<SharedPtr<Asset>> heldAssets;
    heldAssets.reserve(candidates.size());

    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
    for (const SceneMeshCandidate& candidate : candidates)
    {
        SharedPtr<Asset> asset = GetOrLoadAsset(assetManager, candidate.ModelGuid, options.LoadMissingAssets);
        if (!asset || !asset->IsLoaded() || asset->GetType() != AssetType::Model)
        {
            ++stats.SkippedAsset;
            continue;
        }

        auto* model = static_cast<ModelAsset*>(asset.get());
        if (candidate.MeshId >= model->GetMeshCount())
        {
            ++stats.SkippedMesh;
            continue;
        }

        const Mesh& mesh = model->GetMesh(candidate.MeshId);
        if (mesh.PrimitiveTopology != MeshPrimitiveTopology::Triangles ||
            mesh.Vertices.empty() ||
            mesh.Indices.size() < 3)
        {
            ++stats.SkippedMesh;
            continue;
        }
        if ((candidate.IsSkinnedRenderer || mesh.IsSkinned()) && !options.IncludeSkinnedMeshes)
        {
            ++stats.SkippedSkinned;
            continue;
        }

        Ocean::OceanDepthCacheMeshSource source{};
        source.Positions = reinterpret_cast<const Mathematics::Vector3*>(mesh.Vertices.data());
        source.VertexStride = static_cast<uint32>(sizeof(Vertex));
        source.VertexCount = static_cast<uint32>(mesh.Vertices.size());
        source.Indices = mesh.Indices.data();
        source.IndexCount = static_cast<uint32>(mesh.Indices.size());
        source.UseLocalToWorld = true;
        source.LayerMask = candidate.RenderLayerMask;
        MultiplyMatrices(candidate.WorldTransform, mesh.SourceNodeTransform, source.LocalToWorld);

        sources.push_back(source);
        heldAssets.push_back(std::move(asset));
        ++stats.MeshSources;
        stats.Triangles += static_cast<uint32>(mesh.Indices.size() / 3u);
    }

    std::vector<Ocean::OceanDepthCacheAsset> partialCaches;
    partialCaches.reserve(2);

    if (!sources.empty())
    {
        Ocean::OceanDepthCacheAsset meshCache;
        if (!Ocean::BakeOceanDepthCacheFromMeshes(desc,
                                                  sources.data(),
                                                  static_cast<uint32>(sources.size()),
                                                  options.RenderLayerMask,
                                                  meshCache,
                                                  error))
        {
            if (outStats)
                *outStats = stats;
            return false;
        }
        partialCaches.push_back(std::move(meshCache));
    }

    if (!heightfieldSources.empty())
    {
        Ocean::OceanDepthCacheAsset terrainCache;
        if (!Ocean::BakeOceanDepthCacheFromHeightfields(
                desc,
                heightfieldSources.data(),
                static_cast<uint32>(heightfieldSources.size()),
                options.RenderLayerMask,
                terrainCache,
                error))
        {
            if (outStats)
                *outStats = stats;
            return false;
        }
        partialCaches.push_back(std::move(terrainCache));
    }

    if (outStats)
        *outStats = stats;

    if (partialCaches.empty())
    {
        SetError(error, "scene ocean depth bake found no usable mesh or terrain sources");
        return false;
    }

    if (partialCaches.size() == 1)
    {
        outCache = std::move(partialCaches.front());
        return true;
    }

    return Ocean::ComposeOceanDepthCaches(
        partialCaches.data(), static_cast<uint32>(partialCaches.size()), outCache, error);
}

bool SaveOceanDepthCacheFromSceneMeshes(ECS::World& world,
                                        const Ocean::OceanDepthCacheBakeDesc& desc,
                                        const OceanSceneDepthBakeOptions& options,
                                        const std::filesystem::path& path,
                                        OceanSceneDepthBakeStats* outStats,
                                        std::string* error)
{
    Ocean::OceanDepthCacheAsset cache;
    if (!BakeOceanDepthCacheFromSceneMeshes(world, desc, options, cache, outStats, error))
        return false;
    return cache.SaveBinary(path, error);
}

} // namespace GameEngine::Engine::Renderer
