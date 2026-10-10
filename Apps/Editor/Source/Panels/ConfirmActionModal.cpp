#include "Panels/ConfirmActionModal.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"

namespace GameEngine
{

namespace
{
// The editor's generic hide class (theme/core.css `.hidden.hidden`, specificity
// 0,2,0, which outranks Button.css's `.button { display: flex }`). Takes the
// Cancel button out of layout while the modal is a notice, and a notice-only
// text block while it is empty or the modal is a confirmation.
constexpr const char* kHiddenClass = "hidden";
// Modal.css: keeps the caller's line breaks and does not wrap. Show's messages
// are laid out by their callers; a notice's message wraps instead.
constexpr const char* kPreformattedMessageClass = "modal-message-pre";
// Modal.css: lets a wrapped message break inside a word, so a file path with
// no spaces wraps at the window edge instead of running past it.
constexpr const char* kBreakableMessageClass = "modal-message-breakable";
// Modal.css: the secondary line of a notice (a location, a detail), in
// --ui_color_text_dim, which is darker than the message text.
constexpr const char* kDetailMessageClass = "modal-message-detail";

// Shows `text` in `block`, or takes the block out of layout when there is none.
void SetOptionalText(UIElement* block, const std::string& text)
{
    auto* label = dynamic_cast<Label*>(block);
    if (!label)
        return;
    label->SetText(text);
    if (text.empty())
        label->AddClass(kHiddenClass);
    else
        label->RemoveClass(kHiddenClass);
}
} // namespace

ConfirmActionModal::ConfirmActionModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    auto backdrop = std::make_unique<UIElement>();
    m_Backdrop = backdrop.get();
    m_Backdrop->AddClass("modal-backdrop");
    m_Backdrop->SetFocusable(true);

    auto window = std::make_unique<UIElement>();
    window->AddClass("modal-window");
    window->AddClass("modal-window-shadow");
    window->AddClass("modal-window-480");

    // Header
    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("modal-header");
        auto title = std::make_unique<Label>();
        m_Title = title.get();
        title->AddClass("modal-title");
        title->AddClass("modal-title-compact");
        header->AddChild(std::move(title));
        window->AddChild(std::move(header));
    }

    // Content: the message, then a notice's muted detail and its closing text.
    {
        auto content = std::make_unique<UIElement>();
        content->AddClass("modal-content");
        content->AddClass("modal-content-column");
        auto msg = std::make_unique<Label>();
        m_Message = msg.get();
        msg->AddClass("modal-message");
        msg->AddClass(kPreformattedMessageClass);
        content->AddChild(std::move(msg));

        auto detail = std::make_unique<Label>();
        m_Detail = detail.get();
        detail->AddClass("modal-message");
        detail->AddClass(kDetailMessageClass);
        detail->AddClass(kBreakableMessageClass);
        detail->AddClass(kHiddenClass);
        content->AddChild(std::move(detail));

        auto closing = std::make_unique<Label>();
        m_Closing = closing.get();
        closing->AddClass("modal-message");
        closing->AddClass(kBreakableMessageClass);
        closing->AddClass(kHiddenClass);
        content->AddChild(std::move(closing));
        window->AddChild(std::move(content));
    }

    // Footer
    {
        auto footer = std::make_unique<UIElement>();
        footer->AddClass("modal-footer");

        auto cancel = std::make_unique<Button>();
        m_CancelBtn = cancel.get();
        cancel->SetText("Cancel");
        cancel->AddClass("secondary");
        cancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });

        auto confirm = std::make_unique<Button>();
        m_ConfirmBtn = confirm.get();
        confirm->SetText("Continue");
        confirm->AddClass("primary");
        confirm->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnConfirmClicked(); });

        footer->AddChild(std::move(cancel));
        footer->AddChild(std::move(confirm));
        window->AddChild(std::move(footer));
    }

    // Enter and Escape reach this handler from the backdrop (Show focuses it)
    // and bubble up to it from the dismiss button (ShowNotice focuses that,
    // which handles Enter itself as a click).
    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible) return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnConfirmClicked();
        }
        else if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            OnCancelClicked();
        }
    });

    m_Backdrop->AddChild(std::move(window));
    AddChild(std::move(backdrop));
}

void ConfirmActionModal::Show(const std::string& title, const std::string& message,
                              const std::string& confirmLabel)
{
    if (m_CancelBtn)
        m_CancelBtn->RemoveClass(kHiddenClass);
    if (m_Message)
    {
        m_Message->RemoveClass(kBreakableMessageClass);
        m_Message->AddClass(kPreformattedMessageClass);
    }
    SetOptionalText(m_Detail, {});
    SetOptionalText(m_Closing, {});
    Present(title, message, confirmLabel, m_Backdrop);
}

void ConfirmActionModal::ShowNotice(const Notice& notice)
{
    if (m_CancelBtn)
        m_CancelBtn->AddClass(kHiddenClass);
    if (m_Message)
    {
        m_Message->RemoveClass(kPreformattedMessageClass);
        m_Message->AddClass(kBreakableMessageClass);
    }
    SetOptionalText(m_Detail, notice.Detail);
    SetOptionalText(m_Closing, notice.Closing);
    Present(notice.Title, notice.Message, notice.DismissLabel, m_ConfirmBtn);
}

void ConfirmActionModal::Present(const std::string& title, const std::string& message,
                                 const std::string& confirmLabel, UIElement* focusTarget)
{
    m_Visible = true;
    if (auto* lbl = dynamic_cast<Label*>(m_Title))
        lbl->SetText(title);
    if (auto* lbl = dynamic_cast<Label*>(m_Message))
        lbl->SetText(message);
    if (m_ConfirmBtn)
        m_ConfirmBtn->SetText(confirmLabel);
    AddClass("visible");
    if (auto* manager = GetOwnerManager(); manager && focusTarget)
        manager->PostToUI([manager, focusTarget]() { manager->FocusElement(focusTarget); });
}

void ConfirmActionModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void ConfirmActionModal::OnConfirmClicked()
{
    Hide();
    if (m_OnConfirm)
        m_OnConfirm();
}

void ConfirmActionModal::OnCancelClicked()
{
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine
