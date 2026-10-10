// Gamepad mapping on web. Emscripten's JS GLFW implements the 3.3 header only
// partially: glfwGetGamepadState is declared but has no JS implementation, so
// the desktop TU would fail at wasm-ld. Browser pads are read through the raw
// joystick API instead, which the JS library does provide.

#include "Platform/Gamepad.h"

namespace GameEngine
{
namespace Platform
{

bool ReadMappedGamepadState(int, float*, unsigned char*, int& axisCount, int& buttonCount)
{
    axisCount = 0;
    buttonCount = 0;
    return false;
}

} // namespace Platform
} // namespace GameEngine
