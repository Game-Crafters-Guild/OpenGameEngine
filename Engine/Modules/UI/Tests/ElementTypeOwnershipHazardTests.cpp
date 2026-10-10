// Hazards found by adversarial review of the slice-3a branch, each locked by the property it
// violated rather than by the shape of the probe that found it.
//
// The unifying theme is that a registry which lets anyone register anything is not made safe
// by a careful unregister. Three of these are ways a registration could reach somewhere it had
// no business reaching; the fourth is what happens when the code that reacts to a registration
// re-enters the index it is walking.

#include "Scripting/ManagedElementTypes.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "Types/StringId.h"
#include "../Source/UIAttributeAccess.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Scripting;

namespace
{
UIRegistration::ElementFactoryRegistry& Reg()
{
    return UIRegistration::ElementFactoryRegistry::Instance();
}

// A distinct C++ class per tag family, so a test that means "different type" gets one.
class HazardTypeA : public UIElement
{
};
class HazardTypeB : public UIElement
{
};

// Callback state for the re-entrancy arms. The proxies these create/destroy are owned by the
// test, so the arm controls exactly when the index is mutated.
std::vector<std::unique_ptr<UIElement>>* g_Sink = nullptr;
bool g_CreateSpawns = false;
int g_SpawnBudget = 0;
std::string g_SpawnTag;

std::intptr_t SpawningCreate(std::uint64_t, std::uint64_t)
{
    if (g_CreateSpawns && g_SpawnBudget > 0 && g_Sink)
    {
        --g_SpawnBudget;
        // A managed constructor that instantiates another element of its own type — an
        // ordinary thing for a control that builds a subtree.
        if (std::unique_ptr<UIElement> spawned = Reg().Create(g_SpawnTag))
            g_Sink->push_back(std::move(spawned));
    }
    return 7;
}

// Counts every Release the seam performs, so a handle released twice is visible
// as a number rather than as heap damage nobody sees.
int g_ReleaseCalls = 0;
// When non-zero, the next Release re-enters the unload sweep for that owner —
// the shape a managed Release takes when dropping the last reference runs
// finalizers that tear down another context.
std::uint64_t g_SweepOwnerOnRelease = 0;

void CountingSweepingRelease(std::intptr_t)
{
    ++g_ReleaseCalls;
    if (g_SweepOwnerOnRelease == 0)
        return;
    const std::uint64_t owner = g_SweepOwnerOnRelease;
    g_SweepOwnerOnRelease = 0; // one-shot: the sweep releases too, and would recurse
    ManagedElementTypes::Instance().OrphanTypesOwnedBy(owner);
}

// A managed constructor that destroys the very element it is being constructed
// for. Addressed by instance id rather than by position, so the arm hits the
// proxy the walk is currently inside no matter what order the index yields.
bool g_CreateDestroysItsOwnProxy = false;

std::intptr_t DestroyingCreate(std::uint64_t, std::uint64_t instanceId)
{
    if (g_CreateDestroysItsOwnProxy && g_Sink)
    {
        g_CreateDestroysItsOwnProxy = false; // one-shot: the destructor calls back in here
        const auto it = std::find_if(g_Sink->begin(), g_Sink->end(),
                                     [instanceId](const std::unique_ptr<UIElement>& el) {
                                         return el && el->GetInstanceId() == instanceId;
                                     });
        if (it != g_Sink->end())
            g_Sink->erase(it);
    }
    return 9;
}

bool g_ReleaseDestroys = false;

void DestroyingRelease(std::intptr_t)
{
    if (!g_ReleaseDestroys || !g_Sink || g_Sink->empty())
        return;
    g_ReleaseDestroys = false; // the destructor calls Release again; do not recurse
    // Destroy from the FRONT. Whatever the index's internal order, removing an entry that a
    // walk has not yet reached is the case that can make a walk skip one; destroying the most
    // recently added entry is the case most likely to be harmless, and would prove nothing.
    g_Sink->erase(g_Sink->begin());
    g_ReleaseDestroys = true;
}

class ElementTypeOwnershipHazardTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_Sink = nullptr;
        g_CreateSpawns = false;
        g_ReleaseDestroys = false;
        g_ReleaseCalls = 0;
        g_SweepOwnerOnRelease = 0;
        g_CreateDestroysItsOwnProxy = false;
        g_SpawnBudget = 0;
        ManagedElementTypes::Instance().ResetForTests();
        ManagedElementTypes::SetBadgeEnabledForTest(false);
    }

    void TearDown() override
    {
        m_Owned.clear();
        ManagedElementTypes::Instance().ResetForTests();
        ManagedElementTypes::SetBadgeEnabledForTest(std::nullopt);
        g_Sink = nullptr;
    }

    static ManagedElementTypes& Types() { return ManagedElementTypes::Instance(); }

    std::vector<std::unique_ptr<UIElement>> m_Owned;
};
} // namespace

// ---------------------------------------------------------------------------
// A registration may not reach a tag that is not its own.
// ---------------------------------------------------------------------------

TEST_F(ElementTypeOwnershipHazardTest, AnOwnerCannotClaimAnEngineTagAndThenSweepIt)
{
    UIRegistration::RegisterBuiltInControls();
    ASSERT_TRUE(Reg().HasFactory("button"));

    // The whole attack in one line: register a managed type under a built-in's name. If it
    // succeeds, the tag becomes owner-stamped and the next unload deletes the engine control
    // for the rest of the process.
    const std::uint64_t owner = 0xB10Cull;
    EXPECT_EQ(Types().RegisterType("Button", owner), StringId(0))
        << "a non-zero owner must not be able to take over an engine tag";

    // The built-in is untouched in every respect that matters.
    EXPECT_TRUE(Reg().HasFactory("button"));
    std::unique_ptr<UIElement> button = Reg().Create("button");
    ASSERT_NE(button, nullptr);
    EXPECT_NE(dynamic_cast<Button*>(button.get()), nullptr)
        << "Create must still mint the engine control, not a proxy";
    EXPECT_TRUE(Reg().IsSameType(*button, "button"));

    // ...and the sweep the attack depended on finds nothing to take.
    EXPECT_EQ(Types().OrphanTypesOwnedBy(owner), 0u);
    Reg().UnregisterFactoriesOwnedBy(owner);
    EXPECT_TRUE(Reg().HasFactory("button"))
        << "the engine's button survived the unload of an unrelated owner";

    m_Owned.push_back(std::move(button));
}

TEST_F(ElementTypeOwnershipHazardTest, OneOwnerCannotTakeAnotherOwnersTag)
{
    const std::uint64_t first = 0x1111ull;
    const std::uint64_t second = 0x2222ull;
    ASSERT_NE(Types().RegisterType("HazardContested", first), StringId(0));

    EXPECT_EQ(Types().RegisterType("HazardContested", second), StringId(0));
    EXPECT_EQ(Reg().CountFactoriesOwnedBy(first), 1u);
    EXPECT_EQ(Reg().CountFactoriesOwnedBy(second), 0u);

    // The rightful owner may still re-register: reloads do exactly that.
    EXPECT_NE(Types().RegisterType("HazardContested", first), StringId(0));
    EXPECT_EQ(Reg().CountFactoriesOwnedBy(first), 1u);
}

TEST_F(ElementTypeOwnershipHazardTest, RegisterBuiltInControlsDefinesTheTagSetOnlyOnce)
{
    // Every UIManager constructor calls it and so does most of this suite, so the entry point
    // has to survive being called after the module's own initialisation already ran it.
    //
    // The definition counter is the instrument, and it has to be, because NO registry count
    // can express once-only: RegisterFactory assigns by key, so a second definition overwrites
    // every entry with an equal one and every count — factories, aliases, handlers — is
    // invariant by construction. The counts below are a companion check that the set is intact,
    // not evidence about how many times it was built.
    ASSERT_EQ(UIRegistration::BuiltInControlDefinitionCount(), 1u)
        << "the tag set must be defined once, by whichever caller arrives first";

    const std::size_t before = Reg().CountFactoriesOwnedBy(0);
    ASSERT_GT(before, 0u);
    UIRegistration::RegisterBuiltInControls();
    UIRegistration::RegisterBuiltInControls();
    EXPECT_EQ(UIRegistration::BuiltInControlDefinitionCount(), 1u);
    EXPECT_EQ(Reg().CountFactoriesOwnedBy(0), before);
    EXPECT_TRUE(Reg().HasFactory("button"));
    EXPECT_TRUE(Reg().HasFactory("label"));

    std::unique_ptr<UIElement> label = Reg().Create("label");
    ASSERT_NE(label, nullptr);
    EXPECT_NE(dynamic_cast<Label*>(label.get()), nullptr);
    m_Owned.push_back(std::move(label));
}

TEST_F(ElementTypeOwnershipHazardTest, AnAliasCannotSmuggleATagAwayFromItsOwner)
{
    // An alias is a registration too, so it must obey the same rule — otherwise the refusal
    // above is bypassable by aliasing onto the tag you wanted.
    UIRegistration::RegisterBuiltInControls();
    ASSERT_TRUE(Reg().HasFactory("button"));

    UIRegistration::RegisterOwned<HazardTypeA>("HazardAliasSource", 0x3333ull)
        .TagAlias("button");

    std::unique_ptr<UIElement> button = Reg().Create("button");
    ASSERT_NE(button, nullptr);
    EXPECT_NE(dynamic_cast<Button*>(button.get()), nullptr)
        << "an alias registration took over an engine tag";
    m_Owned.push_back(std::move(button));
}

// ---------------------------------------------------------------------------
// A placeholder must not outlive the arrival of the real type.
// ---------------------------------------------------------------------------

TEST_F(ElementTypeOwnershipHazardTest, AnUnknownTagSurvivorStopsMatchingOnceTheTypeRegisters)
{
    // The order every hot-reload start-up hits: a document is parsed before the assembly that
    // defines its types has registered them, leaving a plain UIElement stamped with the tag.
    UIElement placeholder;
    UIAttributeAccess::SetCreatedTag(placeholder, "HazardLateType");
    EXPECT_TRUE(Reg().IsSameType(placeholder, "hazardlatetype"))
        << "while the tag is unregistered the stamp alone is the identity";

    // The type arrives.
    const std::uint64_t owner = 0x4444ull;
    ASSERT_NE(Types().RegisterType("HazardLateType", owner), StringId(0));

    // The placeholder must now STOP matching, or the reconciler adopts it forever and the real
    // control is never built. This is the one case where the stamp is not enough on its own.
    EXPECT_FALSE(Reg().IsSameType(placeholder, "hazardlatetype"))
        << "a plain placeholder still matched a tag that now has a real type behind it";

    // ...while a real proxy of that type does match.
    std::unique_ptr<UIElement> real = Reg().Create("hazardlatetype");
    ASSERT_NE(real, nullptr);
    EXPECT_TRUE(Reg().IsSameType(*real, "hazardlatetype"));

    // ...and once the type goes away again, the survivor's stamp is the identity once more,
    // which is what keeps orphans alive through a reconcile.
    Types().OrphanTypesOwnedBy(owner);
    EXPECT_TRUE(Reg().IsSameType(placeholder, "hazardlatetype"));
    EXPECT_TRUE(Reg().IsSameType(*real, "hazardlatetype"));

    m_Owned.push_back(std::move(real));
}

TEST_F(ElementTypeOwnershipHazardTest, TypeMatchingDoesNotBreakTheCasesItMustPreserve)
{
    UIRegistration::RegisterBuiltInControls();

    // Hand-built control: no stamp, registered C++ type, must still match via RTTI.
    Button handBuilt;
    EXPECT_TRUE(Reg().IsSameType(handBuilt, "button"));
    EXPECT_FALSE(Reg().IsSameType(handBuilt, "label"));

    // Two tags on ONE shared proxy class still separate on the stamp: the registered-type
    // check passes for both, so the stamp is what tells them apart.
    const std::uint64_t owner = 0x5555ull;
    ASSERT_NE(Types().RegisterType("HazardFoo", owner), StringId(0));
    ASSERT_NE(Types().RegisterType("HazardBar", owner), StringId(0));
    std::unique_ptr<UIElement> foo = Reg().Create("hazardfoo");
    std::unique_ptr<UIElement> bar = Reg().Create("hazardbar");
    ASSERT_NE(foo, nullptr);
    ASSERT_NE(bar, nullptr);
    EXPECT_TRUE(Reg().IsSameType(*foo, "hazardfoo"));
    EXPECT_FALSE(Reg().IsSameType(*foo, "hazardbar"));
    EXPECT_TRUE(Reg().IsSameType(*bar, "hazardbar"));
    EXPECT_FALSE(Reg().IsSameType(*bar, "hazardfoo"));

    m_Owned.push_back(std::move(foo));
    m_Owned.push_back(std::move(bar));
}

// ---------------------------------------------------------------------------
// The index survives callbacks that re-enter it.
// ---------------------------------------------------------------------------

TEST_F(ElementTypeOwnershipHazardTest, TheUnloadSeamFinishesWhenReleaseDestroysOtherElements)
{
    const std::uint64_t owner = 0x6666ull;
    g_Sink = &m_Owned;

    ManagedElementCallbacks cb{};
    cb.Create = [](std::uint64_t, std::uint64_t) -> std::intptr_t { return 5; };
    cb.Release = &DestroyingRelease;
    Types().SetCallbacks(cb);

    ASSERT_NE(Types().RegisterType("HazardDestructive", owner), StringId(0));
    for (int i = 0; i < 16; ++i)
    {
        std::unique_ptr<UIElement> el = Reg().Create("hazarddestructive");
        ASSERT_NE(el, nullptr);
        m_Owned.push_back(std::move(el));
    }
    ASSERT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 16u);

    g_ReleaseDestroys = true;
    Types().OrphanTypesOwnedBy(owner);
    g_ReleaseDestroys = false;

    // The property, not the count: nothing may still be Live once the unload seam returns.
    // A Live proxy at this point is holding a handle into a context that is about to go.
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 0u)
        << "the unload seam skipped proxies because Release mutated the index it was walking";
}

TEST_F(ElementTypeOwnershipHazardTest, TheVerdictFinishesWhenConstructionCreatesMoreElements)
{
    const std::uint64_t owner = 0x7777ull;
    g_Sink = &m_Owned;
    g_SpawnTag = "hazardspawning";

    ManagedElementCallbacks cb{};
    cb.Create = &SpawningCreate;
    cb.Release = [](std::intptr_t) {};
    Types().SetCallbacks(cb);

    ASSERT_NE(Types().RegisterType("HazardSpawning", owner), StringId(0));
    for (int i = 0; i < 64; ++i)
    {
        std::unique_ptr<UIElement> el = Reg().Create("hazardspawning");
        ASSERT_NE(el, nullptr);
        m_Owned.push_back(std::move(el));
    }
    ASSERT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 64u);

    Types().OrphanTypesOwnedBy(owner);
    ASSERT_EQ(Types().CountProxiesInState(ManagedTypeState::Pending), 64u);

    // The reload's verdict calls Create for each orphan, and each Create adds more proxies to
    // the index being walked. The budget is deliberately far larger than the orphan count so
    // the index grows many times over during the walk.
    g_CreateSpawns = true;
    g_SpawnBudget = 256;
    const std::uint64_t reloadedOwner = owner + 1;
    ASSERT_NE(Types().RegisterType("HazardSpawning", reloadedOwner), StringId(0));
    Types().NotifyReloadCompleted(owner);
    g_CreateSpawns = false;

    // Every orphan must have been re-materialized. One left Pending means the walk lost it.
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Pending), 0u)
        << "re-materialize skipped proxies because construction mutated the index";
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Faulted), 0u);
}

// A proxy running its own destructor is not a live element, and the index is what
// every walk asks. Still indexed across its own Release, it answers "alive" —
// same address, same tag, same handle — so a sweep re-entered from that Release
// orphans it a second time and releases the handle twice. The double release is
// the half that is countable; the writes into a half-destroyed object are the
// half only a checking allocator would report.
TEST_F(ElementTypeOwnershipHazardTest, AProxyLeavesTheIndexBeforeItsOwnReleaseCanReEnterTheSweep)
{
    const std::uint64_t owner = 0x8888ull;
    g_Sink = &m_Owned;

    ManagedElementCallbacks cb{};
    cb.Create = [](std::uint64_t, std::uint64_t) -> std::intptr_t { return 11; };
    cb.Release = &CountingSweepingRelease;
    Types().SetCallbacks(cb);

    ASSERT_NE(Types().RegisterType("HazardReentrantRelease", owner), StringId(0));
    std::unique_ptr<UIElement> el = Reg().Create("hazardreentrantrelease");
    ASSERT_NE(el, nullptr);
    m_Owned.push_back(std::move(el));
    ASSERT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 1u);
    ASSERT_EQ(g_ReleaseCalls, 0);

    g_SweepOwnerOnRelease = owner;
    m_Owned.clear();

    EXPECT_EQ(g_ReleaseCalls, 1)
        << "the handle was released twice: the sweep re-entered from the destructor still found "
           "the destructing proxy in the index and orphaned it again";
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 0u);
}

// Materialize's Create is managed code, and a managed constructor can destroy
// elements — including the one it is being constructed for. The handle it returns
// waits in a local until the proxy is known to still be there; written straight
// into the proxy it would land in a freed block, along with the state, the badge
// and the attribute replay that follow it.
//
// SILENT without a checking allocator — the write goes into freed heap and the
// process dies later, somewhere unrelated. ASan fails it at the write, which is
// the only place it is legible.
TEST_F(ElementTypeOwnershipHazardTest, AConstructorThatDestroysItsOwnElementGetsNoHandleWritten)
{
    const std::uint64_t owner = 0x9999ull;
    g_Sink = &m_Owned;

    ManagedElementCallbacks cb{};
    cb.Create = &DestroyingCreate;
    cb.Release = [](std::intptr_t) {};
    Types().SetCallbacks(cb);

    ASSERT_NE(Types().RegisterType("HazardSelfDestructing", owner), StringId(0));
    for (int i = 0; i < 4; ++i)
    {
        std::unique_ptr<UIElement> el = Reg().Create("hazardselfdestructing");
        ASSERT_NE(el, nullptr);
        m_Owned.push_back(std::move(el));
    }
    ASSERT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 4u);

    Types().OrphanTypesOwnedBy(owner);
    ASSERT_EQ(Types().CountProxiesInState(ManagedTypeState::Pending), 4u);

    // The reload's verdict re-materializes every orphan, and one of those
    // constructors destroys its own element from inside Create.
    const std::uint64_t reloadedOwner = owner + 1;
    ASSERT_NE(Types().RegisterType("HazardSelfDestructing", reloadedOwner), StringId(0));
    g_CreateDestroysItsOwnProxy = true;
    Types().NotifyReloadCompleted(owner);
    g_CreateDestroysItsOwnProxy = false;

    EXPECT_EQ(m_Owned.size(), 3u) << "the attack did not actually destroy an element";
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Live), 3u);
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Pending), 0u)
        << "the walk lost an orphan that was still there";
}

// ---------------------------------------------------------------------------
// One context's reload says nothing about another's.
// ---------------------------------------------------------------------------

TEST_F(ElementTypeOwnershipHazardTest, AReloadCompletingDoesNotFaultAnotherOwnersElements)
{
    // Callbacks that succeed, so the proxies reach Live and can then be orphaned. Without a
    // Create callback a proxy faults at construction, which is a different state machine than
    // the one this arm is about.
    ManagedElementCallbacks cb{};
    cb.Create = [](std::uint64_t, std::uint64_t) -> std::intptr_t { return 3; };
    cb.Release = [](std::intptr_t) {};
    Types().SetCallbacks(cb);

    const std::uint64_t ownerX = 0x8888ull;
    const std::uint64_t ownerY = 0x9999ull;
    ASSERT_NE(Types().RegisterType("HazardOwnerX", ownerX), StringId(0));
    ASSERT_NE(Types().RegisterType("HazardOwnerY", ownerY), StringId(0));

    std::unique_ptr<UIElement> ex = Reg().Create("hazardownerx");
    std::unique_ptr<UIElement> ey = Reg().Create("hazardownery");
    ASSERT_NE(ex, nullptr);
    ASSERT_NE(ey, nullptr);
    auto* px = static_cast<ManagedElementProxy*>(ex.get());
    auto* py = static_cast<ManagedElementProxy*>(ey.get());

    // Both contexts unload; only X's reload completes.
    Types().OrphanTypesOwnedBy(ownerX);
    Types().OrphanTypesOwnedBy(ownerY);
    ASSERT_EQ(px->State(), ManagedTypeState::Pending);
    ASSERT_EQ(py->State(), ManagedTypeState::Pending);

    Types().NotifyReloadCompleted(ownerX);

    EXPECT_EQ(px->State(), ManagedTypeState::Faulted);
    EXPECT_EQ(py->State(), ManagedTypeState::Pending)
        << "owner Y's elements were declared permanently missing by owner X's reload";

    // Y's own reload still resolves it, in either direction.
    ASSERT_NE(Types().RegisterType("HazardOwnerY", ownerY + 1), StringId(0));
    Types().NotifyReloadCompleted(ownerY);
    EXPECT_EQ(py->State(), ManagedTypeState::Live);

    m_Owned.push_back(std::move(ex));
    m_Owned.push_back(std::move(ey));
}

// ---------------------------------------------------------------------------
// Naming: a proxy is named after its own tag, and a swept tag stops naming a live class.
// ---------------------------------------------------------------------------

TEST_F(ElementTypeOwnershipHazardTest, EachManagedTypeReportsItsOwnName)
{
    const std::uint64_t owner = 0xAAAAull;
    ASSERT_NE(Types().RegisterType("HazardHealthBar", owner), StringId(0));
    ASSERT_NE(Types().RegisterType("HazardAmmoCounter", owner), StringId(0));

    std::unique_ptr<UIElement> bar = Reg().Create("hazardhealthbar");
    std::unique_ptr<UIElement> ammo = Reg().Create("hazardammocounter");
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(ammo, nullptr);

    // Both are the same C++ class, so the RTTI reverse map holds one entry for them; the name
    // has to come from the element. This is what the UI-tree inspector and XML export print.
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(*bar), "HazardHealthBar");
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(*ammo), "HazardAmmoCounter");

    // ...and it survives the type going away, which is exactly when a name is most wanted.
    Types().OrphanTypesOwnedBy(owner);
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(*bar), "HazardHealthBar");

    m_Owned.push_back(std::move(bar));
    m_Owned.push_back(std::move(ammo));
}

TEST_F(ElementTypeOwnershipHazardTest, ASweptTagStopsNamingAClassThatIsStillRegistered)
{
    // Two tags on one C++ class, the SURVIVING one registered first — the order in which
    // keeping the reverse-map entry is not enough, because the entry names the swept tag.
    UIRegistration::Register<HazardTypeB>("HazardSurvivor");
    const std::uint64_t owner = 0xBBBBull;
    UIRegistration::RegisterOwned<HazardTypeB>("HazardDoomed", owner);

    ASSERT_EQ(Reg().UnregisterFactoriesOwnedBy(owner), 1u);

    HazardTypeB handBuilt;
    EXPECT_EQ(Reg().GetElementTagId(handBuilt), Reg().CanonicalTagId("hazardsurvivor"))
        << "a hand-built instance reported a tag that no longer exists";
    EXPECT_TRUE(Reg().IsSameType(handBuilt, "hazardsurvivor"));
    EXPECT_FALSE(Reg().IsSameType(handBuilt, "hazarddoomed"));
    EXPECT_EQ(Reg().GetTagForType(typeid(HazardTypeB)), "hazardsurvivor");
}
