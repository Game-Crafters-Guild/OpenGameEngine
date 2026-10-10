#pragma once

#include "ECS/Entity.h"
#include "SceneView/SplineOwnerQuery.h"

#include <memory>

namespace GameEngine
{
class UIElement;
namespace ECS
{
class World;
}
} // namespace GameEngine

namespace GameEngine::Editor
{

// What shape a mark-up has: the kind glyph the Mark-ups panel rows and the region's member list
// show, by class (MarkupsPanel.css .markup-kind-glyph).
enum class MarkupKind
{
    None, // no shape component (not a mark-up, or one being made)
    Box,
    Sphere,
    Region,
    Path, // a mark-up on an open spline: a river, a road, a way
};

MarkupKind ReadMarkupKind(const ECS::World& world, ECS::EntityHandle entity);
// "Box", "Sphere", "Region", "Path"; empty for None.
const char* MarkupKindName(MarkupKind kind);
// The kind's glyph: a 14 px square with the kind's class and its name as the tooltip.
std::unique_ptr<UIElement> BuildMarkupKindGlyph(MarkupKind kind);

// The mark-ups' claim on a spline (SetSplineOwnerQuery): a mark-up's spline is its shape (a
// region's outline, a path's line), left to the mark-up by the spline tools, its Closed row
// explained in the mark-up's words. The installed bridge answers whether it is hidden.
SplineOwnerClaim QueryMarkupSpline(const ECS::World& world, ECS::EntityHandle entity);

// What is wrong with a region's outline, from its knots: fewer than three (it draws as a line
// until a point is added) or two edges crossing (its lid is dropped until they no longer cross).
enum class RegionOutlineProblem
{
    None,
    TooFewPoints,
    Crosses,
};

// None for an entity that is not a region.
RegionOutlineProblem ReadRegionOutlineProblem(const ECS::World& world, ECS::EntityHandle entity);
// The badge text a problem shows on the panel row ("outline needs three points", "crosses
// itself"); empty for None.
const char* RegionOutlineProblemBadge(RegionOutlineProblem problem);
// The inspector's notice for a problem, naming the fix; empty for None.
const char* RegionOutlineProblemFix(RegionOutlineProblem problem);

} // namespace GameEngine::Editor
