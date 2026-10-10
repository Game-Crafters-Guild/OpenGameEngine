#include "Panels/CameraCustomAspectModal.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"

#include <algorithm>

namespace GameEngine
{

namespace
{
constexpr float kMinAspectDimension = 0.0001f;
}

CameraCustomAspectModal::CameraCustomAspectModal()
{
    AddClass("camera-aspect-modal");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    auto backdrop = std::make_unique<UIElement>();
    m_Backdrop = backdrop.get();
    m_Backdrop->AddClass("camera-aspect-modal-backdrop");

    auto window = std::make_unique<UIElement>();
    m_Window = window.get();
    m_Window->AddClass("modal-window");
    m_Window->AddClass("camera-aspect-modal-window");

    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("camera-aspect-modal-header");
        auto title = std::make_unique<Label>();
        title->SetText("Custom Aspect Ratio");
        title->AddClass("camera-aspect-modal-title");
        header->AddChild(std::move(title));
        m_Window->AddChild(std::move(header));
    }

    {
        auto content = std::make_unique<UIElement>();
        content->AddClass("camera-aspect-modal-content");

        auto description = std::make_unique<Label>();
        description->SetText("Enter the target width and height ratio used for projection and black bars.");
        description->AddClass("camera-aspect-modal-description");
        content->AddChild(std::move(description));

        auto makeFieldRow = [](const char* labelText, FloatField*& outField) {
            auto row = std::make_unique<UIElement>();
            row->AddClass("camera-aspect-modal-field-row");

            auto label = std::make_unique<Label>();
            label->SetText(labelText);
            label->AddClass("camera-aspect-modal-field-label");
            row->AddChild(std::move(label));

            auto field = std::make_unique<FloatField>();
            outField = field.get();
            field->AddClass("camera-aspect-modal-field");
            row->AddChild(std::move(field));
            return row;
        };

        content->AddChild(makeFieldRow("Width", m_WidthField));
        content->AddChild(makeFieldRow("Height", m_HeightField));
        m_Window->AddChild(std::move(content));
    }

    {
        auto footer = std::make_unique<UIElement>();
        footer->AddClass("camera-aspect-modal-footer");

        auto cancel = std::make_unique<Button>();
        cancel->SetText("Cancel");
        cancel->AddClass("secondary");
        cancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });

        auto ok = std::make_unique<Button>();
        ok->SetText("Apply");
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

void CameraCustomAspectModal::Show(float initialWidth, float initialHeight)
{
    m_Visible = true;
    if (m_WidthField)
        m_WidthField->SetValue(std::max(initialWidth, kMinAspectDimension));
    if (m_HeightField)
        m_HeightField->SetValue(std::max(initialHeight, kMinAspectDimension));

    AddClass("visible");

    if (m_WidthField)
    {
        if (auto* mgr = GetOwnerManager())
        {
            FloatField* field = m_WidthField;
            mgr->PostToUI([mgr, field]()
            {
                mgr->FocusElement(field);
                field->SelectAll();
            });
        }
        else
        {
            m_WidthField->SelectAll();
        }
    }
}

void CameraCustomAspectModal::Hide()
{
    m_Visible = false;
    RemoveClass("visible");
}

void CameraCustomAspectModal::OnCommitClicked()
{
    if (!m_WidthField || !m_HeightField)
        return;

    const float width = std::max(m_WidthField->GetValue(), kMinAspectDimension);
    const float height = std::max(m_HeightField->GetValue(), kMinAspectDimension);
    Hide();
    if (m_OnCommit)
        m_OnCommit(width, height);
}

void CameraCustomAspectModal::OnCancelClicked()
{
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine
