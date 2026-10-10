#pragma once

#include "AssetCore/Asset.h"
#include "Assets/LensFlareTypes.h"

#include <string>
#include <vector>

namespace GameEngine {

// Engine-native flare atlas (.flareatlas JSON): a reference to a texture plus a
// table of named sprites (normalized UV rects). Produced by the importer that
// transcodes external authoring formats; consumed by the lens-flare renderer.
//
// Schema:
//   { "texture": "<guid-or-mount-relative-path>",
//     "sprites": [ {"name":"Glow","u":0,"v":0.5,"w":0.5,"h":0.5}, ... ] }
class FlareAtlasAsset : public Asset
{
public:
    FlareAtlasAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::FlareAtlas, path)
    {
    }

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const std::string& GetTextureRef() const { return m_TextureRef; }
    const std::vector<LensFlare::AtlasSprite>& GetSprites() const { return m_Sprites; }

    // Index of a sprite by name, or -1 if not present.
    int32_t FindSprite(const std::string& name) const;

private:
    bool ParseFromText(const std::string& text);

    std::string m_TextureRef; // guid or mount-relative path, resolver-healed
    std::vector<LensFlare::AtlasSprite> m_Sprites;
};

} // namespace GameEngine
