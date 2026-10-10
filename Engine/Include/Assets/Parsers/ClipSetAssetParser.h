#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/ClipSetAsset.h"

namespace GameEngine
{

// ClipSet Asset Parser — handles .clipset JSON documents (lane-based clip arrangements).
// Mirrors MaterialAssetParser: the parser only constructs the asset; AssetManager calls
// Asset::Load() afterwards, which performs the actual JSON read.
class ClipSetAssetParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::ClipSet; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".clipset"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<ClipSetAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create clipset asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "ClipSetAssetParser"; }
    int GetPriority() const override { return 110; }
};

} // namespace GameEngine
