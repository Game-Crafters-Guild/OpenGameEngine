#pragma once

#include "Animation/AnimationController.h"
#include "Assets/ParserRegistry.h"

namespace GameEngine
{

class AnimationControllerAssetParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::AnimationController; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".animcontroller", ".animationcontroller"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            return AssetParseResult(std::make_shared<Animation::AnimationController>(metadata.Guid, metadata.Path));
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create animation controller asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "AnimationControllerAssetParser"; }
    int GetPriority() const override { return 110; }
};

} // namespace GameEngine
