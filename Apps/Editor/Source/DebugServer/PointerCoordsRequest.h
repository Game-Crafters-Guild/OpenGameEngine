#pragma once

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{

// Reads a UI-logical pointer coordinate pair out of a debug-server request — ("x","y")
// for click_element and move_pointer, ("startX","startY")/("endX","endY") for a drag.
//
// Every JSON number arrives as a double, so 1e300 survives parsing and becomes infinity
// in the narrowing cast to float; from there the router carries it into UI hit-testing,
// the editor InputSystem and — in play mode — gameplay. Injection re-checks the pair it
// is finally handed and drops it silently, which is the right answer for a coordinate a
// handler computed itself and the wrong one for a request field: rejected here, the
// value is still attributable to the field that carried it.
//
// On success writes x/y and leaves `error` untouched. On rejection returns false and
// fills `error` with the RefuseRequest refusal the handler returns verbatim; the message
// names both coordinate keys and echoes both RAW request values, so a caller sees the
// number it sent rather than the infinity it narrowed to.
bool ReadPointerCoords(const nlohmann::json& params, const char* xKey, const char* yKey, float& x, float& y,
                       nlohmann::json& error);

} // namespace GameEngine::Editor
