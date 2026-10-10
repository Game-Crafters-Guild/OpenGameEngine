#include "UI/Controls/DockTabBar.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIPrimitive.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"

namespace GameEngine {

DockTabBar::DockTabBar()
{
    auto leftButton = std::make_unique<UIElement>();
    m_ScrollLeftButton = leftButton.get();
    leftButton->AddClass("dock-tab-scroll");
    leftButton->AddClass("dock-tab-scroll-left");
    leftButton->AddClass("hidden");
    leftButton->SetTooltip("Scroll tabs left");
    leftButton->SetFocusable(false);
    leftButton->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
    {
        if (e.Button != 0)
            return;
        ScrollByPage(-1);
        e.Stop();
    });
    AddChild(std::move(leftButton));

    auto rightButton = std::make_unique<UIElement>();
    m_ScrollRightButton = rightButton.get();
    rightButton->AddClass("dock-tab-scroll");
    rightButton->AddClass("dock-tab-scroll-right");
    rightButton->AddClass("hidden");
    rightButton->SetTooltip("Scroll tabs right");
    rightButton->SetFocusable(false);
    rightButton->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
    {
        if (e.Button != 0)
            return;
        ScrollByPage(1);
        e.Stop();
    });
    AddChild(std::move(rightButton));
}

void DockTabBar::AddTabChild(std::unique_ptr<UIElement> tab)
{
    const size_t controlCount = static_cast<size_t>(m_ScrollLeftButton != nullptr) +
                                static_cast<size_t>(m_ScrollRightButton != nullptr);
    const size_t insertIndex = GetChildren().size() >= controlCount
        ? GetChildren().size() - controlCount
        : GetChildren().size();
    InsertChild(insertIndex, std::move(tab));
}

float DockTabBar::GetTabsWidth() const
{
    float width = 1.0f;
    size_t tabCount = 0;
    for (const auto& child : GetChildren())
    {
        if (!child || !child->HasClass("tab"))
            continue;
        width += child->GetLayoutWidth();
        ++tabCount;
    }
    if (tabCount > 1)
        width += kTabGapPx * static_cast<float>(tabCount - 1);
    return width;
}

float DockTabBar::GetMaxScrollOffset() const
{
    const float tabsWidth = GetTabsWidth();
    if (tabsWidth <= GetLayoutWidth() + 0.5f)
        return 0.0f;
    const float viewportWidth = std::max(0.0f, GetLayoutWidth() - kOverflowControlsWidthPx);
    return std::max(0.0f, tabsWidth - viewportWidth);
}

void DockTabBar::ApplyScrollOffset()
{
    for (const auto& child : GetChildren())
    {
        if (!child || !child->HasClass("tab"))
            continue;
        child->Overrides().Set(Style::PositionLeft, StyleLength::Px(-m_ScrollOffset));
        child->MarkDirty(LayoutDirty | VisualDirty);
    }
}

void DockTabBar::SetScrollOffset(float offset)
{
    const float clamped = std::clamp(offset, 0.0f, GetMaxScrollOffset());
    if (std::abs(clamped - m_ScrollOffset) < 0.1f)
        return;
    m_ScrollOffset = clamped;
    ApplyScrollOffset();
    MarkDirty(LayoutDirty | VisualDirty);
}

void DockTabBar::ScrollByPage(int direction)
{
    if (direction == 0 || !IsOverflowing())
        return;
    const float viewportWidth = std::max(0.0f, GetLayoutWidth() - kOverflowControlsWidthPx);
    const float step = std::max(48.0f, viewportWidth * 0.75f);
    SetScrollOffset(m_ScrollOffset + (direction < 0 ? -step : step));
}

void DockTabBar::OnPostLayout()
{
    const float maxScroll = GetMaxScrollOffset();
    const bool overflowing = maxScroll > 0.5f;
    for (UIElement* button : {m_ScrollLeftButton, m_ScrollRightButton})
    {
        if (!button)
            continue;
        if (overflowing)
            button->RemoveClass("hidden");
        else
            button->AddClass("hidden");
    }
    if (m_ScrollOffset > maxScroll)
        SetScrollOffset(maxScroll);
    if (m_ScrollLeftButton)
    {
        if (m_ScrollOffset <= 0.5f)
            m_ScrollLeftButton->AddClass("at-limit");
        else
            m_ScrollLeftButton->RemoveClass("at-limit");
    }
    if (m_ScrollRightButton)
    {
        if (m_ScrollOffset >= maxScroll - 0.5f)
            m_ScrollRightButton->AddClass("at-limit");
        else
            m_ScrollRightButton->RemoveClass("at-limit");
    }
    if (overflowing && !m_HadOverflow)
    {
        for (const auto& child : GetChildren())
        {
            if (child && child->HasClass("tab") && child->HasClass("active"))
            {
                constexpr const char* kTabIdPrefix = "tab:";
                const std::string& id = child->GetId();
                if (id.rfind(kTabIdPrefix, 0) == 0)
                    EnsureTabVisible(id.substr(std::char_traits<char>::length(kTabIdPrefix)));
                break;
            }
        }
    }
    m_HadOverflow = overflowing;
}

void DockTabBar::EnsureTabVisible(const std::string& panelId)
{
    if (!IsOverflowing())
    {
        SetScrollOffset(0.0f);
        return;
    }

    float left = 1.0f;
    const float viewportWidth = std::max(0.0f, GetLayoutWidth() - kOverflowControlsWidthPx);
    for (const auto& child : GetChildren())
    {
        if (!child || !child->HasClass("tab"))
            continue;
        const float right = left + child->GetLayoutWidth();
        if (child->GetId() == std::string("tab:") + panelId)
        {
            if (left < m_ScrollOffset)
                SetScrollOffset(left);
            else if (right > m_ScrollOffset + viewportWidth)
                SetScrollOffset(right - viewportWidth);
            return;
        }
        left = right + kTabGapPx;
    }
}

void DockTabBar::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseDown && e.Button == 2 && IsOverflowing())
    {
        m_MiddlePanning = true;
        m_MiddlePanStartX = e.X;
        m_MiddlePanStartOffset = m_ScrollOffset;
        AddClass("middle-panning");
        e.Capture(this);
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseMove && m_MiddlePanning)
    {
        SetScrollOffset(m_MiddlePanStartOffset - (e.X - m_MiddlePanStartX));
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseUp && e.Button == 2 && m_MiddlePanning)
    {
        m_MiddlePanning = false;
        RemoveClass("middle-panning");
        e.Stop();
        return;
    }

}

static void ShiftSubtree(UIElement* el, float dx) {
    for (const auto& child : el->GetChildren()) {
        UIElement* c = child.get();
        UILayoutAccess::SetLastLayoutRect(*c, c->GetLayoutX() + dx, c->GetLayoutY(),
                             c->GetLayoutWidth(), c->GetLayoutHeight());
        ShiftSubtree(c, dx);
    }
}

void DockTabBar::SetInsertionIndicator(int index, float xLocalPx) {
    m_InsertActive = true;
    m_InsertIndex = index;
    m_InsertX = xLocalPx;
    MarkDirty(VisualDirty);
}

void DockTabBar::ClearInsertionIndicator() {
    if (m_InsertActive) { m_InsertActive = false; MarkDirty(VisualDirty); }
}

void DockTabBar::SetReorderVisual(int fromIdx, int toIdx, float draggedWidth, UIElement* draggedEl) {
    m_ReorderActive = true;
    m_FromIndex = fromIdx;
    m_ToIndex = toIdx;
    m_DraggedWidth = std::max(0.0f, draggedWidth);
    m_Dragged = draggedEl;
    const auto& kids = GetChildren();
    const size_t n = kids.size();
    if (m_TargetOffset.size() != n) {
        m_TargetOffset.assign(n, 0.0f);
        m_CurOffset.resize(n, 0.0f);
    }
    for (size_t k = 0; k < n; ++k) {
        UIElement* el = kids[k].get();
        if (!el->HasClass("tab")) { m_TargetOffset[k] = 0.0f; continue; }
        if (el == m_Dragged) { m_TargetOffset[k] = 0.0f; continue; }
        if (m_ToIndex > m_FromIndex) {
            m_TargetOffset[k] = (k > (size_t)m_FromIndex && k < (size_t)m_ToIndex) ? -m_DraggedWidth : 0.0f;
        } else if (m_ToIndex < m_FromIndex) {
            m_TargetOffset[k] = (k >= (size_t)m_ToIndex && k < (size_t)m_FromIndex) ? +m_DraggedWidth : 0.0f;
        } else {
            m_TargetOffset[k] = 0.0f;
        }
    }
    MarkDirty(VisualDirty);
}

void DockTabBar::ClearReorderVisual() {
    if (!m_ReorderActive) return;
    m_ReorderActive = false;
    m_FromIndex = m_ToIndex = -1;
    m_Dragged = nullptr;
    for (auto& v : m_TargetOffset) v = 0.0f;
    MarkDirty(VisualDirty);
}

float DockTabBar::GetAnimatedOffsetForChild(const UIElement* child) const {
    const auto& kids = GetChildren();
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].get() == child) {
            if (i < m_CurOffset.size()) return m_CurOffset[i];
            break;
        }
    }
    return 0.0f;
}

float DockTabBar::GetBaseXForChild(const UIElement* child) const {
    if (!m_LiveDragTab || m_BaseX.empty())
        return child->GetLayoutX();
    const auto& kids = GetChildren();
    for (size_t i = 0; i < kids.size() && i < m_BaseX.size(); ++i) {
        if (kids[i].get() == child)
            return m_BaseX[i];
    }
    return child->GetLayoutX();
}

void DockTabBar::BeginLiveDrag(UIElement* draggedTab, float grabOffsetX) {
    m_LiveDragTab = draggedTab;
    m_LiveGrabOffsetX = grabOffsetX;

    // Snapshot base layout positions
    const auto& kids = GetChildren();
    const size_t n = kids.size();
    m_BaseX.resize(n);
    m_AnimOffsets.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i)
        m_BaseX[i] = kids[i]->GetLayoutX();
}

void DockTabBar::UpdateLiveDrag(float mouseX) {
    if (!m_LiveDragTab) return;

    const float barX = GetLayoutX();
    const float barW = GetLayoutWidth();
    const float tabW = m_LiveDragTab->GetLayoutWidth();

    float desiredX = mouseX - m_LiveGrabOffsetX;
    desiredX = std::max(barX, std::min(barX + barW - tabW, desiredX));

    // Set offset for dragged tab
    const auto& kids = GetChildren();
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].get() == m_LiveDragTab) {
            if (i < m_BaseX.size() && i < m_AnimOffsets.size())
                m_AnimOffsets[i] = desiredX - m_BaseX[i];
            break;
        }
    }

    MarkDirty(VisualDirty);
}

void DockTabBar::EndLiveDrag() {
    if (!m_LiveDragTab) return;

    // Rebuild contiguous positions from the current child order instead of
    // restoring m_BaseX by index. A live reorder changes which tab occupies an
    // index, and tabs can have different widths, so the cached index positions
    // can otherwise leave a persistent gap after mouse-up.
    const auto& kids = GetChildren();
    float curX = GetLayoutX() + GetLayoutPadding().Left - m_ScrollOffset;
    for (const auto& childPtr : kids) {
        UIElement* child = childPtr.get();
        if (!child->HasClass("tab"))
            continue;
        const float dx = curX - child->GetLayoutX();
        UILayoutAccess::SetLastLayoutRect(*child, curX, child->GetLayoutY(),
                                 child->GetLayoutWidth(), child->GetLayoutHeight());
        if (dx != 0.0f)
            ShiftSubtree(child, dx);
        curX += child->GetLayoutWidth() + kTabGapPx;
    }

    m_LiveDragTab = nullptr;
    m_LiveGrabOffsetX = 0.0f;
    m_BaseX.clear();
    m_AnimOffsets.clear();
    MarkDirty(VisualDirty | LayoutDirty);
}

void DockTabBar::NotifyReorderSwap() {
    // Called after RequestTabReorder swapped children and set new sequential layout positions.
    // Recompute base positions and set offsets so non-dragged tabs animate from old visual pos.
    const auto& kids = GetChildren();
    const size_t n = kids.size();

    // Save old visual positions (base + offset)
    std::vector<float> oldVisualX(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        float base = (i < m_BaseX.size()) ? m_BaseX[i] : kids[i]->GetLayoutX();
        float off = (i < m_AnimOffsets.size()) ? m_AnimOffsets[i] : 0.0f;
        oldVisualX[i] = base + off;
    }

    // Recompute base positions (sequential from bar start)
    m_BaseX.resize(n);
    m_AnimOffsets.resize(n, 0.0f);
    float curX = GetLayoutX() + GetLayoutPadding().Left - m_ScrollOffset;
    for (size_t i = 0; i < n; ++i) {
        if (!kids[i]->HasClass("tab"))
        {
            m_BaseX[i] = kids[i]->GetLayoutX();
            m_AnimOffsets[i] = 0.0f;
            continue;
        }
        m_BaseX[i] = curX;
        curX += kids[i]->GetLayoutWidth() + kTabGapPx;
    }

    // Set offsets: for non-dragged tabs, offset = oldVisualX - newBaseX (animate toward 0)
    // For dragged tab, keep the mouse-driven offset
    for (size_t i = 0; i < n; ++i) {
        if (!kids[i]->HasClass("tab"))
            continue;
        if (kids[i].get() == m_LiveDragTab) {
            // Recompute drag offset to maintain mouse position
            m_AnimOffsets[i] = oldVisualX[i] - m_BaseX[i];
        } else {
            m_AnimOffsets[i] = oldVisualX[i] - m_BaseX[i];
        }
    }
}

bool DockTabBar::TickDragAnimations(float dt) {
    if (m_AnimOffsets.empty()) return false;

    constexpr float kSpeed = 18.0f;
    bool anyActive = false;
    const auto& kids = GetChildren();

    for (size_t i = 0; i < m_AnimOffsets.size() && i < kids.size(); ++i) {
        if (!kids[i]->HasClass("tab"))
            continue;
        if (kids[i].get() == m_LiveDragTab) {
            anyActive = true;
            continue;
        }
        float& off = m_AnimOffsets[i];
        if (std::abs(off) < 0.5f) {
            off = 0.0f;
        } else {
            off *= std::exp(-kSpeed * dt);
            anyActive = true;
        }
    }

    return anyActive;
}

void DockTabBar::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                      const ResolvedStyle& /*style*/,
                                      float x, float y, float w, float h) {
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker — escalate
    // and let the drain re-emit this element on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    // Apply animation offsets to children's layout positions for rendering.
    // We write base + offset into SetLastLayoutRect. On the next frame,
    // UpdateLiveDrag/NotifyReorderSwap use m_BaseX (not GetLayoutX) so
    // there's no accumulation.
    if (!m_AnimOffsets.empty()) {
        TickDragAnimations(0.016f);

        const auto& kids = GetChildren();
        for (size_t i = 0; i < kids.size() && i < m_AnimOffsets.size() && i < m_BaseX.size(); ++i) {
            UIElement* child = kids[i].get();
            if (!child->HasClass("tab"))
                continue;
            float newX = m_BaseX[i] + m_AnimOffsets[i];
            float dx = newX - child->GetLayoutX();
            UILayoutAccess::SetLastLayoutRect(*child, 
                newX,
                child->GetLayoutY(),
                child->GetLayoutWidth(),
                child->GetLayoutHeight());
            if (dx != 0.0f)
                ShiftSubtree(child, dx);
        }
    }

    if (!m_InsertActive) return;

    // (x, y, w, h) arrive in physical px; m_InsertX and CSS constants are
    // logical (m_InsertX comes from a cursor-derived value in layout space).
    const float cs = ctx.ContentScale;
    constexpr float kLineWLogical = 2.0f;
    const float kLineW = kLineWLogical * cs;
    const float kTopPad = 2.0f * cs;
    const float kBotPad = 4.0f * cs;
    const float insertPhysical = m_InsertX * cs;
    float ix = std::max(x, std::min(x + w, x + insertPhysical));
    const float ar = ((m_AccentColor >> 16) & 0xFF) / 255.0f;
    const float ag = ((m_AccentColor >> 8) & 0xFF) / 255.0f;
    const float ab = (m_AccentColor & 0xFF) / 255.0f;
    uint32_t color = UI::PackColor(ar, ag, ab, 0.95f);
    ctx.Emit(UI::MakeRect(ix - kLineW * 0.5f, y + kTopPad, kLineW, std::max(0.0f, h - kBotPad), color));
}

} // namespace GameEngine
