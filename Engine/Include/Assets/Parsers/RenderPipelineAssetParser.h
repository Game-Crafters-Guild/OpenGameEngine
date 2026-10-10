#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/RenderPipelineAsset.h"

namespace GameEngine
{

// RenderPipeline Asset Parser - handles .rendergraph/.renderpipeline documents
class RenderPipelineAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::RenderPipeline; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {".rendergraph", ".renderpipeline"};
    }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<RenderPipelineAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create render pipeline asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "RenderPipelineAssetParser"; }

    int GetPriority() const override { return 110; } // Prefer over generic binary
};

} // namespace GameEngine
