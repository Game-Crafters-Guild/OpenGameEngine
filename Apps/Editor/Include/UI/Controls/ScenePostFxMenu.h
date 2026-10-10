#pragma once

#include "UI/Interaction/ContextMenuManipulator.h"

#include <functional>
#include <vector>

namespace GameEngine
{
class SceneViewController;
class SceneViewToolbar;

namespace Editor
{

// Item set for the scene view post-processing menu (right-click on the toolbar's
// post-process toggle), computed at every show: per-effect rows only exist while that
// effect sits on an enabled PostProcessVolume in the scene, so the set comes out of an ECS
// scan. Empty when `controller` is null — the manipulator shows nothing.
//
// `onAutoExposureToggled` runs after the auto-exposure toggle: it seeds Fixed EV100 as a
// side effect (AE-lock), and an open camera-settings popup must pick up the seeded value
// or its stale field would clobber the seed.
std::vector<ContextMenuManipulator::Item> BuildScenePostFxMenuItems(
    SceneViewController* controller, SceneViewToolbar* toolbar,
    std::function<void()> onAutoExposureToggled);

} // namespace Editor
} // namespace GameEngine
