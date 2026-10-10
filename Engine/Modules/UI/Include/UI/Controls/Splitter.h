#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"
#include "UI/ResolvedStyle.h"
#include "Types/ColorUtils.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/UIEvents.h"

namespace GameEngine {

class Splitter : public UIElement {
public:
    Splitter() = default;

    void OnEvent(UIEvent& e) override {
        if (e.Id == kEventMouseDown) {
            BeginDrag(e);
        } else if (e.Id == kEventMouseMove) {
            UpdateDrag(e);
        } else if (e.Id == kEventMouseUp) {
            EndDrag(e);
        }
    }

    void GetHitTestBounds(float& outX, float& outY, float& outW, float& outH) const override
    {
        constexpr float kHitSize = 6.0f;
        const bool isRow = HasClass("row");
        
        if (isRow)
        {
            // Row splitter: expand horizontally
            outX = GetLayoutX() - kHitSize * 0.5f;
            outY = GetLayoutY();
            outW = kHitSize;
            outH = GetLayoutHeight();
        }
        else
        {
            // Column splitter: expand vertically
            outX = GetLayoutX();
            outY = GetLayoutY() - kHitSize * 0.5f;
            outW = GetLayoutWidth();
            outH = kHitSize;
        }
    }

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override
    {
        uint32_t bg = style.Visual.BackgroundColor;
        if (((bg >> 24) & 0xFF) == 0) return;

        constexpr float kLineSize = 4.0f;
        const bool isRow = HasClass("row");

        // The box is laid out at the line's own width (theme/core.css), so the
        // line fills it rather than overhanging into the panes on either side.
        // Centre it in the box for the case where the two ever disagree.
        float lx, ly, lW, lH;
        if (isRow) {
            const float boxW = w > 0.0f ? w : GetLayoutWidth();
            lx = x + (boxW - kLineSize) * 0.5f;
            ly = y;
            lW = kLineSize;
            lH = h > 0.0f ? h : GetLayoutHeight();
        } else {
            const float boxH = h > 0.0f ? h : GetLayoutHeight();
            lx = x;
            ly = y + (boxH - kLineSize) * 0.5f;
            lW = w > 0.0f ? w : GetLayoutWidth();
            lH = kLineSize;
        }

        uint32_t fill = UI::PackFromARGB(bg);
        auto prim = UI::MakeRect(lx, ly, lW, lH, fill);
        prim.Opacity *= style.Visual.Opacity;
        ctx.Emit(prim);
    }

private:
    static float ResolveMinMainAxisPx(const LayoutInputs& layout, bool isRow)
    {
        const StyleLength& len = isRow ? layout.MinWidth : layout.MinHeight;
        if (len.IsPx())
            return std::max(0.0f, len.Value);
        // Dock splits and older UXML often omit min sizes; keep a modest default for auto/%.
        static constexpr float kDefaultMinPanePx = 48.0f;
        return kDefaultMinPanePx;
    }

    void BeginDrag(UIEvent& e) {
        UIElement* p = GetParent();
        if (!p) return;
        // Expect layout: [WeightedPane A][Splitter][WeightedPane B]
        const auto& ch = p->GetChildren();
        if (ch.size() < 3) return;
        m_PaneA = dynamic_cast<WeightedPane*>(ch[0].get());
        m_PaneB = dynamic_cast<WeightedPane*>(ch[2].get());
        if (!m_PaneA || !m_PaneB) return;

        m_IsRow = HasClass("row");
        m_StartMouseX = e.X; m_StartMouseY = e.Y;
        m_StartAPx = m_IsRow ? m_PaneA->GetLayoutWidth() : m_PaneA->GetLayoutHeight();
        m_StartBPx = m_IsRow ? m_PaneB->GetLayoutWidth() : m_PaneB->GetLayoutHeight();
        m_HandlePx = m_IsRow ? GetLayoutWidth() : GetLayoutHeight();
        // Space shared by the two panes = parent main axis minus this splitter (not wA+wB, which can
        // disagree by a pixel or two and skew ratio vs Yoga's flex distribution).
        {
            const float parentMain = m_IsRow ? p->GetLayoutWidth() : p->GetLayoutHeight();
            const float splitMain = m_HandlePx;
            const float fromParent = std::max(0.0f, parentMain - splitMain);
            const float fromChildren = m_StartAPx + m_StartBPx;
            m_AvailPx = (fromParent > 1e-3f) ? fromParent : fromChildren;
        }
        // Prefer each pane's resolved min on the split axis (e.g. min-width: 0). Fallback only when
        // the style is auto/percent so docks still get a sensible minimum.
        m_MinAPx = ResolveMinMainAxisPx(m_PaneA->GetResolvedStyle().Layout, m_IsRow);
        m_MinBPx = ResolveMinMainAxisPx(m_PaneB->GetResolvedStyle().Layout, m_IsRow);

        // Extract split path from id: "split:<path>"
        m_SplitPath.clear();
        const std::string& id = GetId();
        const std::string prefix = "split:";
        if (id.rfind(prefix, 0) == 0) m_SplitPath = id.substr(prefix.size());

        // Find Dockspace ancestor to update model on release
        m_Dockspace = nullptr;
        for (UIElement* q = p; q; q = q->GetParent()) {
            if (auto* ds = dynamic_cast<DockspaceElement*>(q)) { m_Dockspace = ds; break; }
        }

        // Drive the drag from flex-grow weights, not from measured pixels. Measured widths can differ
        // slightly from wa/(wa+wb) (rounding, borders); snapping weights to pixels on the first drag
        // update caused a visible jump when movement crossed the commit threshold.
        {
            const float wa = m_PaneA->GetFlexWeight();
            const float wb = m_PaneB->GetFlexWeight();
            const float wSum = wa + wb;
            m_InitialRatio = (wSum > 1e-6f) ? (wa / wSum) : 0.5f;
        }
        m_LastRatio = m_InitialRatio;
        // Do not SetFlexWeight here: applying weights + relayout on mouse down makes the split jump
        // (often to the right) before the user moves. First ratio update happens in UpdateDrag.
        m_Dragging = true;
        e.Capture(this);
        e.Stop();
    }

    void UpdateDrag(UIEvent& e) {
        if (!m_Dragging || !m_PaneA || !m_PaneB) return;
        float delta = m_IsRow ? (e.X - m_StartMouseX) : (e.Y - m_StartMouseY);
        // Measured pixel widths can disagree slightly with Yoga flex-grow weights (rounding, mins).
        // Applying SetFlexWeight from pixels when delta is ~0 re-snaps the split and looks like a
        // jump on mouse down. Only commit weights once the user has moved meaningfully.
        constexpr float kDragCommitThresholdPx = 0.5f;
        if (std::fabs(delta) < kDragCommitThresholdPx)
            return;

        if (m_AvailPx <= 1e-3f)
            return;

        // Same A-size bounds as before, expressed in ratio space (see UpdateDrag pixel clamp below).
        const float minAPx = m_MinAPx;
        const float maxAPx = std::max(m_MinAPx, m_AvailPx - m_MinBPx);
        const float minRatio = minAPx / m_AvailPx;
        const float maxRatio = maxAPx / m_AvailPx;

        float ratio = m_InitialRatio + delta / m_AvailPx;
        ratio = std::clamp(ratio, minRatio, maxRatio);
        m_LastRatio = ratio;

        m_PaneA->SetFlexWeight(ratio);
        m_PaneB->SetFlexWeight(1.0f - ratio);
        if (auto* p = GetParent())
        {
            p->MarkDirty(LayoutDirty);
            // Splitter drags change layout; request relayout so virtualized views
            // reflow during the drag (not just on mouse up).
            p->RequestRelayout();
        }
        e.Stop();
    }

    void EndDrag(UIEvent& e) {
        if (!m_Dragging) return;
        m_Dragging = false;
        if (m_Dockspace && m_Dockspace->GetModel() && !m_SplitPath.empty())
        {
            constexpr float kEpsilon = 1e-4f;
            if (std::abs(m_LastRatio - m_InitialRatio) > kEpsilon)
                m_Dockspace->GetModel()->SetSplitRatioByPath(m_SplitPath, m_LastRatio);
        }
        e.Stop();
    }

private:
    bool m_Dragging = false;
    bool m_IsRow = true;
    float m_StartMouseX = 0.0f;
    float m_StartMouseY = 0.0f;
    float m_StartAPx = 0.0f;
    float m_StartBPx = 0.0f;
    float m_AvailPx = 0.0f;
    float m_HandlePx = 0.0f;
    float m_MinAPx = 100.0f;
    float m_MinBPx = 100.0f;
    float m_LastRatio = 0.5f;
    float m_InitialRatio = 0.5f;
    std::string m_SplitPath;
    WeightedPane* m_PaneA = nullptr;
    WeightedPane* m_PaneB = nullptr;
    DockspaceElement* m_Dockspace = nullptr;
};

} // namespace GameEngine

