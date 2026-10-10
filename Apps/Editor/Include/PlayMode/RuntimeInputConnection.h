#pragma once

namespace GameEngine::Input
{
class InputSystem;
}

namespace GameEngine::Editor
{

/// Tells the game's input system where the window feeding it stands, when game
/// input switches on (play starts with the Game View active, or the Game View
/// becomes the active tab). The window reports focus only when it changes, so
/// the sink is told now; the pointer is placed by the next move that reaches it.
void ConnectRuntimeInput(Input::InputSystem& runtimeInput, bool windowFocused);

/// Puts the game's input system into the state of one that hears nothing from
/// the editor window: called when game input switches off (play stops, or the
/// Game View stops being the active tab). Nothing is routed to the sink until it
/// switches back on, so anything it still held would outlive the input that set it.
void DisconnectRuntimeInput(Input::InputSystem& runtimeInput);

} // namespace GameEngine::Editor
