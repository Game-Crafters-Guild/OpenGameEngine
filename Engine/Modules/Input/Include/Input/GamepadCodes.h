#pragma once

// Standard gamepad layout (matches GLFW 3.3+ GLFW_GAMEPAD_* constants).
// Steam Deck / Xbox / Switch Pro via SDL present this mapping through glfwGetGamepadState.

namespace GameEngine::Input
{

enum class GamepadAxis : int
{
    LeftX = 0,
    LeftY = 1,
    RightX = 2,
    RightY = 3,
    LeftTrigger = 4,
    RightTrigger = 5,
};

enum class GamepadButton : int
{
    A = 0,
    B = 1,
    X = 2,
    Y = 3,
    LeftBumper = 4,
    RightBumper = 5,
    Back = 6,
    Start = 7,
    Guide = 8,
    LeftThumb = 9,
    RightThumb = 10,
    DpadUp = 11,
    DpadRight = 12,
    DpadDown = 13,
    DpadLeft = 14,
};

constexpr int kGamepadAxisCount = 6;
constexpr int kGamepadButtonCount = 15;
constexpr int kMaxGamepads = 4;

} // namespace GameEngine::Input
