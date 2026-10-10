#include "UI/Controls/Accordion.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/AccordionItem.h"
#include "../UIAttributeAccess.h"

namespace GameEngine {

Accordion::Accordion()
{
    AddClass("accordion");
    UIAttributeAccess::SetSelectorAttribute(*this, "mode", "multiple", /*markDirty=*/false);
    RequestSubtreeStyleAssetPath("UI/controls/Accordion.css", "editor");
}

void Accordion::SetMode(Mode mode)
{
    if (m_Mode == mode)
        return;
    m_Mode = mode;
    UIAttributeAccess::SetSelectorAttribute(*this, "mode", mode == Mode::Exclusive ? "exclusive" : "multiple");
}

void Accordion::SetAutoExpandFirst(bool autoExpand)
{
    m_AutoExpandFirst = autoExpand;
}

void Accordion::ExpandSection(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Foldouts.size()))
        return;
    
    if (m_Mode == Mode::Exclusive)
    {
        // Collapse all other sections first
        for (int i = 0; i < static_cast<int>(m_Foldouts.size()); ++i)
        {
            if (i != index && m_Foldouts[i])
                m_Foldouts[i]->SetExpanded(false);
        }
    }
    
    if (m_Foldouts[index])
        m_Foldouts[index]->SetExpanded(true);
}

void Accordion::CollapseSection(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Foldouts.size()))
        return;
    if (m_Foldouts[index])
        m_Foldouts[index]->SetExpanded(false);
}

void Accordion::CollapseAll()
{
    for (auto* foldout : m_Foldouts)
    {
        if (foldout)
            foldout->SetExpanded(false);
    }
}

void Accordion::OnPostLayout()
{
    UIElement::OnPostLayout();
    BuildFoldoutsFromItems();
}

void Accordion::BuildFoldoutsFromItems()
{
    if (m_Built)
        return;
    
    m_Built = true;
    m_Foldouts.clear();
    
    bool hasAnyExpanded = false;
    std::vector<AccordionItem*> items;
    
    // Collect AccordionItem children
    for (const auto& child : GetChildren())
    {
        auto* item = dynamic_cast<AccordionItem*>(child.get());
        if (item)
        {
            items.push_back(item);
            if (item->IsExpanded())
                hasAnyExpanded = true;
        }
    }
    
    // Build foldouts for each item
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto* item = items[i];
        auto foldout = std::make_unique<Foldout>();
        Foldout* foldoutPtr = foldout.get();
        
        foldout->SetTitle(item->GetTitle());
        foldout->SetIconClass(item->GetIconClass());
        
        // Determine initial expanded state
        bool shouldExpand = item->IsExpanded();
        if (!hasAnyExpanded && m_AutoExpandFirst && i == 0)
            shouldExpand = true;
        foldout->SetExpanded(shouldExpand);
        
        // Move item's children to foldout content container
        UIElement* content = foldout->GetContentContainer();
        if (content)
        {
            // Collect raw pointers first since TakeChild modifies the children list
            std::vector<UIElement*> childPtrs;
            for (const auto& child : item->GetChildren())
                childPtrs.push_back(child.get());

            for (UIElement* childPtr : childPtrs)
            {
                if (auto taken = item->TakeChild(childPtr))
                    content->AddChild(std::move(taken));
            }
        }
        
        // Set up change callback
        foldoutPtr->SetOnExpandedChanged([this](Foldout& f, bool expanded) {
            OnFoldoutExpandedChanged(f, expanded);
        });
        
        item->SetInternalFoldout(foldoutPtr);
        m_Foldouts.push_back(foldoutPtr);
        
        // Add foldout as child of accordion (after the AccordionItem)
        AddChild(std::move(foldout));
        
        // Hide the original AccordionItem
        item->AddClass("hidden");
    }
}

void Accordion::OnFoldoutExpandedChanged(Foldout& foldout, bool expanded)
{
    int index = FindFoldoutIndex(&foldout);
    
    if (expanded && m_Mode == Mode::Exclusive)
    {
        // Collapse all other sections
        for (int i = 0; i < static_cast<int>(m_Foldouts.size()); ++i)
        {
            if (i != index && m_Foldouts[i] && m_Foldouts[i]->IsExpanded())
                m_Foldouts[i]->SetExpanded(false);
        }
    }
    
    if (m_OnSectionChanged)
        m_OnSectionChanged(*this, index, expanded);
}

int Accordion::FindFoldoutIndex(const Foldout* foldout) const
{
    for (size_t i = 0; i < m_Foldouts.size(); ++i)
    {
        if (m_Foldouts[i] == foldout)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace GameEngine

