#pragma once

// Element types defined outside C++ — today that means C#, and the UI module never learns
// so. A tag registered here mints a ManagedElementProxy: an ordinary UIElement that also
// holds an opaque handle to whatever object the owning language built for it.
//
// The whole of the difficulty is what happens when that owner goes away. A native module
// unload is something the engine PERFORMS, so it can decline it; a managed
// AssemblyLoadContext unload is something the runtime ANNOUNCES, and nothing native gets a
// veto. So the element cannot be kept valid and it must not be destroyed either — destroying
// it would delete authored children, layout and scroll position to fix a cosmetic problem.
// It is orphaned instead: the element stays in the tree, inert, still carrying its tag,
// classes, attributes and children, and a later registration of the same tag re-attaches a
// fresh instance to the SAME element.
//
// Three states, and the middle one is the reason the reload notification exists:
//
//   Live      the tag is registered and this element carries an instance
//   Pending   the owner unloaded; a reload is in flight and may bring the tag back
//   Faulted   a reload COMPLETED without the tag — deleted, renamed, or failed to compile
//
// Deciding "removed" from absence alone would report every reload as a removal for as long as
// it takes to run. Only NotifyReloadCompleted turns absence into a fact.
//
// REGISTRATION CLAIMS; THE VERDICT RECONCILES. Registering a tag touches no proxy at all — it
// claims the tag and returns its id, nothing more. Every proxy transition of a reload happens in
// a verdict, which is the point at which a registration batch is known to be complete: a tag
// that came back materializes its elements, a tag that did not faults them.
//
// A COMPLETED REGISTRATION BATCH IS ITSELF A VERDICT-WORTHY EVENT, so a verdict has two
// sources. The defining language announces one per context that unloaded, through
// NotifyReloadCompleted. The window announces one per owner that claimed a tag inside it, which
// is the case the first cannot reach: restoring a deleted type unloads a context that owned no
// element type and therefore announces nothing, while the owner claiming the tag back is brand
// new and is on nobody's list. Without the second source that element stays inert until some
// later, unrelated reload happens to announce one.
//
// The two overlap harmlessly. Reviving is not owner-filtered — an orphan rejoins whichever owner
// holds its tag now — so a second verdict over the same index finds nothing left to do; and
// faulting IS owner-filtered, so a batch's own verdict can only decide elements of the context
// that claimed it.
//
// The split is forced by the defining language, not chosen for tidiness. The managed binding
// resolves a Create callback through a tag id -> type table it can only fill with an id the
// registration call has already RETURNED. Materializing inside that call therefore asks the
// caller to build a tag it provably cannot name yet; it declines, and a declined Create is
// indistinguishable from a constructor that threw. Re-materializing at the verdict instead is
// the one ordering in which the caller is always ready.
//
// BOTH OF THOSE RUN IN A REGISTRATION WINDOW, AND THE WINDOW IS THE THREAD CONTRACT. Registration
// cannot marshal itself: it must return a tag id synchronously, and an id computed off-thread
// would be read out of the very map the claim writes. So the defining language does not call
// registration or the verdict from its reload thread at all. It stages the batch there, asks for
// a window (RequestRegistrationWindow), and the engine calls back into it from the main thread —
// where the whole claim-then-reconcile sequence runs in ONE slot, so no element construction can
// interleave between a tag being claimed and the caller being able to answer for it.
//
// The entry points refuse off-main rather than trusting that. A refusal is bounded, not
// permanent: any later verdict re-materializes a proxy whose tag is registered by then, whoever
// registered it, so a batch lost to a stalled reload comes back on the next one.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Types/StringId.h"
#include "UI/UIElement.h"

namespace GameEngine::Scripting
{

// How a proxy relates to its defining type right now.
enum class ManagedTypeState : std::uint8_t
{
    Live,
    Pending,
    Faulted,
};

// Calls out to the language that defined the type. Installed once from a context that is
// never unloaded, so the function pointers themselves cannot dangle across a reload — only
// the handles they hand back are perishable, and those are released at the unload seam.
struct ManagedElementCallbacks
{
    // Build the instance backing a freshly created proxy. Returns an opaque handle, or 0 if
    // the owning language declined or threw.
    std::intptr_t (*Create)(std::uint64_t tagId, std::uint64_t instanceId) = nullptr;
    // Apply one authored .uxml attribute to that instance.
    void (*ApplyAttribute)(std::intptr_t handle, const char* name, const char* value) = nullptr;
    // Release the handle. Called when a proxy is destroyed and when its type unloads.
    void (*Release)(std::intptr_t handle) = nullptr;
    // Perform whatever registrations and reload verdicts the owning language has staged. Invoked
    // only from the registration window — see RequestRegistrationWindow — so the language may
    // call the registration and verdict entry points synchronously from inside it.
    void (*PerformRegistrations)() = nullptr;
};

class ManagedElementTypes;

// The one native class behind every externally-defined element type. That they SHARE this
// class is exactly why element identity had to move off C++ RTTI onto the tag id: to the
// compiler every proxy is the same type, and only its stamp says whether it is a HealthBar
// or an AmmoCounter.
class ManagedElementProxy final : public UIElement
{
public:
    // displayTag is the tag as registered (original case). The proxy stamps BOTH inherited
    // members: the id, so it has an identity, and the name, so a document of managed elements
    // is readable in the UI-tree inspector and in XML export instead of every proxy reporting
    // whichever tag registered its shared C++ class last.
    ManagedElementProxy(StringId tagId, std::string_view displayTag, std::uint64_t owner);
    ~ManagedElementProxy() override;

    StringId TagId() const { return m_TagId; }
    std::uint64_t Owner() const { return m_Owner; }
    ManagedTypeState State() const { return m_State; }
    std::intptr_t Handle() const { return m_Handle; }

protected:
    // The authored attributes have landed, so the instance can be configured from them. This
    // runs on every application — first parse, document rebuild, and .uxml reconcile — which
    // is what keeps a hot-edited attribute reaching a live instance.
    void OnAuthoredAttributesApplied(
        const std::unordered_map<std::string, std::string>& attrsLower) override;

private:
    friend class ManagedElementTypes;

    // Push one name/value pair at the instance, if there is one to push.
    void ApplyAttributeToInstance(const std::string& name, const std::string& value) const;

    // Replay the element's CURRENT authored attributes at a newly constructed instance, which
    // knows nothing of what the document said about the element it is attaching to.
    //
    // Reads the inherited attribute map rather than a copy of its own. Every element builder
    // stamps that map before ApplyAttributes runs, so it is populated by the time any of this
    // is reachable — and on a .uxml reconcile it is the only one of the two that is right: the
    // map keeps attributes the reconciler deliberately PRESERVED on the live element, while the
    // set handed to the hook has them filtered out.
    void ApplyAuthoredAttributesToInstance() const;

    std::uint64_t m_Owner = 0;
    std::intptr_t m_Handle = 0;
    ManagedTypeState m_State = ManagedTypeState::Live;
};

class ManagedElementTypes
{
public:
    static ManagedElementTypes& Instance();

    // Badge class names. A style class rather than injected content on purpose: the orphan
    // policy exists to preserve layout, and adding a child to say so would move the very
    // children it is protecting. (There are no pseudo-elements in this UI module to put it
    // in instead — the CSS parser handles no `::` at all.)
    static constexpr const char* kOrphanedClass = "ge-orphaned-type";
    static constexpr const char* kMissingClass = "ge-missing-type";

    // Developer builds paint the badge; a shipping Player never does. GE_UI_ORPHAN_BADGE
    // overrides in either direction.
    static bool BadgeEnabled();

    // Force the badge on or off for the rest of the process, or nullopt to go back to the
    // build/env answer. Test seam: the env read is latched once, and the shipping-Player
    // behaviour has to be reachable from a developer build to be asserted at all.
    static void SetBadgeEnabledForTest(std::optional<bool> enabled);

    // FIRST-WINS. A second install is refused, logged and counted rather than applied: the
    // defining language installs these from whichever thread loaded it, while the main thread may
    // already be invoking them, and reassigning a live std::function frees the callable under its
    // caller. Refusing means nothing is ever reassigned, which is what makes the lock-free read
    // safe. Same rule for the marshaller and the predicate below.
    void SetCallbacks(const ManagedElementCallbacks& callbacks);
    const ManagedElementCallbacks& Callbacks() const { return m_Callbacks; }

    // Installs turned away because something was already installed. Observable so a second host
    // taking the callbacks over does not look like it worked.
    std::size_t RefusedReinstallCount() const { return m_RefusedReinstalls; }

    // How a TREE-VISIBLE transition reaches the UI thread. The orphan seam runs on whichever
    // thread called AssemblyLoadContext.Unload, and a badge class edit recomputes style, so
    // the class edits are queued through this rather than applied where they are decided.
    //
    // Injected rather than called: this policy compiles into Engine and is asserted by suites
    // that never load a CLR, while the only marshaller worth having — ScriptManager's
    // main-thread queue — is reachable only through the engine root singleton, which those
    // suites must not stand up. The managed ABI installs it at start-up.
    //
    // Left unset the transition applies INLINE, which is the right answer for a native-only
    // host: the off-thread caller this defers for exists solely behind that ABI.
    using MainThreadMarshaller = std::function<void(std::function<void()>)>;
    void SetMainThreadMarshaller(MainThreadMarshaller marshaller);

    // Answers "is the calling thread the one that owns the UI tree". Injected for the same
    // reason as the marshaller: the only authority on it is ScriptManager, which these suites
    // must not stand up, and the policy for a host that has no frame loop at all belongs at the
    // ABI rather than here.
    //
    // Left unset every thread is the main thread, which is the right answer for a native-only
    // host: the off-thread caller this guards against exists solely behind the managed ABI.
    using MainThreadPredicate = std::function<bool()>;
    void SetMainThreadPredicate(MainThreadPredicate predicate);

    // Ask for a main-thread slot in which the defining language may register types and close
    // reload verdicts. Safe from ANY thread — this is the one entry point a reload thread is
    // allowed to call, and the only thing it does is hand the window to the marshaller.
    //
    // When the slot runs, every owner that claimed a tag inside it is reconciled as the window
    // closes: the batch is provably complete the moment the callback returns, because the
    // defining language fills its tag id -> type table from these calls' return values.
    //
    // A no-op when no PerformRegistrations callback is installed, which is every native-only
    // host: nothing has staged anything to perform.
    void RequestRegistrationWindow();

    // Claim a tag for an externally-defined type. `owner` identifies the load context and MUST
    // be non-zero — owner 0 is the engine, whose registrations no caller can sweep. Returns the
    // tag id, or 0 if refused.
    //
    // Touches no proxy. An element already orphaned under this tag comes back at the owner's
    // NotifyReloadCompleted, not here — see the header note on why that ordering is forced.
    //
    // Returns 0 and logs if the tag is already registered by a different owner — including any
    // engine tag, which is what stops a managed type named "Button" from replacing the built-in
    // control process-wide and then deleting it when its context unloads.
    //
    // MAIN THREAD ONLY: claiming a tag writes the factory registry that document builds read, and
    // that registry has no synchronization. Called off-main the whole batch is refused (every id
    // 0) and the refusal is logged and counted — it is never performed on the wrong thread.
    StringId RegisterType(std::string_view tagName, std::uint64_t owner);

    // Claim several tags for one owner in one pass, returning one tag id per input and 0
    // wherever the tag was refused. Same rules as RegisterType, which is implemented in terms
    // of this.
    //
    // The C ABI registers one tag per call (GE_UI_RegisterElementType), so this is the C++
    // caller's form. It is no longer the cheaper of the two: with re-materialization moved to
    // the verdict, neither form walks the proxy index at all.
    std::vector<StringId> RegisterTypes(std::span<const std::string_view> tagNames,
                                        std::uint64_t owner);

    // Orphan every live proxy owned by `owner`, then unregister its factories. Returns the
    // number of proxies orphaned. Call this from the unload seam, before the owning context
    // actually goes: the handles have to be released while they are still valid.
    //
    // MAIN THREAD ONLY, same rule and same registry as RegisterType — unregistering factories is
    // the symmetric and larger write of the map document builds read. Called off-main it is
    // refused, logged and counted, and NOTHING is swept. Both halves of that survive for the rest
    // of the process, and the second is the worse one:
    //
    //   the instances stay reachable, so the load context can never be collected; AND
    //   the owner's TAGS stay claimed — m_OwnerByTagId and the factory registry are cleared only
    //   by the sweep this refusal skipped — so the type can never be registered again, by this
    //   context or by the reload that replaces it.
    //
    // That is deliberate. A refusal here means the unload seam was driven from the wrong thread,
    // and a loud, permanent failure is a better answer than tearing the registry out from under a
    // document build. It is not a degraded mode to be relied on.
    //
    // Only the BADGE is marshalled out of this call. The handle release and the Pending state
    // transition are inline and must be: the handle is valid only while the unload callback is on
    // the stack.
    std::size_t OrphanTypesOwnedBy(std::uint64_t owner);

    // The reload of ONE unloaded context finished: its registration batch is complete, so every
    // proxy it left behind can now be decided. A tag that came back re-attaches a fresh
    // instance to the SAME element; a tag that is still absent is now Faulted rather than
    // merely waiting. Before this point, absence only means the reload is still running.
    //
    // Per-owner, not global: with two collectible contexts loaded, one of them reloading says
    // nothing about the other, and faulting everything would paint a user-visible
    // missing-type badge on elements whose own assembly is merely still loading.
    //
    // MAIN THREAD ONLY, same rule and same reason as RegisterType: the reconcile it triggers
    // constructs instances and edits badge classes on live elements. Called off-main it is
    // refused, logged and counted, and the proxies stay as they are — a later verdict decides
    // them.
    //
    // Called from inside a registration window the reconcile runs INLINE, closing the window's
    // claim-then-decide sequence in the one main-thread slot. Called from a main-thread context
    // that is not a window — which is what a native host does — it is MARSHALLED as one task,
    // because the reconcile is tree-visible and the caller may be mid-frame. One task for the
    // owner rather than one per proxy or per tag: the main-thread queue's budget drops to a
    // single task per frame during a hot reload, which is exactly when this fires.
    void NotifyReloadCompleted(std::uint64_t owner);

    // What a refused tag claim found: who holds the tag, and whether the engine still considers
    // that holder's load context live.
    //
    // A LEAKED CONTEXT READS AS LIVE, and that is the honest answer rather than a gap: nothing
    // released the owner because nothing unloaded. It is also the diagnostic one — read next to
    // the runtime's own ALC-leak warning it identifies a tag that no unload will ever free, which
    // is a permanently poisoned tag rather than a transient collision.
    struct TagClaimRefusal
    {
        std::uint64_t Holder = 0;
        bool HolderContextLive = false;
    };

    // Tag claims refused because someone else holds the tag, and what the last one found.
    // Counted rather than only logged for the same reason owner refusals are: the log is
    // deduplicated and bounded, so a count is the half that stays true.
    std::size_t RefusedTagClaimCount() const { return m_RefusedTagClaims; }
    const std::optional<TagClaimRefusal>& LastRefusedTagClaim() const
    {
        return m_LastRefusedTagClaim;
    }

    // Test seams.
    std::size_t ProxyCount() const { return m_Proxies.size(); }
    std::size_t CountProxiesInState(ManagedTypeState state) const;
    std::size_t CountTypesOwnedBy(std::uint64_t owner) const;

    // How many calls the off-main-thread guard has turned away. A return code alone leaves a
    // refused batch looking exactly like one that had nothing to register.
    std::size_t RefusedOffMainThreadCount() const { return m_RefusedOffMainThread; }

    // Drop every registration this object made. Test-only: the registry is a process
    // singleton and a suite that left types behind would leak them into the next test.
    //
    // Deliberately does NOT touch the proxy index: proxies are owned by whoever holds the
    // elements, and forgetting them here would leave live elements calling into an index that
    // no longer knows them. Destroy the elements to empty the index.
    void ResetForTests();

private:
    friend class ManagedElementProxy;

    void NoteProxy(ManagedElementProxy* proxy);
    void ForgetProxy(ManagedElementProxy* proxy);

    // Hand the defining language its slot, then close the batch it registered by reconciling
    // every owner that claimed a tag inside it. The body of RequestRegistrationWindow, split out
    // because it runs on the main thread rather than where the window was asked for.
    void RunRegistrationWindow();

    // Claim one tag for one owner. The whole of what registration does.
    StringId ClaimTag(std::string_view tagName, std::uint64_t owner);

    // Decide every proxy this owner left behind, against the tags that are registered NOW.
    // The body of NotifyReloadCompleted, split out because it runs a frame later on the main
    // thread rather than where the verdict was announced.
    void Reconcile(std::uint64_t owner);

    void Materialize(ManagedElementProxy& proxy);

    // Release the instance and mark the proxy Pending. The INLINE half of the transition, and
    // only that: the handle is valid solely during the unload callback, so it cannot wait for a
    // frame. The badge is tree-visible and goes through ApplyPendingBadges instead.
    void Orphan(ManagedElementProxy& proxy);

    // The tree-visible half, re-derived from the LIVE index rather than from anything the
    // orphan pass captured — it runs a frame later, by which time those proxies may be gone or
    // may have re-materialized. Badges whatever is still Pending under this owner.
    void ApplyPendingBadges(std::uint64_t owner);

    // Run a tree-visible task through the marshaller, or inline when none is installed.
    //
    // Also inline when this thread is ALREADY executing a marshalled task: that task is on the
    // main thread by definition, and re-queuing from inside it would push the work a further
    // frame out and break the registration window's one-slot guarantee. The flag is
    // thread_local, so an unload seam running concurrently on another thread still marshals.
    void RunOnMainThread(std::function<void()> task);

    // Is this thread allowed to touch the registry and the tree? Unset predicate means yes.
    bool OnMainThread() const;

    // Record and log a call that arrived on the wrong thread. `what` names the entry point.
    void RefuseOffMainThread(const char* what, const char* remedyAndConsequence);

    // Record and log a second installation of something that may only be installed once.
    void RefuseReinstall(const char* what);

    void SetBadge(ManagedElementProxy& proxy, ManagedTypeState state);

    // One line per tag per transition, so a document with fifty orphaned HealthBars says so
    // once. Cleared for a tag when it comes back, so a second unload is reported again.
    bool ShouldLogOnce(StringId tagId, ManagedTypeState state);
    void ClearLogState(StringId tagId);

    // Record and report a claim the factory registry turned away, naming the holder and what is
    // known about its context. Deduplicated per (tag, holder) and capped, exactly as owner
    // refusals are: a reload retries the whole batch, so an unbounded report would repeat the
    // same line every reload for the rest of the session.
    void NoteRefusedTagClaim(StringId tagId, const std::string& tagLower,
                             const std::string& display, std::uint64_t claimant);

    // Distinct (tag, holder) pairs this process will name in the log.
    static constexpr std::size_t kMaxReportedTagRefusals = 64;

    // The proxies to act on, captured before any callback runs. Callbacks re-enter this index,
    // so a walk that held an iterator into it would skip entries or dangle.
    std::vector<ManagedElementProxy*> SnapshotProxies() const;

    // Is this snapshot entry still a live proxy? Pointer identity, which means it is ABA-
    // tolerant rather than ABA-proof: a proxy destroyed mid-walk and a new one allocated at
    // the same address answers true.
    //
    // That is safe here, and only because of how the callers use it. Every condition a walk
    // then tests — m_Owner, m_State, m_TagId — is re-read from the live object AFTER this
    // returns, never carried in the snapshot. So an entry that changed identity is judged as
    // whatever now occupies the address, which is the same decision the walk would have
    // reached had it seen that object in the first place. A caller that cached any proxy
    // state alongside the pointer would break this, and would need a generation counter.
    bool StillIndexed(ManagedElementProxy* proxy) const { return m_Proxies.count(proxy) != 0; }

    ManagedElementCallbacks m_Callbacks{};
    MainThreadMarshaller m_Marshaller;
    MainThreadPredicate m_MainThreadPredicate;
    std::size_t m_RefusedOffMainThread = 0;
    std::size_t m_RefusedReinstalls = 0;
    // Is a registration window on the stack, and which owners have claimed a tag inside it.
    // Claims made outside a window are not collected: a native host that never installs a
    // PerformRegistrations callback would otherwise grow this list with nothing to drain it.
    //
    // A plain vector because a window carries one owner in the ordinary case and a handful in
    // the worst: the linear membership check costs less than the hashing would.
    bool m_InWindow = false;
    std::vector<std::uint64_t> m_ClaimedInWindow;
    std::unordered_map<StringId, std::uint64_t> m_OwnerByTagId;
    // Only elements that ARE proxies, which is nothing at all in an editor full of native
    // chrome — the same reason the module-owned handler index exists rather than a tree walk.
    //
    // A SET, not a vector. Every walk that can MUTATE a proxy — OrphanTypesOwnedBy, Reconcile,
    // ApplyPendingBadges — runs or may run callbacks that re-enter this index, so each takes a
    // snapshot and revalidates every entry before touching it. Membership has to be an O(1)
    // question for that to be affordable, and it makes ForgetProxy O(1) rather than a linear
    // erase.
    //
    // CountProxiesInState is the one walk that iterates the index directly: it is const, calls
    // nothing, and only reads a byte of proxy state, so there is nothing for it to re-enter.
    // Snapshotting it would cost an allocation to protect against a hazard it cannot create.
    std::unordered_set<ManagedElementProxy*> m_Proxies;
    std::unordered_set<std::uint64_t> m_Logged; // (tagId, state) pairs already reported
    std::unordered_set<std::uint64_t> m_RefusalLogged; // (tagId, holder) pairs already reported
    std::size_t m_RefusedTagClaims = 0;
    std::optional<TagClaimRefusal> m_LastRefusedTagClaim;
};

} // namespace GameEngine::Scripting
