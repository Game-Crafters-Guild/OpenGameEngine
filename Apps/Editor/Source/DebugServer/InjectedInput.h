#pragma once

namespace GameEngine
{
class UIManager;
struct WindowInputRouterConfig;
namespace Platform
{
class Window;
}

namespace Editor
{

// Synthesized pointer input for the debug server (click_element, move_pointer,
// drags, context menus).
//
// It exists so injected input takes the same path real input does. The platform
// callbacks enter WindowInputRouter, which fans one event out to three sinks:
// the window's UIManager, the editor InputSystem, and — while play mode is
// active with a Game View focused — the runtime input sink, via the play-pointer
// remap. Calling UIManager directly reaches only the first, which is why
// gameplay reading the mouse could not be driven by tooling at all.
//
// Coordinates are UI-logical pixels, the space every debug-server pointer method
// already speaks (UIElement::GetLayoutX/Y and the x/y request parameters). The
// router speaks window client pixels, so these convert on the way in and the
// router converts back for the UI feed, leaving UI hit-testing coordinates
// unchanged.
//
// `config` is the routing policy bound to this window. A config whose sinks were
// never bound (the color picker wires its own window callbacks) keeps the direct
// UIManager feed, because routing an unbound config would drop the event.

// Whether a UI-logical position can be injected at all. JSON numbers arrive as
// doubles, so a request carrying 1e300 is a perfectly good double that becomes
// infinity in the narrowing cast to float — and from here the router feeds UI
// hit-testing, the editor InputSystem and, in play mode, gameplay, none of
// which re-check. Handlers call this on the coordinates a request supplies so
// they can answer with an error naming the field; InjectMouseMove calls it
// again for the coordinates handlers compute themselves (element centres,
// interpolated drag steps).
bool IsInjectablePointerPosition(float logicalX, float logicalY);

void InjectMouseMove(const WindowInputRouterConfig& config, Platform::Window* window, UIManager* ui, float logicalX,
                     float logicalY);
void InjectMouseButton(const WindowInputRouterConfig& config, UIManager* ui, int button, bool pressed);

// The keyboard half, through the same router entry point a typed key takes, so
// an injected key exercises the application's key pre-hooks, a HUD over a running
// game, gameplay and the editor's own actions — the whole chain the tool exists
// to observe.
void InjectKey(const WindowInputRouterConfig& config, UIManager* ui, int key, int action, int mods);

// Returns whether a UI stage turned the character into an edit, so a caller can
// still report how much of the text landed.
bool InjectChar(const WindowInputRouterConfig& config, UIManager* ui, unsigned int codepoint);

} // namespace Editor
} // namespace GameEngine
