#pragma once

#include "AssetCore/Asset.h"
#include <string>

namespace GameEngine
{

// Human-readable scene asset (.scene). This is primarily an editor-authoring format.
// Runtime loading is performed by Scene::LoadSceneFromFile which consumes the file
// and instantiates entities into an ECS World.
class SceneAsset final : public Asset
{
  public:
    SceneAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Scene, path)
    {
    }

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const std::string& GetText() const { return m_Text; }

  private:
    std::string m_Text;
};

} // namespace GameEngine

