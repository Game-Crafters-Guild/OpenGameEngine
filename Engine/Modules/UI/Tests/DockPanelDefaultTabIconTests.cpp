#include <gtest/gtest.h>

#include <functional>

#include "UI/Controls/DockPanel.h"
#include "UI/Controls/DockspaceElement.h"
#include "UI/Layout/Docking.h"
#include "UI/Registration/ElementRegistration.h"

using namespace GameEngine;

namespace {

// Stands in for an editor panel type that declares its own tab icon.
class IconedTestPanel : public DockPanel {
public:
    IconedTestPanel() : DockPanel("Iconed") {}
    std::string_view DeclaredTabIconClass() const override { return "test-panel-icon"; }
};

// Stands in for a panel type that has no icon: it simply does not override.
class IconlessTestPanel : public DockPanel {
public:
    IconlessTestPanel() : DockPanel("Iconless") {}
};

// A type default must survive being shadowed by a base that cannot see it during
// construction, so the resolution point matters — not just the override.
class DerivedIconedTestPanel final : public IconedTestPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "derived-panel-icon"; }
};

UIElement* FindDescendantWithClass(UIElement* root, const std::string& cls)
{
    if (!root)
        return nullptr;
    if (root->HasClass(cls))
        return root;
    for (const auto& child : root->GetChildren())
    {
        if (auto* found = FindDescendantWithClass(child.get(), cls))
            return found;
    }
    return nullptr;
}

// Mounts one panel as a single dock tab and returns the generated tab-icon element.
UIElement* MountAndFindTabIcon(DockspaceElement& dock, DockingManager& dm, DockPanel& panel)
{
    dm.RegisterPanel("P", &panel);
    auto root = DockNode::MakeLeaf();
    root->AddTab("P");
    dm.SetRoot(std::move(root));
    dock.BindModel(&dm);
    dock.RebuildFromModel();
    return FindDescendantWithClass(&dock, "dock-tab-icon");
}

} // namespace

// The defect this guards: a panel constructed directly (as the fallback docking
// path does) never went through layout.uxml, so it reported no icon at all.
TEST(DockPanelDefaultTabIcon, DirectlyConstructedPanelReportsItsTypeDefault)
{
    IconedTestPanel panel;
    EXPECT_FALSE(panel.GetTabIcon().empty());
    EXPECT_EQ(panel.GetTabIcon(), "test-panel-icon");
}

// The default must not be captured during DockPanel's constructor, where a virtual
// call would dispatch to the base and bake in the wrong (empty) answer.
TEST(DockPanelDefaultTabIcon, MostDerivedOverrideWins)
{
    DerivedIconedTestPanel panel;
    EXPECT_EQ(panel.GetTabIcon(), "derived-panel-icon");
}

// An explicit override (a dock config's icon= attribute or a package panel
// descriptor) keeps beating the type default.
TEST(DockPanelDefaultTabIcon, ExplicitIconOverridesTypeDefault)
{
    IconedTestPanel panel;
    panel.SetTabIcon("explicit-icon");
    EXPECT_EQ(panel.GetTabIcon(), "explicit-icon");
}

// An explicit icon on a type that has no default is still honoured.
TEST(DockPanelDefaultTabIcon, ExplicitIconAppliesWhenTypeHasNoDefault)
{
    IconlessTestPanel panel;
    panel.SetTabIcon("explicit-icon");
    EXPECT_EQ(panel.GetTabIcon(), "explicit-icon");
}

// SetTabIcon("") is the "explicitly no icon" lever an authored icon="" reaches:
// an empty override is still an override, so it suppresses the declared icon
// rather than falling back to it.
TEST(DockPanelDefaultTabIcon, EmptyExplicitIconSuppressesDeclaredIcon)
{
    IconedTestPanel panel;
    ASSERT_EQ(panel.GetTabIcon(), "test-panel-icon");
    panel.SetTabIcon("");
    EXPECT_TRUE(panel.GetTabIcon().empty());
}

// Suppression is a value in the override slot, not a latched verdict: a later
// non-empty override still wins.
TEST(DockPanelDefaultTabIcon, ExplicitIconStillWinsAfterSuppression)
{
    IconedTestPanel panel;
    panel.SetTabIcon("");
    ASSERT_TRUE(panel.GetTabIcon().empty());
    panel.SetTabIcon("explicit-icon");
    EXPECT_EQ(panel.GetTabIcon(), "explicit-icon");
}

TEST(DockPanelDefaultTabIcon, PanelWithoutDefaultReportsEmpty)
{
    IconlessTestPanel panel;
    EXPECT_TRUE(panel.GetTabIcon().empty());
}

// Base DockPanel itself must stay icon-less, so nothing inherits a stray mark.
TEST(DockPanelDefaultTabIcon, BaseDockPanelHasNoDefault)
{
    DockPanel panel("Plain");
    EXPECT_TRUE(panel.DeclaredTabIconClass().empty());
    EXPECT_TRUE(panel.GetTabIcon().empty());
}

// End to end through the dockspace: the type default reaches the generated tab.
TEST(DockPanelDefaultTabIcon, MountedTabCarriesTypeDefaultIconElement)
{
    UIRegistration::RegisterBuiltInControls();

    IconedTestPanel panel;
    DockingManager dm;
    DockspaceElement dock;
    UIElement* icon = MountAndFindTabIcon(dock, dm, panel);

    ASSERT_NE(icon, nullptr);
    EXPECT_TRUE(icon->HasClass("test-panel-icon"));
}

// An empty default must mount no icon element at all, not an empty one.
TEST(DockPanelDefaultTabIcon, MountedTabForIconlessPanelHasNoIconElement)
{
    UIRegistration::RegisterBuiltInControls();

    IconlessTestPanel panel;
    DockingManager dm;
    DockspaceElement dock;
    EXPECT_EQ(MountAndFindTabIcon(dock, dm, panel), nullptr);
}

TEST(DockPanelDefaultTabIcon, MountedTabPrefersExplicitIconOverTypeDefault)
{
    UIRegistration::RegisterBuiltInControls();

    IconedTestPanel panel;
    panel.SetTabIcon("explicit-icon");
    DockingManager dm;
    DockspaceElement dock;
    UIElement* icon = MountAndFindTabIcon(dock, dm, panel);

    ASSERT_NE(icon, nullptr);
    EXPECT_TRUE(icon->HasClass("explicit-icon"));
    EXPECT_FALSE(icon->HasClass("test-panel-icon"));
}

// Suppressing a declared icon must mount no icon element at all — not an element
// carrying the declared class, and not an empty one that still takes tab space.
TEST(DockPanelDefaultTabIcon, MountedTabForSuppressedIconHasNoIconElement)
{
    UIRegistration::RegisterBuiltInControls();

    IconedTestPanel panel;
    panel.SetTabIcon("");
    DockingManager dm;
    DockspaceElement dock;
    EXPECT_EQ(MountAndFindTabIcon(dock, dm, panel), nullptr);
}
