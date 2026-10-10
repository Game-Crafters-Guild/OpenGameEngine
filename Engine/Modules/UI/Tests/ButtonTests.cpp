#include <gtest/gtest.h>

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include <vector>
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"

using namespace GameEngine;

static void Send(Button& b, EventId id, float x, float y)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Target = &b;
    e.CurrentTarget = &b;
    b.DispatchEvent(e);
}

static void SendWithMods(Button& b, EventId id, float x, float y, int mods)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Mods = mods;
    e.Target = &b;
    e.CurrentTarget = &b;
    b.DispatchEvent(e);
}

static bool SendWithButton(Button& b, EventId id, float x, float y, int button)
{
    UIEvent e{};
    e.Id = id;
    e.X = x;
    e.Y = y;
    e.Button = button;
    e.Target = &b;
    e.CurrentTarget = &b;
    b.DispatchEvent(e);
    return e.Handled;
}

TEST(ButtonTests, ClickOnlyOnMouseUpInside)
{
    Button b;
    UILayoutAccess::SetLastLayoutRect(b, 10, 10, 100, 40);

    int clicks = 0;
    b.RegisterEventHandler(kEventButtonClick, [&](UIEvent&)
                 { ++clicks; });

    // Down inside arms and applies the pressed visual.
    // (CSS authors style transient pressed via the :active pseudo-class;
    //  Button itself only toggles the .pressed class.)
    Send(b, kEventMouseDown, 20, 20);
    EXPECT_TRUE(b.HasClass("pressed"));

    // Up inside triggers click and clears pressed
    Send(b, kEventMouseUp, 20, 20);
    EXPECT_EQ(clicks, 1);
    EXPECT_FALSE(b.HasClass("pressed"));

    // New gesture: down inside, up outside -> no click
    Send(b, kEventMouseDown, 15, 15);
    EXPECT_TRUE(b.HasClass("pressed"));
    Send(b, kEventMouseUp, 1000, 1000); // clearly outside
    EXPECT_EQ(clicks, 1);
    EXPECT_FALSE(b.HasClass("pressed"));
}

// ---------------------------------------------------------------------------

TEST(ButtonTests, PressedClassTracksHoverWhileArmed)
{
    Button b;
    UILayoutAccess::SetLastLayoutRect(b, 0, 0, 50, 20);

    int clicks = 0;
    b.RegisterEventHandler(kEventButtonClick, [&](UIEvent&)
                 { ++clicks; });

    // Start inside -> pressed
    Send(b, kEventMouseDown, 10, 10);
    EXPECT_TRUE(b.HasClass("pressed"));

    // Move outside -> pressed removed
    Send(b, kEventMouseMove, 100, 100);
    EXPECT_FALSE(b.HasClass("pressed"));

    // Move back inside -> pressed re-applied
    Send(b, kEventMouseMove, 10, 10);
    EXPECT_TRUE(b.HasClass("pressed"));

    // Release inside -> click
    Send(b, kEventMouseUp, 10, 10);
    EXPECT_EQ(clicks, 1);
    EXPECT_FALSE(b.HasClass("pressed"));
}

// ---------------------------------------------------------------------------

// The modifier bitmask has to survive Button::OnEvent's armed mouse-up branch, which is
// what carries e.Mods into TriggerClick. ButtonClickEventTests calls TriggerClick
// directly, so only this arm covers that branch; a Ctrl+click that arrives as a plain
// click is how "save all" silently becomes "save". It does NOT cover the pointer routing
// upstream of Button::OnEvent -- GameUIHostTests owns that.
TEST(ButtonTests, ModifiersFromMouseUpReachClickSubscribers)
{
    Button b;
    UILayoutAccess::SetLastLayoutRect(b, 0, 0, 50, 20);

    int modsSeen = -1;
    b.RegisterEventHandler(kEventButtonClick, [&](UIEvent& e) { modsSeen = e.Mods; });

    SendWithMods(b, kEventMouseDown, 10, 10, 0);
    SendWithMods(b, kEventMouseUp, 10, 10, Input::kModShift | Input::kModControl);
    EXPECT_EQ(modsSeen, Input::kModShift | Input::kModControl)
        << "the mouse-up modifier bitmask did not reach the click subscriber";
}

TEST(ButtonTests, KeyboardActivationRetainsModifiersAndProgrammaticClickDefaultsToPlain)
{
    Button button;
    std::vector<int> activations;
    button.SetOnClick([&](UIEvent& event) { activations.push_back(event.Mods); });
    for (int key : {Input::kKeyCode_Enter, Input::kKeyCode_Space})
    {
        UIEvent event{};
        event.Id = kEventKeyDown;
        event.Key = key;
        event.Mods = Input::kModShift | Input::kModControl;
        event.Target = event.CurrentTarget = &button;
        button.DispatchEvent(event);
    }
    button.TriggerClick();
    EXPECT_EQ(activations, (std::vector<int>{Input::kModShift | Input::kModControl,
                                           Input::kModShift | Input::kModControl, 0}));
}

// Right-click is not a Button behaviour, and Button now has no way to be told
// otherwise. Whatever a right-click means at a given site is a ContextMenuManipulator
// attached there (ContextMenuManipulatorTests); the button itself always lets one past,
// so containers keep offering container-level context menus over their buttons.
TEST(ButtonTests, RightClickIsNeverClaimed)
{
    Button b;
    UILayoutAccess::SetLastLayoutRect(b, 0, 0, 50, 20);

    EXPECT_FALSE(SendWithButton(b, kEventMouseDown, 10, 10, 1));
    EXPECT_FALSE(b.HasClass("pressed"));
    EXPECT_FALSE(SendWithButton(b, kEventMouseUp, 10, 10, 1));
    EXPECT_FALSE(b.HasClass("pressed"));
}
