#pragma once

#include <string>
#include "UI/UIElement.h"

namespace GameEngine {

class Foldout;

// AccordionItem is a declarative wrapper for defining accordion sections.
// It holds title, icon, and content information that the parent Accordion
// uses to create and manage Foldout instances internally.
// Users place their content as children of AccordionItem in XML.
class AccordionItem : public UIElement
{
public:
    AccordionItem();
    ~AccordionItem() override = default;

    // Title displayed in the foldout header
    void SetTitle(const std::string& title);
    const std::string& GetTitle() const { return m_Title; }

    // Optional icon class for the foldout header
    void SetIconClass(const std::string& iconClass);
    const std::string& GetIconClass() const { return m_IconClass; }

    // Initial expanded state (default: first item expanded, others collapsed)
    void SetExpanded(bool expanded);
    bool IsExpanded() const { return m_Expanded; }

    // Internal foldout created by parent Accordion (not user-accessible)
    void SetInternalFoldout(Foldout* foldout) { m_InternalFoldout = foldout; }
    Foldout* GetInternalFoldout() const { return m_InternalFoldout; }

    // Called by Accordion to sync properties to internal foldout
    void SyncToFoldout();

private:
    std::string m_Title;
    std::string m_IconClass;
    bool m_Expanded = false;
    bool m_ExpandedExplicitlySet = false;
    
    Foldout* m_InternalFoldout = nullptr;
};

} // namespace GameEngine

