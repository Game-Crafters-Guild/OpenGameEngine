#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

#include "UI/Layout/DockConfigElements.h"
#include "UI/Layout/EditorDockConfigParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"

using namespace GameEngine;

namespace {

// Three authorings of the same panel type, differing only in how icon= is
// written. Absent and empty are the two the dock config has to tell apart.
constexpr const char* kDockConfigXml = R"(<UIElement>
  <DockConfig id="dock-config">
    <DockPanels>
      <DockablePanel id="Absent" type="UIDemoPanel" />
      <DockablePanel id="Empty" type="UIDemoPanel" icon="" />
      <DockablePanel id="Explicit" type="UIDemoPanel" icon="foo-icon" />
    </DockPanels>
    <DockLayout id="default">
      <DockLeafNode active="Absent">
        <DockTabNode panel="Absent" />
        <DockTabNode panel="Empty" />
        <DockTabNode panel="Explicit" />
      </DockLeafNode>
    </DockLayout>
  </DockConfig>
</UIElement>)";

// Parses the fragment above and keys the resulting panel defs by id.
std::map<std::string, EditorUI::EditorDockPanelDef> ParsePanelDefs()
{
    UIRegistration::RegisterBuiltInControls();

    std::unique_ptr<UIElement> root;
    EXPECT_TRUE(UIParsing::XMLParser::ParseLayoutFromString(kDockConfigXml, root));
    EXPECT_NE(root, nullptr);

    EditorUI::EditorDockConfig cfg;
    EXPECT_TRUE(EditorUI::TryParseEditorDockConfigFromDockspace(root.get(), cfg));

    std::map<std::string, EditorUI::EditorDockPanelDef> byId;
    for (auto& def : cfg.panels)
        byId[def.id] = std::move(def);
    return byId;
}

} // namespace

// Guards the fixture itself: if the fragment stopped parsing, every expectation
// below would pass vacuously on an empty map.
TEST(DockConfigIconAttribute, FixtureParsesAllThreePanels)
{
    const auto defs = ParsePanelDefs();
    ASSERT_EQ(defs.size(), 3u);
    EXPECT_EQ(defs.count("Absent"), 1u);
    EXPECT_EQ(defs.count("Empty"), 1u);
    EXPECT_EQ(defs.count("Explicit"), 1u);
}

// No icon= at all: the panel type's declared icon applies, so the def carries no
// override for the instantiation path to install.
TEST(DockConfigIconAttribute, AbsentAttributeLeavesNoOverride)
{
    const auto defs = ParsePanelDefs();
    ASSERT_EQ(defs.count("Absent"), 1u);
    EXPECT_FALSE(defs.at("Absent").icon.has_value());
}

// icon="" present and empty: an override *is* authored, and its value is the
// empty class — "this panel shows no icon", beating the type's declaration.
TEST(DockConfigIconAttribute, EmptyAttributeIsAnExplicitNoIconOverride)
{
    const auto defs = ParsePanelDefs();
    ASSERT_EQ(defs.count("Empty"), 1u);
    ASSERT_TRUE(defs.at("Empty").icon.has_value());
    EXPECT_TRUE(defs.at("Empty").icon->empty());
}

// A named class is carried through unchanged.
TEST(DockConfigIconAttribute, NamedAttributeOverridesWithThatClass)
{
    const auto defs = ParsePanelDefs();
    ASSERT_EQ(defs.count("Explicit"), 1u);
    ASSERT_TRUE(defs.at("Explicit").icon.has_value());
    EXPECT_EQ(*defs.at("Explicit").icon, "foo-icon");
}

// The distinction the layout depends on: absent and present-empty must not
// collapse to the same def, or "no icon" becomes inexpressible in markup again.
TEST(DockConfigIconAttribute, AbsentAndEmptyAreDistinguishable)
{
    const auto defs = ParsePanelDefs();
    ASSERT_EQ(defs.count("Absent"), 1u);
    ASSERT_EQ(defs.count("Empty"), 1u);
    EXPECT_NE(defs.at("Absent").icon.has_value(), defs.at("Empty").icon.has_value());
}
