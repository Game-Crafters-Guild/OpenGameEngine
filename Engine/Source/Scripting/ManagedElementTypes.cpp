#include "Scripting/ManagedElementTypes.h"

#include "Logger/Logger.h"
#include "Scripting/ManagedTypeOwners.h"
#include "Types/StringUtils.h"
#include "UI/Registration/ElementRegistration.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <string_view>
#include <vector>
#include <cstdlib>
#include <optional>
#include <memory>

namespace GameEngine::Scripting
{

namespace
{
// (tagId, state) folded into one key for the dedup set.
std::uint64_t LogKey(StringId tagId, ManagedTypeState state)
{
    return tagId ^ (static_cast<std::uint64_t>(state) + 0x9E3779B97F4A7C15ull);
}

// (tagId, holder) folded into one key for the refusal dedup set. Its own function rather than a
// reuse of LogKey: the two sets are separate, and folding a full 64-bit owner id the way a
// 3-value enum is folded would be a different mixing question.
std::uint64_t RefusalKey(StringId tagId, std::uint64_t holder)
{
    return tagId ^ (holder * 0x9E3779B97F4A7C15ull);
}
} // namespace

// ---------------------------------------------------------------------------
// ManagedElementProxy
// ---------------------------------------------------------------------------

ManagedElementProxy::ManagedElementProxy(StringId tagId, std::string_view displayTag,
                                         std::uint64_t owner)
    : m_Owner(owner)
{
    // The inherited members ARE the identity, so there is no second copy to disagree with
    // them. The name matters as much as the id: without it every proxy falls through to the
    // RTTI reverse map, which holds one entry for the whole shared proxy class, and a
    // document of managed elements reads as the same tag repeated in the inspector and in
    // XML export.
    m_TagId = tagId;
    m_TagName = std::string(displayTag);
    ManagedElementTypes::Instance().NoteProxy(this);
}

ManagedElementProxy::~ManagedElementProxy()
{
    ManagedElementTypes& types = ManagedElementTypes::Instance();
    // Leave the index BEFORE the managed Release, not after. Release runs managed
    // code, and every walk over the proxies decides what is alive by asking
    // whether it is still indexed. A proxy that is still indexed while it
    // destructs answers "alive", so a walk re-entered from here orphans it a
    // second time — releasing the handle twice and writing to an object that is
    // already coming apart. De-indexed first, the walk simply does not see it.
    const std::intptr_t handle = m_Handle;
    m_Handle = 0;
    types.ForgetProxy(this);

    if (handle != 0 && types.Callbacks().Release)
        types.Callbacks().Release(handle);
}

void ManagedElementProxy::OnAuthoredAttributesApplied(
    const std::unordered_map<std::string, std::string>& attrsLower)
{
    // Straight from the set that just landed — this is the document telling the instance what
    // changed, so there is nothing to store and nothing to replay.
    for (const auto& kv : attrsLower)
        ApplyAttributeToInstance(kv.first, kv.second);
}

void ManagedElementProxy::ApplyAttributeToInstance(const std::string& name,
                                                   const std::string& value) const
{
    if (m_Handle == 0)
        return;
    const ManagedElementCallbacks& cb = ManagedElementTypes::Instance().Callbacks();
    if (!cb.ApplyAttribute)
        return;
    cb.ApplyAttribute(m_Handle, name.c_str(), value.c_str());
}

void ManagedElementProxy::ApplyAuthoredAttributesToInstance() const
{
    for (const auto& kv : m_Attributes)
        ApplyAttributeToInstance(kv.first, kv.second);
}

// ---------------------------------------------------------------------------
// ManagedElementTypes
// ---------------------------------------------------------------------------

ManagedElementTypes& ManagedElementTypes::Instance()
{
    // Deliberately never destroyed, exactly as ElementFactoryRegistry is: a proxy's
    // destructor calls back in here to drop its index entry, and a UI tree torn down during
    // static destruction could otherwise outlive the registry and touch a dead object.
    static ManagedElementTypes* s_instance = new ManagedElementTypes();
    return *s_instance;
}

namespace
{
std::optional<bool> g_BadgeOverride;
}

bool ManagedElementTypes::BadgeEnabled()
{
    if (g_BadgeOverride.has_value())
        return *g_BadgeOverride;
    static const bool kEnabled = []
    {
        const char* v = std::getenv("GE_UI_ORPHAN_BADGE");
        if (v && (v[0] == '0' || v[0] == '1'))
            return v[0] == '1';
#if defined(GE_DEV_DIAG)
        return true;
#else
        return false;
#endif
    }();
    return kEnabled;
}

void ManagedElementTypes::SetBadgeEnabledForTest(std::optional<bool> enabled)
{
    g_BadgeOverride = enabled;
}

// The three installs below are FIRST-WINS, and that is what makes them safe without a lock.
//
// They are called from the defining language's bootstrap, which runs on whichever thread loaded
// it — a thread-pool one whenever the managed pump's watchdog fires — while the main thread may
// already be reading and INVOKING what they write. Reassigning a live std::function frees the
// callable under its caller, which is the hazard; refusing every install after the first removes
// it, because nothing is ever reassigned.
//
// The first write is a different question, and it is answered by INSTALL ORDER rather than here.
// These are three sequential calls, so they do not become visible together; what makes the gap
// harmless is that the callbacks — the only thing that opens a gate — go LAST. Until
// PerformRegistrations is set, RequestRegistrationWindow returns early and nothing reads the
// marshaller; until Create is set, no proxy can be built. See GE_UI_SetElementTypeCallbacks,
// which owns that ordering and explains it. Reversing it there reopens a torn read of the
// marshaller, so the two must be read together.
//
// A rejected re-install is counted rather than silent, so a second host trying to take the
// callbacks over is visible instead of looking like it worked.
void ManagedElementTypes::SetCallbacks(const ManagedElementCallbacks& callbacks)
{
    if (m_Callbacks.Create || m_Callbacks.ApplyAttribute || m_Callbacks.Release ||
        m_Callbacks.PerformRegistrations)
    {
        RefuseReinstall("the element type callbacks");
        return;
    }
    m_Callbacks = callbacks;
}

void ManagedElementTypes::SetMainThreadMarshaller(MainThreadMarshaller marshaller)
{
    if (m_Marshaller)
    {
        RefuseReinstall("the main-thread marshaller");
        return;
    }
    m_Marshaller = std::move(marshaller);
}

void ManagedElementTypes::SetMainThreadPredicate(MainThreadPredicate predicate)
{
    if (m_MainThreadPredicate)
    {
        RefuseReinstall("the main-thread predicate");
        return;
    }
    m_MainThreadPredicate = std::move(predicate);
}

void ManagedElementTypes::RefuseReinstall(const char* what)
{
    ++m_RefusedReinstalls;
    Logger::Log::Error("UI: {} is already installed and may not be replaced. Installation is "
                       "first-wins, because replacing a live callback frees it under whichever "
                       "thread is calling it. The second installation was ignored.",
                       what);
}

namespace
{
// Set for the duration of a marshalled task, so nested RunOnMainThread calls recognise that they
// are already where they were trying to get to. Thread_local rather than a plain member: the
// unload seam can be running on another thread at the same time and must still marshal.
thread_local bool t_InMainThreadTask = false;
} // namespace

void ManagedElementTypes::RunOnMainThread(std::function<void()> task)
{
    if (!m_Marshaller || t_InMainThreadTask)
    {
        task();
        return;
    }

    m_Marshaller([task = std::move(task)]
                 {
                     const bool previous = t_InMainThreadTask;
                     t_InMainThreadTask = true;
                     try
                     {
                         task();
                     }
                     catch (...)
                     {
                         t_InMainThreadTask = previous;
                         throw;
                     }
                     t_InMainThreadTask = previous;
                 });
}

bool ManagedElementTypes::OnMainThread() const
{
    return !m_MainThreadPredicate || m_MainThreadPredicate();
}

void ManagedElementTypes::RefuseOffMainThread(const char* what, const char* remedyAndConsequence)
{
    ++m_RefusedOffMainThread;
    // The tail is the caller's, because the two families of refusal have OPPOSITE consequences.
    // A refused claim or verdict leaves elements inert until something decides them; a refused
    // unload sweep leaves them Live, which is the more alarming outcome and the one a shared
    // sentence would have described backwards.
    Logger::Log::Error("UI: {} was called from a thread that does not own the UI tree and was "
                       "refused. {}",
                       what, remedyAndConsequence);
}

void ManagedElementTypes::RequestRegistrationWindow()
{
    // Deliberately reads the callback before marshalling: a host with nothing staged should not
    // pay for a queued task that would do nothing.
    if (!m_Callbacks.PerformRegistrations)
        return;

    // Capturing `this` is safe because the singleton is never destroyed. The callback pointer is
    // re-read inside the task rather than captured: it is installed from the default context and
    // may be replaced between the request and the window.
    RunOnMainThread([this] { RunRegistrationWindow(); });
}

void ManagedElementTypes::RunRegistrationWindow()
{
    if (!m_Callbacks.PerformRegistrations)
        return;

    // Saved and restored rather than assigned. No path nests a window today, but one that did
    // would otherwise hand the inner window the outer's claims and then discard them.
    const bool wasInWindow = m_InWindow;
    std::vector<std::uint64_t> outerClaims;
    outerClaims.swap(m_ClaimedInWindow);
    m_InWindow = true;

    try
    {
        m_Callbacks.PerformRegistrations();
    }
    catch (...)
    {
        m_InWindow = wasInWindow;
        m_ClaimedInWindow = std::move(outerClaims);
        throw;
    }

    m_InWindow = wasInWindow;
    std::vector<std::uint64_t> claimed;
    claimed.swap(m_ClaimedInWindow);
    m_ClaimedInWindow = std::move(outerClaims);

    // The batch is complete: the callback has returned, so the defining language has been handed
    // every tag id it claimed and can answer for all of them. That is the same condition
    // NotifyReloadCompleted asserts, reached without anyone having to announce it — which is what
    // recovers a type whose restoration unloaded nothing.
    //
    // Reconcile directly rather than through NotifyReloadCompleted: this IS the main-thread slot,
    // so the thread guard has nothing to add and marshalling would push the work a frame out. A
    // host with no marshaller runs the window inline on the calling thread, where registration
    // itself is refused off-main and `claimed` is empty — so there is nothing to decide.
    for (std::uint64_t owner : claimed)
        Reconcile(owner);
}

StringId ManagedElementTypes::RegisterType(std::string_view tagName, std::uint64_t owner)
{
    const std::string_view one[]{tagName};
    const std::vector<StringId> ids = RegisterTypes(one, owner);
    return ids.empty() ? StringId(0) : ids.front();
}

std::vector<StringId> ManagedElementTypes::RegisterTypes(std::span<const std::string_view> tagNames,
                                                        std::uint64_t owner)
{
    std::vector<StringId> tagIds(tagNames.size(), StringId(0));
    // Owner 0 is the engine, and nothing outside C++ may claim it: it is the value the
    // factory sweep refuses, so accepting it here would register types that could never be
    // unregistered again.
    if (owner == 0)
        return tagIds;

    // The whole batch or none of it. A partial claim would hand back some ids and leave the
    // caller's tag -> type table disagreeing with the registry.
    if (!OnMainThread())
    {
        RefuseOffMainThread("registering a UI element type",
                            "Claim it from a registration window "
                            "(GE_UI_RequestTypeRegistrationWindow), which runs on the thread "
                            "that owns the UI tree. Nothing was claimed, so this batch's "
                            "elements stay inert until a later verdict decides them.");
        return tagIds;
    }

    // Claiming is all registration does. What happens to elements already orphaned under these
    // tags is decided at this owner's NotifyReloadCompleted, once the caller has had every id
    // back and can answer for them.
    for (std::size_t i = 0; i < tagNames.size(); ++i)
        tagIds[i] = ClaimTag(tagNames[i], owner);

    return tagIds;
}

StringId ManagedElementTypes::ClaimTag(std::string_view tagName, std::uint64_t owner)
{
    if (owner == 0 || tagName.empty())
        return 0;

    const std::string display{tagName};
    const std::string tagLower = ToLowerAscii(display);

    auto& factories = UIRegistration::ElementFactoryRegistry::Instance();
    const StringId tagId = factories.CanonicalTagId(tagLower);
    if (tagId == 0)
        return 0;

    // Refused when the tag already belongs to someone else — an engine control above all.
    // Without this a managed type named "Button" would replace the built-in for every .uxml in
    // the process, and unloading its context would then delete <Button/> for the rest of the
    // session. The registry owns that rule so it cannot be bypassed by registering directly.
    if (!factories.RegisterFactory(
            tagLower,
            [tagId, display, owner]() -> std::unique_ptr<UIElement>
            { return std::make_unique<ManagedElementProxy>(tagId, display, owner); },
            typeid(ManagedElementProxy),
            owner))
    {
        NoteRefusedTagClaim(tagId, tagLower, display, owner);
        return 0;
    }

    m_OwnerByTagId[tagId] = owner;
    ClearLogState(tagId);
    // Inside a window this owner now owes a verdict, which the window issues once the whole batch
    // has returned. See RunRegistrationWindow.
    if (m_InWindow &&
        std::find(m_ClaimedInWindow.begin(), m_ClaimedInWindow.end(), owner) ==
            m_ClaimedInWindow.end())
    {
        m_ClaimedInWindow.push_back(owner);
    }
    return tagId;
}

void ManagedElementTypes::NoteRefusedTagClaim(StringId tagId, const std::string& tagLower,
                                              const std::string& display, std::uint64_t claimant)
{
    const std::optional<std::uint64_t> holder =
        UIRegistration::ElementFactoryRegistry::Instance().OwnerOfTag(tagLower);
    const std::uint64_t holderId = holder.value_or(0);
    // Owner 0 is the engine, which has no load context to be alive or dead.
    const bool holderLive = holderId != 0 && ManagedTypeOwners::Instance().IsLive(holderId);

    ++m_RefusedTagClaims;
    m_LastRefusedTagClaim = TagClaimRefusal{holderId, holderLive};

    if (m_RefusalLogged.size() >= kMaxReportedTagRefusals ||
        !m_RefusalLogged.insert(RefusalKey(tagId, holderId)).second)
    {
        return;
    }
    // Said once, as the budget is spent, so the log going quiet is itself reported rather than
    // looking like the refusals stopped. Mirrors ManagedTypeOwners::ReportRejected.
    const bool budgetJustSpent = m_RefusalLogged.size() == kMaxReportedTagRefusals;

    // The registry has already said WHICH owner holds the tag. What it cannot say, and what
    // decides whether this is a transient collision or a dead end, is what that owner is: an
    // engine control the claim may never have, or a load context that has to unload before the
    // tag is free — and a context that leaked never will.
    const char* holderKind =
        holderId == 0 ? "it is a built-in engine control and cannot be claimed"
                      : (holderLive ? "its load context is still registered, so the tag is not "
                                      "free until that context unloads"
                                    : "its load context is already released, so the tag is stale "
                                      "and the claim should be retried on the next reload");
    Logger::Log::Warning("UI: the externally-defined element type '{}' could not claim its tag "
                         "for owner {} — owner {} holds it and {}. Its elements stay as they are "
                         "until the tag is claimed successfully.",
                         display, claimant, holderId, holderKind);

    if (budgetJustSpent)
    {
        Logger::Log::Warning("UI: further element-tag claim refusals will be counted but no "
                             "longer logged ({} distinct (tag, holder) pairs reported).",
                             kMaxReportedTagRefusals);
    }
}

std::size_t ManagedElementTypes::OrphanTypesOwnedBy(std::uint64_t owner)
{
    if (owner == 0)
        return 0;

    // MAIN THREAD ONLY, for the same reason registration is: the factory sweep below is the
    // symmetric — and larger — write of the registry a document build reads, and that registry has
    // no synchronization. This is a CONTRACT on the unload seam, not a fallback. Refused, nothing
    // at all happens, and BOTH consequences are permanent: the handles are not released so the
    // instances stay reachable and the context can never be collected (the runtime's own ALC-leak
    // detector reports that half), and the owner's tags are never freed, so the type can never be
    // registered again for the life of the process. Loud and permanent beats silent corruption of
    // a registry the UI thread is reading.
    if (!OnMainThread())
    {
        RefuseOffMainThread(
            "orphaning the elements of an unloading UI element type owner",
            "Drive the unload seam from the thread that owns the UI tree. NOTHING WAS SWEPT: "
            "this owner's elements are still Live and still hold handles into a context that "
            "is going away, and its tags stay claimed for the rest of the process, so the "
            "type can never be registered again.");
        return 0;
    }

    // Two-phase, same reason: Orphan runs the Release callback, and a Release that drops the
    // last reference to another element destroys it, removing it from the index mid-walk.
    std::size_t orphaned = 0;
    for (ManagedElementProxy* proxy : SnapshotProxies())
    {
        if (!StillIndexed(proxy))
            continue;
        if (proxy->m_Owner == owner && proxy->m_State == ManagedTypeState::Live)
        {
            Orphan(*proxy);
            ++orphaned;
        }
    }

    // The factories go after the handles are released, not before: a factory removed first
    // would leave a window in which a document rebuild could mint a plain element for a tag
    // whose proxies are still alive.
    UIRegistration::ElementFactoryRegistry::Instance().UnregisterFactoriesOwnedBy(owner);

    for (auto it = m_OwnerByTagId.begin(); it != m_OwnerByTagId.end();)
        it = (it->second == owner) ? m_OwnerByTagId.erase(it) : std::next(it);

    // The badge is the tree-visible half and it deliberately does NOT run here: this whole
    // function executes on whichever thread called AssemblyLoadContext.Unload, and a class edit
    // recomputes style. ONE task for the owner rather than one per proxy — the main-thread
    // queue's budget drops to a single task per frame during a hot reload, which is exactly
    // when this fires. Capturing `this` is safe because the singleton is never destroyed.
    if (orphaned != 0)
        RunOnMainThread([this, owner] { ApplyPendingBadges(owner); });

    return orphaned;
}

void ManagedElementTypes::ApplyPendingBadges(std::uint64_t owner)
{
    if (!BadgeEnabled())
        return;

    // Re-derived, never carried. Whatever the orphan pass saw may have been destroyed by the
    // time this runs, and a proxy whose type came back in between is Live again and must not be
    // badged. Every condition is read from the live object AFTER StillIndexed, which is the
    // discipline that makes the pointer check ABA-tolerant rather than merely lucky.
    for (ManagedElementProxy* proxy : SnapshotProxies())
    {
        if (!StillIndexed(proxy))
            continue;
        if (proxy->m_State == ManagedTypeState::Pending && proxy->m_Owner == owner)
            SetBadge(*proxy, ManagedTypeState::Pending);
    }
}

void ManagedElementTypes::NotifyReloadCompleted(std::uint64_t owner)
{
    if (owner == 0)
        return;

    if (!OnMainThread())
    {
        RefuseOffMainThread("closing a UI element type reload verdict",
                            "Issue it from a registration window "
                            "(GE_UI_RequestTypeRegistrationWindow), which runs on the thread "
                            "that owns the UI tree. Nothing was decided, so this owner's "
                            "elements stay inert until a later verdict decides them.");
        return;
    }

    // ONE task for the owner, covering both halves of the verdict. Everything Reconcile does is
    // tree-visible — re-attaching an instance replays authored attributes, and either outcome
    // edits a badge class, which recomputes style. Inside a registration window this runs inline
    // and closes the window; from a native host mid-frame it is queued. Capturing `this` is safe
    // because the singleton is never destroyed.
    RunOnMainThread([this, owner] { Reconcile(owner); });
}

void ManagedElementTypes::Reconcile(std::uint64_t owner)
{
    // Re-derived from the LIVE index, never from anything the caller captured: this runs a
    // frame after the verdict was announced, by which time proxies may be gone and tags may
    // have moved. Snapshotted for the same reason as the other walks — Materialize runs the
    // Create callback, and a managed constructor that builds a subtree of its own type adds
    // proxies to this very index.
    for (ManagedElementProxy* proxy : SnapshotProxies())
    {
        if (!StillIndexed(proxy))
            continue;
        if (proxy->m_State == ManagedTypeState::Live)
            continue;

        const auto tagOwner = m_OwnerByTagId.find(proxy->m_TagId);
        if (tagOwner != m_OwnerByTagId.end())
        {
            // The type is available again, so the element comes back whatever left it inert —
            // this reload's unload, or an earlier one that already faulted it. It is never
            // replaced, so its children, layout and scroll offset are exactly where they were.
            //
            // Not filtered by owner: the proxy's stamp names the context that DEFINED it, which
            // a reload retires. What matters is that some live owner holds the tag now, and the
            // proxy joins it so the next unload of THAT context sweeps it. It cannot resurrect a
            // stale type in the process — Materialize resolves through the defining language's
            // tag -> type table, which holds whoever owns the tag NOW.
            //
            // It also retries a proxy left Faulted by a THROWING constructor, on every unrelated
            // window, for as long as its tag stays registered. That is what lets a fixed
            // constructor recover with no further reload; the cost is that a permanently throwing
            // one is re-attempted per window. The native log is deduplicated, but the defining
            // language's own report is not, so that case prints once per reload for the session.
            proxy->m_Owner = tagOwner->second;
            Materialize(*proxy);
            continue;
        }

        // Absence is only a fact for the owner whose reload just finished. Another context's
        // Pending elements are merely still loading, and faulting them would badge working
        // controls as missing.
        if (proxy->m_State != ManagedTypeState::Pending || proxy->m_Owner != owner)
            continue;

        proxy->m_State = ManagedTypeState::Faulted;
        SetBadge(*proxy, ManagedTypeState::Faulted);
        if (ShouldLogOnce(proxy->m_TagId, ManagedTypeState::Faulted))
        {
            Logger::Log::Warning(
                "UI: element type '{}' did not come back after a script reload - its elements "
                "stay in the tree as inert containers. The type was deleted, renamed, or its "
                "assembly failed to load.",
                proxy->m_TagName);
        }
    }
}

void ManagedElementTypes::Materialize(ManagedElementProxy& proxy)
{
    proxy.RemoveClass(kOrphanedClass);
    proxy.RemoveClass(kMissingClass);

    // Both callers hand over a proxy that holds no instance: NoteProxy materializes
    // one straight out of its constructor, and Reconcile skips every proxy in the
    // Live state — and Live is the only state a non-zero handle exists in. A
    // handle here would mean an instance is being dropped without a Release.
    assert(proxy.m_Handle == 0 && "Materialize on a proxy that still holds an instance");

    // Create runs managed code, and a managed constructor can destroy elements —
    // including the one being materialized. The index is the liveness signal
    // (~ManagedElementProxy leaves it before anything else runs), so the handle
    // waits in a local until the proxy is known to still be there.
    const std::intptr_t created =
        m_Callbacks.Create ? m_Callbacks.Create(proxy.m_TagId, proxy.GetInstanceId()) : 0;
    if (!StillIndexed(&proxy))
        return;
    proxy.m_Handle = created;

    if (proxy.m_Handle == 0)
    {
        // The tag is registered but the instance could not be built — a constructor that
        // threw. Faulted rather than Pending: no reload is pending, this one already failed.
        proxy.m_State = ManagedTypeState::Faulted;
        SetBadge(proxy, ManagedTypeState::Faulted);
        if (ShouldLogOnce(proxy.m_TagId, ManagedTypeState::Faulted))
        {
            Logger::Log::Warning("UI: element type '{}' is registered but its instance could not "
                                 "be constructed; the element renders as an inert container.",
                                 proxy.m_TagName);
        }
        return;
    }

    proxy.m_State = ManagedTypeState::Live;
    // The new instance was constructed with its own defaults and knows nothing of what the
    // document said about the element it is attaching to.
    proxy.ApplyAuthoredAttributesToInstance();
}

void ManagedElementTypes::Orphan(ManagedElementProxy& proxy)
{
    // EVERY touch of `proxy` happens BEFORE the callback, and nothing touches it after. Release
    // runs managed code, and a Release that drops the last reference to an element destroys that
    // element's subtree — which can contain THIS proxy. The walk's StillIndexed guard covers the
    // proxies the pass has not reached yet; it cannot cover the one the pass is currently inside.
    // So the proxy is left fully orphaned first (the state it would end in anyway), the values
    // the tail needs are copied out, and the callback goes last.
    //
    // The callback itself stays inline and unconditional: the handle is only valid while the
    // unload callback is on the stack. The badge that goes with this state change is queued by
    // the caller instead.
    const std::intptr_t handle = proxy.m_Handle;
    const StringId tagId = proxy.m_TagId;
    const std::string tagName = proxy.m_TagName;

    proxy.m_Handle = 0;
    proxy.m_State = ManagedTypeState::Pending;

    if (handle != 0 && m_Callbacks.Release)
        m_Callbacks.Release(handle);

    if (ShouldLogOnce(tagId, ManagedTypeState::Pending))
    {
        Logger::Log::Warning("UI: element type '{}' unloaded; its elements stay in the tree and "
                             "re-attach if the type comes back.",
                             tagName);
    }
}

void ManagedElementTypes::SetBadge(ManagedElementProxy& proxy, ManagedTypeState state)
{
    if (!BadgeEnabled())
        return;
    proxy.RemoveClass(kOrphanedClass);
    proxy.RemoveClass(kMissingClass);
    if (state == ManagedTypeState::Pending)
        proxy.AddClass(kOrphanedClass);
    else if (state == ManagedTypeState::Faulted)
        proxy.AddClass(kMissingClass);
}

std::vector<ManagedElementProxy*> ManagedElementTypes::SnapshotProxies() const
{
    std::vector<ManagedElementProxy*> snapshot;
    snapshot.reserve(m_Proxies.size());
    for (ManagedElementProxy* proxy : m_Proxies)
        snapshot.push_back(proxy);
    return snapshot;
}

void ManagedElementTypes::NoteProxy(ManagedElementProxy* proxy)
{
    m_Proxies.insert(proxy);
    // Construction runs inside the factory, so the type is registered by definition and the
    // instance can be built immediately — the constructor is the construction hook.
    Materialize(*proxy);
}

void ManagedElementTypes::ForgetProxy(ManagedElementProxy* proxy)
{
    m_Proxies.erase(proxy);
}

std::size_t ManagedElementTypes::CountProxiesInState(ManagedTypeState state) const
{
    std::size_t count = 0;
    for (const ManagedElementProxy* proxy : m_Proxies)
    {
        if (proxy && proxy->m_State == state)
            ++count;
    }
    return count;
}

std::size_t ManagedElementTypes::CountTypesOwnedBy(std::uint64_t owner) const
{
    std::size_t count = 0;
    for (const auto& kv : m_OwnerByTagId)
    {
        if (kv.second == owner)
            ++count;
    }
    return count;
}

void ManagedElementTypes::ResetForTests()
{
    for (const auto& kv : m_OwnerByTagId)
        UIRegistration::ElementFactoryRegistry::Instance().UnregisterFactoriesOwnedBy(kv.second);
    m_OwnerByTagId.clear();
    m_Logged.clear();
    m_RefusalLogged.clear();
    m_RefusedTagClaims = 0;
    m_LastRefusedTagClaim.reset();
    m_Callbacks = ManagedElementCallbacks{};
    m_Marshaller = {};
    m_MainThreadPredicate = {};
    m_RefusedOffMainThread = 0;
    m_RefusedReinstalls = 0;
    m_InWindow = false;
    m_ClaimedInWindow.clear();
}

bool ManagedElementTypes::ShouldLogOnce(StringId tagId, ManagedTypeState state)
{
    return m_Logged.insert(LogKey(tagId, state)).second;
}

void ManagedElementTypes::ClearLogState(StringId tagId)
{
    m_Logged.erase(LogKey(tagId, ManagedTypeState::Pending));
    m_Logged.erase(LogKey(tagId, ManagedTypeState::Faulted));
}


} // namespace GameEngine::Scripting
