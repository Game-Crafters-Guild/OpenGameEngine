#pragma once

#include "Panels/ColorPicker.h"
#include "UI/Controls/DraggableModal.h"
#include "UI/Interaction/DismissablePopup.h"
#include <functional>

namespace GameEngine
{

class Button;
class Label;

// Opens a window/popup that contains a ColorPicker UI element.
// The picker itself is a pure UIElement; this wrapper handles show/hide and Apply/Cancel.
//
// Undimmed: the picker previews live edits against the UI it is recoloring, so
// the surroundings stay fully readable. Dismissal (outside press, Escape) comes
// from UIManager's dismissable-popup registry, which is window-wide and needs
// no click-catcher of its own.
class ColorPickerPopup : public DraggableModal, public DismissablePopup
{
public:
    using OnApplyCallback = std::function<void(const ColorPickerValue&)>;
    using OnCancelCallback = std::function<void()>;
    using OnValueChangingCallback = std::function<void(const ColorPickerValue&)>;

    ColorPickerPopup();
    ~ColorPickerPopup() override = default;

    void OnOwnerManagerChanged(UIManager* owner) override { UpdatePopupRegistration(owner); }

    // DismissablePopup. This element stays full-parent sized because the host
    // below centers the panel with flex, which needs a box to center within —
    // so the popup root has to be the panel, not the element. Dismissing is a
    // cancel, matching the Cancel button.
    UIElement* GetPopupRoot() override { return m_Panel; }
    bool IsPopupOpen() const override { return m_Visible; }
    void DismissPopup() override { OnCancelClicked(); }

    void Show(const ColorPickerValue& initialValue);
    // Show placed the way ColorPickerWindow places its native tool window for the
    // same cursor: offset to the side the cursor leaves most room on, clamped
    // inside the host window. Coordinates are window-space, which is what
    // UIEvent and UIManager::GetMousePosition report.
    void ShowAt(const ColorPickerValue& initialValue, float cursorX, float cursorY);
    void Hide();
    bool IsVisible() const { return m_Visible; }

    void SetOnApply(OnApplyCallback cb) { m_OnApply = std::move(cb); }
    void SetOnCancel(OnCancelCallback cb) { m_OnCancel = std::move(cb); }
    void SetOnValueChanging(OnValueChangingCallback cb) { m_OnValueChanging = std::move(cb); }

    // Access the wrapped ColorPicker when visible (e.g. to read live value without applying).
    ColorPicker* GetPicker() const { return m_Picker; }

private:
    void OnApplyClicked();
    void OnCancelClicked();

    // DraggableModal: the panel's top-left in this element's space (it spans
    // the host window). PlaceWindow clamps so a whole title bar stays reachable.
    void GetWindowOrigin(float& x, float& y) const override;
    void PlaceWindow(float x, float y) override;
    // Panel extent for placement math. Zero before the first layout pass, where
    // the native tool window's own size stands in.
    float GetPanelWidth() const;
    float GetPanelHeight() const;

    bool m_Visible = false;
    OnApplyCallback m_OnApply;
    OnCancelCallback m_OnCancel;
    OnValueChangingCallback m_OnValueChanging;
    ColorPicker* m_Picker = nullptr;
    UIElement* m_PanelHost = nullptr;
    UIElement* m_Panel = nullptr;
};

} // namespace GameEngine
