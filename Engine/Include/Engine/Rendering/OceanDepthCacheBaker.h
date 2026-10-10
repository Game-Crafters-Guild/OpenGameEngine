#pragma once

#include "Ocean/OceanDepthCacheAsset.h"
#include "Types/Types.h"

#include <filesystem>
#include <string>

namespace GameEngine
{
namespace ECS { class World; }

namespace Engine::Renderer
{

struct OceanSceneDepthBakeOptions
{
    uint32 RenderLayerMask = 0xFFFFFFFFu;
    bool LoadMissingAssets = true;
    bool IncludeDisabledRenderers = false;
    bool IncludeSkinnedMeshes = false;
    bool IncludeTerrainHeightfields = true;
    bool OnlyOceanMeshDepthContributors = false;
};

struct OceanSceneDepthBakeStats
{
    uint32 CandidateRenderers = 0;
    uint32 MeshSources = 0;
    uint32 TerrainSources = 0;
    uint32 Triangles = 0;
    uint32 SkippedLayer = 0;
    uint32 SkippedAsset = 0;
    uint32 SkippedMesh = 0;
    uint32 SkippedSkinned = 0;
    uint32 SkippedTerrain = 0;
    uint32 SkippedUntagged = 0;
};

// Bakes the current scene's MeshRenderer geometry and terrain heightfields into a
// saved ocean depth cache. Sources are filtered by renderLayerMask, transformed by
// WorldTransform, and sampled top-down into Ocean's .oceandepth format.
bool BakeOceanDepthCacheFromSceneMeshes(ECS::World& world,
                                        const Ocean::OceanDepthCacheBakeDesc& desc,
                                        const OceanSceneDepthBakeOptions& options,
                                        Ocean::OceanDepthCacheAsset& outCache,
                                        OceanSceneDepthBakeStats* outStats = nullptr,
                                        std::string* error = nullptr);

bool SaveOceanDepthCacheFromSceneMeshes(ECS::World& world,
                                        const Ocean::OceanDepthCacheBakeDesc& desc,
                                        const OceanSceneDepthBakeOptions& options,
                                        const std::filesystem::path& path,
                                        OceanSceneDepthBakeStats* outStats = nullptr,
                                        std::string* error = nullptr);

} // namespace Engine::Renderer
} // namespace GameEngine
