#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "AssetCore/Asset.h"
#include "UI/UIStyle.h"

namespace GameEngine {

class AssetManager;

class UIStyleAsset : public Asset {
public:
    UIStyleAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::UIStyle, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const Stylesheet& GetStylesheet() const { return m_Stylesheet; }

    // Immutable snapshot shared with UIManager and per-element attachments.
    // Successful loads/reloads publish new handles; retained handles keep their
    // rules until released, including after Unload. Reacquire to see new rules.
    StylesheetHandle GetStylesheetHandle() const;

    // Return the fully flattened cascade for this stylesheet, including any @imports,
    // preserving mid-file @import ordering semantics by splitting the file into segments.
    //
    // Note: this returns only *loaded* imported assets' stylesheets. Callers may want to
    // pre-load dependencies (e.g., via UIHotReload) for fully deterministic results.
    std::vector<StylesheetHandle> GetCascadeHandles(AssetManager& assets) const;

    // Transitive @import dependencies discovered during the last Load/Reload.
    // Used by UIHotReload to propagate changes from imported stylesheets.
    const std::vector<GUID>& GetImportedStyleGuids() const { return m_ImportedStyleGuids; }

  private:
    // Parse an unpublished candidate before replacing the current snapshots.
    bool ReloadFromData(const Vector<uint8>& data) override;
    bool ParseFromData(const Vector<uint8>& data);

    struct Segment
    {
        enum class Kind : uint8_t
        {
            CssText = 0,
            Import
        };

        Kind kind = Kind::CssText;
        // Published CSS segments are immutable, just like the single-sheet handle.
        StylesheetHandle sheet;
        // For Import segments, the imported UIStyle asset guid (resolved at load time when possible).
        GUID importGuid = GUID::Null();
    };

    Stylesheet m_Stylesheet;
    std::vector<Segment> m_Segments;
    std::vector<GUID> m_ImportedStyleGuids;
    mutable StylesheetHandle m_Handle;
};

} // namespace GameEngine
