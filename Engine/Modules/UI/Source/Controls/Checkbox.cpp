#include "UI/Controls/Checkbox.h"

#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

namespace GameEngine {

Checkbox::Checkbox()
{
    AddClass("checkbox");
    EnsureBox();
    EnsureLabel();
}

void Checkbox::EnsureBox()
{
    if (m_BoxChild)
        return;

    auto box = std::make_unique<UIElement>();
    m_BoxChild = box.get();
    m_BoxChild->AddClass("checkbox-box");
    AddChild(std::move(box));
}

void Checkbox::EnsureLabel()
{
    if (m_LabelChild)
        return;

    auto lbl = std::make_unique<Label>();
    m_LabelChild = lbl.get();
    m_LabelChild->AddClass("checkbox-label");
    AddChild(std::move(lbl));
}

void Checkbox::SetText(const std::string& text)
{
    m_Text = text;
    EnsureLabel();
    if (m_LabelChild)
    {
        m_LabelChild->SetText(m_Text);
    }
    MarkDirty(LayoutDirty | VisualDirty);
}

} // namespace GameEngine

