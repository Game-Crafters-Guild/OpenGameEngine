#include "ColorPicker/ColorPickerWindowInput.h"

#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"

#include <utility>

namespace GameEngine
{

WindowInputRouterConfig MakeColorPickerWindowInputConfig(Platform::Window* window,
                                                        ColorPickerWindowInputSources sources)
{
    WindowInputRouterConfig config{};
    config.window = window;
    config.getUi = [getUi = std::move(sources.getUi),
                    isUiReplayActive = std::move(sources.isUiReplayActive)]() -> UIManager*
    {
        if (isUiReplayActive && isUiReplayActive())
            return nullptr;
        return getUi ? getUi() : nullptr;
    };
    config.isPointerGrabbed = sources.isEyedropperActive;
    config.onKeyPre = [isEyedropperActive = std::move(sources.isEyedropperActive),
                       cancelEyedropper = std::move(sources.cancelEyedropper)](int key, int action, int) -> bool
    {
        if (key != Input::kKeyCode_Escape || action != Input::kKeyActionPress)
            return false;
        if (!isEyedropperActive || !isEyedropperActive())
            return false;
        if (cancelEyedropper)
            cancelEyedropper();
        // Escape spent on the grab is not also an Escape for the dialog's UI.
        return true;
    };
    return config;
}

} // namespace GameEngine
