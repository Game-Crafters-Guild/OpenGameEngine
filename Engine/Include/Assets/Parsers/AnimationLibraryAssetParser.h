#pragma once

#include "Animation/AnimationLibrary.h"
#include "Assets/ParserRegistry.h"

namespace GameEngine
{

class AnimationLibraryAssetParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::AnimationLibrary; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".animlib", ".animationlib"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<Animation::AnimationLibrary>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create animation library asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "AnimationLibraryAssetParser"; }
    int GetPriority() const override { return 110; }
};

} // namespace GameEngine
