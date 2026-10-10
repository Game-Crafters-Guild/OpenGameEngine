#pragma once

#include "UI/UIElement.h"

#include <functional>
#include <string>

namespace GameEngine
{

// Two-button modal: Continue / Cancel. ShowNotice turns it into a one-button
// notice, for a fact the user must acknowledge but has no choice about.
class ConfirmActionModal final : public UIElement
{
public:
    ConfirmActionModal();

    void Show(const std::string& title, const std::string& message,
              const std::string& confirmLabel = "Continue");
    // What a notice shows, top to bottom: the message, a muted detail line
    // (a location, say), and closing text. Empty Detail or Closing is left out.
    struct Notice
    {
        std::string Title;
        std::string Message;
        std::string Detail;
        std::string Closing;
        std::string DismissLabel;
    };
    // One button, labelled `DismissLabel` and focused; Cancel is absent. Enter
    // runs the confirm callback and Escape the cancel callback, and both close
    // the notice. The text wraps to the window width and breaks inside a word
    // when it must, because a notice carries text its caller does not control
    // (file paths, loader errors).
    void ShowNotice(const Notice& notice);
    void Hide();
    bool IsVisible() const { return m_Visible; }

    void SetOnConfirm(std::function<void()> cb) { m_OnConfirm = std::move(cb); }
    void SetOnCancel(std::function<void()> cb)  { m_OnCancel  = std::move(cb); }

    // Programmatic equivalents of the buttons (debug-server respond_modal).
    void ChooseConfirm() { OnConfirmClicked(); }
    void ChooseCancel() { OnCancelClicked(); }

private:
    // `focusTarget` takes keyboard focus once the modal is on screen.
    void Present(const std::string& title, const std::string& message,
                 const std::string& confirmLabel, UIElement* focusTarget);
    void OnConfirmClicked();
    void OnCancelClicked();

    UIElement* m_Backdrop    = nullptr;
    UIElement* m_Title       = nullptr;
    UIElement* m_Message     = nullptr;
    UIElement* m_Detail      = nullptr; // notice only
    UIElement* m_Closing     = nullptr; // notice only
    class Button* m_ConfirmBtn = nullptr;
    class Button* m_CancelBtn  = nullptr;

    bool m_Visible = false;

    std::function<void()> m_OnConfirm;
    std::function<void()> m_OnCancel;
};

} // namespace GameEngine
