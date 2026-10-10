#include "UI/Controls/EditorTopToolbar.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "EditorPanelManager.h"
#include "Editor/Settings/SettingsStore.h"
#include "EditorPanelIds.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Input/InputSystem.h"
#include "Platform/ContextMenu.h"
#include "UI/EditorIcons.h"
#include "Platform/Window.h"
#include "PlayMode/PlayModeManager.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/ClassBackgroundImageQuery.h"
#include "UI/Controls/Button.h"
#include "UI/Interaction/ContextMenuManipulator.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ToolbarDragDrop.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{
struct ToolbarButtonInfo
{
    const char* id;
    const char* title;
    const char* iconPath;
};

// Friendly titles for the right-click "show/hide icon" menu. Keep in sync with
// the buttons declared in EditorTopToolbar.uxml (top-toolbar-left/center/right).
constexpr ToolbarButtonInfo kToolbarButtons[] = {
    {"TopToolbarUniversalSearch", "Universal Search",    "editor:Icons/usearch.png"},
    {"TopToolbarLeft1",          "Save Scene",          "editor:Icons/save.png"},
    {"TopToolbarLeft2",          "Open Project Folder", "editor:Icons/folder_open.png"},
    {"TopToolbarPackages",       "Packages",            "editor:Icons/box.png"},
    {"TopToolbarLeft3",          "Web Panel",           "editor:Icons/computer.png"},
    {"TopToolbarTodos",          "Todos",               "editor:Icons/todolist.png"},
    {"TopToolbarBookmarks",      "Bookmarks",           "editor:Icons/book.png"},
    {"TopToolbarBuild",          "Build",               "editor:Icons/game-controller.png"},
    {"TopToolbarSettings",       "Settings",            "editor:Icons/settings.png"},
    {"TopToolbarHelp",           "Help",                "editor:Icons/help.png"},
    {"TopToolbarCenter2",        "Stop",                "editor:Icons/stop.png"},
    {"TopToolbarRecord",         "Record",              "editor:Icons/recording_off_button.png"},
    {"TopToolbarCenter3",        "Pause",               "editor:Icons/pause.png"},
    {"TopToolbarCenter4",        "Play",                "editor:Icons/play.png"},
    {"TopToolbarCenter1",        "Play Fullscreen",     "editor:Icons/fullscreen.png"},
    {"TopToolbarUndoHistory",    "Undo History",        "editor:Icons/undo.png"},
    {"TopToolbarRight1",         "Script Editor",       "editor:Icons/log.png"},
    {"TopToolbarNodeGraph",      "Node Graph",          "editor:Icons/node.png"},
    {"TopToolbarMixer",          "Mixer",               "editor:Icons/mixer.png"},
    {"TopToolbarAnimation",      "Animation",           "editor:Icons/alarm.png"},
    {"TopToolbarTimeline",       "Timeline",            "editor:Icons/film.png"},
    {"TopToolbarVisualProfiler", "Visual Profiler",     "editor:Icons/pulse.png"},
    {"TopToolbarCpuProfiler",    "CPU Profiler",        "editor:Icons/timer.png"},
    {"TopToolbarMonitors",       "Monitors",            "editor:Icons/stats.png"},
    {"TopToolbarVram",           "VRAM",                "editor:Icons/hardware-chip.png"},
    {"TopToolbarRenderGraph",    "Render Graph",        "editor:Icons/pie-chart.png"},
    {"TopToolbarLog",            "Log",                 "editor:Icons/alert-circle.png"},
    {"TopToolbarRight2",         "Info",                "editor:Icons/info.png"},
    {"TopToolbarPanelMenu",      "Panels",              "editor:Icons/menulist.png"},
};

std::string MakeVisibilityPrefKey(const char* buttonId)
{
    std::string key = "ui.toolbar.hidden.";
    key += buttonId;
    return key;
}

bool CanHideToolbarButton(std::string_view buttonId)
{
    return buttonId != EditorPanelIds::TopToolbarPanelMenu;
}

// Element that owns a section's icon run, gaps included. Authored in
// EditorTopToolbar.uxml, sized by CSS to hug its buttons.
constexpr const char* kIconRunClass = "toolbar-icons";

void SetButtonHiddenInPrefs(const char* buttonId, bool hidden)
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetBool(MakeVisibilityPrefKey(buttonId), hidden);
    (void)prefs.Save(&err);
}

void ApplyHiddenClass(UIElement* el, bool hidden)
{
    if (!el)
        return;
    if (hidden)
    {
        if (!el->HasClass("hidden"))
            el->AddClass("hidden");
    }
    else
    {
        if (el->HasClass("hidden"))
            el->RemoveClass("hidden");
    }
    el->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
}

UIElement* FindFirstByClass(UIElement* el, const char* className)
{
    if (!el)
        return nullptr;
    if (el->HasClass(className))
        return el;
    for (const auto& child : el->GetChildren())
        if (auto* found = FindFirstByClass(child.get(), className))
            return found;
    return nullptr;
}

// The launcher button's own mark, worn by a row for a panel that declares no
// icon class of its own. Authored in CSS like every other icon, so the class
// name — not an image path — is what appears here.
constexpr const char* kPanelMenuGenericIconClass = "menulist-icon";

std::string PanelMenuIconPath(const UIManager& ui, std::string_view iconClass)
{
    // Panel launcher rows belong to the platform context menu, not the retained
    // UI tree, so no stylesheet can style them. The image each row shows is
    // still authored in CSS — `background-image` on the panel's tab-icon class
    // — and is read back from the loaded cascade, so the URL exists in exactly
    // one place. An empty result means CSS declared nothing for either class,
    // and a row with no icon beats a row with a guessed one.
    std::string url = UIStyleQuery::ResolveClassBackgroundImageUrl(ui.GetStylesheets(), iconClass);
    if (url.empty())
        url = UIStyleQuery::ResolveClassBackgroundImageUrl(ui.GetStylesheets(),
                                                          kPanelMenuGenericIconClass);
    return url;
}
} // namespace

EditorTopToolbar::EditorTopToolbar()
{
    SetId("EditorTopToolbar");
    AddClass("editor-top-toolbar");
}

EditorTopToolbar::~EditorTopToolbar() = default;

bool EditorTopToolbar::LoadAssets()
{
    if (m_AssetsLoaded)
        return true;
    if (m_AssetLoadFailed || !GetOwnerManager())
        return false;

    auto& assets = EngineCore::GetInstance().GetAssetManager();
    const GUID layoutGuid = assets.ResolveAssetGuid("UI/controls/EditorTopToolbar.uxml", "Editor");
    const GUID styleGuid = assets.ResolveAssetGuid("UI/controls/EditorTopToolbar.css", "Editor");
    if (layoutGuid.IsNull() || styleGuid.IsNull())
    {
        Logger::Log::Error("EditorTopToolbar: failed to resolve toolbar UI assets");
        m_AssetLoadFailed = true;
        return false;
    }

    // Waits on purpose: the editor's own .uxml and .css, small and never cooked, read once at startup.
    auto layoutAsset = assets.LoadAssetAsync(layoutGuid, AssetLoadPriority::High).get();
    auto styleAsset = assets.LoadAssetAsync(styleGuid, AssetLoadPriority::High).get();
    UIManager* ui = GetOwnerManager();
    if (!layoutAsset || layoutAsset->GetType() != AssetType::UILayout ||
        !styleAsset || styleAsset->GetType() != AssetType::UIStyle || !ui ||
        !ui->BindLayoutToSubtreeChildrenFromAsset(
            this, *static_cast<UILayoutAsset*>(layoutAsset.get())) ||
        !ui->AttachStyleToSubtreeFromAsset(
            this, *static_cast<UIStyleAsset*>(styleAsset.get())))
    {
        Logger::Log::Error("EditorTopToolbar: failed to load toolbar UI assets");
        m_AssetLoadFailed = true;
        return false;
    }

    m_AssetsLoaded = true;
    return true;
}

void EditorTopToolbar::OnPostLayout()
{
    if (!m_AssetsLoaded && !m_AssetLoadFailed)
        ScheduleAssetLoad();
}

void EditorTopToolbar::ScheduleAssetLoad()
{
    if (m_AssetLoadScheduled)
        return;
    m_AssetLoadScheduled = true;
    PostAction([this]()
    {
        m_AssetLoadScheduled = false;
        (void)LoadAssets();
    });
}

void EditorTopToolbar::WireControls(UIElement* rootEl, Platform::Window* window, const Callbacks& cb)
{
    if (!rootEl)
        return;

    // Universal Search launcher (same palette as Ctrl/Cmd+K).
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarUniversalSearch)))
    {
        btn->SetTooltip("Universal Search (Ctrl/Cmd+K)");
        if (cb.onUniversalSearchClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onUniversalSearchClicked](UIEvent&) { fn(); });
    }

    // Panel launcher. Its contents are queried on every click so panels
    // contributed by packages after startup are included automatically.
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarPanelMenu)))
    {
        btn->SetTooltip("Panels");
        if (window && cb.getPanelMenuEntries && cb.onPanelMenuEntryClicked)
        {
            btn->RegisterEventHandler(kEventButtonClick, [this, window, getEntries = cb.getPanelMenuEntries,
                             openPanel = cb.onPanelMenuEntryClicked](UIEvent& e)
            {
                UIElement& clicked = *e.CurrentTarget;
                auto entries = getEntries();
                std::sort(entries.begin(), entries.end(), [](const PanelMenuEntry& a, const PanelMenuEntry& b)
                {
                    if (a.title != b.title)
                        return a.title < b.title;
                    return a.panelId < b.panelId;
                });
                if (entries.empty())
                    return;

                m_PanelMenu = CreateContextMenu();
                if (!m_PanelMenu)
                    return;

                const UIManager* ui = GetOwnerManager();
                for (uint32_t i = 0; i < entries.size(); ++i)
                {
                    const uint32_t commandId = i + 1;
                    m_PanelMenu->AddItem(0, entries[i].title, commandId);
                    if (ui)
                        m_PanelMenu->SetItemIcon(commandId,
                                                 PanelMenuIconPath(*ui, entries[i].iconClass));
                }
                m_PanelMenu->SetCommandHandler(
                    [entries = std::move(entries), openPanel](uint32_t commandId)
                    {
                        if (commandId == 0 || commandId > entries.size())
                            return;
                        openPanel(entries[commandId - 1].panelId);
                    });

                const int x = static_cast<int>(clicked.GetLayoutX());
                const int y = static_cast<int>(clicked.GetLayoutY() + clicked.GetLayoutHeight());
                m_PanelMenu->Show(window, x, y);
            });
        }
    }

    // Settings button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarSettings)))
    {
        btn->SetTooltip("Settings");
        if (cb.onSettingsClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onSettingsClicked](UIEvent&) { fn(); });
    }

    // Help button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarHelp)))
    {
        btn->SetTooltip("Help");
        btn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            if (m_PanelManager)
                m_PanelManager->ShowOrActivateHelpPanel();
        });
    }

    // Node Graph button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarNodeGraph)))
    {
        btn->SetTooltip("Node Graph");
        if (cb.onNodeGraphClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onNodeGraphClicked](UIEvent&) { fn(); });
    }

    // Bookmarks button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarBookmarks)))
    {
        btn->SetTooltip("Bookmarks");
        if (cb.onBookmarksClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onBookmarksClicked](UIEvent&) { fn(); });
    }

    // Fullscreen toggle button (play fullscreen preference)
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById("TopToolbarCenter1")))
    {
        btn->SetTooltip("Play Fullscreen");
        m_FullscreenButton = btn;
        if (cb.onFullscreenToggleClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onFullscreenToggleClicked](UIEvent&) { fn(); });
    }

    // Stop button
    if (auto* stopBtn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarCenter2)))
    {
        stopBtn->SetTooltip("Stop");
        m_StopButton = stopBtn;
        if (cb.onStopClicked)
            stopBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onStopClicked](UIEvent&) { fn(); });
    }

    // Movie record shortcut
    if (auto* recordBtn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarRecord)))
    {
        recordBtn->SetTooltip("Record");
        m_RecordButton = recordBtn;
        if (cb.onRecordClicked)
            recordBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onRecordClicked](UIEvent&) { fn(); });
    }

    // Pause button
    if (auto* pauseBtn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarCenter3)))
    {
        pauseBtn->SetTooltip("Pause");
        m_PauseButton = pauseBtn;
        if (cb.onPauseClicked)
            pauseBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onPauseClicked](UIEvent&) { fn(); });
    }

    // Play button
    if (auto* playBtn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarCenter4)))
    {
        playBtn->SetTooltip("Play");
        m_PlayButton = playBtn;
        if (cb.onPlayClicked)
            playBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onPlayClicked](UIEvent&) { fn(); });
    }

    // Save scene (same as Editor.Global.SaveScene: save to path, or Save As modal if untitled)
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarLeft1)))
    {
        btn->SetTooltip("Save Scene");
        m_SaveButton = btn;
        if (cb.onSaveSceneClicked)
        {
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onSaveSceneClicked](UIEvent& e) { fn(e.Mods); });
            std::vector<ContextMenuManipulator::Item> saveItems = {
                {.Path = "Save",
                 .IconPath = EditorIcons::kSave,
                 .OnActivate = [fn = cb.onSaveSceneClicked] { fn(0); }},
                {.Path = "Save As...",
                 .IconPath = EditorIcons::kSave,
                 .OnActivate = [fn = cb.onSaveSceneClicked] { fn(Input::kModShift); }},
            };
            if (cb.onRevertSceneClicked)
            {
                saveItems.push_back({.Separator = true});
                saveItems.push_back({
                    .Path = "Revert Scene",
                    .IconPath = EditorIcons::kReset,
                    .OnActivate = [fn = cb.onRevertSceneClicked] { fn(); },
                });
            }
            btn->AddManipulator(ContextMenuManipulator::Create(std::move(saveItems)));
            btn->SetTooltip("Save Scene. Right-click for options.");
        }
    }

    // Project folder button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarLeft2)))
    {
        btn->SetTooltip("Open Project Folder");
        if (cb.onProjectFolderClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onProjectFolderClicked](UIEvent&) { fn(); });
    }

    // Web panel button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarLeft3)))
    {
        btn->SetTooltip("Web Panel");
        if (cb.onWebClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onWebClicked](UIEvent&) { fn(); });
    }

    // Script Editor button (with legacy id fallback)
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarRight1)))
    {
        btn->SetTooltip("Script Editor");
        if (cb.onScriptEditorClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onScriptEditorClicked](UIEvent&) { fn(); });
    }
    else if (auto* legacyBtn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarScriptEditor)))
    {
        legacyBtn->SetTooltip("Script Editor");
        if (cb.onScriptEditorClicked)
            legacyBtn->RegisterEventHandler(kEventButtonClick, [fn = cb.onScriptEditorClicked](UIEvent&) { fn(); });
    }

    // Todos button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarTodos)))
    {
        btn->SetTooltip("Todos");
        if (cb.onTodosClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onTodosClicked](UIEvent&) { fn(); });
    }

    // Undo History button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarUndoHistory)))
    {
        btn->SetTooltip("Undo History");
        if (cb.onUndoHistoryClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onUndoHistoryClicked](UIEvent&) { fn(); });
    }

    // Packages button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarPackages)))
    {
        btn->SetTooltip("Packages");
        if (cb.onPackagesClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onPackagesClicked](UIEvent&) { fn(); });
    }

    // Build button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarBuild)))
    {
        btn->SetTooltip("Build");
        if (cb.onBuildClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onBuildClicked](UIEvent&) { fn(); });
    }

    // Mixer button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarMixer)))
    {
        btn->SetTooltip("Mixer");
        if (cb.onMixerClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onMixerClicked](UIEvent&) { fn(); });
    }

    // Visual Profiler button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarVisualProfiler)))
    {
        btn->SetTooltip("Visual Profiler");
        if (cb.onVisualProfilerClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onVisualProfilerClicked](UIEvent&) { fn(); });
    }

    // Monitors button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarMonitors)))
    {
        btn->SetTooltip("Monitors");
        if (cb.onMonitorsClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onMonitorsClicked](UIEvent&) { fn(); });
    }

    // Render Graph button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarRenderGraph)))
    {
        btn->SetTooltip("Render Graph");
        if (cb.onRenderGraphClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onRenderGraphClicked](UIEvent&) { fn(); });
    }

    // Animation button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarAnimation)))
    {
        btn->SetTooltip("Animation");
        if (cb.onAnimationClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onAnimationClicked](UIEvent&) { fn(); });
    }

    // Timeline button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarTimeline)))
    {
        btn->SetTooltip("Timeline");
        if (cb.onTimelineClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onTimelineClicked](UIEvent&) { fn(); });
    }

    // Log button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarLog)))
    {
        btn->SetTooltip("Log");
        if (cb.onLogClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onLogClicked](UIEvent&) { fn(); });
    }

    // VRAM button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarVram)))
    {
        btn->SetTooltip("VRAM");
        if (cb.onVramClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onVramClicked](UIEvent&) { fn(); });
    }

    // CPU Profiler button
    if (auto* btn = dynamic_cast<Button*>(rootEl->FindById(EditorPanelIds::TopToolbarCpuProfiler)))
    {
        btn->SetTooltip("CPU Profiler");
        if (cb.onCpuProfilerClicked)
            btn->RegisterEventHandler(kEventButtonClick, [fn = cb.onCpuProfilerClicked](UIEvent&) { fn(); });
    }
}

void EditorTopToolbar::UpdatePlayModeState(Editor::PlayModeState s)
{
    // Toolbar behavior:
    // - Play: only enabled in Edit (Enter play). Disabled while playing/paused/review.
    //         Keep the green "active" icon while in play/paused/entering/exiting.
    // - Pause: enabled only while playing/paused; pressed styling indicates paused.
    // - Stop: enabled while playing/paused/review; disabled in Edit.
    if (m_PlayButton)
    {
        const bool enablePlay = (s == Editor::PlayModeState::Edit);
        const bool playActive =
            (s == Editor::PlayModeState::Play ||
             s == Editor::PlayModeState::Paused ||
             s == Editor::PlayModeState::EnteringPlay ||
             s == Editor::PlayModeState::ExitingPlay);
        m_PlayButton->SetDisabled(!enablePlay);
        if (playActive)
            m_PlayButton->AddClass("pressed");
        else
            m_PlayButton->RemoveClass("pressed");
    }
    if (m_PauseButton)
    {
        const bool enablePause =
            (s == Editor::PlayModeState::Play || s == Editor::PlayModeState::Paused);
        m_PauseButton->SetDisabled(!enablePause);
        if (s == Editor::PlayModeState::Paused)
            m_PauseButton->AddClass("pressed");
        else
            m_PauseButton->RemoveClass("pressed");
    }
    if (m_StopButton)
    {
        const bool enableStop =
            (s == Editor::PlayModeState::Play ||
             s == Editor::PlayModeState::Paused ||
             s == Editor::PlayModeState::ChangeReview ||
             s == Editor::PlayModeState::EnteringPlay ||
             s == Editor::PlayModeState::ExitingPlay);
        m_StopButton->SetDisabled(!enableStop);
    }
}

void EditorTopToolbar::SetSaveDirty(bool dirty)
{
    if (!m_SaveButton)
        return;

    if (dirty)
        m_SaveButton->AddClass("save-dirty-active");
    else
        m_SaveButton->RemoveClass("save-dirty-active");
    m_SaveButton->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);

    for (const auto& child : m_SaveButton->GetChildren())
    {
        if (child && child->HasClass("save-dirty-indicator"))
        {
            if (dirty)
                child->RemoveClass("hidden");
            else
                child->AddClass("hidden");
            child->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            break;
        }
    }
}

void EditorTopToolbar::SetFullscreenActive(bool active)
{
    if (!m_FullscreenButton)
        return;
    if (active)
        m_FullscreenButton->AddClass("pressed");
    else
        m_FullscreenButton->RemoveClass("pressed");
}

void EditorTopToolbar::SetRecordActive(bool active)
{
    if (!m_RecordButton)
        return;
    if (active)
        m_RecordButton->AddClass("active");
    else
        m_RecordButton->RemoveClass("active");
}

void EditorTopToolbar::SetupDragDrop(UIElement* rootEl, Editor::ToolbarDragDrop* dragDrop)
{
    if (!dragDrop || !rootEl)
        return;

    static const std::vector<Editor::ToolbarDragDrop::ContainerConfig> kTopToolbarContainers = {
        // Buttons live in the hugging icon container; the stretched section
        // around it is the drop zone, so a drag can land in empty bar space.
        {"toolbar-icons-left",   "ui.toolbar.left.v3.order",   "top-toolbar-left"},
        {"toolbar-icons-center", "ui.toolbar.center.v2.order", "top-toolbar-center"},
        {"toolbar-icons-right",  "ui.toolbar.right.v2.order",  "top-toolbar-right"},
    };
    dragDrop->Setup(rootEl, kTopToolbarContainers);
    dragDrop->LoadButtonOrder(rootEl, kTopToolbarContainers);
}

void EditorTopToolbar::ApplyButtonVisibilityFromPrefs(UIElement* rootEl)
{
    if (!rootEl)
        return;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    for (const auto& info : kToolbarButtons)
    {
        bool hidden = false;
        if (CanHideToolbarButton(info.id))
            prefs.TryGetBool(MakeVisibilityPrefKey(info.id), hidden);
        ApplyHiddenClass(rootEl->FindById(info.id), hidden);
    }
}

void EditorTopToolbar::WireRightClickToggles(UIElement* rootEl, Platform::Window* window)
{
    if (!rootEl || !window)
        return;

    auto showVisibilityMenu = [this, rootEl, window](int x, int y)
    {
        m_ButtonVisibilityMenu = CreateContextMenu();
        if (!m_ButtonVisibilityMenu)
            return;

        for (uint32_t i = 0; i < std::size(kToolbarButtons); ++i)
        {
            const auto& entry = kToolbarButtons[i];
            UIElement* button = rootEl->FindById(entry.id);
            const bool visible = button && !button->HasClass("hidden");
            const uint32_t commandId = i + 1;
            uint32_t flags = visible ? MenuItemFlag_Checked : MenuItemFlag_None;
            if (!CanHideToolbarButton(entry.id))
                flags |= MenuItemFlag_Disabled;
            m_ButtonVisibilityMenu->AddItem(
                0, entry.title, commandId, flags);
            m_ButtonVisibilityMenu->SetItemIcon(commandId, entry.iconPath);
        }

        m_ButtonVisibilityMenu->SetCommandHandler([rootEl](uint32_t commandId)
        {
            if (commandId == 0 || commandId > std::size(kToolbarButtons))
                return;
            const auto& entry = kToolbarButtons[commandId - 1];
            if (!CanHideToolbarButton(entry.id))
                return;
            UIElement* button = rootEl->FindById(entry.id);
            if (!button)
                return;
            const bool nowHidden = !button->HasClass("hidden");
            SetButtonHiddenInPrefs(entry.id, nowHidden);
            ApplyHiddenClass(button, nowHidden);
        });

        m_ButtonVisibilityMenu->Show(window, x, y);
    };

    // The customization menu belongs to the empty runs of the toolbar. Save and Record
    // consume their own right-clicks through the ContextMenuManipulator attached to each;
    // Button itself claims no right-click, so every other button leaves one unhandled and it
    // bubbles here. The original hit target settles both cases: an event that came through a
    // Button or through the icon container came from the icons, inter-icon gaps included.
    //
    // This handler is the manipulator's own second consumer, and it is not one yet: it
    // activates on a bare right RELEASE, so a press that starts on a button and ends in a gap
    // opens this menu — exactly the unarmed-gesture defect PointerManipulator exists to stop.
    // Migrating it needs one piece the manipulator does not have, a press-time gate, because
    // the "did this come from a button?" test reads the press target and arming replaces the
    // release target with the capture element. Tracked, not done here.
    auto* toolbar = FindFirstByClass(rootEl, "editor-top-toolbar");
    if (!toolbar)
        return;

    toolbar->RegisterEventHandler(kEventMouseUp, [toolbar, showVisibilityMenu](UIEvent& e)
    {
        if (e.Button != 1)
            return;

        for (UIElement* node = e.Target; node && node != toolbar; node = node->GetParent())
        {
            if (dynamic_cast<Button*>(node) || node->HasClass(kIconRunClass))
                return;
        }

        showVisibilityMenu(static_cast<int>(e.X), static_cast<int>(e.Y));
        e.Stop();
    });
}

} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_reg_editorTopToolbar =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::EditorTopToolbar>(
        "EditorTopToolbar",
        []() { return std::make_unique<GameEngine::EditorTopToolbar>(); });
}
