#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/ModelAsset.h"

namespace GameEngine
{

// Model Asset Parser - handles .obj, .gltf, .glb, .fbx (via vendored ufbx),
// and .blend (via vendored fbtBlend).
class ModelAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::Model; }

    std::vector<std::string> GetSupportedExtensions() const override
    {
        return {
            ".obj",
            ".gltf",
            ".glb",
#if defined(GE_HAVE_UFBX)
            ".fbx",
#endif
#if defined(GE_HAVE_FBTBLEND)
            ".blend",
#endif
        };
    }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<ModelAsset>(metadata.Guid, metadata.Path);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, "Failed to create model asset: " + std::string(e.what()));
        }
    }

    std::string GetName() const override { return "ModelAssetParser"; }

    int GetPriority() const override { return 100; }

    // Walk a glTF / .glb model and emit MaterialTexture edges for each
    // external image URI in the model's images[] array. Data URIs and
    // empty entries are skipped. Other model formats (.obj, .fbx, .blend)
    // return true with no edges — they're binary, the syntactic fallback
    // can't run on them safely, and dep extraction for those formats is
    // a separate workstream (image refs live inside the binary chunks).
    bool ExtractDependencies(const GUID& referrer,
                             const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;
};

} // namespace GameEngine
