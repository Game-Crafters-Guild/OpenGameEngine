#include "Search/UniversalSearchController.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "EditorPanelIds.h"
#include "EditorPanelManager.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/UniversalSearchSettings.h"
#include "UndoRedo/UndoRedoService.h"
#include "Panels/AssetsPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/ProjectFolderPickerModal.h"
#include "Panels/ScriptEditorPanel.h"
#include "Panels/SettingsPanel.h"
#include "Platform/Shell.h"
#include "PlayMode/PlayModeManager.h"
#include "Scene/SceneEditorController.h"
#include "Scripting/CoreCLRHost.h"
#include "Scripting/EditorScriptMenuRegistry.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Layout/Docking.h"
#include "UI/UniversalSearchProvider.h"

#include <any>
#include <algorithm>

namespace GameEngine
{
namespace Editor
{
namespace
{
SearchIcon Icon(const char* cssClass)
{
    return SearchIcon::FromClass(cssClass);
}

std::string ScriptProviderName(const std::string& path)
{
    const std::size_t slash = path.find('/');
    return "Script · " + path.substr(0, slash);
}
} // namespace

UniversalSearchController::UniversalSearchController() = default;
UniversalSearchController::~UniversalSearchController() = default;

void UniversalSearchController::Initialize(const Dependencies& deps)
{
    if (m_Dialog || !deps.MainWindow || !deps.MainWindow->ui || !deps.MainWindow->ui->GetRootElement())
        return;
    m_Deps = deps;

    m_Provider = std::make_unique<UniversalSearchProvider>(EngineCore::GetInstance().GetJobSystem());
    auto& provider = *m_Provider;
    provider.SetAssetRegistry(&EngineCore::GetInstance().GetAssetManager().GetRegistry());
    provider.SetWorld(m_Deps.GetWorld ? m_Deps.GetWorld() : nullptr);

    provider.SetOnAssetSelected([this](const std::filesystem::path& path)
    {
        if (!m_Deps.Assets || path.empty())
            return;
        if (m_Deps.PanelManager)
            m_Deps.PanelManager->ShowOrActivatePanel(m_Deps.MainWindow, EditorPanelIds::Assets,
                                                     EditorPanelIds::SceneView);
        m_Deps.Assets->NavigateToAndSelectAsset(path);
    });
    provider.SetOnEntitySelected([this](ECS::EntityHandle entity)
    {
        ECS::World* world = m_Deps.GetWorld ? m_Deps.GetWorld() : nullptr;
        if (!m_Deps.Hierarchy || !world || !entity.IsValid() || !world->IsValid(entity))
            return;
        if (m_Deps.PanelManager)
            m_Deps.PanelManager->ShowOrActivatePanel(m_Deps.MainWindow, EditorPanelIds::Hierarchy, {});
        m_Deps.Hierarchy->SelectEntity(entity);
    });
    provider.SetOnCodeSelected([this](const std::filesystem::path& path, std::size_t line)
    {
        if (path.empty())
            return;
        if (m_Deps.ScriptEditor)
        {
            if (m_Deps.PanelManager)
                m_Deps.PanelManager->ShowOrActivatePanel(m_Deps.MainWindow, EditorPanelIds::ScriptEditor,
                                                         EditorPanelIds::SceneView);
            if (m_Deps.ScriptEditor->OpenScript(path))
            {
                m_Deps.ScriptEditor->ScrollToLine(std::max<std::size_t>(line, 1));
                return;
            }
        }
        Platform::OpenPath(path);
    });

    RegisterCommands();
    RegisterPanels();
    RegisterSettingsCategory();

    auto dialog = std::make_unique<SearchDialog>();
    m_Dialog = dialog.get();
    dialog->SetId("universal-search-command-palette");
    dialog->AddClass("universal-command-palette");
    dialog->SetProvider(&provider);
    dialog->SetPanelWidth(680.0f);
    dialog->SetFilterOptions({
        {"all", "All fields", ""},
        {"commands", "Commands", ">"},
        {"entities", "Entities", "@"},
        {"panels", "Panels", "/"},
        {"settings", "Settings", ":"},
        {"assets", "Assets", "asset:"},
        {"code", "All code", "code:"},
        {"files", "Files", "file:"},
        {"classes", "Classes", "class:"},
        {"functions", "Functions", "function:"},
        {"symbols", "Symbols", "symbol:"},
        {"comments", "Comments", "comment:"},
    });
    dialog->SetMaxListHeight(432.0f);
    dialog->SetFixedHeight(true);
    dialog->SetCentered(true);
    dialog->SetManipulationEnabled(true);
    dialog->SetShouldCloseOnResult([]()
    {
        return !UniversalSearchSettings::Get().GetKeepOpenAfterSelection();
    });
    dialog->SetOnResult([](const SearchResultItem& item)
    {
        try
        {
            const auto& action = std::any_cast<const std::function<void()>&>(item.UserData);
            if (action) action();
        }
        catch (const std::bad_any_cast&) {}
    });
    m_Deps.MainWindow->ui->GetRootElement()->AddChild(std::move(dialog));

    if (m_Deps.GetProjectRoot)
        provider.StartCodeIndex(m_Deps.GetProjectRoot());
}

void UniversalSearchController::RegisterCommands()
{
    auto addCommand = [this](std::string label, std::string detail, std::string keywords,
                             std::string shortcut, SearchIcon icon, int priority,
                             std::function<void()> execute)
    {
        UniversalSearchEntry entry;
        entry.Kind = UniversalSearchKind::Command;
        entry.Label = std::move(label);
        entry.Detail = std::move(detail);
        if (!shortcut.empty())
            entry.Detail += entry.Detail.empty() ? shortcut : " · " + shortcut;
        entry.Keywords = std::move(keywords);
        entry.Provider = "Editor";
        entry.Icon = icon.HasAny() ? std::move(icon) : Icon("icon-command");
        entry.Priority = priority;
        entry.Execute = std::move(execute);
        m_Provider->AddEntry(std::move(entry));
    };

    addCommand("Save Scene", "File", "write persist scene", "Ctrl/Cmd+S",
               Icon("icon-command-save"), 100,
               [this]() { if (m_Deps.SceneEditor) m_Deps.SceneEditor->RequestSaveScene(); });
    addCommand("New Scene", "File", "create empty level world", {},
               Icon("icon-command-new-scene"), 70,
               [this]() { if (m_Deps.SceneEditor) m_Deps.SceneEditor->RequestNewScene(); });
    addCommand("Open Project", "File", "switch choose folder workspace", {},
               Icon("icon-command-open-project"), 65,
               [this]() { if (m_Deps.ProjectFolderModal) m_Deps.ProjectFolderModal->Show(); });
    addCommand("Undo", "Edit", "history revert previous change", "Ctrl/Cmd+Z",
               Icon("icon-command-undo"), 95,
               [this]() { if (m_Deps.UndoRedo) m_Deps.UndoRedo->Undo(); });
    addCommand("Redo", "Edit", "history repeat restore change", "Ctrl/Cmd+Y",
               Icon("icon-command-redo"), 90,
               [this]() { if (m_Deps.UndoRedo) m_Deps.UndoRedo->Redo(); });
    addCommand("Play", "Play Mode", "run start game simulation", "Ctrl/Cmd+P",
               Icon("icon-command-play"), 85,
               [this]()
               {
                   if (m_Deps.PlayMode && m_Deps.PlayMode->GetState() == PlayModeState::Edit)
                       m_Deps.PlayMode->EnterPlayMode();
               });
    addCommand("Pause", "Play Mode", "suspend freeze simulation", {},
               Icon("icon-command-pause"), 70,
               [this]()
               {
                   if (m_Deps.PlayMode && m_Deps.PlayMode->IsPlayingOrPaused())
                       m_Deps.PlayMode->TogglePause();
               });
    addCommand("Stop", "Play Mode", "exit end simulation", {},
               Icon("icon-command-stop"), 75,
               [this]()
               {
                   if (!m_Deps.PlayMode)
                       return;
                   if (m_Deps.PlayMode->GetState() == PlayModeState::ChangeReview)
                       m_Deps.PlayMode->DiscardPendingChanges();
                   else if (m_Deps.PlayMode->GetState() != PlayModeState::Edit)
                       m_Deps.PlayMode->ExitPlayMode();
               });
    addCommand("Reset Editor Layout", "Window / Layout", "restore default panels docking", {},
               Icon("icon-command-reset-layout"), 40,
               [this]() { if (m_Deps.ResetLayout) m_Deps.ResetLayout(); });
    addCommand("Reindex Project Code", "Search", "refresh files symbols classes functions comments", {},
               Icon("icon-command-reindex"), 20,
               [this]()
               {
                   if (m_Provider && m_Deps.GetProjectRoot)
                       m_Provider->RestartCodeIndex(m_Deps.GetProjectRoot());
               });
}

void UniversalSearchController::RegisterPanels()
{
    if (!m_Deps.PanelManager)
        return;
    /* One inventory for every panel launcher: the Window menu, the hamburger
       list and search all read EditorPanelManager::GetPanelMenuEntries(), so
       whether a panel is listed — and its title and icon — is decided in one
       place. Search does not know which panels exist or why one is hidden.
       Indexed as it stands at initialization; panels registered later do not
       appear until the next editor launch. */
    for (const EditorPanelManager::PanelMenuEntry& panel : m_Deps.PanelManager->GetPanelMenuEntries())
    {
        UniversalSearchEntry entry;
        entry.Kind = UniversalSearchKind::Panel;
        entry.Label = panel.title;
        entry.Detail = "Window · " + panel.panelId;
        entry.Keywords = "window panel dock " + panel.panelId;
        entry.Provider = "Editor Panels";
        // The panel's own tab icon when the inventory resolved one, so a result
        // is recognisable; the generic panel glyph otherwise.
        entry.Icon =
            panel.iconClass.empty() ? Icon("icon-panel") : Icon(panel.iconClass.c_str());
        entry.Execute = [this, panelId = panel.panelId]()
        {
            if (m_Deps.PanelManager)
                m_Deps.PanelManager->ShowOrActivatePanel(m_Deps.MainWindow, panelId, {});
        };
        m_Provider->AddEntry(std::move(entry));
    }
}

// The palette owns the editor's Universal Search settings category; the
// SettingsPanel renders it from this registration.
void UniversalSearchController::RegisterSettingsCategory()
{
    Editor::SettingsCategoryDescriptor category;
    category.CategoryId = "universalSearch";
    category.Title = "Universal Search";
    category.Group = Editor::SettingsCategoryGroup::UserSettings;
    category.TreeRowClass = "universal-search-row";
    category.Description = "The Universal Search palette (Ctrl/Cmd+K) finds commands, panels, "
                           "settings, assets, entities, and project code.";

    Editor::SettingsFieldDescriptor keepOpen;
    keepOpen.Label = "Keep Universal Search Open";
    keepOpen.Tooltip = "Keep the Universal Search panel open after activating a result.";
    keepOpen.SearchKeywords = "universal search command palette result selection stay open close";
    Editor::SettingsFieldDescriptor::ToggleField toggle;
    toggle.Get = []() { return UniversalSearchSettings::Get().GetKeepOpenAfterSelection(); };
    toggle.Set = [](bool keep) { UniversalSearchSettings::Get().SetKeepOpenAfterSelection(keep); };
    keepOpen.Control = toggle;
    category.Fields.push_back(std::move(keepOpen));

    Editor::EditorSettingsRegistry::Get().RegisterCategory(std::move(category));
}

// Settings entries are rebuilt on every palette open so categories registered
// after startup (package modules load at project open) are searchable too.
void UniversalSearchController::RefreshSettingsEntries()
{
    std::vector<UniversalSearchEntry> settingsEntries;
    if (m_Deps.Settings)
    {
        for (const SearchableSettingItem& setting : m_Deps.Settings->GetSearchableItems())
        {
            UniversalSearchEntry entry;
            entry.Kind = UniversalSearchKind::Setting;
            entry.Label = setting.Label;
            entry.Detail = "Editor Settings";
            entry.Keywords = setting.Keywords;
            entry.Provider = "Settings";
            entry.Icon = Icon("icon-settings");
            entry.Execute = [this, category = setting.Category]()
            {
                if (m_Deps.PanelManager)
                    m_Deps.PanelManager->ShowOrActivatePanel(m_Deps.MainWindow, EditorPanelIds::Settings, {});
                if (m_Deps.Settings)
                    m_Deps.Settings->RequestShowCategory(category);
            };
            settingsEntries.push_back(std::move(entry));
        }
    }
    m_Provider->SetSettingsEntries(std::move(settingsEntries));
}

void UniversalSearchController::RefreshScriptEntries()
{
    std::vector<UniversalSearchEntry> scriptEntries;
    for (const auto& script : ScriptMenuRegistry::Get().GetToolbarItems())
    {
        UniversalSearchEntry entry;
        entry.Kind = UniversalSearchKind::Command;
        const std::size_t slash = script.path.find_last_of('/');
        entry.Label = slash == std::string::npos ? script.path : script.path.substr(slash + 1);
        entry.Detail = script.path;
        entry.Keywords = script.method;
        entry.Provider = ScriptProviderName(script.path);
        entry.Icon = Icon("icon-command");
        entry.Priority = -10;
        entry.Execute = [commandId = script.commandId]()
        {
            uint64_t domain = 0;
            std::string method;
            if (!ScriptMenuRegistry::Get().TryResolveCommand(commandId, domain, method) || method.empty())
                return;
            try
            {
                auto& clr = EngineCore::GetInstance().GetScriptManager().GetCLRHost();
                int32_t result = 0;
                (void)clr.InvokeInDomain(domain, method.c_str(), static_cast<uint32_t>(method.size()), &result);
            }
            catch (...) {}
        };
        scriptEntries.push_back(std::move(entry));
    }
    m_Provider->SetScriptEntries(std::move(scriptEntries));
}

void UniversalSearchController::Toggle()
{
    if (!m_Dialog || !m_Provider)
        return;
    if (m_Dialog->IsOpen())
    {
        m_Dialog->Close();
        return;
    }

    RefreshScriptEntries();
    RefreshSettingsEntries();
    m_Provider->SetWorld(m_Deps.GetWorld ? m_Deps.GetWorld() : nullptr);
    if (m_Deps.GetProjectRoot)
        m_Provider->StartCodeIndex(m_Deps.GetProjectRoot());
    m_Provider->RefreshRuntimeEntries();
    m_Dialog->Show();
}

void UniversalSearchController::OnProjectRootChanged(const std::filesystem::path& projectRoot)
{
    if (m_Provider)
        m_Provider->RestartCodeIndex(projectRoot);
}

bool UniversalSearchController::IsOpen() const
{
    return m_Dialog && m_Dialog->IsOpen();
}

} // namespace Editor
} // namespace GameEngine
