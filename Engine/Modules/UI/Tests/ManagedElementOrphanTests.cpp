// The orphan policy: what happens to live elements when the type that defined them goes away.
//
// The decided behaviour is inert-but-present. The element is never destroyed, because it may
// hold authored children, layout and scroll state, and deleting it would be data loss to fix
// a cosmetic problem. It drops its instance, gains a dev-only badge CLASS (not a child, which
// would move the very layout the policy exists to preserve), logs once, and re-attaches a
// fresh instance if its type comes back.
//
// The subtle one is Pending vs Faulted. "The tag is absent" is true for the whole duration of
// a reload, so deciding removal from absence alone would report every reload as a removal.
// Only a completed reload turns absence into a fact.
//
// These arms drive the native half with stub callbacks standing in for the managed runtime,
// which is what lets them run in UITests rather than needing a live CoreCLR.

#include "../Source/UIAttributeAccess.h"
#include "Scripting/ManagedElementTypes.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "Types/StringId.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Scripting;

namespace
{
// Stands in for the managed runtime. Handles are dealt out in sequence so a test can tell a
// re-materialized instance from the one it replaced.
//
// It answers only tags it has been TOLD about, which is the contract the real binding is under:
// it resolves tag id -> Type through a table it can only fill with an id the engine has already
// returned to it. A stub that answered unconditionally would be more permissive than the thing
// it stands in for, and would hide any mistake about WHEN the engine asks. Publishing is the
// fixture's Register helper's job, strictly after the registration call returns.
struct StubRuntime
{
    static inline std::unordered_set<std::uint64_t> s_Published;
    static inline std::intptr_t s_NextHandle = 0;
    static inline int s_Created = 0;
    static inline int s_Released = 0;
    static inline int s_Declined = 0;
    static inline bool s_RefuseCreate = false;
    static inline std::vector<std::pair<std::string, std::string>> s_Applied;

    static void Reset()
    {
        s_Published.clear();
        s_NextHandle = 1000;
        s_Created = 0;
        s_Released = 0;
        s_Declined = 0;
        s_RefuseCreate = false;
        s_Applied.clear();
    }

    static void Publish(StringId tagId) { s_Published.insert(static_cast<std::uint64_t>(tagId)); }
    static void Retract(StringId tagId) { s_Published.erase(static_cast<std::uint64_t>(tagId)); }

    static std::intptr_t Create(std::uint64_t tagId, std::uint64_t)
    {
        // The constructor that throws, which is a different failure from the one below: the
        // caller KNOWS the type and cannot build it.
        if (s_RefuseCreate)
            return 0;
        if (s_Published.count(tagId) == 0)
        {
            ++s_Declined;
            return 0;
        }
        ++s_Created;
        return ++s_NextHandle;
    }

    static void ApplyAttribute(std::intptr_t, const char* name, const char* value)
    {
        s_Applied.emplace_back(name ? name : "", value ? value : "");
    }

    static void Release(std::intptr_t) { ++s_Released; }

    static ManagedElementCallbacks Callbacks()
    {
        ManagedElementCallbacks cb{};
        cb.Create = &Create;
        cb.ApplyAttribute = &ApplyAttribute;
        cb.Release = &Release;
        return cb;
    }
};

class ManagedElementOrphanTest : public ::testing::Test
{
protected:
    static constexpr std::uint64_t kOwner = 0xA11Cu;

    void SetUp() override
    {
        StubRuntime::Reset();
        Types().ResetForTests();
        Types().SetCallbacks(StubRuntime::Callbacks());
        ManagedElementTypes::SetBadgeEnabledForTest(true);
    }

    void TearDown() override
    {
        m_Elements.clear();
        Types().ResetForTests();
        ManagedElementTypes::SetBadgeEnabledForTest(std::nullopt);
    }

    static ManagedElementTypes& Types() { return ManagedElementTypes::Instance(); }
    static UIRegistration::ElementFactoryRegistry& Factories()
    {
        return UIRegistration::ElementFactoryRegistry::Instance();
    }

    // Register the way the managed binding does: the caller learns the tag id as the call
    // RETURNS, and can publish its binding only after that.
    static StringId Register(std::string_view tagName, std::uint64_t owner)
    {
        const StringId id = Types().RegisterType(tagName, owner);
        if (id != 0)
            StubRuntime::Publish(id);
        return id;
    }

    // Owns the elements a test creates, so they outlive the registrations that made them —
    // which is the whole point of an orphan.
    std::vector<std::unique_ptr<UIElement>> m_Elements;
};
} // namespace

TEST_F(ManagedElementOrphanTest, OrphanKeepsTheElementItsChildrenAndItsClasses)
{
    ASSERT_NE(Register("HealthBar", kOwner), StringId(0));

    std::unique_ptr<UIElement> el = Factories().Create("healthbar");
    ASSERT_NE(el, nullptr);
    auto* proxy = static_cast<ManagedElementProxy*>(el.get());
    const uint64_t instanceId = proxy->GetInstanceId();
    proxy->AddClass("authored-class");
    proxy->AddChild(std::make_unique<UIElement>());
    proxy->AddChild(std::make_unique<UIElement>());
    const uint64_t firstChildId = proxy->GetChildren()[0]->GetInstanceId();

    ASSERT_EQ(proxy->State(), ManagedTypeState::Live);
    ASSERT_NE(proxy->Handle(), 0);
    ASSERT_EQ(StubRuntime::s_Created, 1);

    EXPECT_EQ(Types().OrphanTypesOwnedBy(kOwner), 1u);

    // Present, inert, and exactly as authored.
    EXPECT_EQ(proxy->GetInstanceId(), instanceId) << "the element must never be destroyed";
    EXPECT_EQ(proxy->State(), ManagedTypeState::Pending);
    EXPECT_EQ(proxy->Handle(), 0) << "the instance handle must be released at the unload seam";
    EXPECT_EQ(StubRuntime::s_Released, 1);
    ASSERT_EQ(proxy->GetChildren().size(), 2u) << "authored children are not the type's to delete";
    EXPECT_EQ(proxy->GetChildren()[0]->GetInstanceId(), firstChildId);
    EXPECT_TRUE(proxy->HasClass("authored-class"));
    EXPECT_TRUE(proxy->HasClass(ManagedElementTypes::kOrphanedClass));

    // Its identity outlives its factory, so a .uxml reconcile still recognises it.
    EXPECT_FALSE(Factories().HasFactory("healthbar"));
    EXPECT_TRUE(Factories().IsSameType(*proxy, "healthbar"));

    m_Elements.push_back(std::move(el));
}

TEST_F(ManagedElementOrphanTest, ReRegisteringTheTypeReAttachesToTheSameElement)
{
    ASSERT_NE(Register("HealthBar", kOwner), StringId(0));

    std::unique_ptr<UIElement> el = Factories().Create("healthbar");
    ASSERT_NE(el, nullptr);
    auto* proxy = static_cast<ManagedElementProxy*>(el.get());
    proxy->AddChild(std::make_unique<UIElement>());
    // Authored the way every element builder authors: the value is stamped ON the element
    // first, and ApplyAttributes then runs the handlers over it. ApplyAttributes does not
    // author anything by itself, so calling it alone would leave the element carrying no
    // attributes at all — which is not a state any document can produce, and re-materialization
    // replays the element's attributes.
    UIAttributeAccess::SetAuthoredAttribute(*proxy, "percent", "40", /*markDirty=*/false);
    Factories().ApplyAttributes(*proxy, {{"percent", "40"}}, std::string{});

    const uint64_t instanceId = proxy->GetInstanceId();
    const uint64_t childId = proxy->GetChildren()[0]->GetInstanceId();
    const std::intptr_t firstHandle = proxy->Handle();

    const StringId tagId = Factories().CanonicalTagId("healthbar");
    ASSERT_EQ(Types().OrphanTypesOwnedBy(kOwner), 1u);
    StubRuntime::Retract(tagId); // the binding goes with the context that defined it
    ASSERT_EQ(proxy->State(), ManagedTypeState::Pending);

    // The reload brings the type back under a NEW owner id, as a fresh load context would, and
    // the verdict that follows is what re-attaches. Registration cannot: at that point the
    // caller has not yet been handed the id it would have to answer for.
    StubRuntime::s_Applied.clear();
    const std::uint64_t reloadedOwner = kOwner + 1;
    ASSERT_NE(Register("HealthBar", reloadedOwner), StringId(0));
    Types().NotifyReloadCompleted(kOwner);

    EXPECT_EQ(StubRuntime::s_Declined, 0) << "the engine asked only once the caller could answer";
    EXPECT_EQ(proxy->State(), ManagedTypeState::Live);
    EXPECT_EQ(proxy->GetInstanceId(), instanceId)
        << "re-materializing must re-attach, not create a replacement element";
    ASSERT_EQ(proxy->GetChildren().size(), 1u);
    EXPECT_EQ(proxy->GetChildren()[0]->GetInstanceId(), childId);
    EXPECT_NE(proxy->Handle(), 0);
    EXPECT_NE(proxy->Handle(), firstHandle) << "a fresh instance, not the released one";
    EXPECT_EQ(StubRuntime::s_Created, 2);

    // Badge cleared, and the document's authored state re-applied to the new instance, which
    // was constructed knowing nothing about this element.
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kOrphanedClass));
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kMissingClass));
    ASSERT_EQ(StubRuntime::s_Applied.size(), 1u);
    EXPECT_EQ(StubRuntime::s_Applied[0].first, "percent");
    EXPECT_EQ(StubRuntime::s_Applied[0].second, "40");

    m_Elements.push_back(std::move(el));
}

TEST_F(ManagedElementOrphanTest, RemovedIsNotDecidedFromAbsenceAlone)
{
    ASSERT_NE(Register("HealthBar", kOwner), StringId(0));
    std::unique_ptr<UIElement> el = Factories().Create("healthbar");
    ASSERT_NE(el, nullptr);
    auto* proxy = static_cast<ManagedElementProxy*>(el.get());

    ASSERT_EQ(Types().OrphanTypesOwnedBy(kOwner), 1u);

    // The reload is in flight. The tag is absent, and absence means nothing yet — an
    // implementation that read removal off the registry would report every reload as one.
    EXPECT_EQ(proxy->State(), ManagedTypeState::Pending);
    EXPECT_TRUE(proxy->HasClass(ManagedElementTypes::kOrphanedClass));
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kMissingClass));

    // The reload completed without the type. NOW absence is a fact.
    Types().NotifyReloadCompleted(kOwner);
    EXPECT_EQ(proxy->State(), ManagedTypeState::Faulted);
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kOrphanedClass));
    EXPECT_TRUE(proxy->HasClass(ManagedElementTypes::kMissingClass));

    // Still not deleted. A faulted element is a visible, debuggable failure, not data loss.
    EXPECT_NE(proxy->GetInstanceId(), 0u);

    // And it still comes back if the type is restored by a LATER reload — a verdict revives
    // whatever its tag has made available again, not only what this reload orphaned.
    ASSERT_NE(Register("HealthBar", kOwner + 1), StringId(0));
    Types().NotifyReloadCompleted(kOwner);
    EXPECT_EQ(proxy->State(), ManagedTypeState::Live);
    EXPECT_FALSE(proxy->HasClass(ManagedElementTypes::kMissingClass));

    m_Elements.push_back(std::move(el));
}

TEST_F(ManagedElementOrphanTest, ReloadCompletingWithTheTypeBackLeavesNothingFaulted)
{
    ASSERT_NE(Register("HealthBar", kOwner), StringId(0));
    std::unique_ptr<UIElement> el = Factories().Create("healthbar");
    ASSERT_NE(el, nullptr);
    auto* proxy = static_cast<ManagedElementProxy*>(el.get());

    ASSERT_EQ(Types().OrphanTypesOwnedBy(kOwner), 1u);
    StubRuntime::Retract(Factories().CanonicalTagId("healthbar"));
    ASSERT_NE(Register("HealthBar", kOwner + 1), StringId(0));
    ASSERT_EQ(proxy->State(), ManagedTypeState::Pending) << "registration alone decides nothing";

    // The ordinary hot-reload shape: every tag claimed during the reload, then one verdict,
    // which must re-attach the type that came back rather than fault it.
    Types().NotifyReloadCompleted(kOwner);
    EXPECT_EQ(proxy->State(), ManagedTypeState::Live);
    EXPECT_EQ(Types().CountProxiesInState(ManagedTypeState::Faulted), 0u);

    m_Elements.push_back(std::move(el));
}

TEST_F(ManagedElementOrphanTest, BadgeIsDevOnlyAndNeverPerturbsLayout)
{
    ASSERT_NE(Register("HealthBar", kOwner), StringId(0));

    // A shipping Player: the diagnostic is off, so the orphan renders as the plain inert
    // container its CSS describes.
    ManagedElementTypes::SetBadgeEnabledForTest(false);
    std::unique_ptr<UIElement> shipping = Factories().Create("healthbar");
    ASSERT_NE(shipping, nullptr);
    auto* shippingProxy = static_cast<ManagedElementProxy*>(shipping.get());
    shippingProxy->AddChild(std::make_unique<UIElement>());
    const std::size_t childCountBefore = shippingProxy->GetChildren().size();

    ASSERT_GE(Types().OrphanTypesOwnedBy(kOwner), 1u);
    EXPECT_EQ(shippingProxy->State(), ManagedTypeState::Pending)
        << "the state machine runs regardless; only the badge is gated";
    EXPECT_FALSE(shippingProxy->HasClass(ManagedElementTypes::kOrphanedClass));
    EXPECT_EQ(shippingProxy->GetChildren().size(), childCountBefore);

    Types().NotifyReloadCompleted(kOwner);
    EXPECT_EQ(shippingProxy->State(), ManagedTypeState::Faulted);
    EXPECT_FALSE(shippingProxy->HasClass(ManagedElementTypes::kMissingClass));

    // A developer build: the badge is a CLASS. It adds no children, so it cannot move the
    // layout the orphan policy exists to preserve.
    ManagedElementTypes::SetBadgeEnabledForTest(true);
    ASSERT_NE(Register("AmmoCounter", kOwner + 2), StringId(0));
    std::unique_ptr<UIElement> dev = Factories().Create("ammocounter");
    ASSERT_NE(dev, nullptr);
    auto* devProxy = static_cast<ManagedElementProxy*>(dev.get());
    devProxy->AddChild(std::make_unique<UIElement>());
    const std::size_t devChildCount = devProxy->GetChildren().size();

    ASSERT_EQ(Types().OrphanTypesOwnedBy(kOwner + 2), 1u);
    EXPECT_TRUE(devProxy->HasClass(ManagedElementTypes::kOrphanedClass));
    EXPECT_EQ(devProxy->GetChildren().size(), devChildCount)
        << "the badge must not inject a child — that would shift the authored content";

    m_Elements.push_back(std::move(shipping));
    m_Elements.push_back(std::move(dev));
}

TEST_F(ManagedElementOrphanTest, AConstructorThatFailsFaultsRatherThanPending)
{
    ASSERT_NE(Register("HealthBar", kOwner), StringId(0));

    StubRuntime::s_RefuseCreate = true;
    std::unique_ptr<UIElement> el = Factories().Create("healthbar");
    ASSERT_NE(el, nullptr);
    auto* proxy = static_cast<ManagedElementProxy*>(el.get());

    // The tag IS registered, so there is no reload to wait for: this one already failed.
    // Pending would promise a recovery that is not coming.
    EXPECT_EQ(proxy->State(), ManagedTypeState::Faulted);
    EXPECT_EQ(proxy->Handle(), 0);
    EXPECT_TRUE(proxy->HasClass(ManagedElementTypes::kMissingClass));

    m_Elements.push_back(std::move(el));
}

TEST_F(ManagedElementOrphanTest, OwnerZeroIsRefusedInBothDirections)
{
    // Owner 0 is the engine. A type registered under it could never be swept, so it is
    // refused at registration rather than accepted and stranded.
    EXPECT_EQ(Register("EngineOwned", 0), StringId(0));
    EXPECT_FALSE(Factories().HasFactory("engineowned"));
    EXPECT_EQ(Types().OrphanTypesOwnedBy(0), 0u);

    // ...and the built-in controls are untouched by an orphan sweep aimed at 0.
    EXPECT_TRUE(Factories().HasFactory("button"));
}

TEST_F(ManagedElementOrphanTest, SweepingOneTypeLeavesAnotherLive)
{
    ASSERT_NE(Register("HealthBar", kOwner), StringId(0));
    ASSERT_NE(Register("AmmoCounter", kOwner + 5), StringId(0));

    std::unique_ptr<UIElement> bar = Factories().Create("healthbar");
    std::unique_ptr<UIElement> ammo = Factories().Create("ammocounter");
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(ammo, nullptr);

    EXPECT_EQ(Types().OrphanTypesOwnedBy(kOwner), 1u);
    EXPECT_EQ(static_cast<ManagedElementProxy*>(bar.get())->State(), ManagedTypeState::Pending);
    EXPECT_EQ(static_cast<ManagedElementProxy*>(ammo.get())->State(), ManagedTypeState::Live)
        << "one context unloading is no reason to orphan another's elements";
    EXPECT_TRUE(Factories().HasFactory("ammocounter"));

    m_Elements.push_back(std::move(bar));
    m_Elements.push_back(std::move(ammo));
}
