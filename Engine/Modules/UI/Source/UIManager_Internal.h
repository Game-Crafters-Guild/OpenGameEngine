#pragma once

#include "UI/UIManager.h"
#include "UI/UIElement.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleOverrides.h"
#include "UI/UIStyle.h"
#include "Rendering/Text/TextLayout.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
#include <yoga/Yoga.h>
#endif

namespace GameEngine
{
class ScrollView;
}

namespace GameEngine::Rendering::Text
{
class FontAtlas;
}

// TextMeasureCtx now lives on RetainedYogaNode — see UIManager.h. The old
// thread_local pool was replaced in Slice 2 step 3 so fast-path element
// appends don't need to re-set YGNodeSetContext each frame.

// P4 sibling style sharing: per-build-pass donor snapshots. A hash hit must
// still prove full key equality against the verification fields (hash
// collisions degrade to a miss, never to a wrong style). The snapshot is
// taken immediately after the donor's cascade and BEFORE per-element
// overrides are applied on top of its ResolvedStyle — later mutation of the
// donor element cannot leak into sharees.
struct GameEngine::UIManager::CascadeShareCache
{
    struct Entry
    {
        // Verification key. Sheet identity is the interned PersistentSheetSetId
        // (stable for the pass and identical for identical sheet lists), NOT
        // the ctx.flatSheets span pointer — that thread_local grows mid-pass
        // and a reallocated address could theoretically alias a freed one.
        UIElement* Parent = nullptr;
        const std::type_info* Type = nullptr;
        uint32_t SheetSetId = 0;
        uint16_t StateBits = 0;
        std::vector<GameEngine::StringId> ClassIds;
        // Snapshot.
        GameEngine::ResolvedStyle Style;
        uint16_t MatchedPseudoStates = 0;
        std::vector<UIElement::CachedMatchedRule> CachedRules;
        bool RuleCacheValid = false;
        uint32_t RuleCacheEpoch = 0;
    };
    std::unordered_map<uint64_t, Entry> Entries;
    // Hover-ancestor set for the rightmost `:hover` share-key bit, rebuilt at
    // each root pass on the ROOT cache. It is read-only for the whole pass, so
    // MT-4.2 parallel chunks share it by pointer (CascadeShareContext::Hover)
    // rather than each copying it (issue #215). It lives here — behind
    // UIManager's unique_ptr<CascadeShareCache> — precisely so the fix does NOT
    // change UIManager's member layout (chunk-private caches carry their own
    // empty, unused HoverChain; no copy is ever made into it).
    std::unordered_set<const GameEngine::UIElement*> HoverChain;
};

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
YGSize MeasureTextFn(YGNodeConstRef node,
                     float width, YGMeasureMode widthMode,
                     float height, YGMeasureMode heightMode);

// Paired with MeasureTextFn on the same node and reading the same context, so
// the two are attached and cleared together.
float BaselineTextFn(YGNodeConstRef node, float width, float height);
#endif

// Defined in StyleApplier.cpp (function-pointer table indexed by StylePropertyId).
#include "StyleApplier.h"

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
void DetachYogaFromParent(YGNodeRef node);
void RemoveAllYogaChildren(YGNodeRef node);

// A WeightedPane's flex-grow is a runtime value (its splitter weight), not a
// CSS property, so YogaAdapter::ApplyStyle can't set it. Every site that pushes
// resolved style into a Yoga node must re-assert the weight afterwards or the
// pane collapses to the CSS default. Used by the incremental restyle and the
// transition re-solve; the BuildYogaRecursive push already has the pane in hand
// and applies the same clamp inline.
void ApplyWeightedPaneFlexGrowOverride(GameEngine::UIElement* el, YGNodeRef node);

// A child of a block-flow container (display: block / inline, and the omitted
// default) does not shrink to fit: CSS block layout has no shrink step, so the
// child keeps its size and overflows. That is a PARENT fact, which
// YogaAdapter::ApplyStyle — one ResolvedStyle onto one node — cannot decide, so
// every site that pushes resolved style into a Yoga node re-asserts it here,
// exactly as it re-asserts the WeightedPane weight above.
//
// An authored flex-shrink still wins. Chrome ignores the whole flex triple on a
// block-flow child (measured: `flex-shrink: 1` and `flex: 1` both leave an 80px
// child at 80px in a 100px block box), because there such a child is not a flex
// item at all. This engine has no block formatting context and honours
// flex-grow and flex-basis inside Block, so dropping only the declared shrink
// would leave one authored `flex-grow: 1; flex-shrink: 1` pair half-obeyed.
// Undeclared is where the divergence actually bites, and that is what moves.
//
// `parentStyle` is null for a root node, which has no container to inherit a
// formatting context from and therefore keeps the flex-container answer.
void ApplyBlockFlowShrinkDefault(YGNodeRef node,
                                 const GameEngine::ResolvedStyle& style,
                                 const GameEngine::ResolvedStyle* parentStyle);
#endif

static constexpr unsigned kUiFontAtlasPx = 20;

// ---------------------------------------------------------------------------
// Shared types used across UIManager_Update.cpp, UIManager_Layout.cpp,
// UIManager_Debug.cpp, and UIManager_Input.cpp.
// ---------------------------------------------------------------------------

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA

// Range into the flat stylesheet/index arrays.
struct SheetSetRef
{
    uint32_t offset = 0;
    uint32_t count = 0;
};

// Return type of BuildYogaRecursive / buildYoga.
struct BuiltNode
{
    YGNodeRef node = nullptr;
    int order = 0;
    // True if this element just got a fresh Yoga node (or was forced to rebuild),
    // so its parent must re-verify the child list (call InsertChildrenSortedByOrder).
    // Lets a parent skip the sort/insert call entirely when its whole child set is
    // structurally stable.
    bool structureDirty = false;
    // True if this element's computed CSS `order` differs from the previous
    // build (RetainedYogaNode::PrevLayout.Order). Ordering is applied by the
    // parent's InsertChildrenSortedByOrder, not by Yoga, so the parent must
    // re-attach even when the child carries no StyleDirty — an
    // Overrides().Set(Style::Order) raises only LayoutDirty.
    bool orderChanged = false;
};

// MT-4.0 cascade seam: per-element data handed collect -> compute -> apply
// within a single BuildYogaRecursive visit. Today the three stages run
// sequentially and immediately per element; making the hand-off explicit is
// the whole point of this slice. MT-4.1 hoists the collect outputs onto the
// retained node; MT-4.2 dispatches ComputeElementCascade to a worker thread
// with just these inputs, then replays the apply stage on the UI thread in
// DFS order. The stage boundaries are documented on each field group.
struct YogaBuildItem
{
    // --- Collect stage outputs (UI thread) -----------------------------
    // Effective stylesheet set index into ctx's flat sheet tables. Spans are
    // re-derived at point of use (never stored) because ctx.flatSheets can
    // reallocate mid-pass as descendants push their own merged sets.
    int32_t LocalSheetSetIdx = -1;
    GameEngine::RetainedYogaNode* Retained = nullptr;
    YGNodeRef Node = nullptr;
    bool IsFreshNode = false;
    bool HadChildrenDirty = false;
    // Sheet-list identity hashes for the subtree-skip gate; committed to the
    // retained node at the slow-path exit so a clean next frame can early-out.
    uint64_t LocalSheetsHashNow = 0;
    uint64_t ParentSheetsHashNow = 0;

    // --- Compute stage outputs (element-local; no YG*, no manager mutation)
    // Decisions the apply stage replays. NeedCascade also drives the layout-
    // signature/measure gate; OverridesDirty forces override re-application.
    bool NeedCascade = false;
    bool OverridesDirty = false;

    // MT-4.1: deferred effects recorded by ComputeElementCascade, merged by
    // ApplyElementBuild on the UI thread (sequential now; deterministically
    // mergeable when MT-4.2 runs compute on a worker). Keeping these off the
    // shared UpdateContext/prof keeps the compute region mutation-free.
    //   DidShareCascade  — cascade was served from the sibling-share cache
    //                      (drives the CascadeShared counter). NeedCascade
    //                      already gates the CascadeCalls / CascadeCallsBuildYoga
    //                      pair, so no separate DidCascade flag is needed.
    //   CascadeComputeMs — wall time spent in ResolveCascadeForElement (only
    //                      accumulated when profiling is on).
    //   TransitionDecision — declared-transition registry action to apply.
    bool DidShareCascade = false;
    double CascadeComputeMs = 0.0;
    GameEngine::UIManager::TransitionRegistration TransitionDecision =
        GameEngine::UIManager::TransitionRegistration::None;
};

// Record for virtualization layout-override patching. ApplyLayoutOverrideRects
// reads the element's resolved style + UIElement parent directly, so the only
// stored datum is the element pointer.
struct LayoutOverridePatchRec
{
    GameEngine::UIElement* element = nullptr;
};

// RAII timer for optional per-section profiling inside Update().
struct ScopedSectionTimer
{
    bool enabled = false;
    double* outMs = nullptr;
    std::chrono::high_resolution_clock::time_point start{};
    ScopedSectionTimer(bool e, double* out) : enabled(e), outMs(out)
    {
        if (enabled)
            start = std::chrono::high_resolution_clock::now();
    }
    ~ScopedSectionTimer()
    {
        if (!enabled || !outMs)
            return;
        const auto end = std::chrono::high_resolution_clock::now();
        *outMs += std::chrono::duration<double, std::milli>(end - start).count();
    }
};

// Per-frame shared mutable state for methods extracted from Update().
// Constructed on Update()'s stack after the stylesheet setup block (~line 1650).
// Holds only references to thread_local or stack-local data -- no ownership.
struct UpdateContext
{
    // Stylesheet resolution (flat table for per-element sheet sets)
    std::vector<const GameEngine::Stylesheet*>& flatSheets;
    // Step 4: parallel-indexed handles. Mirrors flatSheets so the
    // interner's Intern() (which now takes handle spans) can be fed
    // from the full-rebuild's per-element resolved sheet set without
    // rebuilding handles from scratch. flatHandles[i].get() ==
    // flatSheets[i] is an invariant.
    std::vector<GameEngine::StylesheetHandle>& flatHandles;
    std::vector<const GameEngine::StylesheetRuleIndex*>& flatIndices;
    std::vector<SheetSetRef>& sheetSetRefs;
    std::vector<GameEngine::StylesheetHandle>& rootSheets;
    int32_t rootSheetSetIdx = -1;

    // Stylesheet analysis (populated during tree build)
    std::unordered_set<const GameEngine::Stylesheet*>& analysisSheets;
    uint64_t& analysisSheetHash;

    // Typed node lists (rebuilt after each tree build)
    std::vector<GameEngine::ScrollView*>& scrollViews;
    std::vector<LayoutOverridePatchRec>& layoutOverrideNodes;

    // Per-frame dirty counters (references to Update() locals)
    uint32_t& dirtyStyleSeen;
    uint32_t& dirtyLayoutSeen;
    uint32_t& dirtyVisualSeen;
    uint32_t& dirtyChildrenSeen;
    bool& anyChildrenDirty;

    // --- Layout signature tracking ------------------------------------------
    //
    // Per-element layout-signature hashing is expensive on large trees.
    // Update() decides per-frame whether to participate in the signature pass
    // based on two flags below. Getting the scope wrong here has outsized cost
    // — read the invariants before editing.
    //
    // `computeLayoutSignaturesThisFrame` (reference to Update() local)
    //   Enables the signature pass for this frame. Union of:
    //     - computeLayoutSignaturesAll (see below — whole-tree invalidation)
    //     - relayoutRequestedPre (see below — single requesting subtree)
    //     - forceLayoutSignaturesThisFrame (catch-all, e.g. first frame)
    //     - any LayoutDirty/ChildrenDirty seen in the pre-scan
    //   Retry paths in ConvergePostLayout force this on to correctly cascade
    //   styles/measure funcs through newly created Yoga nodes.
    bool& computeLayoutSignaturesThisFrame;

    // `computeLayoutSignaturesAll`
    //   Bypasses the per-element dirty check and forces EVERY element to
    //   recompute its layout signature. Reserved for real whole-tree
    //   invalidations only:
    //     - viewport resized          (every layout rect can change)
    //     - content scale changed     (physical/logical conversion shifts)
    //     - global stylesheet set     (every selector chain can rematch)
    //   Do NOT add RequestRelayout() callers or per-element dirty flags here —
    //   see the Fix A invariant below (commit a3355481). Scoped mutations go
    //   through MarkDirty / SubtreeDirty propagation, which the subtree-skip
    //   fast path honors. Turning on computeLayoutSignaturesAll from a scoped
    //   trigger kills the fast path globally for that frame: on a 3k-element
    //   tree this regressed entity_reselect_sweep peak from ~55ms to ~134ms.
    bool computeLayoutSignaturesAll = false;

    // `relayoutRequestedPre`
    //   RequestRelayout() was consumed this frame (TreeView/ListView
    //   virtualization, dockspace resize, etc.). Fed into
    //   computeLayoutSignaturesThisFrame so the requesting subtree's
    //   signatures are recomputed, and into needInitialLayoutSolve so Yoga
    //   solves rather than skipping.
    //
    //   INVARIANT (Fix A / commit a3355481): every RequestRelayout caller
    //   already pairs with MarkDirty(LayoutDirty|VisualDirty) on the
    //   requesting element. Don't merge this into computeLayoutSignaturesAll
    //   — SubtreeDirty propagation handles the subtree invalidation and the
    //   untouched siblings continue to hit the subtree-skip fast path.
    bool relayoutRequestedPre = false;

    bool& anyLayoutSignatureChanged;
    uint32_t& cssRecomputeWhileSignaturePassDisabled;

    // Layout state (references to Update() locals)
    bool& didSolveLayout;
    bool forceGlobalStyle = false;
    uint32_t viewportW = 0, viewportH = 0;
    // OS content scale (physical px per CSS logical px). Viewport dimensions
    // passed to Yoga are already divided by this; layout rects are multiplied
    // back to physical after CalculateLayout.
    float contentScale = 1.0f;

    // Text measure pool usage (lives in UIManager_Layout.cpp, exposed for spike logging)
    size_t& textMeasureCtxUsed;

    // Pseudo-state snapshots taken before event dispatch, used to detect
    // focus/active changes that need dirty marking after events complete.
    std::string focusIdForYoga;
    bool focusViaKeyboardForYoga = false;
    GameEngine::UIElement* activeTargetForYoga = nullptr;

    // Hover chain scratch: computed by ProcessHoverChain, consumed by DispatchEvents.
    // References to thread_local vectors that retain capacity across frames.
    std::vector<GameEngine::UIElement*>& hoverLeft;
    std::vector<GameEngine::UIElement*>& hoverEntered;

    // Event dispatch outputs.
    uint64_t treeGenBeforeEvents = 0;
    bool treeMutatedDuringEvents = false;
    bool treeRebuiltThisFrame = false;
    bool runInteractive = false;

    // Profiling
    bool profEnabled = false;
    GameEngine::UIManager::UpdateProfileFrame* prof = nullptr;

    // Monotonic per-frame counter bumped whenever a BuildYogaRecursive slot
    // observes a structural change (fresh Yoga node, direct ChildrenDirty, or
    // local-stylesheet attach/detach on this element). Used by the slow path
    // to detect when its own cached subtree is still structurally valid:
    // capture the value at entry, compare at exit — no delta means nothing
    // below mutated, so the previously-populated CachedSubtree / CachedSheetSets
    // can be reused verbatim without an O(N) refill.
    uint64_t subtreeStructureChanges = 0;

    // Stylesheet span accessors
    std::span<const GameEngine::Stylesheet* const> GetSheetSpan(int32_t idx) const
    {
        if (idx < 0)
            return {};
        const auto& ref = sheetSetRefs[(size_t)idx];
        return {flatSheets.data() + ref.offset, ref.count};
    }
    std::span<const GameEngine::StylesheetRuleIndex* const> GetIndexSpan(int32_t idx) const
    {
        if (idx < 0)
            return {};
        const auto& ref = sheetSetRefs[(size_t)idx];
        return {flatIndices.data() + ref.offset, ref.count};
    }
    // Step 4: handle span accessor mirroring GetSheetSpan.
    std::span<const GameEngine::StylesheetHandle> GetHandleSpan(int32_t idx) const
    {
        if (idx < 0)
            return {};
        const auto& ref = sheetSetRefs[(size_t)idx];
        if (ref.offset > flatHandles.size())
            return {};
        const size_t available = flatHandles.size() - ref.offset;
        const size_t count = std::min<size_t>(ref.count, available);
        return {flatHandles.data() + ref.offset, count};
    }
};

#endif // GE_HAVE_YOGA

// ---------------------------------------------------------------------------
// Pure hash/utility functions used by buildYoga and related code.
// Defined inline so they're available from UIManager_Layout.cpp.
// ---------------------------------------------------------------------------

// CSS content box of an element rect: inside padding AND border. The rect Yoga
// reports is the border box, so both insets still have to come off here.
//
// `usedPadding` is a LENGTH, never a percentage — CSS used values, which is
// what `ResolvedStyle::Layout.Padding` is NOT: it holds a bare `10` for
// `padding: 10%`. Emission passes UIElement::GetLayoutPadding(); pointer
// routing passes its ComputePointerStyle copy, whose padding is that same
// value. Border needs no such care — CSS has no percentage border-width.
//
// The rect is physical px; padding/border are logical px, so cs converts (pass
// 1.0f when operating on a logical-px rect, e.g. pointer hit-testing). Single
// source of the box-model rules — glyph emission, caret/selection overlays,
// and pointer hit-testing must agree or carets drift off the drawn text.
struct ContentBox { float X, Y, W, H; };

inline ContentBox ComputeContentBox(float elX, float elY, float elW, float elH,
                                    const GameEngine::Box4& usedPadding,
                                    const GameEngine::Box4& borderWidth,
                                    float cs)
{
    const float padL = usedPadding.Left   * cs;
    const float padT = usedPadding.Top    * cs;
    const float padR = usedPadding.Right  * cs;
    const float padB = usedPadding.Bottom * cs;
    const float blL = borderWidth.Left   * cs;
    const float blT = borderWidth.Top    * cs;
    const float blR = borderWidth.Right  * cs;
    const float blB = borderWidth.Bottom * cs;
    return {
        elX + padL + blL,
        elY + padT + blT,
        std::max(0.0f, elW - padL - padR - blL - blR),
        std::max(0.0f, elH - padT - padB - blT - blB)
    };
}

inline uint64_t HashPtr64(const void* p)
{
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

inline void HashCombine(uint64_t& h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

inline uint64_t HashFloatBits(float f)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(f), "float must be 32-bit");
    std::memcpy(&bits, &f, sizeof(bits));
    return (uint64_t)bits;
}

inline uint64_t HashString64(const std::string& s)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s)
    {
        h ^= (uint64_t)c;
        h *= 1099511628211ULL;
    }
    return h;
}

// Merge local stylesheet handles into an output sheet list (append-or-move-to-end).
template <typename HandleVec>
inline void MergeSheets(std::vector<const GameEngine::Stylesheet*>& outSheets,
                        const HandleVec& localHandles)
{
    for (const auto& sh : localHandles)
    {
        if (!sh)
            continue;
        const GameEngine::Stylesheet* s = sh.get();
        auto it = std::find(outSheets.begin(), outSheets.end(), s);
        if (it != outSheets.end())
        {
            outSheets.erase(it);
            outSheets.push_back(s);
        }
        else
        {
            outSheets.push_back(s);
        }
    }
}

// Step 4: handle-vector variant. Same dedup-by-pointer-identity, but
// returns a handle vector so the result keeps the Stylesheets alive.
template <typename HandleVec>
inline void MergeSheetsHandles(std::vector<GameEngine::StylesheetHandle>& outHandles,
                               const HandleVec& localHandles)
{
    for (const auto& sh : localHandles)
    {
        if (!sh)
            continue;
        const GameEngine::Stylesheet* s = sh.get();
        auto it = std::find_if(outHandles.begin(), outHandles.end(),
                               [s](const GameEngine::StylesheetHandle& h)
                               { return h.get() == s; });
        if (it != outHandles.end())
        {
            outHandles.erase(it);
            outHandles.push_back(sh);
        }
        else
        {
            outHandles.push_back(sh);
        }
    }
}

inline std::string MakeInternalBgKey(uint64_t instanceId)
{
    return std::string("__ui_el_bg_") + std::to_string(instanceId);
}

// True when el's DFS-parent chain terminates at root — i.e. the element is
// attached (crossing Mount portals). Registries and the instanceId map are
// ownership-scoped; consumers that must match the old walks' semantics
// (which only ever saw root-reachable elements) filter through this.
inline bool UIElementReachesRoot(const GameEngine::UIElement* el, const GameEngine::UIElement* root)
{
    const GameEngine::UIElement* top = el;
    for (const GameEngine::UIElement* p = el; p; p = p->GetDfsParent())
        top = p;
    return top == root && root != nullptr;
}

// Access the text-measure context pool usage counter (lives in UIManager_Layout.cpp).
// Returned by reference so Update() can reset it and read it for spike logging.
size_t& GetTextMeasureCtxUsed();

// Periodically trim the text-measure context pool: releases text strings in
// unused entries.
void TrimTextMeasureCtxPool();

void ConfigureUiFontAtlas(GameEngine::Rendering::Text::FontAtlas& atlas);
std::chrono::milliseconds ComputeFontRequestBackoff(uint32_t failCount);
