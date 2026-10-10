// The inspector's outlined action button: an icon element before the button's text, the icon set by
// a CSS class. The preview's Pause button swaps its icon to Play and back, so a swap must leave only
// the new class on the icon; a stale class would keep whichever background-image its sheet declares
// later.

#include <gtest/gtest.h>

#include "UI/Controls/InspectorActionButton.h"
#include "UI/UIElement.h"

using GameEngine::UIElement;
using GameEngine::EditorUI::InspectorActionButton;

TEST(InspectorActionButton, IconComesBeforeTheLabelAndCarriesItsClass)
{
    InspectorActionButton button("Add Phase", "inspector-action-add");

    ASSERT_GE(button.GetChildren().size(), 2u);
    const UIElement* icon = button.GetChildren()[0].get();
    ASSERT_NE(icon, nullptr);
    EXPECT_TRUE(icon->HasClass("inspector-action-icon"));
    EXPECT_TRUE(icon->HasClass("inspector-action-add"));
    EXPECT_EQ(button.GetText(), "Add Phase");
}

TEST(InspectorActionButton, SwappingTheIconLeavesOnlyTheNewClass)
{
    InspectorActionButton button("Pause", "inspector-action-pause");

    button.SetIconClass("inspector-action-play");

    const UIElement* icon = button.GetChildren()[0].get();
    ASSERT_NE(icon, nullptr);
    EXPECT_TRUE(icon->HasClass("inspector-action-icon"));
    EXPECT_TRUE(icon->HasClass("inspector-action-play"));
    EXPECT_FALSE(icon->HasClass("inspector-action-pause"));
}
