#include <gtest/gtest.h>
#include <string>
#include <memory>
#include "UI/Parsers/XMLParser.h"
#include "UI/UIElement.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/Controls/Accordion.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/TextField.h"
#include "../Source/UIAttributeAccess.h"

using namespace GameEngine;
using namespace GameEngine::UIParsing;

TEST(XMLParserTests, ParsesSimpleTreeWithIdAndClass) {
    const std::string xml = R"(<?xml version="1.0"?>
<UI id="root" class="btn">
    <Panel id="p1"/>
</UI>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE((bool)root);
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(*root), "UI");
    EXPECT_EQ(root->GetId(), "root");
    EXPECT_TRUE(root->HasClass("btn"));
    ASSERT_EQ(root->GetChildren().size(), 1u);
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(*root->GetChildren()[0]), "Panel");
    EXPECT_EQ(root->GetChildren()[0]->GetId(), "p1");
}

TEST(XMLParserTests, NameAttributeAliasesToId) {
    const std::string xml = R"(<UIElement name="alias-id" />)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE((bool)root);
    EXPECT_EQ(root->GetId(), "alias-id");
}

TEST(XMLParserTests, LabelInnerTextAndTextAttributeBothWork) {
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<UIElement>
        <label id="a">Hello</label>
        <label id="b" text="World"/>
    </UIElement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE((bool)root);
    ASSERT_EQ(root->GetChildren().size(), 2u);

    auto* a = dynamic_cast<Label*>(root->GetChildren()[0].get());
    auto* b = dynamic_cast<Label*>(root->GetChildren()[1].get());
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->GetText(), "Hello");
    EXPECT_EQ(b->GetText(), "World");
}


TEST(XMLParserTests, TextAreaReadonlyAndTextInputResolveFromMarkup)
{
    // Both entries reach a document only through the built-in tag set, and both fail QUIETLY
    // if they go missing: an unbound `readonly` is dropped by ApplyAttributes and an
    // unregistered <textinput> falls back to a bare UIElement. So the document path is what
    // has to assert them, not the presence of a registration call.
    UIRegistration::RegisterBuiltInControls();

    const std::string xml = R"(<UIElement>
        <textarea id="ro" readonly="true" value="locked"/>
        <textarea id="rw" value="editable"/>
        <textinput id="ti"/>
    </UIElement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE((bool)root);

    auto* readOnly = dynamic_cast<TextArea*>(root->FindById("ro"));
    ASSERT_NE(readOnly, nullptr);
    EXPECT_TRUE(readOnly->IsReadOnly());

    // The negative control: without it, a SetReadOnly bound to the wrong default would pass
    // the arm above for the wrong reason.
    auto* writable = dynamic_cast<TextArea*>(root->FindById("rw"));
    ASSERT_NE(writable, nullptr);
    EXPECT_FALSE(writable->IsReadOnly());

    EXPECT_NE(dynamic_cast<TextInput*>(root->FindById("ti")), nullptr);
}

TEST(XMLParserTests, UnknownTagFallsBackToUIElementWithOriginalName)
{
    const std::string xml = R"(<bogus id='x' class='c' />)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE((bool)root);
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(*root), "bogus");
    EXPECT_EQ(root->GetId(), "x");
    EXPECT_TRUE(root->HasClass("c"));
}

TEST(XMLParserTests, UMLElementBaseTagIsRegistered)
{
    UIRegistration::RegisterBuiltInControls();
    const std::string xml = R"(<uielement id='root'><label id='l' text='ok'/></uielement>)";
    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE((bool)root);
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(*root), "uielement");
    ASSERT_EQ(root->GetChildren().size(), 1u);
    auto* lbl = dynamic_cast<Label*>(root->GetChildren()[0].get());
    ASSERT_NE(lbl, nullptr);
    EXPECT_EQ(lbl->GetText(), "ok");
}

TEST(XMLParserTests, AccordionModeParsesFromAttribute)
{
    UIRegistration::RegisterBuiltInControls();
    const std::string xml = R"(<UIElement>
        <Accordion id="acc" mode="exclusive" />
    </UIElement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE((bool)root);

    UIElement* el = root->FindById("acc");
    ASSERT_NE(el, nullptr);
    auto* acc = dynamic_cast<Accordion*>(el);
    ASSERT_NE(acc, nullptr);
    EXPECT_EQ(acc->GetMode(), Accordion::Mode::Exclusive);
}

TEST(XMLParserTests, DropdownModeParsesFromAttribute)
{
    UIRegistration::RegisterBuiltInControls();
    const std::string xml = R"(<UIElement>
        <Dropdown id="dd" mode="os" />
    </UIElement>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE((bool)root);

    UIElement* el = root->FindById("dd");
    ASSERT_NE(el, nullptr);
    auto* dd = dynamic_cast<Dropdown*>(el);
    ASSERT_NE(dd, nullptr);
    EXPECT_EQ(dd->GetMode(), Dropdown::Mode::Native);
}


TEST(XMLParserTests, FindByIdUsesCanonicalId)
{
    const std::string xml = R"(<UI id="root">
        <Panel name="childA" />
        <Panel id="x" name="alias" />
    </UI>)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE((bool)root);

    UIElement* byNameOnly = root->FindById("childA");
    ASSERT_NE(byNameOnly, nullptr);
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(*byNameOnly), "Panel");
    EXPECT_EQ(byNameOnly->GetId(), "childA");

    // When both id and name are provided, id remains canonical (name is ignored).
    UIElement* byAlias = root->FindById("alias");
    EXPECT_EQ(byAlias, nullptr);

    UIElement* byId = root->FindById("x");
    ASSERT_NE(byId, nullptr);
    EXPECT_EQ(UIAttributeAccess::GetDebugTypeName(*byId), "Panel");
    EXPECT_EQ(byId->GetId(), "x");
}
