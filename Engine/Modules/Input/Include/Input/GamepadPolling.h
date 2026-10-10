#pragma once

namespace GameEngine::Input
{

struct GamepadFrame;

// Read every gamepad's mapped state for this frame (slot 0 = first mapped
// controller). Slots with nothing plugged into them are reported disconnected.
void ReadGamepadFrame(GamepadFrame& frame);

} // namespace GameEngine::Input
