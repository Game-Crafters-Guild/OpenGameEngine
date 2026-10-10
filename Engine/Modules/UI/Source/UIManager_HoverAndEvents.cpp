#include "UI/UIManager.h"
#include "UIManager_Internal.h"
#include "UIAttributeAccess.h"

#include "UI/UIElement.h"
#include "UI/Controls/Mount.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/Interaction/DragDropOverlay.h"
#include "UI/Interaction/DismissablePopup.h"
#include "UI/Interaction/TooltipOverlay.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/TransitionEngine.h"
#include "UI/UIEvents.h"

#include "Input/KeyCodes.h"
#include "UI/Layout/YogaLayout.h"

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
#include <yoga/Yoga.h>
#endif

#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace GameEngine;

namespace
{
class ScopedUpdateEventDispatchGuard final
{
  public:
    ScopedUpdateEventDispatchGuard() { UIElement::SetInEventDispatch(true); }
    ~ScopedUpdateEventDispatchGuard() { UIElement::SetInEventDispatch(false); }

    ScopedUpdateEventDispatchGuard(const ScopedUpdateEventDispatchGuard&) = delete;
    ScopedUpdateEventDispatchGuard& operator=(const ScopedUpdateEventDispatchGuard&) = delete;
};

// Effective clip rect threaded through the recursive hit test. An
// `overflow:hidden` ancestor chain produces an intersected rect; an
// element on a non-None overlay layer resets the inherited clip
// (overlay paints outside its tree parent's bounds, so it must
// hit-test outside too).
struct HitClipState
{
    bool hasClip = false;
    float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
};

void IntersectClipInPlace(HitClipState& clip, float ax, float ay, float aw, float ah)
{
    if (!clip.hasClip)
    {
        clip.hasClip = true;
        clip.cx = ax;
        clip.cy = ay;
        clip.cw = aw;
        clip.ch = ah;
        return;
    }
    const float left = std::max(clip.cx, ax);
    const float top = std::max(clip.cy, ay);
    const float right = std::min(clip.cx + clip.cw, ax + aw);
    const float bottom = std::min(clip.cy + clip.ch, ay + ah);
    clip.cx = left;
    clip.cy = top;
    clip.cw = std::max(0.0f, right - left);
    clip.ch = std::max(0.0f, bottom - top);
}

bool ClipContains(const HitClipState& clip, float mx, float my)
{
    if (!clip.hasClip)
        return true;
    if (clip.cw <= 0.0f || clip.ch <= 0.0f)
        return false;
    return (mx >= clip.cx && mx <= clip.cx + clip.cw && my >= clip.cy && my <= clip.cy + clip.ch);
}

struct HitCandidate
{
    UIElement* el = nullptr;
    // Rendering defers overlays by OverlayLayer, so pointer selection must use
    // the same priority before comparing z-index and traversal order.
    int overlayPriority = 0;
    // Overlays nested in another overlay paint after the overlay subtree that
    // discovered them, whatever their z-index, so at one priority the deeper
    // overlay wins before z-index is compared.
    int overlayDepth = 0;
    int z = 0;
    int order = 0;
};

bool IsDescendantOf(const UIElement* element, const UIElement* ancestor)
{
    const UIElement* cur = element;
    int depth = 0;
    constexpr int kMaxDepth = 2048;
    while (cur && depth++ < kMaxDepth)
    {
        if (cur == ancestor)
            return true;
        cur = cur->GetParent();
    }
    return false;
}

// Popup gating consumes UIManager's typed registries (maintained by the
// controls' OnOwnerManagerChanged) instead of DFS+dynamic_cast over the
// whole tree per hover. The open-state checks stay per-lookup — the
// registries hold every instance, open or not, and are tiny.
//
// The registries are ownership-scoped, but the old walks were
// root-reachability-scoped: an open popup inside a detached-but-owned
// Mount target (inactive dock tab) was invisible to them and must not
// gate input. UIElementReachesRoot (UIManager_Internal.h) restores that
// scoping per entry.
bool ReachesRoot(const UIElement* el, const UIElement* root)
{
    return UIElementReachesRoot(el, root);
}

// An open popup that is actually reachable from the root, or nullptr. Both
// conditions are per-lookup: the registry holds every instance whether open or
// closed, and an instance inside a detached-but-owned Mount target (an
// inactive dock tab) must not gate or answer to input.
UIElement* LivePopupRoot(DismissablePopup* popup, UIElement* root)
{
    if (!popup || !popup->IsPopupOpen())
        return nullptr;
    UIElement* popupRoot = popup->GetPopupRoot();
    if (!popupRoot || !ReachesRoot(popupRoot, root))
        return nullptr;
    return popupRoot;
}

// While any popup is open, input outside every open popup is swallowed rather
// than delivered to whatever sits underneath. This is what lets a popup skip
// the transparent full-window click-catcher: the catcher's only other job,
// closing the popup, is the manager's DismissPopupsForOutsidePress.
UIElement* GatePointerTargetToOpenPopups(const std::vector<DismissablePopup*>& registry,
                                         UIElement* root, UIElement* candidate)
{
    if (!candidate)
        return candidate;

    bool anyOpen = false;
    for (DismissablePopup* popup : registry)
    {
        UIElement* popupRoot = LivePopupRoot(popup, root);
        if (!popupRoot)
            continue;
        anyOpen = true;
        if (IsDescendantOf(candidate, popupRoot))
            return candidate;
    }
    return anyOpen ? nullptr : candidate;
}

// Pre-order DFS hit walk. `inheritedClip` arrives clipped through every
// ancestor with overflow != visible. `parentPosContext` carries the
// z-stacking-context z-index of the nearest positioned ancestor;
// `parentOverlayDepth` counts the overlay elements on the ancestor chain.
//
// Subtree cull: when the inherited clip excludes the cursor, the entire
// subtree is normally pruned. Pruning is unsafe if any descendant carries
// a non-None overlay layer — overlays clear their inherited clip on
// entry and may be hittable outside the ancestor's bounds (Dropdown
// items, modal SearchDialog, drag preview). UIElement::HasOverlayInSubtree()
// gates the prune; when set, we still descend but fast-path through
// non-overlay siblings to find only the overlay-bearing branches.
void HitTestSubtreeRecursive(UIElement* el,
                             float mx, float my,
                             int parentPosContext,
                             int parentOverlayPriority,
                             int parentOverlayDepth,
                             HitClipState inheritedClip,
                             bool requirePointerEvents,
                             int& orderCounter,
                             HitCandidate& best)
{
    if (!el)
        return;

    const ResolvedStyle& rs = el->GetResolvedStyle();

    // Yoga doesn't lay out descendants of a display:none parent, so
    // m_LastX/Y/W/H on this element and below carry stale values from
    // the last visible layout. Hit-testing them lets a closed modal's
    // pointer-events:true backdrop keep its full-screen rect and
    // silently capture every click. Prune the whole subtree.
    if (rs.Layout.DisplayMode == DisplayMode::None)
        return;

    const bool selfIsOverlay = (el->GetOverlayLayer() != OverlayLayer::None);
    // Nested overlays render after the overlay subtree that discovered them,
    // even when their nominal layer is lower (for example Dropdown items in a
    // Modal SearchDialog). Their hit-test priority must likewise never fall
    // below the containing overlay or the modal backdrop wins hover/clicks over
    // a popup that is visibly drawn on top.
    const int selfOverlayPriority = selfIsOverlay
        ? std::max(parentOverlayPriority, static_cast<int>(el->GetOverlayLayer()))
        : parentOverlayPriority;
    const int selfOverlayDepth = parentOverlayDepth + (selfIsOverlay ? 1 : 0);

    // Subtree cull: inherited clip excludes mouse AND self isn't an
    // overlay (overlays reset their clip below — see selfClip). The
    // overlay-in-subtree bit folds in mount targets, so a single check
    // suffices regardless of whether `el` is a Mount host.
    const bool inheritedExcludes = inheritedClip.hasClip && !ClipContains(inheritedClip, mx, my);
    const bool nonOverlayCantHit = inheritedExcludes && !selfIsOverlay;
    if (nonOverlayCantHit && !el->HasOverlayInSubtree())
        return;  // entire subtree pruned — no overlay can rescue it

    // The element's own clip: inherit from ancestors, then reset to
    // "no clip" for overlay layers (dropdown/modal/tooltip/drag-preview
    // panels paint outside the chain's overflow:hidden boxes by design).
    HitClipState selfClip = inheritedClip;
    if (selfIsOverlay)
        selfClip = HitClipState{};

    // Stacking context. An element with explicit z-index > 0 establishes
    // a new positioning context; descendants without their own z-index
    // inherit it. Negative z-index falls through both the explicit and
    // inherited-context checks so it keeps its own value, sorting below
    // z=0 siblings — used by the editor's overlay layering rules.
    const int z = rs.Layout.ZIndex;
    const int selfZ = (z > 0) ? z : (parentPosContext != 0 ? parentPosContext : z);
    const int childPosContext = (z > 0) ? z : parentPosContext;

    const int order = orderCounter++;

    // Element-level hit-test. Skip when overlay-search-mode rules out
    // self, or the visibility/pointer-events gate fails (display:none
    // already filtered above).
    const bool gate = rs.Visual.Visible
        && (!requirePointerEvents || rs.Visual.PointerEvents);
    const bool selfHittable = gate && !nonOverlayCantHit;
    if (selfHittable)
    {
        float x, y, w, h;
        el->GetHitTestBounds(x, y, w, h);
        if (w > 0.0f && h > 0.0f &&
            mx >= x && mx <= x + w && my >= y && my <= y + h &&
            ClipContains(selfClip, mx, my))
        {
            // Tiebreak: equal z + later DFS order wins (matches the
            // `z == bestZ && i >= bestIdx` rule in the flat path).
            const bool samePriority = selfOverlayPriority == best.overlayPriority;
            if (!best.el || selfOverlayPriority > best.overlayPriority ||
                (samePriority && selfOverlayDepth > best.overlayDepth) ||
                (samePriority && selfOverlayDepth == best.overlayDepth &&
                 (selfZ > best.z || (selfZ == best.z && order >= best.order))))
            {
                best.el = el;
                best.overlayPriority = selfOverlayPriority;
                best.overlayDepth = selfOverlayDepth;
                best.z = selfZ;
                best.order = order;
            }
        }
    }

    // Clip propagated to descendants: own overflow:hidden adds a fresh
    // intersection on top of `selfClip`. (`selfClip` already incorporates
    // the overlay reset, so descendants of an overlay element start
    // unclipped, then re-narrow if the overlay element itself has
    // overflow:hidden.)
    HitClipState childClip = selfClip;
    if (rs.Layout.Overflow == Overflow::Hidden)
    {
        // Same region paint clips to: the PADDING box (css-overflow-3), so a
        // child cut at the inner border edge is not hoverable where it is
        // not visible.
        const Box4& bw = rs.Layout.BorderWidth;
        IntersectClipInPlace(childClip,
                             el->GetLayoutX() + bw.Left, el->GetLayoutY() + bw.Top,
                             std::max(0.0f, el->GetLayoutWidth() - bw.Left - bw.Right),
                             std::max(0.0f, el->GetLayoutHeight() - bw.Top - bw.Bottom));
    }

    // Overlay-only descent: when the cull would have pruned us if not for
    // an overlay descendant, skip non-overlay-bearing branches so we walk
    // straight to the overlay branch instead of every leaf. The bit folds
    // in mount targets, so a single HasOverlayInSubtree() check on each
    // candidate is sufficient.
    for (auto& child : el->GetChildren())
    {
        if (!child)
            continue;
        if (nonOverlayCantHit && !child->HasOverlayInSubtree())
            continue;
        HitTestSubtreeRecursive(child.get(), mx, my, childPosContext, selfOverlayPriority,
                                selfOverlayDepth, childClip, requirePointerEvents, orderCounter, best);
    }

    if (UIElement* tgt = el->GetMountTarget())
    {
        if (!nonOverlayCantHit || tgt->HasOverlayInSubtree())
            HitTestSubtreeRecursive(tgt, mx, my, childPosContext, selfOverlayPriority,
                                    selfOverlayDepth, childClip, requirePointerEvents, orderCounter, best);
    }
}

void DispatchFocusEvent(UIElement& el, EventId id)
{
    UIEvent ev{};
    ev.Id = id;
    ev.Target = &el;
    ev.CurrentTarget = &el;
    el.DispatchEvent(ev);
}

// True when `el` is `subtreeRoot` or reaches it through parent links. Mount links are not
// followed: a Mount's target is not owned by its host.
bool IsInOwnedSubtree(const UIElement* el, const UIElement* subtreeRoot)
{
    for (const UIElement* e = el; e; e = e->GetParent())
    {
        if (e == subtreeRoot)
            return true;
    }
    return false;
}

} // namespace

UIElement* UIManager::HitTestTree(float mx, float my, bool requirePointerEvents) const
{
    if (!m_Root)
        return nullptr;
    HitCandidate best{};
    HitClipState rootClip{};
    int orderCounter = 0;
    HitTestSubtreeRecursive(m_Root.get(), mx, my, /*parentPosContext=*/0,
                            /*parentOverlayPriority=*/0, /*parentOverlayDepth=*/0, rootClip,
                            requirePointerEvents, orderCounter, best);
    return GatePointerTargetToOpenPopups(m_DismissablePopupRegistry, m_Root.get(), best.el);
}

void UIManager::ReleaseMouseCapture()
{
    if (!m_MouseCaptured && m_CaptureId.empty() && !m_CaptureElement)
        return;
    m_MouseCaptured = false;
    m_CaptureElement = nullptr;
    m_CaptureInstanceId = 0;
    m_CaptureId.clear();
    // Retarget hover to whatever is under the pointer now, even if the pointer
    // does not move again — the same reason the mouse-up path sets this.
    m_MouseMoved = true;
}

namespace
{
// GE_UI_HOVER_TRACE=1: per-transition log of the hover pipeline — hit result,
// how many elements each transition marked, and how many the narrow filter
// skipped. Diagnostic for "hover state right, pixels stale" reports.
bool HoverTraceEnabled()
{
    static const bool enabled = [] {
        const char* v = std::getenv("GE_UI_HOVER_TRACE");
        return v && v[0] == '1';
    }();
    return enabled;
}

std::string HoverTraceName(UIElement* el)
{
    if (!el)
        return "<none>";
    const std::string& id = el->GetId();
    if (!id.empty())
        return id;
    return UIAttributeAccess::GetDebugTypeName(*el) + "#" + std::to_string(el->GetInstanceId());
}
} // namespace

bool UIManager::DismissPopupsForOutsidePress(UIElement* pressTarget)
{
    UIElement* root = m_Root.get();

    // Every open popup the press landed outside of closes; one that contains
    // the press survives. Applying the test per popup is what makes nesting
    // work: a press in a search dialog's body, outside a dropdown the dialog
    // hosts, closes the dropdown and keeps the dialog.
    //
    // Collect before dismissing — DismissPopup mutates the tree, which would
    // invalidate a registry walk in progress.
    std::vector<DismissablePopup*> toDismiss;
    bool pressInsideSurvivor = false;
    for (DismissablePopup* popup : m_DismissablePopupRegistry)
    {
        UIElement* popupRoot = LivePopupRoot(popup, root);
        if (!popupRoot)
            continue;
        if (pressTarget && (IsDescendantOf(pressTarget, popupRoot) ||
                            popup->PressWithinPopupGroup(pressTarget)))
        {
            pressInsideSurvivor = true;
            continue;
        }
        toDismiss.push_back(popup);
    }
    if (HoverTraceEnabled() && !m_DismissablePopupRegistry.empty())
        Logger::Log::Info("[HoverTrace] outsidePress target={} dismissing={} of {} insideSurvivor={}",
                          HoverTraceName(pressTarget), toDismiss.size(),
                          m_DismissablePopupRegistry.size(), pressInsideSurvivor);

    for (DismissablePopup* popup : toDismiss)
    {
        // The snapshot holds raw pointers, and one dismissal can destroy later
        // entries: a context menu closes its submenus with it, and every panel
        // registers separately. Re-check against the live registry — the
        // destructor unregisters, so absence means this entry is already gone
        // and touching it reads freed memory.
        if (std::find(m_DismissablePopupRegistry.begin(), m_DismissablePopupRegistry.end(),
                      popup) == m_DismissablePopupRegistry.end())
            continue;
        if (popup->IsPopupOpen())
            popup->DismissPopup();
    }
    // The return drives the caller's consume-the-press behavior. A press that
    // landed inside a still-open popup keeps flowing to its target even when
    // it dismissed a sibling layer (clicking a menu's search field with a
    // submenu open must close the submenu AND focus the field in one click);
    // only a press inside no surviving popup is swallowed.
    return !toDismiss.empty() && !pressInsideSurvivor;
}

bool UIManager::DismissTopmostPopup()
{
    UIElement* root = m_Root.get();
    // Attach order is registration order, so the last open entry is the one
    // stacked on top — Escape peels one layer at a time.
    for (auto it = m_DismissablePopupRegistry.rbegin(); it != m_DismissablePopupRegistry.rend(); ++it)
    {
        if (!LivePopupRoot(*it, root))
            continue;
        (*it)->DismissPopup();
        return true;
    }
    return false;
}

namespace
{

// Hover chain diff shared by ProcessHoverChain and TryPointerOnlyFrame:
// elements on prev's ancestor chain but not cur's → outLeft (mouse-leave),
// and vice versa → outEntered (mouse-enter). Chains are collected root-ward;
// each element appears in at most one output.
void DiffHoverChains(UIElement* prevHover, UIElement* curHover,
                     std::vector<UIElement*>& outLeft,
                     std::vector<UIElement*>& outEntered)
{
    outLeft.clear();
    outEntered.clear();
    if (prevHover == curHover)
        return;

    static thread_local std::vector<UIElement*> oldChain;
    static thread_local std::vector<UIElement*> newChain;

    auto collectChain = [](UIElement* start, std::vector<UIElement*>& out)
    {
        out.clear();
        UIElement* cur = start;
        int depth = 0;
        constexpr int kMaxDepth = 1024;
        while (cur && depth++ < kMaxDepth)
        {
            out.push_back(cur);
            cur = cur->GetParent();
        }
    };
    collectChain(prevHover, oldChain);
    collectChain(curHover, newChain);

    auto containsEl = [](const std::vector<UIElement*>& chain, UIElement* el) -> bool
    {
        return std::find(chain.begin(), chain.end(), el) != chain.end();
    };

    outLeft.reserve(oldChain.size());
    outEntered.reserve(newChain.size());

    for (UIElement* el : oldChain)
    {
        if (el && !containsEl(newChain, el))
            outLeft.push_back(el);
    }
    for (UIElement* el : newChain)
    {
        if (el && !containsEl(oldChain, el))
            outEntered.push_back(el);
    }
}

// Records the instance ids of leaf's ancestor chain (leaf first). The
// pre-pass/gate bookkeeping stores the whole chain so ProcessHoverChain can
// fall back to the nearest LIVING ancestor when the leaf is destroyed
// between the two passes (virtualization drain) — diffing against null would
// re-dispatch mouse-enter to ancestors the pointer never left.
void CaptureAncestorChainIds(UIElement* leaf, std::vector<uint64_t>& out)
{
    out.clear();
    UIElement* cur = leaf;
    int depth = 0;
    constexpr int kMaxDepth = 1024;
    while (cur && depth++ < kMaxDepth)
    {
        out.push_back(cur->GetInstanceId());
        cur = cur->GetParent();
    }
}

} // namespace


bool UIManager::MarkHoverTransitionDirty(UIElement* prevHover, UIElement* newHover, PseudoMarkMode mode)
{
    if (prevHover == newHover)
        return false;

    const bool mayDesc = m_StyleAnalysis.Hover.MayAffectDescendants;
    const bool maySib = m_StyleAnalysis.Hover.MayAffectSiblings;
    const bool hoverAffectsAnything = m_StyleAnalysis.Hover.AffectsLayout || m_StyleAnalysis.Hover.AffectsPaint;
    if (!hoverAffectsAnything)
        return false;

    static thread_local std::vector<UIElement*> oldChain;
    static thread_local std::vector<UIElement*> newChain;
    oldChain.clear();
    newChain.clear();

    auto collectChain = [](UIElement* start, std::vector<UIElement*>& out)
    {
        out.clear();
        UIElement* cur = start;
        int depth = 0;
        constexpr int kMaxDepth = 1024;
        while (cur && depth++ < kMaxDepth)
        {
            out.push_back(cur);
            cur = cur->GetParent();
        }
    };
    collectChain(prevHover, oldChain);
    collectChain(newHover, newChain);

    auto containsEl = [](const std::vector<UIElement*>& chain, UIElement* el) -> bool
    {
        return std::find(chain.begin(), chain.end(), el) != chain.end();
    };

    // MarkOnly runs before the frame's cascade: marked elements re-resolve there
    // with the already-updated m_Hovered, so no in-place re-bake is needed. A
    // layout-affecting hover transition additionally raises LayoutDirty so the
    // layout-signature pass (and the solve) runs THIS frame instead of deferring
    // the geometry change to the next one.
    //
    // ResolveInPlace is for post-cascade callers: the re-bake resolves the style
    // immediately, so only VisualDirty is raised — the render-side drain
    // re-emits the element without tripping the full-DFS primitive regen
    // (NotifyDirty_UpdateRegenFlag counts StyleDirty as layout-affecting).
    // A layout-affecting re-bake re-marks StyleDirty itself inside
    // ReResolvePseudoStateElement, which correctly escalates.
    const bool resolveInPlace = (mode != PseudoMarkMode::MarkOnly);
    const bool gated = (mode == PseudoMarkMode::ResolveInPlaceGated);
    unsigned hoverDirty = UIElement::VisualDirty;
    if (!resolveInPlace)
    {
        hoverDirty |= UIElement::StyleDirty;
        if (m_StyleAnalysis.Hover.AffectsLayout)
            hoverDirty |= UIElement::LayoutDirty;
    }
    // In-place re-bake is per-element cascade work; past a small transition
    // size the batch cascade is cheaper (shared sheet spans, parent style in
    // hand). Only the gated mode caps the re-bakes: its overflow StyleDirty
    // marks escalate to a same-frame heavy pass. Post-cascade callers run
    // unbounded — their overflow would not be serviced until NEXT frame,
    // tearing the hover paint across two frames.
    constexpr int kMaxInPlaceReResolves = 12;
    int reResolveBudget = gated ? kMaxInPlaceReResolves : std::numeric_limits<int>::max();
    // Cascade-memoization Phase 5: when the rule cache is the primary
    // cascade path, m_MatchedPseudoStates reflects which dynamic pseudo
    // classes appear in any rule that COULD match an element (structural
    // matches independent of state). Skip MarkDirty for elements with no
    // HoverRules bit — no rule references :hover for them, so the
    // transition can't change their style. Recursion still walks the
    // subtree because descendants may have the bit even when an ancestor
    // doesn't.
    //
    // Without the cache, m_MatchedPseudoStates only captures rules
    // currently matching, which is unsound (an element with
    // `.foo:hover` would have HoverRules=0 when not hovered, then the
    // hover-to-hovered transition would be wrongly skipped). So narrowing
    // is gated on cache being enabled.
    const bool narrow = UIParsing::CSSParser::IsRuleCacheEnabled();
    int traceMarked = 0;
    int traceNarrowSkipped = 0;
    auto shouldMarkHover = [narrow, &traceNarrowSkipped](UIElement* el) -> bool
    {
        if (!narrow)
            return true;
        const bool has = el->HasPseudoStateRules(UIElement::HoverRules);
        if (!has)
            ++traceNarrowSkipped;
        return has;
    };

    // Post-cascade callers re-bake :hover in place with the now-current
    // m_Hovered so the transition doesn't paint one frame late (m_Hovered is
    // already updated at those call sites) — until the budget runs out, after
    // which elements are marked for the batch path instead.
    auto markOneElement = [&](UIElement* el)
    {
        ++traceMarked;
        // Gated mode: elements with declared CSS transitions must take the
        // batch cascade. On a fully gated frame TransitionEngine::Advance
        // never runs, so an in-place re-bake would snap the final value
        // instead of starting the transition; the StyleDirty mark escalates
        // to a same-frame heavy pass, where Advance observes the change.
        // (Non-gated in-place callers run on heavy frames where Advance runs
        // later the same frame — in-place is transition-correct there.)
        const bool declaresTransitions =
            gated && !el->GetResolvedStyle().Transitions.IsEmpty();
        if (resolveInPlace && reResolveBudget > 0 && !declaresTransitions)
        {
            --reResolveBudget;
            el->MarkDirty(hoverDirty);
            ReResolvePseudoStateElement(el);
        }
        else if (resolveInPlace)
        {
            el->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
        }
        else
        {
            el->MarkDirty(hoverDirty);
        }
    };

    auto markHoverDirty = [&](UIElement* root)
    {
        if (!root)
            return;
        if (maySib)
        {
            if (UIElement* p = root->GetParent())
            {
                // Skip root itself: the subtree walk below handles it. Without
                // this, ResolveInPlace re-bakes root's full cascade twice per
                // transition.
                for (auto& ch : p->GetChildren())
                    if (ch && ch.get() != root && shouldMarkHover(ch.get()))
                        markOneElement(ch.get());
            }
        }
        static thread_local std::vector<UIElement*> stack;
        stack.clear();
        stack.push_back(root);
        while (!stack.empty())
        {
            UIElement* cur = stack.back();
            stack.pop_back();
            if (!cur)
                continue;
            if (shouldMarkHover(cur))
                markOneElement(cur);
            if (mayDesc)
            {
                for (const auto& ch : cur->GetChildren())
                    if (ch)
                        stack.push_back(ch.get());
                if (auto* m = dynamic_cast<Mount*>(cur))
                    if (UIElement* tgt = m->GetTarget())
                        stack.push_back(tgt);
            }
        }
    };

    for (UIElement* el : oldChain)
    {
        if (el && !containsEl(newChain, el))
            markHoverDirty(el);
    }
    for (UIElement* el : newChain)
    {
        if (el && !containsEl(oldChain, el))
            markHoverDirty(el);
    }
    if (HoverTraceEnabled())
    {
        Logger::Log::Info("[HoverTrace] mark {} -> {} mode={} marked={} narrowSkipped={} budgetLeft={}",
                          HoverTraceName(prevHover), HoverTraceName(newHover),
                          static_cast<int>(mode), traceMarked, traceNarrowSkipped, reResolveBudget);
    }
    return true;
}

// ---------------------------------------------------------------------------
// PreProcessPointerState: early pointer pre-pass for heavy frames. Runs after
// hover recovery and BEFORE the main cascade/solve, so the frame's single
// style resolution already sees current pointer state — instead of resolving
// with last frame's m_Hovered and patching afterwards. ProcessHoverChain
// stays the authoritative event producer post-solve; it patches the rare
// case where the solve itself moves layout under the cursor.
// ---------------------------------------------------------------------------
bool UIManager::PreProcessPointerState(bool runInteractive)
{
    m_HoverPrePassRan = false;
    m_HoverPrePassMarked = false;
    m_HoverEventDiffBaseChain.clear();
    m_ActivePrePassTargetId = 0;
    if (!runInteractive || !m_Root)
        return false;
    const bool pointerEdge = m_ButtonEdgeSinceLastUpdate;
    if (!m_MouseMoved && !pointerEdge)
        return false;
    if (!IsMousePositionKnown())
        return false;

    GE_CPU_PROFILE_SCOPE("UIManager.Update.PointerPrePass");

    UIElement* prevHover = m_Hovered;
    UIElement* newHover =
        m_MouseCaptured ? m_CaptureElement : HitTestTree(m_MouseX, m_MouseY);
    CaptureAncestorChainIds(prevHover, m_HoverEventDiffBaseChain);
    m_HoverPrePassRan = true;
    m_Hovered = newHover;
    m_HoveredInstanceId = newHover ? newHover->GetInstanceId() : 0;

    bool anyMarked = false;
    if (prevHover != newHover)
    {
        m_HoverPrePassMarked = MarkHoverTransitionDirty(prevHover, newHover, PseudoMarkMode::MarkOnly);
        anyMarked |= m_HoverPrePassMarked;
    }

    // :active press/release edge (rationale in ProcessHoverChain). Marked here
    // so the edge's style resolves in this frame's cascade; ProcessHoverChain
    // skips the same target afterwards to avoid a second re-bake.
    if (pointerEdge && (m_StyleAnalysis.Active.AffectsLayout || m_StyleAnalysis.Active.AffectsPaint))
    {
        if (UIElement* activeTarget = ActiveTarget())
        {
            unsigned flags = UIElement::StyleDirty | UIElement::VisualDirty;
            if (m_StyleAnalysis.Active.AffectsLayout)
                flags |= UIElement::LayoutDirty;
            activeTarget->MarkDirty(flags);
            m_ActivePrePassTargetId = activeTarget->GetInstanceId();
            anyMarked = true;
        }
    }
    return anyMarked;
}

// ---------------------------------------------------------------------------
// TryPointerOnlyFrame: services a bare mouse move over an otherwise-clean
// tree without the heavy pass. See the declaration for the result contract.
// ---------------------------------------------------------------------------
UIManager::PointerFrameResult UIManager::TryPointerOnlyFrame(unsigned dirtyNow, bool interactive, bool envForcesHeavy,
                                                             bool profEnabled, UpdateProfileFrame& prof)
{
    m_PointerMoveDispatchedThisFrame = false;

    // Qualify: interactive, pointer moved, nothing else pending. Visual-only
    // dirt (low bit of dirtyNow) is allowed — it is consumed by the
    // render-side drain and needs no Update-side work. m_PointerGateDecline
    // feeds the GE_UI_HEAVY_INPUT_LOG diagnostic.
    m_PointerGateDecline = PointerGateDecline::NotPointerFrame;
    if (!interactive || !m_Root || envForcesHeavy)
        return PointerFrameResult::NotApplicable;
    if (m_CorrectnessModeEnabled || m_DisableFastPathNoOp)
        return PointerFrameResult::NotApplicable;
    if (!m_MouseMoved || !IsMousePositionKnown())
        return PointerFrameResult::NotApplicable;
    if (m_ButtonEdgeSinceLastUpdate)
        return PointerFrameResult::NotApplicable;
    // Pointer capture retargets moves to the capture element. The event still
    // bubbles through that element's ancestors, and no layout work is needed.
    const bool capturedPointerMove = m_MouseCaptured;
    const uint64_t captureInstanceBefore = m_CaptureInstanceId;
    UIElement* capturedTarget = capturedPointerMove ? m_CaptureElement : nullptr;
    if (capturedPointerMove && !capturedTarget)
        return PointerFrameResult::NotApplicable;
    // A keystroke already ran its handlers this frame; whatever they marked
    // needs the heavy pass, not a pointer-only one.
    if (m_KeyInputSinceLastUpdate)
        return PointerFrameResult::NotApplicable;
    if (m_ScrollWheelMoved || m_ScrollOffsetsChanged)
        return PointerFrameResult::NotApplicable;
    m_PointerGateDecline = PointerGateDecline::DirtyTree;
    if ((dirtyNow & ~unsigned(UIElement::VisualDirty)) != 0u)
        return PointerFrameResult::NotApplicable;
    m_PointerGateDecline = PointerGateDecline::RelayoutOrRebuild;
    if (m_RequestRelayout || m_ForceFullRebuildNextFrame || m_RebuildDockspace)
        return PointerFrameResult::NotApplicable;
    m_PointerGateDecline = PointerGateDecline::Virtualization;
    if (m_VirtualizationImpactFlags.load(std::memory_order_relaxed) != 0u)
        return PointerFrameResult::NotApplicable;
    // Queued virtualization work (EnqueueVirtualizationWork raises no dirty
    // hint) drains only in the heavy pre-solve pass — gating past it would
    // starve rebinds for as long as the pointer keeps moving.
    if (m_VirtualizationCoordinator.HasPending())
        return PointerFrameResult::NotApplicable;
    m_PointerGateDecline = PointerGateDecline::TreeGeneration;
    if (m_TreeStructureGeneration.load(std::memory_order_relaxed) != m_LastBuiltStructureGeneration)
        return PointerFrameResult::NotApplicable;
    m_PointerGateDecline = PointerGateDecline::StylesheetGeneration;
    if (m_AppliedStylesheetSetGeneration != m_StylesheetSetGeneration ||
        m_AppliedStylesheetContentGeneration != m_StylesheetContentGeneration)
        return PointerFrameResult::NotApplicable;
    // Active transitions tick in the heavy tail. Transitions STARTED by a
    // gated hover flip are handled per-element: MarkHoverTransitionDirty's
    // gated mode routes transition-declaring elements to the batch path,
    // which escalates this frame so TransitionEngine::Advance observes the
    // start.
    m_PointerGateDecline = PointerGateDecline::Transitions;
    if (m_TransitionEngine && m_TransitionEngine->HasActiveTransitions())
        return PointerFrameResult::NotApplicable;
    m_PointerGateDecline = PointerGateDecline::DragDrop;
    if (m_DragDrop && m_DragDrop->IsDragging())
        return PointerFrameResult::NotApplicable;
    // A pending focus-change diff (e.g. a blur via SetFocusById("") raises no
    // dirt) is notified by NotifyFocusChange in the heavy pass only — focus
    // events and :focus styling must not wait out a mouse-move streak.
    m_PointerGateDecline = PointerGateDecline::FocusNotify;
    if (m_FocusId != m_LastFocusIdNotified)
        return PointerFrameResult::NotApplicable;
    // Async font installs raise no dirty mark — the heavy pass's BuildYoga
    // measure-input diff is the reflow detector. Gating past a pending
    // install would hold fallback-font layout for as long as the pointer
    // keeps moving. Cleared by the heavy pass once the batch completes.
    m_PointerGateDecline = PointerGateDecline::FontResolve;
    if (m_FontResolvedDirty)
        return PointerFrameResult::NotApplicable;

    m_PointerGateDecline = PointerGateDecline::Taken;

    GE_CPU_PROFILE_SCOPE("UIManager.Update.PointerOnlyFrame");

    // 1. Hit test + hover update — the frame's only pointer processing.
    UIElement* prevHover = m_Hovered;
    UIElement* newHover = nullptr;
    {
        ScopedSectionTimer _tHit(profEnabled, &prof.HitTestMs);
        newHover = capturedPointerMove
            ? capturedTarget
            : HitTestTree(m_MouseX, m_MouseY);
    }
    m_Hovered = newHover;
    m_HoveredInstanceId = newHover ? newHover->GetInstanceId() : 0;

    if (HoverTraceEnabled() && prevHover != newHover)
        Logger::Log::Info("[HoverTrace] fast hit {} -> {} at ({:.0f},{:.0f})",
                          HoverTraceName(prevHover), HoverTraceName(newHover), m_MouseX, m_MouseY);

    // 2. Pseudo-state re-bake. VisualDirty marks ride the render-side drain;
    //    a layout-affecting re-bake re-marks StyleDirty inside
    //    ReResolvePseudoStateElement, which the check below turns into a
    //    heavy pass this same frame.
    if (prevHover != newHover)
        MarkHoverTransitionDirty(prevHover, newHover, PseudoMarkMode::ResolveInPlaceGated);

    if ((m_DirtyHintFlags.load(std::memory_order_relaxed) & ~unsigned(UIElement::VisualDirty)) != 0u)
    {
        // Events have NOT been dispatched yet: seed the pre-pass bookkeeping
        // so ProcessHoverChain diffs enter/leave against the true last-frame
        // hover without re-marking what was already resolved.
        m_HoverPrePassRan = true;
        m_HoverPrePassMarked = true;
        CaptureAncestorChainIds(prevHover, m_HoverEventDiffBaseChain);
        m_PointerGateDecline = PointerGateDecline::EscalatedMark;
        return PointerFrameResult::Escalated;
    }

    // 3. Event dispatch: enter/leave (non-bubbling) + bubbling mouse move,
    //    then one scheduler/dispatcher round for handler follow-ups
    //    (mirrors the heavy path's convergence drain).
    bool treeMutated = false;
    const uint64_t treeGenBefore = m_TreeStructureGeneration.load(std::memory_order_relaxed);
    // Captured BEFORE dispatch: events below cover the prevHover→newHover
    // transition, so a post-dispatch escalation must diff against newHover —
    // whose chain has to be recorded while the handlers can't yet have
    // destroyed it.
    CaptureAncestorChainIds(newHover, m_HoverEventDiffBaseChain);
    {
        ScopedSectionTimer _tEvents(profEnabled, &prof.EventDispatchMs);
        static thread_local std::vector<UIElement*> left;
        static thread_local std::vector<UIElement*> entered;
        DiffHoverChains(prevHover, newHover, left, entered);
        for (UIElement* el : left)
            if (el)
                DispatchBubbleEvent(el, kEventMouseLeave, 0, /*bubble=*/false, treeMutated, treeGenBefore);
        for (UIElement* el : entered)
            if (el)
                DispatchBubbleEvent(el, kEventMouseEnter, 0, /*bubble=*/false, treeMutated, treeGenBefore);
        UIElement* moveTarget = capturedPointerMove ? capturedTarget : m_Hovered;
        if (moveTarget)
            DispatchBubbleEvent(moveTarget, kEventMouseMove, 0, /*bubble=*/true, treeMutated, treeGenBefore);

        if (m_Scheduler)
            m_Scheduler->ProcessDue();
        if (m_Dispatcher && m_Dispatcher->PendingCount())
            m_Dispatcher->Drain();
    }

    // 4. Post-dispatch escalation: a handler mutated something a pointer-only
    //    frame can't service — run the heavy pass this same frame. Events
    //    stay dispatched exactly once: the bookkeeping below makes
    //    ProcessHoverChain a no-op diff and DispatchEvents skip its
    //    mouse-move dispatch.
    const bool captureChanged =
        (m_MouseCaptured != capturedPointerMove) ||
        (m_MouseCaptured && m_CaptureInstanceId != captureInstanceBefore);
    const bool handlersDirtied =
        treeMutated ||
        (m_TreeStructureGeneration.load(std::memory_order_relaxed) != treeGenBefore) ||
        ((m_DirtyHintFlags.load(std::memory_order_relaxed) & ~unsigned(UIElement::VisualDirty)) != 0u) ||
        m_RequestRelayout || m_ScrollOffsetsChanged || m_ScrollWheelMoved ||
        m_KeyInputSinceLastUpdate ||
        captureChanged || m_RebuildDockspace ||
        (m_VirtualizationImpactFlags.load(std::memory_order_relaxed) != 0u) ||
        m_VirtualizationCoordinator.HasPending() ||
        (m_FocusId != m_LastFocusIdNotified);
    if (handlersDirtied)
    {
        m_HoverPrePassRan = true;
        m_HoverPrePassMarked = true;
        // m_HoverEventDiffBaseChain: newHover's chain, captured pre-dispatch.
        m_PointerMoveDispatchedThisFrame = true;
        m_PointerGateDecline = PointerGateDecline::EscalatedDispatch;
        return PointerFrameResult::Escalated;
    }

    // 5. Fully serviced. Consume the pointer input and the VisualDirty hint
    //    bit our own marks raised (the drain owns that work), so the next
    //    frame can take this path again.
    m_HoverEventDiffBaseChain.clear();
    m_MouseMoved = false;
    m_DirtyHintFlags.fetch_and(~unsigned(UIElement::VisualDirty), std::memory_order_relaxed);
    return PointerFrameResult::Handled;
}

// ---------------------------------------------------------------------------
// ProcessHoverChain: hit-test, hover-chain diff, dirty marking, and
// ctx.hoverLeft / ctx.hoverEntered output for downstream event dispatch.
// ---------------------------------------------------------------------------

void UIManager::ProcessHoverChain(UpdateContext& ctx)
{
    GE_CPU_PROFILE_SCOPE("UIManager.Update.HoverChain");
    ScopedSectionTimer _tHit(ctx.profEnabled, &ctx.prof->HitTestMs);

    // Marking diff base: m_Hovered at entry. When the pre-pass ran, that is
    // its hit result — those transition elements were marked pre-cascade and
    // already resolved by this frame's cascade, so only a DIFFERENT hit here
    // (the solve moved layout under the cursor) needs patching.
    UIElement* prevHover = m_Hovered;
    // Event diff base: the true last-frame hover. Enter/leave must span the
    // full frame-over-frame transition even when the pre-pass already
    // advanced m_Hovered. Instance-id lookup — the element may have been
    // destroyed between the two passes (virtualization drain).
    UIElement* eventDiffBase = prevHover;
    const bool prePassRan = m_HoverPrePassRan;
    const bool prePassMarked = m_HoverPrePassMarked;
    m_HoverPrePassRan = false;
    m_HoverPrePassMarked = false;
    if (prePassRan)
    {
        // Deepest living element of the recorded chain. The leaf may have
        // been destroyed between the passes; its nearest living ancestor
        // keeps the enter/leave diff balanced (null would re-enter the
        // whole surviving chain).
        eventDiffBase = nullptr;
        for (uint64_t id : m_HoverEventDiffBaseChain)
        {
            if (UIElement* live = FindElementByInstanceId(id))
            {
                eventDiffBase = live;
                break;
            }
        }
        m_HoverEventDiffBaseChain.clear();
    }

    UIElement* newHover = nullptr;
    if (IsMousePositionKnown())
        newHover = m_MouseCaptured ? m_CaptureElement : HitTestTree(m_MouseX, m_MouseY);

    m_Hovered = newHover;
    m_HoveredInstanceId = newHover ? newHover->GetInstanceId() : 0;

    if (HoverTraceEnabled() && (prevHover != newHover || eventDiffBase != newHover))
        Logger::Log::Info("[HoverTrace] heavy hit {} -> {} (eventBase {}, prePassRan={} prePassMarked={})",
                          HoverTraceName(prevHover), HoverTraceName(newHover),
                          HoverTraceName(eventDiffBase), prePassRan, prePassMarked);
    // Post-solve: this frame's cascade already ran, so the hovered row's
    // resolved background tells whether the :hover rule was applied.
    if (HoverTraceEnabled() && newHover && newHover->HasClass("dropdown-item"))
        Logger::Log::Info("[HoverTrace] row {} post-solve bg=0x{:08X} styleDirty={} visualDirty={}",
                          HoverTraceName(newHover),
                          newHover->GetResolvedStyle().Visual.BackgroundColor,
                          newHover->IsDirty(UIElement::StyleDirty),
                          newHover->IsDirty(UIElement::VisualDirty));

    // --- hover chain diff (enter/leave events) ---
    DiffHoverChains(eventDiffBase, m_Hovered, ctx.hoverLeft, ctx.hoverEntered);

    // --- style marking (patch path) ---
    // If the pre-pass skipped marking (its m_StyleAnalysis snapshot was one
    // frame stale on a stylesheet-change frame), recover with the full
    // transition; otherwise patch only the post-solve delta.
    UIElement* markDiffBase = (prePassRan && !prePassMarked) ? eventDiffBase : prevHover;
    if (markDiffBase != m_Hovered)
        MarkHoverTransitionDirty(markDiffBase, m_Hovered, PseudoMarkMode::ResolveInPlace);

    // :active press/release edge — re-bake the pressed element in-frame so :active
    // applies (and clears) THIS frame, mirroring MarkHoverTransitionDirty above.
    // The button edge can occur with NO hover change (press-and-hold on an already-
    // hovered element), and the unchanged element is skipped by the subtree-skip
    // gate. Skipped when the pre-pass already marked the same target (its style
    // resolved in this frame's cascade); a different target here (hover moved
    // post-solve) still re-bakes.
    if (m_ButtonEdgeSinceLastUpdate &&
        (m_StyleAnalysis.Active.AffectsLayout || m_StyleAnalysis.Active.AffectsPaint))
    {
        if (UIElement* activeTarget = ActiveTarget())
        {
            if (activeTarget->GetInstanceId() != m_ActivePrePassTargetId)
            {
                unsigned flags = UIElement::StyleDirty | UIElement::VisualDirty;
                if (m_StyleAnalysis.Active.AffectsLayout)
                    flags |= UIElement::LayoutDirty;
                activeTarget->MarkDirty(flags);
                ReResolvePseudoStateElement(activeTarget);
            }
        }
    }
    m_ActivePrePassTargetId = 0;
}

// ---------------------------------------------------------------------------
// Shared pseudo-state marker (see the header for the drift it consolidates).
// ---------------------------------------------------------------------------

UIManager::DynamicStyleAnalysis::PseudoInfo UIManager::CombinePseudoInfo(
    const DynamicStyleAnalysis::PseudoInfo& a,
    const DynamicStyleAnalysis::PseudoInfo& b)
{
    DynamicStyleAnalysis::PseudoInfo r{};
    r.Any = a.Any || b.Any;
    r.AffectsLayout = a.AffectsLayout || b.AffectsLayout;
    r.AffectsPaint = a.AffectsPaint || b.AffectsPaint;
    r.MayAffectDescendants = a.MayAffectDescendants || b.MayAffectDescendants;
    r.MayAffectSiblings = a.MayAffectSiblings || b.MayAffectSiblings;
    r.MayAffectInheritedLayout = a.MayAffectInheritedLayout || b.MayAffectInheritedLayout;
    return r;
}

void UIManager::MarkPseudoStateScope(UIElement* el,
                                     const DynamicStyleAnalysis::PseudoInfo& info,
                                     std::uint16_t narrowBits)
{
    if (!el)
        return;
    // Nothing this pseudo touches can change style or geometry — the mark would
    // re-cascade to an identical result. Skip (subsumes the per-site guards the
    // call sites used to carry inline).
    if (!info.AffectsLayout && !info.AffectsPaint)
        return;

    unsigned flags = UIElement::StyleDirty | UIElement::VisualDirty;
    if (info.AffectsLayout)
        flags |= UIElement::LayoutDirty;

    // Cascade-memoization: when the rule cache drives the cascade,
    // m_MatchedPseudoStates records which pseudo-classes any rule that could
    // match the element references. An element with none of narrowBits set has
    // no rule whose match flips on this transition, so its mark is a safe skip.
    // Without the cache the predicate degenerates to "always mark".
    const bool narrow = UIParsing::CSSParser::IsRuleCacheEnabled();
    auto shouldMark = [narrow, narrowBits](UIElement* e) -> bool
    {
        return !narrow || (e->m_MatchedPseudoStates & narrowBits) != 0;
    };

    auto markSubtree = [&](UIElement* rootToMark)
    {
        if (!rootToMark)
            return;
        static thread_local std::vector<UIElement*> stack;
        stack.clear();
        stack.reserve(64);
        stack.push_back(rootToMark);
        while (!stack.empty())
        {
            UIElement* cur = stack.back();
            stack.pop_back();
            if (!cur)
                continue;
            if (shouldMark(cur))
                cur->MarkDirty(flags);
            for (const auto& ch : cur->GetChildren())
            {
                if (ch)
                    stack.push_back(ch.get());
            }
            if (auto* m = dynamic_cast<Mount*>(cur))
            {
                if (UIElement* tgt = m->GetTarget())
                    stack.push_back(tgt);
            }
        }
    };

    // A descendant/child combinator after the pseudo (`.x:focus-within .y`) can
    // restyle an off-chain descendant, so the whole subtree must re-cascade; a
    // sibling combinator (`.x:hover ~ .y`) reaches following siblings, marked by
    // walking the parent's subtree. Both fan-outs stay bounded by shouldMark.
    if (info.MayAffectDescendants)
        markSubtree(el);
    else if (shouldMark(el))
        el->MarkDirty(flags);

    if (info.MayAffectSiblings)
    {
        if (UIElement* p = el->GetParent())
            markSubtree(p);
    }
}

void UIManager::MarkCheckedStateScope(UIElement* el)
{
    MarkPseudoStateScope(el, m_StyleAnalysis.Checked, UIElement::CheckedRules);
}

void UIManager::MarkCustomStateScope(UIElement* el)
{
    MarkPseudoStateScope(el, m_StyleAnalysis.CustomState, UIElement::CustomStateRules);
}

// ---------------------------------------------------------------------------
// DispatchEvents: mouse enter/leave, keyboard/text input, pointer events,
// drag-drop, focus management, and post-event pseudo-state dirty marking.
// ---------------------------------------------------------------------------

void UIManager::DispatchEvents(UpdateContext& ctx)
{
    if (ctx.runInteractive)
    {
        GE_CPU_PROFILE_SCOPE("UIManager.Update.EventDispatch");
        ScopedSectionTimer _tEvents(ctx.profEnabled, &ctx.prof->EventDispatchMs);

        // Dispatch MouseEnter/MouseLeave events for elements whose hover state
        // changed this frame. These are *non-bubbling* by design: hoverEntered
        // / hoverLeft already encode ancestor chains, so each element should see
        // exactly one enter/leave per transition.
        for (UIElement* el : ctx.hoverLeft)
        {
            if (el)
                DispatchBubbleEvent(el, kEventMouseLeave, 0, /*bubble=*/false, ctx.treeMutatedDuringEvents, ctx.treeGenBeforeEvents);
        }
        for (UIElement* el : ctx.hoverEntered)
        {
            if (el)
                DispatchBubbleEvent(el, kEventMouseEnter, 0, /*bubble=*/false, ctx.treeMutatedDuringEvents, ctx.treeGenBeforeEvents);
        }

        // Keyboard and text events have already run. OnKey/OnChar dispatch
        // them synchronously from the platform callback, so their handlers
        // have been called and their consumption already reported to the
        // caller by the time this pass executes. Nothing is queued for us to
        // drain; retire the flag that brought this frame down the interactive
        // path so an idle frame can take a fast path again.
        m_KeyInputSinceLastUpdate = false;

        // Pointer events and focus management.
        {
            // If a drag session is active, update drop hover + previews based on the current hover chain.
            // The DragDropManager resolves the nearest IDropTarget ancestor and drives its preview state.
            if (m_DragDrop && m_DragDrop->IsDragging())
            {
                const int mods = m_Modifiers.Mask();

                // Use a fresh hit test at cursor for drop targeting so the drop target is always
                // correct regardless of capture/fast paths (e.g. bookmark drag to hierarchy).
                UIElement* dropLeaf = IsMousePositionKnown() ? HitTestTree(m_MouseX, m_MouseY) : nullptr;
                UIElement* hoverForDrop = dropLeaf ? dropLeaf : m_Hovered;
                m_DragDrop->UpdateHover(hoverForDrop, m_MouseX, m_MouseY, mods);
                if (!m_DragDropOverlay)
                    m_DragDropOverlay = std::make_unique<UI::Interaction::DragDropOverlay>();
                if (UIElement* root = GetRootElement())
                    m_DragDropOverlay->Update(root, *m_DragDrop, m_MouseX, m_MouseY);
            }
            else
            {
                if (m_DragDropOverlay)
                    m_DragDropOverlay->Hide();
            }

            // Mouse move: route to hovered or captured element. Press handlers
            // are guaranteed to have run first — OnMouseButton dispatches them
            // at the platform callback, which is strictly before this pass — so
            // drag logic can no longer race ahead of click logic on the press
            // (which is what would break Cmd/Alt+Click multi-cursor). The one
            // remaining skip is for TryPointerOnlyFrame having already
            // dispatched this frame's mouse move before escalating here.
            if (!m_PointerMoveDispatchedThisFrame)
            {
                UIElement* tgt = nullptr;
                if (m_MouseCaptured)
                {
                    tgt = m_CaptureElement;
                    if (!tgt && !m_CaptureId.empty())
                    {
                        if (auto* r = GetRootElement())
                            tgt = r->FindById(m_CaptureId);
                    }
                }
                if (!tgt)
                    tgt = m_Hovered;
                if (tgt)
                {
                    DispatchBubbleEvent(tgt, kEventMouseMove, 0, /*bubble=*/true, ctx.treeMutatedDuringEvents, ctx.treeGenBeforeEvents);
                }
            }
            m_PointerMoveDispatchedThisFrame = false;

        }
    }

    // Catch any direct tree mutations that occurred during event dispatch paths that
    // did not run the scheduler/dispatcher drains (e.g., keyboard input to focused controls).
    if (!ctx.treeMutatedDuringEvents) { if (m_TreeStructureGeneration.load(std::memory_order_relaxed) != ctx.treeGenBeforeEvents) ctx.treeMutatedDuringEvents = true; }

    // If focus/active pseudo state changed during event dispatch and those pseudo
    // rules can affect layout or paint, mark affected scopes dirty so the next
    // frame can recompute styles and (if needed) run Yoga.
    if (!ctx.treeMutatedDuringEvents)
    {
        auto findElementById = [this](const std::string& id) -> UIElement*
        {
            if (id.empty() || !GetRootElement())
                return nullptr;
            return GetRootElement()->FindById(id);
        };

        // The narrow bit-mask each transition passes to MarkPseudoStateScope:
        // an element no rule matches for these pseudos can't change on the
        // transition and is skipped when the rule cache drives the cascade.
        constexpr std::uint16_t kFocusBits =
            UIElement::FocusRules | UIElement::FocusVisibleRules | UIElement::FocusWithinRules;
        constexpr std::uint16_t kFocusVisibleBits =
            UIElement::FocusVisibleRules | UIElement::FocusWithinRules;
        constexpr std::uint16_t kActiveBits = UIElement::ActiveRules;

        // A focus-target change flips :focus and :focus-visible together on the
        // gaining/losing element — consult their combined analysis.
        const auto focusInfo = CombinePseudoInfo(m_StyleAnalysis.Focus, m_StyleAnalysis.FocusVisible);
        const bool focusTargetChanged = (m_FocusId != ctx.focusIdForYoga);
        const bool focusVisibleChanged = (m_FocusViaKeyboard != ctx.focusViaKeyboardForYoga);

        if (focusTargetChanged && (focusInfo.AffectsLayout || focusInfo.AffectsPaint))
        {
            MarkPseudoStateScope(findElementById(ctx.focusIdForYoga), focusInfo, kFocusBits);
            MarkPseudoStateScope(findElementById(m_FocusId), focusInfo, kFocusBits);
        }
        else if (focusVisibleChanged &&
                 (m_StyleAnalysis.FocusVisible.AffectsLayout || m_StyleAnalysis.FocusVisible.AffectsPaint))
        {
            MarkPseudoStateScope(findElementById(m_FocusId), m_StyleAnalysis.FocusVisible, kFocusVisibleBits);
        }

        // Active (mouse down/up)
        if (m_StyleAnalysis.Active.AffectsLayout || m_StyleAnalysis.Active.AffectsPaint)
        {
            UIElement* activeTargetNow = m_MouseDown ? ActiveTarget() : nullptr;
            if (activeTargetNow != ctx.activeTargetForYoga)
            {
                MarkPseudoStateScope(ctx.activeTargetForYoga, m_StyleAnalysis.Active, kActiveBits);
                MarkPseudoStateScope(activeTargetNow, m_StyleAnalysis.Active, kActiveBits);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Focus change notification (FocusIn / FocusOut events)
// ---------------------------------------------------------------------------

void UIManager::NotifyFocusChange()
{
    if (m_LastFocusIdNotified == m_FocusId)
        return;

    {
        ScopedUpdateEventDispatchGuard dispatchGuard;
        // The FocusOut target is the element that heard FocusIn, wherever it is now: a
        // panel unmounted since then is unreachable from the root but still alive, and its
        // blur-commit handler is owed this event.
        UIElement* prevEl = FocusNotifiedElement();
        m_FocusNotifiedInstanceId = 0;
        if (prevEl)
            DispatchFocusEvent(*prevEl, kEventFocusOut);
        if (!m_FocusId.empty() && GetRootElement())
        {
            if (UIElement* newEl = GetRootElement()->FindById(m_FocusId))
            {
                m_FocusNotifiedInstanceId = newEl->GetInstanceId();
                DispatchFocusEvent(*newEl, kEventFocusIn);
            }
        }
    }

    m_LastFocusIdNotified = m_FocusId;
}

UIElement* UIManager::FocusNotifiedElement() const
{
    if (m_FocusNotifiedInstanceId == 0)
        return nullptr;
    const auto it = m_ElementsByInstanceId.find(m_FocusNotifiedInstanceId);
    return it != m_ElementsByInstanceId.end() ? it->second : nullptr;
}

void UIManager::ReleaseFocusInDoomedSubtree(const UIElement* doomedRoot)
{
    if (!doomedRoot || (m_FocusId.empty() && m_FocusNotifiedInstanceId == 0))
        return;

    if (UIElement* notified = FocusNotifiedElement(); notified && IsInOwnedSubtree(notified, doomedRoot))
    {
        // Cleared before the dispatch so a handler that moves focus elsewhere keeps it:
        // the next NotifyFocusChange announces that element FocusIn.
        m_FocusNotifiedInstanceId = 0;
        m_LastFocusIdNotified.clear();
        DispatchFocusEvent(*notified, kEventFocusOut);
    }

    // After the dispatch, so a handler that focused something else in the same doomed
    // subtree does not leave the id naming it.
    if (!m_FocusId.empty() && m_Root)
    {
        const UIElement* focused = m_Root->FindById(m_FocusId);
        if (focused && IsInOwnedSubtree(focused, doomedRoot))
            m_FocusId.clear();
    }
}

// ---------------------------------------------------------------------------
// Tab focus order construction
// ---------------------------------------------------------------------------

void UIManager::BuildFocusOrder(UpdateContext& /*ctx*/)
{
    m_FocusOrder.clear();

    struct FocusCandidate
    {
        std::string id;
        int tabIndex;
        size_t domOrder;
    };

    static thread_local std::vector<FocusCandidate> focusCandidates;
    focusCandidates.clear();

    // DFS tree walk gathering focusable elements. Skips display:none and
    // disabled subtrees: neither participates in focus, and a disabled
    // container disables what it contains.
    size_t domOrder = 0;
    std::function<void(UIElement*)> visit = [&](UIElement* el) {
        if (!el)
            return;
        const ResolvedStyle& rs = el->GetResolvedStyle();
        if (rs.Layout.DisplayMode == DisplayMode::None)
            return;  // entire subtree excluded
        if (!el->IsEnabled())
            return;  // entire subtree excluded
        if (rs.Visual.Visible && el->IsFocusable()
            && el->GetTabIndex() >= 0)
        {
            std::string id = el->GetId();
            if (id.empty())
                id = EnsureElementId(el);
            focusCandidates.push_back({id, el->GetTabIndex(), domOrder});
        }
        ++domOrder;
        for (auto& ch : el->GetChildren())
            visit(ch.get());
        if (UIElement* tgt = el->GetMountTarget())
            visit(tgt);
    };
    visit(GetRootElement());

    std::stable_sort(focusCandidates.begin(), focusCandidates.end(),
        [](const FocusCandidate& a, const FocusCandidate& b)
        {
            const auto rankA = (a.tabIndex > 0) ? 0 : 1;
            const auto rankB = (b.tabIndex > 0) ? 0 : 1;
            if (rankA != rankB) return rankA < rankB;
            if (rankA == 0 && a.tabIndex != b.tabIndex) return a.tabIndex < b.tabIndex;
            return a.domOrder < b.domOrder;
        });

    for (const auto& c : focusCandidates)
        m_FocusOrder.push_back(c.id);
}

// ---------------------------------------------------------------------------
// Layout solve + absolute position computation + post-layout convergence
// ---------------------------------------------------------------------------

void UIManager::SolveAndApplyLayout(YGNode*& rootNode, UpdateContext& ctx, bool needSolve)
{
    if (needSolve)
    {
        GE_CPU_PROFILE_SCOPE("UIManager.Update.YogaSolve");
        ScopedSectionTimer _tYoga(ctx.profEnabled, &ctx.prof->YogaMs);
        UILayout::YogaAdapter::CalculateLayout(rootNode, static_cast<float>(ctx.viewportW), static_cast<float>(ctx.viewportH));
        ctx.didSolveLayout = true;
    }

    if (ctx.didSolveLayout)
    {
        // Copy Yoga's solved layout back onto each UIElement, routing rect
        // changes to the primitive drain (elements without a slot range
        // escalate to full regen — the drain can't allocate slots).
        CommitLayoutRects(GetRootElement(), /*incremental=*/true);

        // Override-rect elements moved via the position-only fast path keep
        // stale PositionLeft/Top in their Yoga styles, so the write-back above
        // just snapped their committed rects to those stale values. Re-patch
        // immediately: OnPostLayout handlers (ConvergePostLayout) and this
        // frame's hover/hit-testing must see the authored rects, not Yoga's.
        ApplyLayoutOverrideRects(ctx, /*allOverrides=*/true);
    }

    ConvergePostLayout(rootNode, ctx);
}
