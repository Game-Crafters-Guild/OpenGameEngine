#pragma once

namespace GameEngine::UI
{

/// The wheel gesture that resizes the items of a view: tree and list rows, grid
/// icons, and the size sliders that drive them. Wheel up (a negative ScrollY)
/// makes items bigger in every consumer. The host installs the matcher
/// that decides which modifiers form the gesture; with none installed it is the
/// platform's primary shortcut modifier (Ctrl, or Cmd on macOS). Main thread
/// only, like the UI event dispatch that reads it.
///
/// The matcher is process-wide by design, one gesture for every view: every
/// UIManager in the process reads it. The editor installs its Resize Items
/// binding, so a game UI view running in the editor's Play mode follows that
/// binding too, while a shipped game, which installs none, uses the default. A
/// tree or list view acts only when its host sets a resize callback; a grid view
/// resizes its own icons.
using ItemResizeGestureMatcher = bool (*)(int modifiers);

/// Null restores the primary-modifier default.
void SetItemResizeGestureMatcher(ItemResizeGestureMatcher matcher);
bool MatchesItemResizeGesture(int modifiers);

} // namespace GameEngine::UI
