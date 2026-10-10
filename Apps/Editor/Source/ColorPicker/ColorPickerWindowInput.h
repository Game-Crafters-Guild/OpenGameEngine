#pragma once

#include "Core/WindowInputRouter.h"

#include <functional>

namespace GameEngine
{
class UIManager;

// What a colour picker dialog window supplies to its own routing.
struct ColorPickerWindowInputSources
{
    // The dialog's own UIManager. It is the only stage of this window's chain:
    // a picker dialog hosts no gameplay and no editor actions.
    std::function<UIManager*()> getUi;
    // True while the eyedropper's screen-wide grab owns the pointer.
    std::function<bool()> isEyedropperActive;
    // Ends that grab and restores the colour the picker held before it started.
    std::function<void()> cancelEyedropper;
    // True while a UI replay scenario is loaded.
    std::function<bool()> isUiReplayActive;
};

// The routing policy of a colour picker dialog window, separated from the window
// it is bound to so it can be exercised without one.
//
// The eyedropper samples the whole screen through global cursor monitors, and the
// click that commits a sample is taken by those monitors rather than by the
// dialog. While it is armed the pointer therefore belongs to it: moves and
// buttons reach no stage, so a click meant for a pixel somewhere on screen cannot
// also press the button under the cursor. Keys keep travelling, because Escape is
// how the grab is cancelled.
//
// A loaded UI replay is the other case where this window's chain stands down. A
// replay drives editor UIManagers directly with a deterministic stream, so every
// window answers null for its manager while a scenario is loaded and real OS
// input reaches none of them.
WindowInputRouterConfig MakeColorPickerWindowInputConfig(Platform::Window* window,
                                                         ColorPickerWindowInputSources sources);

} // namespace GameEngine
