#include "UI/Controls/GridView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/VirtualWindowCore.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/ItemResizeGesture.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h"
#include "UI/VirtualizationCoordinator.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/SystemMetrics.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>

#include "Logger/Logger.h"

namespace GameEngine
{
namespace
{
constexpr float kMinIconSizePx = 32.0f;
constexpr float kMaxIconSizePx = 4096.0f;

// Cmd/Cmd+wheel icon delta: scales with current size so larger icons grow/shrink faster per tick.
float GridWheelIconSizeStepPx(float iconSizePx)
{
    constexpr float kBaseStepPx = 16.0f;
    constexpr float kRefIconPx = 80.0f;
    constexpr float kMinScale = 0.5f;
    constexpr float kMaxScale = 4.0f;
    return kBaseStepPx * std::clamp(iconSizePx / kRefIconPx, kMinScale, kMaxScale);
}
} // namespace

// VirtualWindowCore::Impact mirrors UIManager::VirtualizationImpact bit-for-bit
// so the aggregated impact forwards with a single cast.
static_assert(static_cast<std::uint32_t>(UI::VirtualWindowCore::Impact::Rebind) ==
                  UIManager::VirtualizationRebindChanged,
              "Impact::Rebind must match VirtualizationRebindChanged");
static_assert(static_cast<std::uint32_t>(UI::VirtualWindowCore::Impact::LayoutRects) ==
                  UIManager::VirtualizationLayoutRectsChanged,
              "Impact::LayoutRects must match VirtualizationLayoutRectsChanged");
static_assert(static_cast<std::uint32_t>(UI::VirtualWindowCore::Impact::Topology) ==
                  UIManager::VirtualizationTopologyChanged,
              "Impact::Topology must match VirtualizationTopologyChanged");

// Bridges the flat cell pool to the shared window engine. The core drives rows
// (lines) with the column count as the lane multiplier, so a flat slot is
// poolRow * cols + col and an item index is row * cols + col.
struct GridView::HostAdapter final : UI::VirtualWindowCore::Host
{
    GridView& Owner;
    int Cols;

    HostAdapter(GridView& owner, int cols) : Owner(owner), Cols(std::max(1, cols)) {}

    void EnsurePool(int slotCount) override
    {
        Owner.EnsureCellPoolSize(slotCount);
        Owner.UnbindSlotsFrom(slotCount);
    }
    int SlotCount() const override { return static_cast<int>(Owner.m_CellPool.size()); }
    UI::VirtualWindowCore::Impact Rebind(int slot, int itemIndex) override
    {
        const int row = itemIndex / Cols;
        const int col = itemIndex % Cols;
        return Owner.BindCell(slot, itemIndex, row, col) ? UI::VirtualWindowCore::Impact::Rebind
                                                         : UI::VirtualWindowCore::Impact::None;
    }
    void UnbindSlot(int slot) override { Owner.UnbindCell(slot); }
    bool SlotBinds(int slot, int itemIndex) const override
    {
        if (slot < 0 || slot >= static_cast<int>(Owner.m_CellPool.size()))
            return false;
        const CellSlot& s = Owner.m_CellPool[static_cast<size_t>(slot)];
        return s.boundId != 0 && s.boundIndex == itemIndex;
    }
    void DestroyTailSlot() override { Owner.DestroyTailSlot(); }
};

GridView::GridView()
{
    UpdateCellMetrics();

    auto sv = std::make_unique<ScrollView>();
    m_Scroll = sv.get();
    // GridView content is absolutely positioned and sized by virtualization.
    // - Keep the ScrollView filling available height (flex-grow).
    // - Disable ScrollView's default horizontal-overflow measurement: it scans labels and can
    //   overestimate content width for virtualized, absolutely positioned cells, which causes
    //   phantom horizontal scrollbars and flicker.
    m_Scroll->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MinWidth, StyleLength::Px(0.0f))
        .Set(Style::MinHeight, StyleLength::Px(0.0f));
    m_Scroll->Overrides().SetCustomNumber(StringId("--ui_scrollview_measure_horizontal"), 0.0f);
    m_Scroll->Overrides().SetCustomNumber(StringId("--ui_scrollview_primary_modifier_passthrough"), 1.0f);

    // Listen for scroll position changes to update virtualization. ScrollView defers
    // these callbacks to UIManager so they execute outside event dispatch.
    m_Scroll->SetOnScrollChanged([this](float /*scrollX*/, float /*scrollY*/)
                                 {
                                     // Avoid rebinding all pooled cells on every pixel of scroll.
                                     // Scrolling translates the content subtree; we only need to
                                     // rebind when the first visible row / viewport mapping changes.
                                     if (UIManager* ui = GetOwnerManager())
                                     {
                                         ui->EnqueueVirtualizationWork(this,
                                                                       [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                                                       {
                                                                           static_cast<GridView*>(ctx)->UpdateVirtualization(reason);
                                                                       },
                                                                       VirtualizationCoordinator::Reason::ScrollChanged);
                                     }
                                     else
                                     {
                                         UpdateVirtualization(VirtualizationCoordinator::Reason::ScrollChanged);
                                     }
                                     if (m_OnUserScrollChanged && m_Scroll)
                                         m_OnUserScrollChanged(m_Scroll->GetScrollY(), m_Scroll->GetContentHeight(), m_Scroll->GetViewportHeight());
                                 });

    auto content = std::make_unique<UIElement>();
    // Virtualized content container: no flex-wrap / no padding; cells are absolutely positioned.
    content->AddClass("grid-virtual-content");
    m_Content = content.get();

    // Clear hover when cursor is over empty space within content.
    m_Content->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
                                    {
        if (e.CurrentTarget == m_Content && e.Target == m_Content) {
            ClearHover();
        } });
    // Right-click on empty space: forward as id==0 so callers can show a directory menu.
    m_Content->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
                                    {
        if (e.Button != 1) return;
        if (e.CurrentTarget != m_Content || e.Target != m_Content) return;
        if (m_OnContextMenu) m_OnContextMenu(0, e.X, e.Y);
        e.Stop(); });
    // Left-click on empty space: clear selection (common file-browser UX).
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
            }
        }
        m_SelectedId = 0;
        m_SelectedIndex = -1;
        m_PendingCollapseToSingle = false;
        m_PendingCollapseId = 0;
        e.Stop(); });

    m_Scroll->AddContent(std::move(content));
    AddChild(std::move(sv));

    // Every item view answers the one host-configured item resize gesture.
    m_Scroll->RegisterEventHandler(kEventScroll, [this](UIEvent& e)
                                   {
        if (!UI::MatchesItemResizeGesture(e.Mods) || e.ScrollY == 0.0f)
            return;
        AdjustIconSizeFromScroll(e.ScrollY);
        e.Stop();
    });
}

GridView::~GridView()
{
    if (UIManager* registered = m_RegisteredVirtualizationManager.Get())
        registered->UnregisterVirtualizedControl(this);
}

void GridView::OnOwnerManagerChanged(UIManager* owner)
{
    if (UIManager* registered = m_RegisteredVirtualizationManager.Get())
        registered->UnregisterVirtualizedControl(this);
    m_RegisteredVirtualizationManager = UIManagerRef(owner);
    if (owner)
        owner->RegisterVirtualizedControl(this);
}

void GridView::EnqueuePumpWork(UIManager& ui)
{
    ui.EnqueueVirtualizationWork(this,
                                 [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                 { static_cast<GridView*>(ctx)->UpdateVirtualization(reason); },
                                 VirtualizationCoordinator::Reason::DataChanged);
}

void GridView::AutoScrollDuringDrag(float mouseX, float mouseY)
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

bool GridView::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return m_DropAccepts ? m_DropAccepts(typeId) : false;
}

bool GridView::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    // First try to hit a bound pooled cell.
    for (const auto& slot : m_CellPool)
    {
        if (!slot.cell || slot.boundId == 0)
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

    // If we're inside the GridView rect but not over a cell, allow "empty space" drop.
    const float gx = GetLayoutX();
    const float gy = GetLayoutY();
    const float gw = GetLayoutWidth();
    const float gh = GetLayoutHeight();
    if (x >= gx && x < gx + gw && y >= gy && y < gy + gh)
    {
        out.TargetId = 0;
        out.Location = UI::Interaction::DropLocation::OnEmptySpace;
        out.IndentDepth = 0;
        return true;
    }
    return false;
}

UI::Interaction::DropFeedback GridView::CanDrop(const UI::Interaction::DropRequest& request) const
{
    return m_OnCanDrop ? m_OnCanDrop(request) : UI::Interaction::DropFeedback{false, "No drop handler"};
}

void GridView::PerformDrop(const UI::Interaction::DropRequest& request)
{
    if (m_OnPerformDrop)
        m_OnPerformDrop(request);
}

void GridView::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    m_DropPreview = state;
    UpdateDropPreviewClasses();
}

void GridView::UpdateDropPreviewClasses()
{
    // Best-effort: only pooled visible cells will show highlight.
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
            if (m_DropPreview.Allowed) s.cell->AddClass("drop-allowed");
            else s.cell->AddClass("drop-denied");
        }
    }
}

void GridView::ClearHover()
{
    // Hover styling should be driven by CSS :hover (including ancestor hover semantics)
    // via UIManager's hoverTarget. Keep this for API compatibility but avoid
    // mutating classes on every mouse move (it causes expensive dirty propagation).
    m_LastHoverCell = nullptr;
}

float GridView::SnappedIconSize(float px) const
{
    const float clamped = std::clamp(px, kMinIconSizePx, kMaxIconSizePx);
    if (m_IconSizeStepPx <= 0.0f)
        return clamped;
    const float snapped = kMinIconSizePx + std::round((clamped - kMinIconSizePx) / m_IconSizeStepPx) * m_IconSizeStepPx;
    return std::clamp(snapped, kMinIconSizePx, kMaxIconSizePx);
}

float GridView::GrownIconSizeOnGrid(float px) const
{
    const float grown = SnappedIconSize(px + GridWheelIconSizeStepPx(px));
    // A step smaller than half the size grid would snap back to where it started.
    return grown > px ? grown : SnappedIconSize(px + m_IconSizeStepPx);
}

void GridView::SetIconSizeStep(float stepPx)
{
    m_IconSizeStepPx = std::max(0.0f, stepPx);
    SetIconSize(m_IconSize);
}

void GridView::SetIconSize(float px)
{
    const float next = SnappedIconSize(px);
    if (std::fabs(next - m_IconSize) < 0.5f)
        return;

    m_IconSize = next;
    UpdateCellMetrics();
    UpdateThumbSizes();
    UpdateSelectionOutlineGeometry();
    if (m_OnIconSizeChanged)
        m_OnIconSizeChanged(m_IconSize);
    m_DataDirty = true;

    // Icon size changes affect every pooled cell's geometry. Avoid dirtying the entire GridView tree;
    // instead, request a relayout + targeted geometry rebuild for the pooled cells (handled in OnPostLayout).
    m_NeedsGeometryRebuild = true;
    MarkDirty(LayoutDirty | VisualDirty);

    RequestRelayout();
    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this,
                                      [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      {
                                          static_cast<GridView*>(ctx)->UpdateVirtualization(reason);
                                      },
                                      VirtualizationCoordinator::Reason::DataChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
    }
}

void GridView::UpdateCellMetrics()
{
    constexpr float kCellPaddingW = 16.0f;
    constexpr float kCellPaddingH = 40.0f;
    m_CellW = m_IconSize + kCellPaddingW;
    m_CellH = m_IconSize + kCellPaddingH;
}

int GridView::SelectedIndexFromModel(int count) const
{
    // Recover the index from the selection model when the raw index is stale
    // (async provider refreshes reset it to -1 while the model keeps the real
    // selection).
    const int sel = m_SelectedIndex;
    if (sel >= 0 || !m_Selection)
        return sel;
    const auto anchor = m_Selection->GetAnchor();
    for (int i = 0; anchor != 0 && i < count; ++i)
    {
        if (m_Provider->GetItemId(i) == static_cast<GridId>(anchor))
            return i;
    }
    return sel;
}

Box4 GridView::GetContentPadding() const
{
    if (!m_Content)
        return {};
    // The stylesheet (.grid-virtual-content) is the single source of truth for
    // the content inset. Cells are absolutely positioned, so Yoga never applies
    // this padding itself; the virtualization math consumes it instead.
    Box4 padding = m_Content->GetResolvedStyle().Layout.Padding;
    padding.Left = std::max(0.0f, padding.Left);
    padding.Top = std::max(0.0f, padding.Top);
    padding.Right = std::max(0.0f, padding.Right);
    padding.Bottom = std::max(0.0f, padding.Bottom);
    return padding;
}

void GridView::UpdateThumbSizes()
{
    const float titleW = TitleWidthPx();
    const float titleH = TitleHeightPx();
    for (auto& slot : m_CellPool)
    {
        if (slot.thumb)
        {
            slot.thumb->Overrides()
                .Set(Style::Width, StyleLength::Px((float)std::lround(m_IconSize)))
                .Set(Style::Height, StyleLength::Px((float)std::lround(m_IconSize)));
        }
        if (slot.title)
        {
            slot.title->Overrides()
                .Set(Style::Width, StyleLength::Px(titleW))
                .Set(Style::Height, StyleLength::Px(titleH));
        }
    }
}

// The title box is cell geometry, owned here like the thumb box: its width is
// a fraction of the cell and its height is the whole band below the icon. The
// stylesheet spaces the text inside that box (padding) and never sizes it.
float GridView::TitleWidthPx() const
{
    constexpr float kTitleWidthFraction = 0.9f;
    return (float)std::lround(m_CellW * kTitleWidthFraction);
}

float GridView::TitleHeightPx() const
{
    return m_CellH - m_IconSize;
}

void GridView::UpdateSelectionOutlineGeometry()
{
    const float outlineSize = static_cast<float>(std::lround(m_IconSize));
    const float outlineX = static_cast<float>(std::lround((m_CellW - m_IconSize) * 0.5f));

    for (auto& slot : m_CellPool)
    {
        if (!slot.selectionOutline)
            continue;

        UI::Layout::SetAbsolutePosition(*slot.selectionOutline,
                                        Mathematics::Rect{
                                            outlineX,
                                            0.0f,
                                            outlineSize,
                                            outlineSize,
                                        });
    }
}

void GridView::AdjustIconSizeFromScroll(float scrollY)
{
    if (scrollY == 0.0f)
        return;

    // One discrete step per delivered scroll event, whatever its size, so a large trackpad
    // delta cannot skip sizes. Wheel up (negative scrollY) grows the icons.
    const bool grow = scrollY < 0.0f;
    if (m_IconSizeStepPx <= 0.0f)
    {
        const float stepPx = GridWheelIconSizeStepPx(m_IconSize);
        SetIconSize(m_IconSize + (grow ? stepPx : -stepPx));
        return;
    }
    if (grow)
    {
        SetIconSize(GrownIconSizeOnGrid(m_IconSize));
        return;
    }
    // The smallest grid size one step up from which reaches this size, so a detent down undoes a
    // detent up. A step up grows strictly on the grid, so that size is the one the step came from.
    float shrunk = SnappedIconSize(m_IconSize - m_IconSizeStepPx);
    while (shrunk > kMinIconSizePx &&
           GrownIconSizeOnGrid(SnappedIconSize(shrunk - m_IconSizeStepPx)) >= m_IconSize)
        shrunk = SnappedIconSize(shrunk - m_IconSizeStepPx);
    SetIconSize(shrunk);
}

void GridView::EnsureCellPoolSize(int desired)
{
    if (!m_Content || desired <= 0)
        return;

    while ((int)m_CellPool.size() < desired)
    {
        const int slotIndex = (int)m_CellPool.size();

        auto cell = std::make_unique<UIElement>();
        cell->AddClass("grid-cell");
        // Pooled cells must never participate in flex layout flow. If a slot is still unbound
        // (e.g. during startup or transient scroll states) and does not have an absolute rect,
        // Yoga can lay it out as a regular flex child. Because we toggle visibility via opacity,
        // those "flow" slots become invisible spacers that look like missing/blank rows.
        //
        // Make every pooled cell absolute immediately; BindCell will later place it correctly.
        UI::Layout::SetAbsolutePosition(*cell,
                                        Mathematics::Rect{
                                            0.0f,
                                            0.0f,
                                            static_cast<float>(std::lround(m_CellW)),
                                            static_cast<float>(std::lround(m_CellH)),
                                        });
        UI::Layout::SetElementInvisible(*cell, true);
        UIElement* cellRaw = cell.get();

        auto thumb = std::make_unique<UIElement>();
        thumb->AddClass("thumb");
        thumb->Overrides()
            .Set(Style::Width, StyleLength::Px((float)std::lround(m_IconSize)))
            .Set(Style::Height, StyleLength::Px((float)std::lround(m_IconSize)));
        UIElement* thumbRaw = thumb.get();

        auto title = std::make_unique<Label>();
        title->AddClass("grid-title");
        // Definite px size on both axes: Label::SetText then takes its
        // content-only fast path, keeping rebind scroll ticks from raising
        // LayoutDirty (which forces a whole-tree Yoga rebuild pre-solve).
        title->Overrides()
            .Set(Style::Width, StyleLength::Px(TitleWidthPx()))
            .Set(Style::Height, StyleLength::Px(TitleHeightPx()));
        Label* titleRaw = title.get();

        // Virtualized pool elements must keep their geometry even when fully clipped.
        // UIManager scrolls by translating cached geometry; if offscreen items are culled during
        // geometry generation, they can appear blank when they later scroll into view.
        cellRaw->SetDisableClipCulling(true);
        thumbRaw->SetDisableClipCulling(true);
        titleRaw->SetDisableClipCulling(true);

        // Route keyboard focus from children to the GridView so key repeats reach it.
        cellRaw->SetFocusProxy(this);
        thumbRaw->SetFocusProxy(this);
        titleRaw->SetFocusProxy(this);

        cellRaw->AddChild(std::move(thumb));
        cellRaw->AddChild(std::move(title));

        auto selectionOutline = std::make_unique<UIElement>();
        selectionOutline->AddClass("grid-cell-selection-outline");
        UIElement* selectionOutlineRaw = selectionOutline.get();
        const float outlineX = (m_CellW - m_IconSize) * 0.5f;
        UI::Layout::SetAbsolutePosition(*selectionOutlineRaw,
                                        Mathematics::Rect{
                                            outlineX,
                                            0.0f,
                                            static_cast<float>(std::lround(m_IconSize)),
                                            static_cast<float>(std::lround(m_IconSize)),
                                        });
        selectionOutlineRaw->SetDisableClipCulling(true);
        selectionOutlineRaw->SetFocusProxy(this);
        cellRaw->AddChild(std::move(selectionOutline));

        // NOTE: We intentionally do not toggle a ".hover" class here.
        // CSSParser already implements ancestor :hover semantics when UIManager passes
        // hoverTarget, so `.grid-cell:hover .thumb` rules work without manual class churn.

        // Selection / activation / context menu / drag.
        cellRaw->RegisterEventHandler(kEventMouseDown, [this, cellRaw, slotIndex](UIEvent& e)
                                      {
            if (e.Button != 0)
                return;
            if (slotIndex < 0 || slotIndex >= (int)m_CellPool.size())
                return;
            const GridId gid = m_CellPool[slotIndex].boundId;
            if (gid == 0)
                return;

            // A slow second press on the item that is already the sole selection is
            // a rename request, honoured on release unless the press becomes a drag.
            // Modified clicks are selection edits and never rename.
            {
                const bool soleSelected = m_Selection
                    ? (m_Selection->IsSelected(gid) && m_Selection->GetSelection().size() == 1)
                    : (m_SelectedId == gid);
                const bool slowRepeat = (m_LastClickedId == gid) &&
                    ((std::chrono::steady_clock::now() - m_LastClickTime) >= Platform::GetDoubleClickInterval());
                m_PendingRenameRequest =
                    soleSelected && slowRepeat && (e.Mods & Input::kModShortcutMask) == 0;
            }

            // Ensure selection is established on mouse-down so drag payload builders
            // can include the clicked item even before mouse-up.
            if (m_Selection)
            {
                const bool shift = (e.Mods & Input::kModShift) != 0;
                const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);
                if (shift && m_Provider)
                {
                    m_PendingCollapseToSingle = false;
                    m_PendingCollapseId = 0;
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
                            const GridId id = m_Provider->GetItemId(i);
                            if (id != 0)
                                ids.push_back(id);
                        }
                        if (primaryMod)
                        {
                            const auto cur = m_Selection->GetSelection();
                            ids.insert(ids.end(), cur.begin(), cur.end());
                        }
                        const GridId anchorId = m_Provider->GetItemId(a);
                        m_Selection->SetSelection(ids, anchorId);
                    }
                    else
                    {
                        m_Selection->SetSingle(gid);
                    }
                }
                else if (primaryMod)
                {
                    m_PendingCollapseToSingle = false;
                    m_PendingCollapseId = 0;
                    m_Selection->Toggle(gid);
                    m_ShiftAnchorIndex = m_CellPool[slotIndex].boundIndex;
                }
                else
                {
                    // Preserve multi-selection when clicking a selected item.
                    if (m_Selection->IsSelected(gid))
                    {
                        const auto cur = m_Selection->GetSelection();
                        if (cur.size() > 1)
                        {
                            m_Selection->SetAnchor(gid);
                            // If this turns out to be a click (not a drag), collapse to single on mouse-up.
                            m_PendingCollapseToSingle = true;
                            m_PendingCollapseId = gid;
                        }
                        else
                        {
                            m_Selection->SetSingle(gid);
                            m_PendingCollapseToSingle = false;
                            m_PendingCollapseId = 0;
                        }
                    }
                    else
                    {
                        m_Selection->SetSingle(gid);
                        m_PendingCollapseToSingle = false;
                        m_PendingCollapseId = 0;
                    }
                    m_ShiftAnchorIndex = m_CellPool[slotIndex].boundIndex;
                }

                // Track "primary" selection for keyboard-style operations, but keep visuals
                // driven by the selection model (supports multi-select).
                m_SelectedId = gid;
                m_SelectedIndex = m_CellPool[slotIndex].boundIndex;

                // Update selected visuals immediately (best-effort for pooled, visible cells).
                for (auto& s : m_CellPool)
                {
                    if (!s.cell) continue;
                    const bool sel = (s.boundId != 0) && (m_Selection ? m_Selection->IsSelected(s.boundId) : (s.boundId == m_SelectedId));
                    if (sel) s.cell->AddClass("selected");
                    else s.cell->RemoveClass("selected");
                }
            }
            else
            {
                // Fallback single-select when no selection model is attached.
                m_SelectedId = gid;
                m_SelectedIndex = m_CellPool[slotIndex].boundIndex;
                for (auto& s : m_CellPool)
                {
                    if (!s.cell) continue;
                    const bool sel = (s.boundId != 0) && (s.boundId == m_SelectedId);
                    if (sel) s.cell->AddClass("selected");
                    else s.cell->RemoveClass("selected");
                }
            }

            PostSafeAction([this]() {
                if (UIManager* ui = GetOwnerManager())
                    ui->FocusElement(this);
            });

            // Arm drag gesture (drag begins on move threshold).
            m_DragCandidate = true;
            m_DragStartedThisGesture = false;
            m_DragStartX = e.X;
            m_DragStartY = e.Y;
            e.Capture(cellRaw);
        });

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
            constexpr float kThresh = 5.0f;
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
            e.Stop();
        });

        cellRaw->RegisterEventHandler(kEventMouseUp, [this, slotIndex](UIEvent& e)
                                      {
            // If this gesture became a drag, suppress click behavior.
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
            const GridId gid = m_CellPool[slotIndex].boundId;
            if (gid == 0) return;

            // Right click
            if (e.Button == 1) {
                if (m_OnContextMenu) m_OnContextMenu(gid, e.X, e.Y);
                e.Stop();
                return;
            }
            if (e.Button != 0) return;

            // If user clicked on a selected item within a multi-selection, collapse to single on mouse-up.
            if (m_Selection && m_PendingCollapseToSingle && m_PendingCollapseId == gid)
            {
                const bool shift = (e.Mods & Input::kModShift) != 0;
                const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);
                if (!shift && !primaryMod)
                {
                    m_Selection->SetSingle(gid);
                    for (auto& s : m_CellPool)
                    {
                        if (!s.cell) continue;
                        const bool sel = (s.boundId != 0) && m_Selection->IsSelected(s.boundId);
                        if (sel) s.cell->AddClass("selected");
                        else s.cell->RemoveClass("selected");
                    }
                }
            }
            m_PendingCollapseToSingle = false;
            m_PendingCollapseId = 0;

            using clock = std::chrono::steady_clock;
            auto now = clock::now();
            bool isDouble =
                (m_LastClickedId == gid) &&
                ((now - m_LastClickTime) < Platform::GetDoubleClickInterval());
            m_LastClickedId = gid;
            m_LastClickTime = now;
            const bool renameRequested = m_PendingRenameRequest;
            m_PendingRenameRequest = false;
            // A picker popup commits on the first click: it exists only to choose
            // one cell, so requiring a second click reads as a dead control.
            if (isDouble || m_PickerActivation) {
                if (m_OnItemActivated) m_OnItemActivated(gid);
                e.Stop();
                return;
            }

            m_SelectedId = gid;
            m_SelectedIndex = m_CellPool[slotIndex].boundIndex;

            if (renameRequested && m_OnItemRenameRequested)
                m_OnItemRenameRequested(gid);
            e.Stop(); });

        if (m_Content)
        {
            m_Content->AddChild(std::move(cell));
        }

        CellSlot slot{};
        slot.cell = cellRaw;
        slot.thumb = thumbRaw;
        slot.title = titleRaw;
        slot.selectionOutline = selectionOutlineRaw;
        slot.boundId = 0;
        slot.boundIndex = -1;
        m_CellPool.push_back(std::move(slot));
    }
}

// Excess cells are unbound in place, never detached from the tree and never
// display:none'd: mid-frame tree mutations during scroll-driven virtualization
// updates cause flicker from stale traversal caches. UnbindCell keeps the cell
// attached and paint/input-invisible instead.
void GridView::UnbindSlotsFrom(int firstSlot)
{
    for (int i = 0; i < (int)m_CellPool.size(); ++i)
    {
        CellSlot& slot = m_CellPool[(size_t)i];
        if (!slot.cell)
            continue;

        // Safety: older pooled slots may exist without an absolute rect (from
        // before the fix in EnsureCellPoolSize), or may have been created by
        // future template factories. Ensure they never enter flex flow.
        Mathematics::Rect currentRect{};
        if (!UI::Layout::TryGetAbsolutePosition(*slot.cell, currentRect))
        {
            UI::Layout::SetAbsolutePosition(*slot.cell,
                                            Mathematics::Rect{
                                                0.0f,
                                                0.0f,
                                                static_cast<float>(std::lround(m_CellW)),
                                                static_cast<float>(std::lround(m_CellH)),
                                            });
            if (slot.boundId == 0)
                UI::Layout::SetElementInvisible(*slot.cell, true);
        }

        if (i >= firstSlot)
            UnbindCell(i);
    }
}

void GridView::UnbindCell(int slot)
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
    s.boundId = 0;
    s.boundIndex = -1;
    UI::Layout::SetElementInvisible(*s.cell, true);
    // Mark cell dirty to clear any stale cached geometry.
    s.cell->MarkDirty(LayoutDirty | VisualDirty);
}

void GridView::DestroyTailSlot()
{
    if (m_CellPool.empty())
        return;
    CellSlot& slot = m_CellPool.back();
    if (m_Content && slot.cell)
        (void)m_Content->TakeChild(slot.cell);
    m_CellPool.pop_back();
}

bool GridView::BindCell(int slotIndex, int itemIndex, int row, int col)
{
    if (slotIndex < 0 || slotIndex >= (int)m_CellPool.size())
        return false;

    CellSlot& slot = m_CellPool[slotIndex];
    if (!slot.cell)
        return false;
    const GridId prevId = slot.boundId;
    const int prevIndex = slot.boundIndex;

    if (!m_Provider || itemIndex < 0 || itemIndex >= m_Provider->GetItemCount())
    {
        UnbindCell(slotIndex);
        return false;
    }

    const GridId gid = m_Provider->GetItemId(itemIndex);
    const bool bindingChanged = (prevId != gid) || (prevIndex != itemIndex);
    if (bindingChanged && prevId != 0)
    {
        if (UIManager* ui = GetOwnerManager())
            ui->ClearHoverForSubtree(slot.cell);
    }
    slot.boundId = gid;
    slot.boundIndex = itemIndex;

    const float strideX = m_CellW + m_Gap;
    const float strideY = m_CellH + m_Gap;
    const Box4 padding = GetContentPadding();
    const float x = padding.Left + (float)col * strideX;
    const float y = padding.Top + (float)row * strideY;
    UI::Layout::SetElementInvisible(*slot.cell, false);
    UI::Layout::SetAbsolutePosition(*slot.cell,
                                    Mathematics::Rect{
                                        static_cast<float>(std::lround(x)),
                                        static_cast<float>(std::lround(y)),
                                        static_cast<float>(std::lround(m_CellW)),
                                        static_cast<float>(std::lround(m_CellH)),
                                    },
                                    /*positionOnlyFastPath=*/true);

    // Stable ids for safe async updates (e.g. thumbnails).
    auto sid = std::string("gridcell-") + std::to_string((uint64_t)(std::uintptr_t)this) + "-" + std::to_string((uint64_t)gid);
    slot.cell->SetId(sid);

    const bool isSelected = (gid != 0) && (m_Selection ? m_Selection->IsSelected(gid) : (gid == m_SelectedId));
    if (isSelected)
        slot.cell->AddClass("selected");
    else
        slot.cell->RemoveClass("selected");

    slot.cell->RemoveClass("drop-hover");
    slot.cell->RemoveClass("drop-allowed");
    slot.cell->RemoveClass("drop-denied");
    if (m_DropPreview.Visible &&
        m_DropPreview.Hit.Location == UI::Interaction::DropLocation::OnItem &&
        gid != 0 &&
        gid == m_DropPreview.Hit.TargetId)
    {
        slot.cell->AddClass("drop-hover");
        if (m_DropPreview.Allowed)
            slot.cell->AddClass("drop-allowed");
        else
            slot.cell->AddClass("drop-denied");
    }

    // Update cell content via binder or direct text set
    if (m_ItemBinder)
    {
        m_ItemBinder(slot.cell, gid, m_Provider);
    }
    else if (slot.title)
    {
        const char* text = m_Provider->GetLabel(gid);
        slot.title->SetText(text ? std::string(text) : std::string(""));
    }
    
    // MarkDirtySubtree ensures the entire cell subtree gets StyleDirty so
    // descendant labels, icons, and thumbnails pick up the cell's new class
    // state via CSS selectors like `.grid-item.selected .title`. Also
    // required on a data refresh (m_DataDirty): a cell can rebind the same
    // id/index while the underlying content changed. No blanket LayoutDirty:
    // the binder's own mutations mark layout dirt precisely where
    // measurement/position changed, so pure recycling stays out of the Yoga
    // rebuild path. Avoid RequestRelayout() to prevent conservative
    // full-signature passes that create a late-relayout feedback loop.
    const bool markDirty = (bindingChanged || m_DataDirty);
    if (markDirty)
        slot.cell->MarkDirtySubtree(StyleDirty | VisualDirty);
    return markDirty;
}

void GridView::UpdateVirtualization(VirtualizationCoordinator::Reason reason)
{
    if (!m_Content)
        return;

    if (!m_Provider)
    {
        UnbindSlotsFrom(0);
        UI::Layout::SetForcedHeight(*m_Content, 0);
        m_DataDirty = false;
        m_WindowCore.Reset();
        m_LastColumns = -1;
        return;
    }

    if (!m_Scroll)
        return;

    const float viewportW = m_Scroll->GetViewportWidth();
    const float viewportH = m_Scroll->GetViewportHeight();
    if (viewportW <= 0.0f || viewportH <= 0.0f)
        return;

    const float strideX = m_CellW + m_Gap;
    const float strideY = m_CellH + m_Gap;
    const Box4 padding = GetContentPadding();

    const int cols = std::max(1, (int)std::floor(
        (viewportW - padding.Left - padding.Right + m_Gap) / strideX));
    const bool colsChangedFromLast = (m_LastColumns >= 0 && cols != m_LastColumns);
    if (cols != m_Columns)
    {
        m_Columns = cols;
        m_DataDirty = true;
    }

    const int count = m_Provider->GetItemCount();
    const int totalRows = (count > 0) ? ((count + cols - 1) / cols) : 0;

    const float totalH =
        (totalRows <= 0)
            ? 0.0f
            : (padding.Top + padding.Bottom + (float)totalRows * m_CellH + (float)std::max(0, totalRows - 1) * m_Gap);

    const int totalHPx = (int)totalH;
    // Ensure the content element covers the viewport so RMB on "empty space" works
    // even when there are few/no items (without forcing scrollbars).
    const int forcedHPx = std::max(0, std::max(totalHPx, (int)viewportH));
    UI::Layout::SetForcedHeight(*m_Content, forcedHPx);

    // Explicitly provide scroll extents. Yoga often constrains the internal scroll-content
    // root to the viewport size, so relying on layout rects can prevent scrollbars from
    // appearing for virtualized content.
    // GridView virtualization drives height and never needs horizontal scrolling:
    // cell x positions are derived from viewport width (columns), and titles clip.
    // Keeping contentW == viewportW avoids a persistent horizontal scrollbar.
    m_Scroll->SetContentSize(viewportW, totalH);

    const float scrollY = m_Scroll->GetScrollY();
    int anchorRow = (int)std::floor((scrollY - padding.Top) / strideY);
    if (totalRows > 0)
        anchorRow = std::clamp(anchorRow, 0, totalRows - 1);
    else
        anchorRow = 0;

    // Virtualization window:
    // - +1 visible row to cover partial rows at viewport edges (avoids a "blank sliver" at the bottom).
    // - Symmetric overscan (rows above and below) to tolerate rounding/jitter around row boundaries.
    const int overscanAbove = std::max(0, m_OverscanRows);
    const int overscanBelow = std::max(0, m_OverscanRows);
    const int visibleRows = (int)std::ceil(viewportH / strideY) + 1;
    const int desiredRows = std::max(0, visibleRows + overscanAbove + overscanBelow);
    int firstRow = std::max(0, anchorRow - overscanAbove);
    if (totalRows > 0 && desiredRows > 0)
    {
        const int maxFirst = std::max(0, totalRows - desiredRows);
        firstRow = std::clamp(firstRow, 0, maxFirst);
    }
    // Pull the typed provider changeset and normalize it for the core, which
    // never sees id types: All -> full rebind; Subset -> the flat pool slots
    // currently bound to a changed id (the old Subset fast-path scan, reused).
    GridChangeSet changes{};
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
    // any changeset kind — items were added/removed/reordered/regrouped even if
    // the reported ids are a Subset (or None). Treat structure-version movement
    // like an All change: force the full rebind of the window.
    if (changes.StructureVersion != m_LastProviderStructureVersion)
    {
        m_LastProviderStructureVersion = changes.StructureVersion;
        m_DataDirty = true;
    }

    // An All change means same-id cells may carry new content, so force the
    // subtree-refresh gate in BindCell (mirrors the pre-core forceFullRebind).
    if (changes.Kind == ChangeSetKind::All)
        m_DataDirty = true;

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
            const GridId bound = m_CellPool[(size_t)slot].boundId;
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
    // covers provider swap, sort/grouping, refresh, icon-size, and cols change.
    if (m_DataDirty)
        ++m_StructureGeneration;

    UI::VirtualWindowCore::Guard guard{};
    guard.First = firstRow;
    guard.Desired = desiredRows;
    guard.ItemCount = count;
    guard.ContentHPx = forcedHPx;
    guard.ViewportWPx = (int)viewportW;
    guard.ContentWPx = (int)viewportW;
    guard.StructureGeneration = m_StructureGeneration;

    HostAdapter host(*this, cols);
    const UI::VirtualWindowCore::Impact impact =
        m_WindowCore.Update(guard, reason, cols, changesView, host,
                            /*allowShrink=*/!UIElement::IsInEventDispatch());

    // GridView now reports the aggregated impact once (accepted delta: it never
    // notified before, compensating with MarkDirty/RequestRelayout). Impact
    // mirrors VirtualizationImpact bit-for-bit (static_asserted above).
    if (impact != UI::VirtualWindowCore::Impact::None)
    {
        if (UIManager* ui = GetOwnerManager())
            ui->NotifyVirtualizationImpact(static_cast<std::uint32_t>(impact));
    }

    m_DataDirty = false;
    m_LastColumns = cols;

    // When the viewport width changes (e.g. splitter drag), the column count can
    // change. Pooled cell rects are repositioned in OnPostLayout (after the Yoga
    // solve); force a late Yoga pass this frame so the new row/col wrapping is
    // reflected in the final layout/paint ordering (else cells keep the previous
    // column mapping and clip out to the right until input release).
    if (colsChangedFromLast)
    {
        RequestRelayout();
    }
}

void GridView::OnPostLayout()
{
    bool needsRebuild = m_NeedsGeometryRebuild;
    m_NeedsGeometryRebuild = false;
    
    float viewportW = m_Scroll ? m_Scroll->GetViewportWidth() : 0.0f;
    float viewportH = m_Scroll ? m_Scroll->GetViewportHeight() : 0.0f;
    const int viewportWPx = (int)std::lround(viewportW);
    const int viewportHPx = (int)std::lround(viewportH);
    const bool viewportChanged = (viewportWPx != m_LastViewportWPx) || (viewportHPx != m_LastViewportHPx);
    
    // Force geometry rebuild on first valid layout to fix initial corruption
    if (!m_FirstLayoutDone && viewportW > 0.0f && viewportH > 0.0f && !m_CellPool.empty())
    {
        m_FirstLayoutDone = true;
        needsRebuild = true;
        // Force data dirty to ensure complete rebind of all cells
        m_DataDirty = true;
    }

    if (!viewportChanged && !needsRebuild && !m_DataDirty)
        return;
    
    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this,
                                      [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      {
                                          static_cast<GridView*>(ctx)->UpdateVirtualization(reason);
                                      },
                                      VirtualizationCoordinator::Reason::ViewportChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::ViewportChanged);
    }

    // Track last viewport size seen by post-layout so we can coalesce future calls.
    m_LastViewportWPx = viewportWPx;
    m_LastViewportHPx = viewportHPx;
    
    // If a refresh was requested, force full geometry rebuild AFTER virtualization
    // so all newly bound cells get properly marked dirty
    if (needsRebuild)
    {
        // Targeted: mark pooled cells dirty (and their children) so retained geometry is rebuilt,
        // without cascading dirties through unrelated UI.
        auto markDirtySubtree = [](UIElement* root)
        {
            if (!root)
                return;
            std::vector<UIElement*> stack;
            stack.reserve(64);
            stack.push_back(root);
            while (!stack.empty())
            {
                UIElement* el = stack.back();
                stack.pop_back();
                if (!el)
                    continue;
                el->MarkDirty(LayoutDirty | VisualDirty);
                for (const auto& ch : el->GetChildren())
                {
                    if (ch)
                        stack.push_back(ch.get());
                }
            }
        };
        for (auto& slot : m_CellPool)
        {
            if (slot.cell)
                markDirtySubtree(slot.cell);
        }
        // Ensure the dirty flags are observed promptly.
        RequestRelayout();
    }
}

Label* GridView::GetTitleLabelForIndex(int index) const
{
    if (index < 0)
        return nullptr;
    for (const auto& slot : m_CellPool)
    {
        if (slot.cell && slot.boundIndex == index && slot.boundId != 0)
            return slot.title;
    }
    return nullptr;
}

void GridView::RefreshFromProvider()
{
    if (UIElement::IsInEventDispatch())
    {
        this->PostSafeAction([this]()
                         { this->RefreshFromProvider(); });
        return;
    }

    if (m_Provider)
    {
        GridChangeSet changes;
        m_Provider->ConsumeChanges(m_LastProviderChangeVersion, changes);
        if (changes.Version != m_LastProviderChangeVersion)
            m_LastProviderChangeVersion = changes.Version;

        if (changes.Kind == ChangeSetKind::Subset)
        {
            m_PendingChangeSet = std::move(changes);
            m_PendingChangeSetValid = true;
            if (UIManager* ui = GetOwnerManager())
            {
                ui->EnqueueVirtualizationWork(this,
                                              [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                              {
                                                  static_cast<GridView*>(ctx)->UpdateVirtualization(reason);
                                              },
                                              VirtualizationCoordinator::Reason::DataChanged);
            }
            else
            {
                UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
            }
            MarkDirty(VisualDirty);
            return;
        }
    }

    ClearHover();
    m_PendingChangeSetValid = false;
    // Treat refresh as a dataset change (e.g., folder navigation). Clear selection
    // so it doesn't stick to unrelated items when ids are reused.
    m_SelectedId = 0;
    m_SelectedIndex = -1;
    m_LastClickedId = 0;
    m_DataDirty = true;
    m_NeedsGeometryRebuild = true; // Request full geometry rebuild after layout
    
    // Reset first-layout flag to ensure full rebuild on next valid layout
    // This is critical when switching projects - ensures first-layout path triggers
    m_FirstLayoutDone = false;

    // Drop the core's window/ring/pool bookkeeping so it matches the manual pool
    // clear below; m_DataDirty then forces a full rebind on the next update.
    m_WindowCore.Reset();
    m_LastColumns = -1;

    // Explicitly hide and unbind ALL cells in the pool first
    // This prevents stale cells from old dataset interfering with new layout
    for (auto& slot : m_CellPool)
    {
        if (!slot.cell)
            continue;
        slot.boundId = 0;
        slot.boundIndex = -1;
        UI::Layout::SetElementInvisible(*slot.cell, true);
        // Reset position to origin to prevent stale positions affecting layout
        UI::Layout::SetAbsolutePosition(*slot.cell,
                                        Mathematics::Rect{
                                            0.0f,
                                            0.0f,
                                            static_cast<float>(std::lround(m_CellW)),
                                            static_cast<float>(std::lround(m_CellH)),
                                        });
        // Clear title text to prevent stale labels showing after rebind
        if (slot.title)
        {
            slot.title->SetText("");
            slot.title->MarkDirty(LayoutDirty | VisualDirty);
        }
        // Mark cell dirty
        slot.cell->MarkDirty(LayoutDirty | VisualDirty);
    }
    MarkDirty(LayoutDirty | VisualDirty);

    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this,
                                      [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      {
                                          static_cast<GridView*>(ctx)->UpdateVirtualization(reason);
                                      },
                                      VirtualizationCoordinator::Reason::DataChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
    }

    // Ensure pooled cells rebuild geometry after rebinding.
    m_NeedsGeometryRebuild = true;
    RequestRelayout();
    
}

void GridView::InvalidateVirtualization()
{
    m_DataDirty = true;
    m_NeedsGeometryRebuild = true;
    m_FirstLayoutDone = false;
    // Drop the core's window/ring/pool bookkeeping; m_DataDirty forces a full
    // rebind on the next update (view was hidden/shown, no provider change).
    m_WindowCore.Reset();
    m_LastColumns = -1;
    m_LastViewportWPx = -1;
    m_LastViewportHPx = -1;
    MarkDirty(LayoutDirty | VisualDirty);

    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this,
                                      [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      {
                                          static_cast<GridView*>(ctx)->UpdateVirtualization(reason);
                                      },
                                      VirtualizationCoordinator::Reason::ViewportChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::ViewportChanged);
    }
    RequestRelayout();
}

void GridView::SetSelectedIndexKeyboard(int index, bool extendRange, bool scrollIntoView)
{
    if (!m_Provider)
        return;
    const int count = m_Provider->GetItemCount();
    if (count <= 0)
        return;

    const int clamped = std::clamp(index, 0, count - 1);
    const GridId id = m_Provider->GetItemId(clamped);
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
                const GridId rangeId = m_Provider->GetItemId(i);
                if (rangeId != 0)
                    ids.push_back(rangeId);
            }
            const GridId anchorId = m_Provider->GetItemId(a);
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
    // item on mouse-up, so clear any pending "collapse to single" gesture state.
    m_PendingCollapseToSingle = false;
    m_PendingCollapseId = 0;

    m_SelectedId = id;
    m_SelectedIndex = clamped;
    m_LastClickedId = id;
    for (auto& s : m_CellPool)
    {
        if (!s.cell)
            continue;
        const bool sel =
            (s.boundId != 0) &&
            (m_Selection ? m_Selection->IsSelected(s.boundId) : (s.boundId == m_SelectedId));
        if (sel)
            s.cell->AddClass("selected");
        else
            s.cell->RemoveClass("selected");
    }

    m_DataDirty = true;
    if (UIManager* ui = GetOwnerManager())
    {
        ui->EnqueueVirtualizationWork(this,
                                      [](UIElement* ctx, UIManager& /*ui*/, VirtualizationCoordinator::Reason reason)
                                      {
                                          static_cast<GridView*>(ctx)->UpdateVirtualization(reason);
                                      },
                                      VirtualizationCoordinator::Reason::DataChanged);
    }
    else
    {
        UpdateVirtualization(VirtualizationCoordinator::Reason::DataChanged);
    }
    RequestRelayout();

    if (!scrollIntoView || !m_Scroll)
        return;

    const float viewportW = m_Scroll->GetViewportWidth();
    const float viewportH = m_Scroll->GetViewportHeight();
    if (viewportW <= 0.0f || viewportH <= 0.0f)
        return;

    const float strideX = m_CellW + m_Gap;
    const float strideY = m_CellH + m_Gap;
    const Box4 padding = GetContentPadding();
    int cols = std::max(1, (int)std::floor(
        (viewportW - padding.Left - padding.Right + m_Gap) / strideX));
    const int row = clamped / cols;

    const float rowTop = padding.Top + row * strideY;
    const float rowBottom = rowTop + m_CellH;
    const float scrollY = m_Scroll->GetScrollY();

    if (rowTop < scrollY)
        m_Scroll->SetScrollY(rowTop);
    else if (rowBottom > scrollY + viewportH)
        m_Scroll->SetScrollY(std::max(0.0f, rowBottom - viewportH));
}

void GridView::SetSelectedIndex(int index, bool scrollIntoView)
{
    SetSelectedIndexKeyboard(index, /*extendRange*/ false, scrollIntoView);
}

void GridView::OnEvent(UIEvent& e)
{
    if (e.Id == kEventKeyDown && m_Provider)
    {
        const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);
        if (primaryMod && e.Key == Input::kKeyCode_A)
        {
            const int count = m_Provider->GetItemCount();
            if (count > 0)
            {
                std::vector<UI::Interaction::ItemId> ids;
                ids.reserve((size_t)count);
                for (int i = 0; i < count; ++i)
                {
                    const GridId id = m_Provider->GetItemId(i);
                    if (id != 0)
                        ids.push_back(id);
                }
                if (!ids.empty())
                {
                    const GridId anchor = ids.front();
                    m_Selection->SetSelection(ids, anchor);
                    m_SelectedId = anchor;
                    m_SelectedIndex = 0;
                    m_LastClickedId = anchor;
                    for (auto& s : m_CellPool)
                    {
                        if (!s.cell)
                            continue;
                        const bool sel =
                            (s.boundId != 0) &&
                            (m_Selection ? m_Selection->IsSelected(s.boundId) : (s.boundId == m_SelectedId));
                        if (sel)
                            s.cell->AddClass("selected");
                        else
                            s.cell->RemoveClass("selected");
                    }
                }
            }
            e.Stop();
            return;
        }

        // Arrow keys move within the visual grid: Left/Right by one cell and
        // Up/Down by one full row. GridView is the sole owner of grid keyboard
        // navigation — containing panels must not also handle arrows, or every
        // press moves twice.
        if (!primaryMod &&
            (e.Key == Input::kKeyCode_Left || e.Key == Input::kKeyCode_Right ||
             e.Key == Input::kKeyCode_Up || e.Key == Input::kKeyCode_Down))
        {
            const int count = m_Provider->GetItemCount();
            if (count > 0)
            {
                const int cur = std::clamp(SelectedIndexFromModel(count), 0, count - 1);
                const int cols = std::max(1, m_Columns);
                int delta = 0;
                if (e.Key == Input::kKeyCode_Left)
                    delta = -1;
                else if (e.Key == Input::kKeyCode_Right)
                    delta = 1;
                else if (e.Key == Input::kKeyCode_Up)
                    delta = -cols;
                else
                    delta = cols;

                const bool shift = (e.Mods & Input::kModShift) != 0;
                SetSelectedIndexKeyboard(std::clamp(cur + delta, 0, count - 1),
                                         /*extendRange*/ shift,
                                         /*scrollIntoView*/ true);
            }
            e.Stop();
            return;
        }

        // Standard grid keys (Win32/WPF/Qt/AppKit): PageUp/Down move the
        // selection vertically by one viewport of rows — the caret keeps its
        // column, so the index moves by rows*columns — Home/End go to the
        // extremes, and Shift extends from the anchor via the existing
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
                    const int cols = std::max(1, m_Columns);
                    const float strideY = m_CellH + m_Gap;
                    const float viewH = m_Scroll ? m_Scroll->GetViewportHeight() : 0.0f;
                    const int pageRows = strideY > 0.0f
                        ? std::max(1, (int)(viewH / strideY) - 1)
                        : 1;
                    const int cur = std::clamp(SelectedIndexFromModel(count), 0, count - 1);
                    targetIndex = (e.Key == Input::kKeyCode_PageUp) ? cur - pageRows * cols
                                                                    : cur + pageRows * cols;
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

} // namespace GameEngine
