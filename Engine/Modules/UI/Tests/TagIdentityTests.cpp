// Element type identity is the TAG, not the C++ class.
//
// The distinction is invisible while every registered tag has its own C++ class, and it
// becomes load-bearing the moment one class backs a family of tags — which is what a proxy
// for types registered outside C++ is. Under RTTI identity all such tags collapse to one
// type_info, and three things break at once: reconcile reuses a <MyFoo/> for a <MyBar/> slot,
// the reverse tag map keeps only whichever tag registered last, and CSS `MyFoo { }` styles
// <MyBar/>.
//
// SharedProxy below stands in for that native proxy: two tags, one C++ class, exactly as a
// pair of C#-defined element types would be.

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"
#include "UI/UIElement.h"
#include "Types/StringId.h"
#include "../Source/UIAttributeAccess.h"
#include <gtest/gtest.h>
#include <memory>
#include <string>

using namespace GameEngine;

namespace
{
// One C++ class behind several tags — the shape a native proxy for externally-registered
// element types necessarily has.
class SharedProxy : public UIElement
{
};

// Registered once for the whole test binary. Tag names are namespaced so they cannot collide
// with a real control in the process-wide registry.
struct SharedProxyTags
{
    SharedProxyTags()
    {
        UIRegistration::Register<SharedProxy>("TagIdentityFoo");
        UIRegistration::Register<SharedProxy>("TagIdentityBar");
    }
};

const SharedProxyTags& EnsureRegistered()
{
    static const SharedProxyTags kTags;
    return kTags;
}

UIRegistration::ElementFactoryRegistry& Registry()
{
    EnsureRegistered();
    return UIRegistration::ElementFactoryRegistry::Instance();
}
} // namespace

// ---------------------------------------------------------------------------
// The blocker itself: two tags on one C++ class must not be interchangeable.
// ---------------------------------------------------------------------------

TEST(TagIdentityTests, TwoTagsSharingOneCppClassAreDistinctTypes)
{
    auto& reg = Registry();
    std::unique_ptr<UIElement> foo = reg.Create("tagidentityfoo");
    std::unique_ptr<UIElement> bar = reg.Create("tagidentitybar");
    ASSERT_NE(foo, nullptr);
    ASSERT_NE(bar, nullptr);

    // Each element is its own tag and nothing else. Under RTTI identity both of these
    // elements are `SharedProxy`, so all four assertions would report true.
    EXPECT_TRUE(reg.IsSameType(*foo, "tagidentityfoo"));
    EXPECT_FALSE(reg.IsSameType(*foo, "tagidentitybar"));
    EXPECT_TRUE(reg.IsSameType(*bar, "tagidentitybar"));
    EXPECT_FALSE(reg.IsSameType(*bar, "tagidentityfoo"));
}

TEST(TagIdentityTests, ElementTagIdIsPerElementNotPerCppType)
{
    auto& reg = Registry();
    std::unique_ptr<UIElement> foo = reg.Create("tagidentityfoo");
    std::unique_ptr<UIElement> bar = reg.Create("tagidentitybar");
    ASSERT_NE(foo, nullptr);
    ASSERT_NE(bar, nullptr);

    EXPECT_EQ(reg.GetElementTagId(*foo), reg.CanonicalTagId("tagidentityfoo"));
    EXPECT_EQ(reg.GetElementTagId(*bar), reg.CanonicalTagId("tagidentitybar"));
    EXPECT_NE(reg.GetElementTagId(*foo), reg.GetElementTagId(*bar));

    // The RTTI reverse map is the thing that cannot answer this: both elements are the same
    // C++ class, so it holds one entry — whichever tag registered last. Asserting that it
    // collapses documents WHY element identity may not be read from it.
    EXPECT_EQ(reg.GetTagIdForType(typeid(*foo)), reg.GetTagIdForType(typeid(*bar)));
}

// ---------------------------------------------------------------------------
// Everything that was true before must stay true.
// ---------------------------------------------------------------------------

TEST(TagIdentityTests, ElementBuiltDirectlyInCppFallsBackToRtti)
{
    auto& reg = Registry();
    // Editor panels construct controls directly rather than through the registry, so these
    // carry no creation stamp and RTTI is the only identity they have.
    Button button;
    Label label;
    EXPECT_EQ(button.GetTagId(), StringId(0));
    EXPECT_TRUE(reg.IsSameType(button, "button"));
    EXPECT_FALSE(reg.IsSameType(button, "label"));
    EXPECT_TRUE(reg.IsSameType(label, "label"));
    EXPECT_FALSE(reg.IsSameType(label, "button"));
}

TEST(TagIdentityTests, CreatedElementMatchesItsOwnTagAndItsAliases)
{
    auto& reg = Registry();
    std::unique_ptr<UIElement> button = reg.Create("button");
    ASSERT_NE(button, nullptr);
    EXPECT_NE(button->GetTagId(), StringId(0));
    EXPECT_TRUE(reg.IsSameType(*button, "button"));

    // An alias is the same identity as its canonical tag, in both directions.
    std::unique_ptr<UIElement> pane = reg.Create("pane");
    ASSERT_NE(pane, nullptr);
    const std::string canonical = reg.CanonicalTagLower("pane");
    EXPECT_TRUE(reg.IsSameType(*pane, "pane"));
    EXPECT_TRUE(reg.IsSameType(*pane, canonical));
    EXPECT_EQ(reg.CanonicalTagId("pane"), reg.CanonicalTagId(canonical));
}

TEST(TagIdentityTests, StampedAndUnstampedElementsOfOneTypeAgree)
{
    auto& reg = Registry();
    // The same control reached both ways — parsed from a document and built by hand — must
    // be one type, or a reconcile would replace hand-built controls on every hot-reload.
    std::unique_ptr<UIElement> parsed = reg.Create("button");
    Button handBuilt;
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(reg.GetElementTagId(*parsed), reg.GetElementTagId(handBuilt));
}

// ---------------------------------------------------------------------------
// Unknown tags are identities too — which is what lets an element whose factory went away
// survive a reconcile instead of being rebuilt.
// ---------------------------------------------------------------------------

TEST(TagIdentityTests, UnknownTagElementCarriesItsAuthoredIdentity)
{
    auto& reg = Registry();
    UIElement el;
    UIAttributeAccess::SetCreatedTag(el, "MyWidget");

    EXPECT_EQ(reg.GetElementTagId(el), reg.CanonicalTagId("mywidget"));
    EXPECT_TRUE(reg.IsSameType(el, "mywidget"));
    // Case-insensitively, the way the XML parser resolves tags.
    EXPECT_TRUE(reg.IsSameType(el, "MyWidget"));
    EXPECT_FALSE(reg.IsSameType(el, "button"));
    // The display name keeps the authored case for logs and the UI-tree inspector.
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(el), "MyWidget");
}

TEST(TagIdentityTests, UnidentifiableElementAndEmptyTagMatchNothing)
{
    auto& reg = Registry();
    UIElement plain; // registered as "uielement", so it does have an identity
    EXPECT_TRUE(reg.IsSameType(plain, "uielement"));
    EXPECT_FALSE(reg.IsSameType(plain, ""));
    EXPECT_FALSE(reg.IsSameType(plain, "nothing_is_registered_under_this_tag"));
    EXPECT_EQ(reg.CanonicalTagId(""), StringId(0));
}

// ---------------------------------------------------------------------------
// CSS type selectors read the same identity, so a stylesheet cannot cross-style two tags
// that happen to share a C++ class.
// ---------------------------------------------------------------------------

TEST(TagIdentityTests, CssTypeSelectorDoesNotCrossStyleTagsSharingOneCppClass)
{
    auto& reg = Registry();
    UIRegistration::RegisterBuiltInControls();

    Stylesheet sheet{};
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString(
        "tagidentityfoo { width: 42px; }\n", sheet));

    std::unique_ptr<UIElement> foo = reg.Create("tagidentityfoo");
    std::unique_ptr<UIElement> bar = reg.Create("tagidentitybar");
    ASSERT_NE(foo, nullptr);
    ASSERT_NE(bar, nullptr);

    UIParsing::ElementState st{};
    const auto styledFoo = UIParsing::CSSParser::ComputeStyleFor(*foo, sheet, st);
    const auto styledBar = UIParsing::CSSParser::ComputeStyleFor(*bar, sheet, st);

    EXPECT_TRUE(styledFoo.Layout.Width.IsPx());
    EXPECT_NEAR(styledFoo.Layout.Width.Value, 42.0f, 0.01f);
    // Under RTTI identity both elements report the same tag id, so this rule painted both.
    EXPECT_FALSE(styledBar.Layout.Width.IsPx());
}

TEST(TagIdentityTests, CssTypeSelectorStillMatchesBuiltInControls)
{
    Registry();
    UIRegistration::RegisterBuiltInControls();

    Stylesheet sheet{};
    ASSERT_TRUE(UIParsing::CSSParser::ParseStylesFromString("button { width: 42px; }\n", sheet));

    // Both routes to a Button — parsed from a document and constructed directly — must be
    // styled identically, or moving identity onto the element would have broken every
    // hand-built control in the editor.
    std::unique_ptr<UIElement> parsed = Registry().Create("button");
    ASSERT_NE(parsed, nullptr);
    Button handBuilt;
    Label label;

    UIParsing::ElementState st{};
    const auto styledParsed = UIParsing::CSSParser::ComputeStyleFor(*parsed, sheet, st);
    const auto styledHandBuilt = UIParsing::CSSParser::ComputeStyleFor(handBuilt, sheet, st);
    const auto styledLabel = UIParsing::CSSParser::ComputeStyleFor(label, sheet, st);

    EXPECT_TRUE(styledParsed.Layout.Width.IsPx());
    EXPECT_TRUE(styledHandBuilt.Layout.Width.IsPx());
    EXPECT_NEAR(styledParsed.Layout.Width.Value, 42.0f, 0.01f);
    EXPECT_NEAR(styledHandBuilt.Layout.Width.Value, 42.0f, 0.01f);
    EXPECT_FALSE(styledLabel.Layout.Width.IsPx());
}

// GetTagId answers "what is this REGISTERED tag's id" and CanonicalTagId answers "what is
// this tag's identity, registered or not". They agree wherever both have an answer; keeping
// them distinct is what lets an element outlive its factory.
TEST(TagIdentityTests, GetTagIdAndCanonicalTagIdAgreeForRegisteredTags)
{
    auto& reg = Registry();
    EXPECT_EQ(reg.GetTagId("button"), reg.CanonicalTagId("button"));
    EXPECT_EQ(reg.GetTagId("label"), reg.CanonicalTagId("label"));
    EXPECT_EQ(reg.GetTagId("pane"), reg.CanonicalTagId("pane"));

    EXPECT_EQ(reg.GetTagId("nothing_is_registered_under_this_tag"), StringId(0));
    EXPECT_NE(reg.CanonicalTagId("nothing_is_registered_under_this_tag"), StringId(0));
}
