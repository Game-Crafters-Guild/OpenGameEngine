#pragma once

#include "Assets/TextureCook.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Engine/Build/AssetCollector.h"

#include <unordered_map>

namespace GameEngine
{
class AssetRegistry;

// Read staged CPU material documents; resolve paths through the existing source-aware
// AssetManager resolver. All bindings participate: ambiguous/custom use stays Auto.
std::unordered_map<GUID, TextureCookUsage> CollectPackagedTextureUsages(
    const std::filesystem::path& contentRoot, const AssetManifest& manifest, const AssetManager& manager);

// Copy authored texture metadata, then fill unresolved usage and colour, widening an
// authored usage that a material slot needs wider (WidenTextureCookUsage).
// Never writes back into the authoring registry.
void StageTextureImportMetadata(const AssetRegistry& registry, const AssetManifestEntry& entry,
                                TextureCookUsage materialUsage, AssetDatabase::AssetRecord& record);

struct TexturePackageCookStats
{
    size_t Textures = 0;
    size_t UnclassifiedTextures = 0;
    size_t SkippedTextures = 0;
    size_t Artifacts = 0;
    size_t ReusedArtifacts = 0;
    uint64 Bytes = 0;
};

// Bake the textures already copied to the staging tree, using that tree's
// authoritative .assetmanifest metadata. Original cache artifacts are reusable
// only under the exact shared source/settings key and after KTX2 validation.
// BC-capable and uncompressed device variants share a file when their keys agree.
// Failure/cancellation leaves the final export untouched (BuildPipeline publishes
// the staging tree only after success); no partial KTX2 file is published.
// `encodeWorkers` spreads each bake across a pool (CookTexture's `workers`); null
// bakes on the calling thread.
bool StagePackagedTextureCooks(const std::filesystem::path& contentRoot,
                              const AssetManifest& manifest,
                              const AssetRegistry& sourceRegistry,
                              TextureCookEncodeQuality targetBc7Quality,
                              TextureCookWorkers* encodeWorkers,
                              const std::function<bool()>& cancelRequested,
                              TexturePackageCookStats& stats,
                              std::string& error);
} // namespace GameEngine
