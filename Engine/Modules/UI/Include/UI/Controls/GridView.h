#pragma once

#include "UI/UIElement.h"
#include "UI/UIManagerRef.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/ChangeTrackingProvider.h"
#include "UI/Controls/IVirtualizedControl.h"
#include "UI/Controls/VirtualWindowCore.h"
#include "UI/Interaction/Selection.h"
#include "UI/Interaction/Payload.h"
#include "UI/Interaction/DropTarget.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>
#include <string>

namespace GameEngine
{

class ScrollView; // forward
class Label;      // forward

using GridId = uint64_t;
using GridChangeSet = ChangeSet<GridId>;

struct SortDescriptor
{
    enum class Field : uint8_t
    {
        Name,
        Type,
        Size,
        Dimensions,
        Modified,
        Git,
        Tag,
        Referenced,
        Custom,
        Creator
    } field = Field::Name;
    bool ascending = true;
};

struct GroupDescriptor
{
    enum class Key : uint8_t
    {
        None,
        Type
    } key = Key::None;
};

struct IGridDataProvider
{
    virtual ~IGridDataProvider() = default;
    virtual int GetItemCount() const = 0;
    virtual GridId GetItemId(int index) const = 0;
    virtual const char* GetLabel(GridId id) const = 0;
    virtual uint64_t GetIcon(GridId id) const = 0;       // handle/atlas id; 0 if none
    virtual const char* GetTypeKey(GridId id) const = 0; // grouping key when GroupDescriptor::Type
    virtual void ApplySort(const SortDescriptor& desc) = 0;
    virtual void ApplyGrouping(const GroupDescriptor& desc) = 0;
    virtual void ConsumeChanges(uint64_t sinceVersion, GridChangeSet& out) const = 0;
    /// Current change-tracking version, for the UIManager per-frame change pump.
    /// Providers that don't track versions return 0 (they opt out of the pump).
    virtual uint64_t GetChangeVersion() const { return 0; }
};

class GridChangeTrackingProvider : public IGridDataProvider
                               , protected ChangeTrackingProviderBase<GridId>
{
  public:
    using ChangeTrackingProviderBase<GridId>::MarkChanged;
    using ChangeTrackingProviderBase<GridId>::MarkChangedBatch;
    using ChangeTrackingProviderBase<GridId>::MarkAllChanged;
    using ChangeTrackingProviderBase<GridId>::MarkStructureChanged;

    void ConsumeChanges(uint64_t sinceVersion, GridChangeSet& out) const override
    {
        ChangeTrackingProviderBase<GridId>::ConsumeChanges(sinceVersion, out);
    }
    uint64_t GetChangeVersion() const override
    {
        return ChangeTrackingProviderBase<GridId>::GetChangeVersion();
    }
};

class GridView : public UIElement
             , public UI::Interaction::IDropTarget
             , public UI::Interaction::IDragAutoScrollTarget
             , public IVirtualizedControl
{
  public:
    GridView();
    ~GridView() override;

    // Data and grouping/sort
    void SetDataProvider(IGridDataProvider* provider)
    {
        m_Provider = provider;
        m_LastProviderChangeVersion = 0;
        m_LastProviderStructureVersion = 0;
        m_PendingChangeSetValid = false;
        // Changing folders/providers should clear selection to avoid leaking it
        // across unrelated datasets (ids may be reused).
        m_SelectedId = 0;
        m_SelectedIndex = -1;
        if (m_Selection)
            m_Selection->Clear();
        m_LastClickedId = 0;
        m_DataDirty = true;
        m_NeedsGeometryRebuild = true;
        // Reset first-layout flag to ensure proper rebuild with new provider
        m_FirstLayoutDone = false;
        // Drop the window/ring/pool bookkeeping so the new provider rebinds from
        // scratch; m_DataDirty forces a full rebind on the next update.
        m_WindowCore.Reset();
        m_LastColumns = -1;
        // Provider changes affect virtualization/bindings; avoid RequestRelayout() storms.
        MarkDirty(LayoutDirty | VisualDirty);
    }
    void SetSort(const SortDescriptor& desc)
    {
        m_Sort = desc;
        if (m_Provider)
            m_Provider->ApplySort(desc);
        m_DataDirty = true;
        MarkDirty(LayoutDirty | VisualDirty);
    }
    void SetGrouping(const GroupDescriptor& desc)
    {
        m_Group = desc;
        if (m_Provider)
            m_Provider->ApplyGrouping(desc);
        m_DataDirty = true;
        MarkDirty(LayoutDirty | VisualDirty);
    }

    // Item factory: optional hook to create a cell template (used for virtualization pool).
    using ItemFactory = std::function<std::unique_ptr<UIElement>(GridId, IGridDataProvider*)>;
    void SetItemFactory(ItemFactory f)
    {
        m_ItemFactory = std::move(f);
    }

    // Item binder: invoked when a pooled cell is (re)bound to a GridId.
    using ItemBinder = std::function<void(UIElement* cell, GridId, IGridDataProvider*)>;
    void SetItemBinder(ItemBinder b) { m_ItemBinder = std::move(b); }

    // Selection model (shared across views).
    void SetSelectionModel(UI::Interaction::ISelectionModel* model) { m_Selection = model; }

    // Optional drag support (source). If set, GridView will begin a drag on mouse move threshold.
    void SetDragPayloadBuilder(std::function<UI::Interaction::DragPayload()> cb) { m_DragPayloadBuilder = std::move(cb); }
    void SetOnItemActivated(std::function<void(GridId)> cb)
    {
        m_OnItemActivated = std::move(cb);
    }
    /// Fired by a slow second click (past the double-click interval, no
    /// modifiers, no drag) on the item that was already the sole selection —
    /// the file-manager gesture for "edit this name". Double-click still
    /// activates; the host decides what a rename request means.
    void SetOnItemRenameRequested(std::function<void(GridId)> cb)
    {
        m_OnItemRenameRequested = std::move(cb);
    }

    /// Picker mode: the grid is a chooser popup whose only purpose is to pick one
    /// cell, so a SINGLE click activates. Browsing grids leave this off and keep
    /// double-click activation, where a single click only selects.
    void SetPickerActivation(bool enabled) { m_PickerActivation = enabled; }
    void SetOnContextMenu(std::function<void(GridId, float x, float y)> cb)
    {
        m_OnContextMenu = std::move(cb);
    }
    void SetOnSortChanged(std::function<void(const SortDescriptor&)> cb)
    {
        m_OnSortChanged = std::move(cb);
    }

    // Drag/drop target hooks (optional; default is no-op).
    void SetAcceptsPayload(std::function<bool(UI::Interaction::PayloadTypeId)> cb) { m_DropAccepts = std::move(cb); }
    void SetOnCanDrop(std::function<UI::Interaction::DropFeedback(const UI::Interaction::DropRequest&)> cb) { m_OnCanDrop = std::move(cb); }
    void SetOnPerformDrop(std::function<void(const UI::Interaction::DropRequest&)> cb) { m_OnPerformDrop = std::move(cb); }

    int GetItemCount() const { return m_Provider ? m_Provider->GetItemCount() : 0; }
    int GetSelectedIndex() const { return m_SelectedIndex; }
    int GetColumnCount() const { return m_Columns; }
    // Diagnostics: the last window the core resolved. First/ItemCount/ContentHPx
    // come straight from the core's guard; desired cell count folds in the lane
    // (column) count, which the guard does not carry.
    int GetFirstVisibleRow() const { return m_WindowCore.LastGuard().First; }
    int GetDesiredCellCount() const
    {
        const int desiredRows = m_WindowCore.LastGuard().Desired;
        return desiredRows < 0 ? -1 : desiredRows * (m_LastColumns > 0 ? m_LastColumns : 0);
    }
    int GetLastKnownItemCount() const { return m_WindowCore.LastGuard().ItemCount; }
    int GetLastKnownColumns() const { return m_LastColumns; }
    int GetLastContentHeightPx() const { return m_WindowCore.LastGuard().ContentHPx; }
    float GetScrollOffsetY() const { return m_Scroll ? m_Scroll->GetScrollY() : 0.0f; }
    float GetViewportWidth() const { return m_Scroll ? m_Scroll->GetViewportWidth() : 0.0f; }
    float GetViewportHeight() const { return m_Scroll ? m_Scroll->GetViewportHeight() : 0.0f; }
    float GetContentWidth() const { return m_Scroll ? m_Scroll->GetContentWidth() : 0.0f; }
    float GetContentHeight() const { return m_Scroll ? m_Scroll->GetContentHeight() : 0.0f; }
    void SetSelectedIndex(int index, bool scrollIntoView = true);
    // Keyboard-aware selection helper used by editor panels to implement arrow-key
    // navigation. When extendRange is true, selection is expanded/collapsed as a
    // contiguous range using the current Shift anchor (matching Shift+click UX).
    void SetSelectedIndexKeyboard(int index, bool extendRange, bool scrollIntoView = true);
    // Fires the item-activated callback for the currently selected item (if any).
    // Used by editor panels to implement Enter-key activation that mirrors double-click.
    void ActivateSelected()
    {
        if (m_OnItemActivated && m_SelectedId != 0)
            m_OnItemActivated(m_SelectedId);
    }

    // Refresh visible cells from the provider (virtualized, pooled).
    void RefreshFromProvider();

    /// The title label of the pooled cell currently bound to item `index`, or
    /// null when that item is outside the virtualized window. Hosts overlay
    /// per-item UI (inline rename) on it; its parent is the cell.
    Label* GetTitleLabelForIndex(int index) const;
    // The view was hidden and shown again without a provider change. Recompute the
    // virtualization window without clearing selection or treating it as navigation.
    void InvalidateVirtualization();

    void SetIconSize(float px);
    float GetIconSize() const { return m_IconSize; }
    /// Icon sizes land on the smallest icon size plus whole steps of \p stepPx, however they are
    /// set: SetIconSize, a host's slider, the item resize gesture. 0 keeps them continuous.
    void SetIconSizeStep(float stepPx);
    void SetOnIconSizeChanged(std::function<void(float)> cb) { m_OnIconSizeChanged = std::move(cb); }
    /// One item resize step (UI/Interaction/ItemResizeGesture.h): wheel up (negative scrollY) grows the icons.
    void AdjustIconSizeFromScroll(float scrollY);

    /// Called when scroll position changes (scrollY, contentHeight, viewportHeight). Use for load-more-on-scroll.
    void SetOnScrollChanged(std::function<void(float scrollY, float contentHeight, float viewportHeight)> cb) { m_OnUserScrollChanged = std::move(cb); }

    // IVirtualizedControl (C-8 per-frame provider change pump).
    uint64_t ProviderChangeVersion() const override { return m_Provider ? m_Provider->GetChangeVersion() : 0; }
    void EnqueuePumpWork(UIManager& ui) override;

    // Maintain the manager's virtualized-control pump registry.
    void OnOwnerManagerChanged(UIManager* owner) override;

  private:
    // Clamped to the icon size range, then onto the step grid when SetIconSizeStep set one.
    float SnappedIconSize(float px) const;
    // One item resize step up from a size on the step grid: the size-scaled wheel step, snapped,
    // and at least one whole grid step.
    float GrownIconSizeOnGrid(float px) const;
    void OnEvent(UIEvent& e) override;
    void OnPostLayout() override;
    void EnsureCellPoolSize(int desired);
    // Unbind every pooled slot from `firstSlot` onward (window shrank or the
    // provider went away). Also repairs any pooled cell that lost its absolute
    // rect so it never enters flex flow. Cells stay in the tree,
    // paint/input-invisible.
    void UnbindSlotsFrom(int firstSlot);
    // Returns true when the bind marked the cell subtree dirty (binding changed
    // or a data refresh is in flight) so the Host can report a Rebind impact.
    bool BindCell(int slotIndex, int itemIndex, int row, int col);
    void UnbindCell(int slot);
    void DestroyTailSlot();
    void UpdateVirtualization(VirtualizationCoordinator::Reason reason);
    void ClearHover();
    void UpdateCellMetrics();
    Box4 GetContentPadding() const;
    int SelectedIndexFromModel(int count) const;
    void UpdateThumbSizes();
    float TitleWidthPx() const;
    float TitleHeightPx() const;
    void UpdateSelectionOutlineGeometry();
    void UpdateDropPreviewClasses();

    // IDragAutoScrollTarget
    void AutoScrollDuringDrag(float mouseX, float mouseY) override;

    // Bridges the cell pool to the shared window engine (defined in the .cpp).
    struct HostAdapter;

    // IDropTarget
    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

    IGridDataProvider* m_Provider = nullptr; // not owned
    uint64_t m_LastProviderChangeVersion = 0;
    // Last provider StructureVersion the consume path acted on. Movement means a
    // structural mutation (add/remove/reorder/regroup) that must fully rebind the
    // window — see ChangeTrackingProviderBase's contract note (C-7).
    uint64_t m_LastProviderStructureVersion = 0;
    GridChangeSet m_PendingChangeSet;
    bool m_PendingChangeSetValid = false;
    // Manager whose virtualized-control pump registry currently holds this view
    // (mirrors ScrollView::m_RegisteredScrollManager).
    UIManagerRef m_RegisteredVirtualizationManager;
    SortDescriptor m_Sort{};
    GroupDescriptor m_Group{};

    ItemFactory m_ItemFactory; // optional factory for per-item visuals
    ItemBinder m_ItemBinder;   // optional binder for pooled cells

    std::function<void(GridId)> m_OnItemActivated;
    std::function<void(GridId)> m_OnItemRenameRequested;
    bool m_PickerActivation = false;
    std::function<void(GridId, float, float)> m_OnContextMenu;
    std::function<void(const SortDescriptor&)> m_OnSortChanged;
    std::function<void(float)> m_OnIconSizeChanged;
    std::function<void(float, float, float)> m_OnUserScrollChanged;

    UI::Interaction::ISelectionModel* m_Selection = nullptr; // not owned
    std::function<UI::Interaction::DragPayload()> m_DragPayloadBuilder;
    std::function<bool(UI::Interaction::PayloadTypeId)> m_DropAccepts;
    std::function<UI::Interaction::DropFeedback(const UI::Interaction::DropRequest&)> m_OnCanDrop;
    std::function<void(const UI::Interaction::DropRequest&)> m_OnPerformDrop;
    UI::Interaction::DropPreviewState m_DropPreview{};

    // Drag gesture state (per-grid)
    bool m_DragCandidate = false;
    float m_DragStartX = 0.0f;
    float m_DragStartY = 0.0f;
    bool m_DragStartedThisGesture = false;
    bool m_PendingCollapseToSingle = false;
    GridId m_PendingCollapseId = 0;
    // Armed on press when the item was already the sole selection and the
    // previous click on it is older than the double-click interval; honoured on
    // release unless the gesture became a drag.
    bool m_PendingRenameRequest = false;

    // Simple double-click detection state
    std::chrono::steady_clock::time_point m_LastClickTime{};
    GridId m_LastClickedId = 0;

    // Virtualized scroll + content container
    ScrollView* m_Scroll = nullptr; // not owned (child)
    UIElement* m_Content = nullptr; // not owned (child of m_Scroll viewport)

    struct CellSlot
    {
        UIElement* cell = nullptr;
        Label* title = nullptr;                 // optional (when using default template)
        UIElement* thumb = nullptr;             // optional (when using default template)
        UIElement* selectionOutline = nullptr;  // optional (when using default template)
        GridId boundId = 0;
        int boundIndex = -1;
    };
    std::vector<CellSlot> m_CellPool;

    // Hover/selection state
    UIElement* m_LastHoverCell = nullptr;
    GridId m_SelectedId = 0;
    int m_SelectedIndex = -1;
    int m_ShiftAnchorIndex = -1; // cached index for Shift-range selection

    // Layout parameters (match theme.css defaults). The content inset comes
    // from the stylesheet via GetContentPadding(), not from a member.
    float m_IconSize = 80.0f;
    float m_IconSizeStepPx = 0.0f; // 0 = continuous; see SetIconSizeStep
    float m_CellW = 96.0f;
    float m_CellH = 120.0f;
    float m_Gap = 8.0f;
    int m_OverscanRows = 2;
    int m_Columns = 1;
    bool m_DataDirty = true;
    bool m_NeedsGeometryRebuild = false; // Set when cells need full geometry rebuild after layout
    bool m_FirstLayoutDone = false;      // Track if we've done first valid layout

    // Shared index-space window engine: owns the window math, ring-slot mapping,
    // pool ensure/shrink orchestration, changeset application, safety-net repair,
    // and aggregated impact reporting. The grid feeds it rows (lines) with the
    // column count as the lane multiplier (slot = poolRow * cols + col).
    UI::VirtualWindowCore m_WindowCore;
    // Bumped whenever item identity/order changes without moving the window
    // (provider swap, sort, grouping, data refresh). Carried in the guard's
    // StructureGeneration so the core breaks its ring shortcut and fully rebinds.
    std::uint64_t m_StructureGeneration = 0;
    // Scratch reused each update to map a Subset changeset's ids to the flat
    // pool slots currently bound to them (keeps the core id-type-free).
    std::vector<int> m_ChangedSlotScratch;

    // Column count from the last resolved window. Kept because the guard carries
    // no lane count, and the colsChanged->RequestRelayout machinery needs it.
    int m_LastColumns = -1;

    // Cached viewport size (rounded pixels) to avoid redundant viewport-changed enqueues.
    int m_LastViewportWPx = -1;
    int m_LastViewportHPx = -1;
};

} // namespace GameEngine
