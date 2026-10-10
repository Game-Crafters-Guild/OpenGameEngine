#include "Docking/WindowDockingController.h"

#include "Core/WindowInputRouter.h"
#include "EditorPanelManager.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector2.h"
#include "Platform/Window.h"
#include "UI/Controls/DockOverlay.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Layout/DockingHitTest.h"
#include "UI/UIManager.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace GameEngine
{
namespace Editor
{

void WindowDockingController::Initialize(Dependencies deps)
{
    m_Deps = std::move(deps);

    FloatingFrameHost::Dependencies frameDeps;
    frameDeps.GetMainWindow = [windows = m_Deps.Windows]() -> EditorWindowContext*
    { return (windows && !windows->empty()) ? windows->front().get() : nullptr; };
    frameDeps.PreUiActions = m_Deps.PreUiActions;
    frameDeps.RebuildDockspaceNow = m_Deps.RebuildDockspaceNow;
    m_FloatingFrames.Initialize(std::move(frameDeps));
}

void WindowDockingController::Tick()
{

    // Cross-window drag & drop routing (internal DnD between panels/windows).
    // This mirrors the tear-off approach: use global cursor position and global mouse state
    // instead of relying on per-window mouse capture.
    //
    // IMPORTANT: when running deterministic UIReplay, input is injected directly into a specific
    // UIManager and does not drive the OS-level mouse state. The cross-window router uses OS
    // APIs (IsLeftMouseButtonDown/GetCursorScreenPosition) and would immediately cancel per-window
    // drag sessions during replays. Skip routing entirely while UIReplay is active.
    {
        if (m_Deps.IsUiReplayActive())
        {
            // no-op during replay
        }
        else
        {
        const bool lmbDown = Platform::Window::IsLeftMouseButtonDown();

        // If not currently active, latch any per-window drag session as the global session.
        if (!m_CrossDnd.active)
        {
            for (auto& w : (*m_Deps.Windows))
            {
                EditorWindowContext* ctx = w.get();
                if (!ctx || !ctx->ui)
                    continue;
                auto* dd = ctx->ui->GetDragDropManager();
                if (dd && dd->IsDragging())
                {
                    m_CrossDnd.active = true;
                    m_CrossDnd.payload = dd->GetPayload(); // copy (std::any)
                    m_CrossDnd.ctx = dd->GetContext();
                    m_CrossDnd.activeWindow = nullptr;
                    dd->CancelDrag();
                    break;
                }
            }
            m_CrossDnd.lastLmbDown = lmbDown;
        }

        if (m_CrossDnd.active)
        {
            int cx = 0, cy = 0;
            Platform::Window::GetCursorScreenPosition(cx, cy);

            // Find window under cursor.
            EditorWindowContext* under = nullptr;
            for (auto& w : (*m_Deps.Windows))
            {
                EditorWindowContext* t = w.get();
                if (!t || !t->window || !t->ui)
                    continue;
                int wx = 0, wy = 0;
                t->window->GetPosition(wx, wy);
                int ww = 0, wh = 0;
                t->window->GetWindowSize(ww, wh);
                if (cx >= wx && cy >= wy && cx < wx + ww && cy < wy + wh)
                {
                    under = t;
                    break;
                }
            }

            // Transfer drag to the window under cursor (if changed).
            if (under && under->ui && m_CrossDnd.activeWindow != under)
            {
                if (m_CrossDnd.activeWindow && m_CrossDnd.activeWindow->ui)
                {
                    if (auto* prev = m_CrossDnd.activeWindow->ui->GetDragDropManager())
                        prev->CancelDrag();
                }
                m_CrossDnd.activeWindow = under;
                if (auto* cur = under->ui->GetDragDropManager())
                {
                    UI::Interaction::DragPayload p = m_CrossDnd.payload; // copy
                    cur->BeginDrag(std::move(p), m_CrossDnd.ctx);
                }
            }
            else if (!under || !under->ui)
            {
                // Cursor not over any editor window: cancel per-window session.
                if (m_CrossDnd.activeWindow && m_CrossDnd.activeWindow->ui)
                {
                    if (auto* dd = m_CrossDnd.activeWindow->ui->GetDragDropManager())
                        dd->CancelDrag();
                }
                m_CrossDnd.activeWindow = nullptr;
            }

            // Commit/cancel on mouse release edge.
            if (!lmbDown && m_CrossDnd.lastLmbDown)
            {
                if (m_CrossDnd.activeWindow && m_CrossDnd.activeWindow->ui)
                {
                    auto* dd = m_CrossDnd.activeWindow->ui->GetDragDropManager();
                    if (dd && dd->IsDragging())
                    {
                        // Force a hover resolution so the DDM knows which drop target
                        // the cursor is over. This is needed when the drag transferred
                        // to this window in the same frame (UpdateHover hasn't run yet).
                        const Mathematics::Vector2 cursor = m_CrossDnd.activeWindow->ui->GetMousePosition();
                        UIElement* hovered = m_CrossDnd.activeWindow->ui->GetHoveredElement();
                        dd->UpdateHover(hovered, cursor.x, cursor.y);
                        dd->CommitDrop();
                    }
                    else if (dd)
                    {
                        dd->CancelDrag();
                    }
                }
                // Notify the drag source that the session truly ended (after any commit/cancel),
                // exactly once. The per-window CancelDrag calls above (latch + window handoff) are
                // session transfers, not ends, so they deliberately don't fire this.
                if (m_CrossDnd.ctx.OnDragEnded)
                    m_CrossDnd.ctx.OnDragEnded();
                m_CrossDnd.active = false;
                m_CrossDnd.payload = {};
                m_CrossDnd.ctx = {};
                m_CrossDnd.activeWindow = nullptr;
            }
            m_CrossDnd.lastLmbDown = lmbDown;
        }
    }
    }

    // Smooth tear-off drag: if active, move floating window to follow the OS cursor globally
    if (m_TearOff.active && m_TearOff.floatingWindow)
    {
        int cursorX = 0, cursorY = 0;
        Platform::Window::GetCursorScreenPosition(cursorX, cursorY);
        int fl = 0, ft = 0, fr = 0, fb = 0;
        m_TearOff.floatingWindow->GetFrameSize(fl, ft, fr, fb);
        int targetX = static_cast<int>(cursorX - fl - m_TearOff.anchorX);
        int targetY = static_cast<int>(cursorY - ft - m_TearOff.anchorY);
        m_TearOff.floatingWindow->SetPosition(targetX, targetY);
        if (!Platform::Window::IsLeftMouseButtonDown())
        {
            m_TearOff.active = false;
            m_TearOff.floatingWindow->Focus();
        }
    }
    // Docking preview during tear-off: show overlay hint when ALT is held and
    // cursor is over another window's dockspace.
    const bool tearOffAltHeld = Platform::Window::IsOptionKeyDown();
    if (m_TearOff.active && m_TearOff.floatingWindow && tearOffAltHeld)
    {
        EditorWindowContext* targetUnderCursor = nullptr;
        int cx = 0, cy = 0;
        Platform::Window::GetCursorScreenPosition(cx, cy);
        // Find first window (other than the dragged one) whose client rect contains the cursor
        for (auto& w : (*m_Deps.Windows))
        {
            EditorWindowContext* t = w.get();
            if (!t || !t->window)
                continue;
            if (t->window.get() == m_TearOff.floatingWindow)
                continue; // ignore self
            int wx = 0, wy = 0;
            t->window->GetPosition(wx, wy);
            int ww = 0, wh = 0;
            t->window->GetWindowSize(ww, wh);
            if (cx >= wx && cy >= wy && cx < wx + ww && cy < wy + wh)
            {
                targetUnderCursor = t;
                break;
            }
        }

        // Keep previous preview until we compute a new valid target; only hide on failure

        if (targetUnderCursor && targetUnderCursor->ui)
        {
            UIElement* root = targetUnderCursor->ui->GetRootElement();
            UIElement* dockEl = root ? root->FindById("dock") : nullptr;
            if (dockEl)
            {
                // Compute dockspace rect in screen space. GetPosition returns window
                // origin in screen DIP; GetLayout* returns UI-logical pixels.
                // Convert UI-logical → window client (DIP) via UiLogicalToClientScale.
                int wx = 0, wy = 0;
                targetUnderCursor->window->GetPosition(wx, wy);
                const float scale = WindowInputRouter::UiLogicalToClientScale(
                    targetUnderCursor->window.get(), targetUnderCursor->ui.get());
                const float dxw = dockEl->GetLayoutX() * scale;
                const float dyw = dockEl->GetLayoutY() * scale;
                const float dww = dockEl->GetLayoutWidth() * scale;
                const float dhw = dockEl->GetLayoutHeight() * scale;
                const float sleft = wx + dxw;
                const float stop = wy + dyw;

                // Determine zone: edges vs center
                const float edgeFrac = 0.25f;
                const float edgeW = dww * edgeFrac;
                const float edgeH = dhw * edgeFrac;

                enum Zone
                {
                    ZoneCenter = 0,
                    ZoneLeft,
                    ZoneRight,
                    ZoneTop,
                    ZoneBottom
                } zone = ZoneCenter;

                // Pick active zone by cursor position
                if (cx < sleft + edgeW)
                    zone = ZoneLeft;
                else if (cx >= sleft + dww - edgeW)
                    zone = ZoneRight;
                else if (cy < stop + edgeH)
                    zone = ZoneTop;
                else if (cy >= stop + dhw - edgeH)
                    zone = ZoneBottom;
                else
                    zone = ZoneCenter;

                // Record drop candidate (source is the floating window being dragged)
                EditorWindowContext* srcCtx = nullptr;
                for (auto& w2 : (*m_Deps.Windows))
                {
                    if (w2 && w2->window && w2->window.get() == m_TearOff.floatingWindow)
                    {
                        srcCtx = w2.get();
                        break;
                    }
                }
                m_DropCandidate.source = srcCtx;
                m_DropCandidate.target = targetUnderCursor;
                m_DropCandidate.zone = zone;
                m_DropCandidate.valid = (srcCtx != nullptr && targetUnderCursor != nullptr);

                // Route to DockOverlay-based visualization
                ShowDropOverlays();
                m_LastTearOffPreviewTarget = targetUnderCursor;
            }
            else
            {
                ClearAllDockOverlays();
                if (m_LastTearOffPreviewTarget)
                {
                    m_Deps.RenderWindowNow(m_LastTearOffPreviewTarget);
                    m_LastTearOffPreviewTarget = nullptr;
                }
                m_DropCandidate.valid = false;
            }
        }
        else
        {
            ClearAllDockOverlays();
            if (m_LastTearOffPreviewTarget)
            {
                m_Deps.RenderWindowNow(m_LastTearOffPreviewTarget);
                m_LastTearOffPreviewTarget = nullptr;
            }
            m_DropCandidate.valid = false;
        }

        // If drag ended, also hide preview overlays
    }
    else if (m_TearOff.active && m_TearOff.floatingWindow && !tearOffAltHeld)
    {
        // Tear-off active without ALT — clear any stale dock overlays.
        if (m_LastTearOffPreviewTarget || m_DropCandidate.valid)
        {
            ClearAllDockOverlays();
            if (m_LastTearOffPreviewTarget)
            {
                m_Deps.RenderWindowNow(m_LastTearOffPreviewTarget);
                m_LastTearOffPreviewTarget = nullptr;
            }
            m_DropCandidate.valid = false;
        }
    }

    // OS window-move path: if user released after undock and then drags by native caption.
    // Only enter this path when floating windows actually exist, otherwise we would
    // clear dock overlays that the within-dockspace DockTab drag just set up.
    bool hasFloatingWindows = false;
    if (!m_TearOff.active)
    {
        for (auto& w : (*m_Deps.Windows))
        {
            if (w && w->window && !w->floatingPanelId.empty())
            {
                hasFloatingWindows = true;
                break;
            }
        }
    }
    if (!m_TearOff.active && hasFloatingWindows)
    {
        // Track which floating window is being moved (position changes while LMB is down)
        EditorWindowContext* moving = nullptr;
        for (auto& w : (*m_Deps.Windows))
        {
            EditorWindowContext* t = w.get();
            if (!t || !t->window)
                continue;
            if (t->floatingPanelId.empty())
            {
                // Skip non-floating windows
                continue;
            }
            int px = 0, py = 0;
            t->window->GetPosition(px, py);
            bool moved = t->hasLastPos && (px != t->lastPosX || py != t->lastPosY);
            t->lastPosX = px;
            t->lastPosY = py;
            t->hasLastPos = true;
            if (moved)
            {
                moving = t;
                break;
            }
        }

        // DockOverlay-based overlays: only show drop zones when ALT/Option is held
        // so normal window dragging doesn't trigger re-docking.
        // Uses platform-native modifier query (works during OS window drag).
        const bool altHeld = moving && Platform::Window::IsOptionKeyDown();

        if (moving && altHeld)
        {
            // Find a target window under cursor (skip the moving one)
            int cx = 0, cy = 0;
            Platform::Window::GetCursorScreenPosition(cx, cy);
            EditorWindowContext* targetUnderCursor = nullptr;
            for (auto& w : (*m_Deps.Windows))
            {
                EditorWindowContext* t = w.get();
                if (!t || !t->window)
                    continue;
                if (t == moving)
                    continue;
                int wx = 0, wy = 0;
                t->window->GetPosition(wx, wy);
                int ww = 0, wh = 0;
                t->window->GetWindowSize(ww, wh);
                if (cx >= wx && cy >= wy && cx < wx + ww && cy < wy + wh)
                {
                    targetUnderCursor = t;
                    break;
                }
            }

            if (targetUnderCursor && targetUnderCursor->ui)
            {
                if (auto* root = targetUnderCursor->ui->GetRootElement())
                {
                    if (auto* dockEl = root->FindById("dock"))
                    {
                        int wx = 0, wy = 0;
                        targetUnderCursor->window->GetPosition(wx, wy);
                        const float scale = WindowInputRouter::UiLogicalToClientScale(
                            targetUnderCursor->window.get(), targetUnderCursor->ui.get());
                        const float dxw = dockEl->GetLayoutX() * scale;
                        const float dyw = dockEl->GetLayoutY() * scale;
                        const float dww = dockEl->GetLayoutWidth() * scale;
                        const float dhw = dockEl->GetLayoutHeight() * scale;
                        const float sleft = wx + dxw;
                        const float stop = wy + dyw;

                        const float edgeFrac = 0.25f;
                        const float edgeW = dww * edgeFrac;
                        const float edgeH = dhw * edgeFrac;

                        enum Zone
                        {
                            ZoneCenter = 0,
                            ZoneLeft,
                            ZoneRight,
                            ZoneTop,
                            ZoneBottom
                        } zone = ZoneCenter;

                        // Pick active zone by cursor position
                        if (cx < sleft + edgeW)
                            zone = ZoneLeft;
                        else if (cx >= sleft + dww - edgeW)
                            zone = ZoneRight;
                        else if (cy < stop + edgeH)
                            zone = ZoneTop;
                        else if (cy >= stop + dhw - edgeH)
                            zone = ZoneBottom;
                        else
                            zone = ZoneCenter;

                        // Record drop candidate for OS-dragged floating window
                        m_DropCandidate.source = moving;
                        m_DropCandidate.target = targetUnderCursor;
                        m_DropCandidate.zone = zone;
                        m_DropCandidate.valid = (moving != nullptr && targetUnderCursor != nullptr);

                        // Route to DockOverlay-based visualization
                        ShowDropOverlays();
                        // Force an immediate repaint of the target window even if not focused
                        m_LastOSDragPreviewTarget = targetUnderCursor;
                        m_Deps.RenderWindowNow(targetUnderCursor);
                    }
                    else
                    {
                        ClearAllDockOverlays();
                        if (m_LastOSDragPreviewTarget)
                        {
                            m_Deps.RenderWindowNow(m_LastOSDragPreviewTarget);
                            m_LastOSDragPreviewTarget = nullptr;
                        }
                        m_DropCandidate.valid = false;
                    }
                }
                else
                {
                    ClearAllDockOverlays();
                    if (m_LastOSDragPreviewTarget)
                    {
                        m_Deps.RenderWindowNow(m_LastOSDragPreviewTarget);
                        m_LastOSDragPreviewTarget = nullptr;
                    }
                    m_DropCandidate.valid = false;
                }
            }
            else
            {
                ClearAllDockOverlays();
                if (m_LastOSDragPreviewTarget)
                {
                    m_Deps.RenderWindowNow(m_LastOSDragPreviewTarget);
                    m_LastOSDragPreviewTarget = nullptr;
                }
                m_DropCandidate.valid = false;
            }
        }
        else if (moving && !altHeld)
        {
            // Moving without ALT held — clear any stale dock overlays (only if active).
            if (m_LastOSDragPreviewTarget || m_DropCandidate.valid)
            {
                ClearAllDockOverlays();
                if (m_LastOSDragPreviewTarget)
                {
                    m_Deps.RenderWindowNow(m_LastOSDragPreviewTarget);
                    m_LastOSDragPreviewTarget = nullptr;
                }
                m_DropCandidate.valid = false;
            }
        }
        else if (!Platform::Window::IsLeftMouseButtonDown())
        {
            // Mouse released without a moving window — clear overlays.
            ClearAllDockOverlays();
            if (m_LastOSDragPreviewTarget)
            {
                m_Deps.RenderWindowNow(m_LastOSDragPreviewTarget);
                m_LastOSDragPreviewTarget = nullptr;
            }
            m_DropCandidate.valid = false;
        }
        // else: no window moved this frame but mouse is still held — keep
        // the current overlay visible so it doesn't flicker.

        if (!Platform::Window::IsLeftMouseButtonDown())
        {
            if (!m_OSDragActive)
            {
                ClearAllDockOverlays();
                if (m_LastTearOffPreviewTarget)
                {
                    m_Deps.RenderWindowNow(m_LastTearOffPreviewTarget);
                    m_LastTearOffPreviewTarget = nullptr;
                }
            }
        }
    }
}

void WindowDockingController::TickDropCommitOnRelease()
{
        // Commit dock-on-drop on mouse release (global)
        bool lmbNow = Platform::Window::IsLeftMouseButtonDown();
        if ((m_LastLMBDown && !lmbNow && m_DropCandidate.valid) ||
            (!lmbNow && m_DropCandidate.valid && m_OSDragActive))
        {
            // Re-evaluate target/zone at release time using the live cursor position
            DropCandidate commitCand = m_DropCandidate;
            if (commitCand.source)
            {
                ComputeDropCandidateFromCursor(commitCand.source, commitCand);
            }
            if (commitCand.valid)
            {
                PerformDockDrop(commitCand);
            }
            ClearAllDockOverlays();
            m_DropCandidate.valid = false;
            m_OSDragActive = false;
            m_OSDragMouseWasDown = false;
        }
        m_LastLMBDown = lmbNow;
}

void WindowDockingController::BeginTearOff(Platform::Window* sourceWindow,
                                           Platform::Window* floatingWindow,
                                           float anchorX, float anchorY)
{
    // Assigns exactly these five fields — never value-initialize m_TearOff
    // here (ghostMode and the ghost extents must keep their current values).
    m_TearOff.active = true;
    m_TearOff.sourceWindow = sourceWindow;
    m_TearOff.floatingWindow = floatingWindow;
    m_TearOff.anchorX = anchorX;
    m_TearOff.anchorY = anchorY;
}

void WindowDockingController::OnWindowMoved(EditorWindowContext* moving)
{
    // Only interested in floating windows being dragged by OS title bar
    if (!moving || moving->floatingPanelId.empty())
        return;
    if (m_TearOff.active)
        return; // tear-off path already handles preview

    // Only show dock previews when ALT/Option is held
    if (!Platform::Window::IsOptionKeyDown())
    {
        // Clear any stale overlays from a previous ALT-held drag
        if (m_LastOSDragPreviewTarget || m_DropCandidate.valid)
        {
            ClearAllDockOverlays();
            if (m_LastOSDragPreviewTarget)
            {
                m_Deps.RenderWindowNow(m_LastOSDragPreviewTarget);
                m_LastOSDragPreviewTarget = nullptr;
            }
            m_DropCandidate.valid = false;
        }
        return;
    }

    // Mark OS-drag active; track mouse state during OS move
    m_OSDragActive = true;
    m_OSDragMouseWasDown = Platform::Window::IsLeftMouseButtonDown();

    DropCandidate cand;
    if (ComputeDropCandidateFromCursor(moving, cand))
    {
        m_DropCandidate = cand;
        ShowDropOverlays();
        // Force an immediate repaint of the target window even if it's not focused
        m_LastOSDragPreviewTarget = cand.target;
        m_Deps.RenderWindowNow(cand.target);
        return;
    }

    ClearAllDockOverlays();
    // Ensure any previously highlighted target clears immediately
    if (m_LastOSDragPreviewTarget)
    {
        m_Deps.RenderWindowNow(m_LastOSDragPreviewTarget);
        m_LastOSDragPreviewTarget = nullptr;
    }
    m_DropCandidate.valid = false;
}

void WindowDockingController::OnFloatingWindowClosing(EditorWindowContext* closing)
{
    if (!closing)
        return;

    Platform::Window* closingWindow = closing->window.get();
    // If we were tracking a tear-off for this window, stop it now
    if (closingWindow && m_TearOff.floatingWindow == closingWindow)
    {
        m_TearOff.active = false;
        m_TearOff.ghostMode = false;
        ClearAllDockOverlays();
        m_TearOff.floatingWindow = nullptr;
        m_TearOff.sourceWindow = nullptr;
    }
    // The tear-off source dying (e.g. its dockspace emptied) leaves a dangling
    // window pointer; it is only ever identity-compared, but never keep it.
    if (closingWindow && m_TearOff.sourceWindow == closingWindow)
        m_TearOff.sourceWindow = nullptr;

    // The drag state holds raw context pointers; a commit, preview repaint, or
    // router step against the freed context is the alternative.
    if (m_DropCandidate.source == closing || m_DropCandidate.target == closing)
    {
        m_DropCandidate = DropCandidate{};
        ClearAllDockOverlays();
    }
    if (m_LastOSDragPreviewTarget == closing)
        m_LastOSDragPreviewTarget = nullptr;
    if (m_LastTearOffPreviewTarget == closing)
        m_LastTearOffPreviewTarget = nullptr;
    if (m_CrossDnd.activeWindow == closing)
    {
        m_CrossDnd.active = false;
        m_CrossDnd.activeWindow = nullptr;
    }
}

void WindowDockingController::ResetDragStateForShutdown()
{
    // Ensure any in-flight drag/preview state is cleared before tearing down windows
    m_TearOff.active = false;
    m_TearOff.sourceWindow = nullptr;
    m_TearOff.floatingWindow = nullptr;
    m_LastTearOffPreviewTarget = nullptr;
    m_OSDragActive = false;
    m_OSDragMouseWasDown = false;
    m_LastOSDragPreviewTarget = nullptr;
    m_DropCandidate = DropCandidate{};
}

void WindowDockingController::PerformDockDrop(const DropCandidate& cand)
{
    if (!cand.source || !cand.target || cand.source == cand.target)
    {
        return;
    }
    if (!cand.source->docking || !cand.target->docking)
    {
        return;
    }
    if (cand.source->floatingPanelId.empty())
    {
        return;
    }

    const std::string panelId = cand.source->floatingPanelId;
    UIElement* panel = cand.source->docking->GetPanel(panelId);
    if (!panel && m_Deps.GetMainDocking())
        panel = m_Deps.GetMainDocking()->GetPanel(panelId);
    if (!panel)
    {
        return;
    }

    // Ensure target knows the panel
    if (!cand.target->docking->GetPanel(panelId))
    {
        cand.target->docking->RegisterPanel(panelId, panel);
    }

    // Remove from source model
    cand.source->docking->RemoveTab(panelId);
    // Important: if the focused control lived inside the moved panel, focusId becomes stale.
    // UIManager::OnKey will keep consuming keys while focusId is non-empty, so clear it.
    if (cand.source->ui)
    {
        cand.source->ui->ClearFocus();
        cand.source->ui->ClearHover();
    }
    // Detach the panel subtree from its old UIManager before mounting it in
    // the target. Deferring this rebuild leaves one UIElement tree owned by
    // two windows for a frame and can preserve stale style/layout state.
    m_Deps.RebuildDockspaceNow(cand.source, cand.source->docking);

    // Apply to target based on hierarchical kind
    switch (cand.kind)
    {
    case DropKind::Tab:
        cand.target->docking->DockAsTabInLeafByPath(cand.path, panelId);
        break;
    case DropKind::LeafSplit:
    case DropKind::RegionSplit:
        cand.target->docking->DockSplitSubtreeByPath(cand.path, cand.edge, panelId);
        break;
    case DropKind::RootSplit:
        cand.target->docking->DockToRoot(cand.edge, panelId);
        break;
    case DropKind::None:
    default:
        // Fallback: add as tab to first leaf
        if (auto* leaf = EditorPanelManager::FindFirstLeaf(cand.target->docking->GetRoot()))
        {
            leaf->AddTab(panelId);
            (void)leaf->ActivateTab(panelId);
        }
        else
        {
            auto root = DockNode::MakeLeaf();
            root->AddTab(panelId);
            cand.target->docking->SetRoot(std::move(root));
        }
        break;
    }

    // Ensure the newly docked panel is the active tab
    cand.target->docking->ActivateTab(panelId);

    // Mount into the target immediately so the first frame after docking
    // cannot reuse source-window UI state.
    m_Deps.RebuildDockspaceNow(cand.target, cand.target->docking);

    // Rebuild or close source window
    if (cand.source->docking->IsEmpty())
    {
        if (cand.source->window)
            cand.source->window->RequestClose();
    }
}

void WindowDockingController::SetDockEdgeFraction(float frac)
{
    if (frac < 0.01f)
        frac = 0.01f;
    if (frac > 0.49f)
        frac = 0.49f;
    m_DockEdgeFrac = frac;
}

void WindowDockingController::ClearAllDockOverlays()
{
    for (auto& w : (*m_Deps.Windows))
    {
        if (!w || !w->ui)
            continue;
        if (UIElement* root = w->ui->GetRootElement())
        {
            if (UIElement* dock = root->FindById("dock"))
            {
                if (auto* ds = dynamic_cast<DockspaceElement*>(dock))
                {
                    ds->ClearDropPreview();
                }
            }
        }
    }
}

void WindowDockingController::ShowDropOverlays()
{
    // Clear previous overlays so only the current target is highlighted
    ClearAllDockOverlays();
    // Recompute hierarchical target to ensure cand.kind/path/edge are up to date
    if (!m_DropCandidate.source)
    {
        return;
    }
    DropCandidate fresh = m_DropCandidate;
    if (!ComputeDropCandidateFromCursor(m_DropCandidate.source, fresh) || !fresh.valid)
    {
        ClearAllDockOverlays();
        return;
    }
    m_DropCandidate = fresh;

    EditorWindowContext* target = m_DropCandidate.target;
    if (!target || !target->ui || !target->docking)
    {
        ClearAllDockOverlays();
        return;
    }

    if (UIElement* root = target->ui->GetRootElement())
    {
        if (UIElement* dock = root->FindById("dock"))
        {
            // Use UI layout coordinates directly for DockOverlay
            const float dww = dock->GetLayoutWidth();
            const float dhw = dock->GetLayoutHeight();

            DockDropTarget dt;
            dt.Kind = (m_DropCandidate.kind == DropKind::Tab)           ? DockDropTarget::TargetKind::TabBar
                      : (m_DropCandidate.kind == DropKind::LeafSplit)   ? DockDropTarget::TargetKind::LeafSplit
                      : (m_DropCandidate.kind == DropKind::RegionSplit) ? DockDropTarget::TargetKind::RegionSplit
                      : (m_DropCandidate.kind == DropKind::RootSplit)   ? DockDropTarget::TargetKind::RootSplit
                                                                        : DockDropTarget::TargetKind::None;
            dt.Path = m_DropCandidate.path;
            dt.Edge = m_DropCandidate.edge;

            if (auto* ds = dynamic_cast<DockspaceElement*>(dock))
            {
                ds->SetDropPreview(dt, dww, dhw);
            }
        }
    }
}

bool WindowDockingController::ComputeDropCandidateFromCursor(EditorWindowContext* source, DropCandidate& outCandidate)
{
    outCandidate = DropCandidate{};
    if (!source)
        return false;

    // Find target window under cursor
    int cx = 0, cy = 0;
    Platform::Window::GetCursorScreenPosition(cx, cy);
    EditorWindowContext* targetUnderCursor = nullptr;
    for (auto& w : (*m_Deps.Windows))
    {
        auto* t = w.get();
        if (!t || !t->window)
            continue;
        int wx = 0, wy = 0;
        t->window->GetPosition(wx, wy);
        int ww = 0, wh = 0;
        t->window->GetWindowSize(ww, wh);
        if (cx >= wx && cy >= wy && cx < wx + ww && cy < wy + wh)
        {
            targetUnderCursor = t;
            break;
        }
    }

    if (!targetUnderCursor || !targetUnderCursor->ui)
        return false;
    if (UIElement* root = targetUnderCursor->ui->GetRootElement())
    {
        if (UIElement* dock = root->FindById("dock"))
        {
            int wx = 0, wy = 0;
            targetUnderCursor->window->GetPosition(wx, wy);
            const float scale = WindowInputRouter::UiLogicalToClientScale(
                targetUnderCursor->window.get(), targetUnderCursor->ui.get());
            const float dxw = dock->GetLayoutX() * scale;
            const float dyw = dock->GetLayoutY() * scale;
            const float dww = dock->GetLayoutWidth() * scale;
            const float dhw = dock->GetLayoutHeight() * scale;
            const float sleft = wx + dxw;
            const float stop = wy + dyw;

            const float edgeW = dww * m_DockEdgeFrac;
            const float edgeH = dhw * m_DockEdgeFrac;
            enum Zone
            {
                ZoneCenter = 0,
                ZoneLeft,
                ZoneRight,
                ZoneTop,
                ZoneBottom
            } zone = ZoneCenter;
            // Compute hierarchical target (local coords)
            const float localX = static_cast<float>(cx) - sleft;
            const float localY = static_cast<float>(cy) - stop;
            DockDropTarget target = DockingHitTest::Compute(*targetUnderCursor->docking, dww, dhw, localX, localY);

            // Legacy overlay zone (fallback visualization)
            if (cx < sleft + edgeW)
                zone = ZoneLeft;
            else if (cx >= sleft + dww - edgeW)
                zone = ZoneRight;
            else if (cy < stop + edgeH)
                zone = ZoneTop;
            else if (cy >= stop + dhw - edgeH)
                zone = ZoneBottom;
            else
                zone = ZoneCenter;

            outCandidate.source = source;
            outCandidate.target = targetUnderCursor;
            outCandidate.zone = zone;
            // Map DockDropTarget to DropCandidate
            switch (target.Kind)
            {
            case DockDropTarget::TargetKind::Tab:
            case DockDropTarget::TargetKind::TabBar:
                outCandidate.kind = DropKind::Tab;
                break;
            case DockDropTarget::TargetKind::LeafSplit:
                outCandidate.kind = DropKind::LeafSplit;
                break;
            case DockDropTarget::TargetKind::RegionSplit:
                outCandidate.kind = DropKind::RegionSplit;
                break;
            case DockDropTarget::TargetKind::RootSplit:
                outCandidate.kind = DropKind::RootSplit;
                break;
            default:
                outCandidate.kind = DropKind::None;
                break;
            }
            outCandidate.path = target.Path;
            outCandidate.edge = target.Edge;
            outCandidate.valid = (source != nullptr && targetUnderCursor != nullptr);
            return outCandidate.valid;
        }
    }
    return false;
}

} // namespace Editor
} // namespace GameEngine
