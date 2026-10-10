#pragma once

#include "Components/Markup/Markup.h"
#include "Components/Transform.h"
#include "ECS/ECS.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Spline/SplineTypes.h"
#include "Types/Types.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::MarkupECS
{
struct MarkupFootprint;
}

namespace GameEngine::Editor
{

// The region half of the agent's mark-up methods (MarkupRequests.cpp): reading a region's
// parameters, writing its outline and members, and the region parts of the answers. Each
// reader returns the refusal text, naming the fix, or nullopt.

// `outline`: 3 to 256 [x, z] (or [x, y, z], y ignored) points in world meters, in order
// around the area, no two edges crossing, enclosing an area.
std::optional<std::string> ReadRegionOutline(const nlohmann::json& value, std::vector<Mathematics::Vector2>& out);
// `type`: "linear" (the knots drawn exactly) or "smooth" (Catmull-Rom).
std::optional<std::string> ReadRegionType(const nlohmann::json& value, Spline::SplineType& out);
// A region's height range, in meters (MarkupRegion::ExtrudeHeight).
inline constexpr float32 kMinExtrudeHeight = 1.0f;
inline constexpr float32 kMaxExtrudeHeight = 100.0f;
// The undo step's name for a box turned into a region (ConvertMarkupShapeCommand).
inline constexpr const char* kConvertToRegionLabel = "Convert Mark-up to Region";

// `extrudeHeight`: meters, 1 to 100.
std::optional<std::string> ReadExtrudeHeight(const nlohmann::json& value, float32& out);
// `members`: [{entityId | tag, mode: "include" | "exclude"}], the mark-ups `region` includes or
// excludes (MarkupECS::CheckRegionMembers's rules; `region` may be invalid for a region not
// created yet).
std::optional<std::string> ReadRegionMembers(const ECS::World& world, ECS::EntityHandle region,
                                             const nlohmann::json& value,
                                             std::vector<Components::MarkupRegionMember>& out);
// `exclusions`: polygons, each an outline as ReadRegionOutline reads one.
std::optional<std::string> ReadExclusions(const nlohmann::json& value,
                                          std::vector<std::vector<Mathematics::Vector2>>& out);

// Where a new region over `knots` (world x, z) stands: at their label point
// (MarkupECS::RegionLabelPoint) at height `groundY` (the ground there, so the drape's conform rays,
// which start a fixed height above the outline, find the ground), unrotated and unscaled in the
// world, its local transform relative to a parent placed by `parentWorld` (the identity for a root).
struct RegionPlacement
{
    Components::Transform Local;
    Components::WorldTransform World;
};
RegionPlacement NewRegionPlacement(std::span<const Mathematics::Vector2> knots, const float32 (&parentWorld)[16],
                                   float32 groundY);
// Makes `entity`, placed by `transform` (local) and `worldMatrix`, a region: its closed spline
// of `type` holds `knots` (world x, z) entity-local, knot radius 0, and its MarkupRegion has
// `extrudeHeight` and no members. The entity's WorldTransform is set to `worldMatrix`, so the
// area reads right before the next transform pass.
void AddRegionParts(ECS::World& world, ECS::EntityHandle entity, const float32 (&worldMatrix)[16],
                    std::span<const Mathematics::Vector2> knots, Spline::SplineType type, float32 extrudeHeight);
// Replaces the region's knots with `knots` (world x, z) placed by its current placement, and
// its type when `type` is set; empty `knots` keeps the knots. The entity stays where it is.
void RewriteRegionOutline(ECS::World& world, ECS::EntityHandle region, std::span<const Mathematics::Vector2> knots,
                          std::optional<Spline::SplineType> type);
// Writes `members` as the region's member list (at most kMaxRegionMembers).
void SetRegionMembers(ECS::World& world, ECS::EntityHandle region,
                      std::span<const Components::MarkupRegionMember> members);

// What a box mark-up converts to: its outline from above (its footprint) as knots, and its height
// clamped to the region's range. None for anything but a box (a sphere has no corners to keep).
struct BoxRegionShape
{
    std::vector<Mathematics::Vector2> Knots;
    float32 ExtrudeHeight = 0.0f;
};
std::optional<BoxRegionShape> BoxAsRegion(const ECS::World& world, ECS::EntityHandle entity);

// The region's base label point and bounding radius on the ground plane, for a list row; none
// while its outline is not a ring.
struct RegionExtent
{
    Mathematics::Vector3 Center{}; // the label point at the entity's height
    float32 Radius = 0.0f;         // of the circle around Center holding the ring
};
std::optional<RegionExtent> ReadRegionExtent(const ECS::World& world, ECS::EntityHandle region);

// The matrix that places the entity in the world: its WorldTransform, or before the first
// transform pass its local Transform; the identity for an entity with neither.
Mathematics::Matrix4x4 MarkupPlacement(const ECS::World& world, ECS::EntityHandle entity);

// markup_get's `shape` for a region: {shape: "region", outline, knots, type, extrudeHeight,
// area, perimeter, bounds, members}; markup_get adds its groundRange, which reads the display.
nlohmann::json RegionShapeJson(const ECS::World& world, ECS::EntityHandle region);

} // namespace GameEngine::Editor
