#pragma once

#include "AssetCore/AssetRegistry.h" // AssetReference
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace GameEngine::Scene
{
// Pluggable hook for executing/materializing [embed] blocks into real loadable assets.
// SceneIO calls this lazily when a schema resolves a "#id" that refers to an embed.
class ISceneEmbedMaterializer
{
  public:
    virtual ~ISceneEmbedMaterializer() = default;

    // Return true and fill outRef on success. On failure, return false and optionally set outError.
    virtual bool Materialize(const std::filesystem::path& owningSceneFile,
                             const GUID& embedGuid,
                             std::string_view embedId,
                             AssetType assetType,
                             std::string_view embedTypeName,
                             const std::unordered_map<std::string, std::string>& properties,
                             AssetReference& outRef,
                             std::string* outError) = 0;
};

} // namespace GameEngine::Scene

