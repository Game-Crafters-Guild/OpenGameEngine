#include "Platform/Gamepad.h"

#include <GLFW/glfw3.h>

#include <cstring>

namespace GameEngine
{
namespace Platform
{

static_assert(kMappedGamepadAxisCount == GLFW_GAMEPAD_AXIS_LAST + 1,
              "mapped axis layout must match GLFW's gamepad mapping");
static_assert(kMappedGamepadButtonCount == GLFW_GAMEPAD_BUTTON_LAST + 1,
              "mapped button layout must match GLFW's gamepad mapping");

bool ReadMappedGamepadState(int jid,
                            float* axes,
                            unsigned char* buttons,
                            int& axisCount,
                            int& buttonCount)
{
    GLFWgamepadstate state{};
    if (!glfwGetGamepadState(jid, &state))
        return false;

    std::memcpy(axes, state.axes, sizeof(state.axes));
    std::memcpy(buttons, state.buttons, sizeof(state.buttons));
    axisCount = kMappedGamepadAxisCount;
    buttonCount = kMappedGamepadButtonCount;
    return true;
}

} // namespace Platform
} // namespace GameEngine
