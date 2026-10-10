// What the game's input system is told when the editor switches game input on,
// and what it holds after game input switches off (play stops, or the Game View
// stops being the active tab). The editor keeps one runtime input system across
// play sessions and routes nothing to it while game input is off, so whatever it
// still holds is read by the next session.
#include <gtest/gtest.h>

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Mathematics/Vector2.h"
#include "PlayMode/RuntimeInputConnection.h"

using namespace GameEngine;

// Stop play with the pointer at the Game View's edge in a focused window, move
// it away, start play again from the keyboard: the game must not read the
// pointer as still in a focused window, or it edge-scrolls toward a pointer
// that has left.
TEST(RuntimeInputConnectionTests, TheNextSessionDoesNotInheritThePointerOrTheFocus)
{
    Input::InputSystem runtimeInput{Input::SinkRole::Gameplay};
    runtimeInput.OnMouseMove({2.0f, 300.0f});
    runtimeInput.OnWindowFocus(true);
    ASSERT_TRUE(runtimeInput.IsPointerInWindow());
    ASSERT_TRUE(runtimeInput.IsWindowFocused());

    Editor::DisconnectRuntimeInput(runtimeInput);
    runtimeInput.Update(0.016f);

    EXPECT_FALSE(runtimeInput.IsPointerInWindow());
    EXPECT_FALSE(runtimeInput.IsWindowFocused());
}

TEST(RuntimeInputConnectionTests, HeldButtonsAreReleased)
{
    Input::InputSystem runtimeInput{Input::SinkRole::Application};
    runtimeInput.OnMouseMove({10.0f, 20.0f});
    runtimeInput.OnMouseButton(Input::kMouseButton_Left, /*down=*/true, /*mods=*/0);
    runtimeInput.Update(0.016f);
    ASSERT_TRUE(runtimeInput.IsMouseButtonDown(Input::kMouseButton_Left));

    Editor::DisconnectRuntimeInput(runtimeInput);
    runtimeInput.Update(0.016f);

    EXPECT_FALSE(runtimeInput.IsMouseButtonDown(Input::kMouseButton_Left));
    EXPECT_TRUE(runtimeInput.WasMouseButtonReleased(Input::kMouseButton_Left));
}

// The window reports focus only when it changes, and the sink heard nothing
// while it was off: switching on tells it where the window stands now.
TEST(RuntimeInputConnectionTests, ConnectingReportsTheWindowsFocus)
{
    Input::InputSystem runtimeInput{Input::SinkRole::Gameplay};
    Editor::ConnectRuntimeInput(runtimeInput, /*windowFocused=*/true);
    EXPECT_TRUE(runtimeInput.IsWindowFocused());

    Editor::DisconnectRuntimeInput(runtimeInput);
    Editor::ConnectRuntimeInput(runtimeInput, /*windowFocused=*/false);
    EXPECT_FALSE(runtimeInput.IsWindowFocused());
}
