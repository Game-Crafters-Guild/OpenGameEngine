#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "UI/Parsers/XMLParser.h"
#include "UI/UIElement.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/Controls/TextField.h"

using namespace GameEngine;
using namespace GameEngine::UIParsing;

namespace {

UIElement* FindById(UIElement* root, const std::string& id)
{
    return root ? root->FindById(id) : nullptr;
}

} // namespace

TEST(TextFieldXmlTests, TextFieldElementParsesToTextField)
{
    UIRegistration::RegisterBuiltInControls();

    const char* xml = R"(
<uielement id="root">
  <TextField id="field" value="Hello" />
</uielement>
)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE(root);

    UIElement* el = FindById(root.get(), "field");
    ASSERT_NE(el, nullptr);

    auto* tf = dynamic_cast<TextField*>(el);
    ASSERT_NE(tf, nullptr);
    EXPECT_EQ(tf->GetValue(), "Hello");
}

TEST(TextFieldXmlTests, InputTagAliasesToTextField)
{
    UIRegistration::RegisterBuiltInControls();

    const char* xml = R"(
<uielement id="root">
  <input id="legacy" value="42" />
</uielement>
)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE(root);

    UIElement* el = FindById(root.get(), "legacy");
    ASSERT_NE(el, nullptr);

    auto* tf = dynamic_cast<TextField*>(el);
    ASSERT_NE(tf, nullptr);
    EXPECT_EQ(tf->GetValue(), "42");
}

// Inspector layouts opt a free-text field into the value-field click behaviour
// declaratively, without a subclass.
TEST(TextFieldXmlTests, SelectAllOnMouseFocusAttributeOverridesTheDefault)
{
    UIRegistration::RegisterBuiltInControls();

    const char* xml = R"(
<uielement id="root">
  <TextField id="default" value="a" />
  <TextField id="optedIn" value="b" selectallonmousefocus="true" />
  <TextField id="optedOut" value="c" selectallonmousefocus="false" />
</uielement>
)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE(root);

    auto* byDefault = dynamic_cast<TextField*>(FindById(root.get(), "default"));
    auto* optedIn = dynamic_cast<TextField*>(FindById(root.get(), "optedIn"));
    auto* optedOut = dynamic_cast<TextField*>(FindById(root.get(), "optedOut"));
    ASSERT_NE(byDefault, nullptr);
    ASSERT_NE(optedIn, nullptr);
    ASSERT_NE(optedOut, nullptr);

    EXPECT_FALSE(byDefault->SelectsAllOnMouseFocus());
    EXPECT_TRUE(optedIn->SelectsAllOnMouseFocus());
    EXPECT_FALSE(optedOut->SelectsAllOnMouseFocus());
}

TEST(TextFieldXmlTests, LowercaseTextfieldTagAlsoWorks)
{
    UIRegistration::RegisterBuiltInControls();

    const char* xml = R"(
<uielement id="root">
  <textfield id="lower" value="x" />
</uielement>
)";

    std::unique_ptr<UIElement> root;
    ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
    ASSERT_TRUE(root);

    UIElement* el = FindById(root.get(), "lower");
    ASSERT_NE(el, nullptr);

    auto* tf = dynamic_cast<TextField*>(el);
    ASSERT_NE(tf, nullptr);
    EXPECT_EQ(tf->GetValue(), "x");
}

