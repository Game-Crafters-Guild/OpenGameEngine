#include <gtest/gtest.h>

#include "UI/Controls/Checkbox.h"
#include "UI/Controls/EnableDot.h"
#include "UI/Controls/Toggle.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/UIEvents.h"

using namespace GameEngine;

namespace
{

static void SendMouse(UIElement& el, EventId id, float x, float y, int button = 0)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Button = button;
    e.Target = &el;
    e.CurrentTarget = &el;
    el.DispatchEvent(e);
}

} // namespace

TEST(CheckboxTests, MouseClickTogglesCheckedAndAttribute)
{
    Checkbox box;
    UILayoutAccess::SetLastLayoutRect(box, 10.0f, 10.0f, 50.0f, 20.0f);

    EXPECT_FALSE(box.IsChecked());
    EXPECT_FALSE(box.HasClass("checked"));

    SendMouse(box, kEventMouseDown, 20.0f, 20.0f, 0);
    SendMouse(box, kEventMouseUp,   20.0f, 20.0f, 0);

    EXPECT_TRUE(box.IsChecked());
    EXPECT_TRUE(box.HasClass("checked"));

    SendMouse(box, kEventMouseDown, 20.0f, 20.0f, 0);
    SendMouse(box, kEventMouseUp,   20.0f, 20.0f, 0);

    EXPECT_FALSE(box.IsChecked());
    EXPECT_FALSE(box.HasClass("checked"));
}

TEST(CheckboxTests, SetCheckedUpdatesAttributeAndClass)
{
    Checkbox box;

    box.SetChecked(true);
    EXPECT_TRUE(box.IsChecked());
    EXPECT_TRUE(box.HasClass("checked"));

    box.SetChecked(false);
    EXPECT_FALSE(box.IsChecked());
    EXPECT_FALSE(box.HasClass("checked"));
}

TEST(ToggleTests, SpaceKeyTogglesValue)
{
    Toggle toggle;
    UILayoutAccess::SetLastLayoutRect(toggle, 0.0f, 0.0f, 30.0f, 10.0f);

    EXPECT_FALSE(toggle.IsChecked());

    UIEvent e{};
    e.Id = kEventKeyDown;
    e.Key = 32; // space
    e.Target = &toggle;
    e.CurrentTarget = &toggle;

    toggle.DispatchEvent(e);
    EXPECT_TRUE(toggle.IsChecked());

    UIEvent e2 = e;
    toggle.DispatchEvent(e2);
    EXPECT_FALSE(toggle.IsChecked());
}

TEST(ToggleTests, NewInstancesUseConfiguredPresentationClass)
{
    Toggle::SetCheckmarkPresentationEnabled(true);
    Toggle checkmarkToggle;
    EXPECT_TRUE(checkmarkToggle.HasClass("toggle-checkmark"));

    Toggle::SetCheckmarkPresentationEnabled(false);
    Toggle switchToggle;
    EXPECT_FALSE(switchToggle.HasClass("toggle-checkmark"));
}

// The dot draws its state on its mark, the element its sheet styles: filled on, a ring off, a dash
// for a mixed value. A click switches it, and on a mixed value switches to the opposite of the
// value it holds, as a mixed Toggle does.
TEST(EnableDotTests, TheMarkCarriesTheStateItDraws)
{
    EnableDot dot;
    UILayoutAccess::SetLastLayoutRect(dot, 0.0f, 0.0f, 24.0f, 24.0f);
    ASSERT_EQ(dot.GetChildren().size(), 1u);
    const UIElement& mark = *dot.GetChildren().front();

    EXPECT_TRUE(mark.HasClass("enable-dot-mark-off"));
    dot.SetValueWithoutNotify(true);
    EXPECT_TRUE(mark.HasClass("enable-dot-mark-on"));
    EXPECT_FALSE(mark.HasClass("enable-dot-mark-off"));

    dot.SetMixed();
    EXPECT_TRUE(mark.HasClass("enable-dot-mark-mixed"));
    EXPECT_FALSE(mark.HasClass("enable-dot-mark-on"));

    SendMouse(dot, kEventMouseDown, 12.0f, 12.0f, 0);
    SendMouse(dot, kEventMouseUp, 12.0f, 12.0f, 0);
    EXPECT_FALSE(dot.IsChecked());
    EXPECT_TRUE(mark.HasClass("enable-dot-mark-off"));
    EXPECT_FALSE(mark.HasClass("enable-dot-mark-mixed"));
}

// Only the primary button switches the dot; any other press is left to its parent (a section
// header's context menu).
TEST(EnableDotTests, OnlyThePrimaryButtonSwitchesIt)
{
    EnableDot dot;
    UILayoutAccess::SetLastLayoutRect(dot, 0.0f, 0.0f, 24.0f, 24.0f);

    SendMouse(dot, kEventMouseDown, 12.0f, 12.0f, 1);
    SendMouse(dot, kEventMouseUp, 12.0f, 12.0f, 1);
    EXPECT_FALSE(dot.IsChecked());

    SendMouse(dot, kEventMouseDown, 12.0f, 12.0f, 0);
    SendMouse(dot, kEventMouseUp, 12.0f, 12.0f, 0);
    EXPECT_TRUE(dot.IsChecked());
}
