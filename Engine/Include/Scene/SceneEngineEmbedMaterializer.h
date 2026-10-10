#pragma once

#include "Scene/SceneEmbedMaterializer.h"

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Scene
{
// Engine-side embed executor: materializes supported embed types into real Asset instances
// and registers them into AssetManager as "virtual" loaded assets.
class SceneEngineEmbedMaterializer final : public ISceneEmbedMaterializer
{
  public:
    explicit SceneEngineEmbedMaterializer(AssetManager& assets);

    bool Materialize(const std::filesystem::path& owningSceneFile,
                     const GUID& embedGuid,
                     std::string_view embedId,
                     AssetType assetType,
                     std::string_view embedTypeName,
                     const std::unordered_map<std::string, std::string>& properties,
                     AssetReference& outRef,
                     std::string* outError) override;

  private:
    AssetManager& m_Assets;
};

} // namespace GameEngine::Scene

