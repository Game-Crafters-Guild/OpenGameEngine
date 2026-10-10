#include "Assets/HlodBakeDriver.h"

#include "Assets/AssetManager.h"
#include "Assets/HlodCache.h"
#include "Assets/HlodClustering.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/HLODVolume.h"

#include "ECS/Components.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Logger/Logger.h"

#include <optional>
#include <system_error>
#include <string>

namespace GameEngine {
namespace Hlod {

std::filesystem::path ResolveHlodCacheFile(AssetManager& assetManager,
                                           const std::filesystem::path& scenePath) {
    const std::string name = scenePath.stem().string() + ".gehlod";
    if (std::optional<std::filesystem::path> cacheRoot =
            assetManager.GetRegistry().TryGetCacheRoot(scenePath)) {
        return *cacheRoot / "Hlod" / name;
    }
    const std::filesystem::path assetRoot = assetManager.GetAssetRoot();
    if (!assetRoot.empty())
        return assetRoot / ".hlod" / name;
    return {};
}

namespace {

constexpr uint64 kBytesPerMegabyte = 1024ull * 1024ull;

} // namespace

bool ResolveHlodVolumeConfig(GameEngine::ECS::World& world, GridConfig& outConfig) {
    bool found = false;
    world.Query<GameEngine::ECS::Read<GameEngine::Components::HLODVolume>>().Each(
        [&](const GameEngine::Components::HLODVolume& volume) {
            if (found)
                return;
            found = true;
            outConfig.CellSize = volume.CellSize;
            outConfig.MaxInstancingRatio = volume.MaxInstancingRatio;
            outConfig.MinMembers = volume.MinMembers;
            outConfig.VbBudgetBytes = static_cast<uint64>(volume.VBBudgetMB) * kBytesPerMegabyte;
        });
    return found;
}

BakeDriverResult BakeHlodForWorld(GameEngine::ECS::World& world,
                                  const ModelResolver& resolveModel,
                                  const std::filesystem::path& outPath) {
    BakeDriverResult result;

    GridConfig config;
    if (!ResolveHlodVolumeConfig(world, config)) {
        result.Outcome = BakeOutcome::NoVolume;
        return result;
    }

    GatheredMembers gathered = GatherHlodMembers(world, resolveModel);
    result.GatheredMembers = static_cast<uint32>(gathered.Members.size());
    result.SkippedMembers = gathered.SkippedSkinned + gathered.SkippedMorph +
                            gathered.SkippedNoModel + gathered.SkippedNoMaterial +
                            gathered.SkippedNonTriangle + gathered.SkippedDisabled;
    if (gathered.Members.empty()) {
        result.Outcome = BakeOutcome::NoMembers;
        return result;
    }

    HlodBakedScene scene =
        BakeScene(gathered.Members, gathered.Geometry, config, &result.Stats);

    if (!WriteHlodCache(outPath, scene)) {
        result.Outcome = BakeOutcome::WriteFailed;
        return result;
    }

    result.Outcome = BakeOutcome::Wrote;
    return result;
}

bool BakeHlodForScene(AssetManager& assets, GameEngine::ECS::World& world,
                      const std::filesystem::path& scenePath, std::string* outError) {
    const std::filesystem::path outPath = ResolveHlodCacheFile(assets, scenePath);
    if (outPath.empty()) {
        if (outError)
            *outError = "no cache root for scene";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);

    const ModelResolver resolver = [&assets](const GUID& guid) -> const ModelAsset* {
        return dynamic_cast<const ModelAsset*>(assets.GetAsset(guid).get());
    };

    const BakeDriverResult result = BakeHlodForWorld(world, resolver, outPath);
    switch (result.Outcome) {
    case BakeOutcome::Wrote:
        Logger::Log::Info(
            "Bake HLOD: wrote '{}' ({} clusters, {} admitted, {} gathered / {} skipped members, "
            "{} KB proxy VB)",
            outPath.string(), result.Stats.ClusterCount, result.Stats.AdmittedCount,
            result.GatheredMembers, result.SkippedMembers, result.Stats.ProxyVertexBytes / 1024ull);
        return true;
    case BakeOutcome::NoVolume:
        if (outError)
            *outError = "no enabled HLODVolume in the scene";
        return false;
    case BakeOutcome::NoMembers:
        if (outError)
            *outError = "no eligible static members gathered";
        return false;
    case BakeOutcome::WriteFailed:
        if (outError)
            *outError = "atomic write of .gehlod failed";
        return false;
    }
    return false;
}

} // namespace Hlod
} // namespace GameEngine
