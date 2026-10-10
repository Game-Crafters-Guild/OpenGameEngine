#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::Editor
{

// Resolves a debug-server "elementId" request field to the UI-logical point a synthesized
// pointer aims at — the element's layout centre — for click_element, move_pointer,
// open_context_menu and the drag handlers.
//
// FindById is unbounded and visibility-blind, so it also returns elements that occupy no
// pixels: hidden ones (display:none zeroes the Yoga box) and hosts whose content is mounted
// elsewhere. Their box is 0x0, so the "centre" collapses onto the layout origin — for most of
// them the window corner. Injecting a pointer there is not a click on the requested element,
// it is a click on whatever occupies that corner, and the caller is told it succeeded. In the
// editor's default layout `Hierarchy`, `Inspector` and `SceneView` all collapse to the same
// point and all land on the dock tab bar: three different targets, one wrong destination,
// three successful-looking replies. An element whose box has collapsed on BOTH axes is
// refused here instead, because a real pointer could never have reached it either. A box flat
// on only one axis is kept and resolves to a point on that edge: a splitter is zero-wide and
// full-height, hit-testing accepts a point on its line, and elementId drags on splitters
// depend on exactly that coordinate.
//
// On success writes x/y and leaves `error` untouched. On refusal returns false and fills
// `error` with the RefuseRequest refusal the handler returns verbatim; the message names the
// element and states what to do about it.
bool ResolveElementPointerTarget(UIElement* root, const std::string& elementId, float& x, float& y,
                                 nlohmann::json& error);

} // namespace GameEngine::Editor
