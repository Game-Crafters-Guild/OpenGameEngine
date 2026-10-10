// The Scene View tool strip's registered entries (SceneView/SceneViewToolStrip.h): a
// registered entry appears after the built-in buttons of both strips, a click asks the
// view to activate its tool, the strip shows the entry's badge count and hides an entry
// that is not available, reading nothing while hidden; and the editor's own entries (the
// terrain brush's availability and its refusal, the spline tool's menu).

#include "SceneView/SceneViewToolStrip.h"
#include "SceneView/SceneViewToolStripRegistry.h"
#include "SceneView/SplineToolStripEntry.h"
#include "SceneView/TerrainBrushToolStripEntry.h"

#include "Components/Terrain/Terrain.h"
#include "ECS/Entity.h"
#include "UI/Layout/ElementOverrideHelpers.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using Editor::RegisteredToolStripActions;
using Editor::SceneViewToolStripEntry;
using Editor::SceneViewToolStripRegistry;
using Editor::ToolStripPlacement;

namespace
{

void Click(Button& button)
{
    UIEvent e{};
    e.Id = kEventButtonClick;
    e.Target = &button;
    e.CurrentTarget = &button;
    button.DispatchEvent(e);
}

bool IsHidden(const UIElement* element)
{
    const auto display = element ? element->Overrides().Get(Style::Display) : std::nullopt;
    return display.has_value() && display.value() == DisplayMode::None;
}

UIElement* FindButton(UIElement& strip, ToolStripPlacement placement, std::string_view id)
{
    return strip.FindById(Editor::RegisteredToolButtonId(placement, id));
}

} // namespace

TEST(SceneViewToolStrip, ARegisteredEntryIsAppendedActivatesItsToolAndShowsItsBadge)
{
    SceneViewToolStripRegistry registry;
    std::size_t unseen = 0;
    int badgeOpened = 0;
    SceneViewToolStripEntry entry;
    entry.Id = "notes";
    entry.Tooltip = "Notes";
    entry.Icon = "editor:Icons/marker.png";
    entry.BadgeButtonIcon = "editor:Icons/pulse.png";
    int badgeReads = 0;
    entry.BadgeCount = [&unseen, &badgeReads]() {
        ++badgeReads;
        return unseen;
    };
    entry.OnBadgeButton = [&badgeOpened](UIElement&) { ++badgeOpened; };
    registry.Register(entry);
    entry.Tooltip = "Notes, replaced";
    registry.Register(entry);
    ASSERT_EQ(registry.Entries().size(), 1u);

    UIElement strip;
    auto builtIn = std::make_unique<Button>();
    builtIn->SetId("SelectModeBtn");
    strip.AddChild(std::move(builtIn));

    std::vector<std::string> toggled;
    std::vector<std::string> icons;
    RegisteredToolStripActions actions;
    actions.ToggleTool = [&toggled](const std::string& id) { toggled.push_back(id); };
    actions.SetIcon = [&icons](UIElement&, std::string_view path) { icons.emplace_back(path); };
    Editor::RegisteredToolStrip kept;
    kept.Populate(strip, ToolStripPlacement::Floating, registry, actions);
    kept.Populate(strip, ToolStripPlacement::Floating, registry, actions);

    // The built-in button, a divider, then the entry's button and the badge button's host as
    // one group: the tool sits beside the panel its badge opens.
    ASSERT_EQ(strip.GetChildren().size(), 4u);
    EXPECT_EQ(strip.GetChildren()[0]->GetId(), "SelectModeBtn");
    EXPECT_TRUE(strip.GetChildren()[1]->HasClass("scene-tool-divider"));
    auto* button = dynamic_cast<Button*>(strip.GetChildren()[2].get());
    EXPECT_TRUE(strip.GetChildren()[3]->HasClass("scene-tool-badge-host"));
    auto* badgeButton = dynamic_cast<Button*>(strip.FindById(Editor::RegisteredToolBadgeButtonId(ToolStripPlacement::Floating, "notes")));
    ASSERT_NE(button, nullptr);
    ASSERT_NE(badgeButton, nullptr);
    EXPECT_EQ(button->GetId(), Editor::RegisteredToolButtonId(ToolStripPlacement::Floating, "notes"));
    EXPECT_EQ(icons, (std::vector<std::string>{"editor:Icons/marker.png", "editor:Icons/pulse.png"}));

    Click(*button);
    EXPECT_EQ(toggled, std::vector<std::string>{"notes"});
    Click(*badgeButton);
    EXPECT_EQ(badgeOpened, 1);

    auto* count = dynamic_cast<Label*>(strip.FindById(Editor::RegisteredToolBadgeCountId(ToolStripPlacement::Floating, "notes")));
    ASSERT_NE(count, nullptr);
    kept.SetActiveEntry("notes");
    kept.Refresh(nullptr);
    EXPECT_TRUE(button->HasClass("icon-active"));
    EXPECT_TRUE(count->HasClass("hidden"));

    unseen = 3;
    kept.SetActiveEntry("");
    kept.Refresh(nullptr);
    EXPECT_FALSE(button->HasClass("icon-active"));
    EXPECT_FALSE(count->HasClass("hidden"));
    EXPECT_EQ(count->GetText(), "3");

    unseen = 140;
    kept.Refresh(nullptr);
    EXPECT_EQ(count->GetText(), "99+");

    // A hidden strip reads no provider.
    strip.AddClass("hidden");
    const int readsBeforeHidden = badgeReads;
    unseen = 5;
    kept.Refresh(nullptr);
    EXPECT_EQ(badgeReads, readsBeforeHidden);
    EXPECT_EQ(count->GetText(), "99+");
}

TEST(SceneViewToolStrip, ARegisteredEntryAppearsInTheFloatingStripAndTheInlineMirror)
{
    SceneViewToolStripRegistry registry;
    SceneViewToolStripEntry entry;
    entry.Id = "notes";
    entry.Tooltip = "Notes";
    registry.Register(entry);

    std::vector<std::string> toggled;
    RegisteredToolStripActions actions;
    actions.ToggleTool = [&toggled](const std::string& id) { toggled.push_back(id); };
    UIElement floating;
    UIElement inlineMirror;
    Editor::RegisteredToolStrip floatingStrip;
    floatingStrip.Populate(floating, ToolStripPlacement::Floating, registry, actions);
    Editor::RegisteredToolStrip inlineMirrorStrip;
    inlineMirrorStrip.Populate(inlineMirror, ToolStripPlacement::Inline, registry, actions);

    auto* floatingButton = dynamic_cast<Button*>(FindButton(floating, ToolStripPlacement::Floating, "notes"));
    auto* inlineButton = dynamic_cast<Button*>(FindButton(inlineMirror, ToolStripPlacement::Inline, "notes"));
    ASSERT_NE(floatingButton, nullptr);
    ASSERT_NE(inlineButton, nullptr);
    EXPECT_NE(floatingButton->GetId(), inlineButton->GetId());

    Click(*inlineButton);
    EXPECT_EQ(toggled, std::vector<std::string>{"notes"});
    inlineMirrorStrip.Refresh(nullptr);
    inlineMirrorStrip.SetActiveEntry("notes");
    EXPECT_TRUE(inlineButton->HasClass("icon-active"));
}

// Only the strip that shows is refreshed: the hidden one reads no provider.
TEST(SceneViewToolStrip, OnlyTheShownStripReadsTheEntrysProviders)
{
    int reads = 0;
    SceneViewToolStripRegistry registry;
    SceneViewToolStripEntry entry;
    entry.Id = "notes";
    entry.IsAvailable = [&reads](ECS::World*) {
        ++reads;
        return true;
    };
    registry.Register(entry);
    UIElement floating;
    UIElement inlineMirror;
    inlineMirror.AddClass("hidden");
    Editor::RegisteredToolStrip floatingStrip;
    floatingStrip.Populate(floating, ToolStripPlacement::Floating, registry, {});
    Editor::RegisteredToolStrip inlineMirrorStrip;
    inlineMirrorStrip.Populate(inlineMirror, ToolStripPlacement::Inline, registry, {});

    inlineMirrorStrip.Refresh(nullptr);
    EXPECT_EQ(reads, 0);
    floatingStrip.Refresh(nullptr);
    EXPECT_EQ(reads, 1);
}

// A strip hidden while its entry turned available and became the active tool shows that
// tool active when it is shown again, in the view's call order: SetActiveEntry, then
// Refresh, then Refresh alone on the later frames. An entry that turned available while
// another is active comes back without the mark.
TEST(SceneViewToolStrip, AHiddenStripShowsTheActiveEntryWhenItReappears)
{
    bool available = false;
    SceneViewToolStripRegistry registry;
    SceneViewToolStripEntry entry;
    entry.Id = "brush";
    entry.IsAvailable = [&available](ECS::World*) { return available; };
    registry.Register(entry);
    SceneViewToolStripEntry other;
    other.Id = "stamp";
    other.IsAvailable = [&available](ECS::World*) { return available; };
    registry.Register(other);
    UIElement strip;
    Editor::RegisteredToolStrip kept;
    kept.Populate(strip, ToolStripPlacement::Floating, registry, {});
    kept.Refresh(nullptr);

    strip.AddClass("hidden");
    available = true;
    kept.SetActiveEntry("brush");
    kept.Refresh(nullptr);
    strip.RemoveClass("hidden");
    kept.Refresh(nullptr);

    const UIElement* button = FindButton(strip, ToolStripPlacement::Floating, "brush");
    ASSERT_NE(button, nullptr);
    EXPECT_FALSE(IsHidden(button));
    EXPECT_TRUE(button->HasClass("icon-active"));
    const UIElement* otherButton = FindButton(strip, ToolStripPlacement::Floating, "stamp");
    ASSERT_NE(otherButton, nullptr);
    EXPECT_FALSE(IsHidden(otherButton));
    EXPECT_FALSE(otherButton->HasClass("icon-active"));
}

TEST(SceneViewToolStrip, TheTerrainBrushIsShownOnlyWhileTheWorldHoldsATerrain)
{
    ECS::World world;
    SceneViewToolStripRegistry registry;
    registry.Register(Editor::MakeTerrainBrushToolStripEntry());
    UIElement floating;
    UIElement inlineMirror;
    Editor::RegisteredToolStrip floatingStrip;
    floatingStrip.Populate(floating, ToolStripPlacement::Floating, registry, {});
    Editor::RegisteredToolStrip inlineMirrorStrip;
    inlineMirrorStrip.Populate(inlineMirror, ToolStripPlacement::Inline, registry, {});

    floatingStrip.Refresh(&world);
    floatingStrip.SetActiveEntry("");
    inlineMirrorStrip.Refresh(&world);
    inlineMirrorStrip.SetActiveEntry("");
    EXPECT_TRUE(IsHidden(FindButton(floating, ToolStripPlacement::Floating, "terrainBrush")));
    EXPECT_TRUE(IsHidden(FindButton(inlineMirror, ToolStripPlacement::Inline, "terrainBrush")));

    const ECS::EntityHandle terrain = world.Create().GetHandle();
    world.AddComponentImmediate(terrain, Components::Terrain{});
    floatingStrip.Refresh(&world);
    floatingStrip.SetActiveEntry("terrainBrush");
    inlineMirrorStrip.Refresh(&world);
    inlineMirrorStrip.SetActiveEntry("terrainBrush");
    EXPECT_FALSE(IsHidden(FindButton(floating, ToolStripPlacement::Floating, "terrainBrush")));
    EXPECT_FALSE(IsHidden(FindButton(inlineMirror, ToolStripPlacement::Inline, "terrainBrush")));
    EXPECT_TRUE(FindButton(floating, ToolStripPlacement::Floating, "terrainBrush")->HasClass("icon-active"));

    world.DestroyEntityImmediate(terrain);
    floatingStrip.Refresh(&world);
    floatingStrip.SetActiveEntry("terrainBrush");
    EXPECT_TRUE(IsHidden(FindButton(floating, ToolStripPlacement::Floating, "terrainBrush")));
    EXPECT_FALSE(FindButton(floating, ToolStripPlacement::Floating, "terrainBrush")->HasClass("icon-active"));
}

// A view refuses to activate the terrain brush while the world holds no terrain, and the
// refusal names the fix; an id no entry has is refused as unknown.
TEST(SceneViewToolStrip, TheTerrainBrushIsRefusedWithoutATerrainNamingTheFix)
{
    ECS::World world;
    SceneViewToolStripRegistry registry;
    registry.Register(Editor::MakeTerrainBrushToolStripEntry());

    EXPECT_EQ(Editor::RegisteredToolRefusal(registry, "terrainBrush", &world),
              "Terrain Brush needs a terrain in the scene: add a Terrain entity first");
    EXPECT_EQ(Editor::RegisteredToolRefusal(registry, "bogus", &world), "Unknown tool: bogus");

    world.AddComponentImmediate(world.Create().GetHandle(), Components::Terrain{});
    EXPECT_EQ(Editor::RegisteredToolRefusal(registry, "terrainBrush", &world), "");
}

TEST(SceneViewToolStrip, TheSplineEntryMenuListsTheSplineSettingsInOrder)
{
    const SceneViewToolStripEntry entry = Editor::MakeSplineToolStripEntry();
    ASSERT_TRUE(entry.ContextMenuItems);
    const std::vector<ContextMenuManipulator::Item> items = entry.ContextMenuItems(OpenColorPickerWindowFn{});
    ASSERT_FALSE(items.empty());

    // Rows the menu shows at its top level, in the order it shows them.
    const std::vector<std::string> expected = {
        "Curve Type", "Selection Shape", "Control Render Shape", "Knot Size", "Knot Outline Thickness",
        "Spline Thickness", "Default Radius", "Simplify Tolerance", "Smoothing Passes", "Mesh Acceleration",
        "Auto-Connect to Nearby Spline", "Snap to Scene Meshes", "Pivot From Spline Center", "Spline Color...",
        "Brush Stroke Color...", "Reset to Defaults"};
    std::size_t next = 0;
    for (const ContextMenuManipulator::Item& item : items)
    {
        if (next < expected.size() && item.Path == expected[next])
            ++next;
        if (item.Path == "Spline Color...")
            EXPECT_NE(item.Flags & MenuItemFlag_Disabled, 0u) << "a color row needs a picker";
    }
    EXPECT_EQ(next, expected.size()) << "first missing or out of order: "
                                     << (next < expected.size() ? expected[next] : std::string());
    EXPECT_EQ(items.back().Path, "Reset to Defaults");
}


TEST(SceneViewToolStrip, TerrainAvailabilityUsesEachViewsWorld)
{
    ECS::World empty;
    ECS::World terrainWorld;
    terrainWorld.AddComponentImmediate(terrainWorld.Create().GetHandle(), Components::Terrain{});
    const auto entry = Editor::MakeTerrainBrushToolStripEntry();
    ASSERT_TRUE(entry.IsAvailable);
    EXPECT_FALSE(entry.IsAvailable(&empty));
    EXPECT_TRUE(entry.IsAvailable(&terrainWorld));
    EXPECT_FALSE(entry.IsAvailable(&empty));
}
