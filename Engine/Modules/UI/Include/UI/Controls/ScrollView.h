#pragma once

#include <algorithm>
#include <functional>

#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManagerRef.h"

namespace GameEngine
{

class Scrollbar;

// Minimal ScrollView container.
// - Owns an internal clip viewport and a scroll-content root that holds user content
// - Maintains horizontal/vertical scroll offsets
// - Scrollbars are regular child elements; visibility is controlled by the control
class ScrollView : public UIElement
{
  public:
    ScrollView();
    ~ScrollView() override;

    // Maintains UIManager's typed scroll-view registry (replaces the
    // per-frame dynamic_cast tree walk in RebuildTypedNodeLists).
    void OnOwnerManagerChanged(UIManager* owner) override;

    // Viewport holds user content; use this to add children to the scrollable area.
    // Note: This returns the scroll-content element (not the clip viewport).
    void AddContent(std::unique_ptr<UIElement> child);

    // Access viewport (read-only)
    UIElement* GetViewport() const { return m_Viewport; }
    // Access the internal clip viewport element (useful for controls that need
    // to compute visible rects/clipping regions). This is the parent of the
    // scroll-content viewport.
    UIElement* GetClipViewport() const { return m_ClipViewport; }
    UIElement* GetHorizontalScrollbar() const;
    UIElement* GetVerticalScrollbar() const;

    // Horizontal scrolling API (used by single-line TextField)
    float GetScrollX() const { return m_ScrollX; }
    void SetScrollX(float x) const
    {
        // IMPORTANT: compute clamp range using the same live extents that
        // Scrollbar uses for thumb math. Using cached m_ContentW/m_ViewportW can
        // lag a frame behind Yoga/layout updates and makes the scrollbar feel
        // like it is “fighting” during drag.
        float maxX = std::max(0.0f, GetContentWidth() - GetViewportWidth());
        float next = std::clamp(x, 0.0f, maxX);
        if (next != m_ScrollX)
        {
            m_ScrollX = next;
            UpdateContentTransform();
            // Defer callbacks to UIManager so virtualization updates can run at a safe
            // point (outside event dispatch) and so multiple scroll changes coalesce.
            m_ScrollChangedPending = true;
            OnScrollOffsetChanged(/*xChanged=*/true, /*yChanged=*/false);
            NotifyOwnerScrollOffsetsChanged();
        }
    }
    void ScrollBy(float dx, float dy) const
    {
        SetScrollX(m_ScrollX + dx);
        SetScrollY(m_ScrollY + dy);
    }

    // Viewport/content size (for content-aware scrollbars)
    float GetViewportWidth() const
    {
        // Prefer live layout rect when available so scrollbars have stable metrics
        // even on frames where OnPostLayout didn't run (no Yoga solve).
        if (m_ClipViewport)
            return std::max(0.0f, m_ClipViewport->GetLayoutWidth());
        return m_ViewportW;
    }
    float GetViewportHeight() const
    {
        if (m_ClipViewport)
            return std::max(0.0f, m_ClipViewport->GetLayoutHeight());
        return m_ViewportH;
    }
    void SetViewportSize(float w, float h) const
    {
        const float nextW = std::max(0.0f, w);
        const float nextH = std::max(0.0f, h);
        if (m_ViewportW == nextW && m_ViewportH == nextH)
            return;
        m_ViewportW = nextW;
        m_ViewportH = nextH;
        MarkHorizontalMeasureDirty();
        ClampScroll();
        UpdateScrollbarVisibility();
    }

    float GetContentWidth() const
    {
        // When a control sets an explicit content size (e.g. TextArea, virtualized lists),
        // treat it as authoritative. Using the scroll-content Yoga layout size as a fallback
        // can introduce instability when child controls temporarily expand their own layout
        // rects for hit testing, causing overscroll and visible “jump” as ClampScroll corrects.
        if (m_HasExplicitContentSize)
            return std::max(0.0f, m_ContentW);

        // Otherwise ensure we report at least the Yoga layout width; m_ContentW may
        // be expanded beyond layout width for horizontal scrolling.
        float base = 0.0f;
        if (m_Viewport)
            base = std::max(0.0f, m_Viewport->GetLayoutWidth());
        return std::max(m_ContentW, base);
    }
    float GetContentHeight() const
    {
        if (m_HasExplicitContentSize)
            return std::max(0.0f, m_ContentH);

        float base = 0.0f;
        if (m_Viewport)
            base = std::max(0.0f, m_Viewport->GetLayoutHeight());
        return std::max(m_ContentH, base);
    }
    void SetContentSize(float w, float h) const
    {
        const float nextW = std::max(0.0f, w);
        const float nextH = std::max(0.0f, h);
        if (m_ContentW == nextW && m_ContentH == nextH)
            return;
        m_ContentW = nextW;
        m_ContentH = nextH;
        m_HasExplicitContentSize = true;
        MarkHorizontalMeasureDirty();
        ClampScroll();
        UpdateScrollbarVisibility();
    }
    // Clears the explicit content-size override so ScrollView derives extents
    // from Yoga layout again. Most callers don't need this; virtualized controls
    // typically keep content size explicit.
    void ClearExplicitContentSize() const
    {
        if (!m_HasExplicitContentSize)
            return;
        m_HasExplicitContentSize = false;
        // Next OnPostLayout will refresh m_ContentW/H from layout.
        MarkHorizontalMeasureDirty();
        ClampScroll();
        UpdateScrollbarVisibility();
    }

    // Mark cached horizontal overflow measurement dirty. This is called when the
    // scroll-content subtree is mutated so we only re-measure label widths when
    // it can actually change.
    void MarkHorizontalMeasureDirty() const
    {
        m_HorizontalMeasureDirty = true;
    }

    // Fires when the horizontal bar is actually shown or hidden, so a host can
    // move chrome that shares the bar's strip. Visibility is decided by content
    // vs viewport, not by whether horizontal scrolling was enabled.
    void SetOnHorizontalBarVisibilityChanged(std::function<void(bool)> cb) const
    {
        m_OnHorizontalBarVisibilityChanged = std::move(cb);
    }
    // Content/viewport extents (updated by UIManager and controls)
    mutable float m_ViewportW = 0.0f, m_ViewportH = 0.0f;
    mutable float m_ContentW = 0.0f, m_ContentH = 0.0f;
    // When true, m_ContentW/H are controlled explicitly by the host/control (virtualization).
    // When false, OnPostLayout derives content extents from the scroll-content Yoga layout.
    mutable bool m_HasExplicitContentSize = false;

    // Cached horizontal overflow measurement to avoid scanning the full subtree
    // every frame (critical for editor idle FPS).
    mutable std::function<void(bool)> m_OnHorizontalBarVisibilityChanged;
    mutable bool m_HorizontalMeasureDirty = true;
    mutable float m_HorizontalMeasureViewportW = -1.0f;
    mutable float m_HorizontalMeasureMaxW = 0.0f;

    void ClampScroll() const
    {
        const float contentW = GetContentWidth();
        const float contentH = GetContentHeight();
        const float viewportW = GetViewportWidth();
        const float viewportH = GetViewportHeight();
        float maxX = std::max(0.0f, contentW - viewportW);
        float maxY = std::max(0.0f, contentH - viewportH);
        float prevX = m_ScrollX;
        float prevY = m_ScrollY;
        m_ScrollX = std::clamp(m_ScrollX, 0.0f, maxX);
        m_ScrollY = std::clamp(m_ScrollY, 0.0f, maxY);
        if (m_ScrollX != prevX || m_ScrollY != prevY)
        {
            UpdateContentTransform();
            m_ScrollChangedPending = true;
            OnScrollOffsetChanged(/*xChanged=*/(m_ScrollX != prevX), /*yChanged=*/(m_ScrollY != prevY));
            NotifyOwnerScrollOffsetsChanged();
        }
    }

    // Ensure a content-space X position is visible within the viewport rect
    // contentX: absolute content pixel X (e.g., caret X in content coordinates)
    // viewLeft: absolute pixel X of viewport left edge
    // viewWidth: viewport width in pixels
    void EnsureXVisible(float contentX, float viewLeft, float viewWidth) const;

    // Vertical scrolling (for future TextArea)
    float GetScrollY() const { return m_ScrollY; }
    void SetScrollY(float y) const
    {
        float maxY = std::max(0.0f, GetContentHeight() - GetViewportHeight());
        float next = std::clamp(y, 0.0f, maxY);
        if (next != m_ScrollY)
        {
            m_ScrollY = next;
            UpdateContentTransform();
            m_ScrollChangedPending = true;
            OnScrollOffsetChanged(/*xChanged=*/false, /*yChanged=*/true);
            NotifyOwnerScrollOffsetsChanged();
        }
    }

    // Scroll change callback - called when scroll position changes
    void SetOnScrollChanged(std::function<void(float, float)> callback)
    {
        m_OnScrollChanged = std::move(callback);
        // Ensure newly registered listeners (virtualized controls) get a chance to
        // observe the current scroll offsets, even if the offsets were restored or
        // set before the callback was attached (common during startup).
        //
        // This is deferred and coalesced: UIManager will flush it at a safe point.
        // The owner notify keeps the pending flush visible to the pointer-only
        // frame gate — FlushScrollCallbacks runs only in the heavy pass, and this
        // is the one pending-setter that raises no other signal.
        m_ScrollChangedPending = true;
        NotifyOwnerScrollOffsetsChanged();
    }
    bool HasPendingScrollChanged() const { return m_ScrollChangedPending; }

    // NO SetScrollWithoutNotify, and none is needed — the deferral already is one.
    //
    // Field<T> needs SetValueWithoutNotify because its notification is synchronous: a handler
    // calling SetValue would notify from inside a notification. The scroll offset does not
    // work that way. A setter only raises m_ScrollChangedPending, and the flush below clears it
    // BEFORE it delivers, so a handler writing an offset from inside the flush sets the flag
    // again for the NEXT flush instead of re-entering this one. Re-entrancy is structurally
    // impossible here, and a without-notify variant would be an API with no problem to solve.

    // Not const: this dispatches an event, so it can run arbitrary subscriber code. The
    // scroll STATE it reads is still mutable-and-const-written by the setters above; what
    // changed is that observing the flush is no longer a read-only act.
    bool FlushPendingScrollChanged()
    {
        if (!m_ScrollChangedPending)
            return false;
        m_ScrollChangedPending = false;

        // Everything below runs subscriber code, so raise the dispatch depth: without it
        // UIElement::RemoveChild does NOT defer, and a handler removing this ScrollView would
        // destroy the object the rest of this function runs on. Every caller needs this, so it
        // lives here rather than at the call sites; UIManager::FlushScrollCallbacks raises it
        // again across its whole loop, which is a different invariant (the raw pointers to
        // ScrollViews the loop has not reached yet). Nesting is what the depth counter is for.
        //
        // It also defers virtualization pool SHRINK out of the flush, because the virtualized
        // views pass `allowShrink = !IsInEventDispatch()` from this very callback — the
        // conservative direction, and the behaviour VirtualWindowCore.h already documented.
        // See the fuller note at UIManager::FlushScrollCallbacks.
        UIElement::SetInEventDispatch(true);
        struct DispatchDepthScope
        {
            ~DispatchDepthScope() { UIElement::SetInEventDispatch(false); }
        } depthScope;
        // Member first, then the handler table — Button::TriggerClick's order. The member
        // is the last-writer-wins slot virtualized controls own; the table is where any
        // number of subscribers coexist without displacing it or each other.
        if (m_OnScrollChanged)
            m_OnScrollChanged(m_ScrollX, m_ScrollY);

        UIEvent e{};
        e.Id = kEventScrollOffsetChanged;
        e.Target = this;
        e.CurrentTarget = this;
        e.ScrollX = m_ScrollX;
        e.ScrollY = m_ScrollY;
        DispatchEvent<kEventScrollOffsetChanged>(e);
        return true;
    }

    // Ensure a content-space Y position is visible within the viewport rect
    // contentY: absolute content pixel Y (top-left origin space)
    // viewTop: absolute pixel Y of viewport top edge
    // viewHeight: viewport height in pixels
    void EnsureYVisible(float contentY, float viewTop, float viewHeight) const;

    // Post-layout hook: compute viewport/content extents and update scrollbar
    // visibility based on the most recent Yoga layout.
    void OnPostLayout() override;

    // Event hook so UIManager can route scroll-wheel events via UI.Scroll
    // without knowing about ScrollView specifically.
    void OnEvent(UIEvent& e) override;

    // Observe layout of the internal viewport so we can keep viewport size
    // and scroll ranges up to date without UIManager directly poking at us.
    void OnScrollViewportLayout(UIElement* viewport, float W, float H) override;

  private:
    void UpdateContentTransform() const;
    void UpdateScrollbarVisibility() const;
    void OnScrollOffsetChanged(bool xChanged, bool yChanged) const;
    void NotifyOwnerScrollOffsetsChanged() const;

    // Manager whose scroll-view registry currently holds this control —
    // re-homing/destruction must deregister from the OLD manager, which
    // OnOwnerManagerChanged(owner) alone can't name.
    UIManagerRef m_RegisteredScrollManager;

    UIElement* m_ClipViewport = nullptr; // not owning; has class "scroll-viewport" (clip)
    UIElement* m_Viewport = nullptr;     // not owning; has class "scroll-content" (user content root)
    Scrollbar* m_HBar = nullptr;         // optional horizontal bar element (layout only)
    Scrollbar* m_VBar = nullptr;         // optional vertical bar element (layout only)

    // Mark scroll offsets mutable so we can adjust in const rendering flows when needed
    mutable float m_ScrollX = 0.0f;
    mutable float m_ScrollY = 0.0f;

    // Callback for scroll position changes
    mutable std::function<void(float, float)> m_OnScrollChanged;
    // Pending callback flag (coalesced).
    mutable bool m_ScrollChangedPending = false;
};

} // namespace GameEngine

