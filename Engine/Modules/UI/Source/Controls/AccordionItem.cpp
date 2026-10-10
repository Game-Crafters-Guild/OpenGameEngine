#include "UI/Controls/AccordionItem.h"
#include "UI/Controls/Foldout.h"

namespace GameEngine {

AccordionItem::AccordionItem()
{
    AddClass("accordion-item");
}

void AccordionItem::SetTitle(const std::string& title)
{
    m_Title = title;
    SyncToFoldout();
    MarkDirty(LayoutDirty | VisualDirty);
}

void AccordionItem::SetIconClass(const std::string& iconClass)
{
    m_IconClass = iconClass;
    SyncToFoldout();
    MarkDirty(LayoutDirty | VisualDirty);
}

void AccordionItem::SetExpanded(bool expanded)
{
    m_Expanded = expanded;
    m_ExpandedExplicitlySet = true;
    SyncToFoldout();
    MarkDirty(LayoutDirty | VisualDirty);
}

void AccordionItem::SyncToFoldout()
{
    if (!m_InternalFoldout)
        return;
    
    m_InternalFoldout->SetTitle(m_Title);
    m_InternalFoldout->SetIconClass(m_IconClass);
    m_InternalFoldout->SetExpanded(m_Expanded);
}

} // namespace GameEngine

