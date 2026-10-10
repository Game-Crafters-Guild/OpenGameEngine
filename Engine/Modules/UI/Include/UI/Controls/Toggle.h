#pragma once

#include <string>

#include "UI/Controls/BaseField.h"
#include "UI/UIEvents.h"

namespace GameEngine {

class UIElement;

    // Shared boolean toggle base for Checkbox/Toggle (and future RadioButton).
    // Provides value storage via Field<bool>, maintains a "checked" CSS class,
    // and implements basic mouse/keyboard toggling.
class ToggleBase : public Field<bool>
{
public:
    ToggleBase();

    using ValueType = bool;

    bool IsChecked() const { return GetValue(); }
    void SetChecked(bool checked);
    bool IsPseudoChecked() const override { return IsChecked(); }

    // Override to fire value callbacks and keep the "checked" attribute/class
    // in sync with the logical value.
    void SetValue(const bool& v) override;

    // Update visual state without firing callbacks.
    void SetValueWithoutNotify(const bool& v) override;

    // Show an indeterminate "mixed" state for multi-selection where the selected
    // objects disagree. Cleared the moment a definite value is set (incl. a click),
    // so toggling resolves all selected entities to that value.
    void SetMixed();
    bool IsMixed() const { return m_Mixed; }

    void OnEvent(UIEvent& e) override;

protected:
    bool IsPointInside(float x, float y) const;
    // Runs after the checked or mixed state changes, for a subtype that draws that state on a
    // child element rather than on itself.
    virtual void OnToggleStateChanged() {}

private:
    void ClearMixed();
    // Sync the "checked" class (for `.checked` selectors) and route the
    // :checked pseudo flip through the manager's analysis-driven marker.
    void ApplyCheckedState(bool checked);

    bool m_Armed{false};
    bool m_Mixed{false};
};

// Simple on/off toggle control with slider-style visuals provided by CSS.
class Toggle : public ToggleBase
{
public:
    Toggle();

    // Sets the presentation class inherited by Toggles created afterward.
    // Hosts that change this at runtime are responsible for updating existing
    // instances, since Toggle intentionally has no global element registry.
    static void SetCheckmarkPresentationEnabled(bool enabled);
    static bool IsCheckmarkPresentationEnabled();

    private:
        void       EnsureHandle();
        UIElement* m_HandleChild{nullptr};
};

} // namespace GameEngine
