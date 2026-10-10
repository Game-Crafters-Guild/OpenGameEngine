#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

#include "UI/Controls/DockPanel.h"
#include "UI/Layout/DockConfigElements.h"
#include "UI/Layout/Docking.h"
#include "UI/Layout/EditorDockConfigParser.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"

using namespace GameEngine;

namespace {

// menu= controls only whether user-facing panel listings offer the panel; a
// delisted panel stays in the inventory and openable by id. Absent must mean
// listed, so every existing declaration keeps its menu entry.
constexpr const char* kDockConfigXml = R"(<UIElement>
  <DockConfig id="dock-config">
    <DockPanels>
      <DockablePanel id="Default" type="UIDemoPanel" />
      <DockablePanel id="Delisted" type="UIDemoPanel" menu="false" />
      <DockablePanel id="Listed" type="UIDemoPanel" menu="true" />
    </DockPanels>
    <DockLayout id="default">
      <DockLeafNode active="Default">
        <DockTabNode panel="Default" />
        <DockTabNode panel="Delisted" />
        <DockTabNode panel="Listed" />
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
TEST(DockConfigMenuAttribute, FixtureParsesAllThreePanels)
{
    const auto defs = ParsePanelDefs();
    ASSERT_EQ(defs.size(), 3u);
    EXPECT_EQ(defs.count("Default"), 1u);
    EXPECT_EQ(defs.count("Delisted"), 1u);
    EXPECT_EQ(defs.count("Listed"), 1u);
}

// No menu= attribute means listed: the default every pre-existing declaration
// relies on.
TEST(DockConfigMenuAttribute, AbsentAttributeMeansListed)
{
    const auto defs = ParsePanelDefs();
    ASSERT_EQ(defs.count("Default"), 1u);
    EXPECT_TRUE(defs.at("Default").showInMenu);
}

TEST(DockConfigMenuAttribute, FalseDelistsTrueLists)
{
    const auto defs = ParsePanelDefs();
    ASSERT_EQ(defs.count("Delisted"), 1u);
    ASSERT_EQ(defs.count("Listed"), 1u);
    EXPECT_FALSE(defs.at("Delisted").showInMenu);
    EXPECT_TRUE(defs.at("Listed").showInMenu);
}

// The property programmatic opening depends on: delisting a DockPanel must not
// touch docking registration or activation by id (ShowOrActivatePanel resolves
// panels through DockingManager, not through the menu listings).
TEST(DockConfigMenuAttribute, DelistedPanelStillRegistersAndActivatesById)
{
    DockPanel panel("Delisted");
    panel.SetListedInPanelMenus(false);
    EXPECT_FALSE(panel.IsListedInPanelMenus());

    DockingManager docking;
    docking.RegisterPanel("Delisted", &panel);

    auto root = DockNode::MakeLeaf();
    root->AddTab("Delisted");
    docking.SetRoot(std::move(root));

    ASSERT_EQ(docking.GetPanels().count("Delisted"), 1u);
    EXPECT_TRUE(docking.ActivateTab("Delisted"));
}
