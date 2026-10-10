#include "Panels/SaveSceneChangesModal.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"

namespace GameEngine
{

SaveSceneChangesModal::SaveSceneChangesModal()
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
        title->SetText("Save changes?");
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
        msg->SetText("The scene has unsaved changes.");
        msg->AddClass("modal-message");
        content->AddChild(std::move(msg));
        m_Window->AddChild(std::move(content));
    }

    // Footer buttons
    {
        auto footer = std::make_unique<UIElement>();
        footer->AddClass("modal-footer");

        auto cancel = std::make_unique<Button>();
        cancel->SetText("Cancel");
        cancel->AddClass("secondary");
        cancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });

        auto dontSave = std::make_unique<Button>();
        dontSave->SetText("Don't Save");
        dontSave->AddClass("secondary");
        dontSave->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnDontSaveClicked(); });

        auto save = std::make_unique<Button>();
        save->SetText("Save");
        save->AddClass("primary");
        save->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnSaveClicked(); });

        m_CancelButton = cancel.get();
        m_DontSaveButton = dontSave.get();
        m_SaveButton = save.get();

        footer->AddChild(std::move(dontSave));
        footer->AddChild(std::move(cancel));
        footer->AddChild(std::move(save));
        m_Window->AddChild(std::move(footer));
    }

    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible)
            return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnSaveClicked();
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

void SaveSceneChangesModal::Show(const std::string& title, const std::string& message,
                                 const SaveSceneChangesModalLabels& labels)
{
    m_Visible = true;
    if (auto* t = dynamic_cast<Label*>(m_Title))
        t->SetText(title);
    if (auto* m = dynamic_cast<Label*>(m_Message))
        m->SetText(message);
    // Re-applied on every Show, so a prompt that customised them does not leak its wording into the
    // next caller's.
    if (auto* b = dynamic_cast<Button*>(m_SaveButton))
        b->SetText(labels.Save);
    if (auto* b = dynamic_cast<Button*>(m_DontSaveButton))
        b->SetText(labels.DontSave);
    if (auto* b = dynamic_cast<Button*>(m_CancelButton))
        b->SetText(labels.Cancel);
    AddClass("visible");
    if (auto* manager = GetOwnerManager())
    {
        UIElement* backdrop = m_Backdrop;
        manager->PostToUI([manager, backdrop]() { manager->FocusElement(backdrop); });
    }
}

void SaveSceneChangesModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void SaveSceneChangesModal::OnSaveClicked()
{
    Hide();
    if (m_OnSave)
        m_OnSave();
}

void SaveSceneChangesModal::OnDontSaveClicked()
{
    Hide();
    if (m_OnDontSave)
        m_OnDontSave();
}

void SaveSceneChangesModal::OnCancelClicked()
{
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine
