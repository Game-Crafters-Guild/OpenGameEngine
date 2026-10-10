#pragma once

#include "Components/Transform.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "Spline/SplineTypes.h"
#include "Types/Types.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// The path half of the agent's mark-up methods (MarkupRequests.cpp): a path mark-up is a Markup
// on an entity with an open SplineComponent and no other shape (a river, a road, the way to the
// guards' base). Each reader returns the refusal text, naming the fix, or nullopt.

// A path's knots: 2 to 256 points along the way.
inline constexpr std::size_t kMinPathPoints = 2;
inline constexpr std::size_t kMaxPathPoints = 256;

// The undo step's name for a volume turned into a path (ConvertMarkupShapeCommand).
inline constexpr const char* kConvertToPathLabel = "Convert Mark-up to Path";

// Whether `entity` is a path mark-up: Markup and a SplineComponent, no MarkupRegion.
bool IsMarkupPath(const ECS::World& world, ECS::EntityHandle entity);

// `points`: 2 to 256 [x, y, z] points in world meters, in order along the way, no two
// consecutive ones closer than a centimeter. The line drapes on the terrain beneath them, and
// markup_create stands each point on the terrain where there is one (StandPathOnGround); y counts
// only off the terrain.
std::optional<std::string> ReadPathPoints(const nlohmann::json& value, std::vector<Mathematics::Vector3>& out);

// Stands each of `points` on the terrain under it (MarkupTerrainHeightAt), so the knots Edit path
// offers are on the line the Scene View draws; a point off the terrain keeps its y.
void StandPathOnGround(ECS::World& world, std::span<Mathematics::Vector3> points);

// Where a new path through `points` stands: at its first point, unrotated and unscaled.
Components::Transform NewPathPlacement(std::span<const Mathematics::Vector3> points);
// Makes `entity`, placed by `worldMatrix`, a path: an open spline of `type` through `points`
// (world), entity-local, knot radius 0. The entity's WorldTransform is set to `worldMatrix`.
void AddPathParts(ECS::World& world, ECS::EntityHandle entity, const float32 (&worldMatrix)[16],
                  std::span<const Mathematics::Vector3> points, Spline::SplineType type);

// The path's knots in the world; empty without its spline.
std::vector<Mathematics::Vector3> PathWorldPoints(const ECS::World& world, ECS::EntityHandle path);

// markup_get's `shape` for a path: {shape: "path", points: [[x, y, z], ...], type, length}, the
// length along `drapedLine` (the display's ground samples), else along the knots in the world.
nlohmann::json PathShapeJson(const ECS::World& world, ECS::EntityHandle path,
                             std::span<const Mathematics::Vector3> drapedLine);

} // namespace GameEngine::Editor
