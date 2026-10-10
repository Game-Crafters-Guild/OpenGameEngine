#pragma once

#include "AssetCore/Asset.h"
#include "Assets/LensFlareTypes.h"

#include <string>
#include <utility>
#include <vector>

namespace GameEngine {

// Engine-native lens flare (.lensflare JSON): a reference to a FlareAtlas plus
// per-flare globals and an ordered element list. Produced by the importer;
// consumed by the lens-flare extraction system.
//
// Schema:
//   { "atlas": "<guid-or-mount-relative-path>",
//     "globals": { ... FlareGlobals ... },
//     "elements": [ { ... FlareElement ... }, ... ] }
class LensFlareDefinitionAsset : public Asset
{
public:
    LensFlareDefinitionAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::LensFlareDefinition, path)
    {
    }

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const std::string& GetAtlasRef() const { return m_AtlasRef; }
    const LensFlare::FlareGlobals& GetGlobals() const { return m_Globals; }
    const std::vector<LensFlare::FlareElement>& GetElements() const { return m_Elements; }

    // Authoring access used by the editor lens-flare inspector. Runtime callers
    // should continue to use the const accessors above.
    void SetAtlasRef(std::string atlasRef) { m_AtlasRef = std::move(atlasRef); }
    LensFlare::FlareGlobals& EditGlobals() { return m_Globals; }
    std::vector<LensFlare::FlareElement>& EditElements() { return m_Elements; }

    // Persist the current in-memory definition as engine-native .lensflare JSON.
    bool Save() const;

private:
    bool ParseFromText(const std::string& text);

    std::string m_AtlasRef; // guid or mount-relative path, resolver-healed
    LensFlare::FlareGlobals m_Globals;
    std::vector<LensFlare::FlareElement> m_Elements;
};

} // namespace GameEngine
