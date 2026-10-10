#pragma once

#include <memory>
#include <vector>
#include "AssetCore/Asset.h"
#include "UI/UITemplateNode.h"

namespace GameEngine {

class UILayoutAsset : public Asset {
public:
    UILayoutAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::UILayout, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    UITemplateNode* GetRoot() const { return m_Root.get(); }

private:
    std::unique_ptr<UITemplateNode> m_Root;
};

} // namespace GameEngine
