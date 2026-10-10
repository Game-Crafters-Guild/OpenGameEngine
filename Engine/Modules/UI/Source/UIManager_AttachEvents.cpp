// Attach/detach events: kEventAttachedToPanel / kEventDetachedFromPanel.
//
// The mechanism in one paragraph. Every element remembers ONE bit — the last attach
// state its subscribers were told. Mutation sites do not dispatch; they enqueue the top
// of the subtree whose root-reachability may have moved. Once per frame the manager
// walks each queued subtree and dispatches only where an element's CURRENT reachability
// differs from that bit. Edge-triggered, so it is self-correcting rather than dependent
// on every mutation site being paired, and a detach-and-reattach that completes inside
// one frame is not a transition at all.
//
// That last property is the reason for the whole design. UIHotReload::ReconcileChildren
// TakeChild-s EVERY child of the reconciled parent before it matches any of them, then
// re-adds the ones the template still wants with their instance ids, handler tables and
// subscriptions intact. Dispatching at the unlink would therefore fire a detach/attach
// pair on every preserved element on every .uxml save — the spurious pair a reuse must
// not produce. Settling against the last state dispatched removes it by construction,
// with no reconcile-specific special case anywhere.

#include "UI/UIManager.h"

#include "UI/Internal/AttachDetachInternal.h"
#include "UI/Internal/AttachStateAccess.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UIManager_Internal.h"

#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"

#include <cassert>
#include <set>
#include <string>
#include <typeinfo>
#include <cstdlib>
#include <vector>

namespace GameEngine
{

namespace
{

// Raised across the whole settle so a handler's RemoveChild defers exactly as it does
// inside any other dispatch (UIElement::RemoveChild -> PostSafeAction) instead of
// mutating a child list the walk is standing in.
struct ScopedAttachSettleDispatchGuard
{
    ScopedAttachSettleDispatchGuard() { UIElement::SetInEventDispatch(true); }
    ~ScopedAttachSettleDispatchGuard() { UIElement::SetInEventDispatch(false); }
};

// "Attached" — owner-registered AND reaching that owner's root. The same two conjuncts
// UIManager::FindElementByInstanceId resolves through (an O(1) index probe plus
// UIElementReachesRoot), which is why an attached element is exactly one the scripting
// ABI can find and GE_UIElement_IsAlive reports true for.
//
// Asked against the element's OWN owner, not the settling manager: a subtree moved
// between managers is judged by where it ended up, so whichever manager's queue holds
// the entry reaches the same verdict.
bool IsAttachedNow(const UIElement* el)
{
    if (!el)
        return false;
    const UIManager* owner = el->GetOwnerManager();
    // An element re-owned by a manager that died inside the same frame has no owner here:
    // ~UIManager detaches every element that outlives it.
    return owner != nullptr && !owner->IsBeingDestroyed() &&
           UIElementReachesRoot(el, owner->GetRootElement());
}

// Per-settle dispatch counters, live only under GE_UI_ATTACH_LOG. What they answer is the
// question a .uxml save raises: how many elements did this reload actually announce, and
// how many did it preserve silently. "=2" also names each element, which is what turns a
// wrong count into a diagnosis. UI update is single-threaded, so plain statics.
const bool g_AttachLogLevel1 = []
{
    const char* e = std::getenv("GE_UI_ATTACH_LOG");
    return e && (e[0] == '1' || e[0] == '2');
}();
const bool g_AttachLogLevel2 = []
{
    const char* e = std::getenv("GE_UI_ATTACH_LOG");
    return e && e[0] == '2';
}();
std::size_t g_AttachedDispatched = 0;
std::size_t g_DetachedDispatched = 0;

void DispatchAttachEvent(UIElement& el, bool attached)
{
    if (g_AttachLogLevel1)
    {
        (attached ? g_AttachedDispatched : g_DetachedDispatched) += 1;
        if (g_AttachLogLevel2)
        {
            Logger::Log::Info("[UI Attach] {} id='{}' instance={}", attached ? "attach" : "detach",
                              el.GetId(), el.GetInstanceId());
        }
    }

    UIEvent e{};
    e.Id = attached ? kEventAttachedToPanel : kEventDetachedFromPanel;
    e.Target = &el;
    e.CurrentTarget = &el;
    if (attached)
        el.DispatchEvent<kEventAttachedToPanel>(e);
    else
        el.DispatchEvent<kEventDetachedFromPanel>(e);
}

// `child` when the walk may inherit `parent`'s verdict for it, null otherwise. Inheritance
// is only sound down edges the reachability predicate itself follows, and the predicate
// follows GetDfsParent(): an element that is some Mount's target hangs off THAT Mount in
// DFS terms even while it is still someone's child, so it takes the Mount's verdict and
// is reached through the Mount's own walk (or its own queue entry), never through the
// child link.
UIElement* DescendableChild(const UIElement* parent, UIElement* child)
{
    if (!child || child->GetDfsParent() != parent)
        return nullptr;
    // ...and never across an OWNERSHIP boundary. The verdict is computed once at the
    // queued root and inherited down, but IsAttachedNow asks each element's OWN owner for
    // its root — so inheriting into a subtree owned by a different manager would announce
    // an element attached while its own predicate says otherwise, and while neither
    // manager's FindElementByInstanceId can resolve it. That breaks the stated contract
    // that "attached" is exactly what the ABI resolves.
    //
    // This is reachable only transiently: Mount::SetTarget re-owns a foreign target when
    // the Mount already has an owner (Mount.h:102-103), and Mount::OnPostLayout adopts one
    // that was mounted before the Mount itself was parented (Mount.h:198-213). So the
    // frame after the adoption, the owner changes, enqueues, and announces the attach
    // itself — the truthful answer, from the manager that actually owns the element.
    return (child->GetOwnerManager() == parent->GetOwnerManager()) ? child : nullptr;
}

// Collect a queued subtree in PRE-ORDER, before anything is dispatched.
//
// Nothing runs a handler here, and that separation is the whole point. Handlers are free
// to mutate the tree — AddChild and InsertChild are NOT deferred during dispatch, only
// removal is — and an insert at a low index shifts every later sibling along. A walk that
// read the live child vector while dispatching would then skip a sibling outright, and a
// skip has no backstop: the element is already in the tree, so nothing re-queues it and
// its transition is lost rather than deferred. Nor is freezing the count per level enough,
// because a DESCENDANT's handler can mutate an ancestor's list while that ancestor's loop
// is still on the stack. Collecting the whole subtree first means handler mutations cannot
// perturb the traversal at all, which is the same "one generation, snapshotted up front"
// rule the queue drain already follows. Elements a handler adds are announced next frame,
// through their own enqueue.
void CollectSubtree(UIElement* el, std::vector<UIElement*>& out)
{
    if (!el)
        return;
    out.push_back(el);
    for (const std::unique_ptr<UIElement>& child : el->GetChildren())
        CollectSubtree(DescendableChild(el, child.get()), out);
    // A Mount's target is not among its children, but its DFS-parent edge routes through
    // the Mount, so it belongs to this subtree — the portal case GetDfsParent() expresses,
    // which the walk must follow or a mounted subtree never hears anything.
    CollectSubtree(DescendableChild(el, el->GetMountTarget()), out);
}

} // namespace

void UIManager::EnqueueAttachSettle(UIElement* el)
{
    if (!el)
        return;
    // Already queued — here or in another manager — is left exactly as it is. Queued in
    // ANOTHER manager means that settle will judge it, and it asks the element's own owner
    // for its root, so it reaches the same verdict this one would. Queued HERE means it is
    // already waiting, which is what holds a reconcile's TakeChild-then-AddChild to one
    // entry per element rather than two.
    //
    // Only the QUEUE slot is consulted: whether some manager is walking this element right
    // now is a different relationship and must not suppress a queue entry.
    if (UIAttachStateAccess::IsQueued(*el))
        return;
    UIAttachStateAccess::MarkQueued(*el, this);
    m_AttachSettleQueue.push_back(el);
}

void UIManager::ForgetAttachSettle(UIElement* el)
{
    if (!el)
        return;

    // Tombstone rather than erase: the settle indexes into both vectors while handlers
    // run, and a handler is allowed to destroy elements the settle still refers to.
    //
    // EVERY slot, not the first: an element can legitimately hold two queue slots for the
    // length of one settle. The drain clears the queued bit as it consumes the entry,
    // precisely so a handler that re-mutates that subtree can queue it again for the next
    // generation — leaving the consumed slot and the fresh one both pointing here until
    // the drain erases the consumed generation at the end. Stopping at the first match
    // would null the spent slot and leave the live one dangling at a freed element.
    if (UIAttachStateAccess::QueueOwner(*el) == this)
    {
        for (UIElement*& slot : m_AttachSettleQueue)
        {
            if (slot == el)
                slot = nullptr;
        }
        UIAttachStateAccess::ClearQueued(*el);
    }
    // The in-flight snapshot is the same contract: it holds raw pointers across handler
    // calls, so an element dying mid-settle has to erase itself from it too. This is the
    // liveness half of the snapshot walk — the element leaves the registry the walk
    // revalidates against, the way ManagedElementTypes' StillIndexed pass works.
    if (UIAttachStateAccess::WalkOwner(*el) == this)
    {
        for (UIElement*& slot : m_AttachSettleWalk)
        {
            if (slot == el)
                slot = nullptr;
        }
        UIAttachStateAccess::ClearWalking(*el);
    }
}

// A manager's death detaches the elements it announced but does NOT own — an externally
// owned Mount target is the ordinary case. They are still alive and will stay alive; what
// they lose is their root, which is an ordinary detach and the last one anybody can give
// them. The destruction dispatch above cannot cover them: it walks what this manager owns,
// and deliberately does not condemn a Mount target it does not own.
//
// Runs while the instance-id index holds and the manager is not yet marked as being
// destroyed — a managed handler resolves through both — and iterates a SNAPSHOT, because a handler is free to destroy
// elements and that erases entries from the map underneath us.
void UIManager::AnnounceDetachForSurvivingElements()
{
    if (m_ElementsByInstanceId.empty())
        return;

    std::vector<UIElement*> survivors;
    survivors.reserve(m_ElementsByInstanceId.size());
    for (const auto& entry : m_ElementsByInstanceId)
    {
        // The owned tree already had its say a moment ago, which cleared its bits; what is
        // left set belongs to something this manager announced but is not destroying.
        if (entry.second && UIAttachStateAccess::AttachStateDispatched(*entry.second))
            survivors.push_back(entry.second);
    }
    if (survivors.empty())
        return;

    ScopedAttachSettleDispatchGuard dispatchGuard;
    for (UIElement* el : survivors)
    {
        // Re-read rather than trust the snapshot: an earlier handler may have destroyed or
        // re-homed this one. A destroyed element leaves the index, so ask the index.
        auto it = m_ElementsByInstanceId.find(el->GetInstanceId());
        if (it == m_ElementsByInstanceId.end() || it->second != el)
            continue;
        if (!UIAttachStateAccess::AttachStateDispatched(*el))
            continue;
        UIAttachStateAccess::SetAttachStateDispatched(*el, false);
        DispatchAttachEvent(*el, false);
    }
}

void UIManager::ForgetAttachStateOnTeardown()
{
    // Teardown only, and it has to cover TWO populations.
    //
    // (1) Everything this manager still refers to. Elements can outlive their manager (a
    // Mount target it does not own, a stack-allocated element in a test), and by the time
    // ~UIElement runs this manager is already marked as being destroyed, so the forwarder
    // would decline to touch these vectors and the back-pointer would dangle.
    for (std::vector<UIElement*>* vec : {&m_AttachSettleQueue, &m_AttachSettleWalk})
    {
        for (UIElement* el : *vec)
        {
            if (el)
            {
                if (UIAttachStateAccess::QueueOwner(*el) == this)
                    UIAttachStateAccess::ClearQueued(*el);
                if (UIAttachStateAccess::WalkOwner(*el) == this)
                    UIAttachStateAccess::ClearWalking(*el);
            }
        }
        vec->clear();
    }

    // (2) Every element this manager announced as ATTACHED. A manager's death is NOT a
    // detach — nothing is dispatched here, because subscribers of a dying tree have no
    // safe way to touch it — but the dispatched-state bit records what subscribers were
    // told, and leaving it true on an element that SURVIVES makes the next manager's
    // settle read "no change" and stay silent when that element is genuinely re-attached.
    // A panel undocked to a floating window, then re-docked after that window closes, is
    // the live shape. The instance-id index is exactly the set of elements this manager
    // owns — Mount targets included, since SetTarget propagates the owner, which registers
    // them.
    for (const auto& entry : m_ElementsByInstanceId)
    {
        if (!entry.second)
            continue;
#if !defined(NDEBUG) || defined(GE_DEV_DIAG)
        // The detector for the shape ~UIElement's cannot see. That one fires when an
        // element is DESTROYED still believing it is attached; this one fires when an
        // element is never destroyed at all — it outlives this manager, so the only moment
        // anyone could tell it was a moment ago, in AnnounceDetachForSurvivingElements.
        // A bit still set here means that pass missed it.
        if (UIAttachStateAccess::AttachStateDispatched(*entry.second))
        {
            // Keyed like the other detector: one line per element type, so a second,
            // unrelated survivor later in the session is still reported.
            static std::set<std::string> reported;
            if (reported.emplace(typeid(*entry.second).name()).second)
            {
                Logger::Log::Warning(
                    "UI: element id='{}' (instance {}) outlives its UIManager still believing "
                    "it is ATTACHED — AnnounceDetachForSurvivingElements did not reach it, so "
                    "its subscribers never hear the detach. Reported once per element type.",
                    entry.second->GetId(), entry.second->GetInstanceId());
            }
        }
#endif
        UIAttachStateAccess::SetAttachStateDispatched(*entry.second, false);
    }
}

void UIManager::SettleAttachTransitions()
{
    // The steady state: a static tree queues nothing, and the whole mechanism costs one
    // empty-vector test per frame.
    if (m_AttachSettleQueue.empty())
        return;

    GE_CPU_PROFILE_SCOPE("UIManager.Update.AttachSettle");

    // ONE GENERATION PER FRAME. Everything queued when the settle starts is announced
    // now; anything a handler queues by mutating the tree lands past this boundary and
    // is announced next frame. That bounds the settle structurally — it cannot loop on a
    // handler that re-mutates on every attach — at the price of one frame of latency,
    // the same latency PostSafeAction already has and documents. A handler therefore
    // never sees its own edits reflected in a nested settle; there is no nesting.
    const std::size_t generationEnd = m_AttachSettleQueue.size();

    {
        ScopedAttachSettleDispatchGuard dispatchGuard;
        for (std::size_t i = 0; i < generationEnd; ++i)
        {
            UIElement* root = m_AttachSettleQueue[i];
            if (!root)
                continue; // tombstoned: destroyed, or forgotten, since it was queued
            // Consume the entry before dispatching, so a handler that re-mutates this
            // same subtree can queue it again for the next generation.
            UIAttachStateAccess::ClearQueued(*root);
            DispatchSettledSubtree(root, IsAttachedNow(root));
        }
    }

    m_AttachSettleQueue.erase(m_AttachSettleQueue.begin(),
                              m_AttachSettleQueue.begin() + static_cast<std::ptrdiff_t>(generationEnd));

    if (g_AttachLogLevel1 && (g_AttachedDispatched || g_DetachedDispatched))
    {
        Logger::Log::Info("[UI Attach] settled roots={} attach={} detach={} deferred={}",
                          generationEnd, g_AttachedDispatched, g_DetachedDispatched,
                          m_AttachSettleQueue.size());
        g_AttachedDispatched = 0;
        g_DetachedDispatched = 0;
    }
}

// Snapshot the subtree, then dispatch over the snapshot.
//
// `attached` is inherited rather than recomputed per node: root-reachability is a property
// of the DFS-parent chain, so every node under a queued root shares its verdict, and one
// O(depth) test at the root covers an O(n) subtree.
//
// ATTACH IS PRE-ORDER (a parent's handler finds its children already attached, which is
// what a HUD author assumes); DETACH IS THE REVERSE, so a teardown handler runs
// leaf-upward. Reversing a pre-order list gives exactly that — every child precedes its
// parent — and costs no second traversal.
//
// Each visit revalidates instead of trusting the snapshot: a handler can destroy an
// element the snapshot still names (an externally owned Mount target, a document the app
// drops), and ~UIElement tombstones it here on the way out. Reading liveness from the
// registry the destructor maintains, immediately before the touch, is what makes holding
// these pointers across handler calls sound rather than lucky.
void UIManager::DispatchSettledSubtree(UIElement* root, bool attached)
{
    // Nested settles do not happen — the drain raises the dispatch guard and consumes one
    // generation — but the walk buffer is a reused member, so a settle that somehow did
    // re-enter would corrupt it. An assert rather than a silent aliasing bug.
    assert(m_AttachSettleWalk.empty() && "attach settle walk buffer is not re-entrant");

    CollectSubtree(root, m_AttachSettleWalk);
    for (UIElement* el : m_AttachSettleWalk)
    {
        if (el)
        {
            // ONLY the walk slot. Writing the queue slot here would rename whichever
            // manager already had this element queued, and that manager's vector would
            // then keep a pointer no destructor will ever tombstone.
            UIAttachStateAccess::MarkWalking(*el, this);
        }
    }

    const std::size_t count = m_AttachSettleWalk.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        UIElement* el = m_AttachSettleWalk[attached ? i : count - 1 - i];
        if (!el)
            continue; // destroyed by a handler earlier in this same walk
        if (UIAttachStateAccess::AttachStateDispatched(*el) == attached)
            continue;
        UIAttachStateAccess::SetAttachStateDispatched(*el, attached);
        DispatchAttachEvent(*el, attached);
    }

    for (UIElement* el : m_AttachSettleWalk)
    {
        if (!el)
            continue;
        // The queue slot is untouched here by construction: it belongs to whichever
        // manager queued the element, which may not be this one.
        UIAttachStateAccess::ClearWalking(*el);
    }
    m_AttachSettleWalk.clear();
}

// ---- Destruction-detach ----------------------------------------------------------------
//
// The SECOND of this event's two timings, and the one that makes the Unity name honest:
// an element that is about to be destroyed is told so, synchronously, BEFORE anything is
// torn down.
//
// Why synchronous here when every other transition is queued and settled: a queued detach
// is announced next frame, and next frame this element does not exist. Destruction is the
// one transition that can never be elided or deferred, which is also why the two timings
// cannot conflict — the settle exists to swallow transient unlink/relink pairs, and a
// destruction is never transient.
//
// Why it is safe, stated as the property to preserve rather than as reassurance: THIS RUNS
// AT THE POINT DESTRUCTION IS DECIDED, NOT WHERE MEMORY IS RELEASED. Every element in the
// doomed subtree is fully alive — derived parts intact, handler tables intact, still
// resolvable through the owner's index — so this is an ordinary dispatch on a live tree
// that happens to be doomed. The use-after-free class this arc has fixed twice came from
// handlers running against half-destroyed objects, or from mutating a container mid-
// teardown; neither exists here. Dispatching from ~UIElement WOULD have both, which is
// exactly why this is not done there.
//
// Post-order, mirroring the settle's detach order: a child hears before its parent.
namespace
{

// One process-wide re-entrancy latch. A destruction dispatch must not nest: a handler that
// destroys something else mid-dispatch would interleave two doomed sets and invalidate the
// scratch buffer both are walking. UI is single-threaded, so a plain flag is the whole
// mechanism.
bool g_InDestructionDispatch = false;

void CollectPostOrder(UIElement* el, std::vector<UIElement*>& out)
{
    if (!el)
        return;
    for (const std::unique_ptr<UIElement>& child : el->GetChildren())
    {
        // The doomed set is what THIS subtree owns. A Mount target is not owned by its
        // host, so it outlives this destruction and is deliberately not condemned with it;
        // it loses reachability instead, which the settle announces the ordinary way.
        if (child)
            CollectPostOrder(child.get(), out);
    }
    out.push_back(el);
}

} // namespace

void UI::DispatchDestructionDetach(UIElement* doomedRoot)
{
    if (!doomedRoot)
        return;

    if (g_InDestructionDispatch)
    {
        // A handler destroyed something else from inside a destruction dispatch. Refused
        // rather than nested, and said out loud once per occurrence because it means user
        // code is tearing down a tree from inside a teardown notification.
        Logger::Log::Warning(
            "[UI Attach] destruction-detach refused: already dispatching a destruction. The "
            "inner subtree is destroyed WITHOUT announcing, because nesting two doomed sets "
            "would interleave their walks. Destroy from a PostAction instead.");
        return;
    }

    // Reused across calls rather than built per destruction: RemoveChild is a per-mutation
    // path and this ran an allocation on every one of them. Safe as a single buffer because
    // the re-entrancy latch above refuses a nested dispatch, so only one walk is ever in
    // flight; cleared, not shrunk, so the capacity survives into the next teardown.
    static std::vector<UIElement*> doomed;
    doomed.clear();
    CollectPostOrder(doomedRoot, doomed);

    // Condemn the whole set BEFORE dispatching, so a handler on the first element cannot
    // adopt or rescue anything anywhere in it.
    for (UIElement* el : doomed)
        UIAttachStateAccess::SetDoomed(*el, true);

    g_InDestructionDispatch = true;
    {
        // Same guard the settle raises: a handler's RemoveChild self-defers to the
        // dispatcher rather than mutating a child list mid-walk. The deferred action is
        // liveness-guarded on the element that posted it (UIElement::PostSafeAction), so a
        // removal posted against something in this doomed set is dropped rather than run
        // against freed memory.
        ScopedAttachSettleDispatchGuard dispatchGuard;
        // Focus first: a blur-commit handler reads the control and the panel state around
        // it, which the detach handlers below may already be tearing down.
        UIManager* owner = doomedRoot->GetOwnerManager();
        if (owner && !owner->IsBeingDestroyed())
            owner->ReleaseFocusInDoomedSubtree(doomedRoot);
        for (UIElement* el : doomed)
        {
            // THE EDGE RULE STILL APPLIES. An element whose subscribers were never told
            // "attached" must not be told "detached" — a document built and destroyed
            // inside one frame, before any settle ran, is not a transition in either
            // direction. This is also what reconciles a destruction with a still-pending
            // queue slot for the same element: the slot's transition was never dispatched,
            // so there is no edge to close.
            if (!UIAttachStateAccess::AttachStateDispatched(*el))
                continue;
            UIAttachStateAccess::SetAttachStateDispatched(*el, false);
            DispatchAttachEvent(*el, false);
        }
    }
    g_InDestructionDispatch = false;

    // The flag stays SET on the way out. These elements are destroyed by the caller
    // immediately after this returns, and leaving them condemned keeps the refusals honest
    // for anything still holding a raw pointer to them in between.
}

// Internal forwarders used by UIElement / Mount (keeps their headers free of
// UIManager.h). Neither pointer dangles: an element's owner is detached by a dying
// manager, and its settle back-pointers are cleared by ForgetAttachStateOnTeardown.
// Enqueueing is a no-op while the manager is being destroyed, like the other forwarders.
void UI::Detail::EnqueueAttachSettle(UIManager* owner, UIElement* el)
{
    if (owner && el && !owner->IsBeingDestroyed())
        owner->EnqueueAttachSettle(el);
}

// Never reached with a manager that is being destroyed: ~UIManager clears every settle
// back-pointer into it (ForgetAttachStateOnTeardown) before it frees any element, and
// EnqueueAttachSettle above refuses new ones from then on.
void UI::Detail::ForgetAttachSettle(UIManager* owner, UIElement* el)
{
    if (!owner || !el)
        return;
    assert(!owner->IsBeingDestroyed() && "a settle back-pointer survived ForgetAttachStateOnTeardown");
    owner->ForgetAttachSettle(el);
}

} // namespace GameEngine
