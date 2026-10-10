#include "PlayMode/RuntimeInputConnection.h"

#include "Input/InputSystem.h"

namespace GameEngine::Editor
{

void ConnectRuntimeInput(Input::InputSystem& runtimeInput, bool windowFocused)
{
    runtimeInput.OnWindowFocus(windowFocused);
}

void DisconnectRuntimeInput(Input::InputSystem& runtimeInput)
{
    // No move, leave or focus change reaches the sink while it is switched off,
    // so the pointer and the focus would otherwise read as they were when it
    // switches back on, wherever both have gone since. Losing focus is also
    // what releases the held keys and buttons, whose releases will never be
    // routed here.
    runtimeInput.OnMouseLeave();
    runtimeInput.OnWindowFocus(false);
}

} // namespace GameEngine::Editor
