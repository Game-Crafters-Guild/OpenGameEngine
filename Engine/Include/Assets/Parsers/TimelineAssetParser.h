#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/TimelineAsset.h"

namespace GameEngine
{

// Timeline Asset Parser — handles .timeline JSON documents (composite multi-track timelines).
// Mirrors MaterialAssetParser: the parser only constructs the asset; AssetManager calls
// Asset::Load() afterwards, which performs the actual JSON read.
class TimelineAssetParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::Timeline; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".timeline"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<TimelineAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create timeline asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "TimelineAssetParser"; }
    int GetPriority() const override { return 110; }
};

} // namespace GameEngine
