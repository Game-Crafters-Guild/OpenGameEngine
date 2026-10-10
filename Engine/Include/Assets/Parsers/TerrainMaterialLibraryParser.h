#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/TerrainMaterialLibraryAsset.h"

namespace GameEngine {

// Parser for engine-native .terrainmatlib JSON. Creates the asset; AssetManager
// calls Asset::Load() to read + parse the file.
class TerrainMaterialLibraryParser final : public AssetParser
{
public:
    AssetType GetAssetType() const override { return AssetType::TerrainMaterialLibrary; }
    std::vector<std::string> GetSupportedExtensions() const override { return {".terrainmatlib"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, AssetManager&) override
    {
        try
        {
            return AssetParseResult(
                std::make_shared<TerrainMaterialLibraryAsset>(metadata.Guid, metadata.Path));
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(
                false, std::string("Failed to create terrain material library asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "TerrainMaterialLibraryParser"; }
    int GetPriority() const override { return 100; }

    // Emit a dependency edge per referenced texture, so a packaged project carries
    // the maps its terrains shade with instead of loading back to flat tints.
    bool ExtractDependencies(const GUID& referrer, const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

} // namespace GameEngine
