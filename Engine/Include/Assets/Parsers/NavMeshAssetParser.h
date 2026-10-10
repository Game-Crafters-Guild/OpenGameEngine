#pragma once

#include "Assets/NavMeshAsset.h"
#include "Assets/ParserRegistry.h"

namespace GameEngine {

class NavMeshAssetParser final : public AssetParser {
public:
    AssetType GetAssetType() const override { return AssetType::NavigationMesh; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".navmesh"};
    }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<NavMeshAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, "Failed to create NavMesh asset: " + std::string(e.what()));
        }
    }

    std::string GetName() const override { return "NavMeshAssetParser"; }

    int GetPriority() const override { return 100; }
};

} // namespace GameEngine
