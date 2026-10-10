#pragma once

#include "UI/ModuleOwnedCallback.h"
#include "UI/UIElement.h"
#include "UI/UIManagerRef.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/ChangeTrackingProvider.h"
#include "UI/Controls/IVirtualizedControl.h"
#include "UI/Controls/VirtualWindowCore.h"
#include "UI/Interaction/Payload.h"
#include "UI/Interaction/Selection.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/VirtualizationCoordinator.h"
#include "Types/Types.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GameEngine
{

class ScrollView; // forward
class Label;      // forward

using TreeId = uint64_t; // Provider-defined stable IDs per item
using TreeChangeSet = ChangeSet<TreeId>;

struct ITreeDataProvider
{
    virtual ~ITreeDataProvider() = default;
    virtual int GetRootCount() const = 0;
    virtual TreeId GetRootId(int index) const = 0;
    virtual int GetChildCount(TreeId parent) const = 0;
    virtual TreeId GetChildId(TreeId parent, int index) const = 0;
    virtual const char* GetLabel(TreeId id) const = 0;
    virtual bool IsExpandable(TreeId id) const = 0;
    virtual void ConsumeChanges(uint64_t sinceVersion, TreeChangeSet& out) const = 0;
    /// Current change-tracking version, for the UIManager per-frame change pump.
    /// Providers that don't track versions return 0 (they opt out of the pump).
    virtual uint64_t GetChangeVersion() const { return 0; }
};

class TreeChangeTrackingProvider : public ITreeDataProvider
                               , protected ChangeTrackingProviderBase<TreeId>
{
  public:
    using ChangeTrackingProviderBase<TreeId>::MarkChanged;
    using ChangeTrackingProviderBase<TreeId>::MarkChangedBatch;
    using ChangeTrackingProviderBase<TreeId>::MarkAllChanged;
    using ChangeTrackingProviderBase<TreeId>::MarkStructureChanged;

    void ConsumeChanges(uint64_t sinceVersion, TreeChangeSet& out) const override
    {
        ChangeTrackingProviderBase<TreeId>::ConsumeChanges(sinceVersion, out);
    }
    uint64_t GetChangeVersion() const override
    {
        return ChangeTrackingProviderBase<TreeId>::GetChangeVersion();
    }
};

/** Chevron/foldout size vs row icon: linear from 14px@14px icon to 28px@64px icon (2× baseline at max scale). */
float TreeFoldoutGlyphSizePxForTreeIconSize(float treeIconSizePx);

class TreeView : public UIElement
              , public UI::Interaction::IDropTarget
              , public IVirtualizedControl
{
  public:
    TreeView();
    ~TreeView() override;

    void SetDataProvider(ITreeDataProvider* provider)
    {
        m_Provider = provider;
        m_LastProviderChangeVersion = 0;
        m_LastProviderStructureVersion = 0;
        m_PendingChangeSetValid = false;
        m_FlatDirty = true;
        // Drop the core's window/ring/pool bookkeeping so the new provider rebinds
        // from scratch; m_FlatDirty forces a flat rebuild (generation bump) on the
        // next update. Keep the OnPostLayout viewport-coalescing cache reset so the
        // next layout re-enqueues virtualization work.
        m_WindowCore.Reset();
        m_LastViewportWPx = -1;
        m_LastViewportHPx = -1;
        m_TypeAheadRepeatKey = -1;
        m_TypeAheadLastKeyTime = {};
        MarkDirty(ChildrenDirty | VisualDirty | LayoutDirty);
    }
    void SetSelectionModel(UI::Interaction::ISelectionModel* model) { m_Selection = model; }

    // Rows lay out to the viewport width, so the tree reports no horizontal
    // overflow and shows no bar. Opting in lets the ScrollView measure the bound
    // rows' labels and give the bar a real range, and marks the tree so the
    // stylesheet stops suppressing the bar for it.
    void SetHorizontalScrollEnabled(bool v);
    bool IsHorizontalScrollEnabled() const { return m_HorizontalScrollEnabled; }

    void SetOnHorizontalBarVisibilityChanged(std::function<void(bool)> cb)
    {
        if (m_Scroll)
            m_Scroll->SetOnHorizontalBarVisibilityChanged(std::move(cb));
    }

    // Callbacks (UI-agnostic contracts)
    void SetOnSelectionChanged(std::function<void(TreeId)> cb) { m_OnSelectionChanged = std::move(cb); }
    void SetOnItemActivated(std::function<void(TreeId)> cb) { m_OnItemActivated = std::move(cb); }
    void SetOnContextMenu(std::function<void(TreeId, float x, float y)> cb) { m_OnContextMenu = std::move(cb); }
    // Optional: row customization and icon click callbacks (per-item wiring)
    void SetOnRowBound(std::function<void(TreeId, UIElement*)> cb) { m_OnRowBound = std::move(cb); }
    /// Fills `out` with the id of each row the view holds bound now, one entry per row. A pooled row
    /// the view has unbound or destroyed is not in it, so a caller that keeps state per bound row
    /// (SetOnRowBound) can drop the rest.
    void CollectBoundIds(std::vector<TreeId>& out) const;
    /// The row the view holds bound to `id` now, or null when it holds none: a caller that needs a
    /// row's elements asks here instead of keeping row pointers, which the view frees when it trims
    /// its pool.
    UIElement* FindBoundRow(TreeId id) const;
    /// The id `row` is bound to now, or 0 when the view does not hold `row` bound.
    TreeId BoundIdOf(const UIElement* row) const;
    void SetOnIconClicked(std::function<void(TreeId, UIElement*)> cb) { m_OnIconClicked = std::move(cb); }
    // Optional: callbacks for drag-to-enable/disable functionality
    void SetOnIconStateCheck(std::function<bool(TreeId)> cb) { m_OnIconStateCheck = std::move(cb); }
    void SetOnIconStateSet(std::function<void(TreeId, bool, UIElement*)> cb) { m_OnIconStateSet = std::move(cb); }
    // Optional: lock icon (right-side hover icon) with drag-to-lock support
    void SetOnLockClicked(std::function<void(TreeId, UIElement*)> cb);
    void SetOnLockStateCheck(std::function<bool(TreeId)> cb) { m_OnLockStateCheck = std::move(cb); }
    void SetOnLockStateSet(std::function<void(TreeId, bool, UIElement*)> cb) { m_OnLockStateSet = std::move(cb); }

    // Minimal API surface for initial skeleton; rendering/virtualization to be added

    // Options
    void SetShowRoot(bool v) { m_ShowRoot = v; }
    bool IsExpanded(TreeId id) const { return m_Expanded.find(id) != m_Expanded.end(); }
    void SetExpanded(TreeId id, bool expanded);
    void SetOnExpansionChanged(std::function<void(TreeId, bool isExpanded)> cb) { m_OnExpansionChanged = std::move(cb); }
    void SetOnScrollChanged(std::function<void(float scrollY)> cb) { m_OnScrollChanged = std::move(cb); }
    void ClearExpansionState();
    void ForEachExpanded(const std::function<void(TreeId)>& fn) const
    {
        for (TreeId id : m_Expanded)
            fn(id);
    }

    // Convenience: expand all expandable nodes from the current provider and rebuild the UI.
    void ExpandAll();

    // Refresh visible rows from the provider (virtualized, pooled).
    void RefreshFromProvider();

    // After external changes to the bound SelectionModel, update row "selected" classes and
    // notify selection listeners (mirrors an in-view selection gesture).
    void SyncSelectionVisuals();

    // Scroll the tree so the given item is visible.
    void ScrollToItem(TreeId id);

    // Virtualized row height control (in pixels).
    void SetRowHeight(float px);
    float GetRowHeight() const { return m_RowHeight; }

    // Child indentation per depth level (in pixels).
    void SetChildIndent(float px);
    float GetChildIndent() const { return m_ChildIndentPx; }

    // Background icon size for tree titles (in pixels).
    void SetIconSize(float px);
    float GetIconSize() const { return m_IconSizePx; }

    /** The item resize gesture (UI/Interaction/ItemResizeGesture.h) over the inner scroll view. */
    void SetOnItemResizeGesture(std::function<void(float scrollY)> cb) { m_OnItemResizeGesture = std::move(cb); }

    // Optional view-only item filter. Callers that retain matching descendants
    // must also admit their ancestors so the tree can traverse to those rows.
    void SetVisibilityFilter(std::function<bool(TreeId)> filter)
    {
        m_VisibilityFilter = std::move(filter);
        m_FlatDirty = true;
        RefreshFromProvider();
    }

    /** When true, clicking the row foldout expands/collapses the whole subtree (default: single node). */
    void SetFoldoutTogglesEntireSubtree(bool v) { m_FoldoutTogglesEntireSubtree = v; }

    /** When true, Shift+Up/Down behaves like plain Up/Down (no range selection). Used by trees
     *  whose content isn't meaningful to multi-select (e.g. the Settings panel category tree). */
    void SetKeyboardSingleSelectOnly(bool v) { m_KeyboardSingleSelectOnly = v; }
    /** When false, clicking empty space in the tree does NOT clear the selection (default: true). */
    void SetClearSelectionOnBackgroundClick(bool v) { m_ClearSelectionOnBackgroundClick = v; }

    // Debug/telemetry helpers (used by UIManager debug capture).
    float GetScrollOffsetX() const { return m_Scroll ? m_Scroll->GetScrollX() : 0.0f; }
    float GetScrollOffsetY() const { return m_Scroll ? m_Scroll->GetScrollY() : 0.0f; }
    float GetViewportWidth() const { return m_Scroll ? m_Scroll->GetViewportWidth() : 0.0f; }
    float GetViewportHeight() const { return m_Scroll ? m_Scroll->GetViewportHeight() : 0.0f; }
    float GetContentWidth() const { return m_Scroll ? m_Scroll->GetContentWidth() : 0.0f; }
    float GetContentHeight() const { return m_Scroll ? m_Scroll->GetContentHeight() : 0.0f; }
    float GetViewportLayoutY() const { return m_Scroll ? m_Scroll->GetLayoutY() : GetLayoutY(); }
    // Diagnostics: first visible row, desired window, and flat size come straight
    // from the core's last resolved guard (ItemCount == flat size for a 1-lane view).
    int GetFirstVisibleIndex() const { return m_WindowCore.LastGuard().First; }
    int GetDesiredRowCount() const { return m_WindowCore.LastGuard().Desired; }
    size_t GetLastKnownFlatSize() const
    {
        return static_cast<size_t>(std::max(0, m_WindowCore.LastGuard().ItemCount));
    }
    int GetLastViewportWPx() const { return m_LastViewportWPx; }

    struct DebugRowLayoutInfo
    {
        int boundVisibleRows = 0;
        int overlaps = 0;
        float minDeltaY = 0.0f; // min(nextY - prevY) across bound rows (sorted by boundIndex)
        float minRowH = 0.0f;
        float maxRowH = 0.0f;
    };
    DebugRowLayoutInfo GetDebugRowLayoutInfo() const;

    struct DebugBoundRowInfo
    {
        int slotIndex = -1;  // index into m_RowPool (helps detect ring-scroll reuse)
        int boundIndex = -1; // index into m_Flat
        TreeId boundId = 0;
        int depth = 0;
        bool attached = true;
        std::string label;
    };
    // Returns up to maxRows bound pooled rows (sorted by boundIndex).
    // Intended for UIReplay/diagnostics; avoid calling in hot paths.
    std::vector<DebugBoundRowInfo> DebugGetBoundRows(int maxRows = 32) const;

    void OnEvent(UIEvent& e) override;

    // Drag/drop target hooks (optional; default is no-op).
    void SetAcceptsPayload(std::function<bool(UI::Interaction::PayloadTypeId)> cb) { m_DropAccepts = std::move(cb); }
    void SetOnCanDrop(std::function<UI::Interaction::DropFeedback(const UI::Interaction::DropRequest&)> cb) { m_OnCanDrop = std::move(cb); }
    void SetOnPerformDrop(std::function<void(const UI::Interaction::DropRequest&)> cb) { m_OnPerformDrop = std::move(cb); }

    // Drag source hook (optional): called when a drag gesture begins. Receives the id of the row
    // under the press (the drag source) so the payload can be built without depending on selection
    // having been committed (see SetSelectOnMouseUp). Convention: drag the whole selection when the
    // source is part of it, otherwise drag just the source.
    void SetDragPayloadBuilder(std::function<UI::Interaction::DragPayload(TreeId dragSourceId)> cb) { m_DragPayloadBuilder = std::move(cb); }

    // When true, a plain (unmodified) left-click commits selection on mouse-UP instead of mouse-DOWN,
    // and a drag does not change selection at all. Shift/Ctrl, right-click, and keyboard selection
    // stay immediate. This lets a row be dragged elsewhere (e.g. onto an inspector field) without the
    // press re-selecting it and swapping whatever the selection drives. Pair with a drag payload
    // builder (the builder receives the drag-source id since selection isn't committed yet).
    void SetSelectOnMouseUp(bool v) { m_SelectOnMouseUp = v; }

    // IDropTarget
    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

    // IVirtualizedControl (C-8 per-frame provider change pump).
    uint64_t ProviderChangeVersion() const override { return m_Provider ? m_Provider->GetChangeVersion() : 0; }
    void EnqueuePumpWork(UIManager& ui) override;

    // Maintain the manager's virtualized-control pump registry.
    void OnOwnerManagerChanged(UIManager* owner) override;

  private:
    void RebuildFlatList();
    // id -> index into m_Flat via m_FlatIndexById, or -1. Consistent with m_Flat
    // because both are only (re)built together in RebuildFlatList.
    int FlatIndexOf(TreeId id) const;
    void EnsureRowPoolSize(int desired);
    void EnsureAttachedRows(int desired);
    // Returns true when the bind marked the row subtree dirty (binding changed or
    // this bind is part of a data refresh) so the Host reports a Rebind impact.
    bool BindRow(int slotIndex, int itemIndex);
    void UnbindRow(int slot);
    void DestroyTailSlot();
    void UpdateVirtualization(VirtualizationCoordinator::Reason reason);
    void UpdateSelectionClasses();
    void UpdateDropPreviewClasses();

    // Whether a row should render with the "selected" visual. Normally mirrors the selection
    // model, but while a drag is in flight from an unselected row it tentatively highlights the
    // dragged row instead (reverted when the drag ends without committing — see m_DragHighlightId).
    bool IsRowVisuallySelected(TreeId id) const;

    // Clear the transient drag-source highlight and repaint (no-op if not active). Called at every
    // gesture end that doesn't commit a selection (drag end, no-drag threshold cross, safety).
    void ClearDragHighlight();

    void SetSubtreeExpanded(TreeId root, bool expanded);

    // Bridges the flat row pool to the shared window engine (defined in the .cpp).
    struct HostAdapter;

    void OnPostLayout() override;

    ITreeDataProvider* m_Provider = nullptr;    // not owned
    uint64_t m_LastProviderChangeVersion = 0;
    // Last provider StructureVersion the consume path acted on. Movement means a
    // structural mutation (add/remove/reorder/expandability) that must rebuild
    // the flat row set — see ChangeTrackingProviderBase's contract note (C-7).
    uint64_t m_LastProviderStructureVersion = 0;
    TreeChangeSet m_PendingChangeSet;
    bool m_PendingChangeSetValid = false;
    // Manager whose virtualized-control pump registry currently holds this view
    // (mirrors ScrollView::m_RegisteredScrollManager).
    UIManagerRef m_RegisteredVirtualizationManager;
    UI::Interaction::ISelectionModel* m_Selection = nullptr; // not owned

    friend struct UIEventHandlerAccess;

    // Slot bits for the callback members a native user module can own. A module's
    // callable stored in one of these outlives the module's image exactly as a
    // handler-table entry does, so they stamp and revoke through the same
    // protocol (ModuleOwnedCallback.h).
    //
    // Every m_On* member is here. The other callable members
    // (m_VisibilityFilter, m_DropAccepts, m_DragPayloadBuilder) are NOT — they
    // fall outside the m_On* naming the phase-2 census was drawn on, and remain
    // unstamped.
    static constexpr std::uint32_t kOnItemResizeGestureSlot = 1u << 0;
    static constexpr std::uint32_t kOnExpansionChangedSlot = 1u << 1;
    static constexpr std::uint32_t kOnScrollChangedSlot = 1u << 2;
    static constexpr std::uint32_t kOnSelectionChangedSlot = 1u << 3;
    static constexpr std::uint32_t kOnItemActivatedSlot = 1u << 4;
    static constexpr std::uint32_t kOnContextMenuSlot = 1u << 5;
    static constexpr std::uint32_t kOnRowBoundSlot = 1u << 6;
    static constexpr std::uint32_t kOnIconClickedSlot = 1u << 7;
    static constexpr std::uint32_t kOnIconStateCheckSlot = 1u << 8;
    static constexpr std::uint32_t kOnIconStateSetSlot = 1u << 9;
    static constexpr std::uint32_t kOnLockClickedSlot = 1u << 10;
    static constexpr std::uint32_t kOnLockStateCheckSlot = 1u << 11;
    static constexpr std::uint32_t kOnLockStateSetSlot = 1u << 12;
    static constexpr std::uint32_t kOnCanDropSlot = 1u << 13;
    static constexpr std::uint32_t kOnPerformDropSlot = 1u << 14;

    // Every slot, listed once. The four revocation virtuals all walk this, so a
    // slot cannot be covered by some of them and missed by others.
    template <typename Fn>
    void ForEachOwnedSlot(Fn&& fn);
    template <typename Fn>
    void ForEachOwnedSlot(Fn&& fn) const;

    std::size_t CountMemberSlotsOwnedByImage(std::uint64_t base, std::uint64_t size) const override;
    void CollectMemberSlotsOwnedByImage(std::uint64_t base, std::uint64_t size,
                                        std::uint32_t& slotMask) override;
    std::size_t ReleaseCollectedMemberSlots(std::uint32_t slotMask) override;
    std::size_t DropMemberSlotStampsOutsideMappedImages() override;

    UI::ModuleOwnedCallback<void(float)> m_OnItemResizeGesture{this, kOnItemResizeGestureSlot};
    UI::ModuleOwnedCallback<void(TreeId, bool)> m_OnExpansionChanged{this, kOnExpansionChangedSlot};
    UI::ModuleOwnedCallback<void(float)> m_OnScrollChanged{this, kOnScrollChangedSlot};
    bool m_FoldoutTogglesEntireSubtree = false;
    bool m_KeyboardSingleSelectOnly = false;
    bool m_ClearSelectionOnBackgroundClick = true;

    // Keyboard cursor row: moves independently of the selection anchor so
    // Shift+Up/Down can extend a range past its initial endpoint. 0 = unset.
    TreeId m_KeyboardCursorId = 0;

    // Internal scroll + content container
    float MeasureNaturalRowWidth() const;

    ScrollView* m_Scroll = nullptr; // not owned (child)
    bool m_HorizontalScrollEnabled = false;
    // Last pass that could actually measure rows, so a pass with none does not
    // read as "the content is narrow".
    mutable float m_LastNaturalWidth = 0.0f;
    UIElement* m_Content = nullptr; // not owned (child of m_Scroll viewport)

    struct FlatItem
    {
        TreeId id = 0;
        int depth = 0;
    };

    struct RowSlot
    {
        UIElement* row = nullptr;  // container element
        Label* toggle = nullptr;   // foldout toggle (may be hidden when not expandable)
        Label* title = nullptr;    // label
        UIElement* lock = nullptr; // lock icon (right-side hover action)
        TreeId boundId = 0;        // current id bound to this slot
        int boundIndex = -1;       // index into m_Flat
        int boundDepth = 0;
        bool attached = true;
        std::unique_ptr<UIElement> detached; // ownership when not attached to DOM
    };

    std::vector<FlatItem> m_Flat;
    std::vector<RowSlot> m_RowPool;

    // Shared index-space window engine: owns window math, ring-slot mapping, pool
    // ensure/shrink orchestration, changeset application, safety-net repair, and
    // aggregated impact reporting. TreeView feeds it one lane (a line is a
    // fixed-height row) and resolves first-visible via FirstFromUniform.
    UI::VirtualWindowCore m_WindowCore;

    // id -> index into m_Flat, rebuilt with m_Flat in RebuildFlatList (kept
    // consistent because m_Flat is only mutated there). Replaces the O(N) flat
    // scans behind ScrollToItem, shift-click range, type-ahead, and arrow-key nav.
    FastHashMap<TreeId, int> m_FlatIndexById;

    // Scratch reused each update to map a Subset changeset's ids to the flat pool
    // slots currently bound to them (keeps the core id-type-free).
    std::vector<int> m_ChangedSlotScratch;

    // OnPostLayout viewport-coalescing cache (independent of the core's guard):
    // avoids re-enqueuing virtualization work every layout pass while idle.
    int m_LastViewportWPx = -1;
    int m_LastViewportHPx = -1;

    float m_RowHeight = 22.0f;
    float m_ChildIndentPx = 16.0f;
    float m_IconSizePx = 14.0f;
    int m_Overscan = 4;
    bool m_FlatDirty = true;
    // True while UpdateVirtualization rebinds after a data-driven flat-list
    // rebuild: a row can rebind the same id at the same flat index with
    // changed underlying content, so BindRow must restyle even when the
    // binding compare says "unchanged". Re-derived on every virtualization
    // pass.
    bool m_BindingFromDataRefresh = false;

    UI::ModuleOwnedCallback<void(TreeId)> m_OnSelectionChanged{this, kOnSelectionChangedSlot};
    UI::ModuleOwnedCallback<void(TreeId)> m_OnItemActivated{this, kOnItemActivatedSlot};
    UI::ModuleOwnedCallback<void(TreeId, float, float)> m_OnContextMenu{this, kOnContextMenuSlot};
    UI::ModuleOwnedCallback<void(TreeId, UIElement*)> m_OnRowBound{this, kOnRowBoundSlot};
    UI::ModuleOwnedCallback<void(TreeId, UIElement*)> m_OnIconClicked{this, kOnIconClickedSlot};
    std::function<bool(TreeId)> m_VisibilityFilter;
    // Returns true if enabled, false if disabled
    UI::ModuleOwnedCallback<bool(TreeId)> m_OnIconStateCheck{this, kOnIconStateCheckSlot};
    // Sets enabled/disabled state
    UI::ModuleOwnedCallback<void(TreeId, bool, UIElement*)> m_OnIconStateSet{this, kOnIconStateSetSlot};
    UI::ModuleOwnedCallback<void(TreeId, UIElement*)> m_OnLockClicked{this, kOnLockClickedSlot};
    UI::ModuleOwnedCallback<bool(TreeId)> m_OnLockStateCheck{this, kOnLockStateCheckSlot};
    UI::ModuleOwnedCallback<void(TreeId, bool, UIElement*)> m_OnLockStateSet{this, kOnLockStateSetSlot};

    // Drop target state
    std::function<bool(UI::Interaction::PayloadTypeId)> m_DropAccepts;
    UI::ModuleOwnedCallback<UI::Interaction::DropFeedback(const UI::Interaction::DropRequest&)>
        m_OnCanDrop{this, kOnCanDropSlot};
    UI::ModuleOwnedCallback<void(const UI::Interaction::DropRequest&)>
        m_OnPerformDrop{this, kOnPerformDropSlot};
    UI::Interaction::DropPreviewState m_DropPreview{};
    UIElement* m_DropIndicator = nullptr; // owned by root UI tree (created on demand)

    // Drag source state
    std::function<UI::Interaction::DragPayload(TreeId)> m_DragPayloadBuilder;
    bool m_DragCandidate = false;
    TreeId m_DragCandidateId = 0;
    float m_DragStartX = 0.0f;
    float m_DragStartY = 0.0f;

    // If user clicks a selected item within a multi-selection, collapse to single on mouse-up
    // unless the gesture becomes a drag.
    TreeId m_PendingCollapseToSingleId = 0;

    // Select-on-mouse-up (opt-in via SetSelectOnMouseUp). When set, a plain left-press records the
    // row to select here and defers the commit to mouse-up; drag-start clears it so a drag never
    // changes selection. 0 = nothing pending.
    bool m_SelectOnMouseUp = false;
    TreeId m_PendingSelectId = 0;

    // Transient drag-source highlight: while dragging a row that wasn't already selected, that row
    // shows the "selected" visual (without committing the model or swapping the inspector). Cleared
    // when the drag ends — a reorder commits the real selection, an EntityField drop reverts.
    bool m_DragHighlightActive = false;
    TreeId m_DragHighlightId = 0;

    // Liveness token for the onDragEnded callback: the cross-window drag router owns the drag
    // session and can outlive this TreeView (e.g. the source window is closed mid-drag, destroying
    // the tree AND its UIManager). The callback captures a weak_ptr to this and no-ops if expired.
    std::shared_ptr<int> m_LifeToken = std::make_shared<int>(0);

    // Auto-expand on hover while dragging (tree-based drops).
    TreeId m_AutoExpandHoverId = 0;
    std::chrono::steady_clock::time_point m_AutoExpandHoverStart{};

    // Simple double-click detection state (mouse up based)
    std::chrono::steady_clock::time_point m_LastClickTime{};
    TreeId m_LastClickedId = 0;

    // Options/state
    bool m_ShowRoot = false;
    std::set<TreeId> m_Expanded;
    
    // Drag-to-toggle state for entity icons (enable/disable)
    bool m_IconDragging = false;
    bool m_IconDragTargetState = false; // The state to apply to all items (true = enable, false = disable)
    std::set<TreeId> m_IconAppliedInCurrentDrag;

    // Drag-to-toggle state for lock icons
    bool m_LockEnabled = false; // true only when SetOnLockClicked has been called
    bool m_LockDragging = false;
    bool m_LockDragTargetState = false;
    std::set<TreeId> m_LockAppliedInCurrentDrag;

    // Coalesce refresh requests that can arrive mid-frame (e.g. from editor change notifications).
    // We sometimes need a second virtualization pass after the UIManager has safely exited event
    // dispatch / post-layout phases to avoid "jumping" row rects until the next user input.
    bool m_RefreshPosted = false;

    // Flat-list generation counter: increments whenever we rebuild the flattened
    // item list. Carried in the core guard's StructureGeneration so a flat rebuild
    // breaks the ring shortcut and forces a full rebind against the new mapping.
    std::uint64_t m_FlatGeneration = 1;

    std::chrono::steady_clock::time_point m_TypeAheadLastKeyTime{};
    int m_TypeAheadRepeatKey = -1;
};

} // namespace GameEngine
