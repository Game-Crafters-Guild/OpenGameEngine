#pragma once

namespace GameEngine
{
class UIElement;

namespace UI
{

/// True when the manager's keyboard focus resolves to this element or one of
/// its descendants.
///
/// An ad-hoc key handler that acts on a chord must gate on this. UIManager
/// bubbles an unhandled key from the HOVERED element as well as the focused one
/// (UIManager::DispatchInputEvent), so a handler that tests only the chord also
/// runs while its element is merely under the pointer — and the e.Stop() that
/// keeps one keystroke from firing twice then swallows the application binding
/// on that keystroke instead.
bool FocusIsInside(const UIElement& element);

} // namespace UI
} // namespace GameEngine
