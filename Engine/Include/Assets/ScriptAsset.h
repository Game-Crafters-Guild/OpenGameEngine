#pragma once

#include "AssetCore/Asset.h"

#include <string>

namespace GameEngine
{

// Text-based C# script asset (.cs)
class ScriptAsset : public Asset
{
  public:
    ScriptAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Script, path)
    {
    }

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const std::string& GetSourceText() const { return m_Text; }

  private:
    std::string m_Text;
};

} // namespace GameEngine
