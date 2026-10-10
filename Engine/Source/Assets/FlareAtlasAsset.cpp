#include "Assets/FlareAtlasAsset.h"

#include "AssetCore/SharedFileRead.h"

#include <nlohmann/json.hpp>

namespace GameEngine {

bool FlareAtlasAsset::Load()
{
    String text;
    if (!ReadFileTextShared(GetPath(), text))
    {
        SetState(AssetState::Failed);
        return false;
    }
    const bool ok = ParseFromText(text);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

bool FlareAtlasAsset::LoadFromData(const Vector<uint8>& data)
{
    const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    const bool ok = ParseFromText(text);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

void FlareAtlasAsset::Unload()
{
    m_TextureRef.clear();
    m_Sprites.clear();
    SetState(AssetState::Unloaded);
}

int32_t FlareAtlasAsset::FindSprite(const std::string& name) const
{
    for (size_t i = 0; i < m_Sprites.size(); ++i)
    {
        if (m_Sprites[i].Name == name)
            return static_cast<int32_t>(i);
    }
    return -1;
}

bool FlareAtlasAsset::ParseFromText(const std::string& text)
{
    m_TextureRef.clear();
    m_Sprites.clear();

    nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (doc.is_discarded() || !doc.is_object())
        return false;

    m_TextureRef = doc.value("texture", std::string{});

    if (doc.contains("sprites") && doc["sprites"].is_array())
    {
        for (const auto& s : doc["sprites"])
        {
            if (!s.is_object())
                continue;
            LensFlare::AtlasSprite sprite;
            sprite.Name = s.value("name", std::string{});
            sprite.U = s.value("u", 0.0f);
            sprite.V = s.value("v", 0.0f);
            sprite.W = s.value("w", 1.0f);
            sprite.H = s.value("h", 1.0f);
            m_Sprites.push_back(std::move(sprite));
        }
    }

    return true;
}

} // namespace GameEngine
