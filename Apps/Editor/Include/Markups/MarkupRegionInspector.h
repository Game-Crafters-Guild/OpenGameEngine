#pragma once

#include "ECS/Entity.h"

namespace GameEngine
{
struct InspectorContext;
namespace ECS
{
class World;
}
} // namespace GameEngine

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class MarkupEditorBridge;
class UndoRedoService;

// The Mark-up section's Region block on a region mark-up, above the thread (nothing on another
// entity): the outline's problem (if any) with its fix, the height (previewed while dragged, one
// undo step on release), the outline's points, perimeter and outline area, Edit outline (the
// spline tool on the region's knots), and the member list: every other mark-up whose footprint
// overlaps the region, and every member, each with a member checkbox and an Include | Exclude
// choice, each change one undo step. Edits refuse in play mode.
void AddMarkupRegionBlock(const InspectorContext& ctx, MarkupEditorBridge& bridge);

// MarkupRegion's editor traits: no inspector section of its own (the Mark-up section holds the
// Region block), and the move gizmo at the entity's origin.
void RegisterMarkupRegionTraits();

// The volume section's Convert to region row, on a box mark-up only.
void AddConvertToRegionRow(const InspectorContext& ctx, MarkupEditorBridge& bridge);

// Turns the box mark-up `entity` into a region, one way, as one undo step
// (ConvertMarkupShapeCommand): its outline is the box's footprint from above, its height the
// box's, clamped to the region's range; its notes, status and thread stay. False, changing
// nothing, for anything but a box mark-up.
bool ConvertBoxMarkupToRegion(ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
                              EditorChangeNotifications* notifications);

} // namespace GameEngine::Editor
