#include "Input/GamepadPolling.h"

#include "Input/GamepadCodes.h"
#include "Input/GamepadFrame.h"
#include "Platform/Gamepad.h"

#include <GLFW/glfw3.h>

#include <algorithm>

namespace GameEngine::Input
{
namespace
{

// GamepadAxis / GamepadButton enumerate the mapped layout the platform layer
// normalizes to, so its buffers are sized by this module's own counts.
static_assert(kGamepadAxisCount == Platform::kMappedGamepadAxisCount,
              "GamepadAxis must enumerate the platform's mapped axis layout");
static_assert(kGamepadButtonCount == Platform::kMappedGamepadButtonCount,
              "GamepadButton must enumerate the platform's mapped button layout");

void StoreSlot(GamepadFrame::Slot& slot,
               const float* axes,
               int axisCount,
               const unsigned char* buttons,
               int buttonCount)
{
    slot.connected = true;
    const int axisCopy = std::min(axisCount, kGamepadAxisCount);
    for (int i = 0; i < axisCopy; ++i)
        slot.axes[i] = axes[i];

    const int buttonCopy = std::min(buttonCount, kGamepadButtonCount);
    for (int i = 0; i < buttonCopy; ++i)
        slot.buttons[i] = buttons[i] != 0;
}

void ReadJoystickAsGamepad(int joystickId, GamepadFrame::Slot& slot)
{
    int axisCount = 0;
    const float* axes = glfwGetJoystickAxes(joystickId, &axisCount);
    if (!axes || axisCount < 2)
        return;

    float mappedAxes[kGamepadAxisCount] = {};
    for (int i = 0; i < kGamepadAxisCount && i < axisCount; ++i)
        mappedAxes[i] = axes[i];

    if (axisCount > static_cast<int>(GamepadAxis::LeftTrigger))
        mappedAxes[static_cast<int>(GamepadAxis::LeftTrigger)] =
            (mappedAxes[static_cast<int>(GamepadAxis::LeftTrigger)] + 1.0f) * 0.5f;
    if (axisCount > static_cast<int>(GamepadAxis::RightTrigger))
        mappedAxes[static_cast<int>(GamepadAxis::RightTrigger)] =
            (mappedAxes[static_cast<int>(GamepadAxis::RightTrigger)] + 1.0f) * 0.5f;

    int buttonCount = 0;
    const unsigned char* buttons = glfwGetJoystickButtons(joystickId, &buttonCount);
    const int buttonCopy = buttons && buttonCount > 0 ? std::min(buttonCount, kGamepadButtonCount) : 0;

    StoreSlot(slot, mappedAxes, kGamepadAxisCount, buttons, buttonCopy);
}

} // namespace

void ReadGamepadFrame(GamepadFrame& frame)
{
    frame = GamepadFrame{};

    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid)
    {
        if (!glfwJoystickPresent(jid))
            continue;

        const int slot = jid - GLFW_JOYSTICK_1;
        if (slot < 0 || slot >= kMaxGamepads)
            continue;

        float mappedAxes[kGamepadAxisCount] = {};
        unsigned char mappedButtons[kGamepadButtonCount] = {};
        int axisCount = 0;
        int buttonCount = 0;
        if (Platform::ReadMappedGamepadState(jid, mappedAxes, mappedButtons, axisCount, buttonCount))
        {
            StoreSlot(frame.slots[slot], mappedAxes, axisCount, mappedButtons, buttonCount);
            continue;
        }

        // No mapping: Steam Deck / desktop mode often exposes the pad as a raw
        // joystick without a gamepad mapping DB entry, and web has no mapping
        // API at all.
        ReadJoystickAsGamepad(jid, frame.slots[slot]);
    }
}

} // namespace GameEngine::Input
