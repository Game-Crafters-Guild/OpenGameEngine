#pragma once

#include "Assets/NavGridAsset.h"
#include "Assets/ParserRegistry.h"

namespace GameEngine {

class NavGridAssetParser final : public AssetParser {
public:
    AssetType GetAssetType() const override { return AssetType::NavigationGrid; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".navgrid"};
    }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<NavGridAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, "Failed to create NavGrid asset: " + std::string(e.what()));
        }
    }

    std::string GetName() const override { return "NavGridAssetParser"; }

    int GetPriority() const override { return 100; }
};

} // namespace GameEngine
