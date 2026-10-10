// UIElementABI.cpp — the ELEMENT layer of the game-UI scripting ABI: every export takes an
// element instance id and nothing else. No entity, no document, no ECS. The element surface
// belongs to the UI module, so the ABI over it must not learn that entities exist; resolving a
// document to an element is the document layer's one job (GameUIABI.cpp's GE_GameUI_FindElement).
// A GE_UIElement_* function that needed an entity id would be a layering error visible in its
// signature.
//
// GE_UI_GetTagId is the one export here that addresses no element either: it answers "what does
// this tag NAME hash to", which a binding must know before it can map an element's tag id to its
// own wrapper type. It lives on the UI module alone, hence the third prefix.
//
// The safety contract, unchanged in substance from the id-addressed forms these replace: the ABI
// never hands managed code a raw UIElement pointer and never holds one across calls. Every export
// re-resolves through the published gameplay host's UIManager, which is an O(1) instance-id probe
// plus a root-reachability filter — so a destroyed element, a detached one, one re-homed to
// another manager, and a torn-down host all report not-found and no-op rather than dereferencing
// anything. Instance ids come from a process-wide monotonic counter and are never reused, so a
// stale id fails closed forever instead of aliasing a later element.
//
// The reachability filter is anchored at the PUBLISHED host's root, and editor chrome runs on
// separate UIManagers with separate indices, so a script instance id can never resolve to editor
// chrome — structurally, not by a predicate.
//
// Mirrors ECSABI.cpp: extern "C", GE_API/GE_CDECL, every body try/catch -> GE_Result_Fail.
//
// Threading: UI element state has no internal locking; the host ticks it on the main thread.
// Every export here must be called from the user script's main-thread update, and event
// callbacks themselves fire during the host's main-thread event routing.

#include "Scripting/ScriptingABI.h" // GE_API, GE_CDECL, GE_Result

#include "Engine/GameUI/GameplayUI.h"
#include "Engine/GameUI/SessionEvent.h" // private: the session gate is not public C++ API

#include "UI/Controls/BaseField.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleOverrides.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include <set>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>

#include "Logger/Logger.h"

// GE_UIEventData/GE_UIEventCallback + the GE_UIElement_* prototypes are declared in
// ScriptingABI.h (the canonical ABI surface, alongside GE_Input_*/GE_ECS_*). The reverse
// callback hands the opaque GCHandle the script passed in back verbatim so the callee can
// recover its context.

namespace
{
// The whole of element resolution. Null when there is no published gameplay host, when the
// element was destroyed, or when it is owned but no longer reachable from the host's root.
GameEngine::UIElement* ResolveElement(uint64_t instanceId)
{
    GameEngine::UIManager* ui = GameEngine::GameUI::GetUIManager();
    return ui ? ui->FindElementByInstanceId(instanceId) : nullptr;
}

// The events a script may subscribe to, by name. The ABI hashes the name natively so there
// is exactly one source of truth for event ids and no binding-side enum to drift — and the
// list is an ALLOWLIST so a misspelled name is rejected instead of minting a subscription
// that can never fire (the same failure mode, and the same answer, as refusing a click
// subscription on a non-Button).
//
// It is deliberately the set the managed surface exposes today, not every id in UIEvents.h:
// add an entry when a binding grows the event that needs it, so the list stays a statement
// about what is reachable rather than a speculative mirror.
struct SubscribableEvent
{
    std::string_view Name;
    GameEngine::EventId Id;
};

constexpr SubscribableEvent kSubscribableEvents[] = {
    {"UI.ButtonClick", GameEngine::kEventButtonClick}, // Ui.Button.Clicked
    {"UI.MouseEnter", GameEngine::kEventMouseEnter},   // Ui.Element.PointerEntered
    {"UI.MouseMove", GameEngine::kEventMouseMove},     // Ui.Element.MouseMoved
    {"UI.FocusIn", GameEngine::kEventFocusIn},         // Ui.Element.FocusGained
    {"UI.ValueChanging", GameEngine::kEventValueChanging},
    {"UI.ValueChanged", GameEngine::kEventValueChanged},
    {"UI.ScrollOffsetChanged", GameEngine::kEventScrollOffsetChanged},
    // Open registration on purpose: attachment is a property of the tree, not a
    // behaviour of a control type, so every element both raises these and is a valid
    // target for them — there is no ElementDispatchesEvent row below and adding one
    // would be wrong rather than merely strict.
    //
    // UI.DetachedFromPanel fires for BOTH shapes of leaving: a keep-alive unlink
    // (TakeChild, tab deactivation, unmount), announced next frame, and a DESTRUCTION
    // (RemoveChild, ResetBoundDocuments, a reconcile drop, a manager dying), announced
    // synchronously before teardown. So it is a sound place for a script to release
    // external state — every element announced attached is eventually announced detached.
    {"UI.AttachedToPanel", GameEngine::kEventAttachedToPanel},
    {"UI.DetachedFromPanel", GameEngine::kEventDetachedFromPanel},
};

// Which element types actually DISPATCH an event, for the events where the answer is not
// "any of them". Subscribing anywhere else is a permanent silent no-op — Ok, a valid
// token, and a control that never responds — so it is refused, the same way
// GE_UIElement_SetLabelText refuses a non-Label. The rule generalises kEventButtonClick's
// original check; when a new control learns to dispatch one of these, this is the second
// place that has to learn it too.
//
// Note which events are NOT here: the pointer and focus events are dispatched by the
// manager's routing onto whatever element the pointer found, so every element is a valid
// subscription target and a guard would be wrong rather than merely strict.
bool ElementDispatchesEvent(GameEngine::EventId eventId, GameEngine::UIElement* el)
{
    // Button::TriggerClick is the only site in the engine that dispatches this. If a second
    // control ever does, widen this with it (there is a note at the id's declaration).
    if (eventId == GameEngine::kEventButtonClick)
        return dynamic_cast<GameEngine::Button*>(el) != nullptr;

    // Field<T>::NotifyValue dispatches only for the value types the payload can carry —
    // FieldEventValue's specialisations: float and bool through UIEvent::Value, std::string
    // through UIEvent::Text (TextField, Dropdown). A Field<int> or a Field<Vector3> notifies
    // its members and dispatches nothing, so a subscription there could never fire. Keep
    // this list and FieldEventValue's specialisations in step.
    if (eventId == GameEngine::kEventValueChanging || eventId == GameEngine::kEventValueChanged)
        return dynamic_cast<GameEngine::Field<float>*>(el) != nullptr ||
               dynamic_cast<GameEngine::Field<bool>*>(el) != nullptr ||
               dynamic_cast<GameEngine::Field<std::string>*>(el) != nullptr;

    if (eventId == GameEngine::kEventScrollOffsetChanged)
        return dynamic_cast<GameEngine::ScrollView*>(el) != nullptr;

    return true;
}

// Name the refusal, because GE_Result_InvalidArg on its own sends a script author looking for a
// typo in an element id that is perfectly correct. Deduplicated on (element type, event name):
// a caller that retries every frame gets one line, and a genuinely different mistake elsewhere
// still gets its own.
void ReportUndispatchedSubscription(const char* eventName, const GameEngine::UIElement& el)
{
    static std::set<std::pair<std::string, std::string>> reported;
    const std::string type = typeid(el).name();
    const std::string name = eventName ? eventName : "<null>";
    if (!reported.emplace(type, name).second)
        return;

    Logger::Log::Warning(
        "[UI ABI] Refused a '{}' subscription on a {}: that control does not dispatch it, so the "
        "subscription could never fire. Value events come from controls whose value type the event "
        "payload can carry - float, bool and string today, so a slider, a toggle, a text field or a "
        "dropdown; int and vector valued controls are not among them yet. Scroll-offset events come "
        "from a ScrollView only.",
        name, type);
}

// 0 when the name is unknown or null — never a hash of it, so a typo cannot become a
// subscription on an id nothing dispatches.
GameEngine::EventId ResolveEventId(const char* eventName)
{
    if (!eventName || *eventName == '\0')
        return 0;
    const std::string_view name{eventName};
    for (const SubscribableEvent& e : kSubscribableEvents)
        if (e.Name == name)
            return e.Id;
    return 0;
}

} // namespace

extern "C"
{

// Drive a bar/fill element's width as a percentage (0..100). GE_Result_NotFound if the handle no
// longer resolves. Routes through the element's style override bag, whose dirty callback schedules
// the relayout. Note that override-bag writes survive a .uxml reconcile (a different bag from the
// inline-style attribute), while authored `text` does not.
GE_API GE_Result GE_CDECL GE_UIElement_SetWidthPercent(uint64_t instanceId, float pct)
{
    try
    {
        GameEngine::UIElement* el = ResolveElement(instanceId);
        if (!el)
            return GE_Result_NotFound;
        el->Overrides().Set(GameEngine::Style::Width, GameEngine::StyleLength::Percent(pct));
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Set a Label element's text. GE_Result_NotFound if the handle no longer resolves;
// GE_Result_InvalidArg if the element is alive but isn't a Label (so the script learns it
// resolved the wrong id).
//
// A null text is "" — clearing a label is a real operation, so an empty string is a VALUE
// here. Contrast SetClass, which rejects an empty class name: there, empty names nothing.
// The asymmetry is deliberate and follows from what an empty string means to each.
GE_API GE_Result GE_CDECL GE_UIElement_SetLabelText(uint64_t instanceId, const char* text)
{
    try
    {
        GameEngine::UIElement* el = ResolveElement(instanceId);
        if (!el)
            return GE_Result_NotFound;
        auto* label = dynamic_cast<GameEngine::Label*>(el);
        if (!label)
            return GE_Result_InvalidArg;
        label->SetText(text ? text : "");
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Add (on != 0) or remove (on == 0) a CSS class. This is how a HUD expresses state — hidden,
// low-health, selected — while the LOOK stays in CSS where artists own it. There is no
// SetVisible on UIElement by design: visibility is the Display/Visibility style, so a script
// toggles a class and the stylesheet decides what that means. Adding a class that is already
// present, or removing one that is absent, is a no-op.
GE_API GE_Result GE_CDECL GE_UIElement_SetClass(uint64_t instanceId, const char* className, int32_t on)
{
    try
    {
        if (!className || *className == '\0')
            return GE_Result_InvalidArg;
        GameEngine::UIElement* el = ResolveElement(instanceId);
        if (!el)
            return GE_Result_NotFound;
        const std::string name{className};
        if (on != 0)
            el->AddClass(name);
        else
            el->RemoveClass(name);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Is this element still resolvable? A QUERY, so "no" is Ok with *outAlive == 0, not
// GE_Result_NotFound: every other export treats a dead handle as a failed operation because
// the operation really did not happen, but asking whether it is alive succeeds either way.
// A binding that reported liveness as an error could not distinguish "dead" from "the engine
// is not up".
GE_API GE_Result GE_CDECL GE_UIElement_IsAlive(uint64_t instanceId, int32_t* outAlive)
{
    try
    {
        if (!outAlive)
            return GE_Result_InvalidArg;
        *outAlive = 0;
        *outAlive = ResolveElement(instanceId) != nullptr ? 1 : 0;
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// The element's registered tag id — the identity a binding maps to its own wrapper type
// (Button -> Ui.Button, and so on). It is the tag's StringId, cached at registration and
// already what CSS type-selector matching uses, NOT the one-byte UIElementKind tag, which
// has three values and exists only to skip a dynamic_cast on two hot paths.
//
// *outTagId is 0 for a live element that carries no creation stamp AND whose exact C++ type
// was never registered with a tag (an editor-local subclass, an element built by hand). That
// is a valid answer, not an error: the binding falls back to the base wrapper.
GE_API GE_Result GE_CDECL GE_UIElement_GetTagId(uint64_t instanceId, uint64_t* outTagId)
{
    try
    {
        if (!outTagId)
            return GE_Result_InvalidArg;
        *outTagId = 0;
        GameEngine::UIElement* el = ResolveElement(instanceId);
        if (!el)
            return GE_Result_NotFound;
        *outTagId = GameEngine::UIRegistration::ElementFactoryRegistry::Instance().GetElementTagId(*el);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Tag NAME -> tag id. The binding needs this exactly once per wrapper type, to learn which
// id means "button"; asking the engine keeps the hash a single source of truth instead of a
// re-implementation that drifts the day the hash changes.
//
// *outTagId is 0 for an unknown tag, which a caller polls through: the built-in controls
// register during engine start-up, so an early call legitimately answers "not yet".
GE_API GE_Result GE_CDECL GE_UI_GetTagId(const char* tagLower, uint64_t* outTagId)
{
    try
    {
        if (!outTagId)
            return GE_Result_InvalidArg;
        *outTagId = 0;
        if (!tagLower || *tagLower == '\0')
            return GE_Result_InvalidArg;
        *outTagId = GameEngine::UIRegistration::ElementFactoryRegistry::Instance().GetTagId(tagLower);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Subscribe to one event on one element, by name. Subscribing goes through the handler
// table, which is additive and individually revocable: registering here can never destroy a
// built-in control's or another script's handler on the same element.
//
// GE_Result_InvalidArg for an unknown event name — a name nothing dispatches would be a
// permanent silent no-op, and the same reasoning refuses "UI.ButtonClick" on a non-Button,
// because Button::TriggerClick is the only dispatcher of it in the engine. Click means the
// control's own armed-and-inside notion: a release over a button whose press began elsewhere
// is not a click, and a press the pointer stream abandons arrives as kEventMouseCancel and
// never becomes one.
//
// *outToken returns the handler key. Keys are unique within ONE element instance and NOT
// across event ids, which is why unregistering needs the same instance id AND the same event
// name — and why the ABI addresses elements by instance id rather than by a string that a
// rebuild can re-point at a different element.
//
// The subscription is bound to the current gameplay session (the private gate), so it goes
// inert once play stops rather than running script code against the restored edit-mode
// world. That is a damage bound, NOT a lifetime guarantee. `cb` is a static reverse-callback
// stub in the scripting ABI assembly, which lives in the never-unloaded default load
// context, so the function pointer itself cannot dangle; `user` is the caller's opaque token
// — for the C# binding a GCHandle rooting the managed listener list, which may belong to a
// collectible scripts context. Revoking that before its context unloads is the caller's
// responsibility (GE_UIElement_UnregisterEvent); nothing native observes managed unloads, so
// this side cannot do it for them.
GE_API GE_Result GE_CDECL GE_UIElement_RegisterEvent(uint64_t instanceId, const char* eventName,
                                                     GE_UIEventCallback cb, void* user, uint64_t* outToken)
{
    try
    {
        if (outToken)
            *outToken = 0;
        if (!cb)
            return GE_Result_InvalidArg;
        const GameEngine::EventId eventId = ResolveEventId(eventName);
        if (eventId == 0)
            return GE_Result_InvalidArg;
        GameEngine::UIElement* el = ResolveElement(instanceId);
        if (!el)
            return GE_Result_NotFound;
        // A subscription that could never fire is a mistake worth reporting now rather than
        // leaving as a control that silently never responds. InvalidArg alone is mystifying
        // from a script's seat — "I passed a real element and a real event name" — so say which
        // of the two is wrong and why, once per (element type, event) so a polling caller
        // cannot flood the log.
        if (!ElementDispatchesEvent(eventId, el))
        {
            ReportUndispatchedSubscription(eventName, *el);
            return GE_Result_InvalidArg;
        }
        const uint64_t token = GameEngine::GameUI::Detail::RegisterSessionEvent(
            instanceId, eventId,
            [cb, user](const GameEngine::UIEvent& e)
            {
                GE_UIEventData data{};
                data.elementInstanceId = e.CurrentTarget ? e.CurrentTarget->GetInstanceId() : 0;
                data.eventId = e.Id;
                data.x = e.X;
                data.y = e.Y;
                data.button = e.Button;
                data.mods = e.Mods;
                data.value = e.Value;
                data.scrollX = e.ScrollX;
                data.scrollY = e.ScrollY;
                // Points at the dispatch-owned copy (ScopedValueText); the callback runs
                // synchronously inside that window, so the pointer is valid for the call and
                // no longer — exactly what the header documents for these two fields.
                data.text = e.Text.data();
                data.textLen = static_cast<uint32_t>(e.Text.size());
                cb(&data, user);
            });
        if (token == 0)
            return GE_Result_NotFound;
        if (outToken)
            *outToken = token;
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Remove a subscription by its (event name, token) pair. Re-resolves the instance id first:
// if the element was already destroyed (play-stop, hot-reload replacement, doc-disable,
// teardown) this is a safe no-op — never a dangling dereference, and never a token applied
// to a stranger, because the id that no longer resolves cannot be reassigned to another
// element.
GE_API GE_Result GE_CDECL GE_UIElement_UnregisterEvent(uint64_t instanceId, const char* eventName, uint64_t token)
{
    try
    {
        const GameEngine::EventId eventId = ResolveEventId(eventName);
        if (eventId == 0)
            return GE_Result_InvalidArg;
        GameEngine::GameUI::Detail::UnregisterSessionEvent(instanceId, eventId, token);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

} // extern "C"
