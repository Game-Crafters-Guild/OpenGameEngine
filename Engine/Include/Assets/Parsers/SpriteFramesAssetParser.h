#pragma once

#include "Animation/SpriteFrames.h"
#include "Assets/ParserRegistry.h"

namespace GameEngine
{

class SpriteFramesAssetParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::SpriteFrames; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".spriteframes"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            return AssetParseResult(std::make_shared<Animation::SpriteFrames>(metadata.Guid, metadata.Path));
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create sprite frames asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "SpriteFramesAssetParser"; }
    int GetPriority() const override { return 110; }
};

} // namespace GameEngine
