#include "Panels/RestoreSceneBackupModal.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"

namespace GameEngine
{

RestoreSceneBackupModal::RestoreSceneBackupModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    // Backdrop
    auto backdrop = std::make_unique<UIElement>();
    m_Backdrop = backdrop.get();
    m_Backdrop->AddClass("modal-backdrop");
    m_Backdrop->SetFocusable(true);

    // Window
    auto window = std::make_unique<UIElement>();
    m_Window = window.get();
    m_Window->AddClass("modal-window");
    m_Window->AddClass("modal-window-shadow");
    m_Window->AddClass("modal-window-520");

    // Header
    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("modal-header");
        auto title = std::make_unique<Label>();
        m_Title = title.get();
        title->SetText("Restore unsaved changes?");
        title->AddClass("modal-title");
        header->AddChild(std::move(title));
        m_Window->AddChild(std::move(header));
    }

    // Content
    {
        auto content = std::make_unique<UIElement>();
        content->AddClass("modal-content");
        auto msg = std::make_unique<Label>();
        m_Message = msg.get();
        msg->SetText("This scene has autosaved changes from a previous session.");
        msg->AddClass("modal-message");
        content->AddChild(std::move(msg));
        m_Window->AddChild(std::move(content));
    }

    // Footer buttons
    {
        auto footer = std::make_unique<UIElement>();
        footer->AddClass("modal-footer");

        auto discard = std::make_unique<Button>();
        discard->SetText("Discard");
        discard->AddClass("secondary");
        discard->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnDiscardClicked(); });

        auto restore = std::make_unique<Button>();
        restore->SetText("Restore");
        restore->AddClass("primary");
        restore->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnRestoreClicked(); });

        footer->AddChild(std::move(discard));
        footer->AddChild(std::move(restore));
        m_Window->AddChild(std::move(footer));
    }

    // Enter restores (the safe choice). Deliberately no Escape binding:
    // Discard deletes the autosaved work and must be an explicit click.
    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible)
            return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnRestoreClicked();
        }
    });

    m_Backdrop->AddChild(std::move(window));
    AddChild(std::move(backdrop));
}

void RestoreSceneBackupModal::Show(const std::string& title, const std::string& message)
{
    m_Visible = true;
    if (auto* t = dynamic_cast<Label*>(m_Title))
        t->SetText(title);
    if (auto* m = dynamic_cast<Label*>(m_Message))
        m->SetText(message);
    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
}

void RestoreSceneBackupModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void RestoreSceneBackupModal::OnRestoreClicked()
{
    Hide();
    if (m_OnRestore)
        m_OnRestore();
}

void RestoreSceneBackupModal::OnDiscardClicked()
{
    Hide();
    if (m_OnDiscard)
        m_OnDiscard();
}

} // namespace GameEngine
