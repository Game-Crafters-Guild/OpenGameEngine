#include "Input/GamepadEdgeTracker.h"
#include "Input/GamepadFrame.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Mathematics/VectorPrinting.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <vector>

using namespace GameEngine::Input;
using GameEngine::Mathematics::Vector2;

namespace
{

constexpr ContextId kTestContext = HashInput("TestContext");
constexpr ActionId kTestAction = HashInput("TestAction");

constexpr ContextId kMouseContext = HashInput("MouseContext");
constexpr ActionId kMouseAxisAction = HashInput("MouseAxisAction");

// Simple fake key codes to avoid depending on GLFW in this unit test.
constexpr KeyCode kKeyA = 65;              // 'A'
constexpr KeyCode kKeyB = 66;              // 'B'
constexpr MouseCode kMouseButtonRight = 1; // matches GLFW mouse button 1 (RMB) but decoupled from GLFW
constexpr MouseCode kMouseButtonLeft = 0;

constexpr KeyCode kKeyW = 87;

constexpr ActionId kPlainAction = HashInput("PlainA");
constexpr ActionId kShiftAction = HashInput("ShiftA");
constexpr ActionId kActionA = HashInput("ActionA");
constexpr ActionId kActionB = HashInput("ActionB");

} // namespace

TEST(InputSystemTests, ModifierShortcutDistinguishesShiftAFromPlainA)
{
    InputSystem input;

    // Plain A: must *not* have Shift held.
    ActionDesc plain{};
    plain.id = kPlainAction;
    plain.isAxis = false;
    ActionBinding plainBind{DeviceType::Keyboard, kKeyA, 1.0f};
    plainBind.forbiddenMods = kModShift;
    plain.bindings.push_back(plainBind);

    // Shift+A: requires Shift.
    ActionDesc shift{};
    shift.id = kShiftAction;
    shift.isAxis = false;
    ActionBinding shiftBind{DeviceType::Keyboard, kKeyA, 1.0f};
    shiftBind.requiredMods = kModShift;
    shift.bindings.push_back(shiftBind);

    input.RegisterAction(kTestContext, plain);
    input.RegisterAction(kTestContext, shift);
    input.PushContext(kTestContext);

    // Press Shift+A.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/kModShift);
    input.Update(0.016f);

    EXPECT_FALSE(input.IsActionActive(kPlainAction));
    EXPECT_FALSE(input.WasActionTriggered(kPlainAction));

    EXPECT_TRUE(input.IsActionActive(kShiftAction));
    EXPECT_TRUE(input.WasActionTriggered(kShiftAction));

    // Release A while still holding Shift.
    input.OnKey(kKeyA, /*action=*/0, /*mods=*/kModShift);
    input.Update(0.016f);
    EXPECT_FALSE(input.IsActionActive(kShiftAction));

    // Press plain A (no modifiers).
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_TRUE(input.IsActionActive(kPlainAction));
    EXPECT_TRUE(input.WasActionTriggered(kPlainAction));

    EXPECT_FALSE(input.IsActionActive(kShiftAction));
    EXPECT_FALSE(input.WasActionTriggered(kShiftAction));
}

TEST(InputSystemTests, ModifierShortcutUsesModifierKeyStateWhenEventModsMissing)
{
    InputSystem input;

    // Plain A: must *not* have Shift held.
    ActionDesc plain{};
    plain.id = kPlainAction;
    plain.isAxis = false;
    ActionBinding plainBind{DeviceType::Keyboard, kKeyA, 1.0f};
    plainBind.forbiddenMods = kModShift;
    plain.bindings.push_back(plainBind);

    // Shift+A: requires Shift.
    ActionDesc shift{};
    shift.id = kShiftAction;
    shift.isAxis = false;
    ActionBinding shiftBind{DeviceType::Keyboard, kKeyA, 1.0f};
    shiftBind.requiredMods = kModShift;
    shift.bindings.push_back(shiftBind);

    input.RegisterAction(kTestContext, plain);
    input.RegisterAction(kTestContext, shift);
    input.PushContext(kTestContext);

    // Simulate a platform delivering Shift's press without the Shift bit set in mods.
    input.OnKey(kKeyCode_LeftShift, /*action=*/1, /*mods=*/0);

    // Press A (also missing Shift in mods). InputSystem should still detect the shortcut
    // via the modifier key state.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_FALSE(input.WasActionTriggered(kPlainAction));
    EXPECT_TRUE(input.WasActionTriggered(kShiftAction));
}

TEST(InputSystemTests, ModifierStateClearsOnModifierReleaseEvenIfEventModsStillSet)
{
    InputSystem input;

    // Plain A: must *not* have Shift held.
    ActionDesc plain{};
    plain.id = kPlainAction;
    plain.isAxis = false;
    ActionBinding plainBind{DeviceType::Keyboard, kKeyA, 1.0f};
    plainBind.forbiddenMods = kModShift;
    plain.bindings.push_back(plainBind);

    // Shift+A: requires Shift.
    ActionDesc shift{};
    shift.id = kShiftAction;
    shift.isAxis = false;
    ActionBinding shiftBind{DeviceType::Keyboard, kKeyA, 1.0f};
    shiftBind.requiredMods = kModShift;
    shift.bindings.push_back(shiftBind);

    input.RegisterAction(kTestContext, plain);
    input.RegisterAction(kTestContext, shift);
    input.PushContext(kTestContext);

    // Simulate a platform delivering Shift press without mods, then Shift release where mods
    // still reports the Shift bit (pre-release state).
    input.OnKey(kKeyCode_LeftShift, /*action=*/1, /*mods=*/0);
    input.OnKey(kKeyCode_LeftShift, /*action=*/0, /*mods=*/kModShift);

    // Press plain A (no modifiers). If modifier release reconciliation is broken, this would
    // incorrectly route to the Shift binding.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_TRUE(input.WasActionTriggered(kPlainAction));
    EXPECT_FALSE(input.WasActionTriggered(kShiftAction));
}

TEST(InputSystemTests, ModifierShortcutUsesPressTimeModifiersNotLastEventModifiers)
{
    InputSystem input;

    // Plain A: must *not* have Shift held.
    ActionDesc plain{};
    plain.id = kPlainAction;
    plain.isAxis = false;
    ActionBinding plainBind{DeviceType::Keyboard, kKeyA, 1.0f};
    plainBind.forbiddenMods = kModShift;
    plain.bindings.push_back(plainBind);

    // Shift+A: requires Shift.
    ActionDesc shift{};
    shift.id = kShiftAction;
    shift.isAxis = false;
    ActionBinding shiftBind{DeviceType::Keyboard, kKeyA, 1.0f};
    shiftBind.requiredMods = kModShift;
    shift.bindings.push_back(shiftBind);

    input.RegisterAction(kTestContext, plain);
    input.RegisterAction(kTestContext, shift);
    input.PushContext(kTestContext);

    // Press Shift+A, then simulate another input event in the same frame that clears the
    // current modifier snapshot (e.g. Shift released before Update()).
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/kModShift);
    input.OnKey(kKeyB, /*action=*/1, /*mods=*/0);

    input.Update(0.016f);

    EXPECT_TRUE(input.WasActionTriggered(kShiftAction));
    EXPECT_FALSE(input.WasActionTriggered(kPlainAction));
}

TEST(InputSystemTests, ModifierShortcutUsesModsFromPressEventEvenIfCurrentModsChangesBeforeUpdate)
{
    InputSystem input;

    // Plain A: must *not* have Shift held.
    ActionDesc plain{};
    plain.id = kPlainAction;
    plain.isAxis = false;
    ActionBinding plainBind{DeviceType::Keyboard, kKeyA, 1.0f};
    plainBind.forbiddenMods = kModShift;
    plain.bindings.push_back(plainBind);

    // Shift+A: requires Shift.
    ActionDesc shift{};
    shift.id = kShiftAction;
    shift.isAxis = false;
    ActionBinding shiftBind{DeviceType::Keyboard, kKeyA, 1.0f};
    shiftBind.requiredMods = kModShift;
    shift.bindings.push_back(shiftBind);

    input.RegisterAction(kTestContext, plain);
    input.RegisterAction(kTestContext, shift);
    input.PushContext(kTestContext);

    // Press Shift+A, then simulate another key event (e.g. repeat/other key)
    // overwriting the global modifier state before Update().
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/kModShift);
    input.OnKey(kKeyA, /*action=*/2, /*mods=*/0); // GLFW_REPEAT-like event with mods=0
    input.Update(0.016f);

    EXPECT_FALSE(input.WasActionTriggered(kPlainAction));
    EXPECT_TRUE(input.WasActionTriggered(kShiftAction));
}

TEST(InputSystemTests, ModifierShortcutFallsBackToCurrentModsWhenPressEventModsIsMissing)
{
    InputSystem input;

    // Plain A: must *not* have Shift held.
    ActionDesc plain{};
    plain.id = kPlainAction;
    plain.isAxis = false;
    ActionBinding plainBind{DeviceType::Keyboard, kKeyA, 1.0f};
    plainBind.forbiddenMods = kModShift;
    plain.bindings.push_back(plainBind);

    // Shift+A: requires Shift.
    ActionDesc shift{};
    shift.id = kShiftAction;
    shift.isAxis = false;
    ActionBinding shiftBind{DeviceType::Keyboard, kKeyA, 1.0f};
    shiftBind.requiredMods = kModShift;
    shift.bindings.push_back(shiftBind);

    input.RegisterAction(kTestContext, plain);
    input.RegisterAction(kTestContext, shift);
    input.PushContext(kTestContext);

    // Simulate a platform delivering A's press event without the Shift bit set,
    // but updating the global modifier state later in the same frame.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.OnKey(kKeyB, /*action=*/1, /*mods=*/kModShift);
    input.Update(0.016f);

    EXPECT_FALSE(input.WasActionTriggered(kPlainAction));
    EXPECT_TRUE(input.WasActionTriggered(kShiftAction));
}

TEST(InputSystemTests, KeyPressUpdatesRawStateAndActionState)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kTestAction;
    desc.isAxis = false;
    desc.bindings.push_back(ActionBinding{DeviceType::Keyboard, kKeyA, 1.0f});

    input.RegisterAction(kTestContext, desc);
    input.PushContext(kTestContext);

    // Initial state: nothing pressed.
    EXPECT_FALSE(input.IsKeyDown(kKeyA));
    EXPECT_FALSE(input.WasKeyPressed(kKeyA));
    EXPECT_FALSE(input.WasKeyReleased(kKeyA));
    EXPECT_FALSE(input.IsActionActive(kTestAction));
    EXPECT_FALSE(input.WasActionTriggered(kTestAction));

    // Simulate key press.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);

    // First update: pressedThis should be visible and action should trigger.
    input.Update(0.016f);

    EXPECT_TRUE(input.IsKeyDown(kKeyA));
    EXPECT_TRUE(input.WasKeyPressed(kKeyA));
    EXPECT_FALSE(input.WasKeyReleased(kKeyA));

    EXPECT_TRUE(input.IsActionActive(kTestAction));
    EXPECT_TRUE(input.WasActionTriggered(kTestAction));

    // Next frame: transient flags should be cleared, but key still held.
    input.Update(0.016f);

    EXPECT_TRUE(input.IsKeyDown(kKeyA));
    EXPECT_FALSE(input.WasKeyPressed(kKeyA));
    EXPECT_FALSE(input.WasKeyReleased(kKeyA));

    EXPECT_TRUE(input.IsActionActive(kTestAction));
    EXPECT_FALSE(input.WasActionTriggered(kTestAction));

    // Release the key.
    input.OnKey(kKeyA, /*action=*/0, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_FALSE(input.IsKeyDown(kKeyA));
    EXPECT_FALSE(input.WasKeyPressed(kKeyA));
    EXPECT_TRUE(input.WasKeyReleased(kKeyA));

    EXPECT_FALSE(input.IsActionActive(kTestAction));
}

TEST(InputSystemTests, HigherPriorityContextConsumesAction)
{
    InputSystem input;

    constexpr ContextId kUiContext = HashInput("UI");
    constexpr ContextId kGameplayContext = HashInput("Gameplay");

    ActionDesc desc{};
    desc.id = kTestAction;
    desc.isAxis = false;
    desc.bindings.push_back(ActionBinding{DeviceType::Keyboard, kKeyA, 1.0f});

    input.RegisterAction(kUiContext, desc);
    input.RegisterAction(kGameplayContext, desc);

    bool uiReceived = false;
    bool gameplayReceived = false;

    input.AddActionListener(kTestAction, [&](ActionEvent& evt)
                            {
                                // First listener will correspond to the context that is highest priority in the stack.
                                uiReceived = true;
                                evt.consumed = true; // UI consumes the action
                            });

    input.AddActionListener(kTestAction, [&](ActionEvent& evt)
                            {
        (void)evt;
        gameplayReceived = true; });

    // Push Gameplay first, then UI so UI has higher priority.
    input.PushContext(kGameplayContext);
    input.PushContext(kUiContext);

    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_TRUE(uiReceived);
    EXPECT_FALSE(gameplayReceived) << "Consumed action should not reach lower-priority context";
}

TEST(InputSystemTests, MouseButtonActionUpdatesRawStateAndActionState)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kTestAction;
    desc.isAxis = false;
    desc.bindings.push_back(ActionBinding{DeviceType::Mouse, kMouseButtonRight, 1.0f});

    input.RegisterAction(kTestContext, desc);
    input.PushContext(kTestContext);

    // Initial state: nothing pressed.
    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonRight));
    EXPECT_FALSE(input.IsActionActive(kTestAction));
    EXPECT_FALSE(input.WasActionTriggered(kTestAction));

    // Simulate mouse button press.
    input.OnMouseButton(kMouseButtonRight, /*down=*/true, /*mods=*/0);

    // First update: button should be down and action should trigger.
    input.Update(0.016f);

    EXPECT_TRUE(input.IsMouseButtonDown(kMouseButtonRight));
    EXPECT_TRUE(input.IsActionActive(kTestAction));
    EXPECT_TRUE(input.WasActionTriggered(kTestAction));

    // Next frame: transient justPressed flag should clear but button remains held.
    input.Update(0.016f);

    EXPECT_TRUE(input.IsMouseButtonDown(kMouseButtonRight));
    EXPECT_TRUE(input.IsActionActive(kTestAction));
    EXPECT_FALSE(input.WasActionTriggered(kTestAction));

    // Release the button.
    input.OnMouseButton(kMouseButtonRight, /*down=*/false, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonRight));
    EXPECT_FALSE(input.IsActionActive(kTestAction));
}

TEST(InputSystemTests, MouseButtonEdgeQueriesReportTransitions)
{
    InputSystem input;

    // Before any event: no edges.
    EXPECT_FALSE(input.WasMouseButtonPressed(kMouseButtonRight));
    EXPECT_FALSE(input.WasMouseButtonReleased(kMouseButtonRight));

    input.OnMouseButton(kMouseButtonRight, /*down=*/true, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_TRUE(input.WasMouseButtonPressed(kMouseButtonRight));
    EXPECT_FALSE(input.WasMouseButtonReleased(kMouseButtonRight));
    EXPECT_TRUE(input.IsMouseButtonDown(kMouseButtonRight));

    // Next frame with no events: edge flags clear, held state persists.
    input.Update(0.016f);
    EXPECT_FALSE(input.WasMouseButtonPressed(kMouseButtonRight));
    EXPECT_TRUE(input.IsMouseButtonDown(kMouseButtonRight));

    input.OnMouseButton(kMouseButtonRight, /*down=*/false, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_FALSE(input.WasMouseButtonPressed(kMouseButtonRight));
    EXPECT_TRUE(input.WasMouseButtonReleased(kMouseButtonRight));
    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonRight));
}

TEST(InputSystemTests, SameFrameMouseClickPairVisibleToEdgeQueries)
{
    InputSystem input;

    // Press and release inside one frame: a live IsMouseButtonDown poll never
    // sees the click, but both frame-snapshot edges must report it.
    input.OnMouseButton(kMouseButtonRight, /*down=*/true, /*mods=*/0);
    input.OnMouseButton(kMouseButtonRight, /*down=*/false, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonRight));
    EXPECT_TRUE(input.WasMouseButtonPressed(kMouseButtonRight));
    EXPECT_TRUE(input.WasMouseButtonReleased(kMouseButtonRight));

    // Snapshot lives exactly one frame.
    input.Update(0.016f);
    EXPECT_FALSE(input.WasMouseButtonPressed(kMouseButtonRight));
    EXPECT_FALSE(input.WasMouseButtonReleased(kMouseButtonRight));
}

TEST(InputSystemTests, PointerIsOutsideTheWindowUntilTheFirstMove)
{
    // A window the pointer has not entered delivers no move. The position is
    // then the origin and the state says not to act on it: read as a real
    // corner, that silence edge-scrolls a game toward a pointer that is not there.
    InputSystem input;
    input.Update(0.016f);
    EXPECT_FALSE(input.IsPointerInWindow());
    EXPECT_EQ(input.GetMousePosition(), Vector2(0.0f, 0.0f));

    input.OnMouseMove({0.0f, 0.0f});
    EXPECT_TRUE(input.IsPointerInWindow());
    EXPECT_EQ(input.GetMousePosition(), Vector2(0.0f, 0.0f));
}

TEST(InputSystemTests, MouseLeaveTakesThePointerOutOfTheWindowAndKeepsItsLastPosition)
{
    InputSystem input;
    input.OnMouseMove({30.0f, 40.0f});
    input.OnMouseLeave();
    input.Update(0.016f);
    EXPECT_FALSE(input.IsPointerInWindow());
    EXPECT_EQ(input.GetMousePosition(), Vector2(30.0f, 40.0f));

    input.OnMouseMove({50.0f, 60.0f});
    EXPECT_TRUE(input.IsPointerInWindow());
    EXPECT_EQ(input.GetMousePosition(), Vector2(50.0f, 60.0f));
}

TEST(InputSystemTests, WindowFocusIsFalseUntilReportedAndLossReleasesWhatIsHeld)
{
    InputSystem input;
    EXPECT_FALSE(input.IsWindowFocused());
    input.OnWindowFocus(true);
    EXPECT_TRUE(input.IsWindowFocused());

    input.OnMouseMove({10.0f, 20.0f});
    input.OnKey(kKeyA, kKeyActionPress, 0);
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    input.Update(0.016f);
    ASSERT_TRUE(input.IsKeyDown(kKeyA));
    ASSERT_TRUE(input.GetMouseButtonPressPosition(kMouseButtonLeft));

    input.OnWindowFocus(false);
    input.Update(0.016f);
    EXPECT_FALSE(input.IsWindowFocused());
    EXPECT_FALSE(input.IsKeyDown(kKeyA));
    EXPECT_TRUE(input.WasKeyReleased(kKeyA));
    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonLeft));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonLeft));
    // The pointer is where it was: a window behind another still has one.
    EXPECT_TRUE(input.IsPointerInWindow());
    EXPECT_EQ(input.GetMousePosition(), Vector2(10.0f, 20.0f));
}

TEST(InputSystemTests, FocusLossReleasesHeldInputEvenWhenTheGainWasNeverReported)
{
    // A window created focused reports no gain to a sink bound afterwards;
    // its loss must still let go of what the sink holds.
    InputSystem input;
    input.OnKey(kKeyA, kKeyActionPress, 0);
    input.Update(0.016f);
    ASSERT_TRUE(input.IsKeyDown(kKeyA));

    input.OnWindowFocus(false);
    input.Update(0.016f);
    EXPECT_FALSE(input.IsKeyDown(kKeyA));
}

TEST(InputSystemTests, ResetStateLeavesThePointerAndFocusStatesAlone)
{
    InputSystem input;
    input.OnMouseMove({7.0f, 8.0f});
    input.OnWindowFocus(true);
    input.ResetState();
    EXPECT_TRUE(input.IsPointerInWindow());
    EXPECT_TRUE(input.IsWindowFocused());
    EXPECT_EQ(input.GetMousePosition(), Vector2(7.0f, 8.0f));
}

TEST(InputSystemTests, PointerArrivingFromOutsideTheWindowCarriesNoDelta)
{
    InputSystem input;
    ActionDesc desc{};
    desc.id = kMouseAxisAction;
    desc.isAxis = true;
    desc.bindings.push_back(ActionBinding{DeviceType::Mouse, kMouseAxisX, 1.0f});
    input.RegisterAction(kMouseContext, desc);
    input.PushContext(kMouseContext);

    // First arrival: nothing this sink saw travelled to (800, 400).
    input.OnMouseMove({800.0f, 400.0f});
    input.Update(0.016f);
    EXPECT_FLOAT_EQ(input.GetActionState(kMouseAxisAction).value, 0.0f);

    // Re-entry after a leave: the path outside the window was not seen either.
    input.OnMouseLeave();
    input.OnMouseMove({10.0f, 400.0f});
    input.Update(0.016f);
    EXPECT_FLOAT_EQ(input.GetActionState(kMouseAxisAction).value, 0.0f);

    input.OnMouseMove({14.0f, 400.0f});
    input.Update(0.016f);
    EXPECT_FLOAT_EQ(input.GetActionState(kMouseAxisAction).value, 4.0f);
}

TEST(InputSystemTests, MousePressWhileThePointerIsOutsideRecordsNoOrigin)
{
    InputSystem input;
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    input.Update(0.016f);
    EXPECT_TRUE(input.WasMouseButtonPressed(kMouseButtonLeft));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonLeft));

    // Nor after a leave: the last position inside is not where this press was.
    input.OnMouseButton(kMouseButtonLeft, false, 0);
    input.OnMouseMove({10.0f, 20.0f});
    input.OnMouseLeave();
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    input.Update(0.016f);
    EXPECT_TRUE(input.WasMouseButtonPressed(kMouseButtonLeft));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonLeft));
}

TEST(InputSystemTests, MousePressPositionSurvivesMotionBeforeUpdate)
{
    InputSystem input;
    input.OnMouseMove({1705.0f, 251.0f}); // Inspector, outside a 1531 x 929 game view.
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    input.OnMouseMove({685.0f, 551.0f}); // Already inside before the game first polls.
    input.Update(0.016f);

    ASSERT_TRUE(input.IsMouseButtonDown(kMouseButtonLeft));
    EXPECT_TRUE(input.WasMouseButtonPressed(kMouseButtonLeft));
    EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonLeft), Vector2(1705.0f, 251.0f));
    EXPECT_EQ(input.GetMousePosition(), Vector2(685.0f, 551.0f));
}

TEST(InputSystemTests, MousePressPositionIsEmptyUntilAnAcceptedPress)
{
    InputSystem input;
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonLeft));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(-1));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(8));
    input.OnMouseMove({120.0f, 240.0f});
    input.OnMouseButton(kMouseButtonLeft, false, 0);
    input.Update(0.016f);
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonLeft));
}

TEST(InputSystemTests, MousePressPositionPersistsAcrossUpdatesAndRelease)
{
    InputSystem input;
    input.OnMouseMove({12.0f, 24.0f});
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    for (int frame = 0; frame < 3; ++frame)
    {
        input.OnMouseMove({100.0f + static_cast<float>(frame), 200.0f});
        input.Update(0.016f);
        EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonLeft), Vector2(12.0f, 24.0f));
    }
    input.OnMouseButton(kMouseButtonLeft, false, 0);
    input.OnMouseMove({500.0f, 600.0f});
    input.Update(0.016f);
    EXPECT_TRUE(input.WasMouseButtonReleased(kMouseButtonLeft));
    EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonLeft), Vector2(12.0f, 24.0f));
    input.Update(0.016f);
    EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonLeft), Vector2(12.0f, 24.0f));
}

TEST(InputSystemTests, DuplicateMouseDownDoesNotReplacePressPosition)
{
    InputSystem input;
    input.OnMouseMove({-20.0f, 30.0f});
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    input.Update(0.016f);
    input.OnMouseMove({40.0f, 50.0f});
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    input.Update(0.016f);
    EXPECT_FALSE(input.WasMouseButtonPressed(kMouseButtonLeft));
    EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonLeft), Vector2(-20.0f, 30.0f));
}

TEST(InputSystemTests, SameFrameMouseClickPreservesPressPositionAndBothEdges)
{
    InputSystem input;
    input.OnMouseMove({1705.0f, 251.0f});
    input.OnMouseButton(kMouseButtonRight, true, 0);
    input.OnMouseMove({685.0f, 551.0f});
    input.OnMouseButton(kMouseButtonRight, false, 0);
    input.Update(0.016f);
    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonRight));
    EXPECT_TRUE(input.WasMouseButtonPressed(kMouseButtonRight));
    EXPECT_TRUE(input.WasMouseButtonReleased(kMouseButtonRight));
    EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonRight), Vector2(1705.0f, 251.0f));
}

TEST(InputSystemTests, MouseButtonsRetainIndependentMostRecentPresses)
{
    InputSystem input;
    input.OnMouseMove({10.0f, 20.0f});
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    input.OnMouseMove({30.0f, 40.0f});
    input.OnMouseButton(kMouseButtonRight, true, 0);
    input.OnMouseButton(kMouseButtonLeft, false, 0);
    input.OnMouseMove({50.0f, 60.0f});
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    input.Update(0.016f);
    EXPECT_TRUE(input.IsMouseButtonDown(kMouseButtonLeft));
    EXPECT_TRUE(input.WasMouseButtonPressed(kMouseButtonLeft));
    EXPECT_TRUE(input.WasMouseButtonReleased(kMouseButtonLeft));
    EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonLeft), Vector2(50.0f, 60.0f));
    EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonRight), Vector2(30.0f, 40.0f));
}

TEST(InputSystemTests, ResetClearsHeldAndPreviouslyReleasedMousePressPositions)
{
    InputSystem input;
    input.OnMouseMove({10.0f, 20.0f});
    input.OnMouseButton(kMouseButtonLeft, true, 0);
    input.OnMouseButton(kMouseButtonRight, true, 0);
    input.OnMouseButton(kMouseButtonRight, false, 0);
    input.Update(0.016f);
    input.Update(0.016f); // The released button is no longer dirty.
    input.ResetState();
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonLeft));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonRight));
    input.Update(0.016f);
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonLeft));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonRight));
    EXPECT_EQ(input.GetMousePosition(), Vector2(10.0f, 20.0f));
}

TEST(InputSystemTests, PressPositionPollingClaimsOnlyTheQueriedGameplayButton)
{
    InputSystem input{SinkRole::Gameplay};
    input.OnMouseMove({12.0f, 24.0f});
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonLeft));
    EXPECT_TRUE(input.OnMouseButton(kMouseButtonLeft, true, 0));
    EXPECT_FALSE(input.OnMouseButton(kMouseButtonRight, true, 0));
    EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonLeft), Vector2(12.0f, 24.0f));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(kMouseButtonRight));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(-1));
    EXPECT_FALSE(input.GetMouseButtonPressPosition(8));
}

TEST(InputSystemTests, DeclinedMousePressRetainsTheLastAcceptedPosition)
{
    InputSystem input{SinkRole::Gameplay};
    (void)input.GetMouseButtonPressPosition(kMouseButtonLeft);
    input.OnMouseMove({12.0f, 24.0f});
    ASSERT_TRUE(input.OnMouseButton(kMouseButtonLeft, true, 0));
    input.OnMouseButton(kMouseButtonLeft, false, 0);
    input.SetClaimsSuspended(true);
    input.OnMouseMove({100.0f, 200.0f});
    EXPECT_FALSE(input.OnMouseButton(kMouseButtonLeft, true, 0));
    input.Update(0.016f);
    EXPECT_EQ(input.GetMouseButtonPressPosition(kMouseButtonLeft), Vector2(12.0f, 24.0f));
}

TEST(InputSystemTests, MouseAxisActionUsesMouseDelta)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kMouseAxisAction;
    desc.isAxis = true;
    desc.bindings.push_back(ActionBinding{DeviceType::Mouse, kMouseAxisX, 0.5f});

    input.RegisterAction(kMouseContext, desc);
    input.PushContext(kMouseContext);
    input.OnMouseMove({0.0f, 0.0f});
    input.Update(0.016f);

    // No movement yet.
    ActionState st = input.GetActionState(kMouseAxisAction);
    EXPECT_FLOAT_EQ(st.value, 0.0f);

    // Move mouse 10 units in X; expect 10 * 0.5f.
    input.OnMouseMove({10.0f, 0.0f});
    input.Update(0.016f);

    st = input.GetActionState(kMouseAxisAction);
    EXPECT_NEAR(st.value, 5.0f, 1e-5f);

    // No further movement: axis should return to 0 next frame.
    input.Update(0.016f);

    st = input.GetActionState(kMouseAxisAction);
    EXPECT_FLOAT_EQ(st.value, 0.0f);
}

TEST(InputSystemTests, ResetStateReleasesHeldKeysAndMouseButtons)
{
    InputSystem input;

    // Press a key and a mouse button.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.OnMouseButton(kMouseButtonRight, /*down=*/true, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_TRUE(input.IsKeyDown(kKeyA));
    EXPECT_TRUE(input.IsMouseButtonDown(kMouseButtonRight));

    // ResetState should release everything.
    input.ResetState();
    input.Update(0.016f);

    EXPECT_FALSE(input.IsKeyDown(kKeyA));
    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonRight));
    EXPECT_TRUE(input.WasKeyReleased(kKeyA));

    // Next frame: transient release flag should clear.
    input.Update(0.016f);
    EXPECT_FALSE(input.WasKeyReleased(kKeyA));
}

TEST(InputSystemTests, ResetStateDoesNotAffectUnpressedKeys)
{
    InputSystem input;

    // Press and release A normally.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.OnKey(kKeyA, /*action=*/0, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_FALSE(input.IsKeyDown(kKeyA));

    // ResetState should be harmless when nothing is held.
    input.ResetState();
    input.Update(0.016f);

    EXPECT_FALSE(input.IsKeyDown(kKeyA));
    EXPECT_FALSE(input.WasKeyReleased(kKeyA));
}

TEST(InputSystemTests, ClearRegistrationsRemovesActionsAndContexts)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kTestAction;
    desc.isAxis = false;
    desc.bindings.push_back(ActionBinding{DeviceType::Keyboard, kKeyA, 1.0f});

    input.RegisterAction(kTestContext, desc);
    input.PushContext(kTestContext);

    // Action should be queryable.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_TRUE(input.IsActionActive(kTestAction));

    // Clear everything.
    input.ClearRegistrations();

    // Action state should now return default (inactive).
    EXPECT_FALSE(input.IsActionActive(kTestAction));

    // Re-registering should work cleanly.
    input.RegisterAction(kTestContext, desc);
    input.PushContext(kTestContext);

    input.OnKey(kKeyA, /*action=*/0, /*mods=*/0);
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_TRUE(input.IsActionActive(kTestAction));
}

TEST(InputSystemTests, ClearRegistrationsRemovesListeners)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kTestAction;
    desc.isAxis = false;
    desc.bindings.push_back(ActionBinding{DeviceType::Keyboard, kKeyA, 1.0f});

    input.RegisterAction(kTestContext, desc);
    input.PushContext(kTestContext);

    int callCount = 0;
    input.AddActionListener(kTestAction, [&](ActionEvent&) { ++callCount; });

    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_EQ(callCount, 1);

    input.ClearRegistrations();

    // Re-register action but NOT the listener.
    input.RegisterAction(kTestContext, desc);
    input.PushContext(kTestContext);

    input.OnKey(kKeyA, /*action=*/0, /*mods=*/0);
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);

    // Listener should not fire after ClearRegistrations.
    EXPECT_EQ(callCount, 1);
}

TEST(InputSystemTests, SameFramePressAndRelease)
{
    InputSystem input;

    // Press and release A in the same frame (before Update).
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.OnKey(kKeyA, /*action=*/0, /*mods=*/0);
    input.Update(0.016f);

    // Key is no longer held, but both transitions should be visible.
    EXPECT_FALSE(input.IsKeyDown(kKeyA));
    EXPECT_TRUE(input.WasKeyPressed(kKeyA));
    EXPECT_TRUE(input.WasKeyReleased(kKeyA));

    // Next frame: transients clear.
    input.Update(0.016f);
    EXPECT_FALSE(input.WasKeyPressed(kKeyA));
    EXPECT_FALSE(input.WasKeyReleased(kKeyA));
}

TEST(InputSystemTests, GlfwRepeatDoesNotRetriggerPress)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kTestAction;
    desc.isAxis = false;
    desc.bindings.push_back(ActionBinding{DeviceType::Keyboard, kKeyA, 1.0f});

    input.RegisterAction(kTestContext, desc);
    input.PushContext(kTestContext);

    // Initial press.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_TRUE(input.WasKeyPressed(kKeyA));
    EXPECT_TRUE(input.WasActionTriggered(kTestAction));

    // GLFW_REPEAT (action=2) should not re-trigger pressedThis.
    input.OnKey(kKeyA, /*action=*/2, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_TRUE(input.IsKeyDown(kKeyA));
    EXPECT_FALSE(input.WasKeyPressed(kKeyA));
    EXPECT_FALSE(input.WasActionTriggered(kTestAction));
}

TEST(InputSystemTests, MultipleKeysPressedSameFrame)
{
    InputSystem input;

    ActionDesc descA{};
    descA.id = kActionA;
    descA.isAxis = false;
    descA.bindings.push_back(ActionBinding{DeviceType::Keyboard, kKeyA, 1.0f});

    ActionDesc descB{};
    descB.id = kActionB;
    descB.isAxis = false;
    descB.bindings.push_back(ActionBinding{DeviceType::Keyboard, kKeyB, 1.0f});

    input.RegisterAction(kTestContext, descA);
    input.RegisterAction(kTestContext, descB);
    input.PushContext(kTestContext);

    // Press both A and B in the same frame.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.OnKey(kKeyB, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_TRUE(input.WasKeyPressed(kKeyA));
    EXPECT_TRUE(input.WasKeyPressed(kKeyB));
    EXPECT_TRUE(input.WasActionTriggered(kActionA));
    EXPECT_TRUE(input.WasActionTriggered(kActionB));
}

TEST(InputSystemTests, RemoveActionListenerStopsCallbacks)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kTestAction;
    desc.isAxis = false;
    desc.bindings.push_back(ActionBinding{DeviceType::Keyboard, kKeyA, 1.0f});

    input.RegisterAction(kTestContext, desc);
    input.PushContext(kTestContext);

    int callCount = 0;
    auto handle = input.AddActionListener(kTestAction, [&](ActionEvent&) { ++callCount; });

    // First press: listener fires.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_EQ(callCount, 1);

    // Remove listener.
    input.RemoveActionListener(handle);

    // Second press: listener should NOT fire.
    input.OnKey(kKeyA, /*action=*/0, /*mods=*/0);
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_EQ(callCount, 1);
}

TEST(InputSystemTests, DisabledContextDoesNotProduceActionState)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kTestAction;
    desc.isAxis = false;
    desc.bindings.push_back(ActionBinding{DeviceType::Keyboard, kKeyA, 1.0f});

    input.RegisterAction(kTestContext, desc);
    input.PushContext(kTestContext);

    // Press key, context enabled -> action fires.
    input.OnKey(kKeyA, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_TRUE(input.IsActionActive(kTestAction));

    // Disable context while key is held.
    input.SetContextEnabled(kTestContext, false);
    input.Update(0.016f);
    EXPECT_FALSE(input.IsActionActive(kTestAction));

    // Re-enable: key is still held, action should resume.
    input.SetContextEnabled(kTestContext, true);
    input.Update(0.016f);
    EXPECT_TRUE(input.IsActionActive(kTestAction));
}

TEST(InputSystemTests, OutOfRangeKeyCodeIsIgnored)
{
    InputSystem input;

    // Negative key (GLFW_KEY_UNKNOWN = -1) and too-large key should not crash.
    input.OnKey(-1, /*action=*/1, /*mods=*/0);
    input.OnKey(9999, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_FALSE(input.IsKeyDown(-1));
    EXPECT_FALSE(input.IsKeyDown(9999));
    EXPECT_FALSE(input.WasKeyPressed(-1));
    EXPECT_FALSE(input.WasKeyPressed(9999));
}

TEST(InputSystemTests, OutOfRangeMouseButtonIsIgnored)
{
    InputSystem input;

    input.OnMouseButton(-1, true, /*mods=*/0);
    input.OnMouseButton(100, true, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_FALSE(input.IsMouseButtonDown(-1));
    EXPECT_FALSE(input.IsMouseButtonDown(100));
}

TEST(InputSystemTests, MouseDeltaAccumulatesWithinFrame)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kMouseAxisAction;
    desc.isAxis = true;
    desc.bindings.push_back(ActionBinding{DeviceType::Mouse, kMouseAxisX, 0.5f});

    input.RegisterAction(kMouseContext, desc);
    input.PushContext(kMouseContext);
    input.OnMouseMove({0.0f, 0.0f});
    input.Update(0.016f);

    // Two moves coalesced into one frame travel 4 then 6 more. The axis reads
    // the whole 10, not just the last segment: a reader that polls once a frame
    // must not lose the motion the earlier events carried.
    input.OnMouseMove({4.0f, 0.0f});
    input.OnMouseMove({10.0f, 0.0f});
    input.Update(0.016f);

    ActionState st = input.GetActionState(kMouseAxisAction);
    EXPECT_NEAR(st.value, 5.0f, 1e-5f);

    // Next frame: the delta resets, like scroll.
    input.Update(0.016f);
    st = input.GetActionState(kMouseAxisAction);
    EXPECT_FLOAT_EQ(st.value, 0.0f);
}

TEST(InputSystemTests, ScrollAccumulatesWithinFrame)
{
    InputSystem input;

    ActionDesc desc{};
    desc.id = kMouseAxisAction;
    desc.isAxis = true;
    desc.bindings.push_back(ActionBinding{DeviceType::Mouse, kMouseScrollAxisY, 1.0f});

    input.RegisterAction(kMouseContext, desc);
    input.PushContext(kMouseContext);

    // Multiple scroll events in one frame should accumulate.
    input.OnMouseScroll({0.0f, 3.0f}, /*mods=*/0);
    input.OnMouseScroll({0.0f, 2.0f}, /*mods=*/0);
    input.Update(0.016f);

    ActionState st = input.GetActionState(kMouseAxisAction);
    EXPECT_NEAR(st.value, 5.0f, 1e-5f);

    // Next frame: scroll resets.
    input.Update(0.016f);
    st = input.GetActionState(kMouseAxisAction);
    EXPECT_FLOAT_EQ(st.value, 0.0f);
}

TEST(InputSystemTests, BindKeyAutoCreatesAction)
{
    InputSystem input;

    input.PushContext(kTestContext);

    // BindKey without prior RegisterAction should implicitly create the action.
    input.BindKey(kTestContext, kTestAction, ActionBinding{DeviceType::Keyboard, kKeyW, 1.0f});

    input.OnKey(kKeyW, /*action=*/1, /*mods=*/0);
    input.Update(0.016f);

    EXPECT_TRUE(input.IsActionActive(kTestAction));
    EXPECT_TRUE(input.WasActionTriggered(kTestAction));
}

TEST(InputSystemTests, ProducesTextInputCoversEveryCharacterProducingKey)
{
    // Letters, digits and punctuation live in one contiguous ASCII-valued run.
    EXPECT_TRUE(ProducesTextInput(kKeyCode_Space));
    EXPECT_TRUE(ProducesTextInput(kKeyCode_A));
    EXPECT_TRUE(ProducesTextInput(kKeyCode_0));
    EXPECT_TRUE(ProducesTextInput(kKeyCode_Comma));
    EXPECT_TRUE(ProducesTextInput(kKeyCode_GraveAccent));

    // The keypad types too, apart from its Enter.
    EXPECT_TRUE(ProducesTextInput(kKeyCode_NumPad0));
    EXPECT_TRUE(ProducesTextInput(kKeyCode_NumPadEqual));
    EXPECT_FALSE(ProducesTextInput(kKeyCode_NumPadEnter));

    // The non-US printing keys sit outside that ASCII run. Classifying them as
    // non-typing is the inverse of the leak this predicate exists to stop: the
    // field would edit on the character while the key itself reached gameplay.
    EXPECT_TRUE(ProducesTextInput(kKeyCode_World1));
    EXPECT_TRUE(ProducesTextInput(kKeyCode_World2));

    // Navigation, editing and function keys carry no character.
    EXPECT_FALSE(ProducesTextInput(kKeyCode_Left));
    EXPECT_FALSE(ProducesTextInput(kKeyCode_Backspace));
    EXPECT_FALSE(ProducesTextInput(kKeyCode_Delete));
    EXPECT_FALSE(ProducesTextInput(kKeyCode_Escape));
    EXPECT_FALSE(ProducesTextInput(kKeyCode_Tab));
    EXPECT_FALSE(ProducesTextInput(kKeyCode_Enter));
    EXPECT_FALSE(ProducesTextInput(kKeyCode_F1));
    EXPECT_FALSE(ProducesTextInput(kKeyCode_LeftShift));
}

TEST(InputSystemTests, ComposesTextInputSplitsAltChordsByPlatform)
{
    // A plain or shifted keystroke composes on every platform; a
    // primary-modifier chord never does.
    EXPECT_TRUE(ComposesTextInput(kKeyCode_F, 0));
    EXPECT_TRUE(ComposesTextInput(kKeyCode_F, kModShift));
    EXPECT_FALSE(ComposesTextInput(kKeyCode_F, kModControl));
    EXPECT_FALSE(ComposesTextInput(kKeyCode_F, kModSuper));

    // A key that carries no character composes nothing under any modifiers.
    EXPECT_FALSE(ComposesTextInput(kKeyCode_Left, 0));
    EXPECT_FALSE(ComposesTextInput(kKeyCode_Left, kModAlt));

#if defined(__APPLE__)
    // Option is a composition modifier (Option+F types 'ƒ'): the keystroke is
    // text and stays with the focused control.
    EXPECT_TRUE(ComposesTextInput(kKeyCode_F, kModAlt));
    EXPECT_TRUE(ComposesTextInput(kKeyCode_F, kModAlt | kModShift));
#else
    // Alt composes nothing here: Alt+letter is a chord (the default Frame All
    // is Alt+F) and must stay available to the binding that implements it.
    EXPECT_FALSE(ComposesTextInput(kKeyCode_F, kModAlt));
    EXPECT_FALSE(ComposesTextInput(kKeyCode_F, kModAlt | kModShift));
#endif
}

// --- Gameplay-sink claims ---------------------------------------------------
//
// Every test above drives the default Application sink, which ends its chain and
// so takes whatever it is delivered. A Gameplay sink runs ahead of the rest of
// the chain and takes only what it CLAIMS — a press matching a binding of an
// enabled context, or a code something is polling — recording exactly that, so
// its edge queries and the routing chain can never tell different stories.

TEST(InputSystemTests, ApplicationSinkConsumesEveryPressItIsDelivered)
{
    InputSystem input;

    EXPECT_TRUE(input.OnKey(kKeyA, kKeyActionPress, /*mods=*/0));
    EXPECT_FALSE(input.OnKey(kKeyA, kKeyActionRelease, /*mods=*/0)) << "releases are never consumed";
    EXPECT_TRUE(input.OnMouseButton(kMouseButtonRight, /*down=*/true, /*mods=*/0));
    EXPECT_FALSE(input.OnMouseButton(kMouseButtonRight, /*down=*/false, /*mods=*/0));
    EXPECT_TRUE(input.OnMouseScroll({0.0f, 1.0f}, /*mods=*/0));
}

TEST(InputSystemTests, GameplaySinkDeclinesAPressNothingBindsOrPolls)
{
    InputSystem input{SinkRole::Gameplay};

    // A declined press is not stored, so nothing about it can surface later as an
    // edge for a press that went on to fire editor behaviour instead.
    EXPECT_FALSE(input.OnKey(kKeyA, kKeyActionPress, /*mods=*/0));
    input.Update(0.016f);
    EXPECT_FALSE(input.IsKeyDown(kKeyA));
    EXPECT_FALSE(input.WasKeyPressed(kKeyA));

    // The release still arrives, and clears state this sink never held: a no-op,
    // not a phantom release edge.
    EXPECT_FALSE(input.OnKey(kKeyA, kKeyActionRelease, /*mods=*/0));
    input.Update(0.016f);
    EXPECT_FALSE(input.WasKeyReleased(kKeyA));
}

TEST(InputSystemTests, GameplaySinkClaimsABoundKey)
{
    InputSystem input{SinkRole::Gameplay};
    input.BindKey(kTestContext, kTestAction, {DeviceType::Keyboard, kKeyW, 1.0f});
    input.PushContext(kTestContext);

    EXPECT_TRUE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));
    EXPECT_FALSE(input.OnKey(kKeyB, kKeyActionPress, /*mods=*/0)) << "an unbound key is not the game's";

    input.Update(0.016f);
    EXPECT_TRUE(input.WasKeyPressed(kKeyW));
    EXPECT_TRUE(input.IsActionActive(kTestAction));
    EXPECT_FALSE(input.WasKeyPressed(kKeyB));
}

TEST(InputSystemTests, GameplaySinkClaimsOnlyTheModifierCombinationItBinds)
{
    InputSystem input{SinkRole::Gameplay};

    ActionBinding shiftA{DeviceType::Keyboard, kKeyA, 1.0f};
    shiftA.requiredMods = kModShift;
    input.BindKey(kTestContext, kShiftAction, shiftA);
    input.PushContext(kTestContext);

    // Claims are judged by the same rule the action itself is: a sink never takes
    // a combination its action would then ignore.
    EXPECT_FALSE(input.OnKey(kKeyA, kKeyActionPress, /*mods=*/0));
    EXPECT_TRUE(input.OnKey(kKeyA, kKeyActionPress, kModShift));
}

TEST(InputSystemTests, DisabledContextClaimsNothing)
{
    InputSystem input{SinkRole::Gameplay};
    input.BindKey(kTestContext, kTestAction, {DeviceType::Keyboard, kKeyA, 1.0f});
    input.PushContext(kTestContext);
    input.SetContextEnabled(kTestContext, false);

    // A context that cannot move an action cannot claim its keys either — this is
    // what makes the editor's own Game View context switch off cleanly.
    EXPECT_FALSE(input.OnKey(kKeyA, kKeyActionPress, /*mods=*/0));

    input.SetContextEnabled(kTestContext, true);
    EXPECT_TRUE(input.OnKey(kKeyA, kKeyActionPress, /*mods=*/0));
}

TEST(InputSystemTests, PollingClaimsTheKeyItReadsEvenWhenItReadsFalse)
{
    InputSystem input{SinkRole::Gameplay};

    // Poll-style game code registers nothing, so bindings cannot see it; reading
    // the key is the declaration. IsKeyDown returning false is still a read, which
    // is why a script claims its keys before anything has been pressed.
    EXPECT_FALSE(input.IsKeyDown(kKeyW));
    EXPECT_TRUE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));
    EXPECT_FALSE(input.OnKey(kKeyB, kKeyActionPress, /*mods=*/0));
}

TEST(InputSystemTests, PollClaimSurvivesAMissedFrameThenLapses)
{
    InputSystem input{SinkRole::Gameplay};

    // The claim is a sliding window, so a caller that ticks below frame rate or
    // skips a frame keeps its keys.
    EXPECT_FALSE(input.IsKeyDown(kKeyW));
    input.Update(0.016f);
    input.Update(0.016f);
    EXPECT_TRUE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));
    input.OnKey(kKeyW, kKeyActionRelease, /*mods=*/0);

    // One frame further with nothing reading the key and it belongs to whatever
    // is downstream again.
    input.Update(0.016f);
    EXPECT_FALSE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));
}

TEST(InputSystemTests, GameplaySinkKeepsTheHoldItAlreadyTook)
{
    InputSystem input{SinkRole::Gameplay};
    EXPECT_FALSE(input.IsKeyDown(kKeyW));
    ASSERT_TRUE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));

    // The claim lapses while the key is still physically held. Repeats and the
    // release belong to the sink that took the press, or the game would be left
    // holding a key nothing can end.
    input.Update(0.016f);
    input.Update(0.016f);
    input.Update(0.016f);
    EXPECT_TRUE(input.OnKey(kKeyW, kKeyActionRepeat, /*mods=*/0));

    input.OnKey(kKeyW, kKeyActionRelease, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_FALSE(input.IsKeyDown(kKeyW));
}

TEST(InputSystemTests, ClearRegistrationsDropsPollClaims)
{
    InputSystem input{SinkRole::Gameplay};
    EXPECT_FALSE(input.IsKeyDown(kKeyW));

    // Claims are session state: the code that was reading these keys is gone with
    // the registrations, so the play session that follows starts clean.
    input.ClearRegistrations();
    EXPECT_FALSE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));
}

TEST(InputSystemTests, GameplaySinkClaimsBoundMouseButtonsOnly)
{
    InputSystem input{SinkRole::Gameplay};
    input.BindKey(kMouseContext, kTestAction, {DeviceType::Mouse, kMouseButtonRight, 1.0f});
    input.PushContext(kMouseContext);

    EXPECT_TRUE(input.OnMouseButton(kMouseButtonRight, /*down=*/true, /*mods=*/0));
    EXPECT_FALSE(input.OnMouseButton(kMouseButtonLeft, /*down=*/true, /*mods=*/0));

    input.Update(0.016f);
    EXPECT_TRUE(input.IsMouseButtonDown(kMouseButtonRight));
    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonLeft));
}

TEST(InputSystemTests, PollingClaimsTheMouseButtonItReads)
{
    InputSystem input{SinkRole::Gameplay};

    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonLeft));
    EXPECT_TRUE(input.OnMouseButton(kMouseButtonLeft, /*down=*/true, /*mods=*/0));
    EXPECT_FALSE(input.OnMouseButton(kMouseButtonRight, /*down=*/true, /*mods=*/0));
}

TEST(InputSystemTests, GameplaySinkClaimsScrollOnlyForTheBoundAxis)
{
    InputSystem input{SinkRole::Gameplay};

    ActionDesc zoom{};
    zoom.id = kMouseAxisAction;
    zoom.isAxis = true;
    zoom.bindings.push_back({DeviceType::Mouse, kMouseScrollAxisY, 1.0f});
    input.RegisterAction(kMouseContext, zoom);
    input.PushContext(kMouseContext);

    // A wheel has no press to hold and no polling query to read it, so a bound
    // axis is the only thing that can claim one — and only the axis that moved:
    // a game bound to wheel zoom does not take a horizontal trackpad pan.
    EXPECT_TRUE(input.OnMouseScroll({0.0f, 1.0f}, /*mods=*/0));
    EXPECT_FALSE(input.OnMouseScroll({1.0f, 0.0f}, /*mods=*/0));
}

TEST(InputSystemTests, SuspendedGameplaySinkClaimsNothing)
{
    InputSystem input{SinkRole::Gameplay};
    input.BindKey(kTestContext, kTestAction, {DeviceType::Keyboard, kKeyW, 1.0f});
    input.BindKey(kMouseContext, kActionA, {DeviceType::Mouse, kMouseButtonRight, 1.0f});
    input.PushContext(kTestContext);
    input.PushContext(kMouseContext);

    ASSERT_TRUE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0)) << "precondition: the binding claims W";
    input.OnKey(kKeyW, kKeyActionRelease, /*mods=*/0);

    // A paused game acts on no input, so its sink answers for none of it — the
    // binding and the poll claim are both ignored while it is suspended.
    input.SetClaimsSuspended(true);
    EXPECT_FALSE(input.IsKeyDown(kKeyB));
    EXPECT_FALSE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));
    EXPECT_FALSE(input.OnKey(kKeyB, kKeyActionPress, /*mods=*/0));
    EXPECT_FALSE(input.OnMouseButton(kMouseButtonRight, /*down=*/true, /*mods=*/0));
    input.Update(0.016f);
    EXPECT_FALSE(input.IsKeyDown(kKeyW));
    EXPECT_FALSE(input.IsMouseButtonDown(kMouseButtonRight));

    // Resuming restores both claim sources.
    input.SetClaimsSuspended(false);
    EXPECT_TRUE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));
    EXPECT_TRUE(input.OnMouseButton(kMouseButtonRight, /*down=*/true, /*mods=*/0));
}

TEST(InputSystemTests, SuspensionDoesNotStrandAHeldKeyOrTouchAnApplicationSink)
{
    InputSystem input{SinkRole::Gameplay};
    input.BindKey(kTestContext, kTestAction, {DeviceType::Keyboard, kKeyW, 1.0f});
    input.PushContext(kTestContext);
    ASSERT_TRUE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));

    // Suspending mid-hold declines the repeats, but the release is not a claim
    // and still clears the key this sink holds: a pause cannot strand one down.
    input.SetClaimsSuspended(true);
    EXPECT_FALSE(input.OnKey(kKeyW, kKeyActionRepeat, /*mods=*/0));
    input.OnKey(kKeyW, kKeyActionRelease, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_FALSE(input.IsKeyDown(kKeyW));
    EXPECT_TRUE(input.WasKeyReleased(kKeyW)) << "the release this sink's own press earned";

    // An Application sink ends its chain, so there is nothing to hand input to
    // and suspension changes nothing for it.
    InputSystem app;
    app.SetClaimsSuspended(true);
    EXPECT_TRUE(app.OnKey(kKeyB, kKeyActionPress, /*mods=*/0));
}

TEST(InputSystemTests, AnEnabledContextAnswersEvenIfNothingPushedIt)
{
    // What a script authored through the scripting ABI looks like: it registers
    // an action and binds a key, and has no way to push a context. Its action has
    // to be readable anyway, or the binding is write-only.
    InputSystem input{SinkRole::Gameplay};
    input.RegisterAction(kTestContext, ActionDesc{kTestAction, {{DeviceType::Keyboard, kKeyW, 1.0f}}, false});

    EXPECT_TRUE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0)) << "the binding claims W";
    input.Update(0.016f);

    EXPECT_TRUE(input.IsActionActive(kTestAction));
    EXPECT_TRUE(input.WasActionTriggered(kTestAction));

    input.OnKey(kKeyW, kKeyActionRelease, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_FALSE(input.IsActionActive(kTestAction));
    EXPECT_TRUE(input.GetActionState(kTestAction).justReleased);
}

TEST(InputSystemTests, ClaimingAKeyAndReadingItsActionAgreeOnTheSameContexts)
{
    // The two answers are judged the same way, so a key can never be swallowed on
    // behalf of an action that reads dead. Disabling the context withdraws both.
    InputSystem input{SinkRole::Gameplay};
    input.RegisterAction(kTestContext, ActionDesc{kTestAction, {{DeviceType::Keyboard, kKeyW, 1.0f}}, false});

    input.SetContextEnabled(kTestContext, false);
    EXPECT_FALSE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));
    input.Update(0.016f);
    EXPECT_FALSE(input.IsActionActive(kTestAction));

    input.SetContextEnabled(kTestContext, true);
    EXPECT_TRUE(input.OnKey(kKeyW, kKeyActionPress, /*mods=*/0));
    input.Update(0.016f);
    EXPECT_TRUE(input.IsActionActive(kTestAction));
}

TEST(InputSystemTests, APushedContextStillOutranksAnUnpushedOne)
{
    // The fallback is a last resort, not a shortcut past priority: a context on
    // the stack answers first, and only a stack miss reaches the rest.
    InputSystem input{SinkRole::Gameplay};
    constexpr ContextId kUnpushed = HashInput("UnpushedContext");

    input.RegisterAction(kUnpushed, ActionDesc{kTestAction, {{DeviceType::Keyboard, kKeyA, 1.0f}}, false});
    input.RegisterAction(kTestContext, ActionDesc{kTestAction, {{DeviceType::Keyboard, kKeyB, 1.0f}}, false});
    input.PushContext(kTestContext);

    // A is the unpushed context's key: it claims, but the pushed context owns the
    // action id and reports its own (inactive) state.
    EXPECT_TRUE(input.OnKey(kKeyA, kKeyActionPress, /*mods=*/0));
    input.Update(0.016f);
    EXPECT_FALSE(input.IsActionActive(kTestAction)) << "the pushed context answers for this action";

    EXPECT_TRUE(input.OnKey(kKeyB, kKeyActionPress, /*mods=*/0));
    input.Update(0.016f);
    EXPECT_TRUE(input.IsActionActive(kTestAction));
}

// Modifiers are ambient state, not claimed input: a sink that declines a bare
// Ctrl never records it, so a chorded pointer binding can only be judged from
// the mask the event itself carries — for the claim and for the axis it drives.
TEST(InputSystemTests, ChordedScrollClaimIsJudgedFromTheEventMods)
{
    InputSystem input{SinkRole::Gameplay};

    ActionDesc zoom{};
    zoom.id = kMouseAxisAction;
    zoom.isAxis = true;
    ActionBinding ctrlWheel{DeviceType::Mouse, kMouseScrollAxisY, 1.0f};
    ctrlWheel.requiredMods = kModControl;
    zoom.bindings.push_back(ctrlWheel);
    input.RegisterAction(kMouseContext, zoom);
    input.PushContext(kMouseContext);

    // Nothing binds Ctrl alone, so the sink declines it and never learns it is
    // held. (Not asserted through IsKeyDown: a poll is itself a claim source.)
    EXPECT_FALSE(input.OnKey(kKeyCode_LeftControl, kKeyActionPress, kModControl));

    // Control: a bare wheel matches no Ctrl+Wheel binding.
    EXPECT_FALSE(input.OnMouseScroll({0.0f, 1.0f}, /*mods=*/0));
    input.Update(0.016f);
    EXPECT_FLOAT_EQ(input.GetActionState(kMouseAxisAction).value, 0.0f);

    // The chord: claimed from the event's mask, and the axis actually moves —
    // the half that reads the modifier snapshot rather than the claim.
    EXPECT_TRUE(input.OnMouseScroll({0.0f, 1.0f}, kModControl));
    input.Update(0.016f);
    EXPECT_FLOAT_EQ(input.GetActionState(kMouseAxisAction).value, 1.0f);
}

// A chord's transitions are judged by the modifiers held when the button went
// down, the way the key path judges them through modsAtPress — never by whichever
// modifier event happened to arrive last before Update. A claimed click that then
// fails to fire is the swallow-and-dead failure the claim fix exists to avoid.
TEST(InputSystemTests, MouseChordPressFiresWhenTheModifierLeavesBeforeUpdate)
{
    InputSystem input{SinkRole::Gameplay};
    ActionBinding ctrlRight{DeviceType::Mouse, kMouseButtonRight, 1.0f};
    ctrlRight.requiredMods = kModControl;
    input.BindKey(kMouseContext, kTestAction, ctrlRight);
    input.PushContext(kMouseContext);

    ASSERT_TRUE(input.OnMouseButton(kMouseButtonRight, /*down=*/true, kModControl))
        << "precondition: the chord is claimed";
    // Ctrl comes up in the same frame, before the frame's action evaluation.
    input.OnKey(kKeyCode_LeftControl, kKeyActionRelease, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_TRUE(input.WasActionTriggered(kTestAction)) << "a claimed chord must fire its press edge";
}

TEST(InputSystemTests, KeyBindingQueryUsesCurrentEnabledBindingsAndModifierRules)
{
    InputSystem input{SinkRole::Gameplay};
    EXPECT_FALSE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyA, kModControl));
    ActionBinding ctrlA{DeviceType::Keyboard, kKeyA, 1.0f};
    ctrlA.requiredMods = kModControl;
    ctrlA.forbiddenMods = kModShift | kModAlt | kModSuper;
    input.BindKey(kTestContext, kTestAction, ctrlA);
    EXPECT_TRUE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyA, kModControl | kModCapsLock));
    EXPECT_FALSE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyA, 0));
    EXPECT_FALSE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyA, kModControl | kModShift));
    EXPECT_FALSE(input.MatchesKeyBinding(kTestContext, kPlainAction, kKeyA, kModControl));
    input.SetContextEnabled(kTestContext, false);
    EXPECT_FALSE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyA, kModControl));
    input.SetContextEnabled(kTestContext, true);
    input.RegisterAction(kTestContext, {kTestAction, {{DeviceType::Keyboard, kKeyB, 1.0f}}, false});
    EXPECT_FALSE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyA, kModControl));
    EXPECT_TRUE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyB, 0));
    input.ClearBindings(kTestContext, kTestAction);
    EXPECT_FALSE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyB, 0));
    input.BindKey(kTestContext, kTestAction, {DeviceType::Mouse, kKeyB, 1.0f});
    EXPECT_FALSE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyB, 0));
    input.BindKey(kTestContext, kTestAction, ctrlA);
    input.RemoveAction(kTestContext, kTestAction);
    EXPECT_FALSE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyA, kModControl));
}

// A host reservation belongs to the action's registration, so rebinding the
// action's keys moves the reservation with them.
TEST(InputSystemTests, HostReservedKeysFollowTheFlaggedActionsCurrentBindings)
{
    InputSystem input;
    ActionBinding ctrlA{DeviceType::Keyboard, kKeyA, 1.0f};
    ctrlA.requiredMods = kModControl;
    input.RegisterAction(kTestContext, {kTestAction, {ctrlA}, false, /*hostReserved=*/true});
    input.RegisterAction(kTestContext, {kPlainAction, {{DeviceType::Keyboard, kKeyB, 1.0f}}, false});
    EXPECT_TRUE(input.IsHostReservedKey(kKeyA, kModControl));
    EXPECT_FALSE(input.IsHostReservedKey(kKeyA, 0));
    EXPECT_FALSE(input.IsHostReservedKey(kKeyB, 0)) << "an unflagged action reserves nothing";

    input.ClearBindings(kTestContext, kTestAction);
    input.BindKey(kTestContext, kTestAction, {DeviceType::Keyboard, kKeyB, 1.0f, kModAlt});
    EXPECT_FALSE(input.IsHostReservedKey(kKeyA, kModControl));
    EXPECT_TRUE(input.IsHostReservedKey(kKeyB, kModAlt));

    input.SetContextEnabled(kTestContext, false);
    EXPECT_FALSE(input.IsHostReservedKey(kKeyB, kModAlt));
}

TEST(InputSystemTests, KeyBindingQueryDoesNotClaimInputOrMutateActionState)
{
    InputSystem input{SinkRole::Gameplay};
    input.BindKey(kTestContext, kTestAction, {DeviceType::Keyboard, kKeyA, 1.0f});
    int callbacks = 0;
    auto listener = input.AddActionListener(kTestAction, [&](ActionEvent&) { ++callbacks; });
    ASSERT_TRUE(input.MatchesKeyBinding(kTestContext, kTestAction, kKeyA, 0));
    EXPECT_FALSE(input.GetActionState(kTestAction).pressed);
    EXPECT_FALSE(input.GetActionState(kTestAction).justPressed);
    EXPECT_EQ(callbacks, 0);
    input.RemoveAction(kTestContext, kTestAction);
    EXPECT_FALSE(input.OnKey(kKeyA, kKeyActionPress, 0)) << "query must not leave a raw polling claim";
    input.RemoveActionListener(listener);
}

TEST(InputSystemTests, MouseChordReleaseEdgeSurvivesTheModifierLeavingFirst)
{
    InputSystem input{SinkRole::Gameplay};
    ActionBinding ctrlRight{DeviceType::Mouse, kMouseButtonRight, 1.0f};
    ctrlRight.requiredMods = kModControl;
    input.BindKey(kMouseContext, kTestAction, ctrlRight);
    input.PushContext(kMouseContext);

    ASSERT_TRUE(input.OnMouseButton(kMouseButtonRight, /*down=*/true, kModControl));
    input.Update(0.016f);
    ASSERT_TRUE(input.WasActionTriggered(kTestAction)) << "precondition: the chord fired";

    // Ctrl leaves while the button is still held. The hold is judged live, so
    // the action stops reading as active now...
    input.OnKey(kKeyCode_LeftControl, kKeyActionRelease, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_FALSE(input.IsActionActive(kTestAction)) << "the hold is judged against the live modifiers";

    // ...but the press that was taken still owes its release edge.
    input.OnMouseButton(kMouseButtonRight, /*down=*/false, /*mods=*/0);
    input.Update(0.016f);
    EXPECT_TRUE(input.GetActionState(kTestAction).justReleased)
        << "a Ctrl+Click ended by letting go of Ctrl first must still release";
}

// --- Gamepad: the poll differenced into edges, and the edges claimed ---

namespace
{

constexpr int kFirstPad = 0;
constexpr int kSecondPad = 1;

constexpr ContextId kPadContext = HashInput("PadContext");
constexpr ActionId kPadAction = HashInput("PadAction");

int Code(GamepadButton button)
{
    return static_cast<int>(button);
}

// One frame with a pad connected in `slot`, holding `held` and resting its left
// stick at `leftX`.
GamepadFrame PadFrame(int slot, std::initializer_list<GamepadButton> held = {}, float leftX = 0.0f)
{
    GamepadFrame frame;
    frame.slots[slot].connected = true;
    frame.slots[slot].axes[static_cast<int>(GamepadAxis::LeftX)] = leftX;
    for (GamepadButton button : held)
        frame.slots[slot].buttons[static_cast<int>(button)] = true;
    return frame;
}

// Everything one Report call handed out, in the order it arrived: state and
// edges in one sequence, so a row can pin that a pad releases what it held
// before it reports itself gone.
struct TrackerLog
{
    struct Report
    {
        bool isEdge = false;
        int slot = 0;
        bool connected = false;
        float leftX = 0.0f;
        GamepadButton button = GamepadButton::A;
        bool down = false;
    };

    std::vector<Report> reports;

    GamepadEdgeTracker::StateHandler State()
    {
        return [this](int slot, const float* axes, int axisCount, bool connected)
        {
            Report report;
            report.slot = slot;
            report.connected = connected;
            report.leftX = (axes && axisCount > static_cast<int>(GamepadAxis::LeftX))
                               ? axes[static_cast<int>(GamepadAxis::LeftX)]
                               : 0.0f;
            reports.push_back(report);
        };
    }

    GamepadEdgeTracker::ButtonEdgeHandler Edge()
    {
        return [this](int slot, GamepadButton button, bool down)
        {
            Report report;
            report.isEdge = true;
            report.slot = slot;
            report.button = button;
            report.down = down;
            reports.push_back(report);
        };
    }

    std::vector<Report> Edges() const
    {
        std::vector<Report> edges;
        for (const Report& report : reports)
            if (report.isEdge)
                edges.push_back(report);
        return edges;
    }

    std::vector<Report> States() const
    {
        std::vector<Report> states;
        for (const Report& report : reports)
            if (!report.isEdge)
                states.push_back(report);
        return states;
    }

    void Clear() { reports.clear(); }
};

} // namespace

TEST(InputSystemTests, GamepadPressAndReleaseBecomeEdges)
{
    GamepadEdgeTracker tracker;
    TrackerLog log;

    tracker.Report(PadFrame(kFirstPad), log.State(), log.Edge());
    log.Clear();

    tracker.Report(PadFrame(kFirstPad, {GamepadButton::A}), log.State(), log.Edge());
    ASSERT_EQ(log.Edges().size(), 1u);
    EXPECT_EQ(log.Edges()[0].button, GamepadButton::A);
    EXPECT_TRUE(log.Edges()[0].down);

    log.Clear();
    tracker.Report(PadFrame(kFirstPad), log.State(), log.Edge());
    ASSERT_EQ(log.Edges().size(), 1u);
    EXPECT_EQ(log.Edges()[0].button, GamepadButton::A);
    EXPECT_FALSE(log.Edges()[0].down);
}

TEST(InputSystemTests, GamepadPollWithNothingChangedReportsNoEdges)
{
    GamepadEdgeTracker tracker;
    TrackerLog log;

    tracker.Report(PadFrame(kFirstPad), log.State(), log.Edge());
    tracker.Report(PadFrame(kFirstPad, {GamepadButton::A}), log.State(), log.Edge());
    log.Clear();

    // A held button is not an event. Re-reporting the same frame is what every
    // frame of a hold looks like, and a fresh press each time would fire the
    // game's action on every one of them.
    tracker.Report(PadFrame(kFirstPad, {GamepadButton::A}), log.State(), log.Edge());
    tracker.Report(PadFrame(kFirstPad, {GamepadButton::A}), log.State(), log.Edge());
    EXPECT_TRUE(log.Edges().empty());
    EXPECT_EQ(log.States().size(), 2u) << "axes are state, so they are reported every frame";
}

TEST(InputSystemTests, GamepadButtonHeldWhenThePadAppearsIsNoPress)
{
    GamepadEdgeTracker tracker;
    TrackerLog log;

    // The pad arrives with A already down. Nothing pressed it here, so nothing
    // may act on it; the hold becomes the baseline instead.
    tracker.Report(PadFrame(kFirstPad, {GamepadButton::A}), log.State(), log.Edge());
    EXPECT_TRUE(log.Edges().empty());
    ASSERT_EQ(log.States().size(), 1u);
    EXPECT_TRUE(log.States()[0].connected);

    // The release that ends it still differences, so the hold is not stranded.
    log.Clear();
    tracker.Report(PadFrame(kFirstPad), log.State(), log.Edge());
    ASSERT_EQ(log.Edges().size(), 1u);
    EXPECT_EQ(log.Edges()[0].button, GamepadButton::A);
    EXPECT_FALSE(log.Edges()[0].down);
}

TEST(InputSystemTests, GamepadLeavingReleasesEveryButtonItHeld)
{
    GamepadEdgeTracker tracker;
    TrackerLog log;

    tracker.Report(PadFrame(kFirstPad), log.State(), log.Edge());
    tracker.Report(PadFrame(kFirstPad, {GamepadButton::A, GamepadButton::B}), log.State(), log.Edge());
    log.Clear();

    // Unplugged mid-press: the releases have to be synthesized, or every stage
    // holding those buttons holds them forever.
    tracker.Report(GamepadFrame{}, log.State(), log.Edge());
    ASSERT_EQ(log.Edges().size(), 2u);
    EXPECT_FALSE(log.Edges()[0].down);
    EXPECT_FALSE(log.Edges()[1].down);
    ASSERT_EQ(log.States().size(), 1u);
    EXPECT_FALSE(log.States()[0].connected);
    ASSERT_EQ(log.reports.size(), 3u);
    EXPECT_TRUE(log.reports[0].isEdge);
    EXPECT_TRUE(log.reports[1].isEdge);
    EXPECT_FALSE(log.reports[2].isEdge) << "the releases land before the pad reports itself gone";

    // And once it is gone it keeps reporting nothing, rather than a release per frame.
    log.Clear();
    tracker.Report(GamepadFrame{}, log.State(), log.Edge());
    EXPECT_TRUE(log.reports.empty());
}

TEST(InputSystemTests, SlotIdentityIsTheSlotIndexNotTheDevice)
{
    GamepadEdgeTracker tracker;
    TrackerLog log;

    tracker.Report(PadFrame(kFirstPad, {GamepadButton::B}), log.State(), log.Edge());
    log.Clear();

    // A different pad on the same index, holding A instead. Nothing in the frame
    // says the device changed, so the pair differences as one pad would: B reads
    // released and A reads pressed, though no player did either.
    tracker.Report(PadFrame(kFirstPad, {GamepadButton::A}), log.State(), log.Edge());

    ASSERT_EQ(log.Edges().size(), 2u);
    EXPECT_EQ(log.Edges()[0].button, GamepadButton::A);
    EXPECT_TRUE(log.Edges()[0].down);
    EXPECT_EQ(log.Edges()[1].button, GamepadButton::B);
    EXPECT_FALSE(log.Edges()[1].down);
}

TEST(InputSystemTests, TwoPadsChangingInOneReportKeepTheirOwnSlots)
{
    GamepadEdgeTracker tracker;
    TrackerLog log;

    GamepadFrame before;
    before.slots[kFirstPad].connected = true;
    before.slots[kSecondPad].connected = true;
    before.slots[kSecondPad].buttons[static_cast<int>(GamepadButton::B)] = true;
    tracker.Report(before, log.State(), log.Edge());
    log.Clear();

    GamepadFrame after = before;
    after.slots[kFirstPad].buttons[static_cast<int>(GamepadButton::A)] = true;
    after.slots[kSecondPad].buttons[static_cast<int>(GamepadButton::B)] = false;
    tracker.Report(after, log.State(), log.Edge());

    // Each slot is differenced against its own baseline and reported after its
    // own state, so one report carrying two pads cannot cross their edges.
    ASSERT_EQ(log.reports.size(), 4u);
    EXPECT_FALSE(log.reports[0].isEdge);
    EXPECT_EQ(log.reports[0].slot, kFirstPad);
    EXPECT_TRUE(log.reports[1].isEdge);
    EXPECT_EQ(log.reports[1].slot, kFirstPad);
    EXPECT_EQ(log.reports[1].button, GamepadButton::A);
    EXPECT_TRUE(log.reports[1].down);
    EXPECT_FALSE(log.reports[2].isEdge);
    EXPECT_EQ(log.reports[2].slot, kSecondPad);
    EXPECT_TRUE(log.reports[3].isEdge);
    EXPECT_EQ(log.reports[3].slot, kSecondPad);
    EXPECT_EQ(log.reports[3].button, GamepadButton::B);
    EXPECT_FALSE(log.reports[3].down);
}

TEST(InputSystemTests, GamepadAxesAreStateAndAnEmptySlotIsSilent)
{
    GamepadEdgeTracker tracker;
    TrackerLog log;

    tracker.Report(PadFrame(kFirstPad, {}, /*leftX=*/0.5f), log.State(), log.Edge());
    ASSERT_EQ(log.States().size(), 1u) << "only the occupied slot reports";
    EXPECT_EQ(log.States()[0].slot, kFirstPad);
    EXPECT_FLOAT_EQ(log.States()[0].leftX, 0.5f);
    EXPECT_TRUE(log.Edges().empty()) << "a stick is not an edge";
}

TEST(InputSystemTests, ApplicationSinkTakesEveryGamepadPress)
{
    InputSystem input;
    const float axes[kGamepadAxisCount] = {};
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);

    // Nothing is downstream of an Application sink, so declining a press would
    // only destroy it.
    EXPECT_TRUE(input.IsGamepadConnected(kFirstPad));
    EXPECT_TRUE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
    EXPECT_TRUE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));
}

TEST(InputSystemTests, GameplaySinkDeclinesAGamepadPressNothingBindsOrPolls)
{
    InputSystem input{SinkRole::Gameplay};
    const float axes[kGamepadAxisCount] = {};
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);

    EXPECT_FALSE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
    // Delivery is consumption: a declined press is recorded nowhere, so the state
    // queries and the chain cannot tell different stories about who acted.
    EXPECT_FALSE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));
}

TEST(InputSystemTests, GameplaySinkClaimsAPolledGamepadButton)
{
    InputSystem input{SinkRole::Gameplay};
    const float axes[kGamepadAxisCount] = {};
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);

    // Reading the button is the declaration, and a read that returns false is
    // still a read.
    EXPECT_FALSE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));
    EXPECT_TRUE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
    EXPECT_FALSE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::Y), /*down=*/true));
}

TEST(InputSystemTests, GameplaySinkClaimsABoundGamepadButton)
{
    InputSystem input{SinkRole::Gameplay};
    input.BindKey(kPadContext, kPadAction, {DeviceType::Gamepad, Code(GamepadButton::A), 1.0f});
    input.PushContext(kPadContext);

    const float axes[kGamepadAxisCount] = {};
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);

    EXPECT_TRUE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
    EXPECT_FALSE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::B), /*down=*/true));

    input.Update(0.016f);
    EXPECT_TRUE(input.IsActionActive(kPadAction)) << "the button the action reads is the button it claimed";
}

TEST(InputSystemTests, GamepadBindingClaimsTheFirstPadOnly)
{
    InputSystem input{SinkRole::Gameplay};
    input.BindKey(kPadContext, kPadAction, {DeviceType::Gamepad, Code(GamepadButton::A), 1.0f});
    input.PushContext(kPadContext);

    const float axes[kGamepadAxisCount] = {};
    input.OnGamepadState(kSecondPad, axes, kGamepadAxisCount, /*connected=*/true);

    // A binding names a button, not a pad, and the action layer reads the first
    // one — so claiming a second pad's button would consume a press no action
    // can act on.
    EXPECT_FALSE(input.OnGamepadButton(kSecondPad, Code(GamepadButton::A), /*down=*/true));

    // Polling names the pad, so it claims exactly the one it reads.
    EXPECT_FALSE(input.IsGamepadButtonDown(kSecondPad, GamepadButton::B));
    EXPECT_TRUE(input.OnGamepadButton(kSecondPad, Code(GamepadButton::B), /*down=*/true));
}

TEST(InputSystemTests, GamepadPollClaimLapsesWhenPollingStops)
{
    InputSystem input{SinkRole::Gameplay};
    const float axes[kGamepadAxisCount] = {};
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);

    EXPECT_FALSE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));
    input.Update(0.016f);
    input.Update(0.016f);
    EXPECT_TRUE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
    input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/false);

    // One frame further with nothing reading it and the button belongs to
    // whatever is downstream again.
    input.Update(0.016f);
    EXPECT_FALSE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
}

TEST(InputSystemTests, SuspendedGameplaySinkClaimsNoGamepadButton)
{
    InputSystem input{SinkRole::Gameplay};
    input.BindKey(kPadContext, kPadAction, {DeviceType::Gamepad, Code(GamepadButton::A), 1.0f});
    input.PushContext(kPadContext);

    const float axes[kGamepadAxisCount] = {};
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);
    input.SetClaimsSuspended(true);

    // A paused game acts on no input, so the pad belongs to the editor until it
    // resumes.
    EXPECT_FALSE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));

    input.SetClaimsSuspended(false);
    EXPECT_TRUE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
}

TEST(InputSystemTests, GamepadReleaseIsNeverConsumedAndClearsOnlyWhatIsHeld)
{
    InputSystem input{SinkRole::Gameplay};
    const float axes[kGamepadAxisCount] = {};
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);

    // An unmatched release — the other half of a press this sink declined — is a
    // no-op rather than a phantom edge.
    EXPECT_FALSE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::B), /*down=*/false));

    EXPECT_FALSE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));
    ASSERT_TRUE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
    EXPECT_FALSE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/false))
        << "a release answers false so it reaches every stage";
    EXPECT_FALSE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));
}

TEST(InputSystemTests, GamepadLeavingClearsItsAxesAndReleasedButtons)
{
    InputSystem input;
    float axes[kGamepadAxisCount] = {};
    axes[static_cast<int>(GamepadAxis::LeftX)] = 0.75f;
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);
    ASSERT_TRUE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
    ASSERT_FLOAT_EQ(input.GetGamepadAxis(kFirstPad, GamepadAxis::LeftX), 0.75f);

    // The poll releases what the pad held before reporting it gone, which is what
    // leaves no button down behind it.
    input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/false);
    input.OnGamepadState(kFirstPad, nullptr, 0, /*connected=*/false);

    EXPECT_FALSE(input.IsGamepadConnected(kFirstPad));
    EXPECT_FLOAT_EQ(input.GetGamepadAxis(kFirstPad, GamepadAxis::LeftX), 0.0f);
    EXPECT_FALSE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));
}

TEST(InputSystemTests, GamepadStateWithFewerAxesZeroesTheRest)
{
    InputSystem input;
    float axes[kGamepadAxisCount] = {};
    axes[static_cast<int>(GamepadAxis::LeftX)] = 0.75f;
    axes[static_cast<int>(GamepadAxis::RightTrigger)] = 1.0f;
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);
    ASSERT_FLOAT_EQ(input.GetGamepadAxis(kFirstPad, GamepadAxis::RightTrigger), 1.0f);

    // A device without a mapping reports whatever axes it has, and a pad swapped
    // into the same slot can report fewer than the one before it. The axes it
    // does not report read as centred rather than as the last pad's values.
    input.OnGamepadState(kFirstPad, axes, /*axisCount=*/2, /*connected=*/true);
    EXPECT_FLOAT_EQ(input.GetGamepadAxis(kFirstPad, GamepadAxis::LeftX), 0.75f);
    EXPECT_FLOAT_EQ(input.GetGamepadAxis(kFirstPad, GamepadAxis::RightTrigger), 0.0f);
}

TEST(InputSystemTests, ClearRegistrationsDropsGamepadPollClaims)
{
    InputSystem input{SinkRole::Gameplay};
    const float axes[kGamepadAxisCount] = {};
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);
    EXPECT_FALSE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));

    input.ClearRegistrations();
    EXPECT_FALSE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));
}

TEST(InputSystemTests, ResetStateForgetsEveryPad)
{
    InputSystem input{SinkRole::Gameplay};
    float axes[kGamepadAxisCount] = {};
    axes[static_cast<int>(GamepadAxis::LeftX)] = 0.75f;
    input.OnGamepadState(kFirstPad, axes, kGamepadAxisCount, /*connected=*/true);
    input.OnGamepadState(kSecondPad, axes, kGamepadAxisCount, /*connected=*/true);
    EXPECT_FALSE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));
    ASSERT_TRUE(input.OnGamepadButton(kFirstPad, Code(GamepadButton::A), /*down=*/true));

    // The edges that end a play session are the edges that move the pad's chain,
    // so the release for A is delivered somewhere else. A sink left holding the
    // button, reporting a pad and a deflected stick nothing feeds, would hand
    // all three to whatever takes it next.
    input.ResetState();

    EXPECT_FALSE(input.IsGamepadButtonDown(kFirstPad, GamepadButton::A));
    EXPECT_FALSE(input.IsGamepadConnected(kFirstPad));
    EXPECT_FALSE(input.IsGamepadConnected(kSecondPad));
    EXPECT_FLOAT_EQ(input.GetGamepadAxis(kFirstPad, GamepadAxis::LeftX), 0.0f);
}
