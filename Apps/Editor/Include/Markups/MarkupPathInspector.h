#pragma once

namespace GameEngine
{
struct InspectorContext;
} // namespace GameEngine

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// The Mark-up section's Path block, on a path mark-up only (Markup on an open spline, no other
// shape): its point count, its length along the ground (the line the Scene View draws), and Edit
// path, which selects it and enters the spline tool on its knots. It rebuilds when the path's
// spline changes under it.
void AddMarkupPathBlock(const InspectorContext& ctx, MarkupEditorBridge& bridge);

} // namespace GameEngine::Editor
