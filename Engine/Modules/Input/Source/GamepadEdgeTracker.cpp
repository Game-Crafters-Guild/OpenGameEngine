#include "Input/GamepadEdgeTracker.h"

namespace GameEngine
{
namespace Input
{

void GamepadEdgeTracker::Report(const GamepadFrame& frame,
                                const StateHandler& onState,
                                const ButtonEdgeHandler& onButtonEdge)
{
    for (int slot = 0; slot < kMaxGamepads; ++slot)
    {
        const GamepadFrame::Slot& current = frame.slots[slot];
        const GamepadFrame::Slot& previous = m_Previous.slots[slot];

        if (!current.connected)
        {
            if (previous.connected)
            {
                if (onButtonEdge)
                {
                    for (int button = 0; button < kGamepadButtonCount; ++button)
                    {
                        if (previous.buttons[button])
                            onButtonEdge(slot, static_cast<GamepadButton>(button), false);
                    }
                }
                if (onState)
                    onState(slot, nullptr, 0, false);
            }
            continue;
        }

        if (onState)
            onState(slot, current.axes, kGamepadAxisCount, true);

        // A pad that was not there a frame ago contributes no presses: whatever
        // it arrives holding becomes the baseline this frame's copy already is.
        if (previous.connected && onButtonEdge)
        {
            for (int button = 0; button < kGamepadButtonCount; ++button)
            {
                if (current.buttons[button] != previous.buttons[button])
                    onButtonEdge(slot, static_cast<GamepadButton>(button), current.buttons[button]);
            }
        }
    }

    m_Previous = frame;
}

} // namespace Input
} // namespace GameEngine
