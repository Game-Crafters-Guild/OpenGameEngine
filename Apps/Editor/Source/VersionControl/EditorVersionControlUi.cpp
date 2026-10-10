#include "VersionControl/EditorVersionControlUi.h"

#include "EditorPanelIds.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Editor/Vcs/VcsStatusUi.h"
#include "VersionControl/Ui/VcsCommitDialog.h"
#include "VersionControl/Ui/VcsLogViewer.h"
#include "Logger/Logger.h"
#include "Panels/AssetsPanel.h"
#include "Panels/DiffPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/InspectorPanel.h"
#include "UI/UIManager.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Layout/Docking.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "VersionControl/EditorVersionControlService.h"
#include "VersionControl/SceneDiff.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace GameEngine
{

EditorVersionControlUi::EditorVersionControlUi()
{
    m_CommitDialog = std::make_unique<Editor::VcsCommitDialog>();
    m_LogViewer = std::make_unique<Editor::VcsLogViewer>();
    RegisterSettings();
}

void EditorVersionControlUi::RegisterSettings()
{
    Editor::SettingsCategoryDescriptor sceneDiffs;
    sceneDiffs.CategoryId = "vcsSceneDiff";
    sceneDiffs.Title = "Scene Diffs";
    sceneDiffs.Group = Editor::SettingsCategoryGroup::VersionControl;
    sceneDiffs.Description =
        "In-place change indicators are provider-neutral: they follow whichever "
        "version control system the open project uses.";

    Editor::SettingsFieldDescriptor showDots;
    showDots.Label = "Show Hierarchy and Inspector Dots";
    showDots.Tooltip =
        "Mark added, modified and removed entities and properties directly in "
        "the Hierarchy and Inspector.";
    showDots.SearchKeywords =
        "version control vcs git svn scene diff dots markers hierarchy inspector indicators";
    Editor::SettingsFieldDescriptor::ToggleField showDotsToggle;
    showDotsToggle.DefaultValue = true;
    // No PrefKey: VcsStatusUi owns both the persisted key and the process-wide
    // cached value that the decoration paths read every frame.
    showDotsToggle.Get = []() { return Editor::AreVcsSceneDiffIndicatorsVisible(); };
    showDotsToggle.Set = [this](bool visible)
    {
        // Also fired once with the current value when the page is built, which
        // must not cost a decoration pass.
        if (visible == Editor::AreVcsSceneDiffIndicatorsVisible())
            return;
        SetSceneDiffIndicatorsVisible(visible);
    };
    showDots.Control = showDotsToggle;
    sceneDiffs.Fields.push_back(std::move(showDots));

    Editor::EditorSettingsRegistry::Get().RegisterCategory(std::move(sceneDiffs));
}

EditorVersionControlUi::~EditorVersionControlUi() = default;

void EditorVersionControlUi::BindAssetsPanel(AssetsPanel& assetsPanel, UIManager& ui, DiffPanel* diffPanel,
                                             DockingManager* docking)
{
    if (std::find(m_AssetsPanels.begin(), m_AssetsPanels.end(), &assetsPanel) ==
        m_AssetsPanels.end())
        m_AssetsPanels.push_back(&assetsPanel);
    assetsPanel.SetVcsDialogCallbacks(
        [this, &ui](const std::string& defaultMsg) { ShowCommitDialog(ui, defaultMsg); },
        [this, &ui](const std::filesystem::path& filePath) { ShowLogViewer(ui, filePath); });
    
    m_Docking = docking;
    m_Ui = &ui;
    
    if (diffPanel && docking)
    {
        // Ensure the diff panel is registered with the docking manager
        docking->RegisterPanel(EditorPanelIds::Diff, diffPanel);
        
        assetsPanel.SetOnShowDiff([this, diffPanel](const std::filesystem::path& filePath) {
            ShowDiffPanel(diffPanel, filePath);
        });
    }
}

void EditorVersionControlUi::SetScenePathProvider(
    std::function<std::optional<std::filesystem::path>()> provider)
{
    m_ScenePathProvider = std::move(provider);
}

void EditorVersionControlUi::BindHierarchyPanel(HierarchyPanel& hierarchyPanel)
{
    if (std::find(m_HierarchyPanels.begin(), m_HierarchyPanels.end(),
                  &hierarchyPanel) == m_HierarchyPanels.end())
        m_HierarchyPanels.push_back(&hierarchyPanel);
    hierarchyPanel.SetScenePathProvider(m_ScenePathProvider);
    hierarchyPanel.SetSceneDiffProvider([this]()
    {
        const auto path = m_ScenePathProvider ? m_ScenePathProvider()
                                              : std::nullopt;
        return path ? Editor::LoadVersionControlledSceneDiff(*path)
                    : std::vector<Editor::SceneObjectDiff>{};
    });
    hierarchyPanel.SetSceneDiffIndicatorToggle([this]()
    { SetSceneDiffIndicatorsVisible(!Editor::AreVcsSceneDiffIndicatorsVisible()); });
}

void EditorVersionControlUi::BindInspectorPanel(InspectorPanel& inspectorPanel)
{
    if (std::find(m_InspectorPanels.begin(), m_InspectorPanels.end(),
                  &inspectorPanel) == m_InspectorPanels.end())
        m_InspectorPanels.push_back(&inspectorPanel);
    inspectorPanel.SetSceneDiffProvider([this]()
    {
        const auto path = m_ScenePathProvider ? m_ScenePathProvider()
                                              : std::nullopt;
        return path ? Editor::LoadVersionControlledSceneDiff(*path)
                    : std::vector<Editor::SceneObjectDiff>{};
    });
}

// Settings page and the Hierarchy's own menu item both land here, so the two
// entry points cannot drift on what saving and repainting means.
void EditorVersionControlUi::SetSceneDiffIndicatorsVisible(bool visible)
{
    std::string error;
    if (!Editor::SetVcsSceneDiffIndicatorsVisible(visible, &error))
        Logger::Log::Warning(
            "Version control: failed to save the scene diff indicator preference: {}",
            error);
    RefreshSceneDecorations();
}

void EditorVersionControlUi::RefreshSceneDecorations()
{
    // Callers here are scene saves and project (re)initialization; both can
    // move the base revision, which the file-identity memo cannot see.
    Editor::InvalidateVersionControlledSceneDiffCache();
    for (HierarchyPanel* panel : m_HierarchyPanels)
        if (panel)
            panel->RefreshSceneDiffDecorations();
    for (InspectorPanel* panel : m_InspectorPanels)
        if (panel)
            panel->RefreshSceneDiffDecorations();
}

void EditorVersionControlUi::InitializeForProject(
    EditorVersionControlService& service,
    const std::filesystem::path& projectRoot,
    const EditorContext* context)
{
    service.InitializeForProject(projectRoot);
    for (AssetsPanel* panel : m_AssetsPanels)
        if (panel)
        {
            panel->SetContext(context);
            panel->RefreshViews();
        }
    RefreshSceneDecorations();
}

void EditorVersionControlUi::ShowCommitDialog(UIManager& ui, const std::string& defaultMsg)
{
    if (!m_CommitDialog)
        return;

    // The active provider owns the default-commit-message setting.
    std::string msg = defaultMsg;
    Editor::EditorVcsProviderDescriptor provider;
    if (msg.empty() && Editor::EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) &&
        provider.DefaultCommitMessage)
    {
        msg = provider.DefaultCommitMessage();
    }

    m_CommitDialog->Show(
        &ui,
        msg,
        [](const std::string& commitMsg)
        {
            auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration();
            if (vcs && vcs->IsRepository())
            {
                (void)vcs->Commit(commitMsg);
            }
        },
        []() {});
}

void EditorVersionControlUi::ShowLogViewer(UIManager& ui, const std::filesystem::path& filePath)
{
    if (!m_LogViewer)
        return;

    Editor::EditorVcsProviderDescriptor provider;
    if (!Editor::EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider))
    {
        Logger::Log::Warning("EditorVersionControlUi: no active VCS for the log viewer");
        return;
    }
    m_LogViewer->Show(&ui, &provider.Integration(), provider.DisplayName, filePath);
}

void EditorVersionControlUi::ShowDiffPanel(DiffPanel* diffPanel, const std::filesystem::path& filePath)
{
    if (!diffPanel || filePath.empty())
    {
        Logger::Log::Warning("EditorVersionControlUi: ShowDiffPanel called with invalid parameters");
        return;
    }

    if (!m_Docking || !m_Ui)
    {
        Logger::Log::Warning("EditorVersionControlUi: Docking or UI not set when showing diff panel");
        return;
    }

    // Get original content from the active provider (empty when the provider
    // has no base-content primitive).
    std::string originalContent;
    Editor::EditorVcsProviderDescriptor provider;
    if (Editor::EditorVcsProviderRegistry::Get().TryGetActiveProvider(provider) &&
        provider.GetBaseContent)
    {
        originalContent = provider.GetBaseContent(filePath);
    }

    // Get current content from disk
    std::string currentContent;
    if (std::filesystem::exists(filePath))
    {
        std::ifstream file(filePath, std::ios::binary);
        if (file.is_open())
        {
            std::stringstream buffer;
            buffer << file.rdbuf();
            currentContent = buffer.str();
            file.close();
        }
    }

    // Open diff panel
    if (diffPanel->OpenDiff(filePath, originalContent, currentContent))
    {
        // Activate/show the panel
        ActivateDiffPanel(m_Docking, m_Ui, diffPanel);
    }
    else
    {
        Logger::Log::Warning("EditorVersionControlUi: Failed to open diff for file: {}", filePath.string());
    }
}

void EditorVersionControlUi::ActivateDiffPanel(DockingManager* docking, UIManager* ui, DiffPanel* diffPanel)
{
    if (!docking || !ui || !diffPanel)
    {
        Logger::Log::Warning("EditorVersionControlUi: ActivateDiffPanel called with null parameters");
        return;
    }

    // Ensure panel is registered
    docking->RegisterPanel(EditorPanelIds::Diff, diffPanel);

    // Try to activate the panel if it already exists in the tree
    if (docking->ActivateTab(EditorPanelIds::Diff))
    {
        Logger::Log::Info("EditorVersionControlUi: Diff panel activated (already in layout)");
        // Panel exists and was activated, rebuild the dockspace to reflect the activation
        if (auto* rootEl = ui->GetRootElement())
        {
            if (auto* el = rootEl->FindById("dock"))
            {
                if (auto* ds = dynamic_cast<DockspaceElement*>(el))
                {
                    ds->RequestRebuildFromModel();
                }
            }
        }
    }
    else
    {
        Logger::Log::Info("EditorVersionControlUi: Adding Diff panel to layout");
        // Panel doesn't exist in the layout, add it to the middle area (where SceneView is)
        if (DockNode* root = docking->GetRoot())
        {
            // Try to find the leaf containing SceneView (the middle area)
            DockNode* targetLeaf = FindLeafWithTab(root, EditorPanelIds::SceneView);
            if (!targetLeaf)
            {
                // Fallback to GameView if SceneView not found
                targetLeaf = FindLeafWithTab(root, "GameView");
            }
            if (!targetLeaf)
            {
                // Last resort: use first available leaf
                targetLeaf = FindFirstLeaf(root);
            }
            
            if (targetLeaf)
            {
                targetLeaf->AddTab(EditorPanelIds::Diff);
                docking->ActivateTab(EditorPanelIds::Diff);

                // Rebuild the dockspace to show the new panel
                if (auto* rootEl = ui->GetRootElement())
                {
                    if (auto* el = rootEl->FindById("dock"))
                    {
                        if (auto* ds = dynamic_cast<DockspaceElement*>(el))
                        {
                            ds->RequestRebuildFromModel();
                        }
                    }
                }
                Logger::Log::Info("EditorVersionControlUi: Diff panel added and activated");
            }
            else
            {
                Logger::Log::Warning("EditorVersionControlUi: No leaf node found to add Diff panel");
            }
        }
        else
        {
            Logger::Log::Warning("EditorVersionControlUi: Docking manager has no root node");
        }
    }
}

DockNode* EditorVersionControlUi::FindFirstLeaf(DockNode* node)
{
    if (!node)
        return nullptr;
    
    if (node->IsLeaf())
        return node;
    
    if (node->First())
    {
        if (auto* leaf = FindFirstLeaf(node->First()))
            return leaf;
    }
    
    if (node->Second())
    {
        if (auto* leaf = FindFirstLeaf(node->Second()))
            return leaf;
    }
    
    return nullptr;
}

DockNode* EditorVersionControlUi::FindLeafWithTab(DockNode* node, const std::string& tabId)
{
    if (!node)
        return nullptr;
    
    if (node->IsLeaf())
    {
        // Check if this leaf contains the tab we're looking for
        const auto& tabs = node->GetTabs();
        for (const auto& tab : tabs)
        {
            if (tab == tabId)
                return node;
        }
        return nullptr;
    }
    
    // Recurse into children
    if (node->First())
    {
        if (auto* leaf = FindLeafWithTab(node->First(), tabId))
            return leaf;
    }
    
    if (node->Second())
    {
        if (auto* leaf = FindLeafWithTab(node->Second(), tabId))
            return leaf;
    }
    
    return nullptr;
}

} // namespace GameEngine
