#pragma once

#include "EditorApplication.h" // EditorApplication::EditorWindowContext

#include <filesystem>
#include <functional>
#include <memory>

namespace GameEngine
{
class AssetsPanel;
class DockingManager;
class EditorPanelManager;
class HierarchyPanel;
class ProjectFolderPickerModal;
class ScriptEditorPanel;
class SearchDialog;
class SettingsPanel;
class UniversalSearchProvider;
namespace ECS
{
class World;
}

namespace Editor
{
class PlayModeManager;
class SceneEditorController;
class UndoRedoService;

/// Owns the editor's global search/command palette: the multi-domain provider,
/// the SearchDialog mounted on the main window's UI tree, and the registered
/// command/panel/setting entries. EditorApplication owns one instance and
/// forwards the global shortcut to Toggle().
class UniversalSearchController
{
  public:
    /// Everything the palette needs from the application. Raw pointers are
    /// application-lifetime (panels live in the app's panel storage).
    struct Dependencies
    {
        EditorApplication::EditorWindowContext* MainWindow = nullptr;
        EditorPanelManager* PanelManager = nullptr;
        DockingManager* Docking = nullptr;
        AssetsPanel* Assets = nullptr;
        HierarchyPanel* Hierarchy = nullptr;
        SettingsPanel* Settings = nullptr;
        ScriptEditorPanel* ScriptEditor = nullptr;
        SceneEditorController* SceneEditor = nullptr;
        PlayModeManager* PlayMode = nullptr;
        UndoRedoService* UndoRedo = nullptr;
        ProjectFolderPickerModal* ProjectFolderModal = nullptr;
        std::function<ECS::World*()> GetWorld;
        std::function<std::filesystem::path()> GetProjectRoot;
        std::function<void()> ResetLayout;
    };

    UniversalSearchController();
    ~UniversalSearchController();

    /// Build the provider, register commands/panels/settings, and mount the
    /// dialog on the main window. Safe to call once; later calls no-op.
    void Initialize(const Dependencies& deps);

    /// Open the palette (refreshing runtime entries) or close it if open.
    void Toggle();

    /// Re-index project code when the open project changes.
    void OnProjectRootChanged(const std::filesystem::path& projectRoot);

    bool IsOpen() const;

  private:
    void RegisterCommands();
    void RegisterPanels();
    void RegisterSettingsCategory();
    void RefreshScriptEntries();
    void RefreshSettingsEntries();

    Dependencies m_Deps;
    std::unique_ptr<UniversalSearchProvider> m_Provider;
    SearchDialog* m_Dialog = nullptr; // owned by the main window's UI tree
};

} // namespace Editor
} // namespace GameEngine
