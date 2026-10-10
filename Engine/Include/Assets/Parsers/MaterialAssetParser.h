#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/MaterialAsset.h"

namespace GameEngine
{

// Material Asset Parser - handles .material JSON and imported Unity .mat documents.
//
// IMPORTANT: the parser creates the Asset instance only. The AssetManager will call Asset::Load()
// separately, which performs the actual file read + parsing of the MaterialDocument. Build/compile
// (shaderpkg generation) is intentionally a separate step, triggered by the renderer.
class MaterialAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::Material; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".material", ".mat", ".mtlx"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<MaterialAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, std::string("Failed to create material asset: ") + e.what());
        }
    }

    std::string GetName() const override { return "MaterialAssetParser"; }

    int GetPriority() const override { return 110; } // Prefer over generic binary fallback

    // Walk the material JSON and emit DepEdges for shader + texture references.
    // Surface/vertex shader edges use DepEdgeKind::MaterialShader; textures use
    // DepEdgeKind::MaterialTexture. References authored as GUIDs become
    // GUID-form edges; references authored as paths (legacy v2 or v3 path
    // fallback) become path-form edges and resolve at query time.
    bool ExtractDependencies(const GUID& referrer,
                             const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

} // namespace GameEngine
