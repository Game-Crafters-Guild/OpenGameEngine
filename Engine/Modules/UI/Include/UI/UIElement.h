#pragma once
#include "UI/UiDispatcher.h"
#include "UI/ITextMeasurable.h"
#include "Scheduler/Scheduler.h"
#include "UI/UiContext.h"
#include "UI/UiPostHandle.h"
#include "UI/UiPostTarget.h"
#include "UI/ModuleOwnedHandlers.h"



#include <deque>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <set>
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <cassert>
#include <type_traits>
#include <typeinfo>

#include "Types/StringId.h"
#include "UI/UIEvents.h"
#include "UI/UIStyle.h"
#include "UI/StyleOverrides.h"
#include "UI/ResolvedStyle.h"



namespace GameEngine {

namespace Rendering { namespace Text { class FontAtlas; } }

class UIManager;
class TextInput;
class Manipulator;
struct RetainedYogaNode; // forward; full def in UIManager.h

namespace UI { struct PrimitiveEmitContext; }
namespace UI { template <typename Sig> class ModuleOwnedCallback; }
struct ResolvedStyle;

// Internal: UIElements notify their owning UIManager when they become dirty so
// the retained-mode UI update can early-out when nothing changed.
void UIManagerNotifyElementDirty(UIManager* owner, UIElement* el, unsigned flags);
// Internal: UIElements notify their owning UIManager when their tree structure
// changes (children added/removed, portal targets swapped). This allows Update()
// to detect mid-frame mutations without conservative rebuilds.
void UIManagerNotifyTreeStructureChanged(UIManager* owner);
// Internal: UIElements notify UIManager when ownership changes so the manager
// can attach any requested subtree stylesheets from cached UIStyle assets.
void UIManagerNotifyElementOwnerChanged(UIManager* owner, UIElement* el);
// Internal: track/untrack elements that own local (subtree-attached) stylesheets
// so the manager can seed those sheets into the dynamic style analysis every frame
// (otherwise the subtree-skip fast path can drop them and break :hover/:active).
void UIManagerRegisterLocalSheetElement(UIManager* owner, UIElement* el);
void UIManagerUnregisterLocalSheetElement(UIManager* owner, UIElement* el);
void UIManagerUnregisterTransitioningElement(UIManager* owner, UIElement* el);
void UIManagerUnregisterElementInstanceId(UIManager* owner, UIElement* el);
void UIManagerNotifyElementContentDirty(UIManager* owner, UIElement* el);
// NOT PUBLIC API. The one piece of the attach/detach machinery that header-inline code
// reaches (SetOwnerManager below, and Mount's portal edits), which is why it is declared
// here and its internal siblings are not — those live in UI/Internal/AttachDetachInternal.h
// because every caller of them is a .cpp.
//
// Records that `el` sits at the top of a subtree whose root-reachability may have changed,
// so the manager's once-per-frame settle walks it and dispatches kEventAttachedToPanel /
// kEventDetachedFromPanel where the current state differs from the last state dispatched.
// Enqueueing is always safe — the settle is edge-triggered, so a queued element that did
// not actually change dispatches nothing.
namespace UI::Detail
{
void EnqueueAttachSettle(UIManager* owner, UIElement* el);
} // namespace UI::Detail

namespace UI
{
// Dispatch kEventDetachedFromPanel post-order over a subtree that is ABOUT to be destroyed,
// synchronously, while every element in it is still fully alive and still resolvable —
// never from a destructor. Call it where destruction is DECIDED: RemoveChild, a reconcile
// dropping children, AdoptRoot replacing a tree, ~UIManager, and the owner of an element
// no UI tree owns (a docked panel held by the application) before it destroys it. Needs no
// manager: it reads the element's own dispatched state and handler table, so it serves
// ownerless doomed subtrees too. Before the detach events it releases the owner's focus
// from the subtree (UIManager::ReleaseFocusInDoomedSubtree), so a focused control hears
// FocusOut while it can still commit.
void DispatchDestructionDetach(UIElement* doomedRoot);
} // namespace UI

// Event-driven UI rewrite, Stage 1 (additive): return any persistent
// render slots (primitive range, clip slot, draw-order slot) owned by
// `el` to the manager's allocators. Called from ~UIElement when the
// element owns slots. No-op for elements with no slots allocated, which
// is every element today (Stage 2+ wires the allocations).
void UIManagerFreeRenderSlots(UIManager* owner, UIElement* el);
// Stage 5 Block A part 3f: forwarder so SetOwnerManager (header-inline,
// can't include UIManager.h) can clean up the old manager's dirty queues
// + focus-within chain when an element changes owner.
void UIManagerRemoveFromDirtyQueues(UIManager* owner, UIElement* el);
// Route an element's own :checked / custom-state flip through the manager's
// analysis-driven pseudo marker (UIManager::MarkPseudoStateScope). Raises
// LayoutDirty only when the matching rules affect layout and fans out per
// their descendant/sibling combinators — replacing the value path's
// unconditional LayoutDirty and the custom-state path's blind self StyleDirty.
void UIManagerMarkCheckedStateScope(UIManager* owner, UIElement* el);
void UIManagerMarkCustomStateScope(UIManager* owner, UIElement* el);
// Cascade-memoization Phase 6 (sibling-combinator precision): true if
// any selector in the manager's effective stylesheet set uses `+` or
// `~`. Class/id mutation sites consult this to decide whether
// following-sibling caches need invalidation (the conservative "always
// invalidate" path is wasteful for the common case where stylesheets
// don't use sibling combinators).
bool UIManagerUsesSiblingCombinators(const UIManager* owner);
// P4 (C-10): manager-wide rule-cache epoch, bumped whenever stylesheet
// content or the global sheet set changes. Element rule caches stamp the
// epoch at build time and are only trusted when it still matches —
// subtree invalidation walks only reach the rooted tree, so detached
// subtrees (inactive dock tabs, pooled rows) would otherwise keep
// dangling CSSRule pointers across a hot reload.
uint32_t UIManagerRuleCacheEpoch(const UIManager* owner);

// Detach a child's Yoga node from its parent's Yoga tree before the
// child UIElement is destroyed. Reads each side's m_YogaNode (void*
// held on UIElement to avoid yoga dependencies in this header) and
// calls YGNodeRemoveChild when both nodes exist and the child is
// currently a Yoga child of the parent. Idempotent — no-op when
// either node is missing or the relationship doesn't hold.
// Independent of any UIManager so it works for elements with cleared
// m_OwnerManager (mid-detach) and across managers.
void UIElementDetachYogaChild(UIElement* parent, UIElement* child);

// Symmetric to UIElementDetachYogaChild. Ensures both elements have
// retained Yoga nodes (creates if missing), detaches the child from
// any prior Yoga parent, and inserts it at the END of the parent's
// Yoga child list. CSS `order` re-sort is left to the post-cascade
// InsertChildrenSortedByOrder pass — reading a child's resolved order
// requires the cascade to have run, so we can't sort at insert time.
//
// No-op if neither element is attached to a UIManager (the caller is
// constructing an off-line tree; Yoga setup happens at SetOwnerManager
// time via BuildYogaRecursive).
void UIElementAttachYogaChild(UIElement* parent, UIElement* child);

namespace UIParsing { struct ElementState; }

// Overlay layer for elements that need to escape their parent stacking context
// (tooltips, dropdowns, modals, drag previews). Ordered by render priority.
enum class OverlayLayer : uint8_t
{
    None = 0,
    Dropdown,
    // Panel-owned popups (e.g. camera-bookmark previews) that outrank
    // dropdowns but must stay beneath modals.
    Tooltip,
    Modal,
    // Save / Don't Save / Cancel and other action-required dialogs. Torn-off
    // FloatingPanel windows also use Modal so they escape the dock clip;
    // they must never cover a dialog the user has to answer.
    BlockingDialog,
    // Context menus, above panel popups, modals and dialogs alike: a menu can
    // be opened from inside any of them, and whatever it was opened over must
    // not paint on top of it. Below DragPreview, which nothing can overlap
    // while it is up.
    ContextMenu,
    DragPreview,
    // The UIManager-owned hover-help bubble (TooltipOverlay). Always last:
    // it describes whatever is under the cursor right now — including
    // controls inside dropdowns, modals, and drag overlays — so it must
    // render above all of them.
    HoverTooltip
};

// Type tag for fast UIElement kind dispatch without dynamic_cast. Debug builds
// pay a non-trivial RTTI string-compare per dynamic_cast; BuildYogaRecursive
// does `dynamic_cast<Mount*>` and the signature block does
// `dynamic_cast<WeightedPane*>` on every traversed element, so a virtual
// one-byte-returning Kind() shaves measurable time off the slow path.
enum class UIElementKind : uint8_t
{
    Default = 0,
    Mount,
    WeightedPane,
};

class UIElement {
public:
    enum DirtyFlags : unsigned {
        StyleDirty    = 1u << 0,
        LayoutDirty   = 1u << 1,
        VisualDirty   = 1u << 2,
        ChildrenDirty = 1u << 3,
        // Summary bit: set on an element when either the element itself or any
        // descendant has a layout-affecting dirty flag (Style/Layout/Children or
        // an override equivalent). Propagated up the parent chain in MarkDirty
        // with early-stop. UIManager::BuildYogaRecursive uses this to short-circuit
        // cascade/override/measure/insert work on clean subtrees. Cleared at the
        // end of BuildYogaRecursive once a subtree has been verified clean.
        // VisualDirty does not set SubtreeDirty (paint-only, no layout impact).
        SubtreeDirty  = 1u << 4
    };

    // Constructors and destructor are out-of-line so that the implicit
    // destruction of m_YogaState (std::unique_ptr<RetainedYogaNode>,
    // forward-declared above) happens in UIElement.cpp where the full
    // RetainedYogaNode type is visible. Without this, every TU that
    // constructs a UIElement subclass would need to include UIManager.h
    // to satisfy unique_ptr's complete-type requirement.
    UIElement();
    explicit UIElement(const std::string& id);
    virtual ~UIElement();

    // UIElement is not copyable or movable. The StyleOverrides dirty callback
    // captures `this` as its context pointer (see WireOverridesDirtyCallback),
    // so a copy would cross-point and a move would dangle. UI trees are
    // always owned through std::unique_ptr<UIElement>, so this restriction
    // is consistent with how every caller uses the class.
    UIElement(const UIElement&) = delete;
    UIElement& operator=(const UIElement&) = delete;
    UIElement(UIElement&&) = delete;
    UIElement& operator=(UIElement&&) = delete;

    // Weak handle to an element the holder does not own. A callback that outlives the
    // subtree it was created in (an app-owned picker's live-preview callbacks, a deferred
    // action) must capture one of these instead of a raw widget pointer: Get() yields the
    // element while it is alive and nullptr once it has been destroyed. The element owns
    // the control block and allocates it on the first MakeWeakRef, so an element nobody
    // refers to weakly pays one null pointer.
    //
    // Get() is a snapshot for the duration of the call that uses it — the element can
    // still be destroyed by anything the caller does afterwards, so re-Get() rather than
    // stash the pointer.
    //
    // UI thread only. The control block does not own the element (its deleter is a no-op), so
    // holding the lock longer would not keep the element alive: off the UI thread a Get() races
    // the element's destruction however the pointer is held. To get back to an element from
    // another thread, post through a UI::UiPostHandle and Get() inside the posted action.
    template <typename T = UIElement>
    class WeakRef
    {
      public:
        WeakRef() = default;

        T* Get() const
        {
            const std::shared_ptr<UIElement> alive = m_Block.lock();
            return alive ? static_cast<T*>(alive.get()) : nullptr;
        }

      private:
        friend class UIElement;
        explicit WeakRef(std::weak_ptr<UIElement> block) : m_Block(std::move(block)) {}

        std::weak_ptr<UIElement> m_Block;
    };

    template <typename T>
    static WeakRef<T> MakeWeakRef(T* element)
    {
        static_assert(std::is_base_of_v<UIElement, T>, "WeakRef targets are UIElements");
        return WeakRef<T>(element ? element->WeakRefBlock() : std::weak_ptr<UIElement>{});
    }

    // One-byte type tag used to dispatch without dynamic_cast on hot paths
    // (BuildYogaRecursive Mount branch, signature-block WeightedPane branch).
    // Subclasses override and return a value from the UIElementKind enum;
    // the default here keeps the common case cheap.
    virtual UIElementKind Kind() const { return UIElementKind::Default; }

    const std::string& GetId() const { return m_Id; }
    StringId GetIdHash() const { return m_IdHash; }
    void SetId(const std::string& id)
    {
        // Recycled cells reapply stable IDs; unchanged IDs cannot affect selectors.
        if (m_Id == id)
            return;
        m_Id = id;
        m_IdHash = id.empty() ? StringId{0} : HashStringId(id);
        // Cascade-memoization Phase 2: id is part of the structural match
        // key (`#foo` selectors). Self + descendants need cache rebuild —
        // descendants because `#foo .child` selectors target descendants
        // through the parent's id.
        InvalidateRuleCacheSubtree();
        // Cascade-memoization Phase 6: if the stylesheet set uses sibling
        // combinators (`#foo + .bar`, `#foo ~ .baz`), following siblings
        // also need their caches rebuilt.
        if (UIManagerUsesSiblingCombinators(m_OwnerManager))
            InvalidateRuleCacheFollowingSiblings();
    }
    uint64_t GetInstanceId() const { return m_InstanceId; }

    // The canonical tag this element was created as, or 0 when it was constructed directly
    // in C++ rather than through ElementFactoryRegistry::Create. Prefer
    // ElementFactoryRegistry::GetElementTagId, which supplies the RTTI answer for the 0 case;
    // this raw accessor exists for the registry and for callers that specifically need to
    // know whether the element carries a stamp at all.
    StringId GetTagId() const { return m_TagId; }


    // Hot-reload bookkeeping. When non-zero, this element is considered to be
    // managed by a specific hot-reload binding (layout subtree). This lets the
    // reconciler remove template-authored nodes that were removed from the
    // source asset while preserving runtime/code-added nodes (bindingId==0 or
    // different binding).
    uint64_t GetHotReloadBindingId() const { return m_HotReloadBindingId; }
    void SetHotReloadBindingId(uint64_t id) { m_HotReloadBindingId = id; }

    // Classes
    //
    // Storage: parallel vectors `m_ClassNames` (strings, stable for debug
    // and serialization) and `m_ClassIds` (deterministic StringId hashes,
    // used by the cascade hot path). The vectors are insertion-ordered;
    // duplicate adds are ignored. Typical element class count is 2-4, so
    // a small vector beats `std::set`/`std::unordered_set` on cache.
    void AddClass(const std::string& className)
    {
        AddClassInternal(className, HashStringId(className));
    }
    void RemoveClass(const std::string& className)
    {
        RemoveClassInternal(HashStringId(className));
    }
    bool HasClass(const std::string& className) const
    {
        return HasClass(HashStringId(className));
    }
    bool HasClass(StringId classId) const
    {
        for (StringId id : m_ClassIds)
        {
            if (id == classId)
                return true;
        }
        return false;
    }
    const std::vector<std::string>& GetClasses() const { return m_ClassNames; }
    const std::vector<StringId>& GetClassIds() const { return m_ClassIds; }
    // True when any [attr] selector could match per-element state. Used by
    // cascade sharing to disqualify attribute-carrying elements (attribute
    // values are not part of the share key).
    bool HasSelectorAttributes() const { return !m_Attributes.empty(); }

    // Stage 5 Block C: custom pseudo-state API. Selectors of the form
    // `:my-state` match elements whose custom-state set contains "my-state".
    // Custom names are stored by hashed StringId (case-insensitive: hashed
    // from lowercased string at the convenience overloads).
    //
    // Cascade-memoization Phase 5: bodies live in UIElement.cpp so they
    // can consult CSSParser::IsRuleCacheEnabled() and the per-element
    // CustomStateRules predicate to narrow the dirty mark — adding or
    // removing a custom state can only flip a selector match if the
    // element's rule cache references at least one `:foo-state(...)`
    // selector.
    void AddCustomState(StringId stateId);
    void RemoveCustomState(StringId stateId);
    bool HasCustomState(StringId stateId) const
    {
        for (StringId id : m_CustomStateIds)
            if (id == stateId) return true;
        return false;
    }
    // String convenience overloads — case-folded to lowercase before
    // hashing to match the CSS parser's :my-state normalization.
    void AddCustomState(std::string_view stateName);
    void RemoveCustomState(std::string_view stateName);
    bool HasCustomState(std::string_view stateName) const;
    const std::vector<StringId>& GetCustomStateIds() const { return m_CustomStateIds; }

    // Tree structure
    UIElement* GetParent() const { return m_Parent; }
    const std::vector<std::unique_ptr<UIElement>>& GetChildren() const { return m_Children; }
    std::vector<std::unique_ptr<UIElement>>& GetMutableChildren() { return m_Children; }

    // DFS-parent for cascade purposes — same edge BuildYogaRecursive
    // bakes into NodeRec.parentIndex. For Mount portal targets this is
    // the Mount host (the target's m_DfsParent), not the target's owning
    // UIElement parent (m_Parent). For everything else it falls back to
    // m_Parent. Set/cleared by Mount::SetTarget; otherwise nullptr.
    UIElement* GetDfsParent() const { return m_DfsParent ? m_DfsParent : m_Parent; }
    void SetMountHostAsDfsParent(UIElement* host) { m_DfsParent = host; }
    // Owning UIManager (set by UIManager when attached as root, and propagated on AddChild)
    void SetOwnerManager(UIManager* owner)
    {
        // Attach/detach settle, enqueue site 1. THIS call is the top of the owner change:
        // ApplyOwnerManager recurses into the children itself, and root-reachability is
        // inherited along that same edge, so the whole subtree flips as a unit and one
        // queue entry covers it. The entry goes to the manager that is still going to run
        // an Update afterwards — the new owner when there is one, otherwise the old owner,
        // which is the only one left to announce the detach.
        UIManager* const settleQueue =
            (m_OwnerManager != owner) ? (owner ? owner : m_OwnerManager) : nullptr;
        ApplyOwnerManager(owner);
        if (settleQueue)
            UI::Detail::EnqueueAttachSettle(settleQueue, this);
    }

  private:
    // SetOwnerManager's body, minus the settle enqueue: the recursion must not enqueue a
    // descendant that the top's queue entry already covers.
    void ApplyOwnerManager(UIManager* owner)
    {
        if (m_OwnerManager != owner)
        {
            // Stage 2: free render slots in the OLD manager before swapping.
            // After the swap the new manager will allocate fresh slots on
            // the next PrimitiveGen visit. Without this, slots would leak in
            // the old manager and the element would render ghost primitives
            // in the old buffer.
            if (m_OwnerManager &&
                (m_PrimitiveRangeCap > 0 || m_ClipSlotIdx != 0xFFFFu ||
                 m_DrawOrderIdx != 0xFFFFFFFFu))
            {
                UIManagerFreeRenderSlots(m_OwnerManager, this);
            }

            // Stage 5 Block A part 3f: clean up the OLD manager's dirty
            // queues + focus-within chain. Elements re-parented across
            // managers (modal popups, floating panels) would otherwise
            // leave dangling references in the old manager — which the
            // next RebuildFocusWithinChain or drain would dereference.
            // RemoveFromDirtyQueues early-outs the queue scan when
            // m_InQueueFlags == 0, so the only added cost is one chain
            // map probe (skipped via the InFocusChain bit guard inside
            // RemoveFromDirtyQueues).
            if (m_OwnerManager && m_InQueueFlags != 0)
            {
                UIManagerRemoveFromDirtyQueues(m_OwnerManager, this);
            }

            // Subtree-attached stylesheets: drop this element from the OLD
            // manager's analysis-seed set before the owner swaps. RemoveChild ->
            // SetOwnerManager(nullptr) is the reliable detach point, so entries
            // never dangle past an element's removal.
            if (m_OwnerManager && !m_Stylesheets.empty())
                UIManagerUnregisterLocalSheetElement(m_OwnerManager, this);

            // Declared-transition registry: same detach point. The new
            // owner's first cascade re-registers (a freshly attached
            // subtree is MarkDirtySubtree'd, so the cascade always runs).
            if (m_OwnerManager && !m_ResolvedStyle.Transitions.IsEmpty())
                UIManagerUnregisterTransitioningElement(m_OwnerManager, this);

            // instanceId map: drop the OLD manager's mapping; the new
            // owner's NotifyElementOwnerChanged inserts into its own map.
            if (m_OwnerManager)
                UIManagerUnregisterElementInstanceId(m_OwnerManager, this);

            // Cascade-memoization Phase 7: cross-manager owner change
            // invalidates the structural rule cache. The new manager's
            // stylesheet set is almost certainly different (and even if
            // identical, the SheetSetInterner instance differs, so cached
            // sheetIndex values are meaningless). Walk the subtree to
            // mark caches stale; they rebuild lazily on the new owner's
            // next cascade.
            InvalidateRuleCacheSubtree();

            m_OwnerManager = owner;
            RoutePostsTo(owner);
            // Invalidate cached glyph placements — they reference the old
            // UIManager's FontAtlas pages which may differ from the new one.
            if (m_TextMeasurable)
                m_TextMeasurable->ResetTextShapeCache();
            // Stage 2: same wraparound concern for the PrimitiveGen frame
            // counter — reset to 0 so a stale value from a previous owner
            // can't accidentally match the new owner's counter.
            m_LastPrimitiveGenFrame = 0;
            UIManagerNotifyElementOwnerChanged(owner, this);
            // ...and register with the NEW manager so this element's local sheets
            // seed the new owner's style analysis each frame.
            if (owner && !m_Stylesheets.empty())
                UIManagerRegisterLocalSheetElement(owner, this);
            OnOwnerManagerChanged(owner);
        }
        for (auto& ch : m_Children)
            if (ch)
                ch->ApplyOwnerManager(owner);
    }

    // Points m_PostTarget, and m_PostLink's route when a handle exists, at `owner`'s dispatcher,
    // or at nothing for a detached element.
    void RoutePostsTo(UIManager* owner);

  public:
    UIManager* GetOwnerManager() const { return m_OwnerManager; }

    virtual void AddChild(std::unique_ptr<UIElement> child);
    /** Insert a child at a valid index (0 = first). Same ownership rules as AddChild. */
    virtual void InsertChild(size_t index, std::unique_ptr<UIElement> child);
    virtual void RemoveChild(UIElement* child);
    // Remove every child of this element. Prefer this over the
    // `while (!GetChildren().empty()) { RemoveChild(GetChildren().front().get()); }`
    // pattern: that pattern infinite-loops during event dispatch because
    // RemoveChild defers and the live children list does NOT shrink
    // synchronously, so each iteration re-processes the same head and
    // queues an unbounded stream of deferred removals (freeze + leak).
    // RemoveAllChildren snapshots the children list first so each child
    // is touched exactly once regardless of whether RemoveChild runs
    // synchronously or deferred.
    virtual void RemoveAllChildren();
    // Remove a child from this element and return ownership to the caller.
    // Useful for virtualization pools that want to detach elements from the DOM
    // without destroying them.
    //
    // REPARENT CONTRACT (important for primary-flip correctness):
    // If the caller intends to re-add the returned element to another parent
    // via AddChild/InsertChild, BOTH calls MUST happen before the next
    // UIManager::Update(). Between TakeChild and AddChild, the element is
    // absent from any parent's children list and owner-less; the UI tree is
    // in a torn state.
    //
    // If the caller needs to hold a detached element for longer than one
    // frame (pooling, async reparent), consider a virtualization-style
    // Mount slot instead; element staying outside the tree across frames is
    // intentional there and the Mount owns the re-attach timing.
    virtual std::unique_ptr<UIElement> TakeChild(UIElement* child);

    UIElement* FindById(std::string_view id);

    // Tooltip text shown on hover. Empty string means no tooltip.
    void SetTooltip(std::string text) { m_TooltipText = std::move(text); }
    const std::string& GetTooltip() const { return m_TooltipText; }

    // Preferred tooltip placement direction relative to the source element.
    // Auto picks above/below the cursor (existing behaviour).
    enum class TooltipPlacement : uint8_t { Auto = 0, Right, Left, Above, Below };
    void SetTooltipPlacement(TooltipPlacement p) { m_TooltipPlacement = p; }
    TooltipPlacement GetTooltipPlacement() const { return m_TooltipPlacement; }

    // Inline style text (original string, kept for inspection/serialization).
    const std::string& GetInlineStyleText() const { return m_InlineStyleText; }

    // Pseudo-class hook for CSS :checked.
    virtual bool IsPseudoChecked() const { return false; }

    // Per-element stylesheet attachment. Stylesheets added here affect this
    // element and its descendants. Effective ordering across globals and
    // ancestors is resolved by UIManager during the top-down style/layout pass.
    void AddStylesheet(const StylesheetHandle& sheet);
    void RemoveStylesheet(const StylesheetHandle& sheet);
    const std::vector<StylesheetHandle>& GetStylesheets() const { return m_Stylesheets; }
    // Replace a block of stylesheets, preserving relative ordering by inserting the new block
    // at the earliest position occupied by any stylesheet in oldBlock.
    void ReplaceStylesheetBlock(const std::vector<const Stylesheet*>& oldBlock,
                                const std::vector<StylesheetHandle>& newBlock);

    // SDF primitive generation hook: controls emit custom UIPrimitives here.
    // Called after base background/text but before child recursion.
    //
    // Thread contract (parallel drain): the drain may invoke this on a
    // JobSystem worker until the element has been OBSERVED emitting custom
    // primitives (m_CustomEmitObserved — set only after a call returns), so
    // zero-emit calls AND the first-ever emitting call can land off-thread.
    // Every override must therefore start with the escalation preamble:
    //
    //     if (ctx.OffThread) { if (ctx.EscalateFlag) *ctx.EscalateFlag = true; return; }
    //
    // unless its entire body is provably pure (own members + ctx.Emit only —
    // ctx.EmitText is itself escalating and safe). The manager backstops a
    // missing preamble by escalating + asserting on a first emission observed
    // off-thread, but any shared-state work done before that point has
    // already raced. Also note escalated items re-run the override on the UI
    // thread, so pre-escalation member mutations must be idempotent.
    virtual void OnGeneratePrimitives(UI::PrimitiveEmitContext&,
                                      const ResolvedStyle&,
                                      float, float, float, float) {}

    // Post-layout hook: called by UIManager after Yoga layout has been computed
    // and layout rects have been assigned to elements, but before input routing
    // and geometry generation. Controls can use this to update internal state
    // derived from layout (e.g., ScrollView scrollbar visibility) without
    // requiring UIManager to special-case control types.
    virtual void OnPostLayout() {}

    // The registry has finished applying a document's authored attributes to this element.
    // Runs on every application — first parse, document rebuild, and .uxml reconcile — so an
    // element that binds attributes through something other than the registry's name-keyed
    // handler channel can pick up a hot-edited value here.
    //
    // The map is the one the registry was handed: keys are lower-case, and it is the
    // authored set, not the effective one (defaults have been applied to the element by the
    // time this runs, but are not in here).
    virtual void OnAuthoredAttributesApplied(
        const std::unordered_map<std::string, std::string>& /*attrsLower*/)
    {
    }

    // Called when this element's mount visibility changes (attached to or detached
    // from a visible Mount). Fired exactly once per transition:
    //   - isVisible=false on the old target BEFORE the swap
    //   - isVisible=true on the new target AFTER the swap and owner-manager propagation
    //   - isVisible=true on first attach, isVisible=false on Mount destruction/nullptr
    // Override in virtualized controls (ListView, TreeView, GridView) to invalidate
    // viewport caches deterministically.
    virtual void OnMountVisibilityChanged(bool /*isVisible*/) {}

    // Type-dispatch hooks for primitive generation, avoiding dynamic_cast in the hot path.
    virtual TextInput* GetAsTextInput() { return nullptr; }
    virtual UIElement* GetMountTarget() { return nullptr; }

    // Cached interface pointer set by text-bearing subclasses (Label, TextInput, TextArea)
    // during construction. Non-text elements leave this null for zero overhead at lookup time.
    ITextMeasurable* GetTextMeasurable() const { return m_TextMeasurable; }

  protected:
    friend struct UIAttributeAccess;
    friend struct UILayoutAccess;
    // The attach/detach settle's window onto the three members it owns
    // (m_AttachQueueOwner, m_AttachWalkOwner, m_AttachStateDispatched). Kept off
    // the public API because
    // nothing outside the settle and its enqueue sites may write them: an element whose
    // dispatched-state bit disagrees with what its subscribers were actually told is how
    // this mechanism silently drops events.
    friend struct UIAttachStateAccess;
    // Grants UITests the handler-vector census DispatchToHandlers' compaction is defined
    // in terms of. The public API reports only whether a handler fires, so a dead
    // entry that is never swept is invisible from outside; this friend keeps that
    // guarantee testable without widening the element's surface.
    friend struct UIEventHandlerAccess;
    // Control callback slots stamp and release through the protected member-slot
    // protocol below (NoteMemberSlotStamped / ClearMemberSlotStamp /
    // MemberSlotKey / ExecutingHandlerScope), so the slot type is part of that
    // protocol rather than an outside caller.
    template <typename Sig> friend class UI::ModuleOwnedCallback;
    // Called when this element is attached to (or detached from) a UIManager.
    // Useful for controls that want to attach subtree-local stylesheets via AssetManager.
    virtual void OnOwnerManagerChanged(UIManager* /*owner*/) {}

  public:
    // Request that a UIStyle asset (CSS) be attached to this element subtree when it
    // becomes owned by a UIManager. This is asset-driven and cached by AssetManager.
    //
    // Example: `RequestSubtreeStyleAssetPath("UI/Controls/Foldout.css");`
    //
    // The path is resolved to a UIStyle asset once the element is owned by a UIManager,
    // then cached by GUID on the element so we don't keep re-resolving paths.
    // A source alias identifies the control's shipped stylesheet. When that
    // source is mounted, never let a project file with the same path shadow it.
    // Without that source (standalone games), retain normal path resolution.
    struct SubtreeStyleAssetRequest {
        std::string Path;
        std::string SourceAlias;
        bool operator==(const SubtreeStyleAssetRequest&) const = default;
    };
    void RequestSubtreeStyleAssetPath(const std::string& assetPath, const std::string& sourceAlias = {});
    // Cached resolved style GUIDs (persist across owner changes).
    const std::vector<GUID>& GetRequestedSubtreeStyleAssetGuids() const { return m_RequestedSubtreeStyleAssetGuids; }
    // Pending paths (consumed by UIManager on owner attach).
    std::vector<SubtreeStyleAssetRequest> ConsumeRequestedSubtreeStyleAssetPaths();
    // Internal: record a resolved style GUID (deduped).
    void AddRequestedSubtreeStyleGuid(const GUID& guid);

    // Live background image override (programmatic, fast-path).
    // Applied by UIManager after CSS resolution
    // so it behaves like a highest-priority inline override without string parsing.
    struct LiveTextureRef
    {
        uint32_t Handle = 0; // Rendering::RGTextureHandle (kept as uint32_t to avoid heavyweight includes)
        std::function<uint32_t()> Getter{};
        bool IsValid() const { return Handle != 0 || (bool)Getter; }
    };

    struct StyleWriter
    {
        UIElement* el = nullptr;
        explicit StyleWriter(UIElement* e) : el(e) {}

        // Example:
        //   el->Styles()
        //     .SetBackgroundResourceName("scene_main")
        //     .SetBackgroundSizeContain()
        //     .SetBackgroundTint(0xFFFFFFFF);
        //
        // Notes:
        // - Live textures are routed through UIManager's external texture registry using a stable internal key.
        // - CSS `background-image` still works; these overrides behave like a high-priority inline style.

        // Background image sources
        StyleWriter& SetBackgroundResourceName(const std::string& name);
        StyleWriter& SetBackgroundImageNone();    // override to none
        StyleWriter& ResetBackgroundImage();      // remove override; fallback to CSS

        // Background image parameters
        StyleWriter& SetBackgroundTint(uint32_t argb);
        StyleWriter& ResetBackgroundTint();

        StyleWriter& SetBackgroundRepeat(BackgroundRepeat repeat);
        StyleWriter& ResetBackgroundRepeat();

        StyleWriter& SetBackgroundSizeAuto();
        StyleWriter& SetBackgroundSizeCover();
        StyleWriter& SetBackgroundSizeContain();
        StyleWriter& SetBackgroundSizeExplicit(float x, bool xIsPercent, float y, bool yIsPercent);
        StyleWriter& ResetBackgroundSize();

        StyleWriter& SetBackgroundPosition(float x, bool xIsPercent, float y, bool yIsPercent);
        StyleWriter& SetBackgroundPositionPercent(float xPercent, float yPercent);
        StyleWriter& ResetBackgroundPosition();
    };

    StyleWriter Styles() { return StyleWriter(this); }
    const LiveTextureRef* GetBackgroundImageTextureOverride() const
    {
        return m_BackgroundImageTextureRef.IsValid() ? &m_BackgroundImageTextureRef : nullptr;
    }
    bool HasBackgroundImageTextureOverride() const { return m_BackgroundImageTextureRef.IsValid(); }

    // Typed style overrides (sparse property bag).
    StyleOverrides& Overrides() { return m_Overrides; }
    const StyleOverrides& Overrides() const { return m_Overrides; }
    StyleOverrides& InlineOverrides() { return m_InlineOverrides; }
    const StyleOverrides& InlineOverrides() const { return m_InlineOverrides; }

    const ResolvedStyle& GetResolvedStyle() const { return m_ResolvedStyle; }
    ResolvedStyle& GetMutableResolvedStyle() { return m_ResolvedStyle; }

    // Overlay layer for escaping parent stacking contexts
    void SetOverlayLayer(OverlayLayer layer)
    {
        if (m_OverlayLayer == layer)
            return;
        const bool wasOverlay = (m_OverlayLayer != OverlayLayer::None);
        const bool willBeOverlay = (layer != OverlayLayer::None);
        m_OverlayLayer = layer;
        if (wasOverlay != willBeOverlay)
            RefreshOverlaySubtreeBitAndPropagate();
    }
    OverlayLayer GetOverlayLayer() const { return m_OverlayLayer; }

    // True if this element OR any descendant — through both `m_Children`
    // and (for Mount hosts) the mount target — has a non-None overlay
    // layer. Maintained on SetOverlayLayer / AddChild / InsertChild /
    // RemoveChild / TakeChild / Mount::SetTarget. In-flight overlay
    // changes inside a mount target propagate to the host Mount via
    // MountRegistry::NotifyTargetOverlayBitChanged.
    //
    // Consumed by UIManager::HitTestTree to gate subtree culling: when an
    // ancestor's effective clip excludes the cursor, the walk normally
    // prunes the subtree. Pruning is unsafe if a descendant is on a
    // non-None overlay layer, because overlay elements clear their
    // inherited clip on entry and may be hittable outside the ancestor's
    // bounds (Dropdown items, modal SearchDialog, drag preview). When
    // this bit is set, the walk continues into the subtree but only
    // descends through children that themselves carry the bit.
    bool HasOverlayInSubtree() const { return m_HasOverlayInSubtree; }

    // Event handling (bubble-only for most events)
    using EventHandler = std::function<void(UIEvent&)>;

    struct EventHandlerToken {
        EventId Id{0};
        std::uint64_t Key{0};
        explicit operator bool() const { return Id != 0 && Key != 0; }
    };

    EventHandlerToken RegisterEventHandler(EventId id, EventHandler handler) {
        const std::uint64_t key = ++m_NextHandlerKey;
        const std::uint64_t owner = ResolveHandlerOwner(handler);
        m_EventHandlers[id].push_back({key, std::move(handler), false, true, owner});
        m_HandlerBits |= EventHandlerBit(id);
        if (owner) NoteOwnerStampAdded();
        return EventHandlerToken{ id, key };
    }

    EventHandlerToken RegisterEventHandlerOnce(EventId id, EventHandler handler) {
        const std::uint64_t key = ++m_NextHandlerKey;
        const std::uint64_t owner = ResolveHandlerOwner(handler);
        m_EventHandlers[id].push_back({key, std::move(handler), true, true, owner});
        m_HandlerBits |= EventHandlerBit(id);
        if (owner) NoteOwnerStampAdded();
        return EventHandlerToken{ id, key };
    }

    // Attribute an ALREADY-REGISTERED entry to the image that owns `inner`.
    //
    // For the one shape the ownership stamp cannot see through: engine code that
    // wraps a caller-supplied callable in a callable of its own. The outer
    // std::function is Engine.dll's, so the entry resolves to Engine.dll and the
    // user callable inside it stays invisible — the same nesting 722094c79 had to
    // delete from Button. Where the nesting is load-bearing (GameUI's session
    // gate), the wrapper passes the INNER callable here.
    //
    // It takes the callable, NOT an owner address, so it cannot be used to
    // manufacture ownership: the image is resolved here by the same rule the
    // ordinary registration path uses, and an engine-owned or unattributable
    // inner callable resolves to zero and is ignored. Stamping an engine
    // control's handler as module-owned is therefore not expressible.
    template <class R, class... A>
    void AdoptHandlerOwnerFrom(const EventHandlerToken& t, const std::function<R(A...)>& inner) {
        if (!t || !inner) return;
        const std::uint64_t owner = UI::AttributeCallableOwner(inner.target_type());
        if (!owner) return;
        auto it = m_EventHandlers.find(t.Id);
        if (it == m_EventHandlers.end()) return;
        for (auto& he : it->second) {
            if (he.key != t.Key) continue;
            if (!he.ownerCodeAddr) {
                he.ownerCodeAddr = owner;
                NoteOwnerStampAdded();
            }
            break;
        }
    }

    // Deactivating the entry is not enough to release the callable, and the
    // callable is the dangerous part: a std::function built by a hot-swappable
    // native user module carries that module's vtable, and destroying one reads
    // through that vtable. An entry left holding its callable is destroyed
    // eventually whatever happens — by the drain that erases it, or with the
    // element — and if the owning image has been unmapped by then, that read is
    // into freed address space. Destroy it here, while that image is still mapped.
    //
    // The one exception is a handler unregistering ITSELF: destroying the
    // std::function under its own frame would free the captures it is still
    // running out of. That entry keeps its callable, and the sweep at the end of the
    // OUTERMOST dispatch on this element — the first point at which no frame is
    // standing on the entry — erases both. The test is
    // "is THIS handler executing", not "is any dispatch running" — clicking the
    // toolbar's Stop button runs play-mode teardown (and every OnDestroy
    // unregister it performs) inside a UI dispatch, which is precisely the flow
    // that has to release.
    //
    // (The SWEEP in DispatchToHandlers asks the broader question — "is ANY frame of
    // this element running" — and the difference is deliberate. Releasing a callable
    // is per-handler and must happen as early as it safely can, while the image is
    // still mapped. Erasing an ENTRY moves its neighbours, so it is unsafe while any
    // frame is standing anywhere in the deque.)
    bool UnregisterEventHandler(const EventHandlerToken& t) {
        if (!t) return false;
        auto it = m_EventHandlers.find(t.Id);
        if (it == m_EventHandlers.end()) return false;
        // THE RELEASE RULE, for every path that drops a callable out of the
        // handler table:
        //
        // Never clear a table-resident callable by assignment. Destroying it runs
        // its captures' destructors, which for a module handler is module code, and
        // that code can register on this element. The deque keeps the entry's storage
        // stable across that insertion; std::function's assignment
        // is not atomic across that: it destroys the target first and only then
        // writes its own storage empty, so `he.handler = nullptr` puts that write
        // in the freed buffer. The callable moves into a sink that outlives every
        // reference into the table instead.
        //
        // WHAT THE SINK BUYS, exactly: it defers the module code for a callable
        // MSVC keeps on the heap (capture > 48 bytes measured on this toolchain
        // — 56 is the impl constant, the usable inline budget is smaller — or a
        // throwing move) and for one
        // whose captures null on move — a shared_ptr capture is empty in the
        // moved-from copy, so its destructor does nothing. It does NOT defer it for
        // a callable held INLINE whose captures survive a move, a small struct with
        // a working destructor being the plain case: std::function::swap only
        // pointer-swaps when BOTH sides are heap-held, so against an empty sink it
        // takes the three-way move and destroys the moved-from source IN PLACE,
        // here, with `he` live.
        //
        // What makes that survivable is the OTHER half of the rule: the per-id
        // container is a std::deque, so the registration that module code performs
        // appends without moving `he`. The sink orders the module code; the
        // node-based storage is what keeps the entry it runs on top of alive. Both
        // are required — with a std::vector the swap's own trailing write landed in
        // the buffer that registration had just freed.
        //
        // (A self-unregister keeps both callable and stamp: the callable is still
        // running, and the outermost dispatch's sweep clears the stamp when it erases
        // the entry. Holding the stamp until then is the safe direction — it keeps the
        // quiesce ledger pinning the owning image for as long as the callable exists.)
        EventHandler doomed;
        auto& vec = it->second;
        bool changed = false;
        for (auto& he : vec) {
            if (he.key == t.Key) {
                DeactivateHandler(he);
                if (!IsHandlerExecuting(t.Key)) {
                    ClearOwnerStamp(he);
                    doomed.swap(he.handler);
                }
                changed = true;
                break;
            }
        }
        return changed;
    }

    // Attaches a manipulator — a reusable gesture (UI/Interaction/Manipulator.h) — to this
    // element. The one supported way to attach one; a manipulator subscribes through here or
    // not at all.
    //
    //   btn->AddManipulator(ContextMenuManipulator::Create(onMenuRequested));
    //
    // OWNERSHIP, and it is not what the signature suggests. Passing a shared_ptr in does NOT
    // hand the element ownership: the manipulator's own subscriptions in this element's
    // handler table hold the only strong references, and the element keeps a WEAK entry. The
    // difference is observable exactly once, at module unload — revoking a module's handlers
    // destroys the manipulator with them, which is what stops a menu closure from outliving
    // the module that built it. An element that owned the manipulator would keep that closure
    // alive past the unmap. Callers that intend to call RemoveManipulator later keep the
    // shared_ptr Create() handed them; callers that do not may pass it straight in.
    //
    // Returns false and logs, attaching nothing, if the manipulator is null, if THIS INSTANCE
    // is already attached to an element, or if a live manipulator of the SAME concrete type is
    // already attached here.
    //
    // The instance rule is one element at a time: a manipulator records its subscriptions on
    // itself, and those records name no element, so an instance spanning two would unregister
    // the wrong element's handlers on removal. Attach a separate instance to each element.
    //
    // The type rule is that a second manipulator of one type could never fire anyway — the
    // first one's press handler claims the event and the dispatch stops there — so a silent
    // second attach is a call-site error that eats a callback. Different types are
    // unrestricted; they claim different gestures.
    bool AddManipulator(const std::shared_ptr<Manipulator>& manipulator);

    // Detaches a manipulator previously added here: it is disarmed (dropping any in-flight
    // gesture and the pressed visual with it) and every subscription it holds on this element
    // is unregistered. Dropping the last subscription destroys the manipulator, so the caller's
    // shared_ptr may be the only thing left holding it afterwards.
    //
    // Returns false if this manipulator is not attached to this element.
    bool RemoveManipulator(const std::shared_ptr<Manipulator>& manipulator);

    virtual void OnEvent(UIEvent&) {}

    // ONE dispatch API, two overloads, and the compiler picks by what the caller knows.
    //
    //   DispatchEvent(e)                    — the id is only known at run time. Event
    //                                         routing, anything reading e.Id from input.
    //   DispatchEvent<kEventButtonClick>(e) — a control raising its OWN semantic event, where
    //                                         the id is a literal at the call site.
    //
    // Both do the same thing; the second just knows more, so it can test ONE bit against a
    // constant instead of asking whether this element has any handler at all. That matters
    // because the control-event callers are the hot ones: an inspector writing fields in bulk,
    // a slider dragged at pointer rate, both running with nothing subscribed.
    //
    // Overloads rather than a tag parameter, and the resolution is unambiguous by construction
    // rather than by luck: an explicit template argument list excludes the non-template
    // overload outright (a non-template function cannot take one), and a bare call cannot
    // select the template because kEventId is not deducible from the arguments. So each form
    // has exactly one viable candidate. A tag-type parameter would have worked too, but it
    // makes every call site carry a second argument that exists only to steer the compiler.
    //
    // OnEvent runs unconditionally in BOTH. It is the CONTROL's own handling, not a
    // subscriber's, so skipping it when nobody is listening would change behaviour rather than
    // skip work.
    void DispatchEvent(UIEvent& e) {
        OnEvent(e);
        // No handler of ANY id has ever been registered here, so there is nothing
        // to look up. Strictly cheaper than the miss it replaces, and safe for the
        // same reason the per-id bits are: bits are only ever cleared where the
        // id's vector is provably empty.
        if (m_HandlerBits == 0u)
            return;
        DispatchToHandlers(e);
    }

    template <EventId kEventId>
    void DispatchEvent(UIEvent& e) {
        static_assert(EventHandlerBit(kEventId) != kEventEscapeHandlerBit,
                      "The compile-time DispatchEvent needs an id listed in kNamedEventIds; an "
                      "unlisted id shares the escape bit and cannot be tested precisely");
        OnEvent(e);
        if ((m_HandlerBits & EventHandlerBit(kEventId)) == 0u)
            return;
        DispatchToHandlers(e);
    }

    // CONSERVATIVE presence probe for one named id — the same bit DispatchEvent<kEventId>
    // tests. True means "at least one handler MAY be registered": the bit is cleared only
    // where the id's handler deque is provably empty, so it can be stale-set, never
    // stale-clear — false is definitive. For callers that must pay something BEFORE they
    // can dispatch (the string value payload's dispatch-window copy), so the
    // nothing-subscribed path skips that cost along with the dispatch.
    template <EventId kEventId>
    bool HasSubscribedEventHandlers() const {
        static_assert(EventHandlerBit(kEventId) != kEventEscapeHandlerBit,
                      "The presence probe needs an id listed in kNamedEventIds; an unlisted id "
                      "shares the escape bit and cannot be tested precisely");
        return (m_HandlerBits & EventHandlerBit(kEventId)) != 0u;
    }

    // Defer actions to a safe point. Prefer TLS UiContext; fall back to owner UIManager.
    //
    // Callable from any thread. Off the UI thread there is no UI context, so the action goes
    // to the dispatcher of the manager that owns this element at the instant of the call and
    // runs on the UI thread at that manager's next drain. The element itself must be alive for
    // the call; the manager need not be — if it has been destroyed, the action is dropped.
    //
    // Returns false when the action is DROPPED rather than run inline, so it will never
    // execute: no dispatcher or scheduler is reachable, or the owning manager is gone. A caller
    // that latched state on the expectation of it running (a "scheduled once" flag) must
    // release that latch on false, or the latch outlives the action that was supposed to clear
    // it. A dropped action is destroyed on the calling thread.
    //
    // A caller on another thread that cannot guarantee the element outlives the call — a
    // file-watch, job or download callback holding a widget pointer — takes a GetPostHandle()
    // on the UI thread instead and posts through that.
    bool PostAction(std::function<void()> action);

    // A handle that posts to this element's UI thread from any thread without dereferencing
    // the element, and skips the action if the element is destroyed before it runs. UI thread
    // only. See UI::UiPostHandle.
    UI::UiPostHandle GetPostHandle();

private:
    // The handler-table walk, shared by both dispatch entry points above. Never
    // call it directly: the entry points are what apply the presence test.
    void DispatchToHandlers(UIEvent& e) {
        // Read the id ONCE, up front. A handler may legitimately reuse and re-target the event
        // object it was handed, and the compaction below clears a bit keyed on this id — reading
        // e.Id again down there could clear the bit belonging to a DIFFERENT event.
        const EventId dispatchedId = e.Id;
        auto it = m_EventHandlers.find(dispatchedId);
        if (it == m_EventHandlers.end())
            return;
        auto& vec = it->second;
        // THE WALK IS BY KEY, and that choice is what makes it allocate nothing at any
        // subscriber count. Two invariants of the per-id deque carry it:
        //
        //   ASCENDING BY KEY — entries are only ever appended (RegisterEventHandler and
        //     RegisterEventHandlerOnce) with a key from the strictly increasing
        //     ++m_NextHandlerKey, and the single erase path (DrainInactiveHandlers'
        //     remove_if) preserves relative order.
        //   KEYS ARE STABLE, INDICES ARE NOT — a key names one entry for the element's
        //     lifetime and is never reissued, so it survives compaction. An INDEX does
        //     not: a handler's return can drain the table (~ExecutingHandlerScope, when
        //     the outermost frame unwinds), and the drain slides every survivor down over
        //     the entries it drops. An index walk therefore skips one live subscriber for
        //     every entry erased ahead of its cursor.
        //
        // So each step asks the deque for the first entry ABOVE the last key offered a
        // turn — a partition point on ascending keys, O(log n), no allocation, and correct
        // whatever the table did during the previous call.
        //
        // WHAT IT DOES NOT DO IS ISOLATE THE CALL, and it must not. Every step re-finds the
        // LIVE entry and re-reads its `active`, so a handler that unregisters a LATER
        // subscriber actually stops that subscriber from running. The price is that a frame
        // stands on a deque element for the whole call, which is what DrainInactiveHandlers'
        // outermost-frame rule exists to protect. The table's node-stable storage makes
        // invoking a live entry safe against REGISTRATION — pinned by
        // ModuleOwnedHandlerRevocationTests'
        // AHandlerThatRegistersDuringItsOwnDispatchStillReadsItsCaptures — but it says
        // nothing about erasure.
        //
        // A sibling's callable is NOT held alive across the walk, deliberately. Revocation
        // releases the callables of entries that are not currently executing, and it does so
        // in its own sink while the owning image is still mapped (ReleaseCollectedHandlers).
        // Holding copies here would move those destructions to the end of the dispatch, which
        // is only safe for as long as something pins the image until then.
        //
        // THE SET IS FIXED BEFORE THE FIRST CALL by this bound. m_NextHandlerKey is the
        // highest key ever issued on this element, so every entry that exists now is at or
        // below it, and everything a handler registers during the walk is above it: those
        // subscribers fire on the NEXT dispatch of this id, not this one.
        const std::uint64_t lastKeyAtEntry = m_NextHandlerKey;
        // Highest key already OFFERED a turn — offered, not run. An entry found inactive
        // still advances it, or the search below would return that same entry forever.
        std::uint64_t offeredKey = 0;
        for (;;) {
            auto cur = std::partition_point(
                vec.begin(), vec.end(),
                [offeredKey](const HandlerEntry& he) { return he.key <= offeredKey; });
            if (cur == vec.end() || cur->key > lastKeyAtEntry) break;
            offeredKey = cur->key;
            // Unregistered, revoked, or a spent `once`. Erasure is deferred, so an entry
            // in any of those states is still sitting here and is skipped by this test.
            if (!cur->active) continue;
            // Read what we need BEFORE invoking: the handler may register another
            // handler on this same element+event; the deque keeps existing entries'
            // storage stable, but iterators are not part of that guarantee.
            // Never touch cur after handler().
            const std::uint64_t curKey = cur->key;
            // Spend a `once` entry HERE — at the moment the call is committed to,
            // not after it returns, and on the entry rather than in a list the
            // sweep consults. Both matter. The handler may dispatch this id from
            // inside itself, and `active` is the only thing that nested walk reads:
            // an entry still marked active runs a second time, and if it is the
            // handler doing the dispatching it re-enters itself without bound. The
            // callable stays — this entry is about to be the executing one — and
            // the drain erases it, exactly as for a self-unregister.
            if (cur->once) DeactivateHandler(*cur);
            // The early-out below is a TRANSITION, not a level. Sampled per handler,
            // immediately before its call, because only this handler's own effect on
            // e.Handled may end the walk.
            const bool handledBeforeCall = e.Handled;
            {
                // Publish which handler is running so UnregisterEventHandler can tell a
                // self-unregister (must keep the callable alive) from every other
                // unregister (must release it now).
                ExecutingHandlerScope executing(*this, curKey);
                cur->handler(e);
            }
            // A DELIBERATE MIDDLE POSITION, not a principle followed to its end.
            //
            // Handled carries two meanings at once: "no ancestor should see this" and "no
            // sibling after me should run". A control consuming input in its own OnEvent
            // (Button::OnEvent stops an armed press) means only the first, so an event that
            // arrives here ALREADY Handled still reaches every subscriber — otherwise one
            // control's bubbling decision starves subscribers it knows nothing about, and
            // registration order alone decides which single one survives.
            //
            // Carried to its end that argument says never break at all, since a same-element
            // subscriber is not a propagation target. This stops short of that: a subscriber
            // that flips Handled false->true DURING the walk still suppresses the ones behind
            // it, so ordinary stopPropagation keeps working — but only for the first consumer
            // of an event, since a later Stop() on an already-Handled event has no transition
            // to make. Splitting the two meanings (Handled as ancestor-ward only, plus an
            // explicit StopImmediate that ends this walk unconditionally) is what removes that
            // asymmetry; the transition test is the cheaper intermediate.
            //
            // Bubbling is untouched: the parent-ward walk reads e.Handled as a level after
            // DispatchEvent returns (UIManager_Input.cpp), and this loop never clears it.
            if (!handledBeforeCall && e.Handled) break;
        }
        // Everything the dispatch deactivated is erased by DrainInactiveHandlers,
        // which runs from here and from the last executing frame to unwind. It is
        // deferred rather than done inline because erasing moves and destroys deque
        // elements a live frame may be standing on; see the rule stated there.
        //
        // Reached with no handler frame on the stack, which is the case a dispatch
        // whose entries were ALL inactive still has to cover: no scope was ever
        // pushed, so nothing else would drain them.
        DrainInactiveHandlers();
    }

public:
    // Lifetime-safe variant: the action only executes if this element is still
    // alive (reachable via ownerManager->FindElementByInstanceId). Use this for
    // any deferred callback that captures `this`. Same threading and false-means-dropped
    // contract as PostAction.
    bool PostSafeAction(std::function<void()> action);

    // The scheduler of the UIManager whose update is running on this thread; null outside one.
    GameEngine::Scheduler::IScheduler* GetScheduler() const {
        if (auto* ctx = UI::GetTlsUiContext()) return ctx->Scheduler;
        return nullptr;
    }

    // Event-dispatch state (set by UIManager during event routing).
    // The public API stays bool-based for call sites, but internally we track depth
    // so nested dispatch scopes do not accidentally clear the state too early.
    //
    // OUT-OF-LINE ON PURPOSE (double-link disease): these were header-inline
    // over an inline-static depth counter, so every module compiling them
    // (Editor.exe vs Engine.dll) read its OWN copy. Dispatch raised Engine's
    // counter while Editor-compiled widgets always saw 0 — their dispatch
    // guards never fired, and clear-children loops spun forever against
    // Engine's correctly-deferring RemoveChild (46 GB dispatcher queue).
    // Defined in UIElement.cpp so all modules share Engine's single counter.
    static void SetInEventDispatch(bool v);
    static bool IsInEventDispatch();

	    // Last computed layout rectangle (absolute coordinates). Read-only
	    // outside the layout solver — mutate via Overrides() /
	    // UI::Layout::SetAbsolutePosition, not by writing this cache.
	    float GetLayoutX() const { return m_LastX; }
	    float GetLayoutY() const { return m_LastY; }
	    float GetLayoutWidth() const { return m_LastW; }
	    float GetLayoutHeight() const { return m_LastH; }

	    // Opt-in: quantise this element's committed rect onto whole DEVICE pixels.
	    // Yoga's grid is 1/64 of a device pixel, which is what lets a length Chrome
	    // reports as 20.8px survive the solve — but an element that hosts a render
	    // target has to consume it 1:1, and a fractional rect turns every
	    // destination pixel into a blend of source texels. Set on world-viewport
	    // elements only; everything else keeps the 1/64 grid.
	    //
	    // Takes effect from the element's next layout SOLVE, not immediately: the
	    // rect commit prunes subtrees whose relative layout and origin are both
	    // unchanged, so a flag flipped on a fully settled tree is not seen until
	    // something re-solves. Set it where the element is bound (panel mount /
	    // hot-reload reconciliation), which is always followed by one.
	    void SetSnapRectToDevicePixels(bool v)
	    {
	        if (m_SnapRectToDevicePixels == v)
	            return;
	        m_SnapRectToDevicePixels = v;
	        MarkDirty(LayoutDirty);
	    }
	    bool SnapsRectToDevicePixels() const { return m_SnapRectToDevicePixels; }

	    // Padding as the layout solve RESOLVED it, in the same logical px as the
	    // layout rect. `ResolvedStyle::Layout.Padding` is the style INPUT and
	    // still carries a bare percentage NUMBER when PaddingIsPercent is set,
	    // so everything running after the solve — auto-sizing, content boxes,
	    // caret geometry — has to read this instead, or it contributes "10"
	    // where the solve used 10% of the containing block's width. Border needs
	    // no counterpart: CSS has no percentage border-width.
	    const Box4& GetLayoutPadding() const { return m_LayoutPadding; }

	    /// Returns true if the absolute point (x,y) is inside this element's layout rect.
	    bool ContainsPoint(float x, float y) const
	    {
	        return x >= m_LastX && y >= m_LastY
	            && x <= (m_LastX + m_LastW) && y <= (m_LastY + m_LastH);
	    }

	    // Hit-test bounds (can be larger than layout bounds for invisible interactive areas)
	    // Override in derived classes to expand hit area beyond visual bounds (e.g., splitters)
	    virtual void GetHitTestBounds(float& outX, float& outY, float& outW, float& outH) const
	    {
	        outX = m_LastX;
	        outY = m_LastY;
	        outW = m_LastW;
	        outH = m_LastH;
	    }

	    // Focusability and tab order
	    void SetFocusable(bool v) { m_Focusable = v; }
	    bool IsFocusable() const { return m_Focusable; }

	    void SetTabIndex(int idx) { m_TabIndex = idx; }
	    int GetTabIndex() const { return m_TabIndex; }

	    // Enabled/Disabled state (UI-level enable; does not force style changes)
    void SetEnabled(bool enabled)
    {
        if (m_Enabled == enabled)
            return;
        m_Enabled = enabled;
        MarkDirtySubtree(StyleDirty | LayoutDirty | VisualDirty);
    }
    void SetDisabled(bool disabled) { SetEnabled(!disabled); }
    bool IsEnabled() const { return m_Enabled; }
    // False when this element or any ancestor is disabled. Disabling a container
    // disables what it contains, the rule HTML gives the contents of a disabled
    // fieldset, and it is what focus asks: a disabled element and everything
    // inside it is skipped by Tab, refuses focus from a click, and loses focus it
    // already holds. The tree walks that collect focus candidates carry the
    // answer down instead of calling this per element.
    bool IsEnabledInHierarchy() const
    {
        for (const UIElement* e = this; e; e = e->GetDfsParent())
            if (!e->m_Enabled)
                return false;
        return true;
    }

    // Per-element focus pseudo-class predicates, read by the CSS matcher for
    // NON-target (ancestor / sibling) compounds — the focus counterparts to
    // IsEnabled() / IsPseudoChecked(). The owning UIManager maintains these each
    // frame in RebuildFocusWithinChain, before the cascade build pass, so they
    // are stable / read-only during a (possibly parallel) cascade:
    //   :focus-within -> the whole focused-element ancestor chain (InFocusChain)
    //   :focus        -> the focused leaf only (InFocusLeaf)
    //   :focus-visible-> the focused leaf when focus arrived via keyboard
    // The target compound still evaluates these from the passed ElementState, so
    // ComputeStyleFor callers that set state.Focus/... directly are unaffected.
    bool IsInFocusChain() const { return (m_InQueueFlags & InFocusChain) != 0; }
    bool IsPseudoFocused() const { return (m_InQueueFlags & InFocusLeaf) != 0; }
    bool IsPseudoFocusVisible() const { return (m_InQueueFlags & InFocusVisibleLeaf) != 0; }

	    // Request an immediate relayout for the owning UIManager (if attached).
	    // Used by controls that mutate layout-critical properties during input
	    // (splitters, views rebuilding children, etc.).
	    void RequestRelayout() const;
	    		
	    	    // Optional textual content hook used for geometry/debug overlays. Controls
	    // that present text (Label, TextInput-backed fields, etc.) can override
	    // this so UIManager does not need to know their concrete types when
	    // resolving display text.
	    // Overrides must return a reference to stable storage (a member variable),
	    // not a temporary. The returned reference is read during the same frame.
	    virtual const std::string& GetTextContent() const
	    {
	        static const std::string kEmpty;
	        return kEmpty;
	    }


	    // Hook for scrollable containers (e.g., ScrollView) to observe viewport
	    // layout for descendants marked with the "scroll-viewport" class
	    // without UIManager needing direct knowledge of their types.
	    virtual void OnScrollViewportLayout(UIElement* viewport, float W, float H)
	    {
	        (void)viewport;
	        (void)W;
	        (void)H;
	    }
	
	    // Focus proxy: optional indirection for internal editors (e.g. TextInput)
	    // that want focus/tab/keyboard routing to go through an owning control.
	    void SetFocusProxy(UIElement* el) { m_FocusProxy = el; }
	    UIElement* GetFocusProxy() const { return m_FocusProxy; }
	
	    UIElement* ResolveFocusTarget()
	    {
	        UIElement* cur = this;
	        int depth = 0;
	        while (cur && cur->m_FocusProxy && cur->m_FocusProxy != cur && depth < 4)
	        {
	            cur = cur->m_FocusProxy;
	            ++depth;
	        }
	        return cur ? cur : this;
	    }
	
	    const UIElement* ResolveFocusTarget() const
	    {
	        const UIElement* cur = this;
	        int depth = 0;
	        while (cur && cur->m_FocusProxy && cur->m_FocusProxy != cur && depth < 4)
	        {
	            cur = cur->m_FocusProxy;
	            ++depth;
	        }
	        return cur ? cur : this;
	    }
	
	    bool IsFocusTargetForId(const std::string& focusId) const
	    {
	        const UIElement* target = ResolveFocusTarget();
	        return target && !focusId.empty() && !target->GetId().empty() && target->GetId() == focusId;
	    }
	
	    // Dirty tracking
	    void MarkDirty(unsigned flags)
	    {
	        m_DirtyFlags |= flags;
	        // Propagate the summary bit up the parent chain when this is a
	        // layout-affecting dirty. Early-stop: hitting an ancestor that already
	        // has SubtreeDirty means all further ancestors have it too (invariant).
	        PropagateSubtreeDirtyIfNeeded(flags);
	        if (m_OwnerManager)
	        {
	            UIManagerNotifyElementDirty(m_OwnerManager, this, flags);
	        }
	    }
	    bool IsDirty(unsigned flags) const
	    {
	        if ((m_DirtyFlags & flags) != 0)
	            return true;
	        if ((flags & LayoutDirty) != 0 && m_Overrides.IsLayoutDirty())
	            return true;
	        if ((flags & VisualDirty) != 0 && m_Overrides.IsVisualDirty())
	            return true;
	        if ((flags & StyleDirty) != 0 && m_Overrides.NeedsCascadeRerun())
	            return true;
	        return false;
	    }
	    void ClearDirty(unsigned flags)
	    {
	        m_DirtyFlags &= ~flags;
	        if ((flags & (LayoutDirty | VisualDirty | StyleDirty)) != 0)
	            m_Overrides.ClearDirty();
	    }

    // Content-only mutation: this element's primitive BYTES changed (text
    // characters, image frame) but nothing layout, style resolve, or any
    // OTHER element can observe. Queues the render-side primitive drain
    // without raising dirty hints — the idle gate can take the frame; the
    // drain and its dirty-range upload run render-side regardless, so the
    // change still paints this frame at a few hundred bytes of upload.
    // Caller contract: the layout rect is stable and the change cannot
    // affect inherited/derived style. Avoid per-frame content marks on an
    // element under a display:none ANCESTOR whose stale rect intersects
    // the viewport — the drain's own-flags gate only checks the element's
    // own visibility, so such marks escalate to a full regen each frame.
    void MarkContentDirty()
    {
        if (m_OwnerManager)
            UIManagerNotifyElementContentDirty(m_OwnerManager, this);
    }

    void MarkDirtySubtree(unsigned flags)
    {
        // Set flags on self + all descendants in one pass, then propagate the
        // summary bit up the ancestor chain exactly once.
        //
        // Stage 5 Block A part 3f.2: each descendant must also land in the
        // dirty queues — drain processes only queued elements, so without
        // per-descendant notification a MarkDirtySubtree call would only
        // queue the root and drain would miss the descendants entirely.
        // Notification happens inside MarkDirtySubtreeFlagsOnlyInternal so
        // the recursion does dirty-flag setting and queue push together.
        constexpr unsigned kLayoutAffecting = StyleDirty | LayoutDirty | ChildrenDirty;
        const bool layoutAffecting = (flags & kLayoutAffecting) != 0;
        const unsigned fullFlags = layoutAffecting ? (flags | SubtreeDirty) : flags;
        MarkDirtySubtreeFlagsOnlyInternal(fullFlags);
        if (layoutAffecting)
        {
            // DFS parent, not m_Parent — see PropagateSubtreeDirtyIfNeeded.
            UIElement* p = GetDfsParent();
            while (p && (p->m_DirtyFlags & SubtreeDirty) == 0)
            {
                p->m_DirtyFlags |= SubtreeDirty;
                p = p->GetDfsParent();
            }
        }
    }

  private:
    // Actual child-removal body. The public virtual RemoveChild() defers to
    // this either inline (when called outside event dispatch) or via a queued
    // action (when called during dispatch). The queued action MUST call this
    // directly rather than re-entering RemoveChild(), otherwise the dispatch
    // re-check would re-queue infinitely if the dispatcher processes its
    // queue while still inside event dispatch.
    void RemoveChildImpl(UIElement* child);

    // Called after m_Children changed at `index`: a child was inserted there,
    // or the child that was there was removed. Structural pseudos are baked
    // into each element's rule cache, so this restyles exactly the elements
    // whose match the change can flip: this element when it crosses the
    // :empty boundary, and the siblings whose position from either end, count
    // or preceding siblings changed for a pseudo or combinator the stylesheets
    // use. `changedType` is the inserted or removed child's dynamic type, the
    // key of the :*-of-type pseudos.
    void RestyleAfterChildListChange(size_t index, bool inserted, const std::type_info& changedType);
    // Re-cascades this subtree with freshly matched rules.
    void RestyleForStructuralChange();

    void AddClassInternal(const std::string& className, StringId classId)
    {
        for (StringId existing : m_ClassIds)
        {
            if (existing == classId)
                return; // Already present; preserve insertion order.
        }
        m_ClassNames.push_back(className);
        m_ClassIds.push_back(classId);
        // Class changes almost always affect paint, and only sometimes affect layout.
        // Avoid marking LayoutDirty here so interactive input (e.g. scroll drags that rebind
        // virtualized cells) can stay on the cheap patch paths. If a class change *does*
        // affect layout, the UIManager's signature checks will request a relayout.
        MarkDirtySubtree(StyleDirty | VisualDirty);
        // Cascade-memoization Phase 2: class is the most common structural
        // match key (`.foo` selectors). Self + descendants invalidate.
        InvalidateRuleCacheSubtree();
        // Cascade-memoization Phase 6: when the stylesheet set uses
        // `+`/`~` combinators, an element's class flip changes the match
        // for following siblings (`.foo + .bar`, `.foo ~ .baz`).
        if (UIManagerUsesSiblingCombinators(m_OwnerManager))
            InvalidateRuleCacheFollowingSiblings();
    }
    void RemoveClassInternal(StringId classId)
    {
        for (size_t i = 0; i < m_ClassIds.size(); ++i)
        {
            if (m_ClassIds[i] == classId)
            {
                m_ClassIds.erase(m_ClassIds.begin() + (ptrdiff_t)i);
                m_ClassNames.erase(m_ClassNames.begin() + (ptrdiff_t)i);
                MarkDirtySubtree(StyleDirty | VisualDirty);
                // Cascade-memoization Phase 2: see AddClassInternal note.
                InvalidateRuleCacheSubtree();
                if (UIManagerUsesSiblingCombinators(m_OwnerManager))
                    InvalidateRuleCacheFollowingSiblings();
                return;
            }
        }
    }

    void PropagateSubtreeDirtyIfNeeded(unsigned flags)
    {
        constexpr unsigned kLayoutAffecting = StyleDirty | LayoutDirty | ChildrenDirty;
        if ((flags & kLayoutAffecting) == 0)
            return;
        // GetDfsParent() (not m_Parent): a Mount target is a detached root
        // whose ownership parent is null, but its DFS parent is the Mount
        // host. Following the DFS edge carries the summary bit across the
        // portal so the subtree-skip gate can trust SubtreeDirty for
        // subtrees containing Mounts.
        UIElement* p = this;
        while (p && (p->m_DirtyFlags & SubtreeDirty) == 0)
        {
            p->m_DirtyFlags |= SubtreeDirty;
            p = p->GetDfsParent();
        }
    }

    // Set flags directly on self + all descendants. Each element also
    // notifies the owner manager so the dirty queues see the whole
    // subtree (not just the root). Caller handles the one-time ancestor
    // walk after this returns.
    void MarkDirtySubtreeFlagsOnlyInternal(unsigned flags)
    {
        m_DirtyFlags |= flags;
        if (m_OwnerManager)
        {
            UIManagerNotifyElementDirty(m_OwnerManager, this, flags);
        }
        for (auto& ch : m_Children)
        {
            if (ch)
                ch->MarkDirtySubtreeFlagsOnlyInternal(flags);
        }
    }

  public:

    // Geometry/culling hints
    //
    // UIManager may skip geometry generation for elements that are fully clipped by the effective
    // ancestor clip stack (viewport scissor becomes 0x0). That is usually a good optimization, but
    // it breaks retained-mode "scroll translation" for virtualized pools: items that start fully
    // offscreen would never have geometry baked, then appear blank when scrolled into view until a
    // future full geometry rebuild.
    //
    // Virtualized controls can set this to keep geometry generation even when the element is fully
    // clipped in the current frame; the draw will still be suppressed by a 0x0 scissor.
    void SetDisableClipCulling(bool v) { m_DisableClipCulling = v; }
    bool IsClipCullingDisabled() const { return m_DisableClipCulling; }

private:
    // Wire the StyleOverrides dirty callback on construction so any mutation
    // to this element's overrides (Set/Reset/Clear, SetCustom, etc.) routes
    // through MarkDirty — which propagates SubtreeDirty up the ancestor chain.
    // Without this the override dirty bits stayed internal to StyleOverrides
    // and the Slice 2 fast path had to abort any subtree containing a
    // descendant with override-dirty.
    void WireOverridesDirtyCallback()
    {
        m_Overrides.SetDirtyCallback(
            +[](void* ctx, uint32_t f) {
                auto* self = static_cast<UIElement*>(ctx);
                unsigned mark = 0;
                if (f & StyleOverrides::Layout)  mark |= LayoutDirty;
                if (f & StyleOverrides::Visual)  mark |= VisualDirty;
                if (f & StyleOverrides::Cascade) mark |= StyleDirty;
                if (mark)
                    self->MarkDirty(mark);
            },
            this);
    }

    inline static std::atomic<uint64_t> s_NextInstanceId{1};
    uint64_t m_InstanceId = 0;
    uint64_t m_HotReloadBindingId = 0;

    // Where PostAction lands when no UI context is current: m_OwnerManager's dispatcher, held
    // by shared ownership so a worker thread's post can never reach a destroyed manager.
    // Written with m_OwnerManager on the UI thread (ApplyOwnerManager), read from any thread.
    UI::UiPostTarget m_PostTarget;

    // Shared with this element's UiPostHandles. Null until the first GetPostHandle, then kept
    // in step with m_PostTarget; ~UIElement clears it so handles stop posting and skip what they
    // already queued. The pointer itself is UI thread only; handles hold their own copy.
    std::shared_ptr<UI::UiPostLink> m_PostLink;

protected:
    struct AttributeKeyHash
    {
        using is_transparent = void;

        size_t operator()(std::string_view value) const noexcept
        {
            // Case-insensitive ASCII hash (matches attribute name normalization).
            size_t h = 1469598103934665603ull;
            for (char c : value)
            {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                h ^= static_cast<unsigned char>(c);
                h *= 1099511628211ull;
            }
            return h;
        }

        size_t operator()(const std::string& value) const noexcept { return operator()(std::string_view(value)); }
    };

    struct AttributeKeyEqual
    {
        using is_transparent = void;

        bool operator()(std::string_view a, std::string_view b) const noexcept
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                char ca = a[i];
                char cb = b[i];
                if (ca >= 'A' && ca <= 'Z')
                    ca = static_cast<char>(ca - 'A' + 'a');
                if (cb >= 'A' && cb <= 'Z')
                    cb = static_cast<char>(cb - 'A' + 'a');
                if (ca != cb)
                    return false;
            }
            return true;
        }

        bool operator()(const std::string& a, const std::string& b) const noexcept
        {
            return operator()(std::string_view(a), std::string_view(b));
        }
    };

    using AttributeMap = std::unordered_map<std::string, std::string, AttributeKeyHash, AttributeKeyEqual>;

	    // Last layout rect storage
	    float m_LastX = 0.0f;
	    float m_LastY = 0.0f;
	    float m_LastW = 0.0f;
	    float m_LastH = 0.0f;
	    Box4 m_LayoutPadding{};

    // Owning UI manager pointer (not owned). UI thread only.
    UIManager* m_OwnerManager = nullptr;

    // ---- Attach/detach settle state (UIAttachStateAccess is the only writer) ----
    //
    // ONE POINTER PER REFERENCE KIND, and they must stay separate: a manager can refer to
    // an element from its settle QUEUE (a transition may be waiting) and a DIFFERENT
    // manager can refer to the same element from the snapshot of a walk in flight. A
    // single slot cannot name both, and the one it loses is the one whose vector then
    // keeps a raw pointer nothing will ever tombstone — a use-after-free the moment the
    // element dies. Non-null IS the membership flag for each vector.
    //
    // Neither is m_OwnerManager: a detached element is still referred to by the manager it
    // just left, which is the one that has to announce the detach. ~UIElement leaves both,
    // which is what makes destruction not a detach and keeps both vectors clean.
    UIManager* m_AttachQueueOwner = nullptr;
    UIManager* m_AttachWalkOwner = nullptr;
    // Set for the duration of a destruction-detach dispatch over the subtree this element
    // belongs to. It exists so the tree-edit refusals can tell "condemned" from "alive":
    // a handler reacting to the detach must not be able to re-parent this element back
    // into the tree or adopt a child into it, because the memory is released the moment
    // the dispatch returns and a rescued pointer would dangle.
    bool m_Doomed = false;
    // The last attach state DISPATCHED to this element's subscribers. The settle
    // dispatches only where this differs from the element's current reachability, which
    // is what makes a detach-and-reattach inside one frame dispatch nothing.
    bool m_AttachStateDispatched = false;

    ITextMeasurable* m_TextMeasurable = nullptr;

    std::string m_Id;
    StringId m_IdHash = 0; // Cache of HashStringId(m_Id); 0 when m_Id is empty. Maintained by SetId.
    // The tag this element was CREATED as, and the element's type identity.
    //
    // m_TagId is the canonical tag's StringId, stamped by ElementFactoryRegistry::Create
    // (and by SetCreatedTag for elements the registry did not name). It is what
    // IsSameType and CSS type-selector matching consult, because C++ RTTI cannot tell two
    // registry types apart when they share one C++ class — which every C#-defined element
    // type does, all of them being the same native proxy.
    //
    // 0 means "created outside the registry" (an editor panel doing `new Button`), and the
    // registry falls back to RTTI for those.
    //
    // m_TagId is set once at creation and never changes: an element's tag is its identity,
    // and a reconcile that finds a different tag replaces the element rather than re-tagging it.
    StringId m_TagId = 0;
    // Display name for elements the registry's RTTI reverse map cannot name correctly: unknown
    // XML tags (no registered type at all), and proxies for types registered from outside C++
    // (a whole family of tags sharing one C++ class, so the reverse map holds one entry for all
    // of them). Both stamp it at creation. Empty means "ask the registry", which is right for
    // every ordinary control, where the C++ type does identify the tag.
    std::string m_TagName;
    std::string m_TooltipText;
    TooltipPlacement m_TooltipPlacement = TooltipPlacement::Auto;
    std::vector<std::string> m_ClassNames;
    std::vector<StringId> m_ClassIds;
    // Stage 5 Block C: custom pseudo-state ids. Small set in practice
    // (most elements have 0 custom states; controls with explicit states
    // typically carry 1-3). Stored as a vector for cache locality on the
    // MatchesSelectorChain hot path. No string copy kept — all matching
    // is done by hashed StringId.
    std::vector<StringId> m_CustomStateIds;
    AttributeMap m_Attributes;
    UIElement* m_Parent = nullptr;
    // Mount portal host: when this element is a Mount target, points to
    // the Mount UIElement that displays it. Provides the DFS-parent edge
    // that NodeRec.parentIndex tracked before Stage 7 step 6.3b. Maintained
    // synchronously by Mount::SetTarget / SwapTargetForActivation / dtor.
    // nullptr for non-Mount-targets — GetDfsParent() falls back to m_Parent.
    UIElement* m_DfsParent = nullptr;
public:
    // Owned per-element retained Yoga state (Stage 7 step 8). Allocated on
    // first BuildYogaRecursive visit; destroyed in ~UIElement when the
    // owning element dies. Declared BEFORE m_Children so that children
    // destruct first and detach their YGNodes from this element's YGNode
    // before the parent's YGNode is freed by ~RetainedYogaNode.
    //
    // Forward-declared so this header doesn't depend on UIManager.h. The
    // unique_ptr's destructor instantiates in UIElement.cpp (which does
    // include UIManager.h), so the complete-type requirement is met.
    //
    // Public to match the rest of UIElement's UI-internal mutable state
    // (m_YogaNode etc.); the field is read and written from
    // UIManager_Layout.cpp, UIManager_Update.cpp, etc.
    std::unique_ptr<RetainedYogaNode> m_YogaState;
protected:
    std::vector<std::unique_ptr<UIElement>> m_Children;
    unsigned m_DirtyFlags = 0;
    bool m_DisableClipCulling = false;

    // Stylesheets attached directly to this element.
    std::vector<StylesheetHandle> m_Stylesheets;
    std::vector<SubtreeStyleAssetRequest> m_RequestedSubtreeStyleAssetPaths;
    std::vector<GUID> m_RequestedSubtreeStyleAssetGuids;

    // Programmatic style overrides (allocated only when needed to keep UIElement lean).
    LiveTextureRef m_BackgroundImageTextureRef{};
    // Inline style overrides (parsed from "style" attribute).
    std::string m_InlineStyleText;

    // Typed style overrides (sparse property bag).
    StyleOverrides m_Overrides;
    StyleOverrides m_InlineOverrides;
    ResolvedStyle m_ResolvedStyle;
    OverlayLayer m_OverlayLayer = OverlayLayer::None;

    // Overlay-subtree bit. See HasOverlayInSubtree() for semantics.
    bool m_HasOverlayInSubtree = false;

    // Recompute m_HasOverlayInSubtree from m_OverlayLayer + child bits
    // and walk up the parent chain if the value changed. O(depth) per
    // mutation; called from SetOverlayLayer + AddChild / InsertChild /
    // RemoveChild / TakeChild whenever the state could shift.
    void RefreshOverlaySubtreeBitAndPropagate();

    // Event handlers by id (extensible event system).
    //
    // ownerCodeAddr identifies the hot-swappable module image whose code this
    // entry's callable is; 0 means engine-owned (Engine.dll / Editor.exe), which
    // is every handler in every host that never loads a user module. It is the
    // address of the callable's own type_info, which the erasure emits into the
    // image that instantiated it — so it is both the identity and the range key
    // the unload-time revocation tests. See UI/ModuleOwnedHandlers.h.
    struct HandlerEntry {
        std::uint64_t key;
        EventHandler handler;
        bool once;
        bool active;
        std::uint64_t ownerCodeAddr;
    };
    // BOTH CONTAINERS ARE NODE-BASED ON PURPOSE, for two different reasons, and
    // both are load-bearing.
    //
    // OUTER: DispatchToHandlers holds `auto& vec = it->second` across its whole handler
    // loop, and a handler may dispatch a DIFFERENT event id on this same element
    // (Button's semantic click), inserting a key and rehashing. std::unordered_map
    // rehashing invalidates iterators but NOT references to mapped values, so
    // `vec` survives. A flat/open-addressed map moves its values and would dangle
    // it.
    //
    // INNER: an entry's callable can run engine or module code that registers ANOTHER
    // handler on this same element and event — from inside the callable while it is
    // executing, or from its captures' destructors while it is being released or
    // compacted away. std::deque insertion at either end does not move the elements
    // already in it, so that registration cannot pull the storage out from under a
    // callable that is mid-call or mid-destruction. std::vector reallocates, which
    // frees exactly that storage; it made all three of those moments a
    // use-after-free (see the release rule on UnregisterEventHandler).
    //
    // ORDER IS AN INVARIANT OF THE INNER DEQUE, not an accident of how it is filled:
    // entries are ASCENDING BY KEY, because the only insert is a push_back carrying
    // ++m_NextHandlerKey and the only erase is DrainInactiveHandlers' order-preserving
    // remove_if. DispatchToHandlers' walk is a binary search over that order, so anything
    // that inserts out of order (a push_front, an insert in the middle, a reused key)
    // silently drops subscribers from dispatch rather than failing loudly.
    //
    // The cost of the node-based inner container is paid ONLY on storage, never on
    // dispatch: the walk visits entries in place, so it allocates nothing at any
    // subscriber count — and the presence bits mean the no-subscriber case never reaches
    // this container at all.
    std::unordered_map<EventId, std::deque<HandlerEntry>> m_EventHandlers;

    // The manipulators attached here — WEAK, deliberately, and lazily allocated so an element
    // that never sees one pays a single null pointer rather than an empty vector.
    //
    // Weak because ownership must stay entirely inside the handler table above: that is what
    // makes unload-time revocation total (see AddManipulator). Entries therefore expire on
    // their own when revocation or a Remove drops the last subscription, and an expired entry
    // is indistinguishable from an absent one — so there is no cleanup hook to forget to call
    // and nothing to keep in step with element destruction.
    //
    // Present only to make manipulators DISCOVERABLE: without it there is no Remove and no
    // way to notice a second attach of a type that could never fire.
    std::unique_ptr<std::vector<std::weak_ptr<Manipulator>>> m_Manipulators;

    // WeakRef control block: a non-owning shared_ptr to this element (no-op deleter) whose
    // only job is to expire every outstanding WeakRef. Null until the first MakeWeakRef;
    // released first thing in ~UIElement.
    std::shared_ptr<UIElement> m_WeakRefBlock;
    std::weak_ptr<UIElement> WeakRefBlock();

    // How many stamped callables this element holds: entries above carrying a
    // non-zero ownerCodeAddr, plus any stamped member slots (below). The
    // process-wide index holds this element exactly while this is non-zero, so
    // revocation never walks the element tree — and the counter keeps every
    // other path (unregister, compaction, clear, destruction) O(1) rather than
    // rescanning.
    std::uint32_t m_OwnedHandlerCount = 0;

    // One bit per named event id (UIEvents.h: kNamedEventIds), plus one escape bit
    // shared by every id outside that table: "this element holds at least one
    // handler for it". Read by the dispatch entry points to skip the map probe.
    // Conservative by construction — see the maintenance rule in UIEvents.h.
    std::uint32_t m_HandlerBits = 0;

    static std::uint64_t ResolveHandlerOwner(const EventHandler& h) {
        return h ? UI::AttributeCallableOwner(h.target_type()) : 0;
    }
    void NoteOwnerStampAdded() {
        if (++m_OwnedHandlerCount == 1) UI::NoteElementOwnsModuleHandler(this);
    }
    void ClearOwnerStamp(HandlerEntry& he) {
        if (!he.ownerCodeAddr) return;
        he.ownerCodeAddr = 0;
        if (--m_OwnedHandlerCount == 0) UI::ForgetElementOwnsModuleHandler(this);
    }

    // ---- Member callback slots -------------------------------------------
    //
    // A control's m_On* member (Button's two click slots, TreeView's fifteen)
    // holds a callable exactly like a handler-table entry does, and a native
    // module's callable stored in one outlives that module's image just the
    // same. Controls declare these as UI::ModuleOwnedCallback<Sig>, which does
    // the stamping; see ModuleOwnedCallback.h. Those
    // slots are therefore stamped and revoked through the same protocol; they
    // differ only in WHERE the callable sits, so the control supplies the four
    // hooks below and everything else — the index, the count, the quiesce
    // ledger, the executing-frame rule — is shared.
    //
    // Slots are addressed by a bit in a per-control mask, so a control declares
    // its own slot bits and nothing here needs to know how many there are.
    void NoteMemberSlotStamped(std::uint64_t owner) {
        if (owner) NoteOwnerStampAdded();
    }
    void ClearMemberSlotStamp(std::uint64_t& owner) {
        if (!owner) return;
        owner = 0;
        if (--m_OwnedHandlerCount == 0) UI::ForgetElementOwnsModuleHandler(this);
    }

    // Executing-frame keys for member slots. They share ExecutingHandlerScope
    // and IsHandlerExecuting with the handler table so revocation honours ONE
    // rule — never release a callable under its own frame — rather than a
    // second, parallel guard that could drift from it. The key spaces cannot
    // collide: table keys come from ++m_NextHandlerKey and so start at 1 and
    // count up, never setting the top bit.
    static constexpr std::uint64_t kMemberSlotKeyBit = 1ull << 63;
    static constexpr std::uint64_t MemberSlotKey(std::uint32_t slotBit) {
        return kMemberSlotKeyBit | slotBit;
    }

    virtual std::size_t CountMemberSlotsOwnedByImage(std::uint64_t, std::uint64_t) const {
        return 0;
    }
    virtual void CollectMemberSlotsOwnedByImage(std::uint64_t, std::uint64_t, std::uint32_t&) {}
    virtual std::size_t ReleaseCollectedMemberSlots(std::uint32_t) { return 0; }
    virtual std::size_t DropMemberSlotStampsOutsideMappedImages() { return 0; }

    // Module-unload protocol; defined in ModuleOwnedHandlers.cpp, which owns it.
    //
    // Split in two because RELEASING a callable runs the module's own code (the
    // closure's captured destructors), and that code can mutate the very tables a
    // walk is holding references into. Collect decides and runs no user code;
    // Release does the freeing, re-finding each entry so it never holds a
    // reference across one.
    struct RevocationBatch
    {
        UIElement* Element = nullptr;
        struct Entry { EventId Id; std::uint64_t Key; };
        std::vector<Entry> Entries;
        // Member slots decided in the same collect pass, as a bit per slot; the
        // control that declared the bits is the one that releases them.
        std::uint32_t MemberSlotMask = 0;
    };
    std::size_t CountHandlersOwnedByImage(std::uint64_t base, std::uint64_t size) const;
    void CollectHandlersOwnedByImage(std::uint64_t base, std::uint64_t size,
                                     std::vector<RevocationBatch>& batches);
    std::size_t ReleaseCollectedHandlers(const RevocationBatch& batch);
    std::size_t DropStampsOutsideMappedImages();
    friend std::size_t UI::CountHandlersOwnedByImage(std::uint64_t, std::uint64_t);
    friend std::size_t UI::RevokeHandlersOwnedByImage(std::uint64_t, std::uint64_t);
    friend void UI::CloseImageAttribution(std::uint64_t, std::uint64_t);

    // Handlers of THIS element currently executing, innermost first (nested and
    // re-entrant dispatch both push). Read by UnregisterEventHandler, to avoid
    // destroying a callable a live frame is running out of, and by
    // DrainInactiveHandlers, to avoid erasing an entry a live frame is standing on.
    // Intrusive and stack-allocated: dispatch is a hot path and must not allocate.
    struct ExecutingHandler { std::uint64_t Key; ExecutingHandler* Prev; };
    ExecutingHandler* m_ExecutingHandlers = nullptr;

    // Entries deactivated but not yet erased, across EVERY id of this element.
    // Deactivation is cheap and erasure is deferred, so this is what lets the
    // unwinding frame answer "is there anything to drain" with one comparison
    // instead of walking every id on a path that is almost always empty-handed.
    std::uint32_t m_InactiveHandlerCount = 0;

    // True across DrainInactiveHandlers' mutation window. Emptying an entry can run
    // user code — std::function::swap against an empty sink takes a three-way move
    // and destroys an INLINE-held callable's captures in place — and that code can
    // dispatch on this element. Such a dispatch has no handler frame on the stack,
    // so the outermost-frame test alone would let it re-enter the drain and compact
    // the deque this one is midway through, double-freeing the entry both are
    // dropping.
    bool m_SweepingHandlers = false;

    // Deactivating is the ONLY way an entry becomes erasable, so it is the only
    // place the pending count moves. Idempotent: revocation deactivates entries an
    // unregister may already have.
    void DeactivateHandler(HandlerEntry& he) {
        if (!he.active) return;
        he.active = false;
        ++m_InactiveHandlerCount;
    }

    // Erase every deactivated entry, across every id.
    //
    // WHY IT IS DEFERRED AT ALL: erasing destroys deque elements and remove_if MOVES
    // the ones it keeps, down over the ones it drops. A dispatch frame is mid-call
    // through the LIVE entry, standing on one of those elements, so sweeping under it
    // corrupts that frame two ways — dropping ITS entry frees the closure it is
    // running out of, and dropping any EARLIER entry relocates its closure, which for
    // an inline-held callable moves the captures away mid-call. That is the other
    // half of why UnregisterEventHandler keeps an executing callable alive: it only
    // works if the erase runs after the last frame standing on it has returned.
    //
    // WHY IT DRAINS EVERY ID, not the one just dispatched: an id whose every dispatch
    // is nested never reaches its own dispatch tail, and a member-slot callback that
    // dispatches on its own element is an ordinary way to get there. Draining per-id
    // left one dead entry per register/unregister cycle, unbounded: every one of them
    // is an entry the dispatch walk still has to step over and skip. This also settles
    // entries on an id that is simply never dispatched again.
    void DrainInactiveHandlers() {
        if (m_InactiveHandlerCount == 0 || m_ExecutingHandlers || m_SweepingHandlers)
            return;
        // The callables go to a sink that outlives the container mutation, the same
        // rule UnregisterEventHandler and ReleaseCollectedHandlers follow: destroying
        // one runs its captures' destructors, which is module code that can register
        // on this element, dispatch on it, or destroy it.
        //
        // NODE-BASED, for the reason the handler table itself is. Emptying an entry
        // can run that module code already — std::function::swap against an empty sink
        // takes a three-way move and destroys an INLINE-held callable's captures in
        // place — and that code can deactivate further entries, so this collection can
        // grow while it is being filled. A vector would reallocate and move the
        // callables it already holds, and moving an inline-held one destroys the source
        // in place: precisely what the sink exists to defer. A deque appends without
        // touching what is already in it, so the count does not have to be known up
        // front to be safe.
        std::deque<EventHandler> doomed;
        // Scoped rather than assigned back at the end: the collect pass runs module
        // destructors, and one that throws would otherwise leave the flag raised
        // forever — an element that silently never drains again, which is a leak that
        // looks like nothing at all.
        struct SweepGuard {
            bool& Flag;
            explicit SweepGuard(bool& f) : Flag(f) { Flag = true; }
            ~SweepGuard() { Flag = false; }
            SweepGuard(const SweepGuard&) = delete;
            SweepGuard& operator=(const SweepGuard&) = delete;
        } sweepGuard(m_SweepingHandlers);
        // Ids snapshotted first: emptying an entry can run module code that registers
        // on this element, and that insertion can rehash the map. A map iterator does
        // not survive it; a re-find per id does.
        std::vector<EventId> ids;
        ids.reserve(m_EventHandlers.size());
        for (const auto& kv : m_EventHandlers) ids.push_back(kv.first);
        for (EventId id : ids) {
            auto it = m_EventHandlers.find(id);
            if (it == m_EventHandlers.end()) continue;
            auto& vec = it->second;
            // BY INDEX, not by iterator. Emptying an entry can run module code that
            // registers on this element, and while deque::push_back does not move the
            // elements already stored it DOES invalidate iterators — the same
            // distinction the dispatch loop above turns on. Appending never shifts an
            // existing index, so re-reading vec[i] and vec.size() survives one.
            for (std::size_t i = 0; i < vec.size(); ++i) {
                HandlerEntry& he = vec[i];
                if (he.active) continue;
                // Last point that can keep the module-owned-handler index honest: this
                // is the only removal path for a self-unregister and for `once`
                // handlers, neither of which passes through UnregisterEventHandler's
                // release.
                ClearOwnerStamp(he);
                doomed.emplace_back().swap(he.handler);
            }
            // EMPTY is the condition, not merely inactive. An entry deactivated by
            // module code that ran during the collect pass above still holds its
            // callable, and erasing it here would destroy that callable in place,
            // mid-mutation — the hazard this whole function is arranged to avoid. Such
            // an entry stays inert and is emptied and erased by the next drain, which
            // the pending count still asks for.
            //
            // THAT EMPTIES THE DROPPED ENTRIES, NOT THE WINDOW. remove_if also MOVES
            // every entry it KEEPS, down over the ones it drops, and move-assigning an
            // inline-held callable destroys the moved-from source in place — so a
            // surviving handler's captures can run a destructor inside this call, and
            // that destructor is module code that can dispatch on this element. It is
            // m_SweepingHandlers, not the emptiness of the dropped entries, that keeps
            // such a dispatch from re-entering the drain and compacting the deque this
            // erase is midway through. The flag is load-bearing here; do not conclude
            // from "the dropped entries hold nothing" that it can go.
            vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const HandlerEntry& he){
                if (he.active || he.handler) return false;
                --m_InactiveHandlerCount;
                return true;
            }), vec.end());
            // The one place a presence bit may be cleared: this id's deque is now
            // provably empty, so no dispatch of it can reach a handler. Clearing
            // anywhere less certain would silently drop events, which is why the
            // escape bit — shared by every unlisted id — is never cleared at all.
            if (vec.empty()) {
                const std::uint32_t bit = EventHandlerBit(id);
                if (bit != kEventEscapeHandlerBit)
                    m_HandlerBits &= ~bit;
            }
        }
        // Order matters and comes from the declarations above: sweepGuard is declared
        // AFTER `doomed`, so it dies FIRST — the flag drops, and only then is the sink
        // destroyed. A destructor that dispatches on this element therefore gets a
        // consistent table AND an unblocked drain, which is the whole point of
        // deferring it to here.
    }

    struct ExecutingHandlerScope {
        ExecutingHandlerScope(UIElement& owner, std::uint64_t key)
            : m_Owner(owner), m_Frame{key, owner.m_ExecutingHandlers} {
            m_Owner.m_ExecutingHandlers = &m_Frame;
        }
        // Unwinding the LAST frame on this element is the moment deferred entries
        // become erasable, and it is the only moment that covers a dispatch nested
        // inside a member-slot callback: that callback publishes a frame but is not
        // itself a dispatch, so no DispatchToHandlers tail runs after it. Without
        // this, an id whose every dispatch is nested never reaches a drain and
        // accumulates one dead entry per register/unregister cycle.
        ~ExecutingHandlerScope() {
            m_Owner.m_ExecutingHandlers = m_Frame.Prev;
            if (!m_Frame.Prev) m_Owner.DrainInactiveHandlers();
        }
        ExecutingHandlerScope(const ExecutingHandlerScope&) = delete;
        ExecutingHandlerScope& operator=(const ExecutingHandlerScope&) = delete;
        UIElement& m_Owner;
        ExecutingHandler m_Frame;
    };

    // LOAD-BEARING FOR MODULE UNLOAD, not just for unregister.
    //
    // A revocation can run mid-dispatch — clicking Stop runs play-exit and the
    // deferred module swap inside a UI dispatch. It releases the callables of every
    // entry that is not currently executing, and this guard is what excludes the one
    // that is: releasing a callable under its own frame frees the captures it is
    // running out of. That entry keeps its callable AND its owner stamp, so the
    // quiesce ledger counts it and refuses the unmap for as long as the frame is
    // inside it.
    //
    // NOTHING ELSE OUTLIVES THE REVOCATION. Dispatch holds no copy of any sibling's
    // callable — it walks the table in place — so a revoked sibling is destroyed
    // inside the revocation, against a still-mapped image, and the walk simply finds
    // it inactive when it gets there. The pin therefore has to cover the executing
    // frame only, which is a per-element question this guard answers exactly.
    bool IsHandlerExecuting(std::uint64_t key) const {
        for (const ExecutingHandler* f = m_ExecutingHandlers; f; f = f->Prev)
            if (f->Key == key) return true;
        return false;
    }

    // Monotonic per-element key source for EventHandlerToken. Keep this a MEMBER, never a
    // process/module static: RegisterEventHandler is header-inline and runs in every consuming
    // image (Editor.exe, Engine.dll, GameEngine.Native.dll, user-script DLLs), so a
    // function-local static would be duplicated per image (the "double-link disease" the
    // dispatch-depth counter above hit — see SetInEventDispatch) and break token uniqueness
    // across the DLL boundary. Living on the shared UIElement object, `++m_NextHandlerKey`
    // mutates ONE counter regardless of which binary runs the inline code, and keys are unique
    // within the element's lifetime — which is all UnregisterEventHandler needs, since it only
    // matches within one resolved element.
    //
    // Keys are NOT unique across elements: every element's first handler is key 1. A caller that
    // holds a token must therefore re-resolve the SAME ELEMENT INSTANCE to unregister — resolving
    // by a string id is not enough, because a rebuild can put a different element behind that id
    // and the token would then match a stranger's handler. The scripting ABI addresses elements by
    // instance id for exactly this reason (GE_UIElement_RegisterEvent/UnregisterEvent), and
    // instance ids are never reused. Keys are not unique across EVENT IDS either, which is why a
    // token has to be presented with the event it was minted for.
    std::uint64_t m_NextHandlerKey = 0;



    // Focusability/enable flags
    bool m_Enabled = true;
    bool m_Focusable = false;
    bool m_SnapRectToDevicePixels = false;
    int  m_TabIndex = 0;
	    UIElement* m_FocusProxy = nullptr;

  public:
    // Event-driven UI rewrite, Stage 1 (additive, currently unused at
    // runtime): per-element render handle into the manager's persistent
    // SSBO mirrors. Stage 2+ wires actual allocations through these.
    //
    // m_PrimitiveRangeStart : start in m_PersistentPrimitives (kInvalidSlot
    //                         when no primitives are owned — pure-layout
    //                         containers, display:none, visibility:hidden).
    // m_PrimitiveRangeCount : logical count of slots in active use.
    // m_PrimitiveRangeCap   : allocated capacity (>= count). Lets small
    //                         text edits absorb without relocation.
    // m_ClipSlotIdx         : index in m_PersistentClipRects (0xFFFF when
    //                         this element does not own a clip slot).
    // m_AmbientClipIdx      : clip-stack top at this element's last
    //                         full-DFS self-emission (0xFFFF = unclipped).
    //                         Overlay and Mount subtrees don't follow tree
    //                         ancestry, so the drain reads this recorded
    //                         emit-time ambient instead of inferring it
    //                         from baked primitives or a parent walk.
    // m_DrawOrderIdx        : start in m_DrawOrder for this element's
    //                         slice (size mirrors m_PrimitiveRangeCount).
    //
    // Sentinels match SlotAllocator::kInvalidSlot and UI::kNoClip; not
    // typed as those constants here to keep the headers light.
    uint32_t m_PrimitiveRangeStart = 0xFFFFFFFFu;
    uint16_t m_PrimitiveRangeCount = 0;
    uint16_t m_PrimitiveRangeCap   = 0;
    uint16_t m_ClipSlotIdx         = 0xFFFFu;
    uint16_t m_AmbientClipIdx      = 0xFFFFu;
    uint32_t m_DrawOrderIdx        = 0xFFFFFFFFu;

    // Ancestor-chain opacity product at the last primitive emit, EXCLUDING
    // this element's own opacity. The drain seeds emission from this instead
    // of the baked primitive opacity (which already includes self — reusing
    // it would multiply self in twice per drain). Stored rather than derived
    // because the emit-time chain is not the parent chain: deferred overlays
    // reset the accumulator and Mount hops route through the host.
    float m_PrimitiveAncestorOpacity = 1.0f;

    // Stage 2: re-entrancy detector for the per-element finalize path in
    // PrimitiveGen. Each PrimitiveGen frame bumps UIManager::m_PrimitiveGenFrame
    // and writes that value here when the element is visited. If the visit
    // enters with this already equal to the current frame counter, an event
    // handler or control extension has caused a synchronous re-entry — that
    // would silently overwrite the element's freshly-written slot range with
    // a second pass of (possibly different) primitive data. Asserts in Debug.
    //
    // Wraparound: the counter skips 0 on wrap (UIManager bumps past it), so a
    // freshly-attached element with default-initialized 0 here can't collide
    // with an in-flight current frame value.
    uint32_t m_LastPrimitiveGenFrame = 0;

    // PrimitiveGen frame counter of the last FULL DFS that actually processed
    // this element (stamped after the viewport/clip cull, full mode only).
    // The E1 drain compares this against the manager's last-full-DFS counter:
    // a mismatch means the element was clip-culled out of (or created after)
    // the last full walk, so its slot range / DrawOrder bookkeeping is stale
    // and must not be used for in-place slice surgery.
    uint32_t m_LastFullDfsFrame = 0;

    // Sticky marker: this element's OnGeneratePrimitives override has emitted
    // at least one primitive. Custom control emission may touch shared
    // text/measure state, so the parallel drain re-emits flagged elements on
    // the UI thread. The flag is set AFTER a call returns, so it does NOT
    // cover zero-emit calls or the first-ever emitting call — those rely on
    // the override's OffThread escalation preamble (see the thread-contract
    // note on OnGeneratePrimitives) plus the manager's assert backstop.
    bool m_CustomEmitObserved = false;

    // Membership bits for UIManager-side containers holding raw pointers to
    // this element. ~UIElement gates RemoveFromDirtyQueues on
    // m_InQueueFlags != 0 so destruction pays the cleanup only when the
    // element is actually referenced.
    enum InQueueFlag : uint8_t
    {
        // Enqueued in UIManager::m_PrimitiveDataDirty (render-side drain).
        // PushPrimitiveDataDirty consults this to avoid duplicate entries;
        // the drain clears it on dequeue.
        InPrimitiveDataDirty = 1u << 0,
        // Member of UIManager::m_FocusWithinChain (the focused element and all
        // its ancestors) — the :focus-within predicate for this element.
        InFocusChain         = 1u << 1,
        // The focused leaf itself (:focus). Always a subset of InFocusChain, so
        // it never changes the ~UIElement `m_InQueueFlags != 0` cleanup gate.
        InFocusLeaf          = 1u << 2,
        // The focused leaf when focus arrived via the keyboard (:focus-visible).
        // Subset of InFocusLeaf.
        InFocusVisibleLeaf   = 1u << 3,
    };
    uint8_t m_InQueueFlags = 0;

    // Stage 5 Block B: per-element predicate for "do any matched style
    // rules reference this pseudo-state?" Computed as a side output of
    // ComputeStyleInto each time the cascade resolves this element.
    // Mark-dirty sites consult this bitmask to skip pushing elements
    // that wouldn't change style for the pseudo-state being toggled.
    //
    // Bit ordering matches MatchedPseudoFlag below; `Custom` is a single
    // catch-all bit (custom states are open-ended, so we can't enumerate).
    enum MatchedPseudoFlag : uint16_t
    {
        HoverRules        = 1u << 0,
        ActiveRules       = 1u << 1,
        FocusRules        = 1u << 2,
        FocusVisibleRules = 1u << 3,
        FocusWithinRules  = 1u << 4,
        DisabledRules     = 1u << 5,
        EnabledRules      = 1u << 6,
        CheckedRules      = 1u << 7,
        CustomStateRules  = 1u << 8,
    };
    uint16_t m_MatchedPseudoStates = 0;

    // Cascade-matcher memoization (Phase 1, additive — readers wired in
    // Phases 3+).
    //
    // Per-element cache of structurally-matching rules. Populated by
    // BuildRuleCache (forthcoming Phase 3) by walking the candidate rules
    // in the element's effective stylesheet set and applying a STRUCTURAL
    // match — same as MatchesSelectorChain but treating dynamic pseudo
    // classes (:hover/:focus/:active/:disabled/:enabled/:checked/
    // :focus-visible/:focus-within/custom) as `true`. Apply-time uses the
    // cache + per-rule pseudo filtering, skipping the per-rule selector
    // chain walk entirely on cache hits.
    //
    // Cache validity is per-element. Mutations that affect structural
    // matching (class/id change, parent change, stylesheet attach,
    // sibling-combinator-affecting sibling change) call
    // InvalidateRuleCacheSubtree to mark self + descendants as stale.
    struct CachedMatchedRule
    {
        const CSSRule* rule = nullptr;
        uint16_t       sheetIndex = 0;        // for stable sort key in cascade
        StyleOrigin    origin = StyleOrigin::Author; // the rule's sheet's cascade origin
        bool           hasDynamicPseudo = false;  // skip per-rule pseudo walk if false
    };

    // `mutable` so CSSParser::ComputeStyleInto (taking `const UIElement&`)
    // can lazily populate the cache during cascade. Logically const —
    // this is a memoization cache for a pure function of element state +
    // stylesheet content.
    mutable std::vector<CachedMatchedRule> m_CachedMatchedRules;
    mutable bool m_RuleCacheValid = false;
    // Manager rule-cache epoch at build time; a mismatch means stylesheet
    // content changed while this element was outside the invalidation
    // walk's reach (detached subtree) — the cached CSSRule pointers may
    // dangle and must not be trusted.
    mutable uint32_t m_RuleCacheEpoch = 0;

    bool HasPseudoStateRules(MatchedPseudoFlag flag) const
    {
        return (m_MatchedPseudoStates & static_cast<uint16_t>(flag)) != 0;
    }

    const std::vector<CachedMatchedRule>& GetCachedMatchedRules() const
    {
        return m_CachedMatchedRules;
    }
    bool IsRuleCacheValid() const
    {
        return m_RuleCacheValid && m_OwnerManager &&
               m_RuleCacheEpoch == UIManagerRuleCacheEpoch(m_OwnerManager);
    }

    // Marks this element + every descendant's rule cache stale. Cheap when
    // already invalid (early-out on the recursion). Called by mutation sites
    // in Phase 2.
    //
    // Cascade-memoization Phase 7: also resets m_MatchedPseudoStates to
    // all-bits-set. The Phase 5 narrowing predicate `(bits & flag) != 0`
    // then defaults to "true" (mark dirty) until the next cascade
    // rebuilds with real bits — conservative against the case where a
    // stylesheet hot-reload added a rule referencing a pseudo we hadn't
    // tracked. If we cleared bits to 0, narrowing would default to "skip"
    // and miss legitimate dirty marks.
    void InvalidateRuleCacheSubtree()
    {
        if (!m_RuleCacheValid)
        {
            // Children may still be valid (we lazy-invalidate). Recurse so
            // subtree-mutation semantics are deterministic regardless of
            // entry state.
            for (auto& ch : m_Children)
                if (ch && ch->m_RuleCacheValid)
                    ch->InvalidateRuleCacheSubtree();
            // Mount portal targets are not part of m_Children but participate
            // in cascade. Their cached rule pointers reference the same
            // stylesheets as the visible tree, so they must be invalidated
            // alongside m_Children — otherwise a stylesheet-set change leaves
            // dangling rule pointers in mounted subtrees and the next cascade
            // dereferences them.
            if (UIElement* tgt = GetMountTarget())
                if (tgt->m_RuleCacheValid)
                    tgt->InvalidateRuleCacheSubtree();
            return;
        }
        m_RuleCacheValid = false;
        m_CachedMatchedRules.clear();  // free the vector storage proactively
        m_MatchedPseudoStates = 0xFFFFu;  // conservative: mark-dirty default until rebuilt
        for (auto& ch : m_Children)
            if (ch)
                ch->InvalidateRuleCacheSubtree();
        if (UIElement* tgt = GetMountTarget())
            tgt->InvalidateRuleCacheSubtree();
    }

    // Cascade-memoization Phase 6: invalidate the rule cache subtree of
    // every sibling AFTER this element under the same parent. Used by
    // class/id mutation sites when the active stylesheet set contains
    // `+` or `~` selectors — a class change on element X can flip the
    // match for X.next (`X + Y`) or any later sibling (`X ~ Y`). For
    // stylesheets without sibling combinators this walk is wasted; the
    // call site checks `UIManagerUsesSiblingCombinators` first.
    void InvalidateRuleCacheFollowingSiblings()
    {
        UIElement* p = m_Parent;
        if (!p)
            return;
        bool foundSelf = false;
        for (auto& sib : p->m_Children)
        {
            if (!sib)
                continue;
            if (foundSelf)
                sib->InvalidateRuleCacheSubtree();
            else if (sib.get() == this)
                foundSelf = true;
        }
    }

    // Fast-access shadow of m_YogaState->node, kept in sync at every site
    // that creates or replaces the retained Yoga node. Lets hot-path readers
    // (HitTest layout writeback, Yoga child re-attach) avoid a unique_ptr
    // deref + RetainedYogaNode field load.
    //
    // Untyped (void*) to keep Yoga's <yoga/Yoga.h> out of UIElement.h's
    // dependency surface. Layout-side code casts to YGNodeRef on use.
    void* m_YogaNode = nullptr;

};

	} // namespace GameEngine
