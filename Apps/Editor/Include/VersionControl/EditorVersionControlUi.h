#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
class UIManager;
class AssetsPanel;
class DiffPanel;
class DockingManager;
class DockNode;
struct EditorContext;
class EditorVersionControlService;
class HierarchyPanel;
class InspectorPanel;

namespace Editor
{
class VcsCommitDialog;
class VcsLogViewer;
}

// Owns VCS-related Editor dialogs and provides UI callbacks for panels.
class EditorVersionControlUi final
{
  public:
    EditorVersionControlUi();
    ~EditorVersionControlUi();

    void BindAssetsPanel(AssetsPanel& assetsPanel, UIManager& ui, DiffPanel* diffPanel = nullptr,
                         DockingManager* docking = nullptr);
    void SetScenePathProvider(
        std::function<std::optional<std::filesystem::path>()> provider);
    void BindHierarchyPanel(HierarchyPanel& hierarchyPanel);
    void BindInspectorPanel(InspectorPanel& inspectorPanel);
    void RefreshSceneDecorations();
    // Persists the shared Hierarchy/Inspector indicator preference and repaints
    // every bound panel.
    void SetSceneDiffIndicatorsVisible(bool visible);
    void InitializeForProject(EditorVersionControlService& service,
                              const std::filesystem::path& projectRoot,
                              const EditorContext* context);

  private:
    // Declares the provider-neutral Version Control settings page. The
    // SettingsPanel renders it from the descriptor; nothing here builds rows.
    void RegisterSettings();
    void ShowCommitDialog(UIManager& ui, const std::string& defaultMsg);
    void ShowLogViewer(UIManager& ui, const std::filesystem::path& filePath);
    void ShowDiffPanel(DiffPanel* diffPanel, const std::filesystem::path& filePath);
    void ActivateDiffPanel(DockingManager* docking, UIManager* ui, DiffPanel* diffPanel);
    static DockNode* FindFirstLeaf(DockNode* node);
    static DockNode* FindLeafWithTab(DockNode* node, const std::string& tabId);

    std::unique_ptr<Editor::VcsCommitDialog> m_CommitDialog;
    std::unique_ptr<Editor::VcsLogViewer> m_LogViewer;
    DockingManager* m_Docking = nullptr;
    UIManager* m_Ui = nullptr;
    std::function<std::optional<std::filesystem::path>()> m_ScenePathProvider;
    std::vector<AssetsPanel*> m_AssetsPanels;
    std::vector<HierarchyPanel*> m_HierarchyPanels;
    std::vector<InspectorPanel*> m_InspectorPanels;
};

} // namespace GameEngine
