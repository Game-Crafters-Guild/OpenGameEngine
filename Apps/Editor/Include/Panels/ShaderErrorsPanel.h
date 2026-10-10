#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Panels/ShaderErrorRows.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/ListView.h"

namespace GameEngine {

class INativeContextMenu;
class Label;
namespace Platform { class Window; }

// Shader Errors: surfaces failed material shader compiles (surface .glsl,
// vertex modifiers) the moment they fail — no inspector selection required.
// Data source is MaterialSystem::ShaderErrors() (failure upserts, success
// clears); Update() polls its version, rebuilds the row list, and asks the
// editor to reveal the panel when new failures land. Rows carry the real
// file:line from the compile-error attribution work, so double-click opens
// the author's surface file at the failing line. Layout is a list on the left
// and the selected row's full, untruncated diagnostic on the right. The list
// selects its first row on rebuild, so the detail view always describes
// something. Source is read by double-clicking a row, which opens the Script
// Editor at the failing line.
class ShaderErrorsPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-shader-errors-icon"; }

    ShaderErrorsPanel();
    ~ShaderErrorsPanel() override;

    void Update();

    void SetOnOpenInEditor(std::function<void(const std::filesystem::path&, size_t line, size_t column)> cb)
    {
        m_OnOpenInEditor = std::move(cb);
    }
    void SetOnOpenInScriptEditor(std::function<void(const std::filesystem::path&, size_t line, size_t column)> cb)
    {
        m_OnOpenInScriptEditor = std::move(cb);
    }
    // Invoked (on the UI thread) on every error-log version change that leaves
    // the row list non-empty — the editor activates this panel's dock tab. That
    // includes REPEAT failures: hitting save again on a still-broken shader
    // re-reveals deliberately (Update() in the .cpp is the authority).
    void SetOnRequestReveal(std::function<void()> cb) { m_OnRequestReveal = std::move(cb); }
    void SetWindow(Platform::Window* window) { m_Window = window; }

private:
    void RebuildRows();
    void SelectEntry(int index);
    void OpenEntry(int index);
    void OpenEntryInScriptEditor(int index);
    void ShowContextMenu(int index, float x, float y);
    bool CopyErrorToClipboard(int index) const;
    bool CopyAllErrorsToClipboard() const;
    void ShowDetail(const ShaderErrorRow* row);

    class RowsProvider;
    std::unique_ptr<RowsProvider> m_RowsProvider;
    ListView* m_RowListView = nullptr;
    Label* m_DetailMessage = nullptr;

    std::vector<ShaderErrorRow> m_Rows;
    uint64_t m_SeenVersion = 0;
    int m_SelectedIndex = -1;
    int m_ContextMenuIndex = -1;

    std::function<void(const std::filesystem::path&, size_t, size_t)> m_OnOpenInEditor;
    std::function<void(const std::filesystem::path&, size_t, size_t)> m_OnOpenInScriptEditor;
    std::function<void()> m_OnRequestReveal;
    Platform::Window* m_Window = nullptr;
    std::unique_ptr<INativeContextMenu> m_ContextMenu;
};

} // namespace GameEngine
