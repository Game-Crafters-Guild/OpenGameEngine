#include "Engine/Build/StagedAssetCook.h"

#include "Assets/ParserRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/SceneExportStrip.h"
#include "Engine/Build/TexturePackageCook.h"

namespace GameEngine
{
bool CookStagedContent(const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                       const AssetRegistry& registry, const ParserRegistry& parsers,
                       TextureCookEncodeQuality textureQuality, TextureCookWorkers* textureWorkers,
                       const std::function<bool()>& cancelRequested, TexturePackageCookStats& textureStats,
                       std::string& error)
{
    if (!StripEditorOnlyComponentsFromStagedScenes(contentRoot, manifest, cancelRequested, error))
        return false;
    if (!StagePackagedTextureCooks(contentRoot, manifest, registry, textureQuality, textureWorkers, cancelRequested,
                                   textureStats, error))
        return false;
    return CookStagedAssets(contentRoot, manifest, parsers, cancelRequested, error);
}

bool CookStagedAssets(const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                      const ParserRegistry& parsers, const std::function<bool()>& cancelRequested,
                      std::string& error)
{
    error.clear();
    for (const auto& entry : manifest.entries)
    {
        if (cancelRequested && cancelRequested())
            return false;
        const auto parser = parsers.FindParser(entry.sourcePath);
        if (!parser)
            continue;
        std::string reason;
        if (!parser->CookForPackage(contentRoot / entry.outputPath, reason))
        {
            error = entry.outputPath.generic_string() + ": " + reason;
            return false;
        }
    }
    return true;
}
} // namespace GameEngine
