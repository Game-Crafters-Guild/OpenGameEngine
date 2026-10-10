#pragma once

#include "AssetCore/Asset.h"

#include <string>

namespace GameEngine
{

// Generic text-based XML asset (.xml) for cases where the XML is not a UI layout.
// UI layout XML is represented by UILayoutAsset in the UI module.
class XmlAsset : public Asset
{
  public:
    XmlAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::XML, path)
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
