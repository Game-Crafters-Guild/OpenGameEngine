#pragma once

#include "UI/UIElement.h"
#include "UI/UIManagerRef.h"
#include "UI/Controls/ChangeTrackingProvider.h"
#include "UI/Controls/IVirtualizedControl.h"
#include "UI/Controls/VirtualWindowCore.h"
#include "UI/Interaction/Selection.h"
#include "UI/Interaction/Payload.h"
#include "UI/Interaction/DropTarget.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine
{

class ScrollView;
class Label;

using ListId = uint64_t;
using ListChangeSet = ChangeSet<ListId>;

/// Data provider interface for ListView.
/// Provides item count, ids, and heights for variable-height virtualization.
struct IListDataProvider
{
    virtual ~IListDataProvider() = default;
    virtual int GetItemCount() const = 0;
    virtual ListId GetItemId(int index) const = 0;
    /// Return the height of an item in pixels. If all items have same height, return constant.
    virtual float GetItemHeight(int index) const = 0;
    virtual void ConsumeChanges(uint64_t sinceVersion, ListChangeSet& out) const = 0;
    /// Current change-tracking version, for the UIManager per-frame change pump.
    /// Providers that don't track versions return 0 (they opt out of the pump).
    virtual uint64_t GetChangeVersion() const { return 0; }
};

class ListChangeTrackingProvider : public IListDataProvider
                               , protected ChangeTrackingProviderBase<ListId>
{
  public:
    using ChangeTrackingProviderBase<ListId>::MarkChanged;
    using ChangeTrackingProviderBase<ListId>::MarkChangedBatch;
    using ChangeTrackingProviderBase<ListId>::MarkAllChanged;
    using ChangeTrackingProviderBase<ListId>::MarkStructureChanged;

    void ConsumeChanges(uint64_t sinceVersion, ListChangeSet& out) const override
    {
        ChangeTrackingProviderBase<ListId>::ConsumeChanges(sinceVersion, out);
    }
    uint64_t GetChangeVersion() const override
    {
        return ChangeTrackingProviderBase<ListId>::GetChangeVersion();
    }
};

/// Virtualized list view with support for variable-height items.
/// Uses element pooling and absolute positioning for high performance.
class ListView : public UIElement
             , public UI::Interaction::IDropTarget
             , public UI::Interaction::IDragAutoScrollTarget
             , public IVirtualizedControl
{
  public:
    ListView();
    ~ListView() override;

    // Optional fixed header row (does not scroll vertically, but stays horizontally aligned
    // with the list content when horizontally scrolling).
    void SetHeader(std::unique_ptr<UIElement> header);
    UIElement* GetHeader() const { return m_HeaderContent; }

    void SetDataProvider(IListDataProvider* provider);
    void SetSelectionModel(UI::Interaction::ISelectionModel* model) { m_Selection = model; }

    /// Factory: creates a new UIElement for an item (used for pool expansion).
    using ItemFactory = std::function<std::unique_ptr<UIElement>(ListId, IListDataProvider*)>;
    void SetItemFactory(ItemFactory f) { m_ItemFactory = std::move(f); }

    /// Binder: invoked when a pooled cell is (re)bound to an item.
    using ItemBinder = std::function<void(UIElement* cell, ListId, int index, IListDataProvider*)>;
    void SetItemBinder(ItemBinder b) { m_ItemBinder = std::move(b); }

    // Callbacks
    void SetOnSelectionChanged(std::function<void(ListId)> cb) { m_OnSelectionChanged = std::move(cb); }
    void SetOnItemActivated(std::function<void(ListId)> cb) { m_OnItemActivated = std::move(cb); }
    /// Fired by a slow second click (past the double-click interval, no
    /// modifiers, no drag) on the row that was already the sole selection —
    /// the file-manager gesture for "edit this name". Double-click still
    /// activates; the host decides what a rename request means.
    void SetOnItemRenameRequested(std::function<void(ListId)> cb) { m_OnItemRenameRequested = std::move(cb); }
    void SetOnContextMenu(std::function<void(ListId, float x, float y)> cb) { m_OnContextMenu = std::move(cb); }

    /// Picker mode: the list is a chooser popup whose only purpose is to pick one
    /// row, so a SINGLE click activates. It also keeps ownership of the rest of
    /// that press, because the activation callback may remove the list before
    /// mouse-up and the release must not retarget to whatever the popup uncovers.
    /// Browsing lists leave this off and keep double-click activation, where a
    /// single click only selects.
    void SetPickerActivation(bool enabled) { m_PickerActivation = enabled; }

    // Drag source hook (optional): called when a drag gesture begins.
    void SetDragPayloadBuilder(std::function<UI::Interaction::DragPayload()> cb) { m_DragPayloadBuilder = std::move(cb); }

    // Drag/drop target hooks (optional; default is no-op).
    void SetAcceptsPayload(std::function<bool(UI::Interaction::PayloadTypeId)> cb) { m_DropAccepts = std::move(cb); }
    void SetOnCanDrop(std::function<UI::Interaction::DropFeedback(const UI::Interaction::DropRequest&)> cb) { m_OnCanDrop = std::move(cb); }
    void SetOnPerformDrop(std::function<void(const UI::Interaction::DropRequest&)> cb) { m_OnPerformDrop = std::move(cb); }

    /// Force refresh of visible items from the provider (rebuilds entire layout cache).
    void RefreshFromProvider();
    /// The view was hidden and shown again without a provider change. Recompute
    /// visible rows without clearing selection.
    void InvalidateVirtualization();

    /// Notify that items were appended to the end of the list.
    /// This is an O(k) operation where k is the number of appended items,
    /// much faster than RefreshFromProvider() which is O(n) for n total items.
    /// @param count Number of items appended.
    void NotifyItemsAppended(int count);

    /// Get currently selected item id (0 if none).
    ListId GetSelectedId() const { return m_SelectedId; }

    /// Programmatically select an item.
    void SetSelectedId(ListId id);

    /// Get the current scroll offset (vertical).
    float GetScrollOffset() const;
    float GetScrollX() const;

    int GetItemCount() const;
    int GetSelectedIndex() const;
    void SetSelectedIndex(int index, bool scrollIntoView = true);
    // Keyboard-aware selection helper used by editor panels to implement arrow-key
    // navigation. When extendRange is true, selection is expanded/collapsed as a
    // contiguous range using the current Shift anchor (matching Shift+click UX).
    void SetSelectedIndexKeyboard(int index, bool extendRange, bool scrollIntoView = true);
    // Fires the item-activated callback for the currently selected item (if any).
    // Used by editor panels to implement Enter-key activation that mirrors double-click.
    void ActivateSelected()
    {
        if (!m_OnItemActivated)
            return;
        // A full provider refresh (e.g. a column resize) clears m_SelectedId but leaves the
        // selection model intact. Recover the id via GetSelectedIndex(), which validates the
        // selection anchor against the provider — so Enter-to-activate survives a refresh and
        // never dispatches a stale id.
        ListId id = m_SelectedId;
        if (id == 0 && m_Provider)
        {
            const int index = GetSelectedIndex();
            if (index >= 0)
                id = m_Provider->GetItemId(index);
        }
        if (id != 0)
            m_OnItemActivated(id);
    }

    /// Set the scroll offset (vertical).
    void SetScrollOffset(float offset);

    /// Forward ScrollView scroll changes (x, y).
    void SetOnScrollChanged(std::function<void(float, float)> cb) { m_OnScrollChanged = std::move(cb); }

    /// Callback for the item resize gesture (UI/Interaction/ItemResizeGesture.h). The delta is the scroll amount (negative = wheel up, positive = wheel down).
    /// The caller should adjust their data provider's item heights and call RefreshFromProvider().
    void SetOnItemResizeGesture(std::function<void(float delta)> cb) { m_OnItemResizeGesture = std::move(cb); }

    /// Scroll to the end of the list. This is deferred until after layout if needed.
    void ScrollToEnd();

    /// Get the total content height (sum of all item heights).
    float GetTotalContentHeight() const { return (float)m_TotalContentHeightPx; }

    /// Get the viewport height (visible area).
    float GetViewportHeight() const;

    /// Get the viewport width (visible area, excludes the vertical scrollbar gutter).
    float GetViewportWidth() const;

    /// Set explicit content width (pixels) for horizontal scrolling.
    void SetContentWidth(float widthPx);

    // Virtualization inspection (useful for automation + diagnostics). First,
    // visible count, and item count come straight from the core's last guard.
    int GetFirstVisibleIndex() const { return m_WindowCore.LastGuard().First; }
    int GetVisibleCount() const { return m_WindowCore.LastGuard().Desired; }
    int GetLastKnownItemCount() const { return m_WindowCore.LastGuard().ItemCount; }
    int GetCellPoolSize() const { return (int)m_CellPool.size(); }

    // Pooled-row resolution for automation. Pooled rows carry no stable
    // per-logical-row element ids (slots are recycled across binds), so callers
    // that need "row N of this list" ask the pooling authority directly.
    /// Scroll so the item at `index` is fully inside the viewport. Unlike
    /// SetSelectedIndex(index, true) this does not touch selection. The row
    /// binds on the next virtualization pass, not inline. Before the first
    /// layout the viewport has no height, so the scroll is deferred to the
    /// first layout that has one.
    void ScrollIndexIntoView(int index);
    /// The pooled row wrapper currently bound to logical `index`, or null when
    /// that index is outside the bound window.
    UIElement* GetCellForIndex(int index) const;
    /// Logical item index of the bound pooled row containing the window-space
    /// point, or -1 when the point is over no bound row.
    int GetIndexAtPoint(float x, float y) const;

    /// Call fn for each currently visible row's user content (for applying column widths etc. without full refresh).
    void ForEachVisibleCell(std::function<void(UIElement* userContent)> fn) const;

    void OnEvent(UIEvent& e) override;

    // IVirtualizedControl (C-8 per-frame provider change pump).
    uint64_t ProviderChangeVersion() const override { return m_Provider ? m_Provider->GetChangeVersion() : 0; }
    void EnqueuePumpWork(UIManager& ui) override;

    // Maintain the manager's virtualized-control pump registry.
    void OnOwnerManagerChanged(UIManager* owner) override;

  private:
    void OnPostLayout() override;
    void UpdateHeaderScrollX(float scrollX);
    void UpdateVisibleRowWidths();
    void EnsureCellPoolSize(int desired);
    // Unbind every pooled slot from `firstSlot` onward (window shrank or the
    // provider went away). Cells stay in the tree, paint/input-invisible.
    void UnbindSlotsFrom(int firstSlot);
    // Returns true when the bind marked the cell subtree dirty (binding changed or
    // a data refresh is in flight) so the Host can report a Rebind impact.
    bool BindCell(int slotIndex, int itemIndex, int yOffsetPx);
    void UnbindCell(int slot);
    void DestroyTailSlot();
    void UpdateVirtualization(VirtualizationCoordinator::Reason reason);
    void SchedulePrewarmVirtualization();
    void ClearHover();
    void RebuildLayoutCache();
    // Push the cached total content height to the scroll view so its scroll range covers every
    // item. SetScrollY clamps against the published height, so a scroll that follows a data
    // change has to publish first or it clamps against the previous item count. Returns the
    // forced content height in pixels (never below the viewport).
    int PublishContentSize(float viewportW, float viewportH);
    void UpdateDropPreviewClasses();
    /// Update .selected class on all pooled cells from current selection (no virtualization pass).
    void ApplySelectionStylesToPool();

    // Bridges the flat cell pool to the shared window engine (defined in the .cpp).
    struct HostAdapter;

    // IDragAutoScrollTarget
    void AutoScrollDuringDrag(float mouseX, float mouseY) override;

    // IDropTarget
    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

    float m_ExplicitContentWidthPx = -1.0f;

    IListDataProvider* m_Provider = nullptr;
    uint64_t m_LastProviderChangeVersion = 0;
    // Last provider StructureVersion the consume path acted on. Movement means a
    // structural mutation (add/remove/reorder) that must rebuild the window and
    // the height cache — see ChangeTrackingProviderBase's contract note (C-7).
    uint64_t m_LastProviderStructureVersion = 0;
    ListChangeSet m_PendingChangeSet;
    bool m_PendingChangeSetValid = false;
    // Manager whose virtualized-control pump registry currently holds this view;
    // re-homing/destruction must deregister from the OLD manager (mirrors
    // ScrollView::m_RegisteredScrollManager).
    UIManagerRef m_RegisteredVirtualizationManager;
    UI::Interaction::ISelectionModel* m_Selection = nullptr;

    ItemFactory m_ItemFactory;
    ItemBinder m_ItemBinder;

    std::function<void(ListId)> m_OnSelectionChanged;
    std::function<void(ListId)> m_OnItemActivated;
    std::function<void(ListId)> m_OnItemRenameRequested;
    std::function<void(ListId, float, float)> m_OnContextMenu;
    std::function<void(float, float)> m_OnScrollChanged;
    std::function<void(float)> m_OnItemResizeGesture;
    bool m_PickerActivation = false;
    int m_SelectedIndex = -1;
    int m_ShiftAnchorIndex = -1; // cached index for Shift-range selection

    std::chrono::steady_clock::time_point m_LastClickTime{};
    ListId m_LastClickedId = 0;

    ScrollView* m_Scroll = nullptr;
    UIElement* m_Content = nullptr;

    UIElement* m_HeaderHost = nullptr;    // fixed row (clipping container)
    UIElement* m_HeaderWrapper = nullptr; // absolute-positioned wrapper (tracks scrollX)
    UIElement* m_HeaderContent = nullptr;             // user header root (child of wrapper)
    int m_HeaderHeightPx = 0;

    struct CellSlot
    {
        UIElement* cell = nullptr;        // The absolute-positioned row wrapper
        UIElement* userContent = nullptr; // The user-provided content element inside the wrapper
        ListId boundId = 0;
        int boundIndex = -1;
    };
    std::vector<CellSlot> m_CellPool;

    UIElement* m_LastHoverCell = nullptr;
    ListId m_SelectedId = 0;

    // Layout cache for variable-height items: cumulative Y positions.
    // NOTE: We store these in integer pixel space to keep ScrollView scroll extents
    // and per-cell absolute rects perfectly consistent. Using floats here and rounding
    // per-cell rects can accumulate drift and allow scrolling into empty space.
    // m_CumulativeYPx[i] = sum of rounded heights (px) of items 0..i-1 (so [0] = 0).
    std::vector<int> m_CumulativeYPx;
    int m_TotalContentHeightPx = 0;
    int m_MinItemHeightPx = 0;
    bool m_LayoutCacheDirty = true;

    int m_OverscanItems = 4;
    bool m_DataDirty = true;
    bool m_PendingScrollToEnd = false;
    int m_PendingScrollIntoViewIndex = -1;
    bool m_VirtualizationRetryScheduled = false;

    // Pre-warm virtualization so the first user scroll doesn't resize the pool.
    // This runs for a few ticks after mount/provider changes until we have a valid
    // viewport and the pool is at least the last known desired size.
    bool m_PrewarmScheduled = false;
    int m_PrewarmTicksRemaining = 0;

    // Cached viewport size (rounded pixels) to coalesce OnPostLayout re-enqueues.
    int m_LastViewportWPx = -1;
    int m_LastViewportHPx = -1;

    // Shared index-space window engine: owns the window math, ring-slot mapping,
    // pool ensure/shrink orchestration, changeset application, safety-net repair,
    // and aggregated impact reporting. ListView feeds it one lane (lines == rows)
    // and resolves first-visible from the variable-height prefix sum below.
    UI::VirtualWindowCore m_WindowCore;
    // Bumped whenever item identity/order changes without moving the window
    // (provider swap, refresh, append, data refresh). Carried in the guard's
    // StructureGeneration so the core breaks its ring shortcut and fully rebinds.
    std::uint64_t m_StructureGeneration = 0;
    // Scratch reused each update to map a Subset changeset's ids to the flat pool
    // slots currently bound to them (keeps the core id-type-free).
    std::vector<int> m_ChangedSlotScratch;

    // Drag source state
    std::function<UI::Interaction::DragPayload()> m_DragPayloadBuilder;
    bool m_DragCandidate = false;
    float m_DragStartX = 0.0f;
    float m_DragStartY = 0.0f;
    bool m_DragStartedThisGesture = false;
    bool m_PendingCollapseToSingle = false;
    ListId m_PendingCollapseId = 0;
    // Armed on press when the row was already the sole selection and the
    // previous click on it is older than the double-click interval; honoured on
    // release unless the gesture became a drag.
    bool m_PendingRenameRequest = false;

    // Drop target state
    std::function<bool(UI::Interaction::PayloadTypeId)> m_DropAccepts;
    std::function<UI::Interaction::DropFeedback(const UI::Interaction::DropRequest&)> m_OnCanDrop;
    std::function<void(const UI::Interaction::DropRequest&)> m_OnPerformDrop;
    UI::Interaction::DropPreviewState m_DropPreview{};
};

} // namespace GameEngine
