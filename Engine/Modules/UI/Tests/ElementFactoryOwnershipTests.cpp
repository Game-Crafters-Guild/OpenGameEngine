// Registrations carry an owner, and an owner can be swept wholesale.
//
// This is what makes a type registered from a load context that later unloads removable at
// all. Two properties are load-bearing and both are pinned here:
//
//  - Owner 0 is the engine, and there is no argument that sweeps it. A caller able to name
//    only its own non-zero owner cannot take a built-in control out of the registry.
//  - A sweep is complete. A tag registration is not one entry: it is the factory, every alias
//    tag pointing at it, and every attribute handler declared alongside it. The handler
//    channel is keyed by attribute NAME and applied to any element carrying that name, so one
//    left behind goes on mutating elements of unrelated types.

#include "UI/Controls/Button.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "Types/StringId.h"
#include <gtest/gtest.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

using namespace GameEngine;

namespace
{
// Stands in for a type registered from outside C++: several tags, one C++ class.
class OwnedProxy : public UIElement
{
public:
    void SetProbe(const std::string& v) { Probe = v; }
    std::string Probe;
};

// A second class so the sweep's shared-type_info handling is exercised rather than assumed.
class OtherOwnedProxy : public UIElement
{
};

// Any non-zero values; owner ids are opaque to the registry.
constexpr std::uint64_t kOwnerA = 1ull;
constexpr std::uint64_t kOwnerB = 2ull;

UIRegistration::ElementFactoryRegistry& Registry()
{
    return UIRegistration::ElementFactoryRegistry::Instance();
}
} // namespace

TEST(ElementFactoryOwnershipTests, SweepTakesTheFactoryItsAliasesAndItsAttributeHandlers)
{
    auto& reg = Registry();
    ASSERT_EQ(reg.CountFactoriesOwnedBy(kOwnerA), 0u) << "test owner id must start clean";

    UIRegistration::RegisterOwned<OwnedProxy>("OwnedAlpha", kOwnerA)
        .Attr("ownedprobe", &OwnedProxy::SetProbe)
        .TagAlias("ownedalias");

    // Canonical tag + alias.
    EXPECT_EQ(reg.CountFactoriesOwnedBy(kOwnerA), 2u);
    EXPECT_EQ(reg.CountAttrHandlersOwnedBy(kOwnerA), 1u);
    EXPECT_TRUE(reg.HasFactory("ownedalpha"));
    EXPECT_TRUE(reg.HasFactory("ownedalias"));

    // The handler really is live before the sweep, so its absence afterwards means something.
    {
        std::unique_ptr<UIElement> el = reg.Create("ownedalpha");
        ASSERT_NE(el, nullptr);
        reg.ApplyAttributes(*el, {{"ownedprobe", "applied"}}, std::string{});
        EXPECT_EQ(static_cast<OwnedProxy&>(*el).Probe, "applied");
    }

    EXPECT_EQ(reg.UnregisterFactoriesOwnedBy(kOwnerA), 2u)
        << "both the canonical tag and its alias are one registration";

    EXPECT_EQ(reg.CountFactoriesOwnedBy(kOwnerA), 0u);
    EXPECT_EQ(reg.CountAttrHandlersOwnedBy(kOwnerA), 0u);
    EXPECT_FALSE(reg.HasFactory("ownedalpha"));
    EXPECT_FALSE(reg.HasFactory("ownedalias")) << "an alias must be swept with its canonical tag";
    EXPECT_EQ(reg.Create("ownedalpha"), nullptr);
    EXPECT_EQ(reg.Create("ownedalias"), nullptr);

    // The surviving half of the hazard: a handler outliving its type would keep applying to
    // any element that happens to carry the attribute name.
    {
        UIElement stranger;
        reg.ApplyAttributes(stranger, {{"ownedprobe", "leaked"}}, std::string{});
        // Nothing to assert on the stranger itself — the handler dynamic_casts and no-ops —
        // so assert on the channel, which is where the leak would live.
        EXPECT_EQ(reg.CountAttrHandlersOwnedBy(kOwnerA), 0u);
    }
}

TEST(ElementFactoryOwnershipTests, EngineRegistrationsCannotBeSwept)
{
    auto& reg = Registry();
    ASSERT_TRUE(reg.HasFactory("button"));
    const StringId buttonId = reg.GetTagId("button");
    ASSERT_NE(buttonId, StringId(0));

    // Owner 0 is the engine, and the sweep refuses it outright rather than obeying.
    EXPECT_EQ(reg.UnregisterFactoriesOwnedBy(0), 0u);

    EXPECT_TRUE(reg.HasFactory("button"));
    EXPECT_TRUE(reg.HasFactory("label"));
    EXPECT_EQ(reg.GetTagId("button"), buttonId);
    std::unique_ptr<UIElement> button = reg.Create("button");
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(reg.IsSameType(*button, "button"));
}

TEST(ElementFactoryOwnershipTests, SweepingOneOwnerLeavesAnotherOwnersRegistrationIntact)
{
    auto& reg = Registry();
    ASSERT_EQ(reg.CountFactoriesOwnedBy(kOwnerB), 0u);

    UIRegistration::RegisterOwned<OtherOwnedProxy>("OwnedBeta", kOwnerB);
    // A third tag on the SAME C++ class as owner B's, registered by the engine. Sweeping B
    // must not blind the RTTI reverse map that the engine's tag still depends on.
    UIRegistration::Register<OtherOwnedProxy>("OwnedGammaEngine");

    EXPECT_EQ(reg.CountFactoriesOwnedBy(kOwnerB), 1u);
    EXPECT_EQ(reg.UnregisterFactoriesOwnedBy(kOwnerB), 1u);

    EXPECT_FALSE(reg.HasFactory("ownedbeta"));
    EXPECT_TRUE(reg.HasFactory("ownedgammaengine"));
    // The shared C++ class is still registered under the engine's tag, so its reverse entry
    // must have survived — a hand-built OtherOwnedProxy still has an identity.
    OtherOwnedProxy handBuilt;
    EXPECT_EQ(reg.GetElementTagId(handBuilt), reg.CanonicalTagId("ownedgammaengine"));
    EXPECT_TRUE(reg.IsSameType(handBuilt, "ownedgammaengine"));
}

TEST(ElementFactoryOwnershipTests, ElementsOutliveTheirUnregisteredFactory)
{
    auto& reg = Registry();
    const std::uint64_t owner = 3ull;
    ASSERT_EQ(reg.CountFactoriesOwnedBy(owner), 0u);

    UIRegistration::RegisterOwned<OwnedProxy>("OwnedTransient", owner);
    std::unique_ptr<UIElement> el = reg.Create("ownedtransient");
    ASSERT_NE(el, nullptr);
    const StringId stamped = el->GetTagId();
    ASSERT_NE(stamped, StringId(0));

    EXPECT_EQ(reg.UnregisterFactoriesOwnedBy(owner), 1u);

    // The element keeps the identity it was created with. This is what an orphaned element
    // depends on: with no factory left, its tag is the only thing that still knows what it is,
    // and it must keep matching that tag so a reconcile preserves it instead of rebuilding it.
    EXPECT_EQ(el->GetTagId(), stamped);
    EXPECT_EQ(reg.GetElementTagId(*el), reg.CanonicalTagId("ownedtransient"));
    EXPECT_TRUE(reg.IsSameType(*el, "ownedtransient"));
    // ...while the tag is no longer a REGISTERED one, which is the difference between
    // GetTagId and CanonicalTagId.
    EXPECT_EQ(reg.GetTagId("ownedtransient"), StringId(0));
}
