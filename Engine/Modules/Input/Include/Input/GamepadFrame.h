#pragma once

#include "Input/GamepadCodes.h"

namespace GameEngine
{
namespace Input
{

// One frame of mapped gamepad state for every slot the engine tracks.
//
// Axes and buttons are in the standard layout GamepadAxis/GamepadButton
// enumerate, so a frame reads the same whichever physical device produced it. A
// slot with nothing plugged into it reports connected == false and holds no
// state.
struct GamepadFrame
{
    struct Slot
    {
        bool connected = false;
        float axes[kGamepadAxisCount] = {};
        bool buttons[kGamepadButtonCount] = {};
    };

    Slot slots[kMaxGamepads] = {};
};

} // namespace Input
} // namespace GameEngine
