// The type-lifecycle half of externally-defined element types: who may own a type, and when the
// tree-visible part of losing one is allowed to touch the tree.
//
// Two properties are under test and they are independent of each other.
//
// OWNERSHIP IS NOT DECORATIVE. ManagedElementTypes takes an owner id as a plain number and
// trusts it, which is right for C++ but not for a script that can declare its own DllImport and
// pass any number. ManagedTypeOwners is what makes the id unforgeable in practice, and a refusal
// has to be OBSERVABLE — a return code alone leaves a forged id looking exactly like a call that
// had nothing to do.
//
// THE TRANSITION IS SPLIT BY THREAD, NOT BY CONVENIENCE. The unload seam runs on whichever
// thread called AssemblyLoadContext.Unload. Releasing the managed handle cannot wait — that
// handle is valid only while the callback is on the stack — but adding a badge CLASS recomputes
// style, so it must reach the UI thread. These arms drive the marshaller directly, which is what
// lets them assert the deferral without a frame loop.
//
// The exports over all of this (GE_UI_AcquireTypeOwner and friends) live in GameEngine.Native,
// which this suite does not link; what is asserted here is the engine-side contract those
// exports are a thin, validating forwarder to.

#include "Scripting/ManagedElementTypes.h"
#include "Scripting/ManagedTypeOwners.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "Types/StringId.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Scripting;

namespace
{
// Stands in for the managed runtime, as in ManagedElementOrphanTests: handles are dealt out in
// sequence so an arm can tell a re-materialized instance from the one it replaced.
//
// IT ANSWERS ONLY TAGS IT HAS BEEN TOLD ABOUT, which is the one property that makes this suite
// able to see an ordering mistake at all. The managed binding maps tag id -> Type and can only
// fill that map with an id the engine has already RETURNED to it; asked for anything else it
// declines, and the engine reads the 0 as a constructor that failed. A stub that answered
// unconditionally would be more permissive than the contract it stands in for, and every arm
// built on it would pass whether or not the engine respected the caller's ordering.
//
// Publishing is therefore never done by the stub itself: it is done by the fixture's Register
// helpers, strictly after the registration call has returned.
struct StubRuntime
{
    static inline std::unordered_set<std::uint64_t> s_Published;
    static inline std::intptr_t s_NextHandle = 0;
    static inline int s_Created = 0;
    static inline int s_Released = 0;
    static inline int s_Declined = 0;

    // What the defining language does when it is handed a registration window, and how many
    // windows it has been given. Held rather than hard-coded: each arm stages its own.
    static inline std::function<void()> s_Window;
    static inline int s_WindowsRun = 0;

    static void Reset()
    {
        s_Published.clear();
        s_NextHandle = 5000;
        s_Created = 0;
        s_Released = 0;
        s_Declined = 0;
        s_Window = {};
        s_WindowsRun = 0;
    }

    static void Publish(StringId tagId) { s_Published.insert(static_cast<std::uint64_t>(tagId)); }
    static void Retract(StringId tagId) { s_Published.erase(static_cast<std::uint64_t>(tagId)); }

    static std::intptr_t Create(std::uint64_t tagId, std::uint64_t)
    {
        if (s_Published.count(tagId) == 0)
        {
            ++s_Declined;
            return 0;
        }
        ++s_Created;
        return ++s_NextHandle;
    }

    static void ApplyAttribute(std::intptr_t, const char*, const char*) {}
    static void Release(std::intptr_t) { ++s_Released; }

    static void PerformRegistrations()
    {
        ++s_WindowsRun;
        if (s_Window)
            s_Window();
    }

    static ManagedElementCallbacks Callbacks()
    {
        ManagedElementCallbacks cb{};
        cb.Create = &Create;
        cb.ApplyAttribute = &ApplyAttribute;
        cb.Release = &Release;
        cb.PerformRegistrations = &PerformRegistrations;
        return cb;
    }
};

// Stands in for "is this the thread that owns the UI tree". Off by default in the arms that
// install it, so a call has to be inside a window to be allowed — which is exactly the shape the
// ABI's ScriptManager-backed predicate has in the editor.
bool g_OnMainThread = false;

// Stands in for ScriptManager's main-thread queue. Holding the tasks rather than running them is
// the whole point: it is the only way to observe the window between "the element went Pending"
// and "the tree was told".
std::vector<std::function<void()>> g_Queued;

void DrainQueued()
{
    std::vector<std::function<void()>> batch;
    batch.swap(g_Queued);
    for (std::function<void()>& task : batch)
        task();
}

class ManagedTypeLifecycleTest : public ::testing::Test
{
protected:
    static constexpr std::uint64_t kNativeOwner = 0xBEEFu;

    void SetUp() override
    {
        StubRuntime::Reset();
        g_Queued.clear();
        g_OnMainThread = false;
        // ResetForTests clears the marshaller, so anything installing one installs it after.
        Types().ResetForTests();
        Owners().ResetForTests();
        Types().SetCallbacks(StubRuntime::Callbacks());
        ManagedElementTypes::SetBadgeEnabledForTest(true);
    }

    void TearDown() override
    {
        m_Elements.clear();
        Types().ResetForTests();
        Owners().ResetForTests();
        ManagedElementTypes::SetBadgeEnabledForTest(std::nullopt);
        g_Queued.clear();
        g_OnMainThread = false;
    }

    static ManagedElementTypes& Types() { return ManagedElementTypes::Instance(); }
    static ManagedTypeOwners& Owners() { return ManagedTypeOwners::Instance(); }
    static UIRegistration::ElementFactoryRegistry& Factories()
    {
        return UIRegistration::ElementFactoryRegistry::Instance();
    }

    static void InstallCapturingMarshaller()
    {
        Types().SetMainThreadMarshaller([](std::function<void()> task)
                                        { g_Queued.push_back(std::move(task)); });
    }

    // Route the main-thread question through g_OnMainThread. Without this the predicate is unset
    // and every thread is the main thread, which is the native-only host's answer.
    static void InstallThreadPredicate()
    {
        Types().SetMainThreadPredicate([] { return g_OnMainThread; });
    }

    // Register in the ORDER the managed binding registers in: the caller learns a tag id only
    // as the registration call returns, and can publish its binding only after that. Every arm
    // goes through these rather than calling RegisterType directly, so no arm can accidentally
    // assume a runtime that could answer sooner.
    static StringId Register(std::string_view tagName, std::uint64_t owner)
    {
        const StringId id = Types().RegisterType(tagName, owner);
        if (id != 0)
            StubRuntime::Publish(id);
        return id;
    }

    static std::vector<StringId> RegisterBatch(std::span<const std::string_view> tagNames,
                                               std::uint64_t owner)
    {
        std::vector<StringId> ids = Types().RegisterTypes(tagNames, owner);
        for (StringId id : ids)
        {
            if (id != 0)
                StubRuntime::Publish(id);
        }
        return ids;
    }

    // Build one element for an already-registered tag and keep it alive past its registration,
    // which is what an orphan is.
    ManagedElementProxy* MakeProxy(std::string_view tagLower)
    {
        std::unique_ptr<UIElement> el = Factories().Create(tagLower);
        if (!el)
            return nullptr;
        auto* proxy = static_cast<ManagedElementProxy*>(el.get());
        m_Elements.push_back(std::move(el));
        return proxy;
    }

    std::vector<std::unique_ptr<UIElement>> m_Elements;
};
} // namespace

// ---------------------------------------------------------------------------
// Owner ids: minted, not chosen.
// ---------------------------------------------------------------------------

TEST_F(ManagedTypeLifecycleTest, AcquiredIdsAreNonZeroAndDistinct)
{
    std::unordered_set<std::uint64_t> seen;
    for (int i = 0; i < 64; ++i)
    {
        const std::uint64_t owner = Owners().Acquire();
        EXPECT_NE(owner, 0u) << "0 is the engine and must never be minted";
        EXPECT_TRUE(Owners().IsLive(owner));
        EXPECT_TRUE(seen.insert(owner).second) << "a live id was minted twice";
    }
    EXPECT_EQ(Owners().LiveCount(), 64u);
}

TEST_F(ManagedTypeLifecycleTest, AnIdNeverMintedIsRefusedAndTheRefusalIsObservable)
{
    // The forged case: a script that declared its own DllImport and passed a number.
    constexpr std::uint64_t kForged = 0x1234'5678'9ABC'DEF0ull;
    ASSERT_FALSE(Owners().IsLive(kForged));
    ASSERT_EQ(Owners().RejectedCallCount(), 0u);

    EXPECT_FALSE(Owners().ValidateOwner(kForged, OwnerEntryPoint::RegisterElementType));
    EXPECT_EQ(Owners().RejectedCallCount(), 1u) << "a refusal that is not recorded is invisible";

    // A different entry point is a different refusal, not a deduplicated repeat of the first.
    EXPECT_FALSE(Owners().ValidateOwner(kForged, OwnerEntryPoint::OrphanElementTypes));
    EXPECT_FALSE(Owners().ValidateOwner(kForged, OwnerEntryPoint::NotifyReloadCompleted));
    EXPECT_EQ(Owners().RejectedCallCount(), 3u);
}

TEST_F(ManagedTypeLifecycleTest, ZeroIsTheEngineAndIsNeverAValidOwner)
{
    EXPECT_FALSE(Owners().IsLive(0));
    EXPECT_FALSE(Owners().ValidateOwner(0, OwnerEntryPoint::OrphanElementTypes));
    EXPECT_EQ(Owners().RejectedCallCount(), 1u);
}

TEST_F(ManagedTypeLifecycleTest, ReleasingTwiceIsRefusedTheSecondTime)
{
    const std::uint64_t owner = Owners().Acquire();
    EXPECT_TRUE(Owners().Release(owner));
    EXPECT_FALSE(Owners().IsLive(owner));
    // The second release is what a double-unload looks like from the ABI, and it must not be
    // mistaken for success.
    EXPECT_FALSE(Owners().Release(owner));
    EXPECT_FALSE(Owners().ValidateOwner(owner, OwnerEntryPoint::NotifyReloadCompleted))
        << "a released id must not be usable to decide another context's elements";
}

TEST_F(ManagedTypeLifecycleTest, ARetiredIdIsNeverMintedAgain)
{
    const std::uint64_t retired = Owners().Acquire();
    ASSERT_TRUE(Owners().Release(retired));

    for (int i = 0; i < 512; ++i)
        EXPECT_NE(Owners().Acquire(), retired) << "an id names one load context for the process";
}

TEST_F(ManagedTypeLifecycleTest, EveryRefusalIsCountedEvenWhenTheLogStopsNamingThem)
{
    // A caller in a loop must not be able to silence the counter or flood the log. The log is
    // bounded by construction (a fixed number of distinct call sites are named); the COUNT is
    // what stays true, and it is the half a test can assert.
    for (std::uint64_t i = 1; i <= 500; ++i)
        EXPECT_FALSE(Owners().ValidateOwner(i * 0x9E37u, OwnerEntryPoint::OrphanElementTypes));
    EXPECT_EQ(Owners().RejectedCallCount(), 500u);
}

// ---------------------------------------------------------------------------
// The orphan transition is split: handle inline, badge marshalled.
// ---------------------------------------------------------------------------

TEST_F(ManagedTypeLifecycleTest, TheHandleGoesInlineButTheBadgeWaitsForTheMainThread)
{
    ASSERT_NE(Register("DeferBar", kNativeOwner), StringId(0));
    ManagedElementProxy* proxy = MakeProxy("deferbar");
    ASSERT_NE(proxy, nullptr);
    ASSERT_NE(proxy->Handle(), 0);

    InstallCapturingMarshaller();
    EXPECT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);

    // Inline: the managed handle is only valid while the unload callback is on the stack.
    EXPECT_EQ(proxy->Handle(), 0);
    EXPECT_EQ(proxy->State(), ManagedTypeState::Pending);
    EXPECT_EQ(StubRuntime::s_Released, 1);

    // Deferred: a class edit recomputes style and this ran on the unload thread.
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kOrphanedClass));
    ASSERT_EQ(g_Queued.size(), 1u);

    DrainQueued();
    EXPECT_TRUE(proxy->HasClass(ManagedElementTypes::kOrphanedClass));
}

TEST_F(ManagedTypeLifecycleTest, OneTaskPerOwnerNoMatterHowManyElementsAreOrphaned)
{
    ASSERT_NE(Register("BudgetBar", kNativeOwner), StringId(0));
    for (int i = 0; i < 32; ++i)
        ASSERT_NE(MakeProxy("budgetbar"), nullptr);

    InstallCapturingMarshaller();
    EXPECT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 32u);

    // The main-thread queue's budget drops to a single task per frame during a hot reload, which
    // is exactly when this fires — one task per proxy would take 32 frames to paint one badge.
    EXPECT_EQ(g_Queued.size(), 1u);

    DrainQueued();
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Pending), 32u);
}

TEST_F(ManagedTypeLifecycleTest, OrphaningNothingQueuesNothing)
{
    ASSERT_NE(Register("QuietBar", kNativeOwner), StringId(0));
    InstallCapturingMarshaller();

    EXPECT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 0u);
    EXPECT_TRUE(g_Queued.empty()) << "an owner with no live elements has nothing to tell the tree";
}

TEST_F(ManagedTypeLifecycleTest, ATypeThatComesBackEndsTheReloadUnbadgedWhateverOrderTheTasksRunIn)
{
    ASSERT_NE(Register("FastBar", kNativeOwner), StringId(0));
    ManagedElementProxy* proxy = MakeProxy("fastbar");
    ASSERT_NE(proxy, nullptr);

    InstallCapturingMarshaller();
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);
    ASSERT_EQ(g_Queued.size(), 1u) << "the orphan badge is queued first";

    // The reload re-registers the tag and then announces its verdict, which is the only thing
    // that re-materializes. Both halves are now queued, and the badge task is ahead of the
    // verdict: the element is genuinely orphaned in between, so it may be badged there.
    constexpr std::uint64_t kSecondOwner = 0xBEE5u;
    ASSERT_NE(Register("FastBar", kSecondOwner), StringId(0));
    Types().NotifyReloadCompleted(kNativeOwner);
    ASSERT_EQ(g_Queued.size(), 2u) << "one task for the orphan badge, one for the verdict";

    // What must be true once the queue has drained: the element is back, on the new owner, and
    // carries neither badge. The verdict re-derives from the live index, so it corrects a badge
    // the earlier task applied rather than being confused by it.
    DrainQueued();
    EXPECT_EQ(proxy->State(), ManagedTypeState::Live);
    EXPECT_EQ(proxy->Owner(), kSecondOwner) << "the proxy joins whoever holds its tag now";
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kOrphanedClass));
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kMissingClass));
    EXPECT_EQ(StubRuntime::s_Declined, 0) << "the caller had published by the time it was asked";
}

TEST_F(ManagedTypeLifecycleTest, AProxyDestroyedBeforeTheTaskRunsIsNotTouched)
{
    ASSERT_NE(Register("GoneBar", kNativeOwner), StringId(0));
    ASSERT_NE(MakeProxy("gonebar"), nullptr);
    ASSERT_NE(MakeProxy("gonebar"), nullptr);

    InstallCapturingMarshaller();
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 2u);
    ASSERT_EQ(g_Queued.size(), 1u);

    // The window the deferral opens: the elements the orphan pass saw can be destroyed before
    // the task runs. A task holding raw proxy pointers would dereference a dead one here.
    m_Elements.clear();
    ASSERT_EQ(Types().ProxyCount(), 0u);

    DrainQueued();
    SUCCEED() << "the task re-derives from the live index, so an emptied index is simply empty";
}

TEST_F(ManagedTypeLifecycleTest, WithNoMarshallerTheTransitionAppliesInline)
{
    // The native-only host, and the shape every other suite in this module depends on: the
    // off-thread caller the deferral exists for lives behind the managed ABI, which installs the
    // marshaller. Absent that, deferring would strand the badge forever.
    ASSERT_NE(Register("InlineBar", kNativeOwner), StringId(0));
    ManagedElementProxy* proxy = MakeProxy("inlinebar");
    ASSERT_NE(proxy, nullptr);

    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);
    EXPECT_TRUE(proxy->HasClass(ManagedElementTypes::kOrphanedClass));
}

// ---------------------------------------------------------------------------
// Batch registration: one snapshot for the whole reload, same discipline.
// ---------------------------------------------------------------------------

TEST_F(ManagedTypeLifecycleTest, ABatchRegistersEveryTagAndReturnsOneIdEach)
{
    const std::string_view tags[] = {"BatchA", "BatchB", "BatchC"};
    const std::vector<StringId> ids = RegisterBatch(tags, kNativeOwner);

    ASSERT_EQ(ids.size(), 3u);
    for (StringId id : ids)
        EXPECT_NE(id, StringId(0));
    EXPECT_TRUE(Factories().HasFactory("batcha"));
    EXPECT_TRUE(Factories().HasFactory("batchb"));
    EXPECT_TRUE(Factories().HasFactory("batchc"));
    EXPECT_EQ(Types().CountTypesOwnedBy(kNativeOwner), 3u);
}

TEST_F(ManagedTypeLifecycleTest, RegistrationClaimsTheTagAndTouchesNoProxy)
{
    const std::string_view tags[] = {"ClaimA", "ClaimB"};
    ASSERT_EQ(RegisterBatch(tags, kNativeOwner).size(), 2u);
    ManagedElementProxy* a = MakeProxy("claima");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);
    ASSERT_EQ(a->State(), ManagedTypeState::Pending);

    // The half of the protocol that makes the other half possible. Re-registering the tags
    // claims them and returns their ids and does nothing else — in the real ABI this call has
    // not yet returned, so the caller cannot answer for the tag it names.
    StubRuntime::s_Created = 0;
    constexpr std::uint64_t kReloadedOwner = 0xBEE7u;
    ASSERT_EQ(RegisterBatch(tags, kReloadedOwner).size(), 2u);

    EXPECT_EQ(StubRuntime::s_Created, 0) << "registration must not ask for an instance";
    EXPECT_EQ(StubRuntime::s_Declined, 0);
    EXPECT_EQ(a->State(), ManagedTypeState::Pending) << "the verdict has not been announced yet";
}

TEST_F(ManagedTypeLifecycleTest, TheVerdictReMaterializesOrphansOfEveryTagThatCameBack)
{
    const std::string_view tags[] = {"ReviveA", "ReviveB"};
    ASSERT_EQ(RegisterBatch(tags, kNativeOwner).size(), 2u);

    ManagedElementProxy* a = MakeProxy("revivea");
    ManagedElementProxy* b = MakeProxy("reviveb");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 2u);
    ASSERT_EQ(a->State(), ManagedTypeState::Pending);
    ASSERT_EQ(b->State(), ManagedTypeState::Pending);

    // The reload: every tag claimed, then ONE verdict for the owner walks the index once. Both
    // elements are the SAME elements — children, layout and scroll offset are exactly where
    // they were.
    constexpr std::uint64_t kReloadedOwner = 0xBEE7u;
    const std::uint64_t aId = a->GetInstanceId();
    const std::uint64_t bId = b->GetInstanceId();
    ASSERT_EQ(RegisterBatch(tags, kReloadedOwner).size(), 2u);
    Types().NotifyReloadCompleted(kNativeOwner);

    EXPECT_EQ(a->State(), ManagedTypeState::Live);
    EXPECT_EQ(b->State(), ManagedTypeState::Live);
    EXPECT_EQ(a->GetInstanceId(), aId);
    EXPECT_EQ(b->GetInstanceId(), bId);
    EXPECT_FALSE(a->HasClass(ManagedElementTypes::kOrphanedClass));
    EXPECT_FALSE(b->HasClass(ManagedElementTypes::kOrphanedClass));
}

TEST_F(ManagedTypeLifecycleTest, TheVerdictMaterializesEachOrphanExactlyOnce)
{
    const std::string_view tags[] = {"OnceA", "OnceB"};
    ASSERT_EQ(RegisterBatch(tags, kNativeOwner).size(), 2u);
    ASSERT_NE(MakeProxy("oncea"), nullptr);
    ASSERT_NE(MakeProxy("onceb"), nullptr);
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 2u);

    // One walk for the whole owner must not visit a proxy twice — the state filter, not a tag
    // filter, is what has to catch it, because Materialize makes the entry Live as it goes.
    StubRuntime::s_Created = 0;
    constexpr std::uint64_t kReloadedOwner = 0xBEE9u;
    ASSERT_EQ(RegisterBatch(tags, kReloadedOwner).size(), 2u);
    Types().NotifyReloadCompleted(kNativeOwner);
    EXPECT_EQ(StubRuntime::s_Created, 2);
}

TEST_F(ManagedTypeLifecycleTest, ABatchRefusesATagOwnedByAnotherContextWithoutFailingTheRest)
{
    constexpr std::uint64_t kOther = 0xBEEBu;
    ASSERT_NE(Register("Contested", kOther), StringId(0));

    const std::string_view tags[] = {"MineA", "Contested", "MineB"};
    const std::vector<StringId> ids = RegisterBatch(tags, kNativeOwner);

    ASSERT_EQ(ids.size(), 3u);
    EXPECT_NE(ids[0], StringId(0));
    EXPECT_EQ(ids[1], StringId(0)) << "a tag another context owns is refused, not taken over";
    EXPECT_NE(ids[2], StringId(0)) << "one refused tag must not sink the tags after it";
    EXPECT_EQ(Types().CountTypesOwnedBy(kNativeOwner), 2u);
}

TEST_F(ManagedTypeLifecycleTest, ABatchRefusesOwnerZeroOutright)
{
    const std::string_view tags[] = {"EngineA", "EngineB"};
    const std::vector<StringId> ids = RegisterBatch(tags, 0);

    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], StringId(0));
    EXPECT_EQ(ids[1], StringId(0));
    EXPECT_FALSE(Factories().HasFactory("enginea"))
        << "owner 0 is the engine, and its registrations are the ones no sweep can remove";
}

// ---------------------------------------------------------------------------
// The ordering the whole protocol exists for.
//
// The managed binding maps tag id -> Type and can only fill that map with an id the engine has
// already RETURNED to it. So between "the engine claimed the tag" and "the caller learned its
// id" there is a window in which the caller cannot answer for that tag at all. Materializing
// inside the registration call lands squarely in that window: Create is asked for a tag the
// caller provably cannot build, declines, and the engine reads the 0 as a constructor that
// threw — badging every C#-defined element as a missing type on every successful reload.
//
// Registration therefore claims and returns, and the verdict reconciles. This arm drives that
// exact order, and it is the regression test for the defect above: it failed on the protocol
// that materialized during registration.
// ---------------------------------------------------------------------------

TEST_F(ManagedTypeLifecycleTest, AReloadReMaterializesWhenTheCallerLearnsTheTagIdOnlyOnReturn)
{
    // Cold start. Register publishes strictly after the call returns, because the id it keys on
    // IS that call's return value.
    const StringId tagId = Register("Revive", kNativeOwner);
    ASSERT_NE(tagId, StringId(0));

    ManagedElementProxy* proxy = MakeProxy("revive");
    ASSERT_NE(proxy, nullptr);
    ASSERT_EQ(proxy->State(), ManagedTypeState::Live);
    const std::uint64_t instanceId = proxy->GetInstanceId();

    // The context unloads: the binding goes with the types it named.
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);
    StubRuntime::Retract(tagId);
    ASSERT_EQ(proxy->State(), ManagedTypeState::Pending);

    // The reload re-registers the same tag under a fresh owner and publishes on return, then
    // announces the verdict — which is the first moment the caller can answer for the tag.
    constexpr std::uint64_t kReloadedOwner = 0xBEEDu;
    const StringId reloadedTagId = Register("Revive", kReloadedOwner);
    ASSERT_EQ(reloadedTagId, tagId) << "the tag id is derived from the name and must be stable";
    Types().NotifyReloadCompleted(kNativeOwner);

    EXPECT_EQ(StubRuntime::s_Declined, 0)
        << "the engine asked for an instance before the caller could know the tag id";
    EXPECT_EQ(proxy->State(), ManagedTypeState::Live)
        << "the element must come back rather than fault; §7.7 is the promise under test";
    EXPECT_EQ(proxy->GetInstanceId(), instanceId) << "the SAME element, not a rebuilt one";
    EXPECT_EQ(proxy->Owner(), kReloadedOwner) << "and it joins the context that holds its tag now";
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kMissingClass))
        << "a successful reload must not paint the missing-type badge";
}

TEST_F(ManagedTypeLifecycleTest, AVerdictWithTheTagStillAbsentFaultsAndBadges)
{
    const StringId tagId = Register("Deleted", kNativeOwner);
    ASSERT_NE(tagId, StringId(0));
    ManagedElementProxy* proxy = MakeProxy("deleted");
    ASSERT_NE(proxy, nullptr);

    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);
    StubRuntime::Retract(tagId);
    ASSERT_EQ(proxy->State(), ManagedTypeState::Pending);

    // The other half of the same verdict: nothing re-registered this tag, so absence is now a
    // fact rather than a reload still running.
    Types().NotifyReloadCompleted(kNativeOwner);
    EXPECT_EQ(proxy->State(), ManagedTypeState::Faulted);
    EXPECT_TRUE(proxy->HasClass(ManagedElementTypes::kMissingClass));
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kOrphanedClass));
}

TEST_F(ManagedTypeLifecycleTest, AnotherOwnersPendingElementsAreNotFaultedByThisVerdict)
{
    const StringId mine = Register("MineOnly", kNativeOwner);
    constexpr std::uint64_t kOtherOwner = 0xBEA7u;
    const StringId theirs = Register("TheirsOnly", kOtherOwner);
    ASSERT_NE(mine, StringId(0));
    ASSERT_NE(theirs, StringId(0));

    ManagedElementProxy* a = MakeProxy("mineonly");
    ManagedElementProxy* b = MakeProxy("theirsonly");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // Two collectible contexts unload; only one of them reloads. The other's elements are
    // merely still loading, and badging them missing would be a lie about a working control.
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kOtherOwner), 1u);
    StubRuntime::Retract(mine);
    StubRuntime::Retract(theirs);

    Types().NotifyReloadCompleted(kNativeOwner);
    EXPECT_EQ(a->State(), ManagedTypeState::Faulted);
    EXPECT_EQ(b->State(), ManagedTypeState::Pending) << "not this reload's verdict to give";
    EXPECT_FALSE(b->HasClass(ManagedElementTypes::kMissingClass));
}

TEST_F(ManagedTypeLifecycleTest, TheVerdictIsOneMarshalledTaskForTheWholeOwner)
{
    const std::string_view tags[] = {"MarshalA", "MarshalB"};
    ASSERT_EQ(RegisterBatch(tags, kNativeOwner).size(), 2u);
    for (int i = 0; i < 16; ++i)
    {
        ASSERT_NE(MakeProxy("marshala"), nullptr);
        ASSERT_NE(MakeProxy("marshalb"), nullptr);
    }
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 32u);

    // Reconciling re-attaches instances and edits badge classes, both of which recompute style,
    // and the verdict arrives on the reload thread. It is queued — and as ONE task, because the
    // main-thread queue's budget drops to a single task per frame during a hot reload.
    InstallCapturingMarshaller();
    constexpr std::uint64_t kReloadedOwner = 0xBEA9u;
    ASSERT_EQ(RegisterBatch(tags, kReloadedOwner).size(), 2u);
    Types().NotifyReloadCompleted(kNativeOwner);

    EXPECT_EQ(g_Queued.size(), 1u) << "32 elements across 2 tags is still one verdict";
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Pending), 32u)
        << "nothing tree-visible may happen before the task runs";

    DrainQueued();
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 32u);
}

// ---------------------------------------------------------------------------
// The registration window: which thread may claim a tag, and what a refusal costs.
//
// Registration cannot marshal itself. It must return the tag id synchronously — the caller keys
// its tag -> type table on the returned value — and the id cannot even be COMPUTED off-thread,
// because it is read from the same canonical map the claim writes. So the entry points refuse
// off-main instead, and the caller reaches them through a window that runs on the main thread.
//
// These arms drive the predicate directly, which is what lets them assert the contract without a
// frame loop or a CLR.
// ---------------------------------------------------------------------------

TEST_F(ManagedTypeLifecycleTest, RegistrationOffTheMainThreadIsRefusedAndTheRefusalIsObservable)
{
    InstallThreadPredicate();
    ASSERT_FALSE(g_OnMainThread);
    ASSERT_EQ(Types().RefusedOffMainThreadCount(), 0u);

    EXPECT_EQ(Types().RegisterType("OffMainOnly", kNativeOwner), 0u)
        << "claiming a tag writes the factory registry that document builds read";
    EXPECT_EQ(Types().RefusedOffMainThreadCount(), 1u)
        << "a refusal that is not recorded is indistinguishable from a call with nothing to do";
    EXPECT_EQ(Factories().Create("offmainonly"), nullptr)
        << "the refusal must leave no factory behind";
}

TEST_F(ManagedTypeLifecycleTest, AnOffMainBatchClaimsNothingAtAll)
{
    InstallThreadPredicate();

    // The whole batch or none of it: a partial claim would hand back some ids and leave the
    // caller's tag -> type table disagreeing with the registry about the rest.
    const std::string_view tags[] = {"OffBatchA", "OffBatchB", "OffBatchC"};
    const std::vector<StringId> ids = Types().RegisterTypes(tags, kNativeOwner);
    ASSERT_EQ(ids.size(), 3u);
    for (StringId id : ids)
        EXPECT_EQ(id, 0u);

    EXPECT_EQ(Types().CountTypesOwnedBy(kNativeOwner), 0u);
    EXPECT_EQ(Types().RefusedOffMainThreadCount(), 1u) << "one refusal for the batch, not one per tag";
}

TEST_F(ManagedTypeLifecycleTest, TheVerdictOffTheMainThreadIsRefusedAndDecidesNothing)
{
    ASSERT_NE(Register("VerdictThread", kNativeOwner), 0u);
    ASSERT_NE(MakeProxy("verdictthread"), nullptr);
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);

    InstallThreadPredicate();
    Types().NotifyReloadCompleted(kNativeOwner);

    EXPECT_EQ(Types().RefusedOffMainThreadCount(), 1u);
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Pending), 1u)
        << "a refused verdict decides nothing - the element is still merely waiting, not Faulted";
}

TEST_F(ManagedTypeLifecycleTest, ABatchLostToTheWrongThreadComesBackOnTheNextVerdict)
{
    ASSERT_NE(Register("Stalled", kNativeOwner), 0u);
    ASSERT_NE(MakeProxy("stalled"), nullptr);
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);

    // A reload whose registration landed off the main thread - the watchdog case. Refused.
    InstallThreadPredicate();
    EXPECT_EQ(Types().RegisterType("Stalled", 0xBEA1u), 0u);
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Pending), 1u);

    // THE COST OF REFUSING IS BOUNDED, NOT PERMANENT, and this is the property that makes
    // refusing an acceptable answer at all. The next reload registers on the main thread, and its
    // verdict revives the element even though the owner that defined it is long retired - the
    // revive branch is deliberately not owner-filtered.
    g_OnMainThread = true;
    constexpr std::uint64_t kLaterOwner = 0xBEA2u;
    ASSERT_NE(Register("Stalled", kLaterOwner), 0u);
    Types().NotifyReloadCompleted(kLaterOwner);

    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 1u);
    EXPECT_EQ(Types().RefusedOffMainThreadCount(), 1u) << "only the one refused call";
}

TEST_F(ManagedTypeLifecycleTest, OrphaningOffTheMainThreadIsRefusedAndSweepsNothing)
{
    // The unload seam performs the symmetric, larger write of the registry registration is
    // guarded for: it unregisters a whole owner's factories, aliases and attribute handlers while
    // a document build may be reading the same maps.
    ASSERT_NE(Register("UnloadThread", kNativeOwner), StringId(0));
    ManagedElementProxy* proxy = MakeProxy("unloadthread");
    ASSERT_NE(proxy, nullptr);

    InstallThreadPredicate();
    ASSERT_FALSE(g_OnMainThread);
    EXPECT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 0u);

    EXPECT_EQ(Types().RefusedOffMainThreadCount(), 1u);
    EXPECT_EQ(proxy->State(), ManagedTypeState::Live) << "a refused sweep must change nothing";
    EXPECT_TRUE(Factories().HasFactory("unloadthread"))
        << "the factories the UI thread reads must still be there";
}

TEST_F(ManagedTypeLifecycleTest, InstallationIsFirstWinsAndASecondInstallIsRefusedObservably)
{
    // The defining language installs these from whichever thread loaded it, while the main thread
    // may already be invoking them. Replacing a live std::function frees the callable under its
    // caller; refusing every install after the first is what removes that hazard, so the refusal
    // has to be real rather than a last-writer-wins overwrite.
    ASSERT_EQ(Types().RefusedReinstallCount(), 0u);

    ManagedElementCallbacks other{};
    other.Create = [](std::uint64_t, std::uint64_t) -> std::intptr_t { return 999; };
    Types().SetCallbacks(other); // the fixture already installed StubRuntime's

    EXPECT_EQ(Types().RefusedReinstallCount(), 1u);
    EXPECT_EQ(Types().Callbacks().Create, &StubRuntime::Create)
        << "the first installation must still be the one in force";

    InstallCapturingMarshaller();
    InstallCapturingMarshaller();
    InstallThreadPredicate();
    InstallThreadPredicate();
    EXPECT_EQ(Types().RefusedReinstallCount(), 3u) << "one per refused install, marshaller included";
}

TEST_F(ManagedTypeLifecycleTest, TheSameCallIsRefusedOutsideTheWindowAndSucceedsInsideIt)
{
    InstallThreadPredicate();

    // Outside: refused.
    EXPECT_EQ(Types().RegisterType("WindowGated", kNativeOwner), 0u);

    // Inside: the identical call, allowed. The window is the only thing that changed.
    StubRuntime::s_Window = []
    {
        g_OnMainThread = true;
        const StringId id = Types().RegisterType("WindowGated", kNativeOwner);
        EXPECT_NE(id, 0u) << "the window IS the main thread; registration belongs here";
        g_OnMainThread = false;
    };
    Types().RequestRegistrationWindow();

    EXPECT_EQ(StubRuntime::s_WindowsRun, 1);
    EXPECT_EQ(Types().CountTypesOwnedBy(kNativeOwner), 1u);
}

TEST_F(ManagedTypeLifecycleTest, TheRegistrationWindowClaimsAndDecidesInOneMainThreadSlot)
{
    ASSERT_NE(Register("Windowed", kNativeOwner), 0u);
    ASSERT_NE(MakeProxy("windowed"), nullptr);
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);

    InstallCapturingMarshaller();
    DrainQueued(); // the orphan badge task, so the counts below are about the window alone
    ASSERT_TRUE(g_Queued.empty());

    // What the defining language does with its slot: claim every tag the reload brought back,
    // then close the verdict. Both are main-thread-only entry points, called synchronously.
    constexpr std::uint64_t kReloadedOwner = 0xBEA7u;
    StubRuntime::s_Window = []
    {
        ASSERT_NE(Register("Windowed", kReloadedOwner), 0u);
        Types().NotifyReloadCompleted(kReloadedOwner);
    };

    // Asked for from the reload thread: queued, and nothing has happened yet.
    Types().RequestRegistrationWindow();
    EXPECT_EQ(StubRuntime::s_WindowsRun, 0);
    EXPECT_EQ(g_Queued.size(), 1u);
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Pending), 1u);

    DrainQueued();

    EXPECT_EQ(StubRuntime::s_WindowsRun, 1);
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 1u)
        << "the verdict ran INSIDE the window, not a frame later";
    EXPECT_TRUE(g_Queued.empty())
        << "a verdict raised from inside the window must run inline, not re-queue itself";
}

// ---------------------------------------------------------------------------
// A refused tag claim says who holds the tag, and what that holder is.
//
// The registry's own refusal names both owner ids and stops there, which is enough for the
// ordinary collision and not enough for the one that hurts: a load context that LEAKED keeps its
// tag for the life of the process, and every later registration of that tag is refused with a
// message indistinguishable from a transient one. The claimant's elements then never update,
// never orphan and never badge — silently, forever.
//
// The leak itself is upstream of this machinery. What is answerable here is that the refusal
// reports the holder and whether the engine still considers its context live, so a poisoned
// session is diagnosable from its own log rather than by reading this file.
// ---------------------------------------------------------------------------

TEST_F(ManagedTypeLifecycleTest, ARefusedClaimNamesAHolderWhoseContextIsStillLive)
{
    // The poisoned-tag shape: the holder acquired an id and never released it, which is what a
    // leaked load context looks like from here — indistinguishable from one that is merely busy,
    // and that is exactly what the log has to say rather than imply.
    const std::uint64_t holder = Owners().Acquire();
    ASSERT_NE(Register("Poisoned", holder), StringId(0));
    ASSERT_EQ(Types().RefusedTagClaimCount(), 0u);

    constexpr std::uint64_t kClaimant = 0xBED1u;
    EXPECT_EQ(Types().RegisterType("Poisoned", kClaimant), StringId(0));

    EXPECT_EQ(Types().RefusedTagClaimCount(), 1u)
        << "a refusal that is not recorded is indistinguishable from a claim with nothing to do";
    ASSERT_TRUE(Types().LastRefusedTagClaim().has_value());
    EXPECT_EQ(Types().LastRefusedTagClaim()->Holder, holder) << "the refusal must name the holder";
    EXPECT_TRUE(Types().LastRefusedTagClaim()->HolderContextLive)
        << "a leaked context reads as live, which is the fact that makes the tag permanently held";
    EXPECT_EQ(Types().CountTypesOwnedBy(kClaimant), 0u);
}

TEST_F(ManagedTypeLifecycleTest, ARefusedClaimReportsAHolderWhoseContextHasBeenReleased)
{
    // The other reading of the same refusal, and the reason the liveness bit is worth carrying:
    // a holder whose context released is a stale registration the next reload can clear, not a
    // dead end. The tag is still refused — releasing an owner id does not sweep its types, the
    // orphan pass does — but the diagnosis differs.
    const std::uint64_t holder = Owners().Acquire();
    ASSERT_NE(Register("Stale", holder), StringId(0));
    ASSERT_TRUE(Owners().Release(holder));

    EXPECT_EQ(Types().RegisterType("Stale", 0xBED3u), StringId(0));
    ASSERT_TRUE(Types().LastRefusedTagClaim().has_value());
    EXPECT_EQ(Types().LastRefusedTagClaim()->Holder, holder);
    EXPECT_FALSE(Types().LastRefusedTagClaim()->HolderContextLive);
}

TEST_F(ManagedTypeLifecycleTest, EveryRefusedClaimIsCountedThoughTheLogNamesItOnce)
{
    // The retry shape: a reload re-registers the whole batch, so the same refusal recurs for the
    // life of the session. The log is deduplicated per (tag, holder) so it does not repeat that
    // line every reload; the COUNT is what stays true, and it is the half a test can assert.
    const std::uint64_t holder = Owners().Acquire();
    ASSERT_NE(Register("Repeated", holder), StringId(0));

    for (int i = 0; i < 50; ++i)
        EXPECT_EQ(Types().RegisterType("Repeated", 0xBED5u), StringId(0));
    EXPECT_EQ(Types().RefusedTagClaimCount(), 50u);
}

TEST_F(ManagedTypeLifecycleTest, ClaimingAnEngineTagIsRefusedAndNamesTheEngine)
{
    // The built-in case, which must not be reported as a load context at all: owner 0 has no
    // context to be alive or dead, and telling a developer to wait for it to unload would be
    // advice about something that can never happen.
    // Registered here rather than assumed: which suites have run before this one in a
    // single-process sweep decides whether the built-ins are up, and an arm whose subject
    // depends on test order is not an instrument. The call is idempotent.
    UIRegistration::RegisterBuiltInControls();
    ASSERT_TRUE(Factories().HasFactory("button"));

    EXPECT_EQ(Types().RegisterType("Button", 0xBED7u), StringId(0));
    ASSERT_TRUE(Types().LastRefusedTagClaim().has_value());
    EXPECT_EQ(Types().LastRefusedTagClaim()->Holder, 0u);
    EXPECT_FALSE(Types().LastRefusedTagClaim()->HolderContextLive);
}

// ---------------------------------------------------------------------------
// A registration batch completing is itself a verdict-worthy event.
//
// The verdict an unloading context announces covers only owners that HAD something to unload.
// Restoring a deleted type is the case that falls outside it: the domain carrying the deleted
// type registered nothing, so nothing unloaded, so nothing announced a verdict — and the owner
// that just claimed the tag back reconciled nothing. The element stayed faulted until the next
// unrelated edit, which is the two-reload recovery the live loop measured.
//
// So the window closes the batch it ran: every owner that claimed a tag inside it gets a
// verdict, whether or not anyone announced one. The revive branch is not owner-filtered, so an
// orphan recovers regardless of which context orphaned it.
// ---------------------------------------------------------------------------

TEST_F(ManagedTypeLifecycleTest, RestoringADeletedTypeRecoversOnTheFirstReloadThatClaimsItsTag)
{
    const StringId tagId = Register("Restored", kNativeOwner);
    ASSERT_NE(tagId, StringId(0));
    ManagedElementProxy* proxy = MakeProxy("restored");
    ASSERT_NE(proxy, nullptr);
    const std::uint64_t instanceId = proxy->GetInstanceId();

    // Reload one: the type's file is deleted. Its context unloads and its verdict faults the
    // element, because nothing brought the tag back.
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kNativeOwner), 1u);
    StubRuntime::Retract(tagId);
    Types().NotifyReloadCompleted(kNativeOwner);
    ASSERT_EQ(proxy->State(), ManagedTypeState::Faulted);
    ASSERT_TRUE(proxy->HasClass(ManagedElementTypes::kMissingClass));

    // Reload two: the file is restored. THE CONTEXT THAT UNLOADED HERE OWNED NO ELEMENT TYPE —
    // the deleted file was its only one — so no owner is awaiting a verdict and the defining
    // language announces none. All that happens in this window is the fresh claim.
    constexpr std::uint64_t kRestoredOwner = 0xBEC1u;
    StubRuntime::s_Window = [] { ASSERT_NE(Register("Restored", kRestoredOwner), StringId(0)); };
    Types().RequestRegistrationWindow();
    ASSERT_EQ(StubRuntime::s_WindowsRun, 1);

    EXPECT_EQ(proxy->State(), ManagedTypeState::Live)
        << "one reload, not two: the window that claimed the tag owes its own verdict";
    EXPECT_EQ(proxy->GetInstanceId(), instanceId) << "the SAME element, not a rebuilt one";
    EXPECT_EQ(proxy->Owner(), kRestoredOwner) << "and it joins the context that holds its tag now";
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kMissingClass));
    EXPECT_EQ(StubRuntime::s_Declined, 0)
        << "the verdict ran after the claim returned, so the caller could answer for the tag";
}

TEST_F(ManagedTypeLifecycleTest, AWindowsOwnVerdictDoesNotFaultAnotherContextsPendingElements)
{
    // The cost of issuing a verdict nobody asked for, and the bound on it: the fault branch is
    // owner-filtered, so a claim by one context says nothing about another's elements — which
    // are merely still loading and would be badged as missing if it did.
    const StringId theirs = Register("StillLoading", 0xBEC3u);
    ASSERT_NE(theirs, StringId(0));
    ManagedElementProxy* waiting = MakeProxy("stillloading");
    ASSERT_NE(waiting, nullptr);
    ASSERT_EQ(Types().OrphanTypesOwnedBy(0xBEC3u), 1u);
    StubRuntime::Retract(theirs);
    ASSERT_EQ(waiting->State(), ManagedTypeState::Pending);

    constexpr std::uint64_t kUnrelatedOwner = 0xBEC5u;
    StubRuntime::s_Window = [] { ASSERT_NE(Register("Unrelated", kUnrelatedOwner), StringId(0)); };
    Types().RequestRegistrationWindow();
    ASSERT_EQ(StubRuntime::s_WindowsRun, 1);

    EXPECT_EQ(waiting->State(), ManagedTypeState::Pending)
        << "not this batch's verdict to give";
    EXPECT_FALSE(waiting->HasClass(ManagedElementTypes::kMissingClass));
}

TEST_F(ManagedTypeLifecycleTest, AskingForAWindowWithNothingInstalledQueuesNothing)
{
    // A native-only host has no defining language to call back into, and must not pay a queued
    // task per request to discover that.
    // Installation is first-wins, so a different callback set means starting from nothing
    // installed rather than overwriting what the fixture put in.
    Types().ResetForTests();
    ManagedElementCallbacks callbacks = StubRuntime::Callbacks();
    callbacks.PerformRegistrations = nullptr;
    Types().SetCallbacks(callbacks);
    InstallCapturingMarshaller();

    Types().RequestRegistrationWindow();

    EXPECT_TRUE(g_Queued.empty());
    EXPECT_EQ(StubRuntime::s_WindowsRun, 0);
}
