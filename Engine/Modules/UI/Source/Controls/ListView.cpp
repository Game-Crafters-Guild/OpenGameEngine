#include "UI/Controls/ListView.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/SystemMetrics.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/VirtualWindowCore.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/ItemResizeGesture.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/VirtualizationCoordinator.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace GameEngine
{

namespace
{
static int RoundToPx(float v)
{
    // UI layout can be fractional (DPI scaling), but absolute override
    // rects are integer pixels. Keep all virtualization math in the same rounded
    // pixel space to avoid cumulative drift (which can manifest as overscroll to
    // empty space at the end of long lists).
    if (!(v > 0.0f))
        return 0;
    return std::max(0, (int)std::lround(v));
}
} // namespace

// VirtualWindowCore::Impact mirrors UIManager::VirtualizationImpact bit-for-bit
// so the aggregated impact forwards with a single cast. (Same checks live in
// GridView.cpp; static_assert produces no symbol, so repeating them per-TU is fine.)
static_assert(static_cast<std::uint32_t>(UI::VirtualWindowCore::Impact::Rebind) ==
                  UIManager::VirtualizationRebindChanged,
              "Impact::Rebind must match VirtualizationRebindChanged");
static_assert(static_cast<std::uint32_t>(UI::VirtualWindowCore::Impact::LayoutRects) ==
                  UIManager::VirtualizationLayoutRectsChanged,
              "Impact::LayoutRects must match VirtualizationLayoutRectsChanged");
static_assert(static_cast<std::uint32_t>(UI::VirtualWindowCore::Impact::Topology) ==
                  UIManager::VirtualizationTopologyChanged,
              "Impact::Topology must match VirtualizationTopologyChanged");

// Bridges the flat cell pool to the shared window engine. ListView is one lane
// (a line is a row), so a flat slot equals the ring's line slot and an item
// index equals the row index; the row's Y offset comes from the prefix-sum cache.
struct ListView::HostAdapter final : UI::VirtualWindowCore::Host
{
    ListView& Owner;

    explicit HostAdapter(ListView& owner) : Owner(owner) {}

    void EnsurePool(int slotCount) override
    {
        Owner.EnsureCellPoolSize(slotCount);
        Owner.UnbindSlotsFrom(slotCount);
    }
    int SlotCount() const override { return static_cast<int>(Owner.m_CellPool.size()); }
    UI::VirtualWindowCore::Impact Rebind(int slot, int itemIndex) override
    {
        int yOffsetPx = 0;
        if (itemIndex >= 0 && (size_t)itemIndex < Owner.m_CumulativeYPx.size())
            yOffsetPx = Owner.m_CumulativeYPx[(size_t)itemIndex];
        return Owner.BindCell(slot, itemIndex, yOffsetPx) ? UI::VirtualWindowCore::Impact::Rebind
                                                          : UI::VirtualWindowCore::Impact::None;
    }
    void UnbindSlot(int slot) override { Owner.UnbindCell(slot); }
    bool SlotBinds(int slot, int itemIndex) const override
    {
        if (slot < 0 || slot >= (int)Owner.m_CellPool.size())
            return false;
        const CellSlot& s = Owner.m_CellPool[(size_t)slot];
        return s.boundId != 0 && s.boundIndex == itemIndex;
    }
    void DestroyTailSlot() override { Owner.DestroyTailSlot(); }
};

ListView::ListView()
{
    // Fixed header host (optional). The header content is positioned via absolute overrides
    // so it can follow horizontal scroll without requiring Yoga/style rebuilds.
    auto headerHost = std::make_unique<UIElement>();
    m_HeaderHost = headerHost.get();
    m_HeaderHost->AddClass("list-header-host");
    m_HeaderHost->Overrides()
        .Set(Style::Display, DisplayMode::None)
        .Set(Style::FlexGrow, 0.0f)
        .Set(Style::FlexShrink, 0.0f)
        .Set(Style::FlexBasis, StyleLength::Auto())
        .Set(Style::OverflowProp, Overflow::Hidden)
        .Set(Style::Position, PositionType::Relative)
        .Set(Style::MinWidth, StyleLength::Px(0.0f))
        .Set(Style::MinHeight, StyleLength::Px(0.0f));

    auto headerWrapper = std::make_unique<UIElement>();
    m_HeaderWrapper = headerWrapper.get();
    m_HeaderWrapper->AddClass("list-header-wrapper");
    // Header content can be fully clipped during horizontal scroll; keep geometry generated so it can
    // be revealed by scroll-only translation without requiring a full rebuild.
    m_HeaderWrapper->SetDisableClipCulling(true);
    // Header wrapper is absolutely positioned; rect is applied via SetAbsolutePosition().
    m_HeaderHost->AddChild(std::move(headerWrapper));

    AddChild(std::move(headerHost));

    auto sv = std::make_unique<ScrollView>();
    m_Scroll = sv.get();
    m_Scroll->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MinWidth, StyleLength::Px(0.0f))
        .Set(Style::MinHeight, StyleLength::Px(0.0f));

    // Every item view answers the one host-configured item resize gesture.
    m_Scroll->RegisterEventHandler(kEventScroll, [this](UIEvent& e)
                                   {
        if (UI::MatchesItemResizeGesture(e.Mods) && e.ScrollY != 0.0f && m_OnItemResizeGesture)
        {
            // scrollY is negative when scrolling up (wheel forward), positive when scrolling down
            m_OnItemResizeGesture(e.ScrollY);
            e.Stop();
        } });

    // Listen for scroll position changes to update virtualization
    m_Scroll->SetOnScrollChanged([this](float scrollX, float scrollY)
                                 {
        if (m_OnScrollChanged)
            m_OnScrollChanged(scrollX, scrollY);
        UpdateHeaderScrollX(scrollX);
        // Avoid rebinding cells on every pixel of scroll. Scrolling translates the
        // scroll-content subtree; we only need to rebind when the visible mapping
        // changes (UpdateVirtualization's cached-state check handles that).
        if (UIManager* ui = GetOwnerManager())
        {
            ui->EnqueueVirtualizationWork(this,
                                          [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                          {
                                              static_cast<ListView*>(ctx)->UpdateVirtualization(reason);
                                          },
                                          VirtualizationCoordinator::Reason::ScrollChanged);
        }
        else
        {
            // Best-effort fallback for cases where the control isn't currently attached.
            UpdateVirtualization(VirtualizationCoordinator::Reason::ScrollChanged);
        } });

    auto content = std::make_unique<UIElement>();
    content->AddClass("list-virtual-content");
    // Virtualized rows can be clipped during horizontal scroll; keep their geometry generated so
    // scroll-only translation can reveal them without requiring a full rebuild.
    content->SetDisableClipCulling(true);
    m_Content = content.get();

    m_Content->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
                                    {
        if (e.CurrentTarget == m_Content && e.Target == m_Content)
            ClearHover(); });

    m_Content->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
                                    {
        if (e.Button != 1) return;
        if (e.CurrentTarget != m_Content || e.Target != m_Content) return;
        if (m_OnContextMenu) m_OnContextMenu(0, e.X, e.Y);
        e.Stop(); });
    // Left-click on empty space: clear selection.
    m_Content->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
                                    {
        if (e.Button != 0) return;
        if (e.CurrentTarget != m_Content || e.Target != m_Content) return;
        if (m_Selection)
        {
            m_Selection->Clear();
            for (auto& s : m_CellPool)
            {
                if (!s.cell) continue;
                s.cell->RemoveClass("selected");
                if (s.userContent) s.userContent->RemoveClass("selected");
            }
        }
        m_SelectedId = 0;
        m_SelectedIndex = -1;
        m_PendingCollapseToSingle = false;
        m_PendingCollapseId = 0;
        e.Stop(); });

    m_Scroll->AddContent(std::move(content));
    AddChild(std::move(sv));
}

ListView::~ListView()
{
    if (UIManager* registered = m_RegisteredVirtualizationManager.Get())
        registered->UnregisterVirtualizedControl(this);
}

void ListView::OnOwnerManagerChanged(UIManager* owner)
{
    if (UIManager* registered = m_RegisteredVirtualizationManager.Get())
        registered->UnregisterVirtualizedControl(this);
    m_RegisteredVirtualizationManager = UIManagerRef(owner);
    if (owner)
        owner->RegisterVirtualizedControl(this);
}

void ListView::EnqueuePumpWork(UIManager& ui)
{
    ui.EnqueueVirtualizationWork(this,
                                 [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                 { static_cast<ListView*>(ctx)->UpdateVirtualization(reason); },
                                 VirtualizationCoordinator::Reason::DataChanged);
}

void ListView::AutoScrollDuringDrag(float mouseX, float mouseY)
{
    if (!m_Scroll)
        return;
    UIElement* clip = m_Scroll->GetClipViewport();
    if (!clip)
        return;

    const float vx = clip->GetLayoutX();
    const float vy = clip->GetLayoutY();
    const float vw = clip->GetLayoutWidth();
    const float vh = clip->GetLayoutHeight();
    if (!(vh > 0.0f) || !(vw > 0.0f))
        return;
    if (mouseX < vx || mouseX >= vx + vw || mouseY < vy || mouseY >= vy + vh)
        return;

    constexpr float kEdgePx = 28.0f;
    constexpr float kMaxSpeedPxPerFrame = 18.0f;

    const float distTop = mouseY - vy;
    const float distBot = (vy + vh) - mouseY;

    float dy = 0.0f;
    if (distTop < kEdgePx)
    {
        const float t = std::clamp((kEdgePx - distTop) / kEdgePx, 0.0f, 1.0f);
        dy = -kMaxSpeedPxPerFrame * t;
    }
    else if (distBot < kEdgePx)
    {
        const float t = std::clamp((kEdgePx - distBot) / kEdgePx, 0.0f, 1.0f);
        dy = kMaxSpeedPxPerFrame * t;
    }

    if (dy != 0.0f)
        m_Scroll->ScrollBy(0.0f, dy);
}

bool ListView::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return m_DropAccepts ? m_DropAccepts(typeId) : false;
}

bool ListView::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    // Hit bound pooled row wrappers only: unbound slots keep whatever layout
    // rect they last had, so a stale rect must never produce a row hit (same
    // guard cluster as GetCellForIndex/GetIndexAtPoint).
    for (const auto& slot : m_CellPool)
    {
        if (!slot.cell || slot.boundId == 0 || slot.boundIndex < 0)
            continue;
        const float rx = slot.cell->GetLayoutX();
        const float ry = slot.cell->GetLayoutY();
        const float rw = slot.cell->GetLayoutWidth();
        const float rh = slot.cell->GetLayoutHeight();
        if (x >= rx && x < rx + rw && y >= ry && y < ry + rh)
        {
            out.TargetId = slot.boundId;
            out.Location = UI::Interaction::DropLocation::OnItem;
            out.IndentDepth = 0;
            return true;
        }
    }

    // If we're inside the ListView rect but not over a row, allow "empty space" drop.
    const float lx = GetLayoutX();
    const float ly = GetLayoutY();
    const float lw = GetLayoutWidth();
    const float lh = GetLayoutHeight();
    if (x >= lx && x < lx + lw && y >= ly && y < ly + lh)
    {
        out.TargetId = 0;
        out.Location = UI::Interaction::DropLocation::OnEmptySpace;
        out.IndentDepth = 0;
        return true;
    }
    return false;
}

UI::Interaction::DropFeedback ListView::CanDrop(const UI::Interaction::DropRequest& request) const
{
    return m_OnCanDrop ? m_OnCanDrop(request) : UI::Interaction::DropFeedback{false, "No drop handler"};
}

void ListView::PerformDrop(const UI::Interaction::DropRequest& request)
{
    if (m_OnPerformDrop)
        m_OnPerformDrop(request);
}

void ListView::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    m_DropPreview = state;
    UpdateDropPreviewClasses();
}

void ListView::UpdateDropPreviewClasses()
{
    for (auto& s : m_CellPool)
    {
        if (!s.cell)
            continue;
        s.cell->RemoveClass("drop-hover");
        s.cell->RemoveClass("drop-allowed");
        s.cell->RemoveClass("drop-denied");
        if (m_DropPreview.Visible &&
            m_DropPreview.Hit.Location == UI::Interaction::DropLocation::OnItem &&
            s.boundId != 0 &&
            s.boundId == m_DropPreview.Hit.TargetId)
        {
            s.cell->AddClass("drop-hover");
            if (m_DropPreview.Allowed)
                s.cell->AddClass("drop-allowed");
            else
                s.cell->AddClass("drop-denied");
        }
    }
}

void ListView::SetHeader(std::unique_ptr<UIElement> header)
{
    if (!m_HeaderHost || !m_HeaderWrapper)
        return;

    // Clear existing header content (if any).
    if (m_HeaderContent)
    {
        m_HeaderWrapper->RemoveChild(m_HeaderContent);
        m_HeaderContent = nullptr;
    }

    if (!header)
    {
        m_HeaderHost->Overrides().Set(Style::Display, DisplayMode::None);
        UI::Layout::ClearForcedHeight(*m_HeaderHost);
        m_HeaderHeightPx = 0;
        return;
    }

    m_HeaderContent = header.get();
    m_HeaderWrapper->AddChild(std::move(header));
    m_HeaderHost->Overrides().Set(Style::Display, DisplayMode::Flex);

    // Ensure the host has a non-zero height even though the header wrapper is absolutely positioned.
    // We'll refine this on the next OnPostLayout() once the header content has a measured height.
    constexpr int kDefaultHeaderHPx = 28;
    int desiredHPx = RoundToPx(m_HeaderContent->GetLayoutHeight());
    if (desiredHPx <= 0)
        desiredHPx = kDefaultHeaderHPx;
    if (m_HeaderHeightPx != desiredHPx)
    {
        m_HeaderHeightPx = desiredHPx;
        UI::Layout::SetForcedHeight(*m_HeaderHost, m_HeaderHeightPx);
    }

    // Ensure the header is aligned immediately to current scrollX (best-effort).
    const float sx = m_Scroll ? m_Scroll->GetScrollX() : 0.0f;
    UpdateHeaderScrollX(sx);
}

void ListView::UpdateHeaderScrollX(float scrollX)
{
    if (!m_HeaderHost || !m_HeaderWrapper || !m_HeaderContent)
        return;

    // Compute desired wrapper rect (relative to headerHost).
    const int leftPx = -RoundToPx(scrollX);
    const int topPx = 0;

    int widthPx = 0;
    if (m_ExplicitContentWidthPx > 0.0f)
        widthPx = RoundToPx(m_ExplicitContentWidthPx);
    else if (m_Scroll)
        widthPx = RoundToPx(m_Scroll->GetContentWidth());
    if (widthPx <= 0)
        widthPx = RoundToPx(m_HeaderHost->GetLayoutWidth());

    int heightPx = (m_HeaderHeightPx > 0) ? m_HeaderHeightPx : RoundToPx(m_HeaderHost->GetLayoutHeight());
    if (heightPx <= 0)
        heightPx = RoundToPx(m_HeaderContent->GetLayoutHeight());

    UI::Layout::SetAbsolutePosition(*m_HeaderWrapper,
                                    Mathematics::Rect{
                                        static_cast<float>(leftPx),
                                        static_cast<float>(topPx),
                                        static_cast<float>(widthPx),
                                        static_cast<float>(heightPx),
                                    });
}

void ListView::UpdateVisibleRowWidths()
{
    if (m_ExplicitContentWidthPx <= 0.0f)
        return;
    const int widthPx = RoundToPx(m_ExplicitContentWidthPx);
    if (widthPx <= 0)
        return;
    for (const auto& slot : m_CellPool)
    {
        if (!slot.cell || slot.boundId == 0)
            continue;
        Mathematics::Rect currentRect{};
        if (!UI::Layout::TryGetAbsolutePosition(*slot.cell, currentRect))
            continue;
        if (RoundToPx(currentRect.Width) != widthPx)
        {
            UI::Layout::SetAbsolutePosition(*slot.cell,
                                            Mathematics::Rect{
                                                0.0f,
                                                currentRect.Y,
                                                static_cast<float>(widthPx),
                                                std::max(0.0f, currentRect.Height),
                                            });
        }
    }
}

void ListView::SetDataProvider(IListDataProvider* provider)
{
    m_Provider = provider;
    m_LastProviderChangeVersion = 0;
    m_LastProviderStructureVersion = 0;
    m_PendingChangeSetValid = false;
    m_SelectedId = 0;
    m_SelectedIndex = -1;
    m_LastClickedId = 0;
    m_DataDirty = true;
    m_LayoutCacheDirty = true;
    // Drop the core's window/ring/pool bookkeeping so the new provider rebinds from
    // scratch; m_DataDirty forces a full rebind on the next update.
    m_WindowCore.Reset();
    // Provider changes affect the virtualized content size and bindings, not Yoga layout.
    // Avoid RequestRelayout() to keep idle/update frames cheap.
    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this, [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      { static_cast<ListView*>(ctx)->UpdateVirtualization(reason); }, VirtualizationCoordinator::Reason::DataChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
    }
    SchedulePrewarmVirtualization();
    // Provider swaps typically only require paint/binding changes; any layout-affecting updates
    // (forced content height, scrollbar range) are driven by absolute overrides/ScrollView and
    // will mark their own layout dirties when needed.
    MarkDirty(VisualDirty);
}

void ListView::SetSelectedId(ListId id)
{
    if (m_SelectedId == id)
        return;
    m_SelectedId = id;
    if (m_Provider && m_SelectedId != 0)
    {
        const int count = m_Provider->GetItemCount();
        for (int i = 0; i < count; ++i)
        {
            if (m_Provider->GetItemId(i) == m_SelectedId)
            {
                m_SelectedIndex = i;
                break;
            }
        }
    }
    if (m_Selection)
        m_Selection->SetSingle(id);
    m_DataDirty = true;
    // Selection is paint-only (background/text colors). No need to force a relayout.
    MarkDirty(VisualDirty);
}

void ListView::ClearHover()
{
    // Hover styling should be driven by CSS :hover (including ancestor hover semantics)
    // via UIManager's hoverTarget. Keep this for API compatibility but avoid
    // mutating classes on every mouse move (it causes expensive dirty propagation).
    m_LastHoverCell = nullptr;
}

int ListView::PublishContentSize(float viewportW, float viewportH)
{
    const int viewportHPx = std::max(0, (int)std::ceil(viewportH));
    const int totalHPx = std::max(0, m_TotalContentHeightPx);
    const int forcedHPx = std::max(totalHPx, viewportHPx);
    UI::Layout::SetForcedHeight(*m_Content, forcedHPx);

    // Virtualization only needs to drive height.
    //
    // Do NOT feed ScrollView::GetContentWidth() back into SetContentSize() here.
    // GetContentWidth() returns max(m_ContentW, layoutWidth). If a ListView was previously
    // mounted in a wider container, m_ContentW can remain large and will never shrink,
    // which causes phantom horizontal scrollbars after docking/layout changes.
    //
    // Instead, set the explicit content width to the current viewport width and let
    // ScrollView's own horizontal-measure logic (if enabled by theme) expand it when needed.
    float contentW = (viewportW > 0.0f) ? viewportW : 0.0f;
    if (m_ExplicitContentWidthPx > 0.0f)
        contentW = std::max(contentW, m_ExplicitContentWidthPx);
    m_Scroll->SetContentSize(contentW, (float)totalHPx);
    return forcedHPx;
}

void ListView::RebuildLayoutCache()
{
    m_CumulativeYPx.clear();
    m_TotalContentHeightPx = 0;
    m_MinItemHeightPx = 0;

    if (!m_Provider)
    {
        m_LayoutCacheDirty = false;
        return;
    }

    const int count = m_Provider->GetItemCount();
    m_CumulativeYPx.reserve((size_t)count + 1);
    m_CumulativeYPx.push_back(0);

    int yPx = 0;
    int minHPx = 0;
    for (int i = 0; i < count; ++i)
    {
        const int hPx = RoundToPx(m_Provider->GetItemHeight(i));
        if (hPx > 0)
            minHPx = (minHPx == 0) ? hPx : std::min(minHPx, hPx);
        yPx += hPx;
        m_CumulativeYPx.push_back(yPx);
    }
    m_TotalContentHeightPx = yPx;
    m_MinItemHeightPx = minHPx;
    m_LayoutCacheDirty = false;
}

void ListView::EnsureCellPoolSize(int desired)
{
    if (!m_Content || desired <= 0)
        return;

    bool grewPool = false;
    while ((int)m_CellPool.size() < desired)
    {
        grewPool = true;
        const int slotIndex = (int)m_CellPool.size();

        // Always create a wrapper for absolute positioning.
        auto wrapper = std::make_unique<UIElement>();
        wrapper->AddClass("list-cell");
        // Allow horizontal scrolling to reveal content without forcing a geometry rebuild.
        wrapper->SetDisableClipCulling(true);

        // Get user content from factory or create default label
        std::unique_ptr<UIElement> userContentPtr;
        if (m_ItemFactory && m_Provider)
            userContentPtr = m_ItemFactory(0, m_Provider);

        if (!userContentPtr)
        {
            auto defaultLabel = std::make_unique<Label>();
            defaultLabel->AddClass("list-item");
            userContentPtr = std::move(defaultLabel);
        }

        UIElement* userContentRaw = userContentPtr.get();

        // Add user content as child of the wrapper
        wrapper->AddChild(std::move(userContentPtr));

        UIElement* cellRaw = wrapper.get();
        std::unique_ptr<UIElement> cell = std::move(wrapper);

        // Route keyboard focus from children to the ListView so key repeats reach it.
        cellRaw->SetFocusProxy(this);
        if (userContentRaw)
            userContentRaw->SetFocusProxy(this);

        // NOTE: We intentionally do not toggle a ".hover" class here.
        // CSSParser already implements ancestor :hover semantics when UIManager passes
        // hoverTarget, so `:hover` rules behave like standard CSS without manual class churn.

        // Selection / activation / context menu / drag.
        // IMPORTANT: Selection is established on mouse-down so drag payload builders can include
        // the clicked item even before mouse-up.
        cellRaw->RegisterEventHandler(kEventMouseDown, [this, cellRaw, slotIndex](UIEvent& e)
                                      {
            if (e.Button != 0)
                return;
            if (slotIndex < 0 || slotIndex >= (int)m_CellPool.size())
                return;
            const ListId lid = m_CellPool[slotIndex].boundId;
            if (lid == 0)
                return;

            // Read before the press edits the selection: a rename request needs
            // "the row was already the sole selection".
            const bool pressWasOnSoleSelected = m_Selection
                ? (m_Selection->IsSelected(lid) && m_Selection->GetSelection().size() == 1)
                : (m_SelectedId == lid);

            if (m_Selection)
            {
                const bool shift = (e.Mods & Input::kModShift) != 0;
                const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);
                if (shift && m_Provider)
                {
                    // Avoid scanning provider ids (O(n)). Use our cached anchor index.
                    if (m_SelectedIndex < 0)
                        m_SelectedIndex = m_CellPool[slotIndex].boundIndex;
                    if (m_ShiftAnchorIndex < 0)
                        m_ShiftAnchorIndex = m_SelectedIndex;

                    const int a = m_ShiftAnchorIndex;
                    const int b = m_CellPool[slotIndex].boundIndex;
                    if (a >= 0 && b >= 0 && a < m_Provider->GetItemCount() && b < m_Provider->GetItemCount())
                    {
                        const int lo = std::min(a, b);
                        const int hi = std::max(a, b);
                        std::vector<UI::Interaction::ItemId> ids;
                        ids.reserve((size_t)(hi - lo + 1));
                        for (int i = lo; i <= hi; ++i)
                        {
                            const ListId id = m_Provider->GetItemId(i);
                            if (id != 0)
                                ids.push_back(id);
                        }
                        if (primaryMod)
                        {
                            const auto cur = m_Selection->GetSelection();
                            ids.insert(ids.end(), cur.begin(), cur.end());
                        }
                        const ListId anchorId = m_Provider->GetItemId(a);
                        m_Selection->SetSelection(ids, anchorId);
                    }
                    else
                    {
                        m_Selection->SetSingle(lid);
                    }
                }
                else if (primaryMod)
                {
                    m_Selection->Toggle(lid);
                    m_ShiftAnchorIndex = m_CellPool[slotIndex].boundIndex;
                }
                else
                {
                    // Preserve multi-selection when clicking a selected item.
                    if (m_Selection->IsSelected(lid))
                    {
                        const auto cur = m_Selection->GetSelection();
                        if (cur.size() > 1)
                        {
                            m_Selection->SetAnchor(lid);
                            m_PendingCollapseToSingle = true;
                            m_PendingCollapseId = lid;
                        }
                        else
                        {
                            m_Selection->SetSingle(lid);
                            m_PendingCollapseToSingle = false;
                            m_PendingCollapseId = 0;
                        }
                    }
                    else
                    {
                        m_Selection->SetSingle(lid);
                        m_PendingCollapseToSingle = false;
                        m_PendingCollapseId = 0;
                    }
                    m_ShiftAnchorIndex = m_CellPool[slotIndex].boundIndex;
                }
                m_SelectedId = lid;
                m_SelectedIndex = m_CellPool[slotIndex].boundIndex;
            }
            else
            {
                m_SelectedId = lid;
                m_SelectedIndex = m_CellPool[slotIndex].boundIndex;
            }

            // Update selection visuals immediately for pooled, visible rows.
            // This keeps UI in sync during clicks/drags without requiring a full virtualization pass.
            for (auto& s : m_CellPool)
            {
                if (!s.cell)
                    continue;
                const bool isSelected = (s.boundId != 0) && (m_Selection ? m_Selection->IsSelected(s.boundId) : (s.boundId == m_SelectedId));
                if (isSelected)
                {
                    s.cell->AddClass("selected");
                    if (s.userContent)
                        s.userContent->AddClass("selected");
                }
                else
                {
                    s.cell->RemoveClass("selected");
                    if (s.userContent)
                        s.userContent->RemoveClass("selected");
                }
            }

            if (m_OnSelectionChanged) m_OnSelectionChanged(lid);

            using clock = std::chrono::steady_clock;
            auto now = clock::now();
            const bool isRepeatClick = (m_LastClickedId == lid);
            bool isDouble = isRepeatClick &&
                ((now - m_LastClickTime) < Platform::GetDoubleClickInterval());
            m_LastClickedId = lid;
            m_LastClickTime = now;
            // A slow second press on the sole selected row is a rename request,
            // honoured on release unless the press becomes a drag. Modified
            // clicks are selection edits and never rename.
            m_PendingRenameRequest = isRepeatClick && !isDouble && !m_PickerActivation &&
                                     pressWasOnSoleSelected &&
                                     (e.Mods & Input::kModShortcutMask) == 0;
            // A picker popup commits on the first click: it exists only to choose
            // one row, so requiring a second click reads as a dead control.
            if ((isDouble || m_PickerActivation) && m_OnItemActivated)
            {
                m_DragCandidate = false;
                m_DragStartedThisGesture = false;
                m_PendingCollapseToSingle = false;
                m_PendingCollapseId = 0;

                // Popup activation callbacks may remove the list before mouse-up. Keep
                // ownership of the rest of this press so it cannot be retargeted to a
                // newly exposed control underneath.
                if (m_PickerActivation)
                {
                    if (UIManager* owner = GetOwnerManager())
                    {
                        if (UIElement* root = owner->GetRootElement())
                            e.Capture(root);
                    }
                }
                m_OnItemActivated(lid);
                e.Stop();
                return;
            }

            // Keyboard routing uses m_FocusId. Hover-time focus assignment can be skipped when the
            // tree mutates during this dispatch, or it can clear focus if the hit target does not
            // resolve to a focusable control — so arrow keys never reach this view. Take focus after
            // the default mouse-down focus pass (deferred) so list navigation is reliable.
            PostSafeAction([this]() {
                if (UIManager* ui = GetOwnerManager())
                    ui->FocusElement(this);
            });

            // Arm drag gesture (drag begins on move threshold).
            if (m_DragPayloadBuilder)
            {
                m_DragCandidate = true;
                m_DragStartedThisGesture = false;
                m_DragStartX = e.X;
                m_DragStartY = e.Y;
                e.Capture(cellRaw);
            } });

        cellRaw->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
                                      {
            if (!m_DragCandidate)
                return;
            if (!m_DragPayloadBuilder)
                return;
            UIManager* ui = GetOwnerManager();
            if (!ui)
                return;
            auto* dd = ui->GetDragDropManager();
            if (!dd || dd->IsDragging())
                return;
            const float dx = e.X - m_DragStartX;
            const float dy = e.Y - m_DragStartY;
            const float dist2 = dx * dx + dy * dy;
            constexpr float kThresh = 6.0f;
            if (dist2 < (kThresh * kThresh))
                return;
            UI::Interaction::DragPayload payload = m_DragPayloadBuilder();
            if (!payload.IsValid())
                return;
            UI::Interaction::DragSessionContext ctx{};
            ctx.SourceWidgetId = GetInstanceId();
            dd->BeginDrag(std::move(payload), ctx);
            m_DragCandidate = false;
            m_DragStartedThisGesture = true;
            m_PendingCollapseToSingle = false;
            m_PendingCollapseId = 0;
            e.Stop(); });

        cellRaw->RegisterEventHandler(kEventMouseUp, [this, slotIndex](UIEvent& e)
                                      {
            if (m_DragStartedThisGesture)
            {
                m_DragStartedThisGesture = false;
                m_DragCandidate = false;
                m_PendingCollapseToSingle = false;
                m_PendingCollapseId = 0;
                m_PendingRenameRequest = false;
                e.Stop();
                return;
            }

            m_DragCandidate = false;
            if (slotIndex < 0 || slotIndex >= (int)m_CellPool.size()) return;
            const ListId lid = m_CellPool[slotIndex].boundId;
            if (lid == 0) return;

            if (e.Button == 1)
            {
                if (m_OnContextMenu) m_OnContextMenu(lid, e.X, e.Y);
                e.Stop();
                return;
            }

            if (e.Button != 0) return;

            const bool renameRequested = m_PendingRenameRequest;
            m_PendingRenameRequest = false;
            if (renameRequested && m_OnItemRenameRequested)
                m_OnItemRenameRequested(lid);

            if (m_Selection && m_PendingCollapseToSingle && m_PendingCollapseId == lid)
            {
                const bool shift = (e.Mods & Input::kModShift) != 0;
                const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);
                if (!shift && !primaryMod)
                {
                    m_Selection->SetSingle(lid);
                    for (auto& s : m_CellPool)
                    {
                        if (!s.cell)
                            continue;
                        const bool isSelected = (s.boundId != 0) && m_Selection->IsSelected(s.boundId);
                        if (isSelected)
                        {
                            s.cell->AddClass("selected");
                            if (s.userContent) s.userContent->AddClass("selected");
                        }
                        else
                        {
                            s.cell->RemoveClass("selected");
                            if (s.userContent) s.userContent->RemoveClass("selected");
                        }
                    }
                    if (m_OnSelectionChanged) m_OnSelectionChanged(lid);
                }
            }
            m_PendingCollapseToSingle = false;
            m_PendingCollapseId = 0;
            });

        m_Content->AddChild(std::move(cell));

        CellSlot slot{};
        slot.cell = cellRaw;
        slot.userContent = userContentRaw;
        slot.boundId = 0;
        slot.boundIndex = -1;
        m_CellPool.push_back(std::move(slot));
    }

    if (grewPool)
    {
        if (UIManager* ui = GetOwnerManager())
            ui->NotifyVirtualizationImpact(UIManager::VirtualizationTopologyChanged);
    }
}

// Excess cells are unbound in place, never detached from the tree and never
// display:none'd: virtualized controls can rebind during UIManager's
// scroll-callback flush, after the per-frame node list has already been built,
// so detaching caused a one-frame "flicker" where old nodes were skipped and
// new nodes were missing from the cached traversal; display:none forces
// layout/paint topology changes that can drop retained paint-command ranges
// ("blank" rows until a later full rebuild). UnbindCell keeps the cell
// attached and paint/input-invisible instead.
void ListView::UnbindSlotsFrom(int firstSlot)
{
    for (int i = std::max(firstSlot, 0); i < (int)m_CellPool.size(); ++i)
        UnbindCell(i);
}

void ListView::UnbindCell(int slot)
{
    if (slot < 0 || slot >= (int)m_CellPool.size())
        return;
    CellSlot& s = m_CellPool[(size_t)slot];
    if (!s.cell)
        return;
    if (s.boundId != 0)
    {
        if (UIManager* ui = GetOwnerManager())
            ui->ClearHoverForSubtree(s.cell);
    }
    s.cell->RemoveClass("selected");
    s.cell->RemoveClass("drop-hover");
    s.cell->RemoveClass("drop-allowed");
    s.cell->RemoveClass("drop-denied");
    if (s.userContent)
        s.userContent->RemoveClass("selected");
    s.boundId = 0;
    s.boundIndex = -1;
    UI::Layout::SetElementInvisible(*s.cell, true);
}

void ListView::DestroyTailSlot()
{
    if (m_CellPool.empty())
        return;
    CellSlot& slot = m_CellPool.back();
    if (m_Content && slot.cell)
        (void)m_Content->TakeChild(slot.cell);
    m_CellPool.pop_back();
}

bool ListView::BindCell(int slotIndex, int itemIndex, int yOffsetPx)
{
    if (slotIndex < 0 || slotIndex >= (int)m_CellPool.size())
        return false;

    CellSlot& slot = m_CellPool[(size_t)slotIndex];
    if (!slot.cell)
        return false;

    const ListId prevId = slot.boundId;
    const int prevIndex = slot.boundIndex;

    if (!m_Provider || itemIndex < 0 || itemIndex >= m_Provider->GetItemCount())
    {
        UnbindCell(slotIndex);
        return false;
    }

    const ListId lid = m_Provider->GetItemId(itemIndex);
    slot.boundId = lid;
    slot.boundIndex = itemIndex;
    const bool bindingChanged = (prevId != lid) || (prevIndex != itemIndex);
    if (bindingChanged && prevId != 0)
    {
        if (UIManager* ui = GetOwnerManager())
            ui->ClearHoverForSubtree(slot.cell);
    }

    const int heightPx = RoundToPx(m_Provider->GetItemHeight(itemIndex));
    int widthPx = 0;
    if (m_Scroll)
    {
        // IMPORTANT:
        // For horizontally-scrollable lists (e.g. Assets ListView with many columns), each row wrapper
        // must span the *content width*, not just the viewport width. Otherwise, any overflow-hidden
        // clipping on the row will clamp child columns and cause “missing” cells during horizontal scroll.
        widthPx = (m_ExplicitContentWidthPx > 0.0f) ? RoundToPx(m_ExplicitContentWidthPx) : (int)std::lround(m_Scroll->GetContentWidth());
        if (widthPx <= 0)
            widthPx = (int)std::lround(m_Scroll->GetViewportWidth());
    }
    if (widthPx <= 0)
        widthPx = (int)std::lround(GetLayoutWidth());

    UI::Layout::SetElementInvisible(*slot.cell, false);
    UI::Layout::SetAbsolutePosition(*slot.cell,
                                    Mathematics::Rect{
                                        0.0f,
                                        static_cast<float>(yOffsetPx),
                                        static_cast<float>(std::max(0, widthPx)),
                                        static_cast<float>(std::max(0, heightPx)),
                                    },
                                    /*positionOnlyFastPath=*/true);

    const bool isSelected = (lid != 0) && (m_Selection ? m_Selection->IsSelected(lid) : (lid == m_SelectedId));
    if (isSelected)
    {
        slot.cell->AddClass("selected");
        if (slot.userContent)
            slot.userContent->AddClass("selected");
    }
    else
    {
        slot.cell->RemoveClass("selected");
        if (slot.userContent)
            slot.userContent->RemoveClass("selected");
    }

    slot.cell->RemoveClass("drop-hover");
    slot.cell->RemoveClass("drop-allowed");
    slot.cell->RemoveClass("drop-denied");
    if (m_DropPreview.Visible &&
        m_DropPreview.Hit.Location == UI::Interaction::DropLocation::OnItem &&
        lid != 0 &&
        lid == m_DropPreview.Hit.TargetId)
    {
        slot.cell->AddClass("drop-hover");
        if (m_DropPreview.Allowed)
            slot.cell->AddClass("drop-allowed");
        else
            slot.cell->AddClass("drop-denied");
    }

    // Pass userContent to the binder so users get their actual element, not the wrapper
    if (m_ItemBinder && slot.userContent)
        m_ItemBinder(slot.userContent, lid, itemIndex, m_Provider);

    // Force CSS re-evaluation on the entire cell subtree when the binding changes
    // or during a full data refresh (m_DataDirty). MarkDirtySubtree is needed
    // (not just MarkDirty) because descendant labels/icons may depend on the
    // cell's class via CSS selectors. During a data refresh, even cells that map
    // to the same ID/index need restyling because the underlying content changed.
    // No blanket LayoutDirty: the binder's own mutations (SetText, override
    // writes) mark layout dirt precisely where measurement/position changed.
    // The view forwards the aggregated impact once after Update(); per-cell
    // notifies here armed late whole-tree relayouts (the documented failure mode).
    const bool markDirty = (bindingChanged || m_DataDirty);
    if (markDirty)
        slot.cell->MarkDirtySubtree(StyleDirty | VisualDirty);
    return markDirty;
}

void ListView::UpdateVirtualization(VirtualizationCoordinator::Reason reason)
{
    if (!m_Content)
        return;

    // Settle the layout cache and window first (the height re-probe below can flip
    // m_LayoutCacheDirty and re-run this loop), then hand the settled window to the
    // shared core in a single Update() call.
    for (int pass = 0; pass < 2; ++pass)
    {
        if (m_LayoutCacheDirty)
            RebuildLayoutCache();

        if (!m_Provider)
        {
            UnbindSlotsFrom(0);
            UI::Layout::SetForcedHeight(*m_Content, 0);
            m_DataDirty = false;
            m_WindowCore.Reset();
            return;
        }

        if (!m_Scroll)
        {
            // The ScrollView can be temporarily unavailable during mount/rebuild. Defer
            // a retry so the pool/content extents are ready before first user scroll.
            if (!m_VirtualizationRetryScheduled)
            {
                m_VirtualizationRetryScheduled = true;
                PostSafeAction([this, reason]()
                               {
                    m_VirtualizationRetryScheduled = false;
                    this->UpdateVirtualization(reason); });
            }
            return;
        }

        const float viewportH = m_Scroll->GetViewportHeight();
        if (viewportH <= 0.0f)
        {
            // On the first frame after mount/layout changes, the viewport rect can be 0 while Yoga
            // settles. If we wait until the first scroll event to build the pool/content size, the
            // first scrollbar drag pays a one-time heavy cost (scrollbars appear, pool expands).
            // Defer a retry so this work happens as soon as layout becomes meaningful.
            if (!m_VirtualizationRetryScheduled)
            {
                m_VirtualizationRetryScheduled = true;
                PostSafeAction([this, reason]()
                               {
                    m_VirtualizationRetryScheduled = false;
                    this->UpdateVirtualization(reason); });
            }
            return;
        }
        const int viewportWPx = (int)std::lround(m_Scroll->GetViewportWidth());

        const int count = m_Provider->GetItemCount();
        if ((int)m_CumulativeYPx.size() < count + 1)
        {
            // Provider count changed without an explicit refresh/append notification.
            // Rebuild the cache so binary searches remain correct.
            m_LayoutCacheDirty = true;
            continue;
        }
        const float viewportW = m_Scroll->GetViewportWidth();
        const int forcedHPx = PublishContentSize(viewportW, viewportH);
        const int contentWPx = (int)std::lround(m_Scroll->GetContentWidth());

        const float scrollY = m_Scroll->GetScrollY();
        const int scrollYPx = std::max(0, (int)std::floor(scrollY));
        const int bottomYPx = std::max(0, (int)std::ceil(scrollY + viewportH));

        // First visible via the shared prefix-sum helper (the cumulative-Y cache
        // stays a ListView policy structure, rebuilt by RebuildLayoutCache()).
        const int firstVisible = UI::VirtualWindowCore::FirstFromPrefixSum(m_CumulativeYPx, scrollYPx, count);

        // Find last visible item.
        int lastVisible = firstVisible;
        if (count > 0)
        {
            auto itLast = std::lower_bound(m_CumulativeYPx.begin(), m_CumulativeYPx.end(), bottomYPx);
            if (itLast != m_CumulativeYPx.begin())
                --itLast;
            lastVisible = std::clamp((int)std::distance(m_CumulativeYPx.begin(), itLast), 0, count - 1);
            if (lastVisible < firstVisible)
                lastVisible = firstVisible;
        }

        // Detect dynamic provider height changes for visible items (keeps scroll extents stable).
        if (count > 0 && pass == 0)
        {
            auto cachedHeightFor = [&](int idx) -> int
            {
                if (idx < 0 || idx + 1 >= (int)m_CumulativeYPx.size())
                    return -1;
                return m_CumulativeYPx[(size_t)idx + 1] - m_CumulativeYPx[(size_t)idx];
            };
            auto shouldRebuildFor = [&](int idx) -> bool
            {
                const int ch = cachedHeightFor(idx);
                if (ch <= 0)
                    return false;
                const int ah = RoundToPx(m_Provider->GetItemHeight(idx));
                return std::abs(ah - ch) > 1;
            };
            const int mid = (firstVisible + lastVisible) / 2;
            if (shouldRebuildFor(firstVisible) || shouldRebuildFor(mid) || shouldRebuildFor(lastVisible))
            {
                m_LayoutCacheDirty = true;
                continue;
            }
        }

        // Scroll-position-independent pool upper bound (view policy): size from the
        // minimum item height, not the current visible count, so a scrollbar drag
        // never resizes the pool mid-drag (which would mutate the tree and force
        // UIManager down the heavy path). This feeds the core's Guard.Desired.
        constexpr int kExtraVisibleSlack = 2; // rounding / boundary guard
        constexpr int kMaxPool = 512;         // safety cap for pathological (tiny) min heights
        int minHPx = m_MinItemHeightPx;
        if (minHPx <= 0 && count > 0 && m_TotalContentHeightPx > 0)
        {
            // If the cache was built while empty, min height can remain 0 even after incremental
            // appends. Fall back to average height so we don't allocate an enormous pool.
            minHPx = std::max(1, m_TotalContentHeightPx / std::max(1, count));
        }
        // Floor to a reasonable row height so the pool doesn't max out at kMaxPool merely because
        // heights aren't computed yet (a typical row is 24-32px; 16px leaves buffer).
        constexpr int kMinReasonableItemHeight = 16;
        minHPx = std::max(kMinReasonableItemHeight, minHPx);
        const int maxVisibleByMin = std::max(0, (int)std::ceil(viewportH / (float)minHPx) + kExtraVisibleSlack);

        int desiredCount = std::max(0, maxVisibleByMin + m_OverscanItems);
        desiredCount = std::min(desiredCount, kMaxPool);

        // Pull the typed provider changeset and normalize it for the core, which
        // never sees id types: All -> full rebind; Subset -> the flat pool slots
        // currently bound to a changed id.
        ListChangeSet changes{};
        if (m_PendingChangeSetValid)
        {
            changes = std::move(m_PendingChangeSet);
            m_PendingChangeSetValid = false;
        }
        else
        {
            m_Provider->ConsumeChanges(m_LastProviderChangeVersion, changes);
        }
        if (changes.Version != m_LastProviderChangeVersion)
            m_LastProviderChangeVersion = changes.Version;

        // C-7: a structural provider mutation (MarkStructureChanged) can arrive as
        // any changeset kind — items were added/removed/reordered even if the
        // reported ids are a Subset (or None). Treat structure-version movement
        // like an All change: force the full rebind and rebuild the height cache
        // (item heights/positions may have shifted).
        if (changes.StructureVersion != m_LastProviderStructureVersion)
        {
            m_LastProviderStructureVersion = changes.StructureVersion;
            m_DataDirty = true;
            m_LayoutCacheDirty = true;
        }

        // An All change means same-id cells may carry new content and item heights
        // may have shifted, so force the BindCell subtree-refresh gate and rebuild
        // the height cache next pass (mirrors the pre-core forceFullRebind).
        if (changes.Kind == ChangeSetKind::All)
        {
            m_DataDirty = true;
            m_LayoutCacheDirty = true;
        }

        // If the handling above dirtied the height cache (structural / All), rebuild
        // it before binding so the forced height and scroll extents reflect the new
        // item set. The loop re-runs once (pass < 2); the changeset is already
        // consumed, so the second pass sees None and the latched m_DataDirty drives
        // the full rebind against the fresh cache. Without this, a pump-driven data
        // change (no scroll/viewport event) would leave the extents stale until the
        // next user scroll.
        if (m_LayoutCacheDirty && pass == 0)
            continue;

        m_ChangedSlotScratch.clear();
        UI::VirtualWindowCore::ChangeSetView changesView{};
        if (changes.Kind == ChangeSetKind::All)
        {
            changesView.ChangeKind = UI::VirtualWindowCore::ChangeSetView::Kind::All;
        }
        else if (changes.Kind == ChangeSetKind::Subset && !changes.Ids.empty())
        {
            for (int slot = 0; slot < (int)m_CellPool.size(); ++slot)
            {
                const ListId bound = m_CellPool[(size_t)slot].boundId;
                if (bound == 0)
                    continue;
                if (std::find(changes.Ids.begin(), changes.Ids.end(), bound) != changes.Ids.end())
                    m_ChangedSlotScratch.push_back(slot);
            }
            changesView.ChangeKind = UI::VirtualWindowCore::ChangeSetView::Kind::Subset;
            changesView.AffectedSlots = m_ChangedSlotScratch;
        }

        // m_DataDirty forces a full rebind: bump the structure generation so the
        // guard cannot match m_Last and the core skips its ring shortcut. This is
        // the single translation of the old "m_DataDirty => full rebind" rule and
        // covers provider swap, refresh, append, and selection-driven refreshes.
        if (m_DataDirty)
            ++m_StructureGeneration;

        UI::VirtualWindowCore::Guard guard{};
        guard.First = firstVisible;
        guard.Desired = desiredCount;
        guard.ItemCount = count;
        guard.ContentHPx = forcedHPx;
        guard.ViewportWPx = viewportWPx;
        guard.ContentWPx = contentWPx;
        guard.StructureGeneration = m_StructureGeneration;

        HostAdapter host(*this);
        const UI::VirtualWindowCore::Impact impact =
            m_WindowCore.Update(guard, reason, /*lanes=*/1, changesView, host,
                                /*allowShrink=*/!UIElement::IsInEventDispatch());

        // Forward the aggregated impact once (accepted delta from the pre-core code:
        // the per-cell notifies inside BindCell are gone). Impact mirrors
        // VirtualizationImpact bit-for-bit (static_asserted above).
        if (impact != UI::VirtualWindowCore::Impact::None)
        {
            if (UIManager* ui = GetOwnerManager())
                ui->NotifyVirtualizationImpact(static_cast<std::uint32_t>(impact));
        }

        m_DataDirty = false;
        return;
    }
}

void ListView::OnPostLayout()
{
    // Keep header aligned to current horizontal scroll (no vertical scroll).
    if (m_Scroll)
        UpdateHeaderScrollX(m_Scroll->GetScrollX());

    // Update header host height from measured content (absolute-positioned wrapper doesn't
    // contribute to parent height).
    bool headerChanged = false;
    if (m_HeaderHost && m_HeaderContent)
    {
        const int measuredHPx = RoundToPx(m_HeaderContent->GetLayoutHeight());
        if (measuredHPx > 0 && measuredHPx != m_HeaderHeightPx)
        {
            m_HeaderHeightPx = measuredHPx;
            UI::Layout::SetForcedHeight(*m_HeaderHost, m_HeaderHeightPx);
            headerChanged = true;
            // Ensure wrapper picks up the new height immediately.
            if (m_Scroll)
                UpdateHeaderScrollX(m_Scroll->GetScrollX());
        }
    }

    // Handle deferred scroll-to-end after layout is complete (before the early-out
    // so a pending scroll-to-end always takes effect on the first valid layout).
    if (m_PendingScrollToEnd)
    {
        m_PendingScrollToEnd = false;
        float viewportHeight = GetViewportHeight();
        if (viewportHeight > 0.0f)
        {
            float maxScroll = (float)m_TotalContentHeightPx - viewportHeight;
            if (maxScroll > 0.0f)
                SetScrollOffset(maxScroll);
        }
    }
    if (m_PendingScrollIntoViewIndex >= 0 && GetViewportHeight() > 0.0f)
    {
        const int index = m_PendingScrollIntoViewIndex;
        m_PendingScrollIntoViewIndex = -1;
        ScrollIndexIntoView(index);
    }

    // Coalesce viewport-changed work: if the viewport hasn't changed, the data isn't dirty,
    // and the header didn't change, avoid enqueuing a virtualization item every layout pass.
    // This is critical for editor idle perf -- without this guard the heavy path runs every
    // frame because OnPostLayout unconditionally re-enqueues coordinator work.
    const float viewportW = m_Scroll ? m_Scroll->GetViewportWidth() : 0.0f;
    const float viewportH = m_Scroll ? m_Scroll->GetViewportHeight() : 0.0f;
    const int viewportWPx = (int)std::lround(viewportW);
    const int viewportHPx = (int)std::lround(viewportH);
    const bool viewportChanged = (viewportWPx != m_LastViewportWPx) || (viewportHPx != m_LastViewportHPx);
    if (viewportChanged)
    {
        m_LastViewportWPx = viewportWPx;
        m_LastViewportHPx = viewportHPx;
    }

    if (!viewportChanged && !m_DataDirty && !headerChanged)
        return;

    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this, [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      { static_cast<ListView*>(ctx)->UpdateVirtualization(reason); }, VirtualizationCoordinator::Reason::ViewportChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::ViewportChanged);
    }
    // If this ListView was mounted into a container that is settling (dock/tab switches),
    // keep trying for a few ticks so pool sizing happens before the first scroll drag.
    SchedulePrewarmVirtualization();
}

void ListView::ForEachVisibleCell(std::function<void(UIElement* userContent)> fn) const
{
    if (!fn)
        return;
    for (const auto& slot : m_CellPool)
    {
        if (slot.userContent && slot.boundId != 0)
            fn(slot.userContent);
    }
}

void ListView::RefreshFromProvider()
{
    if (UIElement::IsInEventDispatch())
    {
        PostSafeAction([this]()
                       { RefreshFromProvider(); });
        return;
    }

    if (m_Provider)
    {
        ListChangeSet changes;
        m_Provider->ConsumeChanges(m_LastProviderChangeVersion, changes);
        if (changes.Version != m_LastProviderChangeVersion)
            m_LastProviderChangeVersion = changes.Version;

        if (changes.Kind == ChangeSetKind::Subset)
        {
            m_PendingChangeSet = std::move(changes);
            m_PendingChangeSetValid = true;
            if (UIManager* ui = GetOwnerManager())
            {
                ui->EnqueueVirtualizationWork(this, [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                              { static_cast<ListView*>(ctx)->UpdateVirtualization(reason); }, VirtualizationCoordinator::Reason::DataChanged);
            }
            else
            {
                UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
            }
            SchedulePrewarmVirtualization();
            MarkDirty(VisualDirty);
            return;
        }
    }

    ClearHover();
    m_PendingChangeSetValid = false;
    m_SelectedId = 0;
    m_SelectedIndex = -1;
    m_LastClickedId = 0;
    m_DataDirty = true;
    m_LayoutCacheDirty = true;
    // Drop the core's window/ring/pool bookkeeping; m_DataDirty forces a full rebind
    // on the next update.
    m_WindowCore.Reset();
    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this, [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      { static_cast<ListView*>(ctx)->UpdateVirtualization(reason); }, VirtualizationCoordinator::Reason::DataChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
    }
    SchedulePrewarmVirtualization();
    // Refresh updates bindings and virtualized content size. Avoid RequestRelayout() to prevent
    // conservative full-signature passes; layout signatures will drive solves when needed.
    MarkDirty(VisualDirty);
}

void ListView::InvalidateVirtualization()
{
    m_DataDirty = true;
    m_LayoutCacheDirty = true;
    // Drop the core's window/ring/pool bookkeeping; m_DataDirty forces a full rebind
    // on the next update (view was hidden/shown, no provider change).
    m_WindowCore.Reset();
    // Reset the OnPostLayout viewport-coalescing cache so the next layout re-enqueues.
    m_LastViewportWPx = -1;
    m_LastViewportHPx = -1;
    MarkDirty(LayoutDirty | VisualDirty);

    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this, [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      { static_cast<ListView*>(ctx)->UpdateVirtualization(reason); }, VirtualizationCoordinator::Reason::ViewportChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::ViewportChanged);
    }
    SchedulePrewarmVirtualization();
    RequestRelayout();
}

void ListView::OnEvent(UIEvent& e)
{
    if (e.Id == kEventKeyDown && m_Provider)
    {
        const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);
        if (primaryMod && e.Key == Input::kKeyCode_A)
        {
            if (!m_Selection)
            {
                e.Stop();
                return;
            }

            const int count = m_Provider->GetItemCount();
            if (count > 0)
            {
                std::vector<UI::Interaction::ItemId> ids;
                ids.reserve((size_t)count);
                for (int i = 0; i < count; ++i)
                {
                    const ListId id = m_Provider->GetItemId(i);
                    if (id != 0)
                        ids.push_back(id);
                }
                if (!ids.empty())
                {
                    const ListId anchor = ids.front();
                    m_Selection->SetSelection(ids, anchor);
                    m_SelectedId = anchor;
                    m_SelectedIndex = 0;
                    ApplySelectionStylesToPool();
                    if (m_OnSelectionChanged)
                        m_OnSelectionChanged(anchor);
                }
            }
            e.Stop();
            return;
        }

        // Standard list-control keys (Win32/WPF/Qt/AppKit): PageUp/Down move
        // the selection by one viewport page — variable row heights page by
        // viewport pixel height through the prefix-sum cache — Home/End go to
        // the extremes, and Shift extends from the anchor via the existing
        // keyboard range path.
        if (!primaryMod &&
            (e.Key == Input::kKeyCode_PageUp || e.Key == Input::kKeyCode_PageDown ||
             e.Key == Input::kKeyCode_Home || e.Key == Input::kKeyCode_End))
        {
            const int count = m_Provider->GetItemCount();
            if (count > 0)
            {
                int targetIndex = 0;
                if (e.Key == Input::kKeyCode_End)
                {
                    targetIndex = count - 1;
                }
                else if (e.Key != Input::kKeyCode_Home)
                {
                    if (m_LayoutCacheDirty || (int)m_CumulativeYPx.size() < count + 1)
                        RebuildLayoutCache();
                    // GetSelectedIndex, not m_SelectedIndex: async provider
                    // refreshes (e.g. the Assets git-status column) reset the
                    // raw index to -1 while the selection model keeps the real
                    // selection — paging must start from the recovered index.
                    const int cur = std::clamp(GetSelectedIndex(), 0, count - 1);
                    const int viewH = m_Scroll ? (int)m_Scroll->GetViewportHeight() : 0;
                    const int curTop = m_CumulativeYPx[(size_t)cur];
                    const int targetY = (e.Key == Input::kKeyCode_PageUp) ? curTop - viewH
                                                                          : curTop + viewH;
                    targetIndex = UI::VirtualWindowCore::FirstFromPrefixSum(
                        m_CumulativeYPx, targetY, count);
                    if (targetIndex == cur)
                        targetIndex = (e.Key == Input::kKeyCode_PageUp) ? cur - 1 : cur + 1;
                    targetIndex = std::clamp(targetIndex, 0, count - 1);
                }
                const bool shift = (e.Mods & Input::kModShift) != 0;
                SetSelectedIndexKeyboard(targetIndex, /*extendRange*/ shift, /*scrollIntoView*/ true);
            }
            e.Stop();
            return;
        }
    }

    UIElement::OnEvent(e);
}

void ListView::NotifyItemsAppended(int count)
{
    if (UIElement::IsInEventDispatch())
    {
        PostSafeAction([this, count]()
                       { NotifyItemsAppended(count); });
        return;
    }

    if (!m_Provider || count <= 0)
        return;

    // Incrementally extend the layout cache instead of rebuilding it
    const int totalCount = m_Provider->GetItemCount();
    const int oldCount = totalCount - count;

    // If cache is dirty or doesn't match expected size, fall back to full rebuild
    if (m_LayoutCacheDirty || static_cast<int>(m_CumulativeYPx.size()) != oldCount + 1)
    {
        m_LayoutCacheDirty = true;
        m_DataDirty = true;
        if (UIManager* ui = GetOwnerManager())
        {
            ui->EnqueueVirtualizationWork(this, [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                          { static_cast<ListView*>(ctx)->UpdateVirtualization(reason); }, VirtualizationCoordinator::Reason::DataChanged);
        }
        else
        {
            UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
        }
        MarkDirty(LayoutDirty | VisualDirty);
        return;
    }

    // Extend the cumulative Y array for new items
    int yPx = m_TotalContentHeightPx;
    for (int i = oldCount; i < totalCount; ++i)
    {
        const int hPx = RoundToPx(m_Provider->GetItemHeight(i));
        if (hPx > 0)
        {
            m_MinItemHeightPx = (m_MinItemHeightPx == 0) ? hPx : std::min(m_MinItemHeightPx, hPx);
        }
        yPx += hPx;
        m_CumulativeYPx.push_back(yPx);
    }
    m_TotalContentHeightPx = yPx;

    m_DataDirty = true;
    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this, [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      { static_cast<ListView*>(ctx)->UpdateVirtualization(reason); }, VirtualizationCoordinator::Reason::DataChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
    }
    SchedulePrewarmVirtualization();
    // Appends change bindings/content size. Layout-affecting changes are expressed via
    // Absolute override/ScrollView state (forced height/content size), so avoid marking
    // LayoutDirty here to keep scrollbar drags on the scroll-only path.
    MarkDirty(VisualDirty);
}

float ListView::GetScrollOffset() const
{
    return m_Scroll ? m_Scroll->GetScrollY() : 0.0f;
}

float ListView::GetScrollX() const
{
    return m_Scroll ? m_Scroll->GetScrollX() : 0.0f;
}

int ListView::GetItemCount() const
{
    return m_Provider ? m_Provider->GetItemCount() : 0;
}

int ListView::GetSelectedIndex() const
{
    if (m_SelectedIndex >= 0)
        return m_SelectedIndex;

    if (!m_Provider || !m_Selection)
        return m_SelectedIndex;

    const int count = m_Provider->GetItemCount();
    if (count <= 0)
        return m_SelectedIndex;

    const auto findIndexForId = [this, count](UI::Interaction::ItemId id) -> int
    {
        if (id == 0)
            return -1;
        for (int i = 0; i < count; ++i)
        {
            if (m_Provider->GetItemId(i) == static_cast<ListId>(id))
                return i;
        }
        return -1;
    };

    if (const int anchorIndex = findIndexForId(m_Selection->GetAnchor()); anchorIndex >= 0)
        return anchorIndex;

    for (UI::Interaction::ItemId id : m_Selection->GetSelection())
    {
        if (const int selectedIndex = findIndexForId(id); selectedIndex >= 0)
            return selectedIndex;
    }

    return m_SelectedIndex;
}

void ListView::ApplySelectionStylesToPool()
{
    for (auto& s : m_CellPool)
    {
        if (!s.cell)
            continue;
        const bool isSelected = (s.boundId != 0) && (m_Selection ? m_Selection->IsSelected(s.boundId) : (s.boundId == m_SelectedId));
        if (isSelected)
        {
            s.cell->AddClass("selected");
            if (s.userContent)
                s.userContent->AddClass("selected");
        }
        else
        {
            s.cell->RemoveClass("selected");
            if (s.userContent)
                s.userContent->RemoveClass("selected");
        }
    }
}

void ListView::SetSelectedIndexKeyboard(int index, bool extendRange, bool scrollIntoView)
{
    if (!m_Provider)
        return;
    const int count = m_Provider->GetItemCount();
    if (count <= 0)
        return;
    const int clamped = std::clamp(index, 0, count - 1);
    const ListId id = m_Provider->GetItemId(clamped);
    if (id == 0)
        return;

    if (extendRange && m_Selection)
    {
        if (m_SelectedIndex < 0)
            m_SelectedIndex = clamped;
        if (m_ShiftAnchorIndex < 0)
            m_ShiftAnchorIndex = m_SelectedIndex;

        const int a = m_ShiftAnchorIndex;
        const int b = clamped;
        if (a >= 0 && b >= 0 && a < count && b < count)
        {
            const int lo = std::min(a, b);
            const int hi = std::max(a, b);
            std::vector<UI::Interaction::ItemId> ids;
            ids.reserve((size_t)(hi - lo + 1));
            for (int i = lo; i <= hi; ++i)
            {
                const ListId rangeId = m_Provider->GetItemId(i);
                if (rangeId != 0)
                    ids.push_back(rangeId);
            }
            const ListId anchorId = m_Provider->GetItemId(a);
            m_Selection->SetSelection(ids, anchorId);
        }
        else
        {
            m_Selection->SetSingle(id);
            m_ShiftAnchorIndex = clamped;
        }
    }
    else
    {
        m_SelectedIndex = clamped;
        m_ShiftAnchorIndex = clamped;
        m_SelectedId = id;
        if (m_Selection)
            m_Selection->SetSingle(id);
    }

    // Keyboard-driven selection changes should not later collapse to a different
    // row on mouse-up, so clear any pending "collapse to single" gesture state.
    m_PendingCollapseToSingle = false;
    m_PendingCollapseId = 0;

    m_SelectedId = id;
    m_SelectedIndex = clamped;
    ApplySelectionStylesToPool();
    if (m_OnSelectionChanged)
        m_OnSelectionChanged(id);

    if (scrollIntoView)
        ScrollIndexIntoView(clamped);
}

void ListView::ScrollIndexIntoView(int index)
{
    if (!m_Provider || !m_Scroll)
        return;
    const int count = m_Provider->GetItemCount();
    if (count <= 0)
        return;
    const int clamped = std::clamp(index, 0, count - 1);

    const float viewH = m_Scroll->GetViewportHeight();
    if (viewH <= 0.0f)
    {
        m_PendingScrollIntoViewIndex = clamped;
        return;
    }

    // Only run the heavy pass when the scroll position actually changes.
    if (m_LayoutCacheDirty || (int)m_CumulativeYPx.size() < count + 1)
        RebuildLayoutCache();
    // The scroll view clamps against the height it was last told. After an append the cache
    // knows the new total but the scroll view still holds the old one, so a scroll toward the
    // new rows would clamp to the old range.
    PublishContentSize(m_Scroll->GetViewportWidth(), viewH);

    const float yTop = (float)m_CumulativeYPx[(size_t)clamped];
    const float yBottom = (float)m_CumulativeYPx[(size_t)clamped + 1];
    const float scrollY = m_Scroll->GetScrollY();

    float newScrollY = scrollY;
    if (yTop < scrollY)
        newScrollY = yTop;
    else if (yBottom > scrollY + viewH)
        newScrollY = std::max(0.0f, yBottom - viewH);

    const bool scrollActuallyChanges = (std::fabs(newScrollY - scrollY) > 0.5f);
    if (scrollActuallyChanges)
    {
        SetScrollOffset(newScrollY);
        m_DataDirty = true;
        RequestRelayout();
        // Do not call UpdateVirtualization() here — it stalls the caller (e.g. a key
        // handler). The scroll change triggers m_Scroll's SetOnScrollChanged callback,
        // which enqueues UpdateVirtualization, so visible cells update on the next
        // coordinator run.
    }
}

UIElement* ListView::GetCellForIndex(int index) const
{
    if (index < 0)
        return nullptr;
    for (const auto& slot : m_CellPool)
    {
        if (slot.cell && slot.boundIndex == index && slot.boundId != 0)
            return slot.cell;
    }
    return nullptr;
}

int ListView::GetIndexAtPoint(float x, float y) const
{
    // Same slot-rect geometry as HitTestDropTarget: wrappers are absolutely
    // positioned in window space.
    for (const auto& slot : m_CellPool)
    {
        if (!slot.cell || slot.boundId == 0 || slot.boundIndex < 0)
            continue;
        const float rx = slot.cell->GetLayoutX();
        const float ry = slot.cell->GetLayoutY();
        const float rw = slot.cell->GetLayoutWidth();
        const float rh = slot.cell->GetLayoutHeight();
        if (x >= rx && x < rx + rw && y >= ry && y < ry + rh)
            return slot.boundIndex;
    }
    return -1;
}

void ListView::SetSelectedIndex(int index, bool scrollIntoView)
{
    SetSelectedIndexKeyboard(index, /*extendRange*/ false, scrollIntoView);
}
void ListView::SetScrollOffset(float offset)
{
    if (m_Scroll)
        m_Scroll->SetScrollY(offset);
}

void ListView::SetContentWidth(float widthPx)
{
    m_ExplicitContentWidthPx = (widthPx > 0.0f) ? widthPx : -1.0f;
    if (m_Content)
    {
        if (m_ExplicitContentWidthPx > 0.0f)
            m_Content->Overrides().Set(Style::MinWidth, StyleLength::Px((float)std::lround(m_ExplicitContentWidthPx)));
        else
            m_Content->Overrides().Reset(Style::MinWidth);
    }
    // Update header wrapper rect immediately so column resize takes effect this frame
    // instead of waiting for OnPostLayout (which would use the previous frame's width).
    if (m_Scroll)
        UpdateHeaderScrollX(m_Scroll->GetScrollX());
    // Update each visible row wrapper's width so rows stay in sync with header during column resize.
    UpdateVisibleRowWidths();
    RequestRelayout();
}

void ListView::ScrollToEnd()
{
    // If we don't have a valid viewport yet, defer until after layout
    float viewportHeight = GetViewportHeight();
    if (viewportHeight <= 0.0f)
    {
        m_PendingScrollToEnd = true;
        return;
    }

    float maxScroll = (float)m_TotalContentHeightPx - viewportHeight;
    if (maxScroll > 0.0f)
        SetScrollOffset(maxScroll);
}

float ListView::GetViewportHeight() const
{
    return m_Scroll ? m_Scroll->GetViewportHeight() : 0.0f;
}

float ListView::GetViewportWidth() const
{
    return m_Scroll ? m_Scroll->GetViewportWidth() : 0.0f;
}

void ListView::SchedulePrewarmVirtualization()
{
    // Extend the warmup window on each call.
    m_PrewarmTicksRemaining = std::max(m_PrewarmTicksRemaining, 8);
    if (m_PrewarmScheduled)
        return;

    m_PrewarmScheduled = true;
    PostSafeAction([this]()
                   {
        m_PrewarmScheduled = false;
        if (m_PrewarmTicksRemaining <= 0)
            return;

        --m_PrewarmTicksRemaining;
        this->UpdateVirtualization(VirtualizationCoordinator::Reason::Prewarm);

        // Keep ticking until we have a meaningful viewport and the pool meets the last known desired size.
        const bool haveViewport = (m_Scroll && m_Scroll->GetViewportHeight() > 0.0f);
        const int desired = std::max(0, m_WindowCore.LastGuard().Desired);
        const bool poolOk = ((int)m_CellPool.size() >= desired);
        const bool shouldContinue =
            (!haveViewport) ||
            (!poolOk && m_Provider && m_Provider->GetItemCount() > 0);

        if (shouldContinue && m_PrewarmTicksRemaining > 0)
        {
            this->SchedulePrewarmVirtualization();
        } });
}

} // namespace GameEngine
