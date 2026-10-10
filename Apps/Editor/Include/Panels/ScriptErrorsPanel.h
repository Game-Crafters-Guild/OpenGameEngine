#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Panels/ScriptDiagnosticsByAssembly.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/ListView.h"

namespace GameEngine {

struct CompileServerDiagnostic;
class INativeContextMenu;
class ScriptTextArea;
class UIElement;
class ScrollView;
namespace Platform { class Window; }

class ScriptErrorsPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-script-errors-icon"; }

    ScriptErrorsPanel();
    ~ScriptErrorsPanel() override;

    void Update();

    void SetOnOpenInEditor(std::function<void(const std::filesystem::path&, size_t line, size_t column)> cb)
    {
        m_OnOpenInEditor = std::move(cb);
    }
    void SetOnOpenInScriptEditor(std::function<void(const std::filesystem::path&, size_t line, size_t column)> cb)
    {
        m_OnOpenInScriptEditor = std::move(cb);
    }
    void SetWindow(Platform::Window* window) { m_Window = window; }

private:
    void SetupObservers();
    void TeardownObservers();
    // Hand the model the assemblies a reload compiles when the package set changes.
    void RetainCompiledAssemblies();
    // Rebind the list to m_Diagnostics after it was replaced. keptSelection is
    // the new index of the selected row when it survived, or -1, which clears
    // the selection and the preview.
    void ShowRows(int keptSelection);
    void SelectEntry(int index);
    void OpenEntry(int index);
    void OpenEntryInScriptEditor(int index);
    void ShowContextMenu(int index, float x, float y);
    bool CopyErrorToClipboard(int index) const;
    bool CopyAllErrorsToClipboard() const;
    void LoadPreview(const CompileServerDiagnostic& diag);
    void ScrollPreviewToLine(size_t lineNumber, size_t diagnosticColumn);

    class DiagnosticsProvider;
    std::unique_ptr<DiagnosticsProvider> m_DiagnosticsProvider;
    ListView* m_DiagnosticListView = nullptr;
    ScrollView* m_PreviewScrollView = nullptr;
    ScriptTextArea* m_PreviewTextArea = nullptr;
    std::filesystem::path m_PreviewFilePath;

    // Filled by the compile-server observers on compile threads; Update copies
    // its rows into m_Diagnostics.
    ScriptDiagnosticsByAssembly m_DiagnosticsByAssembly;
    uint64_t m_SeenPackageCodeModulesRevision = 0;

    uint64_t m_DiagnosticsObserverId = 0;
    uint64_t m_CompileStartedObserverId = 0;

    std::vector<CompileServerDiagnostic> m_Diagnostics;
    int m_SelectedIndex = -1;
    int m_ContextMenuIndex = -1;

    std::function<void(const std::filesystem::path&, size_t, size_t)> m_OnOpenInEditor;
    std::function<void(const std::filesystem::path&, size_t, size_t)> m_OnOpenInScriptEditor;
    Platform::Window* m_Window = nullptr;
    std::unique_ptr<INativeContextMenu> m_ContextMenu;
};

} // namespace GameEngine
