#include "Markups/MarkupPathPort.h"

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "MarkupECS/MarkupRegionMesh.h"
#include "Markups/MarkupRegionDisplay.h"
#include "Markups/MarkupRegionPort.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Quaternion.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace GameEngine::Editor
{

using Mathematics::Vector3;
using nlohmann::json;

namespace
{

// Consecutive points closer than this are one point.
constexpr float32 kMinPathSpacing = 0.01f;

const Spline::SplineData* PathSplineData(const ECS::World& world, ECS::EntityHandle path)
{
    const auto* spline = world.GetComponent<Components::SplineComponent>(path);
    const SplineECS::SplineService* service = SplineECS::SplineService::TryGet();
    if (!spline || !service)
        return nullptr;
    return service->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
}

// The type as the port names it: "smooth" for the Catmull-Rom curve markup_create's smooth makes;
// a type the spline inspector set otherwise reads as it.
const char* PathTypeName(Spline::SplineType type)
{
    switch (type)
    {
    case Spline::SplineType::Linear:
        return "linear";
    case Spline::SplineType::CatmullRom:
        return "smooth";
    case Spline::SplineType::CubicBezier:
        return "bezier";
    }
    return "linear";
}

} // namespace

bool IsMarkupPath(const ECS::World& world, ECS::EntityHandle entity)
{
    return world.GetComponent<Components::Markup>(entity) &&
           world.GetComponent<Components::SplineComponent>(entity) &&
           !world.GetComponent<Components::MarkupRegion>(entity);
}

std::optional<std::string> ReadPathPoints(const json& value, std::vector<Vector3>& out)
{
    out.clear();
    if (!value.is_array() || value.size() < kMinPathPoints || value.size() > kMaxPathPoints)
        return std::string("points must be 2 to 256 [x, y, z] points in meters, in order along the way");
    for (const json& point : value)
    {
        if (!point.is_array() || point.size() != 3 ||
            !std::all_of(point.begin(), point.end(), [](const json& part) { return part.is_number(); }))
            return std::string("Each point is [x, y, z] in meters");
        const Vector3 read(point[0].get<float32>(), point[1].get<float32>(), point[2].get<float32>());
        if (!std::isfinite(read.x) || !std::isfinite(read.y) || !std::isfinite(read.z))
            return std::string("The points must be finite numbers");
        if (!out.empty() && (read - out.back()).Length() < kMinPathSpacing)
            return "Points " + std::to_string(out.size() - 1) + " and " + std::to_string(out.size()) +
                   " are the same place; drop one";
        out.push_back(read);
    }
    return std::nullopt;
}

void StandPathOnGround(ECS::World& world, std::span<Vector3> points)
{
    for (Vector3& point : points)
    {
        if (const std::optional<float32> ground = MarkupTerrainHeightAt(world, Mathematics::Vector2(point.x, point.z)))
            point.y = *ground;
    }
}

Components::Transform NewPathPlacement(std::span<const Vector3> points)
{
    return Components::Transform::FromTRS(points.front(), Mathematics::Quaternion::Identity(),
                                          Vector3(1.0f, 1.0f, 1.0f));
}

void AddPathParts(ECS::World& world, ECS::EntityHandle entity, const float32 (&worldMatrix)[16],
                  std::span<const Vector3> points, Spline::SplineType type)
{
    SplineECS::SplineService& splines = SplineECS::SplineService::Get();
    const SplineECS::SplineHandle handle = splines.CreateSpline(type, false);
    Spline::SplineData& data = *splines.GetSplineData(handle);
    const Mathematics::Matrix4x4 worldToLocal = Mathematics::Inverse(Mathematics::Matrix4x4::FromColumnMajor(worldMatrix));
    for (const Vector3& point : points)
        data.AddPoint(worldToLocal.TransformPoint(point), 0.0f);
    data.MarkDirty();
    Spline::RebuildSplineCache(data);
    Components::SplineComponent spline{};
    spline.SplineDataIndex = handle.Index();
    spline.SplineDataGeneration = handle.Generation();
    spline.DefaultRadius = 0.0f; // a path mark-up has no swept band
    Components::WorldTransform placed{};
    std::copy(std::begin(worldMatrix), std::end(worldMatrix), std::begin(placed.matrix));
    world.AddComponentImmediate(entity, placed);
    world.AddComponentImmediate(entity, spline);
}

std::vector<Vector3> PathWorldPoints(const ECS::World& world, ECS::EntityHandle path)
{
    std::vector<Vector3> points;
    const Spline::SplineData* data = PathSplineData(world, path);
    if (!data)
        return points;
    const Mathematics::Matrix4x4 placement = MarkupPlacement(world, path);
    points.reserve(data->Points.size());
    for (const Spline::SplineControlPoint& point : data->Points)
        points.push_back(placement.TransformPoint(point.Position));
    return points;
}

json PathShapeJson(const ECS::World& world, ECS::EntityHandle path, std::span<const Vector3> drapedLine)
{
    json points = json::array();
    const std::vector<Vector3> knots = PathWorldPoints(world, path);
    for (const Vector3& point : knots)
        points.push_back(json::array({point.x, point.y, point.z}));
    const Spline::SplineData* data = PathSplineData(world, path);
    return json{{"shape", "path"},
                {"points", std::move(points)},
                {"type", data ? PathTypeName(data->Type) : "linear"},
                {"length", MarkupECS::PolylineLength(drapedLine.empty() ? std::span<const Vector3>(knots) : drapedLine)}};
}

} // namespace GameEngine::Editor
