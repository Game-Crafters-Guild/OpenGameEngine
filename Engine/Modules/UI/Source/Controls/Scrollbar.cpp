#include "UI/Controls/Scrollbar.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <algorithm>

namespace GameEngine {

namespace
{
class ScrollbarThumbElement final : public UIElement
{
  public:
    ScrollbarThumbElement()
    {
        AddClass("scrollbar-thumb");
        constexpr float kDefaultThumbRadius = 4.0f;
        Overrides()
            .Set(Style::Position, PositionType::Absolute)
            .Set(Style::FlexGrow, 0.0f)
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::PointerEvents, false)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{kDefaultThumbRadius, kDefaultThumbRadius, kDefaultThumbRadius, kDefaultThumbRadius})
            .Set(Style::BorderWidth, Box4{});
    }
};
} // namespace

Scrollbar::Scrollbar(Orientation o)
    : m_Orientation(o)
{
    AddClass("scrollbar");
    if (o == Orientation::Horizontal)
        AddClass("horizontal");
    else
        AddClass("vertical");

    Overrides()
        .Set(Style::Position, PositionType::Relative)
        .Set(Style::PositionLeft, StyleLength::Auto())
        .Set(Style::PositionTop, StyleLength::Auto())
        .Set(Style::PositionRight, StyleLength::Auto())
        .Set(Style::PositionBottom, StyleLength::Auto())
        .Set(Style::FlexGrow, 0.0f)
        .Set(Style::FlexShrink, 0.0f);

    if (o == Orientation::Vertical)
    {
        Overrides()
            .Set(Style::Width, StyleLength::Px(m_ThicknessPx))
            .Set(Style::MinWidth, StyleLength::Px(m_ThicknessPx));
    }
    else
    {
        Overrides()
            .Set(Style::Height, StyleLength::Px(m_ThicknessPx))
            .Set(Style::MinHeight, StyleLength::Px(m_ThicknessPx));
    }

    auto thumb = std::make_unique<ScrollbarThumbElement>();
    m_Thumb = thumb.get();
    AddChild(std::move(thumb));
}

bool Scrollbar::UpdateThumbLayout() const
{
    if (!m_Thumb)
        return false;

    const ScrollView* sv = FindScrollView();
    if (!sv)
        return false;

    const float barX = GetLayoutX();
    const float barY = GetLayoutY();
    const float barW = GetLayoutWidth();
    const float barH = GetLayoutHeight();
    if (barW <= 0.0f || barH <= 0.0f)
        return false;

    float scroll = 0.0f;
    float viewport = 0.0f, content = 0.0f;
    float trackLen = (m_Orientation == Orientation::Horizontal) ? barW : barH;
    if (m_Orientation == Orientation::Horizontal)
    {
        scroll = sv->GetScrollX();
        viewport = sv->GetViewportWidth();
        content = sv->GetContentWidth();
    }
    else
    {
        scroll = sv->GetScrollY();
        viewport = sv->GetViewportHeight();
        content = sv->GetContentHeight();
    }

    float thumbLen = trackLen; // default: full length when not scrollable
    float pos = 0.0f;
    if (viewport > 0.0f && content > viewport)
    {
        // Size thumb by viewport/content ratio, clamp to a minimum size.
        thumbLen = std::max(20.0f, trackLen * (viewport / content));
        float usable = std::max(0.0f, trackLen - thumbLen);
        float maxScroll = std::max(0.0f, content - viewport);
        float t = (maxScroll > 0.0f) ? std::clamp(scroll / maxScroll, 0.0f, 1.0f) : 0.0f;
        pos = t * usable;
    }
    else if (!IsHidden())
    {
        // Non-scrollable metrics while the bar is visible are a transient
        // (mid-convergence layout rects, e.g. during a hover-lift relayout).
        // Snapping the thumb to full-track paints as the grabber vanishing
        // for a frame; keep the last thumb rect until the metrics settle —
        // if the content genuinely stopped overflowing, the visibility pass
        // hides the whole bar this same frame.
        return false;
    }

    constexpr float kThumbInset = 2.0f;

    float nextX = barX;
    float nextY = barY;
    float nextW = barW;
    float nextH = barH;
    if (m_Orientation == Orientation::Horizontal)
    {
        nextX = barX + pos;
        nextY = barY + kThumbInset;
        nextW = thumbLen;
        nextH = std::max(0.0f, barH - kThumbInset * 2.0f);
    }
    else
    {
        nextX = barX + kThumbInset;
        nextY = barY + pos;
        nextW = std::max(0.0f, barW - kThumbInset * 2.0f);
        nextH = thumbLen;
    }

    const bool moved =
        (m_Thumb->GetLayoutX() != nextX) ||
        (m_Thumb->GetLayoutY() != nextY) ||
        (m_Thumb->GetLayoutWidth() != nextW) ||
        (m_Thumb->GetLayoutHeight() != nextH);

    if (moved)
    {
        // Pure moves take the position-only fast path — layout dirt here turns
        // every scroll tick into a Yoga solve and kills drain-only scrolling.
        // The direct rect commit below closes the fast path's stale window
        // (its post-solve patch never runs on quiet frames).
        const bool thumbSizeChanged =
            m_Thumb->GetLayoutWidth() != nextW || m_Thumb->GetLayoutHeight() != nextH;
        UI::Layout::SetAbsolutePosition(*m_Thumb,
                                        Mathematics::Rect{nextX - barX, nextY - barY, nextW, nextH},
                                        /*positionOnlyFastPath=*/!thumbSizeChanged);
        UILayoutAccess::SetLastLayoutRect(*m_Thumb, nextX, nextY, nextW, nextH);
        m_Thumb->MarkDirty(UIElement::VisualDirty);
    }
    return moved;
}

void Scrollbar::OnEvent(UIEvent& e)
{
    // Only respond when the event is targeting this scrollbar in the bubble
    // chain.
    if (e.CurrentTarget != this)
        return;

    if (e.Id == kEventMouseDown)
    {
        // Only react to primary button presses.
        if (e.Button != 0)
            return;

        UIManager* owner = GetOwnerManager();
        if (!owner)
            return;

        float x = GetLayoutX();
        float y = GetLayoutY();
        float W = GetLayoutWidth();
        float H = GetLayoutHeight();
        if (W <= 0.0f || H <= 0.0f)
            return;

        ResolvedStyle style{};
        OnPointerDown(e.X, e.Y, x, y, W, H, style);

        // Capture the mouse so drag continues even if the pointer leaves the
        // scrollbar bounds.
        e.Capture(this);
        e.Stop();
    }
    else if (e.Id == kEventMouseMove)
    {
        // m_Dragging is the scrollbar's own authoritative drag state: set in
        // OnPointerDown, cleared by kEventMouseUp (delivered via capture).
        // Gate on that instead of IsMouseDown so transient UIManager state
        // (e.g. a spurious secondary-button event resetting m_MouseDown) can't
        // silently terminate an active drag.
        if (!m_Dragging)
            return;

        float x = GetLayoutX();
        float y = GetLayoutY();
        float W = GetLayoutWidth();
        float H = GetLayoutHeight();
        if (W <= 0.0f || H <= 0.0f)
            return;

        ResolvedStyle style{};
        OnPointerDrag(e.X, e.Y, x, y, W, H, style);
        e.Stop();
    }
    else if (e.Id == kEventMouseUp || e.Id == kEventMouseCancel)
    {
        // Stop dragging when the mouse button is released, or when the gesture is
        // cancelled because the pointer left the surface. Capture is released by
        // UIManager after bubbling the event.
        m_Dragging = false;
        RemoveClass("active");
        e.Stop();
    }
}

void Scrollbar::OnPointerDown(float mouseX, float mouseY,
                              float x, float y, float W, float H,
                              const ResolvedStyle& /*style*/) {
    // If the user clicks the track (outside the thumb), jump the thumb so its center
    // aligns with the click, then begin dragging from that new position.
    //
    // This matches common editor scrollbar UX and makes large navigation quicker.
    if (auto* sv = FindScrollViewMutable())
    {
        float scroll = 0.0f;
        float viewport = 0.0f, content = 0.0f;
        float trackLen = (m_Orientation == Orientation::Horizontal) ? W : H;
        float clickPos = (m_Orientation == Orientation::Horizontal) ? (mouseX - x) : (mouseY - y);

        if (m_Orientation == Orientation::Horizontal)
        {
            scroll = sv->GetScrollX();
            viewport = sv->GetViewportWidth();
            content = sv->GetContentWidth();
        }
        else
        {
            scroll = sv->GetScrollY();
            viewport = sv->GetViewportHeight();
            content = sv->GetContentHeight();
        }

        // Not scrollable: nothing to do.
        if (!(viewport > 0.0f && content > viewport) || trackLen <= 0.0f)
        {
            m_Dragging = false;
            return;
        }

        const float maxScroll = std::max(0.0f, content - viewport);
        float thumbLen = std::max(20.0f, trackLen * (viewport / content));
        float usable = std::max(0.0f, trackLen - thumbLen);
        float t0 = (maxScroll > 0.0f) ? std::clamp(scroll / maxScroll, 0.0f, 1.0f) : 0.0f;
        float thumbPos = t0 * usable;

        // If click is outside the thumb, jump scroll so thumb center is at click.
        const bool clickInThumb = (clickPos >= thumbPos && clickPos <= (thumbPos + thumbLen));
        if (!clickInThumb && usable > 0.0f)
        {
            float targetThumbPos = std::clamp(clickPos - (thumbLen * 0.5f), 0.0f, usable);
            float t = targetThumbPos / usable;
            float targetScroll = t * maxScroll;
            if (m_Orientation == Orientation::Horizontal)
                sv->SetScrollX(targetScroll);
            else
                sv->SetScrollY(targetScroll);

            // Start dragging from the new scroll position.
            scroll = targetScroll;
        }

        m_Dragging = true;
        AddClass("active");
        m_DragStartMouse = (m_Orientation == Orientation::Horizontal) ? mouseX : mouseY;
        m_DragStartScroll = scroll;
        return;
    }

    m_Dragging = false;
}

void Scrollbar::OnPointerDrag(float mouseX, float mouseY,
                              float x, float y, float W, float H,
                              const ResolvedStyle& /*style*/) {
    (void)x; (void)y;

    if (!m_Dragging) return;
    float delta = (m_Orientation == Orientation::Horizontal) ? (mouseX - m_DragStartMouse) : (mouseY - m_DragStartMouse);
    if (auto* sv = FindScrollViewMutable()) {
        float viewport = 0.0f, content = 0.0f;
        float trackLen = (m_Orientation == Orientation::Horizontal) ? W : H;
        if (m_Orientation == Orientation::Horizontal) { viewport = sv->GetViewportWidth(); content = sv->GetContentWidth(); }
        else { viewport = sv->GetViewportHeight(); content = sv->GetContentHeight(); }
        float thumbLen = trackLen;
        float usable = 0.0f;
        if (viewport > 0.0f && content > viewport) {
            thumbLen = std::max(20.0f, trackLen * (viewport / content));
            usable = std::max(0.0f, trackLen - thumbLen);
        }
        float maxScroll = std::max(0.0f, content - viewport);
        float dScroll = (usable > 0.0f) ? (delta * (maxScroll / usable)) : 0.0f;
        float target = m_DragStartScroll + dScroll;
        if (m_Orientation == Orientation::Horizontal) sv->SetScrollX(target);
        else sv->SetScrollY(target);
    }
}

} // namespace GameEngine
