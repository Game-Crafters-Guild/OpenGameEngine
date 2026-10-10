#pragma once

#include <functional>

namespace GameEngine {

class UIElement;

namespace EditorUI {

/**
 * Gives a plain element the activation gesture every control in the engine
 * uses: a left press arms it and takes the pointer capture, a release inside
 * activates, and a press that is cancelled or ends outside activates nothing.
 * The element is also made focusable, so Enter and Space activate it as Button
 * does.
 *
 * The gesture itself is `PointerManipulator` — arming, capture, the `pressed`
 * visual across the target's edge, cancel handling and detach cleanup all come
 * from there rather than being re-derived here. This adds only the left-button
 * filter and the keyboard path.
 *
 * It exists for elements that cannot be a Button or a Foldout: a 22px row
 * cannot be a Button, because Button.css sizes `button, .button` at a 32px
 * min-height. Not being a Button should cost the layout, never the behaviour.
 *
 * ONE DIVERGENCE FROM BUTTON: an UNMODIFIED left press activates. Shift, Ctrl,
 * Alt or Cmd held over the press activates nothing, where Button activates on
 * any left press because it tests the button index alone. The exact-modifier
 * match is `ManipulatorActivationFilter`'s rule, and it is the useful one:
 * whoever wants Shift+click to mean something else on such an element declares
 * a second filter for it and the two cannot overlap. Nothing declares one
 * today, so a modified press bubbles to an ancestor that wants it.
 */
void ActivateOnRelease(UIElement& element, std::function<void()> activate);

} // namespace EditorUI
} // namespace GameEngine
