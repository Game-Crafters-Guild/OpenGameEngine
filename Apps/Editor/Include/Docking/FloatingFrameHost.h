#pragma once

#include "EditorApplication.h" // EditorApplication::EditorWindowContext

#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{
class DeferredActionQueue;
class DockingManager;
class FloatingPanel;

namespace Editor
{

/// Presents torn-off panels as in-canvas frames on a host that has one OS
/// window. The frames are FloatingPanel elements on the main window's UI
/// root; this host creates, tracks, raises, redocks and closes them, so the
/// dock tree and the panel manager can treat "floating" as one more place a
/// panel can be, whichever presentation the platform gives it.
class FloatingFrameHost
{
  public:
    using EditorWindowContext = EditorApplication::EditorWindowContext;

    struct Dependencies
    {
        // The window whose UI root hosts the frames and whose docking model
        // the panels leave and rejoin. Resolved per call: the window list is
        // rebuilt during Initialize.
        std::function<EditorWindowContext*()> GetMainWindow;
        // Pre-UI safe point. Tear-off and close both arrive mid-dispatch (a tab
        // drag, the close button's click) and the dock rebuild they need
        // mutates the tree that dispatch is walking.
        DeferredActionQueue* PreUiActions = nullptr;
        // EditorPanelManager::RebuildDockspaceNow, bound by the app.
        std::function<void(EditorWindowContext*, DockingManager*)> RebuildDockspaceNow;
    };

    void Initialize(Dependencies deps);

    /// Tear the panel out of the dock into a frame. Samples the cursor now,
    /// while the tab drag that asked is still the current event, and mounts
    /// on the next pre-UI safe point.
    void QueueTearOff(std::string panelId);

    /// True when the panel is currently presented as a frame, which is then
    /// raised above its siblings. The dock must not add the panel a second time.
    bool Raise(const std::string& panelId);

    /// Unmount every frame without restoring tabs. A layout apply places the
    /// panels itself.
    void DismissAll();

  private:
    void TearOff(const std::string& panelId, float cursorX, float cursorY);
    void QueueClose(FloatingPanel* frame);
    void Close(FloatingPanel* frame);
    void UpdateDockPreview(float mouseX, float mouseY, bool altHeld);
    void OnTitleDragEnd(FloatingPanel* frame, float mouseX, float mouseY, bool altHeld, bool cancelled);
    void Redock(FloatingPanel* frame, float mouseX, float mouseY);
    /// Hide the frame, drop it from tracking and from the UI root.
    void Unmount(FloatingPanel* frame, EditorWindowContext* mainWindow);

    Dependencies m_Deps;
    // Owned by the main window's UI root; tracked here from Show to Unmount.
    std::vector<FloatingPanel*> m_Frames;
};

} // namespace Editor
} // namespace GameEngine
