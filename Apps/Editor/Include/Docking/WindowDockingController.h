#pragma once

#include "Docking/FloatingFrameHost.h"
#include "EditorApplication.h" // EditorApplication::EditorWindowContext

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
class DeferredActionQueue;
class DockingManager;
namespace Platform
{
class Window;
}

namespace Editor
{

/// Owns the editor's cross-window drag-drop routing, tear-off drag preview,
/// ALT-held dock previews during OS window moves, dock-drop commit, and the
/// in-canvas floating frames a single-window host tears panels off into.
/// Window/GPU-context creation stays in EditorApplication
/// (UndockToFloatingWindow); this controller is policy + per-frame state only.
class WindowDockingController
{
  public:
    using EditorWindowContext = EditorApplication::EditorWindowContext;

    // Drop candidate (updated during preview; executed on mouse-up)
    enum class DropKind
    {
        None,
        Tab,
        LeafSplit,
        RegionSplit,
        RootSplit
    };
    struct DropCandidate
    {
        EditorWindowContext* source = nullptr;
        EditorWindowContext* target = nullptr;
        int zone = 0; // legacy overlay: 0=center,1=left,2=right,3=top,4=bottom
        DropKind kind = DropKind::None;
        std::string path;                         // path for leaf/region
        DockPosition edge = DockPosition::Center; // split direction for *Split kinds
        bool valid = false;
    };

    struct Dependencies
    {
        // App-lifetime window list; the app owns/mutates the vector.
        std::vector<std::unique_ptr<EditorWindowContext>>* Windows = nullptr;
        // Main-window docking model: recreated during Initialize and reset at
        // shutdown, so resolve lazily.
        std::function<DockingManager*()> GetMainDocking;
        // Deterministic UI replay injects input directly into one UIManager;
        // the OS-state-driven cross-window router must stand down then.
        std::function<bool()> IsUiReplayActive;
        // EditorApplication::RenderSingle — passive repaint of an unfocused
        // preview target during native move loops.
        std::function<void(EditorWindowContext*)> RenderWindowNow;
        // EditorPanelManager::RebuildDockspaceNow (private static, friend of
        // the app; bound by an app lambda).
        std::function<void(EditorWindowContext*, DockingManager*)> RebuildDockspaceNow;
        // The app's pre-UI safe point; floating-frame tear-off and close land
        // there because both arrive mid-dispatch.
        DeferredActionQueue* PreUiActions = nullptr;
    };

    void Initialize(Dependencies deps);

    /// Torn-off panels presented as frames inside the main window, for hosts
    /// with one OS window. The tear-off gesture, ShowPanel and layout restore
    /// all go through it.
    FloatingFrameHost& FloatingFrames() { return m_FloatingFrames; }

    /// Render() pre-loop: cross-window DnD routing, tear-off follow/preview,
    /// and ALT-held OS-drag previews. Platform mouse state is deliberately
    /// re-queried mid-pass (not sampled once) to preserve observable behavior.
    void Tick();

    /// Dock-on-drop commit on mouse release. Called once per window iteration
    /// of the render loop, at the same position the inline block occupied.
    void TickDropCommitOnRelease();

    /// Activate tear-off tracking for a freshly undocked floating window. The
    /// anchor offsets must match the values the caller used to position the
    /// window relative to the cursor.
    void BeginTearOff(Platform::Window* sourceWindow, Platform::Window* floatingWindow,
                      float anchorX, float anchorY);

    /// Docking-preview reaction to an OS window move (the caller stamps
    /// EditorWindowContext::lastWindowMoveAt first).
    void OnWindowMoved(EditorWindowContext* moving);

    /// A floating window is being torn down: drop tear-off tracking and null
    /// every raw drag-state pointer that references the dying context, so no
    /// commit/preview/router step can touch freed memory.
    void OnFloatingWindowClosing(EditorWindowContext* closing);

    /// Clear all in-flight drag/preview state before window teardown.
    void ResetDragStateForShutdown();

    void ClearAllDockOverlays();
    void PerformDockDrop(const DropCandidate& cand);
    void SetDockEdgeFraction(float frac);

  private:
    bool ComputeDropCandidateFromCursor(EditorWindowContext* source, DropCandidate& outCandidate);
    void ShowDropOverlays();

    Dependencies m_Deps;
    FloatingFrameHost m_FloatingFrames;

    // Tear-off (undock) drag state for smooth window positioning while mouse is held
    struct TearOffDragState
    {
        bool active = false;
        Platform::Window* sourceWindow = nullptr;   // window where the drag started
        Platform::Window* floatingWindow = nullptr; // the newly created floating window
        float anchorX = 16.0f;                      // desired cursor offset inside new window client area
        float anchorY = 12.0f;
        bool ghostMode = false; // when true, move an overlay instead of the real window until drop
        int ghostWidth = 960;
        int ghostHeight = 600;
    };
    TearOffDragState m_TearOff{};

    // Cross-window drag/drop routing (payload can move across UIManagers/windows).
    struct CrossWindowDragDropState
    {
        bool active = false;
        UI::Interaction::DragPayload payload{};
        UI::Interaction::DragSessionContext ctx{};
        EditorWindowContext* activeWindow = nullptr; // window currently hosting the per-window drag session
        bool lastLmbDown = false;
    } m_CrossDnd{};

    DropCandidate m_DropCandidate{};
    bool m_LastLMBDown = false;

    // Docking preview configuration
    float m_DockEdgeFrac = 0.25f; // thickness of edge drop zones (0..0.5)

    // OS title-bar drag tracking (native move loop)
    bool m_OSDragActive = false;
    bool m_OSDragMouseWasDown = false;
    EditorWindowContext* m_LastOSDragPreviewTarget = nullptr; // last window we rendered an OS-drag preview for
    EditorWindowContext* m_LastTearOffPreviewTarget = nullptr; // last window highlighted during tear-off preview
};

} // namespace Editor
} // namespace GameEngine
