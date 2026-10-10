#pragma once

#include "AssetCore/Asset.h"

#include <string>

namespace GameEngine {

// Text shader source asset (.glsl/.hlsl/.vert/.frag/.comp/...)
class ShaderSourceAsset : public Asset
{
public:
    ShaderSourceAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::Shader, path)
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

