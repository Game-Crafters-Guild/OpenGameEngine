#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// A TEST DOUBLE for the game-UI element ABI, not a UI implementation.
//
// The managed binding (GameEngine.Scripting.Ui) has bookkeeping of its own worth testing
// directly: one wrapper per live element, one native subscription however many C# listeners a
// multicast event has, per-listener exception isolation, revocation at load-context unload. None
// of that can be exercised against the real engine from a `dotnet test` host, which has no
// window, no device and no mounted UIDocument — so the managed suite would otherwise be limited
// to asserting that things throw.
//
// So this file stands in for the element side of the ABI with an in-memory element table and a
// list of subscriptions, plus a handful of GE_TestUI_* exports the tests drive it with. It
// deliberately mirrors the REAL exports' argument validation and result codes (empty id rejected
// before lookup, unknown event name rejected, dead element -> GE_Result_NotFound, liveness as an
// Ok-with-a-flag query, SetLabelText refused on a non-Label, UI.ButtonClick refused on a
// non-Button), because a double that answered differently would make a green managed test mean
// nothing — and because a rule the double does not enforce is a rule no managed test can ever
// observe.
//
// The two lists that must agree with the engine are the event allowlist and the type rules
// below. Both are hand-mirrored from Engine/Source/Scripting/UIElementABI.cpp, so nothing here
// can catch a divergence on its own; AbiNativeSmokeTest walks the same four event names against
// the REAL export and asserts each is known, which is the reconciliation this file cannot do.
//
// What it cannot prove is anything about the engine: whether a Button dispatches a click, whether
// a cancelled press becomes one, whether a detached element still resolves. Those live in UITests
// (GameUIElementHandleTests, GameUIHostTests) against the real tree, and the C surface's own
// contract is pinned in ScriptingAbiIntegrationTests (AbiNativeSmokeTest). This file is the third
// leg: the binding's logic, with the engine replaced by something a test can drive.

#define GE_SCRIPTING_BUILD 1
#include "Scripting/ScriptingABI.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

struct ShimElement
{
    std::string ElementId;
    std::uint64_t TagId = 0;
    bool Alive = false;
    std::uint64_t NextToken = 0; // per element, exactly like UIElement::m_NextHandlerKey

    // Value storage behind the typed accessor doubles. One element carries them all — the
    // TYPE RULES decide which are reachable for its tag, exactly as the real exports'
    // dynamic_casts decide by Field<T>.
    float FloatValue = 0.0f;
    bool BoolValue = false;
    std::string TextValue;
    float ScrollX = 0.0f;
    float ScrollY = 0.0f;
    int SelectedIndex = -1;
    // Seeded by GE_TestUI_SetDropdownData: the double has no option list, only the pair the
    // current selection resolves to.
    std::string DropdownValue;
    std::string DropdownLabel;
};

struct ShimSubscription
{
    std::uint64_t InstanceId = 0;
    std::string EventName;
    GE_UIEventCallback Cb = nullptr;
    void* User = nullptr;
    std::uint64_t Token = 0;
};

std::unordered_map<std::uint64_t, ShimElement> g_Elements;
std::vector<ShimSubscription> g_Subscriptions;
std::uint64_t g_NextInstanceId = 1;
// How many times the binding asked an element for its tag id. The binding's wrapper cache is
// supposed to make that once per element, and nothing else can observe whether it does.
std::uint64_t g_GetTagIdCalls = 0;

// The engine derives tag ids with FNV-1a 64 over the canonical lowercased tag. The exact
// function does not matter to the binding — only that name->id and element->id agree, which is
// the property the binding's type mapping rests on — but matching it keeps the double honest.
std::uint64_t Fnv1a64(const char* s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (; s && *s; ++s)
    {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(*s));
        h *= 1099511628211ull;
    }
    return h;
}

// The same allowlist the real export carries, for the same reason: a name nothing dispatches
// must be rejected rather than becoming a subscription that can never fire.
bool IsKnownEvent(const char* name)
{
    if (!name)
        return false;
    static const char* const kNames[] = {"UI.ButtonClick",   "UI.MouseEnter",    "UI.MouseMove",
                                         "UI.FocusIn",       "UI.ValueChanging", "UI.ValueChanged",
                                         "UI.ScrollOffsetChanged", "UI.AttachedToPanel",
                                         "UI.DetachedFromPanel"};
    for (const char* known : kNames)
        if (std::strcmp(known, name) == 0)
            return true;
    return false;
}

// The two type rules the real element ABI enforces. The double stores a tag id per element, so
// it can answer them the same way the engine's dynamic_cast does.
bool IsTag(const ShimElement& el, const char* tagLower);

ShimElement* Resolve(std::uint64_t instanceId)
{
    auto it = g_Elements.find(instanceId);
    if (it == g_Elements.end() || !it->second.Alive)
        return nullptr;
    return &it->second;
}

bool IsTag(const ShimElement& el, const char* tagLower)
{
    return el.TagId == Fnv1a64(tagLower);
}

// The double's stand-ins for the real exports' dynamic_casts: one predicate per Field<T>
// instantiation, listing the built-in tags that ARE one. Every value guard below asks these
// rather than spelling a tag list out again, so the double stays exactly as permissive as the
// engine — a double that refuses what the engine accepts sends a binding author hunting a bug
// that does not exist. `intfield` is deliberately in none of them: Field<int> has no payload
// encoding. The arm that keeps these lists honest against the engine is
// GameUIElementHandleTests, which drives the real exports.
bool IsFloatField(const ShimElement& el) // Field<float>
{
    return IsTag(el, "slider") || IsTag(el, "floatfield");
}

bool IsBoolField(const ShimElement& el) // Field<bool>
{
    return IsTag(el, "toggle") || IsTag(el, "checkbox");
}

bool IsTextField(const ShimElement& el) // the writable Field<std::string> controls
{
    return IsTag(el, "textfield") || IsTag(el, "input") || IsTag(el, "textinput") ||
           IsTag(el, "textarea");
}

// A Dropdown is a Field<std::string> too — its field value is the selected option's VALUE —
// but a read-only one: the value is derived from the option list, so the raw text setter
// refuses it.
bool IsStringField(const ShimElement& el)
{
    return IsTextField(el) || IsTag(el, "dropdown");
}

// Deliver one event to every subscription on (instanceId, eventName), returning how many ran.
// Snapshot first: a listener may unsubscribe from inside the callback, which mutates the list.
int DispatchToSubscribers(std::uint64_t instanceId, const char* eventName, const GE_UIEventData& data)
{
    std::vector<ShimSubscription> snapshot;
    for (const ShimSubscription& sub : g_Subscriptions)
        if (sub.InstanceId == instanceId && sub.EventName == eventName)
            snapshot.push_back(sub);
    for (const ShimSubscription& sub : snapshot)
        sub.Cb(&data, sub.User);
    return static_cast<int>(snapshot.size());
}

// What a notifying control double announces: ValueChanging then ValueChanged, one payload —
// the order Field<T>'s notify pair produces. `text` non-null rides the pointer-and-length
// fields, exactly as the real dispatch points them at its dispatch-owned copy.
void AnnounceValue(std::uint64_t instanceId, float value, const std::string* text)
{
    GE_UIEventData data{};
    data.elementInstanceId = instanceId;
    data.value = value;
    if (text)
    {
        data.text = text->data();
        data.textLen = static_cast<uint32_t>(text->size());
    }
    data.eventId = Fnv1a64("UI.ValueChanging");
    DispatchToSubscribers(instanceId, "UI.ValueChanging", data);
    data.eventId = Fnv1a64("UI.ValueChanged");
    DispatchToSubscribers(instanceId, "UI.ValueChanged", data);
}

// The one string-out contract the real exports use: copy what fits, always report the full
// length, no NUL terminator.
GE_Result CopyOut(const std::string& value, char* buffer, int32_t bufferLen, int32_t* outLen)
{
    if (!outLen || bufferLen < 0 || (!buffer && bufferLen != 0))
        return GE_Result_InvalidArg;
    *outLen = static_cast<int32_t>(value.size());
    const int32_t copy = *outLen < bufferLen ? *outLen : bufferLen;
    if (copy > 0)
        std::memcpy(buffer, value.data(), static_cast<size_t>(copy));
    return GE_Result_Ok;
}

} // namespace

// File-scope so GE_TestUI_RegisterNoopEvent can take its address; it is never invoked, because
// the subscriptions it creates exist only to be counted.
static void GE_CDECL ShimNoopEventCallback(const GE_UIEventData*, void*) {}

extern "C"
{

GE_API GE_Result GE_CDECL GE_GameUI_FindElement(uint64_t /*entityId*/, const char* id, uint64_t* outInstanceId)
{
    if (!outInstanceId)
        return GE_Result_InvalidArg;
    *outInstanceId = 0;
    if (!id || *id == '\0')
        return GE_Result_InvalidArg;
    for (const auto& kv : g_Elements)
    {
        if (kv.second.Alive && kv.second.ElementId == id)
        {
            *outInstanceId = kv.first;
            return GE_Result_Ok;
        }
    }
    return GE_Result_NotFound;
}

GE_API GE_Result GE_CDECL GE_UIElement_SetWidthPercent(uint64_t instanceId, float /*pct*/)
{
    return Resolve(instanceId) ? GE_Result_Ok : GE_Result_NotFound;
}

GE_API GE_Result GE_CDECL GE_UIElement_SetLabelText(uint64_t instanceId, const char* /*text*/)
{
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    // Alive but not a Label is a caller error, not a miss — the real export says so with
    // dynamic_cast, and a script learns it resolved the wrong id.
    return IsTag(*el, "label") ? GE_Result_Ok : GE_Result_InvalidArg;
}

GE_API GE_Result GE_CDECL GE_UIElement_SetClass(uint64_t instanceId, const char* className, int32_t /*on*/)
{
    if (!className || *className == '\0')
        return GE_Result_InvalidArg;
    return Resolve(instanceId) ? GE_Result_Ok : GE_Result_NotFound;
}

GE_API GE_Result GE_CDECL GE_UIElement_IsAlive(uint64_t instanceId, int32_t* outAlive)
{
    if (!outAlive)
        return GE_Result_InvalidArg;
    *outAlive = Resolve(instanceId) ? 1 : 0;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_GetTagId(uint64_t instanceId, uint64_t* outTagId)
{
    if (!outTagId)
        return GE_Result_InvalidArg;
    *outTagId = 0;
    ++g_GetTagIdCalls;
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    *outTagId = el->TagId;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UI_GetTagId(const char* tagLower, uint64_t* outTagId)
{
    if (!outTagId)
        return GE_Result_InvalidArg;
    *outTagId = 0;
    if (!tagLower || *tagLower == '\0')
        return GE_Result_InvalidArg;
    *outTagId = Fnv1a64(tagLower);
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_RegisterEvent(uint64_t instanceId, const char* eventName,
                                                     GE_UIEventCallback cb, void* user, uint64_t* outToken)
{
    if (outToken)
        *outToken = 0;
    if (!cb || !IsKnownEvent(eventName))
        return GE_Result_InvalidArg;
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    // Only Button dispatches UI.ButtonClick, so a subscription anywhere else could never
    // fire and the real export refuses it. Without this the Button-only rule would be
    // unobservable from the managed suite, while this file claims to mirror it.
    if (std::strcmp(eventName, "UI.ButtonClick") == 0 && !IsTag(*el, "button"))
        return GE_Result_InvalidArg;
    // Same reason, for the events only certain controls dispatch: a value event needs a
    // control whose value type the payload can carry, a scroll-offset event needs a
    // ScrollView. The real export decides the first by dynamic_cast to Field<float>,
    // Field<bool> or Field<std::string>; these three predicates are those three casts, and
    // which tags they cover is documented where they are defined.
    if ((std::strcmp(eventName, "UI.ValueChanged") == 0 || std::strcmp(eventName, "UI.ValueChanging") == 0) &&
        !IsFloatField(*el) && !IsBoolField(*el) && !IsStringField(*el))
        return GE_Result_InvalidArg;
    if (std::strcmp(eventName, "UI.ScrollOffsetChanged") == 0 && !IsTag(*el, "scrollview"))
        return GE_Result_InvalidArg;

    ShimSubscription sub;
    sub.InstanceId = instanceId;
    sub.EventName = eventName;
    sub.Cb = cb;
    sub.User = user;
    sub.Token = ++el->NextToken;
    g_Subscriptions.push_back(sub);
    if (outToken)
        *outToken = sub.Token;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_UnregisterEvent(uint64_t instanceId, const char* eventName, uint64_t token)
{
    if (!IsKnownEvent(eventName))
        return GE_Result_InvalidArg;
    for (auto it = g_Subscriptions.begin(); it != g_Subscriptions.end(); ++it)
    {
        if (it->InstanceId == instanceId && it->Token == token && it->EventName == eventName)
        {
            g_Subscriptions.erase(it);
            break;
        }
    }
    // A dead element is a safe no-op, never a failure — the real export says the same.
    return GE_Result_Ok;
}

// ---- Typed value accessor doubles ----------------------------------------------------------
//
// Mirrors of Engine/Source/Scripting/UIElementValueABI.cpp, with tags standing in for the
// dynamic_casts and stored values standing in for the controls. The NOTIFY semantics mirror
// the real controls': a float or bool setter with notify announces ValueChanging+ValueChanged
// after an actual change (the controls early-out on an unchanged value); a TEXT setter is a
// silent programmatic sync either way, because TextFieldBase::SetValue is (pinned engine-side
// in GameUIElementHandleTests); a dropdown index setter with notify announces the seeded
// option VALUE string. Scroll setters store and announce nothing here — the real event rides
// the manager's coalesced flush, which the double does not simulate; managed tests raise
// UI.ScrollOffsetChanged through the dispatch drivers instead.

GE_API GE_Result GE_CDECL GE_UIElement_GetValueFloat(uint64_t instanceId, float* outValue)
{
    if (!outValue)
        return GE_Result_InvalidArg;
    *outValue = 0.0f;
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsFloatField(*el))
        return GE_Result_InvalidArg;
    *outValue = el->FloatValue;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_SetValueFloat(uint64_t instanceId, float value, int32_t notify)
{
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsFloatField(*el))
        return GE_Result_InvalidArg;
    if (el->FloatValue == value)
        return GE_Result_Ok; // the controls' unchanged-write early-out, mirrored
    el->FloatValue = value;
    if (notify != 0)
        AnnounceValue(instanceId, value, nullptr);
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_GetValueBool(uint64_t instanceId, int32_t* outValue)
{
    if (!outValue)
        return GE_Result_InvalidArg;
    *outValue = 0;
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsBoolField(*el))
        return GE_Result_InvalidArg;
    *outValue = el->BoolValue ? 1 : 0;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_SetValueBool(uint64_t instanceId, int32_t value, int32_t notify)
{
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsBoolField(*el))
        return GE_Result_InvalidArg;
    const bool next = value != 0;
    if (el->BoolValue == next)
        return GE_Result_Ok;
    el->BoolValue = next;
    if (notify != 0)
        AnnounceValue(instanceId, next ? 1.0f : 0.0f, nullptr);
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_GetValueText(uint64_t instanceId, char* buffer, int32_t bufferLen,
                                                    int32_t* outLen)
{
    if (outLen)
        *outLen = 0;
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (IsTag(*el, "dropdown"))
        return CopyOut(el->DropdownValue, buffer, bufferLen, outLen);
    if (!IsTextField(*el))
        return GE_Result_InvalidArg;
    return CopyOut(el->TextValue, buffer, bufferLen, outLen);
}

GE_API GE_Result GE_CDECL GE_UIElement_SetValueText(uint64_t instanceId, const char* text, int32_t notify)
{
    (void)notify; // TextFieldBase::SetValue is a silent programmatic sync with either flag.
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    // IsTextField, not IsStringField: a Dropdown reads as a Field<std::string> but its value
    // is derived from its options, so the real export refuses the raw write rather than let
    // the selected index, the header label and the item classes desync.
    if (!IsTextField(*el))
        return GE_Result_InvalidArg;
    el->TextValue = text ? text : "";
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_GetScrollOffset(uint64_t instanceId, float* outX, float* outY)
{
    if (!outX || !outY)
        return GE_Result_InvalidArg;
    *outX = 0.0f;
    *outY = 0.0f;
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsTag(*el, "scrollview"))
        return GE_Result_InvalidArg;
    *outX = el->ScrollX;
    *outY = el->ScrollY;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_SetScrollX(uint64_t instanceId, float x)
{
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsTag(*el, "scrollview"))
        return GE_Result_InvalidArg;
    el->ScrollX = x;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_SetScrollY(uint64_t instanceId, float y)
{
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsTag(*el, "scrollview"))
        return GE_Result_InvalidArg;
    el->ScrollY = y;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_GetDropdownSelectedIndex(uint64_t instanceId, int32_t* outIndex)
{
    if (!outIndex)
        return GE_Result_InvalidArg;
    *outIndex = -1;
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsTag(*el, "dropdown"))
        return GE_Result_InvalidArg;
    *outIndex = el->SelectedIndex;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_SetDropdownSelectedIndex(uint64_t instanceId, int32_t index,
                                                                int32_t notify)
{
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsTag(*el, "dropdown"))
        return GE_Result_InvalidArg;
    if (el->SelectedIndex == index)
        return GE_Result_Ok; // Dropdown::SetSelectedIndex early-returns on the same index
    el->SelectedIndex = index;
    if (notify != 0)
        AnnounceValue(instanceId, 0.0f, &el->DropdownValue);
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UIElement_GetDropdownSelectedLabel(uint64_t instanceId, char* buffer,
                                                                int32_t bufferLen, int32_t* outLen)
{
    if (outLen)
        *outLen = 0;
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    if (!IsTag(*el, "dropdown"))
        return GE_Result_InvalidArg;
    return CopyOut(el->DropdownLabel, buffer, bufferLen, outLen);
}

// ---- Test-only drivers. Not part of the ABI; they exist so a managed test can build a tiny
// element tree, fire events at it, and count what the binding actually registered. ----

// Seed what a dropdown double's current selection resolves to; the real control derives both
// from its option list, which the double does not carry.
GE_API GE_Result GE_CDECL GE_TestUI_SetDropdownData(uint64_t instanceId, const char* value,
                                                    const char* label)
{
    ShimElement* el = Resolve(instanceId);
    if (!el)
        return GE_Result_NotFound;
    el->DropdownValue = value ? value : "";
    el->DropdownLabel = label ? label : "";
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_TestUI_AddElement(const char* elementId, const char* tagLower, uint64_t* outInstanceId)
{
    if (!outInstanceId)
        return GE_Result_InvalidArg;
    *outInstanceId = 0;
    if (!elementId || !tagLower)
        return GE_Result_InvalidArg;
    ShimElement el;
    el.ElementId = elementId;
    el.TagId = Fnv1a64(tagLower);
    el.Alive = true;
    const std::uint64_t id = g_NextInstanceId++;
    g_Elements[id] = el;
    *outInstanceId = id;
    return GE_Result_Ok;
}

// Destroys the element the way the tree does: it stops resolving, and it never resolves again,
// because instance ids are not reused.
GE_API GE_Result GE_CDECL GE_TestUI_DestroyElement(uint64_t instanceId)
{
    auto it = g_Elements.find(instanceId);
    if (it != g_Elements.end())
        it->second.Alive = false;
    for (auto s = g_Subscriptions.begin(); s != g_Subscriptions.end();)
        s = (s->InstanceId == instanceId) ? g_Subscriptions.erase(s) : s + 1;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_TestUI_SubscriptionCount(uint64_t instanceId, const char* eventName, int32_t* outCount)
{
    if (!outCount)
        return GE_Result_InvalidArg;
    *outCount = 0;
    for (const ShimSubscription& sub : g_Subscriptions)
        if (sub.InstanceId == instanceId && (!eventName || sub.EventName == eventName))
            ++*outCount;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_TestUI_Dispatch(uint64_t instanceId, const char* eventName, float x, float y,
                                             int32_t mods, int32_t* outInvoked)
{
    if (outInvoked)
        *outInvoked = 0;
    if (!IsKnownEvent(eventName))
        return GE_Result_InvalidArg;
    GE_UIEventData data{};
    data.elementInstanceId = instanceId;
    data.eventId = Fnv1a64(eventName);
    data.x = x;
    data.y = y;
    data.mods = mods;
    const int invoked = DispatchToSubscribers(instanceId, eventName, data);
    if (outInvoked)
        *outInvoked = invoked;
    return GE_Result_Ok;
}

// Register a do-nothing subscription and hand back the export's own result code, so a test
// can observe the ABI's argument rules without owning a managed callback.
// Raise an event that carries a PAYLOAD rather than a pointer position — a value change or
// a scroll offset. Kept separate from GE_TestUI_Dispatch so the pointer-shaped callers keep
// their signature; what these events exist to deliver is exactly the fields it adds.
GE_API GE_Result GE_CDECL GE_TestUI_DispatchPayload(uint64_t instanceId, const char* eventName,
                                                    float value, float scrollX, float scrollY,
                                                    int32_t* outInvoked)
{
    if (outInvoked)
        *outInvoked = 0;
    if (!IsKnownEvent(eventName))
        return GE_Result_InvalidArg;
    GE_UIEventData data{};
    data.elementInstanceId = instanceId;
    data.eventId = Fnv1a64(eventName);
    data.value = value;
    data.scrollX = scrollX;
    data.scrollY = scrollY;
    const int invoked = DispatchToSubscribers(instanceId, eventName, data);
    if (outInvoked)
        *outInvoked = invoked;
    return GE_Result_Ok;
}

// Raise an event that carries a TEXT payload — the pointer-and-length pair a string-valued
// control's value events ride. The pointer handed to callbacks is `text` itself, valid for
// exactly the call, which is the same window the real dispatch guarantees.
GE_API GE_Result GE_CDECL GE_TestUI_DispatchTextPayload(uint64_t instanceId, const char* eventName,
                                                        const char* text, int32_t* outInvoked)
{
    if (outInvoked)
        *outInvoked = 0;
    if (!IsKnownEvent(eventName))
        return GE_Result_InvalidArg;
    GE_UIEventData data{};
    data.elementInstanceId = instanceId;
    data.eventId = Fnv1a64(eventName);
    data.text = text;
    data.textLen = text ? static_cast<uint32_t>(std::strlen(text)) : 0;
    const int invoked = DispatchToSubscribers(instanceId, eventName, data);
    if (outInvoked)
        *outInvoked = invoked;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_TestUI_RegisterNoopEvent(uint64_t instanceId, const char* eventName)
{
    std::uint64_t token = 0;
    return GE_UIElement_RegisterEvent(instanceId, eventName, &ShimNoopEventCallback, nullptr, &token);
}

GE_API GE_Result GE_CDECL GE_TestUI_GetTagIdCallCount(uint64_t* outCount)
{
    if (!outCount)
        return GE_Result_InvalidArg;
    *outCount = g_GetTagIdCalls;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_TestUI_Reset()
{
    g_Elements.clear();
    g_Subscriptions.clear();
    g_GetTagIdCalls = 0;
    // Instance ids keep climbing: never reusing one is the property the whole handle model
    // rests on, so the double must not reset it either.
    return GE_Result_Ok;
}

// ---- The element-TYPE lifecycle, doubled ------------------------------------------------
//
// The managed half of C#-defined element types — discovery, staging, the registration window,
// the unload seam — is ordinary managed bookkeeping whose bugs are ordering bugs, and none of it
// is reachable from a dotnet-test host without these exports. The engine-side contract stays
// pinned in UITests (ManagedTypeLifecycleTests) against the real registry; this stands in for the
// engine so the BINDING's ordering can be driven.
//
// THE WINDOW IS DEFERRED HERE, EXACTLY AS THE ENGINE DEFERS IT. RequestTypeRegistrationWindow
// only raises a flag; a test drains it with GE_TestUI_DrainRegistrationWindow. That gap is the
// whole point — in the engine it is at least one main-thread task, and it is the interval in
// which a second swap can unload a context whose batch is already staged. A double that ran the
// window synchronously would close the very window the ordering bugs live in.

namespace
{
struct ShimTypeOwner
{
    bool Live = false;
};

std::unordered_map<std::uint64_t, ShimTypeOwner> g_TypeOwners;
std::uint64_t g_NextTypeOwner = 1000;
// tagLower -> owner that holds it, mirroring ElementFactoryRegistry's first-registration-wins.
std::unordered_map<std::string, std::uint64_t> g_OwnerByTag;
std::uint64_t g_OwnersMinted = 0;
std::uint64_t g_OrphanCalls = 0;
std::uint64_t g_VerdictCalls = 0;
std::uint64_t g_WindowRequests = 0;
void(GE_CDECL* g_PerformRegistrations)() = nullptr;
} // namespace

GE_API GE_Result GE_CDECL GE_UI_AcquireTypeOwner(uint64_t* outOwner)
{
    if (!outOwner)
        return GE_Result_InvalidArg;
    const std::uint64_t owner = ++g_NextTypeOwner;
    g_TypeOwners[owner] = ShimTypeOwner{true};
    ++g_OwnersMinted;
    *outOwner = owner;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UI_ReleaseTypeOwner(uint64_t owner)
{
    auto it = g_TypeOwners.find(owner);
    if (it == g_TypeOwners.end() || !it->second.Live)
        return GE_Result_InvalidArg;
    it->second.Live = false;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UI_SetElementTypeCallbacks(
    intptr_t(GE_CDECL* /*create*/)(uint64_t, uint64_t),
    void(GE_CDECL* /*applyAttribute*/)(intptr_t, const char*, const char*),
    void(GE_CDECL* /*release*/)(intptr_t),
    void(GE_CDECL* performRegistrations)())
{
    g_PerformRegistrations = performRegistrations;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UI_RequestTypeRegistrationWindow(void)
{
    ++g_WindowRequests;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UI_RegisterElementType(uint64_t owner, const char* tagName, uint64_t* outTagId)
{
    if (!outTagId)
        return GE_Result_InvalidArg;
    *outTagId = 0;
    if (!tagName || *tagName == '\0')
        return GE_Result_InvalidArg;
    auto it = g_TypeOwners.find(owner);
    if (owner == 0 || it == g_TypeOwners.end() || !it->second.Live)
        return GE_Result_Ok; // refused, and a refusal is an ok call with a 0 id

    std::string key;
    for (const char* c = tagName; *c; ++c)
        key.push_back(static_cast<char>((*c >= 'A' && *c <= 'Z') ? (*c - 'A' + 'a') : *c));

    // First registration wins, exactly as ElementFactoryRegistry does.
    auto held = g_OwnerByTag.find(key);
    if (held != g_OwnerByTag.end() && held->second != owner)
        return GE_Result_Ok;

    g_OwnerByTag[key] = owner;
    *outTagId = Fnv1a64(key.c_str());
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UI_OrphanElementTypes(uint64_t owner, uint64_t* outOrphaned)
{
    if (outOrphaned)
        *outOrphaned = 0;
    ++g_OrphanCalls;
    for (auto it = g_OwnerByTag.begin(); it != g_OwnerByTag.end();)
        it = (it->second == owner) ? g_OwnerByTag.erase(it) : std::next(it);
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_UI_NotifyReloadCompleted(uint64_t owner)
{
    ++g_VerdictCalls;
    auto it = g_TypeOwners.find(owner);
    if (it == g_TypeOwners.end() || !it->second.Live)
        return GE_Result_InvalidArg;
    return GE_Result_Ok;
}

// ---- Test drivers for the type lifecycle ----

GE_API GE_Result GE_CDECL GE_TestUI_DrainRegistrationWindow(int32_t* outRan)
{
    if (outRan)
        *outRan = 0;
    if (!g_PerformRegistrations)
        return GE_Result_Ok;
    // Drained on demand rather than gated on a pending request: an arm may drive StageTypesFrom
    // directly, which is the binding's internal seam and does not itself ask for a window (Arm and
    // the reload event do). The property that matters here is that the window is SEPARATE from
    // staging, which it still is. GE_TestUI_WindowRequestCount is how an arm asserts a request.
    g_PerformRegistrations();
    if (outRan)
        *outRan = 1;
    return GE_Result_Ok;
}

// Who holds a tag right now: 0 when nobody does. This is what says whether a staged batch was
// registered, and under which owner.
GE_API GE_Result GE_CDECL GE_TestUI_TagOwner(const char* tagLower, uint64_t* outOwner)
{
    if (!outOwner)
        return GE_Result_InvalidArg;
    *outOwner = 0;
    if (!tagLower)
        return GE_Result_InvalidArg;
    auto it = g_OwnerByTag.find(tagLower);
    if (it != g_OwnerByTag.end())
        *outOwner = it->second;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_TestUI_TypeOwnerStats(uint64_t* outMinted, uint64_t* outLive,
                                                   uint64_t* outOrphanCalls, uint64_t* outVerdictCalls)
{
    if (outMinted)
        *outMinted = g_OwnersMinted;
    if (outLive)
    {
        std::uint64_t live = 0;
        for (const auto& kv : g_TypeOwners)
            if (kv.second.Live)
                ++live;
        *outLive = live;
    }
    if (outOrphanCalls)
        *outOrphanCalls = g_OrphanCalls;
    if (outVerdictCalls)
        *outVerdictCalls = g_VerdictCalls;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_TestUI_WindowRequestCount(uint64_t* outCount)
{
    if (!outCount)
        return GE_Result_InvalidArg;
    *outCount = g_WindowRequests;
    return GE_Result_Ok;
}

GE_API GE_Result GE_CDECL GE_TestUI_ResetTypeLifecycle()
{
    g_TypeOwners.clear();
    g_OwnerByTag.clear();
    g_OwnersMinted = 0;
    g_OrphanCalls = 0;
    g_VerdictCalls = 0;
    g_WindowRequests = 0;
    // g_PerformRegistrations is deliberately kept: the managed side installs it once per process
    // (installation is first-wins), so clearing it would make every test after the first blind.
    return GE_Result_Ok;
}

} // extern "C"
#endif
