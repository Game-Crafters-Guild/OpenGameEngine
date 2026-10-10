#pragma once

#include "UI/Interaction/ContextMenuManipulator.h"

#include <functional>
#include <vector>

namespace GameEngine
{
namespace Editor
{
class MovieRecorderController;

// Item set for the toolbar record button's right-click menu: resolution/frame-rate
// presets and fade options, read from and written to the recorder settings store through
// the controller. State hooks resolve at every show; the controller is app-owned and
// outlives any open menu.
std::vector<ContextMenuManipulator::Item> MovieRecorderMenuItems(
    MovieRecorderController& controller, std::function<void()> openRecordingSettings);

} // namespace Editor
} // namespace GameEngine
