#include "Panels/RenameLayoutModal.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"

namespace GameEngine
{

RenameLayoutModal::RenameLayoutModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    auto backdrop = std::make_unique<UIElement>();
    m_Backdrop = backdrop.get();
    m_Backdrop->AddClass("modal-backdrop");

    auto window = std::make_unique<UIElement>();
    m_Window = window.get();
    m_Window->AddClass("modal-window");
    m_Window->AddClass("modal-window-shadow");
    m_Window->AddClass("modal-window-420");

    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("modal-header");
        auto title = std::make_unique<Label>();
        m_Title = title.get();
        title->SetText("Rename Layout");
        title->AddClass("modal-title");
        header->AddChild(std::move(title));
        m_Window->AddChild(std::move(header));
    }

    {
        auto content = std::make_unique<UIElement>();
        content->AddClass("modal-content");
        content->AddClass("rename-layout-modal-content");

        auto label = std::make_unique<Label>();
        label->SetText("New name");
        label->AddClass("modal-field-label");
        content->AddChild(std::move(label));

        auto field = std::make_unique<TextField>();
        m_NameField = field.get();
        field->SetValue("");
        field->AddClass("modal-text-field");
        field->SetOnCommit([this]() { OnCommitClicked(); });
        content->AddChild(std::move(field));

        m_Window->AddChild(std::move(content));
    }

    {
        auto footer = std::make_unique<UIElement>();
        footer->AddClass("modal-footer");

        auto cancel = std::make_unique<Button>();
        cancel->SetText("Cancel");
        cancel->AddClass("secondary");
        cancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });

        auto ok = std::make_unique<Button>();
        ok->SetText("Rename");
        ok->AddClass("primary");
        ok->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCommitClicked(); });

        footer->AddChild(std::move(cancel));
        footer->AddChild(std::move(ok));
        m_Window->AddChild(std::move(footer));
    }

    m_Backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible)
            return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnCommitClicked();
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

void RenameLayoutModal::Show(const std::string& title, const std::string& initialValue)
{
    m_Visible = true;
    if (m_Title)
        m_Title->SetText(title);
    if (m_NameField)
        m_NameField->SetValue(initialValue);

    AddClass("visible");

    if (m_NameField)
    {
        if (auto* mgr = GetOwnerManager())
        {
            TextField* field = m_NameField;
            mgr->PostToUI([mgr, field]()
            {
                mgr->FocusElement(field);
                field->SelectAll();
            });
        }
        else
        {
            m_NameField->SelectAll();
        }
    }
}

void RenameLayoutModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void RenameLayoutModal::OnCommitClicked()
{
    if (!m_NameField)
        return;
    const std::string value = m_NameField->GetValue();
    Hide();
    if (m_OnCommit)
        m_OnCommit(value);
}

void RenameLayoutModal::OnCancelClicked()
{
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine
