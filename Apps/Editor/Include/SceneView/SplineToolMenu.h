#pragma once

#include "InspectorRegistry.h"
#include "UI/Interaction/ContextMenuManipulator.h"

#include <vector>

namespace GameEngine::Editor
{

// The spline tool button's right-click menu: the spline editor settings (curve type,
// selection shape, control sizes, stroke behaviour, colors). Built on every show, since
// the curve type gates whole submenus and mesh snapping disables rows. The color rows
// open `openColorPicker` and are disabled without one.
std::vector<ContextMenuManipulator::Item> BuildSplineToolMenuItems(const OpenColorPickerWindowFn& openColorPicker);

} // namespace GameEngine::Editor
