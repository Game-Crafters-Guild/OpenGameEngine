#pragma once

#include "UI/UIManager.h"
#include "Platform/Window.h"

namespace GameEngine
{

/// Sets up a cursor callback on the UIManager that changes the OS cursor
/// based on the hovered UI element's CSS cursor property.
void SetupUICursorCallback(UIManager* ui, Platform::Window* window);

} // namespace GameEngine
