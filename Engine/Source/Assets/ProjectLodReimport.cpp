#include "Assets/ProjectLodReimport.h"

#include "AssetCore/AssetEvents.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/LodAssetSettings.h"
#include "Assets/ModelAsset.h"
#include "Logger/Logger.h"

namespace GameEngine {

ProjectLodReimportResult ReimportProjectLods(AssetManager& assets)
{
    const Vector<GUID> models = assets.GetRegistry().GetAssetsByType(AssetType::Model);
    Logger::Log::Info("Reimport LODs: scanning {} model asset(s)", models.size());

    ProjectLodReimportResult result;
    for (const GUID& guid : models)
    {
        AssetMetadata meta;
        if (!assets.GetRegistry().TryGetAssetMetadata(guid, meta))
        {
            ++result.Skipped;
            continue;
        }

        const ResolvedLodSettings resolved = ResolveLodSettings(&assets, meta.Path);
        if (!resolved.Generate)
        {
            ++result.Skipped;
            continue;
        }

        SharedPtr<Asset> asset = assets.LoadAssetAsync(guid, AssetLoadPriority::High).get();
        auto* model = dynamic_cast<ModelAsset*>(asset.get());
        if (!model || !model->IsLoaded())
        {
            Logger::Log::Warning("Reimport LODs: '{}' could not be loaded, skipping",
                                 meta.Path.filename().string());
            ++result.Skipped;
            continue;
        }

        const uint32 lods = model->GenerateLODs(resolved.Config, resolved.GenerateSkinned);
        assets.GetEventDispatcher().DispatchEvent(
            AssetEvent(AssetEventType::AssetReloaded, guid, AssetType::Model, meta.Path.string()));
        Logger::Log::Info("Reimport LODs: '{}' -> {} level(s)", meta.Path.filename().string(), lods);
        ++result.Regenerated;
    }

    Logger::Log::Info("Reimport LODs: done ({} regenerated, {} skipped/opt-out)", result.Regenerated,
                      result.Skipped);
    return result;
}

} // namespace GameEngine
