#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/AnimationGraphAsset.h"

namespace GameEngine
{

class AnimationGraphAssetParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::AnimationGraph; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".animgraph"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<AnimationGraphAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create animation graph asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "AnimationGraphAssetParser"; }
    int GetPriority() const override { return 110; }
};

} // namespace GameEngine
