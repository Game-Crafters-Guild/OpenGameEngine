#pragma once

#include "UI/UIElement.h"

namespace GameEngine {

// A simple container element that exposes a programmatic flex-grow weight.
// UIManager's Yoga builder recognizes this element and applies the weight
// to the corresponding YG node so panes can share space by ratio.
class WeightedPane : public UIElement {
public:
    explicit WeightedPane(float weight = 1.0f)
        : m_Weight(weight) {}

    UIElementKind Kind() const override { return UIElementKind::WeightedPane; }

    void SetFlexWeight(float w) { m_Weight = (w < 0.0f) ? 0.0f : w; MarkDirty(LayoutDirty); }
    float GetFlexWeight() const { return m_Weight; }

private:
    float m_Weight = 1.0f; // relative weight (used as Yoga flex-grow)
};

} // namespace GameEngine

