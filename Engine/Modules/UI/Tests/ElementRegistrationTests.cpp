#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/DockPanel.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Scrollbar.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/Registration/ElementRegistration.h"
#include "Types/StringId.h"
#include "../Source/UIAttributeAccess.h"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

using namespace GameEngine;

// ---------------------------------------------------------------------------
// Factory type registration: typeid-based identity via ElementFactoryRegistry.
// ---------------------------------------------------------------------------

TEST(ElementRegistrationTests, IsSameTypeRecognizesButton)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    Button b;
    EXPECT_TRUE(reg.IsSameType(b, "button"));
    EXPECT_FALSE(reg.IsSameType(b, "label"));
}

TEST(ElementRegistrationTests, IsSameTypeRecognizesLabel)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    Label l;
    EXPECT_TRUE(reg.IsSameType(l, "label"));
    EXPECT_FALSE(reg.IsSameType(l, "button"));
}

TEST(ElementRegistrationTests, IsSameTypeRecognizesCheckbox)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    Checkbox cb;
    EXPECT_TRUE(reg.IsSameType(cb, "checkbox"));
}

TEST(ElementRegistrationTests, IsSameTypeRecognizesToggle)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    Toggle t;
    EXPECT_TRUE(reg.IsSameType(t, "toggle"));
}

TEST(ElementRegistrationTests, ReverseLookupReturnsCanonicalTag)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    EXPECT_EQ(reg.GetTagForType(typeid(Button)), "button");
    EXPECT_EQ(reg.GetTagForType(typeid(Label)), "label");
    EXPECT_EQ(reg.GetTagForType(typeid(Slider)), "slider");
    EXPECT_EQ(reg.GetTagForType(typeid(DockPanel)), "dockpanel");
    EXPECT_EQ(reg.GetTagForType(typeid(WeightedPane)), "pane");
    EXPECT_EQ(reg.GetTagForType(typeid(Scrollbar)), "scrollbar");
    EXPECT_EQ(reg.GetTagForType(typeid(UIElement)), "uielement");
}

TEST(ElementRegistrationTests, GetTypeInfoResolvesRegisteredTags)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    const std::type_info* btnTi = reg.GetTypeInfo("button");
    ASSERT_NE(btnTi, nullptr);
    EXPECT_EQ(*btnTi, typeid(Button));

    const std::type_info* lblTi = reg.GetTypeInfo("label");
    ASSERT_NE(lblTi, nullptr);
    EXPECT_EQ(*lblTi, typeid(Label));
}

TEST(ElementRegistrationTests, GetTypeInfoReturnsNullForUnknownTag)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    EXPECT_EQ(reg.GetTypeInfo("nonexistent_widget"), nullptr);
}

// ---------------------------------------------------------------------------
// StringId-based tag identity (GetTagId / GetTagIdForType).
// ---------------------------------------------------------------------------

TEST(ElementRegistrationTests, GetTagIdReturnsConsistentStringIdForCanonicalTags)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();

    // GetTagId must return the HashStringId of the canonical lowercase tag.
    EXPECT_EQ(reg.GetTagId("button"), HashStringId("button"));
    EXPECT_EQ(reg.GetTagId("label"), HashStringId("label"));
    EXPECT_EQ(reg.GetTagId("slider"), HashStringId("slider"));
    EXPECT_EQ(reg.GetTagId("toggle"), HashStringId("toggle"));
    EXPECT_EQ(reg.GetTagId("checkbox"), HashStringId("checkbox"));
    EXPECT_EQ(reg.GetTagId("uielement"), HashStringId("uielement"));

    // Non-zero for all registered types.
    EXPECT_NE(reg.GetTagId("button"), StringId(0));
    EXPECT_NE(reg.GetTagId("label"), StringId(0));
}

TEST(ElementRegistrationTests, GetTagIdResolvesAliasToCanonicalId)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();

    // "input" is a registered alias for TextField. The canonical tag for
    // TextField is "textfield", so the alias must return the same StringId.
    StringId canonical = reg.GetTagId("textfield");
    StringId alias = reg.GetTagId("input");
    EXPECT_NE(canonical, StringId(0));
    EXPECT_EQ(alias, canonical);
}

TEST(ElementRegistrationTests, GetTagIdReturnsZeroForUnknownTag)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    EXPECT_EQ(reg.GetTagId("nonexistent_widget"), StringId(0));
    EXPECT_EQ(reg.GetTagId("bogus"), StringId(0));
}

TEST(ElementRegistrationTests, GetTagIdForTypeMatchesGetTagId)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();

    // The StringId obtained from a runtime typeid must equal the StringId
    // obtained by looking up the tag name string.
    EXPECT_EQ(reg.GetTagIdForType(typeid(Button)), reg.GetTagId("button"));
    EXPECT_EQ(reg.GetTagIdForType(typeid(Label)), reg.GetTagId("label"));
    EXPECT_EQ(reg.GetTagIdForType(typeid(Slider)), reg.GetTagId("slider"));
    EXPECT_EQ(reg.GetTagIdForType(typeid(Toggle)), reg.GetTagId("toggle"));
    EXPECT_EQ(reg.GetTagIdForType(typeid(Checkbox)), reg.GetTagId("checkbox"));
    EXPECT_EQ(reg.GetTagIdForType(typeid(UIElement)), reg.GetTagId("uielement"));
}

TEST(ElementRegistrationTests, GetTagIdForTypeReturnsZeroForUnregisteredType)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();
    // std::string is not a UIElement type -- should return 0.
    EXPECT_EQ(reg.GetTagIdForType(typeid(std::string)), StringId(0));
}

TEST(ElementRegistrationTests, GetTagIdForTypeEqualsHashOfCanonicalTag)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();

    // Verify the round-trip: GetTagForType gives the canonical string,
    // hashing that string must equal GetTagIdForType.
    std::string btnTag = reg.GetTagForType(typeid(Button));
    EXPECT_EQ(reg.GetTagIdForType(typeid(Button)), HashStringId(btnTag));

    std::string lblTag = reg.GetTagForType(typeid(Label));
    EXPECT_EQ(reg.GetTagIdForType(typeid(Label)), HashStringId(lblTag));
}

// ---------------------------------------------------------------------------
// Comprehensive coverage of all built-in controls.
// ---------------------------------------------------------------------------

TEST(ElementRegistrationTests, AllBuiltInControlsHaveConsistentTagIds)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();

    // Every canonical tag must: resolve to a non-zero StringId, equal the
    // deterministic hash of its name, and round-trip through GetTypeInfo ->
    // GetTagIdForType back to the same StringId.
    const std::vector<std::string> knownTags = {
        "uielement",
        "label",
        "button",
        "textfield",
        "textarea",
        "floatfield",
        "intfield",
        "vector3field",
        "checkbox",
        "toggle",
        "slider",
        "dropdown",
        "foldout",
        "accordionitem",
        "accordion",
        "scrollbar",
        "scrollview",
        "treeview",
        "gridview",
        "listview",
        "splitview",
        "splitter",
        "pane",
        "dockspace",
        "dockleaf",
        "docktabbar",
        "docktab",
        "dockoverlay",
        "dockpanel",
    };

    for (const auto& tag : knownTags)
    {
        SCOPED_TRACE(tag);

        // 1) GetTagId returns a non-zero, deterministic hash.
        StringId tagId = reg.GetTagId(tag);
        EXPECT_NE(tagId, StringId(0))
            << "Tag '" << tag << "' should resolve to a non-zero StringId";
        EXPECT_EQ(tagId, HashStringId(tag))
            << "Tag '" << tag << "' StringId should equal HashStringId of tag";

        // 2) The type_info is registered for this tag.
        const std::type_info* ti = reg.GetTypeInfo(tag);
        ASSERT_NE(ti, nullptr)
            << "Tag '" << tag << "' should have a registered type_info";

        // 3) Round-trip: GetTagIdForType(type) must match GetTagId(tag).
        StringId typeId = reg.GetTagIdForType(*ti);
        EXPECT_EQ(typeId, tagId)
            << "GetTagIdForType should match GetTagId for '" << tag << "'";
    }
}

TEST(ElementRegistrationTests, AllBuiltInAliasesResolveToCanonicalTagId)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();

    // Each alias must produce the same StringId as its canonical tag.
    struct AliasEntry { const char* alias; const char* canonical; };
    const AliasEntry aliases[] = {
        {"input",        "textfield"},
        {"floatfield",   "floatfield"},
        {"intfield",     "intfield"},
        {"vector3field", "vector3field"},
        {"checkbox",     "checkbox"},
        {"toggle",       "toggle"},
        {"slider",       "slider"},
        {"dropdown",     "dropdown"},
        {"foldout",      "foldout"},
        {"accordionitem","accordionitem"},
        {"accordion",    "accordion"},
    };

    for (const auto& e : aliases)
    {
        SCOPED_TRACE(std::string(e.alias) + " -> " + e.canonical);
        StringId aliasId     = reg.GetTagId(e.alias);
        StringId canonicalId = reg.GetTagId(e.canonical);
        EXPECT_NE(aliasId, StringId(0));
        EXPECT_EQ(aliasId, canonicalId);
    }
}

TEST(ElementRegistrationTests, TextInputIsRegisteredAsItsOwnTag)
{
    auto& reg = UIRegistration::ElementFactoryRegistry::Instance();

    // The inner single-line editor of TextField and the numeric fields. Only a registration
    // makes CSS type selectors (`.inspector-field TextInput`) resolve and match that node, so
    // losing the tag costs styling rather than an error — nothing else would report it.
    ASSERT_TRUE(reg.HasFactory("textinput"));
    std::unique_ptr<UIElement> el = reg.Create("textinput");
    ASSERT_NE(el, nullptr);
    EXPECT_NE(dynamic_cast<TextInput*>(el.get()), nullptr);
    EXPECT_EQ(reg.GetTagId("textinput"), HashStringId("textinput"));
}

// ---------------------------------------------------------------------------
// GetDebugTypeName (UIAttributeAccess).
// ---------------------------------------------------------------------------

TEST(ElementRegistrationTests, DebugTypeNameReturnsFactoryTag)
{
    Button b;
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(b), "button");

    Label l;
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(l), "label");

    UIElement el;
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(el), "uielement");
}

TEST(ElementRegistrationTests, DebugTypeNamePrefersTagNameOverFactoryLookup)
{
    UIElement el;
    UIAttributeAccess::SetCreatedTag(el, "bogus");
    // m_TagName is preferred over factory lookup, so even though UIElement is
    // registered as "uielement", the created-tag name wins.
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(el), "bogus");
}

TEST(ElementRegistrationTests, DebugTypeNameUsesFactoryWhenNoTagNameSet)
{
    Button btn;
    // No m_TagName set, so factory reverse lookup returns "button".
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(btn), "button");
}
