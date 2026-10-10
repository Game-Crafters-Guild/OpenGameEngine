#include "UI/Controls/Toggle.h"

#include "Input/KeyCodes.h"
#include "UI/UIElement.h"

#include <atomic>

namespace GameEngine {

namespace
{
std::atomic_bool g_CheckmarkPresentationEnabled{false};
}

ToggleBase::ToggleBase()
{
    // Toggles are focusable so they can participate in keyboard navigation.
    SetFocusable(true);
}

void ToggleBase::SetChecked(bool checked)
{
    SetValue(checked);
}

void ToggleBase::SetValue(const bool& v)
{
    ClearMixed();  // a definite value is never indeterminate

    if (GetValue() == v)
        return;

    // A boolean flip changes no layout on the toggle by itself; only the
    // :checked / .checked CSS rules can, and the style analysis decides whether
    // they do. So store the value directly rather than through
    // Field<bool>::SetValue, whose unconditional LayoutDirty is right for
    // text/number fields (content resizes) but wrong here — it forced a Yoga
    // solve on every toggle regardless of the CSS.
    m_Value = v;

    // Fire callbacks for listeners.
    NotifyValueChanging();
    NotifyValueChanged();

    ApplyCheckedState(v);
}

void ToggleBase::SetValueWithoutNotify(const bool& v)
{
    ClearMixed();

    if (GetValue() == v)
        return;

    m_Value = v;

    ApplyCheckedState(v);
}

void ToggleBase::ApplyCheckedState(bool checked)
{
    // Keep the "checked" class in sync so `.checked` selectors match (log-filter
    // toggles and others style via the class, not the :checked pseudo).
    // AddClass/RemoveClass marks StyleDirty|VisualDirty across the subtree and
    // invalidates the rule cache, re-cascading both `.checked` class rules and
    // the :checked pseudo (which flips together with the value).
    if (checked)
        AddClass("checked");
    else
        RemoveClass("checked");

    // Layer the :checked analysis on top: it adds LayoutDirty when a :checked
    // rule is layout-affecting (e.g. `toggle:checked { justify-content }`) and
    // fans out per descendant/sibling combinators — the piece the class mark,
    // which reaches layout only via the signature fallback, does not supply.
    UIManagerMarkCheckedStateScope(GetOwnerManager(), this);
    OnToggleStateChanged();
}

void ToggleBase::SetMixed()
{
    m_Mixed = true;
    AddClass("mixed");
    OnToggleStateChanged();
}

void ToggleBase::ClearMixed()
{
    if (!m_Mixed)
        return;
    m_Mixed = false;
    RemoveClass("mixed");
    OnToggleStateChanged();
}

bool ToggleBase::IsPointInside(float x, float y) const
{
    float lx = GetLayoutX();
    float ly = GetLayoutY();
    float w  = GetLayoutWidth();
    float h  = GetLayoutHeight();
    return (x >= lx && y >= ly && x < (lx + w) && y < (ly + h));
}

void ToggleBase::OnEvent(UIEvent& e)
{
    // A disabled control never activates. The UI manager delivers no press,
    // release, key or focus to a disabled element or anything inside one (the
    // delivery rule for disabled subtrees, UIManager_Input.cpp); this gate is the
    // control's own second guarantee, so an event dispatched to it directly cannot
    // flip a value its owner refuses to store. Focus and text events still bridge
    // to Field<bool> so the control keeps its focus classes. A cancel passes: it
    // only disarms, and a press held while the control was disabled must let go.
    if (!IsEnabled() &&
        (e.Id == kEventMouseDown || e.Id == kEventMouseMove || e.Id == kEventMouseUp ||
         e.Id == kEventKeyDown))
    {
        Field<bool>::OnEvent(e);
        return;
    }

    if (e.Id == kEventMouseDown)
    {
        // Arm the toggle and show pressed feedback.
        m_Armed = true;
        AddClass("pressed");
        e.Capture(this);
        e.Stop();
        return;
    }

    if (e.Id == kEventMouseMove)
    {
        if (m_Armed)
        {
            if (IsPointInside(e.X, e.Y))
            {
                if (!HasClass("pressed"))
                    AddClass("pressed");
            }
            else
            {
                if (HasClass("pressed"))
                    RemoveClass("pressed");
            }
            e.Stop();
        }
        return;
    }

    if (e.Id == kEventMouseCancel)
    {
        // Press abandoned (pointer left the surface): disarm without toggling.
        if (HasClass("pressed"))
            RemoveClass("pressed");
        m_Armed = false;
        return;
    }

    if (e.Id == kEventMouseUp)
    {
        if (m_Armed)
        {
            bool inside = IsPointInside(e.X, e.Y);
            if (HasClass("pressed"))
                RemoveClass("pressed");
            if (inside)
            {
                SetChecked(!IsChecked());
            }
            e.Stop();
        }
        m_Armed = false;
        return;
    }

    if (e.Id == kEventKeyDown)
    {
        // Keyboard activation when the control is focused.
        if (e.Key == Input::kKeyCode_Space || e.Key == Input::kKeyCode_Enter)
        {
            SetChecked(!IsChecked());
            e.Stop();
            return;
        }
    }

    // Fallback to Field<bool> default bridge for focus/text events.
    Field<bool>::OnEvent(e);
}

Toggle::Toggle()
{
    AddClass("toggle");
    // Control styling is an Editor-shipped UIStyle asset.
    RequestSubtreeStyleAssetPath("UI/controls/Toggle.css", "editor");
    if (IsCheckmarkPresentationEnabled())
        AddClass("toggle-checkmark");
    EnsureHandle();
}

void Toggle::SetCheckmarkPresentationEnabled(bool enabled)
{
    g_CheckmarkPresentationEnabled.store(enabled, std::memory_order_relaxed);
}

bool Toggle::IsCheckmarkPresentationEnabled()
{
    return g_CheckmarkPresentationEnabled.load(std::memory_order_relaxed);
}

void Toggle::EnsureHandle()
{
    if (m_HandleChild)
        return;

    auto knob = std::make_unique<UIElement>();
    m_HandleChild = knob.get();
    m_HandleChild->AddClass("toggle-knob");
    AddChild(std::move(knob));
}

} // namespace GameEngine
