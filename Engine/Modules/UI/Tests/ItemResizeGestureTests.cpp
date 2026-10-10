// The item resize gesture (UI/Interaction/ItemResizeGesture.h): one process-wide matcher decides
// which wheel modifiers resize the items of every tree, list and grid view. With no host matcher it
// is the primary shortcut modifier; a host matcher replaces that gate for every view at once,
// including views that existed before it was installed.

#include <gtest/gtest.h>

#include "Input/InputSystem.h"
#include "UI/Controls/GridView.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/TreeView.h"
#include "UI/Interaction/ItemResizeGesture.h"
#include "UI/UIEvents.h"

using namespace GameEngine;

TEST(ItemResizeGesture, ATreeFiresOnThePrimaryModifierWheel)
{
    TreeView view;
    int calls = 0;
    view.SetOnItemResizeGesture([&](float) { ++calls; });

    ASSERT_FALSE(view.GetChildren().empty());
    UIEvent e;
    e.Id = kEventScroll;
    e.Mods = Input::kModControl;
    e.ScrollY = -30.0f;
    view.GetChildren()[0]->DispatchEvent(e);
    EXPECT_EQ(calls, 1);
}

TEST(ItemResizeGesture, AHostMatcherReplacesThePrimaryModifierGate)
{
    TreeView view;
    int calls = 0;
    view.SetOnItemResizeGesture([&](float) { ++calls; });
    struct RestorePolicy { ~RestorePolicy() { UI::SetItemResizeGestureMatcher(nullptr); } } restore;
    UI::SetItemResizeGestureMatcher([](int) { return false; });

    ASSERT_FALSE(view.GetChildren().empty());
    UIEvent e;
    e.Id = kEventScroll;
    e.Mods = Input::kModControl;
    e.ScrollY = -30.0f;
    view.GetChildren()[0]->DispatchEvent(e);
    EXPECT_EQ(calls, 0);

    UI::SetItemResizeGestureMatcher([](int) { return true; });
    e.Mods = 0;
    view.GetChildren()[0]->DispatchEvent(e);
    EXPECT_EQ(calls, 1);
}

TEST(ItemResizeGesture, AHostMatcherGovernsEveryExistingItemView)
{
    TreeView tree;
    ListView list;
    GridView grid;
    int treeCalls = 0;
    int listCalls = 0;
    tree.SetOnItemResizeGesture([&](float) { ++treeCalls; });
    list.SetOnItemResizeGesture([&](float) { ++listCalls; });
    grid.SetIconSize(64.0f);
    struct RestorePolicy { ~RestorePolicy() { UI::SetItemResizeGestureMatcher(nullptr); } } restore;
    auto dispatch = [](UIElement& view)
    {
        // Dispatch to each direct child, including the scroll viewport.
        for (const auto& child : view.GetChildren())
        {
            UIEvent event;
            event.Id = kEventScroll;
            event.Mods = Input::kModControl;
            event.ScrollY = -30.0f;
            child->DispatchEvent(event);
        }
    };
    UI::SetItemResizeGestureMatcher([](int) { return false; });
    dispatch(tree);
    dispatch(list);
    dispatch(grid);
    EXPECT_EQ(treeCalls, 0);
    EXPECT_EQ(listCalls, 0);
    EXPECT_FLOAT_EQ(grid.GetIconSize(), 64.0f);
    UI::SetItemResizeGestureMatcher([](int) { return true; });
    dispatch(tree);
    dispatch(list);
    dispatch(grid);
    EXPECT_EQ(treeCalls, 1);
    EXPECT_EQ(listCalls, 1);
    EXPECT_NE(grid.GetIconSize(), 64.0f);
}
