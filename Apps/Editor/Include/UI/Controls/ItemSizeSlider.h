#pragma once

#include "Events/Event.h"
#include "UI/Controls/Slider.h"

#include <functional>

namespace GameEngine::EditorUI
{

// The toolbar slider that sets how big a view draws its items: the Hierarchy's rows, the Assets
// panel's rows and icons. The item resize gesture (UI/Interaction/ItemResizeGesture.h) over the
// slider does what it does over the view the slider sizes, and a plain wheel passes through
// unhandled. The UI manager still delivers the wheel to a disabled element, so a disabled
// slider, or one inside a disabled container, ignores the gesture itself. The tooltip names the
// gesture's current binding (Keyboard Shortcuts > View Navigation > Resize Items) and follows a
// rebind. The look comes from UI/controls/ItemSizeSlider/ItemSizeSlider.css, requested for this
// slider's own subtree, which also gives every item size slider the same width.
class ItemSizeSlider : public Slider
{
public:
    ItemSizeSlider();

    // Receives the gesture's scroll (negative on wheel up). The owner applies the same step
    // the gesture takes over its view, so the slider and the view never disagree.
    void SetOnResizeGesture(std::function<void(float scrollY)> callback)
    {
        m_OnResizeGesture = std::move(callback);
    }

private:
    std::function<void(float)> m_OnResizeGesture;
    EventSubscription m_ShortcutBindingsSaved;
};

} // namespace GameEngine::EditorUI
