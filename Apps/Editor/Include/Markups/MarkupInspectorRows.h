#pragma once

#include "Types/Types.h"

#include <functional>
#include <string>

namespace GameEngine
{
struct InspectorContext;
class UIElement;
} // namespace GameEngine

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// The rows the Mark-up section's shape blocks share (the Region block, the Path block).

// Meters to a tenth: "12.5 m".
std::string FormatMeters(float32 meters);

// A read-only row: its label and a value text.
void AddValueRow(UIElement* parent, const char* labelText, const std::string& value, const char* tooltip);

// Edit outline (a region's) or Edit path: a button that selects the mark-up and puts the main
// Scene View's spline tool on its knots. Disabled in play mode.
void AddEditKnotsRow(const InspectorContext& ctx, MarkupEditorBridge& bridge, const char* text, const char* cssClass,
                     const char* tooltip);

// A block's rows are built once: a knot edit in the Scene View, an agent's update or an undo
// changes the mark-up's shape (its spline and its version, its region's members, the other
// mark-ups the scene holds) under them, so the block asks the inspector for one rebuild when that
// changed. `eachFrame` runs first every frame (the Height row follows the height there without a
// rebuild). One compare per frame.
void FollowMarkupShapeChanges(const InspectorContext& ctx, std::function<void()> eachFrame);

} // namespace GameEngine::Editor
