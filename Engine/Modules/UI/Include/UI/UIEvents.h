#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include "Types/StringId.h"
#include "UI/UIStyle.h"

namespace GameEngine
{

class UIElement; // fwd

namespace UI
{
class IPlatformApi;
} // namespace UI

using EventId = StringId;

// Simple constexpr FNV-1a 64-bit hash for extensible event ids
constexpr EventId HashEvent(std::string_view sv)
{
    return HashStringId(sv);
}

// Common event ids (extensible via HashEvent("...")).
//
// Pointer events:
//  - UI.MouseMove / UI.MouseDown / UI.MouseUp bubble through the ancestor
//    chain (see UIManager_Update.cpp event routing).
//  - UI.MouseEnter / UI.MouseLeave are *non-bubbling* hover-transition
//    events, analogous to DOM's mouseenter/mouseleave: each element on the
//    hover chain receives at most one enter/leave per transition, and
//    ancestors receive their own events via the precomputed hover sets.
inline constexpr EventId kEventMouseDown = HashEvent("UI.MouseDown");
inline constexpr EventId kEventMouseUp = HashEvent("UI.MouseUp");
// The in-flight press ended without a release, because the pointer stream itself ended
// (UIManager::CancelPress — the Game View viewport the host reads pointer from was left).
// Bubbles like MouseUp and carries the unknown-position sentinel in X/Y, because no
// release position exists. A control that arms on MouseDown must disarm here and must
// NOT activate. Deliberately NOT delivered as a MouseUp: subscribers reasonably read a
// MouseUp as "the user released this", and many never look at the position.
inline constexpr EventId kEventMouseCancel = HashEvent("UI.MouseCancel");
inline constexpr EventId kEventMouseMove = HashEvent("UI.MouseMove");
inline constexpr EventId kEventMouseEnter = HashEvent("UI.MouseEnter");
inline constexpr EventId kEventMouseLeave = HashEvent("UI.MouseLeave");
inline constexpr EventId kEventScroll = HashEvent("UI.Scroll");
inline constexpr EventId kEventKeyDown = HashEvent("UI.KeyDown");
inline constexpr EventId kEventKeyUp = HashEvent("UI.KeyUp");
// Text-specific events used by controls like TextInput/TextFieldBase
inline constexpr EventId kEventTextInput = HashEvent("UI.TextInput");
inline constexpr EventId kEventFocusIn = HashEvent("UI.FocusIn");
inline constexpr EventId kEventFocusOut = HashEvent("UI.FocusOut");
// Fired when a CSS transition reaches its target value.
inline constexpr EventId kEventTransitionEnd = HashEvent("UI.TransitionEnd");
// BUTTON's click, and the name says so deliberately: this is not a generic
// "something was clicked" event that any element might raise. Button::TriggerClick
// is the only dispatcher in the engine — armed-and-inside at mouse-up (the press
// began on this button and the pointer is still over it), or Space/Enter while
// focused. A plain element never fires it, whatever is done to it with a pointer.
//
// The arming is the whole point: a release over a button whose press began
// elsewhere is NOT a click. Non-bubbling; the control dispatches it on itself only.
//
// Carries Mods; X/Y are left ZERO, not a click point — keyboard activation has no
// position, so there is no honest value to put there. Read the pointer position
// from the mouse events if you need it.
//
// The scripting ABI depends on the single-dispatcher property:
// GE_UIElement_RegisterEvent rejects this event on a non-Button element, because a
// subscription there could never fire. A second control that dispatches this must
// widen that check (Engine/Source/Scripting/UIElementABI.cpp) — and should make this
// name honest again, since by then it would no longer be Button's alone.
inline constexpr EventId kEventButtonClick = HashEvent("UI.ButtonClick");

// A Field<T> control's value changed. Both fire from the control's own
// NotifyValueChanging/NotifyValueChanged — the same pair the m_On* member callbacks
// fire from — so a subscription sees exactly what a built-in callback sees, in the
// same order (members first, then the table), and SetValueWithoutNotify raises
// neither. Changing precedes Changed; a control that only commits raises Changed.
//
// The new value rides in UIEvent::Value for numeric types (a float as itself, a bool
// as 0 or 1) and in UIEvent::Text for string-valued fields (UTF-8, dispatch-stable —
// see the Text member below). A Field<T> whose T has neither payload — int, Vector3 —
// notifies its members and dispatches NOTHING rather than firing a value event with no
// value in it, and the scripting ABI refuses a subscription there for the same reason
// it refuses a click on a non-Button: a subscription that could never fire is a bug
// reported late. Which types dispatch, and through which payload, is FieldEventValue's
// contract.
//
// Non-bubbling; the control dispatches on itself, like kEventButtonClick.
inline constexpr EventId kEventValueChanging = HashEvent("UI.ValueChanging");
inline constexpr EventId kEventValueChanged = HashEvent("UI.ValueChanged");

// A ScrollView's scroll OFFSET reached a new value — not the wheel input, which is
// kEventScroll and carries a delta. The new offsets are in ScrollX/ScrollY.
//
// Dispatched from the coalesced flush UIManager already runs at its safe point
// (UIManager::FlushScrollCallbacks), so several offset writes within one frame
// produce ONE event carrying the settled offsets, and a handler runs outside the
// scroll machinery rather than in the middle of it. Non-bubbling.
inline constexpr EventId kEventScrollOffsetChanged = HashEvent("UI.ScrollOffsetChanged");

// The element joined, or left, a tree that reaches its manager's root — UI Toolkit's
// AttachToPanelEvent / DetachFromPanelEvent, and the analogy is exact enough to keep
// the names. "Attached" is owner-registered AND root-reachable: the same two conjuncts
// UIManager::FindElementByInstanceId resolves through, so an attached element is
// precisely one the scripting ABI can find and IsAlive reports true for. An
// owned-but-unreachable subtree — an inactive dock tab — is detached on purpose, so tab
// activation raises attach and deactivation raises detach.
//
// Universal: any element may raise these, and any element is a valid subscription
// target. There is no dispatching-control gate the way kEventButtonClick and the value
// events have one, because attachment is a property of the tree rather than a behaviour
// of a control type.
//
// EDGE-TRIGGERED AND SETTLED ONCE PER FRAME, never dispatched at the mutation site. Each
// element remembers the last attach state its subscribers were told, and the settle
// (UIManager_AttachEvents.cpp) dispatches only where the current state differs. That is
// what makes a detach-and-reattach WITHIN one frame — which is exactly what
// UIHotReload::ReconcileChildren does to every preserved child on a .uxml save — dispatch
// nothing at all, rather than a spurious detach/attach pair. The price is one frame of
// latency: a tree edit made during frame N is announced at the top of frame N+1.
//
// TWO TIMINGS, ONE CONTRACT: every element that was announced ATTACHED is eventually
// announced DETACHED, including when it is destroyed. That is what makes these safe to
// unsubscribe from external state in, and it is why the UI Toolkit names are honest here.
//
//   * An UNLINK that leaves the element alive — TakeChild, a dock tab deactivating, a
//     Mount unmounting — is queued and settled, as above. Announced at the top of the next
//     frame, and elided entirely if the element is back in place by then.
//
//   * A DESTRUCTION is announced SYNCHRONOUSLY, at the point destruction is decided and
//     before anything is torn down: RemoveChild / RemoveAllChildren (and so
//     GameUIHost::ResetBoundDocuments), a reconcile dropping children, SetRoot replacing a
//     tree, and ~UIManager. Post-order, and complete by the time that call returns.
//
// The two cannot conflict. A queued transition may be elided because the element might be
// back before the settle runs; a destruction can never be elided, and the element will not
// exist next frame, so it is the one transition that has to be immediate. The edge rule
// governs both: an element whose subscribers were never told "attached" is never told
// "detached", so a subtree built and destroyed inside one frame is silent in both
// directions.
//
// WHY THE SYNCHRONOUS DISPATCH IS SAFE, as a property to preserve rather than a claim: it
// runs where destruction is DECIDED, not where memory is released. Every element in the
// doomed subtree is still whole — derived parts, handler tables, owner registration — so
// it is an ordinary dispatch on a live tree that happens to be doomed. Dispatching from
// ~UIElement would instead run handlers against half-destroyed objects, which is the
// use-after-free class this design exists to avoid; it is deliberately not done there.
// While that dispatch is in flight the subtree is CONDEMNED: adopting into it, and
// rescuing an element out of it, are both refused and logged (UIElement::AddChild /
// InsertChild / TakeChild), because either would dangle the moment the dispatch returns.
//
// THE ONE RESIDUE, AND THE API RULE THAT COVERS IT: an element owned OUTSIDE the tree — a
// Mount target, a panel held in application storage, a test's unique_ptr — that its owner
// destroys DIRECTLY while it is still attached. There is no engine-side decision point on
// that path, because the decision is made by the owning code.
//
//   RULE: unmount or remove an externally owned element before destroying it.
//         mount->SetTarget(nullptr), or parent->RemoveChild(el), and then release it.
//
// Both of those ARE decision points, so following the rule closes the gap. A build with
// asserts live detects a violation for you: ~UIElement warns once when an element is
// destroyed while its subscribers still believe it is attached. Note the narrowness — a
// manager DYING with such an element still mounted is NOT this case and is handled
// (UIManager::AnnounceDetachForSurvivingElements): the element survives, and it is told.
//
// Non-bubbling: the settle walks the changed subtree and dispatches on each element in
// it, so an ancestor hears about itself, not about its descendants.
inline constexpr EventId kEventAttachedToPanel = HashEvent("UI.AttachedToPanel");
inline constexpr EventId kEventDetachedFromPanel = HashEvent("UI.DetachedFromPanel");

// A .uxml layout bound to this element was just reconciled into its subtree
// (UIHotReload::ApplyLayoutReload): a hot reload of the file, or another element binding
// the same layout. Dispatched on the BOUND TARGET only, after the reconcile is complete,
// once per reconcile. Non-bubbling, so a panel hears about its own layout and nobody else's.
//
// The reconcile keeps an element (same object, same handler table) only where the new
// template still declares its id with the same tag. An element whose id was removed or
// renamed, or whose tag changed, is DESTROYED, and new template elements are created
// unwired. So a subscriber holding elements resolved by id re-resolves them here, holds
// them as UIElement::WeakRef so a destroyed one reads null, and wires handlers once per
// element INSTANCE: re-registering on a preserved element would make it fire twice.
inline constexpr EventId kEventLayoutReconciled = HashEvent("UI.LayoutReconciled");

// ---- Handler-presence bits -------------------------------------------------
//
// UIElement carries one bit per named id recording "this element holds at least
// one handler for that id", so a dispatch nobody subscribed to is a bit test
// rather than a hash lookup. EventId is a 64-bit FNV-1a hash, so ids are neither
// dense nor closed — any code may mint one with HashEvent("...") — which is why
// the mapping is this explicit table and every id outside it shares one escape
// bit that falls through to the lookup.
//
// THE BITS ARE CONSERVATIVE BY CONSTRUCTION, and that asymmetry is the entire
// safety argument: a bit left set costs exactly the lookup that would have been
// paid anyway, while a bit wrongly cleared silently drops events. So a bit is set
// on registration and cleared only where the id's handler deque is provably empty
// (UIElement::DrainInactiveHandlers, which runs when the last executing frame on
// the element unwinds). The escape bit is set-only.
//
// Adding an id here is what earns it the cheap path; leaving one out is safe.
inline constexpr EventId kNamedEventIds[] = {
    kEventMouseDown, kEventMouseUp,   kEventMouseCancel, kEventMouseMove,
    kEventMouseEnter, kEventMouseLeave, kEventScroll,    kEventKeyDown,
    kEventKeyUp,     kEventTextInput, kEventFocusIn,     kEventFocusOut,
    kEventTransitionEnd, kEventButtonClick, kEventValueChanging, kEventValueChanged,
    kEventScrollOffsetChanged, kEventAttachedToPanel, kEventDetachedFromPanel, kEventLayoutReconciled,
};
inline constexpr std::size_t kNamedEventIdCount = sizeof(kNamedEventIds) / sizeof(kNamedEventIds[0]);

// "This element holds a handler for some id outside the table above."
inline constexpr std::uint32_t kEventEscapeHandlerBit = 1u << 31;

static_assert(kNamedEventIdCount < 31, "One bit per named id plus the escape bit must fit in a uint32_t");

// The bit for `id`, or the escape bit when the id is not a named one. Folds to a
// constant wherever the id is a constant, which is the point: a control
// dispatching its own event tests one bit against a literal.
constexpr std::uint32_t EventHandlerBit(EventId id)
{
    for (std::size_t i = 0; i < kNamedEventIdCount; ++i)
        if (kNamedEventIds[i] == id)
            return 1u << i;
    return kEventEscapeHandlerBit;
}

// Two ids colliding (a duplicate entry, or two names hashing the same) would give
// them one shared bit, so clearing either would silently drop the other's events.
constexpr bool NamedEventIdsAreDistinct()
{
    for (std::size_t i = 0; i < kNamedEventIdCount; ++i)
        for (std::size_t j = i + 1; j < kNamedEventIdCount; ++j)
            if (kNamedEventIds[i] == kNamedEventIds[j])
                return false;
    return true;
}
static_assert(NamedEventIdsAreDistinct(), "Named event ids must be distinct — each owns one handler-presence bit");

struct UIEvent
{
    EventId Id{0};
    float X{0.0f};
    float Y{0.0f};
    int Button{0};
    bool ButtonDown{false};
    int Key{0};
    int Mods{0};
    unsigned int Codepoint{0};           // for UI.TextInput events
    UI::IPlatformApi* Platform{nullptr}; // optional platform for text editing shortcuts
    float ScrollX{0.0f};                 // UI.Scroll: wheel delta. UI.ScrollOffsetChanged: the new offset.
    float ScrollY{0.0f};
    // The new value, for UI.ValueChanging / UI.ValueChanged. A bool value arrives as
    // 0 or 1; a value type with no encoding never reaches here, because its control
    // does not dispatch (see kEventValueChanged).
    float Value{0.0f};
    // The new value as UTF-8 text, for UI.ValueChanging / UI.ValueChanged on string-valued
    // fields (a TextField's text; a Dropdown's selected option VALUE). Empty for every other
    // event, including numeric value events. POINTS AT A DISPATCH-OWNED COPY that is valid
    // for exactly this dispatch (ScopedValueText): handlers may compare it and copy it, never
    // store it. A handler writing the field mid-dispatch changes the field, not this payload.
    std::string_view Text{};

    StylePropertyId TransitionProperty{StylePropertyId::Unknown}; // for UI.TransitionEnd

    UIElement* Target{nullptr};        // original hit element (or capture owner)
    UIElement* CurrentTarget{nullptr}; // element whose handlers are currently invoked

    // Set by the handler that acted on this event, and the only answer the
    // dispatcher routes on: a handled key stops propagating and never reaches
    // the application's registered shortcuts, the way a defaultPrevented DOM
    // event never triggers a browser accelerator. A handler that does not act
    // must leave this alone so the chord bubbles.
    bool Handled{false};
    UIElement* CaptureRequested{nullptr};

    void Stop() { Handled = true; }
    void Capture(UIElement* el) { CaptureRequested = el; }
};

} // namespace GameEngine
