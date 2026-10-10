// UIElementValueABI.cpp — the typed VALUE accessors of the element ABI: read and write a
// control's value as the type it actually is, with no string parse and no attribute-handler
// walk on the path — one typed export per hot property. Split from UIElementABI.cpp, which
// owns identity, liveness and subscription;
// this file owns nothing but values.
//
// THE TYPE RULES, once: every accessor resolves the element and dynamic_casts to the
// Field<T> instantiation (or concrete control) it addresses. A live element of the wrong
// type is GE_Result_InvalidArg — the caller resolved a real element and asked it something
// it cannot answer, and saying so beats a getter inventing a default. A dead handle is
// GE_Result_NotFound, as everywhere in the element ABI.
//
// THE NOTIFY CONTRACT, once: setters take `notify`. Non-zero routes the control's own
// virtual SetValue — its clamping, quantisation, presentation sync AND its own notification
// contract, which is the control's to define: a Slider or Toggle announces the change
// (members, then the handler table), while a text field's programmatic SetValue is a silent
// sync today (TextFieldBase notifies only from user editing paths). Zero routes
// SetValueWithoutNotify: the write lands with the same clamping and visual consequences and
// nobody is told — Unity's SetValueWithoutNotify, same name, same habit.
//
// STRING GETTERS use one buffer contract, documented on the prototypes: copy
// min(len, bufferLen) UTF-8 bytes, no NUL terminator, always report the full length in
// *outLen so a short buffer is a visible retry rather than silent truncation.
//
// Safety contract, threading, and why every call re-resolves by instance id: see
// UIElementABI.cpp — identical here.

#include "Scripting/ScriptingABI.h" // GE_API, GE_CDECL, GE_Result

#include "AbiTextOut.h"
#include "Engine/GameUI/GameplayUI.h"

#include "UI/Controls/BaseField.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/ScrollView.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <string>

namespace
{

// Same resolution as UIElementABI.cpp: null when there is no published gameplay host, when
// the element was destroyed, or when it is no longer reachable from the host's root.
GameEngine::UIElement* ResolveElement(uint64_t instanceId)
{
    GameEngine::UIManager* ui = GameEngine::GameUI::GetUIManager();
    return ui ? ui->FindElementByInstanceId(instanceId) : nullptr;
}

// Resolve to a typed control, distinguishing "gone" from "wrong type" for the caller.
template <typename TControl>
GE_Result ResolveTyped(uint64_t instanceId, TControl*& outControl)
{
    outControl = nullptr;
    GameEngine::UIElement* el = ResolveElement(instanceId);
    if (!el)
        return GE_Result_NotFound;
    outControl = dynamic_cast<TControl*>(el);
    return outControl ? GE_Result_Ok : GE_Result_InvalidArg;
}

} // namespace

extern "C"
{

// Any Field<float> — a Slider, a FloatField. The value read is the committed one: the
// control's own SetValue clamps and quantises before it stores.
GE_API GE_Result GE_CDECL GE_UIElement_GetValueFloat(uint64_t instanceId, float* outValue)
{
    try
    {
        if (!outValue)
            return GE_Result_InvalidArg;
        *outValue = 0.0f;
        GameEngine::Field<float>* field = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, field);
        if (rc != GE_Result_Ok)
            return rc;
        *outValue = field->GetValue();
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_UIElement_SetValueFloat(uint64_t instanceId, float value, int32_t notify)
{
    try
    {
        GameEngine::Field<float>* field = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, field);
        if (rc != GE_Result_Ok)
            return rc;
        if (notify != 0)
            field->SetValue(value);
        else
            field->SetValueWithoutNotify(value);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Any Field<bool> — a Toggle, a Checkbox. *outValue is 0 or 1.
GE_API GE_Result GE_CDECL GE_UIElement_GetValueBool(uint64_t instanceId, int32_t* outValue)
{
    try
    {
        if (!outValue)
            return GE_Result_InvalidArg;
        *outValue = 0;
        GameEngine::Field<bool>* field = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, field);
        if (rc != GE_Result_Ok)
            return rc;
        *outValue = field->GetValue() ? 1 : 0;
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_UIElement_SetValueBool(uint64_t instanceId, int32_t value, int32_t notify)
{
    try
    {
        GameEngine::Field<bool>* field = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, field);
        if (rc != GE_Result_Ok)
            return rc;
        if (notify != 0)
            field->SetValue(value != 0);
        else
            field->SetValueWithoutNotify(value != 0);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Any Field<std::string> — a TextField, a TextArea, and a Dropdown FOR READING (its field
// value is the selected option's value string).
GE_API GE_Result GE_CDECL GE_UIElement_GetValueText(uint64_t instanceId, char* buffer, int32_t bufferLen,
                                                    int32_t* outLen)
{
    try
    {
        if (outLen)
            *outLen = 0;
        GameEngine::Field<std::string>* field = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, field);
        if (rc != GE_Result_Ok)
            return rc;
        return GameEngine::ScriptingAbi::CopyTextOut(field->GetValue(), buffer, bufferLen, outLen);
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_UIElement_SetValueText(uint64_t instanceId, const char* text, int32_t notify)
{
    try
    {
        GameEngine::Field<std::string>* field = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, field);
        if (rc != GE_Result_Ok)
            return rc;
        // A Dropdown's field value is DERIVED from its options: writing it raw would desync
        // the selected index, the header label and the item classes. Drive a dropdown by
        // index (GE_UIElement_SetDropdownSelectedIndex); this setter refuses it so the
        // desync is an error at the call instead of a broken control later.
        if (dynamic_cast<GameEngine::Dropdown*>(field) != nullptr)
            return GE_Result_InvalidArg;
        const std::string value{text ? text : ""};
        if (notify != 0)
            field->SetValue(value);
        else
            field->SetValueWithoutNotify(value);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// ScrollView offsets, in pixels. The setters clamp to the scrollable range and coalesce:
// however many writes land in a frame, subscribers hear ONE settled offset at the manager's
// flush. There is deliberately no without-notify variant — the deferral already is one
// (ScrollView.h documents why re-entrancy is structurally impossible there).
GE_API GE_Result GE_CDECL GE_UIElement_GetScrollOffset(uint64_t instanceId, float* outX, float* outY)
{
    try
    {
        if (!outX || !outY)
            return GE_Result_InvalidArg;
        *outX = 0.0f;
        *outY = 0.0f;
        GameEngine::ScrollView* view = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, view);
        if (rc != GE_Result_Ok)
            return rc;
        *outX = view->GetScrollX();
        *outY = view->GetScrollY();
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_UIElement_SetScrollX(uint64_t instanceId, float x)
{
    try
    {
        GameEngine::ScrollView* view = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, view);
        if (rc != GE_Result_Ok)
            return rc;
        view->SetScrollX(x);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_UIElement_SetScrollY(uint64_t instanceId, float y)
{
    try
    {
        GameEngine::ScrollView* view = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, view);
        if (rc != GE_Result_Ok)
            return rc;
        view->SetScrollY(y);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

// Dropdown selection. *outIndex is -1 when nothing is selected, exactly as the control
// reports it.
GE_API GE_Result GE_CDECL GE_UIElement_GetDropdownSelectedIndex(uint64_t instanceId, int32_t* outIndex)
{
    try
    {
        if (!outIndex)
            return GE_Result_InvalidArg;
        *outIndex = -1;
        GameEngine::Dropdown* dropdown = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, dropdown);
        if (rc != GE_Result_Ok)
            return rc;
        *outIndex = dropdown->GetSelectedIndex();
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_UIElement_SetDropdownSelectedIndex(uint64_t instanceId, int32_t index,
                                                                int32_t notify)
{
    try
    {
        GameEngine::Dropdown* dropdown = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, dropdown);
        if (rc != GE_Result_Ok)
            return rc;
        if (notify != 0)
            dropdown->SetSelectedIndex(index);
        else
            dropdown->SetSelectedIndexWithoutNotify(index);
        return GE_Result_Ok;
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

GE_API GE_Result GE_CDECL GE_UIElement_GetDropdownSelectedLabel(uint64_t instanceId, char* buffer,
                                                                int32_t bufferLen, int32_t* outLen)
{
    try
    {
        if (outLen)
            *outLen = 0;
        GameEngine::Dropdown* dropdown = nullptr;
        const GE_Result rc = ResolveTyped(instanceId, dropdown);
        if (rc != GE_Result_Ok)
            return rc;
        return GameEngine::ScriptingAbi::CopyTextOut(dropdown->GetSelectedLabel(), buffer, bufferLen, outLen);
    }
    catch (...)
    {
        return GE_Result_Fail;
    }
}

} // extern "C"
