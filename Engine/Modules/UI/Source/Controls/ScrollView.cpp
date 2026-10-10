#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/Controls/Label.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "Input/KeyCodes.h"
#include "Rendering/Text/FontAtlas.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <typeinfo>
#include <vector>

namespace GameEngine {

namespace {

// Internal layout elements for ScrollView. These enforce core behavior via
// typed style overrides (no dependency on external theme CSS).

static float MeasureMultilineMaxWidth(Rendering::Text::FontAtlas* font,
                                      std::string_view text,
                                      float pixelSize, float letterSpacingPx)
{
    if (!font || text.empty())
        return 0.0f;

    float maxW = 0.0f;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t nl = text.find('\n', start);
        const size_t len = (nl == std::string::npos) ? (text.size() - start) : (nl - start);
        std::string_view line(text.data() + start, len);
        auto m = font->MeasureText(line, pixelSize, letterSpacingPx);
        maxW = std::max(maxW, m.metrics.width);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return maxW;
}

class ScrollRowElement final : public UIElement
{
public:
    ScrollRowElement()
    {
        Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::FlexGrow, 1.0f)
            .Set(Style::FlexShrink, 1.0f)
            .Set(Style::MinWidth, StyleLength::Px(0.0f))
            .Set(Style::MinHeight, StyleLength::Px(0.0f));
    }
};

class ScrollClipViewportElement final : public UIElement
{
public:
    ScrollClipViewportElement()
    {
        Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::FlexGrow, 1.0f)
            .Set(Style::FlexShrink, 1.0f)
            .Set(Style::OverflowProp, Overflow::Hidden)
            .Set(Style::MinWidth, StyleLength::Px(0.0f))
            .Set(Style::MinHeight, StyleLength::Px(0.0f));
    }
};

class ScrollContentElement final : public UIElement
{
public:
    explicit ScrollContentElement(const ScrollView* owner) : m_Owner(owner)
    {
        Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::FlexGrow, 0.0f)
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::MinWidth, StyleLength::Px(0.0f))
            .Set(Style::MinHeight, StyleLength::Px(0.0f));
    }

    void AddChild(std::unique_ptr<UIElement> child) override
    {
        UIElement::AddChild(std::move(child));
        if (m_Owner)
            m_Owner->MarkHorizontalMeasureDirty();
    }
    void RemoveChild(UIElement* child) override
    {
        UIElement::RemoveChild(child);
        if (m_Owner)
            m_Owner->MarkHorizontalMeasureDirty();
    }
    std::unique_ptr<UIElement> TakeChild(UIElement* child) override
    {
        auto owned = UIElement::TakeChild(child);
        if (m_Owner)
            m_Owner->MarkHorizontalMeasureDirty();
        return owned;
    }

private:
    const ScrollView* m_Owner = nullptr;
};

bool IsScrollbarInTargetChain(UIElement* target)
{
    for (UIElement* p = target; p; p = p->GetParent())
    {
        if (dynamic_cast<Scrollbar*>(p) != nullptr)
            return true;
    }
    return false;
}

} // namespace

ScrollView::~ScrollView()
{
    if (UIManager* registered = m_RegisteredScrollManager.Get())
        registered->UnregisterScrollView(this);
}

void ScrollView::OnOwnerManagerChanged(UIManager* owner)
{
    if (UIManager* registered = m_RegisteredScrollManager.Get())
        registered->UnregisterScrollView(this);
    m_RegisteredScrollManager = UIManagerRef(owner);
    if (owner)
        owner->RegisterScrollView(this);
}

ScrollView::ScrollView()
{
    AddClass("scrollview");
    RequestSubtreeStyleAssetPath("UI/controls/ScrollView.css", "editor");
    // `display` is declared in that sheet, not here: an application stylesheet
    // has to be able to hide a whole scroller with a state class, and an
    // override outranks every rule. The rest is the scroller's own layout
    // structure -- a column that fills its parent and may shrink below its
    // content -- which no stylesheet is meant to restate.
    Overrides()
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::FlexShrink, 1.0f)
        .Set(Style::MinWidth, StyleLength::Px(0.0f))
        .Set(Style::MinHeight, StyleLength::Px(0.0f));
    // Layout structure:
    // this (column)
    //  ├── row (scroll-row)
    //  │    ├── viewport (scroll-viewport)
    //  │    └── vertical scrollbar
    //  └── horizontal scrollbar

    // Row container to hold viewport + vertical bar
    auto row = std::make_unique<ScrollRowElement>();
    row->AddClass("scroll-row");

    // Create a clip viewport (used for clipping) and an inner content root that holds user content.
    auto vpClip = std::make_unique<ScrollClipViewportElement>();
    vpClip->AddClass("scroll-viewport");
    m_ClipViewport = vpClip.get();

    auto vpContent = std::make_unique<ScrollContentElement>(this);
    vpContent->AddClass("scroll-content");
    m_Viewport = vpContent.get();

    vpClip->AddChild(std::move(vpContent));
    row->AddChild(std::move(vpClip));

    // Vertical scrollbar (sits to the right of viewport)
    auto vb = std::make_unique<Scrollbar>(Scrollbar::Orientation::Vertical);
    m_VBar = vb.get();
    m_VBar->SetHidden(true); // show only when needed
    row->AddChild(std::move(vb));

    // Append the row to this
    AddChild(std::move(row));

    // Horizontal scrollbar sits below
    auto hb = std::make_unique<Scrollbar>(Scrollbar::Orientation::Horizontal);
    m_HBar = hb.get();
    m_HBar->SetHidden(true); // show only when needed
    AddChild(std::move(hb));
}

void ScrollView::AddContent(std::unique_ptr<UIElement> child) {
    if (m_Viewport) m_Viewport->AddChild(std::move(child));
}

UIElement* ScrollView::GetHorizontalScrollbar() const
{
    return m_HBar;
}

UIElement* ScrollView::GetVerticalScrollbar() const
{
    return m_VBar;
}

void ScrollView::UpdateContentTransform() const
{
    if (!m_Viewport)
        return;

    // Scroll offsets are applied by UIManager as a post-layout translation of
    // the scroll-content subtree (avoids Yoga solves during scrolling).
    //
    // IMPORTANT: Do NOT mark the subtree VisualDirty here. UIManager has a
    // dedicated "positions-only" path that patches per-element offsets in-place
    // (and translates cached text vertices) without regenerating all UI geometry.
}

void ScrollView::UpdateScrollbarVisibility() const
{
    if (!m_HBar || !m_VBar)
        return;

    const float vw = GetViewportWidth();
    const float vh = GetViewportHeight();
    const float cw = GetContentWidth();
    const float ch = GetContentHeight();

    // Optional theme override: allow a ScrollView instance to force scrollbars visible.
    // Useful for editor panels where users expect to always see the scroll thumb.
    bool forceH = false;
    bool forceV = false;
    bool suppressH = false;
    bool suppressV = false;
    // IMPORTANT:
    // Some editor subsystems (e.g. LogPanel) can update controls even when the
    // panel is not currently mounted in the active UI tree. In that case, the
    // element does not participate in the current UIManager::Update, and its
    // cached resolved-style pointer can be stale (frame-local, non-owning).
    // Never dereference resolved styles unless we are attached to a UIManager.
    if (GetOwnerManager() != nullptr)
    {
        const ResolvedStyle& rs = GetResolvedStyle();
        if (rs.GetCustomNumber(HashStringId("--ui_scrollbar_force_horizontal")).value_or(0.0f) > 0.0f)
            forceH = true;
        if (rs.GetCustomNumber(HashStringId("--ui_scrollbar_force_vertical")).value_or(0.0f) > 0.0f)
            forceV = true;

        if (rs.Layout.OverflowX == Overflow::Hidden)
            suppressH = true;
        if (rs.Layout.OverflowY == Overflow::Hidden)
            suppressV = true;
    }

    // Epsilon against scrollbar flicker from fractional Yoga sizes, rounding
    // differences, and small transient extent changes (e.g. a hover-lift
    // animation moving content by ±3px). A bar toggle is expensive feedback:
    // it reserves thickness, so flipping reflows the content sideways.
    // Content within the epsilon of the viewport simply doesn't get a bar.
    constexpr float kScrollEpsPx = 4.0f;
    // If we don't have meaningful extents yet (first layout), keep bars hidden.
    if (!forceH && !forceV && (vw <= 1.0f || vh <= 1.0f))
    {
        m_HBar->SetHidden(true);
        m_VBar->SetHidden(true);
        return;
    }

    // Compare in rounded pixel space to avoid false-positive scrollbars from
    // tiny fractional differences (DPI scaling, Yoga rounding).
    const int vwPx = (int)std::lround(vw);
    const int vhPx = (int)std::lround(vh);
    const int cwPx = (int)std::lround(cw);
    const int chPx = (int)std::lround(ch);
    const int vThickPx = (int)std::lround(m_VBar->GetThicknessPx());
    const int hThickPx = (int)std::lround(m_HBar->GetThicknessPx());

    const int epsPx = (int)std::lround(kScrollEpsPx);
    // Compute the "no scrollbars" viewport in pixel space using the current
    // layout's clip viewport size plus whatever thickness is currently being
    // reserved by visible bars. This avoids oscillation/flicker where enabling
    // one scrollbar reduces the viewport enough to require the other, and vice versa.
    const int baseVwPx = vwPx + (m_VBar->IsHidden() ? 0 : std::max(0, vThickPx));
    const int baseVhPx = vhPx + (m_HBar->IsHidden() ? 0 : std::max(0, hThickPx));

    bool needH = forceH;
    bool needV = forceV;
    for (int iter = 0; iter < 3; ++iter)
    {
        const int effVhPx = std::max(0, baseVhPx - (needH ? std::max(0, hThickPx) : 0));

        const bool nextNeedV = forceV || (chPx > effVhPx + epsPx);
        // IMPORTANT:
        // Don't create a horizontal scrollbar solely because a vertical scrollbar became visible.
        //
        // Many scrollable controls (especially virtualized lists/grids/trees) drive their content width
        // to match the viewport width. When a vertical scrollbar appears, the *effective* viewport width
        // shrinks by the bar thickness. If we compare content width against that reduced width, we can
        // erroneously enable a horizontal scrollbar even though the content would naturally reflow/shrink
        // to the new viewport size on the next layout.
        //
        // Instead, decide horizontal overflow relative to the "no-scrollbars" width.
        const bool nextNeedH = forceH || (cwPx > baseVwPx + epsPx);
        if (nextNeedV == needV && nextNeedH == needH)
            break;
        needV = nextNeedV;
        needH = nextNeedH;
    }

    if (suppressH)
        needH = false;
    if (suppressV)
        needV = false;

    const bool hideHorizontal = !needH;
    const bool hideVertical = !needV;
    const bool horizontalVisibilityChanged = (m_HBar->IsHidden() != hideHorizontal);
    const bool verticalVisibilityChanged = (m_VBar->IsHidden() != hideVertical);

    m_HBar->SetHidden(hideHorizontal);
    m_VBar->SetHidden(hideVertical);

    if (horizontalVisibilityChanged && m_OnHorizontalBarVisibilityChanged)
        m_OnHorizontalBarVisibilityChanged(!hideHorizontal);

    // A scrollbar display flip changes the clip viewport's available size.
    // Percentage-sized descendants below scroll-content must participate in
    // the convergence retry as well; otherwise Yoga can retain their result
    // from the pre-flip width while flex siblings absorb the entire delta.
    if ((horizontalVisibilityChanged || verticalVisibilityChanged) && m_Viewport)
    {
        if (UIManager* ui = GetOwnerManager())
            ui->InvalidateRetainedLayout(m_Viewport);
        else
            m_Viewport->RequestRelayout();
    }
}

void ScrollView::OnScrollOffsetChanged(bool xChanged, bool yChanged) const
{
    // Scroll offsets are applied by UIManager as a post-layout translation of the
    // scroll-content subtree (no Yoga solve). Keep scrollbar thumbs in sync by
    // updating their layout rects directly (no geometry rebuild needed).
    if (xChanged && m_HBar)
        (void)m_HBar->UpdateThumbLayout();
    if (yChanged && m_VBar)
        (void)m_VBar->UpdateThumbLayout();
}

void ScrollView::NotifyOwnerScrollOffsetsChanged() const
{
    if (UIManager* ui = GetOwnerManager())
        ui->NotifyScrollOffsetsChanged();
}

void ScrollView::EnsureXVisible(float contentX, float viewLeft, float viewWidth) const {
    // Keep caret/content point within [viewLeft + scrollX, viewLeft + scrollX + viewWidth]
    float left = viewLeft + m_ScrollX;
    float right = left + viewWidth;
    if (contentX < left) {
        SetScrollX(viewLeft > contentX ? 0.0f : (contentX - viewLeft));
    } else if (contentX > right) {
        SetScrollX(contentX - (viewLeft + viewWidth));
    }
}

void ScrollView::EnsureYVisible(float contentY, float viewTop, float viewHeight) const {
    // Keep content point within [viewTop + scrollY, viewTop + scrollY + viewHeight]
    float top = viewTop + m_ScrollY;
    float bottom = top + viewHeight;
    if (contentY < top) {
        SetScrollY(viewTop > contentY ? 0.0f : (contentY - viewTop));
    } else if (contentY > bottom) {
        SetScrollY(contentY - (viewTop + viewHeight));
    }
}
	
	void ScrollView::OnEvent(UIEvent& e)
	{
    if (e.Id == kEventScroll && e.CurrentTarget == this)
    {
        if (Input::IsPrimaryShortcutModifier(e.Mods))
        {
            const ResolvedStyle& rs = GetResolvedStyle();
            if (rs.GetCustomNumber(HashStringId("--ui_scrollview_primary_modifier_passthrough")).value_or(0.0f) > 0.0f)
            {
                return; // let parent controls handle primary-modifier scroll
            }
        }

        // Simple pixel-based scrolling; callers convert wheel deltas into pixels
        // before raising UI.Scroll (see UIManager::OnScroll()).
        float dx = e.ScrollX;
        float dy = e.ScrollY;
        if (dx == 0.0f && dy == 0.0f)
            return;

        const float maxX = std::max(0.0f, GetContentWidth() - GetViewportWidth());
        const float maxY = std::max(0.0f, GetContentHeight() - GetViewportHeight());

        // UX: if vertical wheel scrolling is requested but the view cannot scroll vertically,
        // fall back to horizontal scrolling when horizontal overflow exists. This matches common
        // editor behavior and keeps wheel scrolling useful for single-line content (e.g. TextField).
        // When the pointer is over a scrollbar, do not remap vertical wheel to horizontal scroll —
        // that axis swap feels like a wrong scroll direction on the scrollbar.
        if (!IsScrollbarInTargetChain(e.Target) && dx == 0.0f && dy != 0.0f && maxY <= 0.0f && maxX > 0.0f)
        {
            dx = dy;
            dy = 0.0f;
        }
        if (maxX <= 0.0f)
            dx = 0.0f;
        if (maxY <= 0.0f)
            dy = 0.0f;
        // A view that cannot move in the requested direction leaves the tick to
        // its ancestors, which is the same answer it gives when a view with range
        // is already at its end: consumed means moved.
        if (dx == 0.0f && dy == 0.0f)
            return;

        const float prevX = m_ScrollX;
        const float prevY = m_ScrollY;
        ScrollBy(dx, dy);
        if (m_ScrollX != prevX || m_ScrollY != prevY)
        {
            e.Stop();
        }
        return;
    }
	
	    UIElement::OnEvent(e);
	}
	
	void ScrollView::OnScrollViewportLayout(UIElement* viewport, float W, float H)
	{
	    if (viewport != m_ClipViewport)
	        return;
	
	    SetViewportSize(W, H);
    // When content size is not driven explicitly, keep extents in sync with layout.
    // Virtualized controls (GridView/ListView/TreeView) typically call SetContentSize(...)
    // themselves; in that case, clobbering m_ContentW/H here causes scrollbar flicker
    // during resize/layout convergence.
    if (!m_HasExplicitContentSize && m_Viewport)
    {
        // NOTE: This will set m_HasExplicitContentSize=true via SetContentSize(),
        // so prefer to update the cached fields directly in this path.
        m_ContentW = std::max(0.0f, m_Viewport->GetLayoutWidth());
        m_ContentH = std::max(0.0f, m_Viewport->GetLayoutHeight());
    }
    UpdateScrollbarVisibility();
	}

    void ScrollView::OnPostLayout()
    {
        // The clip viewport is the visible region; the scroll-content root holds
        // the user content. Both layout rects are updated by UIManager before
        // calling OnPostLayout().
        if (m_ClipViewport)
        {
            m_ViewportW = std::max(0.0f, m_ClipViewport->GetLayoutWidth());
            m_ViewportH = std::max(0.0f, m_ClipViewport->GetLayoutHeight());
        }
        if (m_Viewport)
        {
            if (!m_HasExplicitContentSize)
            {
                m_ContentW = std::max(0.0f, m_Viewport->GetLayoutWidth());
                m_ContentH = std::max(0.0f, m_Viewport->GetLayoutHeight());
            }
        }

        // Horizontal scrolling: Yoga layout often stretches children to the
        // viewport width, which means long label lines can be clipped without
        // increasing the scroll-content width. To make horizontal scrollbars
        // appear when they should, expand contentW based on the measured width
        // of leaf <label> elements.
        // Horizontal overflow measurement:
        // By default, enable label width scanning so long single-line labels can expand the scroll
        // extents and show a horizontal scrollbar when appropriate.
        //
        // Panels that prefer clipped labels (common in editor UIs) can disable this by setting:
        //   --ui_scrollview_measure_horizontal: 0
        bool allowHorizontalMeasure = true;
        {
            const ResolvedStyle& rs = GetResolvedStyle();
            if (auto v = rs.GetCustomNumber(HashStringId("--ui_scrollview_measure_horizontal")))
                allowHorizontalMeasure = (*v > 0.0f);
        }

        // Horizontal label-width measurement is a heuristic to enable horizontal scrollbars when Yoga
        // stretches children to the viewport width. When a control drives scroll extents explicitly
        // (virtualized lists/grids/trees), this heuristic is not meaningful and can cause viewport
        // height to oscillate as the visible window changes.
        if (allowHorizontalMeasure && !m_HasExplicitContentSize && m_Viewport && m_ClipViewport &&
            m_ViewportW > 0.0f && m_ContentW <= m_ViewportW + 0.5f)
        {
            // If the viewport width changes, prior horizontal-measure results are no longer valid.
            if (m_HorizontalMeasureViewportW < 0.0f || std::abs(m_ViewportW - m_HorizontalMeasureViewportW) > 0.5f)
            {
                m_HorizontalMeasureDirty = true;
            }

            if (UIManager* ui = GetOwnerManager())
            {
                if (m_HorizontalMeasureDirty)
                {
                    m_HorizontalMeasureViewportW = m_ViewportW;
                    m_HorizontalMeasureMaxW = m_ContentW;

                    const float rootX = m_Viewport->GetLayoutX();
                    float maxW = m_ContentW;

                    std::vector<UIElement*> stack;
                    stack.reserve(64);
                    stack.push_back(m_Viewport);
                    while (!stack.empty())
                    {
                        UIElement* el = stack.back();
                        stack.pop_back();
                        if (!el)
                            continue;

                        // Only consider labels; editable fields manage their own scrolling.
                        if (typeid(*el) == typeid(Label))
                        {
                            const ResolvedStyle* st = ui ? ui->TryGetResolvedStyleFor(el) : nullptr;
                            if (st)
                            {
                                if (!st->Visual.Visible || st->Layout.DisplayMode == DisplayMode::None)
                                    continue;

                                auto* font = ui->ResolveFontForStyle(el->GetResolvedStyle());
                                const float px = std::max(1.0f, st->Visual.FontSize);
                                std::string fallback = el->GetTextContent();
                                std::string_view textView = fallback;
                                float textW = MeasureMultilineMaxWidth(font, textView, px,
                                                                       st->Visual.LetterSpacing);
                                // Compared against GetLayoutWidth below, which is
                                // the border box, so both insets belong here.
                                float insetW = st->Layout.Padding.Left + st->Layout.Padding.Right +
                                               st->Layout.BorderWidth.Left + st->Layout.BorderWidth.Right;
                                float xOff = el->GetLayoutX() - rootX;
                                float elW = el->GetLayoutWidth();
                                float fullW = textW + insetW;
                                float availableW = m_ViewportW - xOff;
                                if (fullW > elW && elW > 0.0f && elW < availableW - 1.0f)
                                    fullW = elW;
                                maxW = std::max(maxW, xOff + fullW);
                            }
                        }

                        for (const auto& ch : el->GetChildren())
                            stack.push_back(ch.get());
                    }

                    m_HorizontalMeasureMaxW = std::max(m_HorizontalMeasureMaxW, maxW);
                    m_HorizontalMeasureDirty = false;
                }
            }

            // Apply cached measurement (if any) without re-scanning.
            m_ContentW = std::max(m_ContentW, m_HorizontalMeasureMaxW);
        }

        // NOTE: ClampScroll is intentionally NOT called here. OnPostLayout fires
        // during each pass of UIManager::ConvergePostLayout, where intermediate
        // Yoga passes can produce transient layout rects. Clamping against those
        // would destroy the scroll position. UIManager calls ClampScroll on all
        // ScrollViews after convergence settles (via ClampAllScrollOffsets).

        // Update bar visibility. This may toggle the 'hidden' class on the
        // scrollbar elements, marking them StyleDirty so UIManager can re-apply
        // CSS and re-run Yoga layout when needed.
        UpdateScrollbarVisibility();

        // Ensure thumb rects are up to date for this frame (heavy path assigns layout rects
        // before OnPostLayout; we derive thumb position/size from live ScrollView metrics).
        if (m_HBar)
            (void)m_HBar->UpdateThumbLayout();
        if (m_VBar)
            (void)m_VBar->UpdateThumbLayout();
    }

} // namespace GameEngine
