#pragma once

#include <string>
#include <vector>
#include <functional>
#include "UI/UIElement.h"

namespace GameEngine {

class Foldout;
class AccordionItem;

// Accordion control that manages multiple collapsible sections using Foldouts.
// Users define sections using AccordionItem children in XML. The Accordion
// automatically creates and manages Foldout instances for each item.
// 
// Features:
// - Exclusive mode: only one section can be expanded at a time
// - Multiple mode: any number of sections can be expanded
// - Automatic first-item expansion by default
//
// Usage in XML:
//   <Accordion>
//     <AccordionItem title="Section 1" expanded="true">
//       <Label>Content 1</Label>
//     </AccordionItem>
//     <AccordionItem title="Section 2">
//       <Label>Content 2</Label>
//     </AccordionItem>
//   </Accordion>
class Accordion : public UIElement
{
public:
    enum class Mode
    {
        Exclusive,  // Only one section can be open at a time
        Multiple    // Multiple sections can be open simultaneously
    };

    Accordion();
    ~Accordion() override = default;

    // Mode: exclusive (one at a time) or multiple
    void SetMode(Mode mode);
    Mode GetMode() const { return m_Mode; }

    // Whether to auto-expand the first item if no items are explicitly expanded
    void SetAutoExpandFirst(bool autoExpand);
    bool GetAutoExpandFirst() const { return m_AutoExpandFirst; }

    // Expand a specific section by index
    void ExpandSection(int index);
    
    // Collapse a specific section by index
    void CollapseSection(int index);
    
    // Collapse all sections
    void CollapseAll();

    // Callback when a section is expanded/collapsed
    using SectionChangedHandler = std::function<void(Accordion&, int index, bool expanded)>;
    void SetOnSectionChanged(SectionChangedHandler handler) { m_OnSectionChanged = std::move(handler); }

    // Called after layout to build internal foldouts from AccordionItem children
    void OnPostLayout() override;

protected:
    void BuildFoldoutsFromItems();
    void OnFoldoutExpandedChanged(Foldout& foldout, bool expanded);
    int FindFoldoutIndex(const Foldout* foldout) const;

private:
    Mode m_Mode = Mode::Multiple;
    bool m_AutoExpandFirst = true;
    bool m_Built = false;
    
    std::vector<Foldout*> m_Foldouts;
    SectionChangedHandler m_OnSectionChanged;
};

} // namespace GameEngine

