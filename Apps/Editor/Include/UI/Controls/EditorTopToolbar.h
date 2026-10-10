#pragma once

#include "UI/UIElement.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{

class Button;
class INativeContextMenu;
class EditorPanelManager;

namespace Platform
{
class Window;
} // namespace Platform

namespace Editor
{
enum class PlayModeState : std::uint8_t;
class ToolbarDragDrop;
} // namespace Editor

/**
 * Wires the in-UI top toolbar buttons (play/pause/stop, panel toggles, project folder, etc.).
 * Extracted from EditorApplication to separate button wiring from the application's main loop.
 *
 * This is distinct from EditorToolbar, which manages the native platform menu bar.
 */
class EditorTopToolbar final : public UIElement
{
  public:
    struct PanelMenuEntry
    {
        std::string panelId;
        std::string title;
        std::string iconClass;
    };

    /// Callbacks invoked when toolbar buttons are clicked. Each is optional.
    struct Callbacks
    {
        std::function<std::vector<PanelMenuEntry>()> getPanelMenuEntries;
        std::function<void(const std::string& panelId)> onPanelMenuEntryClicked;
        std::function<void()> onUniversalSearchClicked;
        std::function<void()> onSettingsClicked;
        std::function<void()> onNodeGraphClicked;
        std::function<void()> onBookmarksClicked;
        std::function<void()> onWebClicked;
        std::function<void()> onScriptEditorClicked;
        std::function<void()> onTodosClicked;
        std::function<void()> onUndoHistoryClicked;
        std::function<void()> onPackagesClicked;
        std::function<void()> onBuildClicked;
        std::function<void()> onMixerClicked;
        std::function<void()> onMonitorsClicked;
        std::function<void()> onVisualProfilerClicked;
        std::function<void()> onRenderGraphClicked;
        std::function<void()> onAnimationClicked;
        std::function<void()> onTimelineClicked;
        std::function<void()> onLogClicked;
        std::function<void()> onVramClicked;
        std::function<void()> onCpuProfilerClicked;
        /// Modifier bitmask on mouse-up (Input::kModShift, etc.); 0 for keyboard-triggered clicks.
        std::function<void(int mods)> onSaveSceneClicked;
        std::function<void()> onRevertSceneClicked;
        std::function<void()> onProjectFolderClicked;
        std::function<void()> onFullscreenToggleClicked;
        std::function<void()> onRecordClicked;
        std::function<void()> onPlayClicked;
        std::function<void()> onPauseClicked;
        std::function<void()> onStopClicked;
    };

    EditorTopToolbar();
    ~EditorTopToolbar() override;

    /// Inject the panel manager after factory instantiation (the UI factory
    /// default-constructs elements declared in .uxml).
    void SetPanelManager(EditorPanelManager* panelManager) { m_PanelManager = panelManager; }

    /// Load this control's UXML and stylesheet into its subtree.
    bool LoadAssets();
    void OnPostLayout() override;

    /// Wire all toolbar buttons inside rootEl to the provided callbacks.
    void WireControls(UIElement* rootEl, Platform::Window* window, const Callbacks& callbacks);

    /// Update play/pause/stop button visuals to match the current play mode state.
    void UpdatePlayModeState(Editor::PlayModeState s);

    /// Set up toolbar drag-and-drop (icon reordering).
    void SetupDragDrop(UIElement* rootEl, Editor::ToolbarDragDrop* dragDrop);

    /// Read user preferences and hide any top-toolbar buttons the user has disabled.
    void ApplyButtonVisibilityFromPrefs(UIElement* rootEl);

    /// Attach an icon-visibility context menu to the empty top-toolbar background.
    /// Right-clicks on a button, or in the run of bar its container's icons
    /// occupy, do not open it — buttons with their own context menu show that
    /// menu, the rest stay inert. Changes persist via editor preferences.
    void WireRightClickToggles(UIElement* rootEl, Platform::Window* window);

    /// Highlight or de-highlight the fullscreen toggle button.
    void SetFullscreenActive(bool active);

    /// Highlight or de-highlight the movie recording toolbar button.
    void SetRecordActive(bool active);

    /// Show or hide the unsaved-changes indicator on the save button.
    void SetSaveDirty(bool dirty);

    /// Cached button pointers (may be null if not yet wired or not found).
    Button* GetPlayButton() const { return m_PlayButton; }
    Button* GetPauseButton() const { return m_PauseButton; }
    Button* GetStopButton() const { return m_StopButton; }
    Button* GetRecordButton() const { return m_RecordButton; }

  private:
    void ScheduleAssetLoad();

    EditorPanelManager* m_PanelManager = nullptr; // not owned
    bool m_AssetsLoaded = false;
    bool m_AssetLoadScheduled = false;
    bool m_AssetLoadFailed = false;
    Button* m_FullscreenButton = nullptr;
    Button* m_SaveButton = nullptr;
    Button* m_RecordButton = nullptr;
    Button* m_PlayButton = nullptr;
    Button* m_PauseButton = nullptr;
    Button* m_StopButton = nullptr;
    std::unique_ptr<INativeContextMenu> m_PanelMenu;
    std::unique_ptr<INativeContextMenu> m_ButtonVisibilityMenu;
};

} // namespace GameEngine
