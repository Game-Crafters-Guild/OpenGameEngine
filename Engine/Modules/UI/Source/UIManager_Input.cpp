#include "UI/UIManager.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "UI/Controls/Mount.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/TooltipOverlay.h"
#include "UI/UiContext.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/Utf8Helpers.h"
#include "UIAttributeAccess.h"

#include <filesystem>

using namespace GameEngine;

namespace
{
// Depth budget for ancestor bubble chains: defends event dispatch against
// malformed parent cycles. Far above any legitimate UI tree depth.
constexpr int kMaxBubbleDepth = 2048;

class ScopedInputEventDispatchGuard final
{
public:
    ScopedInputEventDispatchGuard() { UIElement::SetInEventDispatch(true); }
    ~ScopedInputEventDispatchGuard() { UIElement::SetInEventDispatch(false); }

    ScopedInputEventDispatchGuard(const ScopedInputEventDispatchGuard&) = delete;
    ScopedInputEventDispatchGuard& operator=(const ScopedInputEventDispatchGuard&) = delete;
};

// The delivery rule for disabled subtrees: an element that is disabled, or inside a disabled
// element (a mount's content counts as its host's), receives no press, release, key or text
// input and takes no focus, the way a disabled form control behaves. Such input goes to the first
// ancestor outside the disabled subtree instead (FirstInputReceiver), so the panels around it
// still hear it. Hover, and so tooltips, and the wheel still reach the element: scrolling only
// reads, so a disabled list still scrolls, and a control that changes a value on the wheel checks
// its own enabled state. A pointer stream captured before the disable keeps its moves and ends as
// a cancel at the release. Hit testing is unaffected. Controls keep their own enabled checks as a
// second guarantee.
bool ReceivesInput(const UIElement* el)
{
    return el && el->IsEnabledInHierarchy();
}

// Where input aimed at `el` is delivered: `el`, or when it is inside a disabled subtree the first
// ancestor on the event's bubble path (GetParent) outside it. Null when there is none.
UIElement* FirstInputReceiver(UIElement* el)
{
    while (el && !ReceivesInput(el))
        el = el->GetParent();
    return el;
}
} // namespace

void UIManager::OnMouseMove(float x, float y)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    // Avoid treating redundant cursor-pos callbacks (same position) as "input".
    // Some platforms/drivers can emit cursor callbacks even when the cursor
    // hasn't moved, which would defeat UIManager::Update's input-only fast paths
    // and force an expensive BuildYoga on otherwise idle frames.
    if (x == m_MouseX && y == m_MouseY)
    {
        return;
    }
    m_MouseX = x;
    m_MouseY = y;
    m_MouseMoved = true;
}

void UIManager::OnCursorEnter(bool entered)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    if (!entered)
    {
        // When the cursor leaves the window, GLFW may stop delivering cursor-pos
        // callbacks. Force the cursor position to a sentinel outside the UI so
        // hover-dependent widgets (e.g., popovers) can reliably close.
        //
        // However, when a mouse capture is active (scrollbar drag, splitter
        // resize, etc.), preserve the last known cursor position. The sentinel
        // (-1,-1) would produce an incorrect delta for the captured element and
        // can cause the drag to jump or stall. Cursor-pos callbacks typically
        // continue via OS-level capture (SetCapture on Windows), so the position
        // will be updated on subsequent frames.
        if (!m_MouseCaptured)
        {
            m_MouseX = kMousePositionUnknown;
            m_MouseY = kMousePositionUnknown;
        }
        m_MouseMoved = true;

        // IMPORTANT:
        // Do NOT clear m_Hovered here. We want the next Update() to observe
        // prevHover != newHover and dispatch proper MouseLeave events for the
        // old hover chain. Clearing here would lose that information.
        //
        // Note: we intentionally do NOT clear capture here; captured drags should
        // remain active until mouse-up is received.
    }
}

UIElement* UIManager::ResolveButtonTarget() const
{
    // Capture outranks the pointer: a control that took the gesture keeps it
    // even once the cursor has left, which is the same rule the release edge
    // and CancelPress apply.
    if (m_MouseCaptured)
    {
        if (m_CaptureElement)
            return m_CaptureElement;
        if (!m_CaptureId.empty())
        {
            if (UIElement* r = GetRootElement())
            {
                if (UIElement* byId = r->FindById(m_CaptureId))
                    return byId;
            }
        }
    }
    return PointerTargetForDispatch();
}

UIElement* UIManager::PointerTargetForDispatch() const
{
    if (!IsMousePositionKnown())
        return nullptr;
    // m_MouseMoved is raised by any pointer motion, and by a tree mutation under
    // a stationary cursor, and is cleared only once hover has been resolved for
    // the position that is current now. So while it is low the resolved hover
    // already IS the hit test for this position and repeating the walk buys
    // nothing; while it is high the cached hover is a frame behind, and the walk
    // is what makes a press land where the cursor actually is. Resolved by
    // instance id rather than through m_Hovered so a tree rebuild cannot hand
    // back a stale pointer.
    if (!m_MouseMoved)
        return m_HoveredInstanceId != 0 ? FindElementByInstanceId(m_HoveredInstanceId) : nullptr;
    return HitTestTree(m_MouseX, m_MouseY);
}

void UIManager::MarkPseudoStateForSyncTransition(uint64_t activeBeforeInstanceId,
                                                 const std::string& focusIdBefore)
{
    UIElement* root = GetRootElement();
    if (!root)
        return;

    if (m_StyleAnalysis.Active.AffectsLayout || m_StyleAnalysis.Active.AffectsPaint)
    {
        /* Dispatch can rebuild whole subtrees (a handler that clears a panel),
           so the before-element is carried as an instance id and re-resolved
           here — the same stale-pointer rule PointerTargetForDispatch states.
           A destroyed element resolves to null: gone means no dirty to mark. */
        UIElement* activeBefore =
            activeBeforeInstanceId != 0 ? FindElementByInstanceId(activeBeforeInstanceId) : nullptr;
        UIElement* activeNow = m_MouseDown ? ActiveTarget() : nullptr;
        if (activeNow != activeBefore)
        {
            unsigned flags = UIElement::StyleDirty | UIElement::VisualDirty;
            if (m_StyleAnalysis.Active.AffectsLayout)
                flags |= UIElement::LayoutDirty;
            if (activeBefore)
                activeBefore->MarkDirty(flags);
            if (activeNow)
                activeNow->MarkDirty(flags);
        }
    }

    if (m_FocusId == focusIdBefore)
        return;
    // Whole subtree, for the reason the Tab path states: the styled :focus node
    // is often a descendant of the focusable element, and StyleDirty on the
    // ancestor alone does not re-resolve its children.
    constexpr unsigned kFocusFlags = UIElement::StyleDirty | UIElement::VisualDirty;
    if (!focusIdBefore.empty())
    {
        if (UIElement* prevEl = root->FindById(focusIdBefore))
            prevEl->MarkDirtySubtree(kFocusFlags);
    }
    if (!m_FocusId.empty())
    {
        if (UIElement* newEl = root->FindById(m_FocusId))
            newEl->MarkDirtySubtree(kFocusFlags);
    }
}

bool UIManager::OnMouseButton(int button, bool down)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    // Accept any mouse button. Dispatched here, at the callback, so both edges
    // of a click that completes inside one frame interval are delivered in
    // platform order — there is no sampling boundary left for a transition to
    // be lost at.
    // One pressed-state latch across all buttons, so a second button pressed
    // while the first is held carries no transition of its own — an edge is a
    // change of that latch. Reporting the newest button either way keeps the
    // release edge naming whichever button most recently spoke.
    const bool isEdge = (down != m_MouseDown);

    UIElement* const activeBeforeEl = m_MouseDown ? ActiveTarget() : nullptr;
    const uint64_t activeBefore = activeBeforeEl ? activeBeforeEl->GetInstanceId() : 0;
    m_FocusIdBeforeButtonDispatch = m_FocusId;

    m_MouseButton = button;
    m_MouseDown = down;
    if (!isEdge)
        return false;
    m_ButtonEdgeSinceLastUpdate = true;

    // Handlers post work (inspector rebuilds, popup dismissals) instead of
    // mutating the tree underneath the dispatch; DispatchBubbleEvent runs that
    // deferred work before it returns, so the tree is settled here.
    const bool consumed = down ? DispatchMouseButtonPress() : DispatchMouseButtonRelease();

    MarkPseudoStateForSyncTransition(activeBefore, m_FocusIdBeforeButtonDispatch);

    return consumed;
}

bool UIManager::DispatchMouseButtonPress()
{
    // Any pointer press takes priority over hover help. Dismiss at the input
    // boundary so every tooltip kind follows the same rule, even when the
    // target does not open a context menu.
    DismissActiveTooltip();

    UIElement* const pointerTarget = PointerTargetForDispatch();
    UIElement* target = m_MouseCaptured ? ResolveButtonTarget() : pointerTarget;

    constexpr int kLeftMouseButton = 0;
    constexpr int kRightMouseButton = 1;
    if (m_MouseButton == kLeftMouseButton || m_MouseButton == kRightMouseButton)
    {
        if (DismissPopupsForOutsidePress(target))
        {
            // Consume the outside click that dismissed the popup, and capture
            // the rest of this press so controls underneath the closing popup
            // cannot reinterpret it as a drag.
            m_FocusViaKeyboard = false;
            m_FocusId.clear();
            if (UIElement* root = GetRootElement())
            {
                m_MouseCaptured = true;
                m_CaptureElement = root;
                m_CaptureInstanceId = root->GetInstanceId();
                m_CaptureId = root->GetId();
            }
            return true;
        }
    }

    // Middle-mouse — notify the tooltip overlay for immediate show, when the
    // host has enabled that option.
    constexpr int kMiddleMouseButton = 2;
    if (m_MouseButton == kMiddleMouseButton && m_TooltipOverlay && m_TooltipConfigProvider &&
        m_TooltipConfigProvider().MiddleMouseShow)
    {
        m_TooltipOverlay->OnMiddleMousePressed(pointerTarget);
    }

    // Reset the explicit-focus tracker before dispatch; SetFocusById sets it
    // when a handler claims focus (e.g. ListView calling FocusElement via
    // PostSafeAction). If that happens, the post-dispatch auto-assignment below
    // defers to the handler's choice instead of overwriting it.
    m_FocusExplicitSetDuringDispatch = false;

    bool treeMutated = false;
    const uint64_t treeGenBefore = m_TreeStructureGeneration.load(std::memory_order_relaxed);
    // A press on a disabled subtree goes to the first ancestor outside it (FirstInputReceiver), so
    // nothing in it arms, captures or starts a drag. It still dismissed an open popup above.
    if (!m_MouseCaptured)
        target = FirstInputReceiver(target);
    // DispatchBubbleEvent, not a bare bubble: accepting a handler's capture
    // request is part of dispatching a press, and it is what every drag,
    // slider and scrollbar gesture is built on.
    const bool consumed =
        target ? DispatchBubbleEvent(target, kEventMouseDown, m_MouseButton, /*bubble=*/true, treeMutated,
                                     treeGenBefore)
               : false;

    m_FocusViaKeyboard = false;
    // If the press started a captured interaction (drag, slider, etc.), defer
    // focus until mouse-up so focus-driven layout does not invalidate capture
    // before move.
    if (!treeMutated && !m_FocusExplicitSetDuringDispatch && !m_MouseCaptured)
    {
        // A disabled control takes no focus from a press, as it takes none from
        // Tab: pressing one leaves the caret nowhere rather than parking it on a
        // control that refuses every key it is then sent.
        UIElement* focusTarget = pointerTarget ? pointerTarget->ResolveFocusTarget() : nullptr;
        if (focusTarget && focusTarget->IsFocusable() && focusTarget->IsEnabledInHierarchy())
        {
            EnsureElementId(focusTarget);
            m_FocusId = focusTarget->GetId();
        }
        else
        {
            m_FocusId.clear();
        }
    }

    return consumed;
}

bool UIManager::DispatchMouseButtonRelease()
{
    // A gesture whose capturer was disabled mid-drag (it or a container of it) ends as a cancel,
    // not a release (ReceivesInput): it lets go cleanly and completes nothing, no click and no drop.
    if (m_MouseCaptured)
    {
        if (UIElement* capturer = ResolveButtonTarget(); capturer && !ReceivesInput(capturer))
        {
            CancelPress();
            return true;
        }
    }

    const bool hadCapture = m_MouseCaptured;
    UIElement* const capturedForFocus = m_CaptureElement;
    const std::string capturedIdForFocus = m_CaptureId;

    UIElement* const pointerTarget = PointerTargetForDispatch();
    UIElement* upTarget = hadCapture ? ResolveButtonTarget() : pointerTarget;
    // A release on a disabled subtree goes to the first ancestor outside it (FirstInputReceiver).
    if (!hadCapture)
        upTarget = FirstInputReceiver(upTarget);

    bool treeMutated = false;
    const uint64_t treeGenBefore = m_TreeStructureGeneration.load(std::memory_order_relaxed);
    // Same tracker as the press half: a handler that claims focus during this
    // release (a toolbar button whose click opens a dialog and focuses its
    // field) keeps it, and the auto-assignment below stays out of the way.
    m_FocusExplicitSetDuringDispatch = false;
    const bool consumed =
        upTarget ? DispatchBubbleEvent(upTarget, kEventMouseUp, m_MouseButton, /*bubble=*/true, treeMutated,
                                       treeGenBefore)
                 : false;

    // Commit any active drag/drop session on mouse up (after the UI event bubbles).
    if (m_DragDrop && m_DragDrop->IsDragging())
    {
        m_DragDrop->CommitDrop(m_Modifiers.Mask());
    }

    m_MouseCaptured = false;
    m_CaptureElement = nullptr;
    m_CaptureInstanceId = 0;
    m_CaptureId.clear();
    if (hadCapture)
    {
        // Retarget hover to the element under the pointer after capture ends,
        // even if the pointer stays still.
        m_MouseMoved = true;
    }

    if (treeMutated)
        return consumed;

    bool preserveFocus = false;
    if (hadCapture && !m_FocusId.empty())
    {
        UIElement* capturedResolved = capturedForFocus;
        if (!capturedResolved && !capturedIdForFocus.empty())
        {
            if (UIElement* root = GetRootElement())
                capturedResolved = root->FindById(capturedIdForFocus);
        }
        if (capturedResolved)
            preserveFocus = capturedResolved->IsFocusTargetForId(m_FocusId);
    }
    if (preserveFocus)
        return consumed;
    if (m_FocusExplicitSetDuringDispatch)
        return consumed;

    // A captured press deferred its focus assignment to this release (see the
    // press half). Honor the capture's focus target first so a drag that ends
    // outside the control (text drag-selection past the field edge) still
    // focuses it; non-focusable captures (scrollbar thumb, splitter) fall
    // through without touching focus.
    UIElement* released = nullptr;
    if (hadCapture)
    {
        released = capturedForFocus;
        if (!released && !capturedIdForFocus.empty())
        {
            if (UIElement* root = GetRootElement())
                released = root->FindById(capturedIdForFocus);
        }
        if (released && !(released->ResolveFocusTarget() && released->ResolveFocusTarget()->IsFocusable()))
            released = nullptr;
    }
    if (!released)
        released = pointerTarget;
    UIElement* focusTarget = released ? released->ResolveFocusTarget() : nullptr;
    if (focusTarget && focusTarget->IsFocusable() && focusTarget->IsEnabledInHierarchy())
    {
        EnsureElementId(focusTarget);
        m_FocusId = focusTarget->GetId();
    }
    // Do not clear focus when the release target is not focusable (e.g. modal
    // backdrop). Mouse-down on the same non-focusable element already cleared
    // focus; clearing again on mouse-up would steal focus from a control that
    // gained it during this gesture (e.g. SearchDialog::Show sets focus, then
    // release over the backdrop would wipe it).

    return consumed;
}

void UIManager::CancelPress()
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    if (!m_MouseDown && !m_MouseCaptured)
        return;

    // Same target rule as the release edge: capture outranks the pointer.
    UIElement* target = ResolveButtonTarget();

    UIElement* const activeBeforeEl = m_MouseDown ? ActiveTarget() : nullptr;
    const uint64_t activeBefore = activeBeforeEl ? activeBeforeEl->GetInstanceId() : 0;
    // Disarm before dispatch, so a handler that queries the manager mid-cancel
    // sees a gesture that is already over.
    m_MouseDown = false;
    // The cancel is itself a transition: the frame still owes an :active
    // re-cascade and a hover re-evaluation for it.
    m_ButtonEdgeSinceLastUpdate = true;

    if (target)
    {
        bool treeMutated = false;
        const uint64_t treeGenBefore = m_TreeStructureGeneration.load(std::memory_order_relaxed);
        DispatchBubbleEvent(target, kEventMouseCancel, m_MouseButton, /*bubble=*/true, treeMutated,
                            treeGenBefore);
    }

    // An abandoned drag is cancelled, never committed: the drop position is exactly the
    // thing the pointer took with it.
    if (m_DragDrop && m_DragDrop->IsDragging())
        m_DragDrop->CancelDrag();

    // Cleared after dispatch so a handler cannot re-capture into a dead gesture.
    m_MouseCaptured = false;
    m_CaptureElement = nullptr;
    m_CaptureInstanceId = 0;
    m_CaptureId.clear();
    // Retarget hover from scratch once a pointer is fed again.
    m_MouseMoved = true;

    // Focus is untouched by a cancel — only the :active half has anything to do.
    MarkPseudoStateForSyncTransition(activeBefore, m_FocusId);
}

bool UIManager::BubbleEventFrom(UIElement* target, UIEvent& ev)
{
    if (!target)
        return false;

    // Snapshot the chain by instanceId: a handler can rebuild the tree while we
    // are walking it, and raw parent pointers would dangle. The depth budget and
    // the repeat check also defend against malformed parent cycles.
    static thread_local std::vector<std::uint64_t> bubbleIds;
    bubbleIds.clear();
    bubbleIds.reserve(16);
    {
        UIElement* pBuild = target;
        int depth = 0;
        while (pBuild && depth++ < kMaxBubbleDepth)
        {
            const std::uint64_t iid = pBuild->GetInstanceId();
            if (iid == 0)
                break;
            if (!bubbleIds.empty() && std::find(bubbleIds.begin(), bubbleIds.end(), iid) != bubbleIds.end())
                break;
            bubbleIds.push_back(iid);
            pBuild = pBuild->GetParent();
        }
    }

    ev.Target = target;
    for (const std::uint64_t iid : bubbleIds)
    {
        UIElement* p = FindElementByInstanceId(iid);
        if (!p)
            continue;
        ev.CurrentTarget = p;
        p->DispatchEvent(ev);
        if (ev.Handled)
            break;
    }
    return ev.Handled;
}

bool UIManager::DispatchInputEvent(UIEvent& ev, bool allowHoveredFallback)
{
    UIElement* root = GetRootElement();
    UIElement* focusedEl = (root && !m_FocusId.empty()) ? root->FindById(m_FocusId) : nullptr;
    UIElement* hoveredEl =
        (allowHoveredFallback && m_HoveredInstanceId != 0) ? FindElementByInstanceId(m_HoveredInstanceId) : nullptr;
    // Keys and text never reach a disabled subtree, whether by focus (a control disabled while it
    // holds focus, before the focus chain drops it) or by the hover fallback: they go to the first
    // ancestor outside it (FirstInputReceiver), so a panel's own keys keep working over it.
    focusedEl = FirstInputReceiver(focusedEl);
    hoveredEl = FirstInputReceiver(hoveredEl);

    bool handled = false;
    {
        ScopedInputEventDispatchGuard dispatchGuard;
        handled = BubbleEventFrom(focusedEl ? focusedEl : hoveredEl, ev);

        // Focus answered first and declined. A hovered element is the other
        // thing the user is aiming this key at, so it gets its turn — this is
        // what keeps a viewport shortcut firing while an unrelated control
        // holds focus.
        if (!handled && focusedEl && hoveredEl && hoveredEl != focusedEl)
        {
            ev.Handled = false;
            handled = BubbleEventFrom(hoveredEl, ev);
        }
    }

    // Handlers post work (inspector rebuilds, popup dismissals) instead of
    // mutating the tree underneath the dispatch. Run it now that we have left.
    if (m_Scheduler)
        m_Scheduler->ProcessDue();
    if (m_Dispatcher)
        m_Dispatcher->Drain();
    return handled;
}

bool UIManager::OnChar(unsigned int codepoint)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    if (!Utf8::IsTextInputCodepoint(codepoint))
        return false;
    if (m_FocusId.empty())
        return false;

    m_KeyInputSinceLastUpdate = true;

    UIEvent ev{};
    ev.Id = kEventTextInput;
    ev.X = m_MouseX;
    ev.Y = m_MouseY;
    ev.Codepoint = codepoint;
    // Text follows focus, never the pointer: a character belongs to whatever is
    // being typed into, not to whatever the cursor happens to rest over.
    return DispatchInputEvent(ev, /*allowHoveredFallback=*/false);
}

bool UIManager::OnKey(int key, int action, int mods)
{
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    // Key action constants (GLFW-compatible values)
    constexpr int kActionRelease = 0; // GLFW_RELEASE
    constexpr int kActionPress   = 1; // GLFW_PRESS
    constexpr int kActionRepeat  = 2; // GLFW_REPEAT

    const bool isPress = (action == kActionPress);
    const bool isRelease = (action == kActionRelease);
    const bool isRepeat = (action == kActionRepeat);

    // Optional diagnostics: log each key event with where it landed and whether
    // anything consumed it. Enable with GE_UI_KEYEVENT_LOG=1.
    static const bool s_KeyLog = []() -> bool
    {
        const char* e = std::getenv("GE_UI_KEYEVENT_LOG");
        return (e && e[0] == '1');
    }();
    static int s_KeyLogBudget = 128;

    m_Modifiers.ApplyKeyEvent(key, isRelease, mods);

    bool consumed = false;

    // The UI's diagnostic hotkeys are opt-in (GE_UI_DEBUG_KEYS=1), because a
    // bare function key belongs to whatever is running: claiming F4-F12 here
    // withholds them from the playing game and from every application behind
    // the UI, and does it silently. Read once per manager at construction.
    const bool debugHotkey = m_DebugKeysEnabled && isPress && Input::IsFunctionKey(key);

    // Debug toggle: F4 toggles text debug overlay and requests a one-shot dump (press only)
    if (debugHotkey && key == Input::kKeyCode_F4)
    {
        m_TextDebugOverlayEnabled = !m_TextDebugOverlayEnabled;
        Logger::Log::Info("[UI] Text debug overlay {}", m_TextDebugOverlayEnabled ? "ON" : "OFF");
        m_TextDebugDumpRequested = true; // write a snapshot this frame
        return true;                     // do not forward to focused field or input system
    }

    // Text clip/scissor debug toggles (press only)
    if (debugHotkey && key == Input::kKeyCode_F5)
    {
        m_TextDebugDisableElementScissor = !m_TextDebugDisableElementScissor;
        Logger::Log::Info("[UI] Text element scissor {}", m_TextDebugDisableElementScissor ? "DISABLED (clip-stack only)" : "ENABLED");
        return true;
    }
    if (debugHotkey && key == Input::kKeyCode_F6)
    {
        m_TextDebugDisableClipStack = !m_TextDebugDisableClipStack;
        Logger::Log::Info("[UI] Text clip-stack scissor {}", m_TextDebugDisableClipStack ? "DISABLED" : "ENABLED");
        return true;
    }
    if (debugHotkey && key == Input::kKeyCode_F7)
    {
        // Cycle debug expansion: 0 -> 1 -> 2 -> 4 -> 0
        int next = 0;
        if (m_TextDebugExpandScissorPx == 0) next = 1;
        else if (m_TextDebugExpandScissorPx == 1) next = 2;
        else if (m_TextDebugExpandScissorPx == 2) next = 4;
        else next = 0;
        m_TextDebugExpandScissorPx = next;
        Logger::Log::Info("[UI] Text scissor expand {}", m_TextDebugExpandScissorPx);
        return true;
    }
    if (debugHotkey && key == Input::kKeyCode_F8)
    {
        m_TextDebugDumpRequested = true;
        Logger::Log::Info("[UI] Text debug dump requested (UI_TextDebug.txt)");
        return true;
    }

    // UI Update profiling toggles (press only)
    if (debugHotkey && key == Input::kKeyCode_F9)
    {
        const bool next = !IsUpdateProfilingEnabled();
        SetUpdateProfilingEnabled(next);
        Logger::Log::Info("[UI] Update profiling {}", next ? "ON" : "OFF");
        // When enabling, print a one-shot summary after a few frames collect.
        if (next)
            RequestUpdateProfilingDump();
        return true;
    }
    if (debugHotkey && key == Input::kKeyCode_F10)
    {
        RequestUpdateProfilingDump();
        Logger::Log::Info("[UI] Update profiling dump requested");
        return true;
    }

// Export current UI layout + resolved styles to disk, for diagnosing flex
// sizing, background gaps, and scrollbars. This one keeps a compile-time guard
// on top of the env gate because DebugExportLayoutAndStyles itself only exists
// in debug builds.
#if defined(_DEBUG)
    if (debugHotkey && key == Input::kKeyCode_F11)
    {
        std::error_code ec;
        const std::filesystem::path outDir = std::filesystem::current_path(ec) / "UI_DebugExport";
        if (!ec)
        {
            std::filesystem::create_directories(outDir, ec);
        }

        if (ec)
        {
            Logger::Log::Warning("[UI] Failed to create UI_DebugExport directory ({})", ec.message());
            return true;
        }

        const bool ok = DebugExportLayoutAndStyles(outDir.string());
        Logger::Log::Info("[UI] DebugExportLayoutAndStyles {} -> {}", ok ? "OK" : "FAILED", outDir.string());
        return true;
    }
#endif

    // Debug capture toggle: start/stop per-frame UI capture to JSONL.
    if (debugHotkey && key == Input::kKeyCode_F12)
    {
        ToggleDebugCapture();
        return true;
    }

    // Layout diagnostics hotkeys removed (trim debug).

    // Tab navigation across focusable elements (text fields, buttons). Fires on
    // press and on OS key-repeat so holding Tab keeps advancing focus.
    if ((isPress || isRepeat) && key == Input::kKeyCode_Tab)
    {
        if (m_FocusOrder.empty())
        {
            // Focus order is normally rebuilt during UIManager::Update(). However, hosts/tests may
            // call Tab before an interactive update occurs (or after a large tree swap). Build a
            // best-effort focus order from the current live tree using resolved styles.
            if (UIElement* root = GetRootElement())
            {
                struct Candidate
                {
                    std::string id;
                    int tabIndex = 0;
                    size_t domOrder = 0;
                };
                std::vector<Candidate> candidates;
                candidates.reserve(64);

                std::vector<UIElement*> stack;
                stack.reserve(128);
                stack.push_back(root);
                size_t dom = 0;
                while (!stack.empty())
                {
                    UIElement* el = stack.back();
                    stack.pop_back();
                    if (!el)
                        continue;
                    ++dom;

                    const ResolvedStyle& st = el->GetResolvedStyle();
                    if (!st.Visual.Visible || st.Layout.DisplayMode == DisplayMode::None)
                    {
                        // Skip subtree if explicitly not displayed; mirrors Update() focus collection.
                        continue;
                    }
                    if (!el->IsEnabled())
                    {
                        // Skip subtree: a disabled container disables its contents.
                        continue;
                    }
                    if (el->IsFocusable())
                    {
                        int tabIndex = el->GetTabIndex();
                        if (tabIndex >= 0)
                        {
                            std::string id = el->GetId();
                            if (!id.empty())
                                candidates.push_back({id, tabIndex, dom});
                        }
                    }

                    // DFS: push children in reverse so the first child is visited first.
                    const auto& ch = el->GetChildren();
                    for (size_t i = ch.size(); i-- > 0;)
                        if (ch[i])
                            stack.push_back(ch[i].get());
                    if (auto* m = dynamic_cast<Mount*>(el))
                        if (UIElement* tgt = m->GetTarget())
                            stack.push_back(tgt);
                }

                std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
                                 {
                                     const auto rankA = (a.tabIndex > 0) ? 0 : 1;
                                     const auto rankB = (b.tabIndex > 0) ? 0 : 1;
                                     if (rankA != rankB) return rankA < rankB;
                                     if (rankA == 0 && a.tabIndex != b.tabIndex) return a.tabIndex < b.tabIndex;
                                     return a.domOrder < b.domOrder;
                                 });
                m_FocusOrder.clear();
                for (const auto& c : candidates)
                    m_FocusOrder.push_back(c.id);
            }
        }
        if (m_FocusOrder.empty())
            return false;
        m_FocusViaKeyboard = true;

        // Tab cycles within the panel that currently holds focus, wrapping at the
        // ends, instead of walking every focusable in the window. Without this,
        // tabbing past the last inspector field would continue into other panels
        // rather than returning to the first field. The scope is the nearest
        // ancestor carrying the "panel" class (added by DockPanel); when focus is
        // not inside a panel we fall back to the global order.
        UIElement* root = GetRootElement();
        UIElement* focusedEl = (root && !m_FocusId.empty()) ? root->FindById(m_FocusId) : nullptr;
        UIElement* scope = nullptr;
        for (UIElement* p = focusedEl; p; p = p->GetParent())
        {
            if (p->HasClass("panel"))
            {
                scope = p;
                break;
            }
        }

        std::vector<std::string> scopedOrder;
        if (scope && root)
        {
            auto isWithin = [](UIElement* el, UIElement* ancestor) -> bool
            {
                for (UIElement* p = el; p; p = p->GetParent())
                    if (p == ancestor)
                        return true;
                return false;
            };
            for (const std::string& id : m_FocusOrder)
            {
                UIElement* e = root->FindById(id);
                if (e && isWithin(e, scope))
                    scopedOrder.push_back(id);
            }
        }
        const std::vector<std::string>& order = scopedOrder.empty() ? m_FocusOrder : scopedOrder;

        // find current index within the active order
        int idx = -1;
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == m_FocusId)
            {
                idx = (int)i;
                break;
            }
        bool backwards = (mods & Input::kModShift) != 0;
        if (backwards)
            idx = (idx <= 0) ? (int)order.size() - 1 : idx - 1;
        else
            idx = (idx + 1) % (int)order.size();
        const std::string prevFocusId = m_FocusId;
        m_FocusId = order[idx];
        // OnKey runs before Update snapshots focus (ctx.focusIdForYoga), so the
        // DispatchEvents pass that repaints :focus / :focus-visible on mouse and
        // programmatic focus changes sees no diff for Tab and the focus outline
        // would not follow. Re-cascade the outgoing and incoming elements here so
        // the outline tracks Tab immediately. We mark the whole subtree because
        // the styled :focus node is often a descendant of the focusable element
        // (e.g. a FloatField's inner .float-field-input), and StyleDirty on the
        // ancestor alone does not re-resolve its children.
        if (root)
        {
            if (!prevFocusId.empty())
                if (UIElement* prevEl = root->FindById(prevFocusId))
                    prevEl->MarkDirtySubtree(UIElement::StyleDirty | UIElement::VisualDirty);
            if (UIElement* newEl = root->FindById(m_FocusId))
                newEl->MarkDirtySubtree(UIElement::StyleDirty | UIElement::VisualDirty);
        }
        return true;
    }

    // Ignore unknown keys: nothing can handle them meaningfully and some
    // IME/platform paths generate noisy UNKNOWN events.
    constexpr int kKeyCode_Unknown = -1; // GLFW_KEY_UNKNOWN
    if (key == kKeyCode_Unknown)
        return false;

    if (!isPress && !isRelease && !isRepeat)
        return false;

    m_KeyInputSinceLastUpdate = true;

    // Keyboard input dismisses hover help, the same as a pointer press and the
    // same as every native tooltip (Win32 cancels a tooltip on relayed key
    // input; Chrome hides the title tooltip on keydown). Non-consuming: the key
    // still reaches its handlers below, which is also what makes hover content
    // dismissible without moving the pointer (WCAG 2.1 SC 1.4.13).
    if (isPress || isRepeat)
        DismissActiveTooltip();

    UIEvent ev{};
    ev.Id = isRelease ? kEventKeyUp : kEventKeyDown; // repeats are presses for navigation
    ev.X = m_MouseX;
    ev.Y = m_MouseY;
    ev.Key = key;
    ev.Mods = mods;
    ev.Platform = m_Platform;

    // A focus move a key handler makes is keyboard focus (SetFocusById), as Tab's is.
    const bool outerKeyDispatch = m_DispatchingKey;
    m_DispatchingKey = true;
    consumed = DispatchInputEvent(ev, m_KeyHoverFallbackEnabled);
    m_DispatchingKey = outerKeyDispatch;

    // Escape closes the topmost open popup — but only once bubbling has declined
    // it, so a control inside the popup (a search field clearing its query
    // first) keeps priority. Opening a popup takes no focus, so this is also the
    // ordinary path for one raised by a toolbar click the pointer has since left.
    if (!consumed && !isRelease && key == Input::kKeyCode_Escape)
        consumed = DismissTopmostPopup();

    if (s_KeyLog && s_KeyLogBudget-- > 0)
    {
        const char* act = isRelease ? "RELEASE" : (isRepeat ? "REPEAT" : "PRESS");
        const std::string hoverTagStr = m_Hovered ? UIAttributeAccess::GetDebugTypeName(*m_Hovered) : std::string();
        const char* hoverTag = m_Hovered ? hoverTagStr.c_str() : "<null>";
        const char* hoverId = "<null>";
        if (m_Hovered)
            hoverId = m_Hovered->GetId().empty() ? "<no-id>" : m_Hovered->GetId().c_str();
        Logger::Log::Warning(
            "[UI KeyEvent] key={} action={} mods=0x{} consumed={} focus='{}' hovered(tag='{}' id='{}')",
            key,
            act,
            mods,
            consumed ? 1 : 0,
            m_FocusId.empty() ? "<none>" : m_FocusId.c_str(),
            hoverTag,
            hoverId);
    }

    // The dispatch chain's own answer is the whole answer: an element that acted
    // stops the key here, and anything nobody acted on routes on to the
    // application's registered shortcuts. A control must therefore consume only
    // the chords it implements — over-consumption here is a stolen accelerator,
    // not a missing exemption.
    return consumed;
}

void UIManager::ClearFocus()
{
    m_FocusId.clear();
}

int UIManager::GetModifierKeys() const
{
    return m_Modifiers.Mask();
}

void UIManager::ResetModifierKeys()
{
    m_Modifiers.Reset();
}

void UIManager::SyncModifierKeys(int mods)
{
    m_Modifiers.ApplyEventMask(mods);
}

void UIManager::ReconcileModifierKeys(int liveMods)
{
    m_Modifiers.ApplyLiveMask(liveMods);
}

void UIManager::SetFocusById(const std::string& id)
{
    // A disabled element, or one inside a disabled element, takes no focus (ReceivesInput): the
    // request leaves focus where it was. An id that does not resolve yet is kept, as before.
    if (!id.empty() && GetRootElement())
    {
        if (const UIElement* el = GetRootElement()->FindById(id); el && !ReceivesInput(el))
            return;
    }
    m_FocusId = id;
    m_FocusExplicitSetDuringDispatch = true;
    // Made while a key is dispatched, the move answers that key: :focus-visible draws, as after
    // Tab. A press clears the origin again after its own dispatch.
    if (m_DispatchingKey)
        m_FocusViaKeyboard = true;
    // Mark style dirty so :focus pseudo-class updates on the target element.
    if (!id.empty() && GetRootElement())
    {
        if (auto* el = GetRootElement()->FindById(id))
            el->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    }
}

void UIManager::FocusElement(UIElement* el)
{
    if (!el)
        return;
    SetFocusById(EnsureElementId(el));
}

bool UIManager::OnScroll(float xoffset, float yoffset)
{
    // Bind TLS UiContext so any UI calls within scroll handlers route to this manager.
    UI::UiContextScope uiScope(m_Dispatcher.get(), m_Scheduler.get());
    (void)xoffset;

    // Scrolling moves the content the tooltip is anchored to, so the hover help
    // stops describing what is under the pointer. Browsers hide it here too.
    DismissActiveTooltip();

    // Convert wheel deltas into pixel scroll amounts and route through UI.Scroll.
    // Default wheel scrolls vertically; Shift+wheel scrolls horizontally.
    const float k = 30.0f; // pixels per wheel step
    float dx = 0.0f, dy = 0.0f;
    if (m_Modifiers.Shift() && !m_Modifiers.PrimaryModifier())
    {
        // Shift+wheel: horizontal scroll using the vertical delta.
        dx = -yoffset * k;
    }
    else
    {
        // Default (including primary modifier): vertical scroll only.
        dy = -yoffset * k;
    }

    if (dx == 0.0f && dy == 0.0f)
        return false;

    UIElement* target = nullptr;
    if (m_HoveredInstanceId != 0)
    {
        target = FindElementByInstanceId(m_HoveredInstanceId);
    }
    // With nothing under the pointer, follow focus: a keyboard-focused text field
    // inside a ScrollView still scrolls, and bubbling finds the scrollable
    // ancestor without UIManager naming ScrollView. Then the first focusable
    // element in tab order, for a flow that has focused nothing yet. A game
    // surface turns both off (SetScrollFocusFallbackEnabled): its HUD passes the
    // pointer through to the world, so a tick with nothing hovered is the game's.
    if (m_ScrollFocusFallbackEnabled)
    {
        if (!target && !m_FocusId.empty())
        {
            if (UIElement* root = GetRootElement())
            {
                target = root->FindById(m_FocusId);
            }
        }
        if (!target && m_FocusId.empty() && !m_FocusOrder.empty())
        {
            if (UIElement* root = GetRootElement())
            {
                target = root->FindById(m_FocusOrder[0]);
            }
        }
    }
    if (!target)
        return false;
    UIEvent ev{};
    ev.Id = kEventScroll;
    ev.X = m_MouseX;
    ev.Y = m_MouseY;
    ev.ScrollX = dx;
    ev.ScrollY = dy;
    ev.Mods = m_Modifiers.Mask();
    ev.Target = target;

    // Snapshot bubble chain by instanceId to avoid UAF if handlers rebuild the UI.
    static thread_local std::vector<std::uint64_t> bubbleIds;
    bubbleIds.clear();
    bubbleIds.reserve(16);
    {
        UIElement* pBuild = target;
        int depth = 0;
        while (pBuild && depth++ < kMaxBubbleDepth)
        {
            const std::uint64_t iid = pBuild->GetInstanceId();
            if (iid == 0)
                break;
            if (!bubbleIds.empty() && std::find(bubbleIds.begin(), bubbleIds.end(), iid) != bubbleIds.end())
                break;
            bubbleIds.push_back(iid);
            pBuild = pBuild->GetParent();
        }
    }

    {
        ScopedInputEventDispatchGuard dispatchGuard;
        for (size_t di = 0; di < bubbleIds.size() && di < static_cast<size_t>(kMaxBubbleDepth); ++di)
        {
            UIElement* p = FindElementByInstanceId(bubbleIds[di]);
            if (!p)
                continue;
            ev.CurrentTarget = p;
            p->DispatchEvent(ev);
            if (ev.Handled)
                break;
        }
        if (bubbleIds.size() >= static_cast<size_t>(kMaxBubbleDepth) && !ev.Handled)
        {
            Logger::Log::Warning("UI: ancestor depth limit reached during scroll event bubbling");
        }
    }
    // Treat scroll as input for Update() fast-path gating.
    // ScrollView applies scrolling via retained "positions-only" patching during Update().
    // If Update() early-outs (no input/no dirty), scroll translations won't be applied until
    // some later mouse/key event, which looks like "scroll only updates on mouse-up".
    if (ev.Handled)
        m_ScrollWheelMoved = true;
    return ev.Handled;
}

// ---------------------------------------------------------------------------
// DispatchBubbleEvent (extracted from Update lambda)
// ---------------------------------------------------------------------------

bool UIManager::DispatchBubbleEvent(UIElement* target, EventId id, int button,
                                    bool bubble, bool& treeMutated,
                                    uint64_t treeGenBefore)
{
    if (!target)
        return false;
    UIEvent ev;
    ev.Id = id;
    ev.X = m_MouseX;
    ev.Y = m_MouseY;
    if (id == kEventMouseCancel)
    {
        // No release position exists — the pointer is off the surface, while m_MouseX/Y
        // still hold wherever it was last seen, which is typically inside the armed
        // control. Anything that reads a position off this event must not find one.
        ev.X = kMousePositionUnknown;
        ev.Y = kMousePositionUnknown;
    }
    ev.Button = button;
    ev.ButtonDown = m_MouseDown;
    ev.Target = target;
    ev.Mods = m_Modifiers.Mask();
    // Snapshot a stable bubble chain by instanceId so UI tree mutations during dispatch
    // (e.g., inspector rebuilding controls) can't produce use-after-free.
    //
    // This also defends against malformed parent cycles by enforcing a depth budget.
    static thread_local std::vector<std::uint64_t> bubbleIds;
    bubbleIds.clear();
    bubbleIds.reserve(16);
    {
        UIElement* pBuild = target;
        int depth = 0;
        while (pBuild && depth++ < kMaxBubbleDepth)
        {
            const std::uint64_t iid = pBuild->GetInstanceId();
            if (iid == 0)
                break;
            // Cycle guard: if we see the same id twice, stop.
            if (!bubbleIds.empty() && std::find(bubbleIds.begin(), bubbleIds.end(), iid) != bubbleIds.end())
                break;
            bubbleIds.push_back(iid);
            if (!bubble)
                break;
            pBuild = pBuild->GetParent();
        }
    }

    {
        UIElement::SetInEventDispatch(true);
        for (size_t di = 0; di < bubbleIds.size() && di < static_cast<size_t>(kMaxBubbleDepth); ++di)
        {
            UIElement* p = FindElementByInstanceId(bubbleIds[di]);
            if (!p)
                continue;
            ev.CurrentTarget = p;
            p->DispatchEvent(ev);
            if (ev.Handled)
            {
                break;
            } // handled: terminate cleanly to avoid false depth-limit warning
        }
        if (bubble && bubbleIds.size() >= static_cast<size_t>(kMaxBubbleDepth) && !ev.Handled)
        {
            Logger::Log::Warning("UI: ancestor depth limit reached during event bubbling");
        }
    }
    UIElement::SetInEventDispatch(false);
    // Snapshot the requested-capture element's identity BEFORE running the deferred actions below.
    // Those actions (e.g. an inspector rebuild posted from a dropdown commit) can destroy that
    // element, so ev.captureRequested must not be dereferenced afterwards — we re-resolve it by
    // instanceId and only ever touch the validated pointer.
    const uint64_t captureReqInstanceId =
        ev.CaptureRequested ? ev.CaptureRequested->GetInstanceId() : 0;
    // Execute scheduled and deferred actions posted during dispatch after leaving event-dispatch.
    // Detect structural mutations via UIManager's tree-generation counter rather than guessing
    // based on "pending count" (many actions only mark dirty and do not change the tree).
    if (m_Scheduler)
    {
        m_Scheduler->ProcessDue();
    }
    if (m_Dispatcher)
    {
        m_Dispatcher->Drain();
    }
    if (!treeMutated) { if (m_TreeStructureGeneration.load(std::memory_order_relaxed) != treeGenBefore) treeMutated = true; }
    // Accept capture — but only if the requesting element survived the deferred actions above.
    // Prefer storing the pointer (re-resolved by id) to work across Mount portals.
    if (captureReqInstanceId != 0 && !m_MouseCaptured)
    {
        UIElement* cap = FindElementByInstanceId(captureReqInstanceId);
        if (cap)
        {
            m_MouseCaptured = true;
            m_CaptureElement = cap;
            m_CaptureInstanceId = cap->GetInstanceId();
            m_CaptureId = cap->GetId();
            if (m_CaptureId.empty())
            {
                // Capture must be restorable across rebuilds (splitter drag, virtualization).
                m_CaptureId = EnsureElementId(cap);
            }
        }
    }
    return ev.Handled;
}
