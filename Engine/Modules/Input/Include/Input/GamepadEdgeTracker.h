#pragma once

#include "Input/GamepadCodes.h"
#include "Input/GamepadFrame.h"

#include <functional>

namespace GameEngine
{
namespace Input
{

// Turns the per-frame gamepad poll into the two things an input chain carries:
// connection and axes as continuous state, buttons as press and release edges
// against the previous poll.
//
// A pad that appears with buttons already held reports no presses for them. The
// hold is adopted as the baseline instead, so the release that ends it still
// differences into an edge and nothing acts on a press the player never made.
// A pad that disappears reports a release for every button it held, because a
// release that never arrives is what leaves a button stuck down.
//
// A slot is identified by its index and nothing else: the frame carries no
// device identity. One pad leaving and another arriving on the same index
// between two polls — a long frame, a debugger pause — therefore read as one
// pad whose buttons changed, and whatever the newcomer holds differences into
// presses. Telling the two apart needs a per-device id from the platform read,
// which the web backend has no API for.
class GamepadEdgeTracker
{
  public:
    /// Continuous state for one slot: whether a pad is there and where its axes
    /// stand. Reported every frame a pad is connected, and once when one leaves.
    /// @param gamepadIndex Slot index (0 is the first pad).
    /// @param axes         Mapped axes in GamepadAxis order; null when the slot is empty.
    /// @param axisCount    Number of entries in `axes`.
    /// @param connected    Whether a pad occupies the slot.
    using StateHandler = std::function<void(int gamepadIndex, const float* axes, int axisCount, bool connected)>;

    /// One button transition for one slot: true for a press, false for a release.
    using ButtonEdgeHandler = std::function<void(int gamepadIndex, GamepadButton button, bool down)>;

    /// Reports what changed between the previous frame and `frame`, then keeps
    /// `frame` as the baseline for the next call. State for a slot is reported
    /// before that slot's edges, so a stage learns a pad arrived before it is
    /// told what its buttons did; a slot that leaves reports its releases first
    /// and its disconnection after, so the releases still land on a stage that
    /// believes the pad is there.
    void Report(const GamepadFrame& frame, const StateHandler& onState, const ButtonEdgeHandler& onButtonEdge);

  private:
    GamepadFrame m_Previous;
};

} // namespace Input
} // namespace GameEngine
