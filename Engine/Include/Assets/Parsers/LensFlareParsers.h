#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/FlareAtlasAsset.h"
#include "Assets/LensFlareDefinitionAsset.h"

namespace GameEngine {

// Parser for engine-native .flareatlas JSON. Creates the asset; AssetManager
// calls Asset::Load() to read + parse the file.
class FlareAtlasParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::FlareAtlas; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".flareatlas"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager&) override
    {
        try
        {
            return AssetParseResult(std::make_shared<FlareAtlasAsset>(metadata.Guid, metadata.Path));
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create flare atlas asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "FlareAtlasParser"; }
    int GetPriority() const override { return 100; }

    // Emit a dependency edge for the referenced texture.
    bool ExtractDependencies(const GUID& referrer, const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

// Parser for engine-native .lensflare JSON.
class LensFlareDefinitionParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::LensFlareDefinition; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".lensflare"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager&) override
    {
        try
        {
            return AssetParseResult(
                std::make_shared<LensFlareDefinitionAsset>(metadata.Guid, metadata.Path));
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create lens flare asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "LensFlareDefinitionParser"; }
    int GetPriority() const override { return 100; }

    // Emit a dependency edge for the referenced atlas.
    bool ExtractDependencies(const GUID& referrer, const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

} // namespace GameEngine
