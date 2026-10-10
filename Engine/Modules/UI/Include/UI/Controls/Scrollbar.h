#pragma once

#include "UI/UIElement.h"
#include "UI/Controls/ScrollView.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"
#include "UI/UIEvents.h"

namespace GameEngine {

class Scrollbar : public UIElement {
public:
    enum class Orientation { Horizontal, Vertical };

    explicit Scrollbar(Orientation o);

    static constexpr float kDefaultThicknessPx = 12.0f;

    void SetHidden(bool hidden)
    {
        if (m_Hidden == hidden)
            return;
        m_Hidden = hidden;
        if (hidden)
            Overrides().Set(Style::Display, DisplayMode::None);
        else
            Overrides().Reset(Style::Display);
        MarkDirty(StyleDirty | LayoutDirty | VisualDirty);
    }
    bool IsHidden() const { return m_Hidden; }

    void SetThicknessPx(float px)
    {
        if (px < 0.0f)
            px = 0.0f;
        if (m_ThicknessPx == px)
            return;
        m_ThicknessPx = px;
        if (m_Orientation == Orientation::Vertical)
        {
            Overrides()
                .Set(Style::Width, StyleLength::Px(px))
                .Set(Style::MinWidth, StyleLength::Px(px));
        }
        else
        {
            Overrides()
                .Set(Style::Height, StyleLength::Px(px))
                .Set(Style::MinHeight, StyleLength::Px(px));
        }
        MarkDirty(StyleDirty | LayoutDirty | VisualDirty);
    }
    float GetThicknessPx() const { return m_ThicknessPx; }

    // Update the thumb element's last layout rect based on current ScrollView metrics.
    // Returns true if the thumb moved (position or size changed).
    bool UpdateThumbLayout() const;

    UIElement* GetThumbElement() const { return m_Thumb; }

    // Event-driven pointer interactions for dragging the thumb. These are
    // driven by UI.MouseDown/UI.MouseMove/UI.MouseUp events in OnEvent.
    void OnEvent(UIEvent& e) override;

    void OnPointerDown(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style);
    void OnPointerDrag(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style);

private:
    const ScrollView* FindScrollView() const {
        const UIElement* p = GetParent();
        while (p) {
            if (auto sv = dynamic_cast<const ScrollView*>(p)) return sv;
            p = p->GetParent();
        }
        return nullptr;
    }

    ScrollView* FindScrollViewMutable() const {
        UIElement* p = const_cast<UIElement*>(static_cast<const UIElement*>(this));
        while (p) {
            if (auto sv = dynamic_cast<ScrollView*>(p)) return sv;
            p = p->GetParent();
        }
        return nullptr;
    }

    Orientation m_Orientation;
    bool m_Hidden = false;
    float m_ThicknessPx = kDefaultThicknessPx;
    UIElement* m_Thumb = nullptr; // not owning; child element (thumb)
    mutable bool m_Dragging = false;
    mutable float m_DragStartMouse = 0.0f;
    mutable float m_DragStartScroll = 0.0f;
};

} // namespace GameEngine

