#include "MarkupECS/MarkupRegionArea.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "MarkupECS/MarkupRegionOutline.h"
#include "Mathematics/Geometry.h"
#include "SplineECS/SplineService.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::MarkupECS
{

using Components::MarkupRegionMember;
using Mathematics::Vector2;

namespace
{

// The ground-plane position of the entity-local point (x, y, z) under the column-major `matrix`.
Vector2 PlaceXZ(const float32 (&matrix)[16], float32 x, float32 y, float32 z)
{
    return Vector2(matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
                   matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14]);
}

void FitBounds(MarkupFootprint& footprint)
{
    if (footprint.IsCircle())
    {
        footprint.Min = Vector2(footprint.Center.x - footprint.Radius, footprint.Center.y - footprint.Radius);
        footprint.Max = Vector2(footprint.Center.x + footprint.Radius, footprint.Center.y + footprint.Radius);
        return;
    }
    footprint.Min = footprint.Ring.front();
    footprint.Max = footprint.Ring.front();
    for (const Vector2& point : footprint.Ring)
    {
        footprint.Min = Vector2(std::min(footprint.Min.x, point.x), std::min(footprint.Min.y, point.y));
        footprint.Max = Vector2(std::max(footprint.Max.x, point.x), std::max(footprint.Max.y, point.y));
    }
}

std::optional<MarkupFootprint> RegionFootprint(const ECS::World& world, ECS::EntityHandle entity,
                                               const float32 (&matrix)[16])
{
    const auto* spline = world.GetComponent<Components::SplineComponent>(entity);
    const SplineECS::SplineService* service = SplineECS::SplineService::TryGet();
    if (!spline || !service)
        return std::nullopt;
    const Spline::SplineData* data =
        service->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    if (!data || !data->IsEffectivelyClosed())
        return std::nullopt;
    MarkupFootprint footprint;
    // A linear outline is its knots, exactly; a curved one is its evaluated ring (16 samples per
    // segment) decimated as the agent reads it, so markup_get's outline is the ring tested.
    if (data->Type == Spline::SplineType::Linear)
    {
        footprint.Ring.reserve(data->Points.size());
        for (const Spline::SplineControlPoint& knot : data->Points)
            footprint.Ring.push_back(PlaceXZ(matrix, knot.Position.x, knot.Position.y, knot.Position.z));
        return footprint;
    }
    if (data->ClosedPolygonXZ.size() < 3)
        return std::nullopt;
    std::vector<Vector2> placed;
    placed.reserve(data->ClosedPolygonXZ.size());
    for (const Vector2& point : data->ClosedPolygonXZ)
        placed.push_back(PlaceXZ(matrix, point.x, 0.0f, point.y));
    footprint.Ring = DecimateRegionRing(placed);
    return footprint;
}

// A box's scale is its full extents: the unit cube's corners at +-0.5.
MarkupFootprint BoxFootprint(const float32 (&matrix)[16])
{
    Vector2 corners[8];
    for (int corner = 0; corner < 8; ++corner)
        corners[corner] = PlaceXZ(matrix, (corner & 1) ? 0.5f : -0.5f, (corner & 2) ? 0.5f : -0.5f,
                                  (corner & 4) ? 0.5f : -0.5f);
    MarkupFootprint footprint;
    footprint.Ring = Mathematics::ConvexHull(corners);
    return footprint;
}

// A sphere's scale.x is its diameter.
MarkupFootprint SphereFootprint(const float32 (&matrix)[16])
{
    MarkupFootprint footprint;
    footprint.Center = Vector2(matrix[12], matrix[14]);
    footprint.Radius = 0.5f * std::sqrt(matrix[0] * matrix[0] + matrix[1] * matrix[1] + matrix[2] * matrix[2]);
    return footprint;
}

bool IsMarkupWithShape(const ECS::World& world, ECS::EntityHandle entity)
{
    return world.IsValid(entity) && world.GetComponent<Components::Markup>(entity) &&
           (world.GetComponent<Components::MarkupVolume>(entity) ||
            (world.GetComponent<Components::MarkupRegion>(entity) &&
             world.GetComponent<Components::SplineComponent>(entity)));
}

} // namespace

bool MarkupFootprint::Contains(const Vector2& point) const
{
    if (point.x < Min.x || point.x > Max.x || point.y < Min.y || point.y > Max.y)
        return false;
    if (IsCircle())
    {
        const float32 dx = point.x - Center.x;
        const float32 dy = point.y - Center.y;
        return dx * dx + dy * dy <= Radius * Radius;
    }
    return Mathematics::PointInPolygon(point, Ring);
}

std::optional<MarkupFootprint> ReadMarkupFootprint(const ECS::World& world, ECS::EntityHandle entity)
{
    if (!IsMarkupWithShape(world, entity))
        return std::nullopt;
    const auto* transform = world.GetComponent<Components::WorldTransform>(entity);
    if (!transform)
        return std::nullopt;
    std::optional<MarkupFootprint> footprint;
    if (world.GetComponent<Components::MarkupRegion>(entity))
        footprint = RegionFootprint(world, entity, transform->matrix);
    else if (world.GetComponent<Components::MarkupVolume>(entity)->Shape == Components::MarkupVolumeShape::Sphere)
        footprint = SphereFootprint(transform->matrix);
    else
        footprint = BoxFootprint(transform->matrix);
    // A zero scale collapses the shape: it covers nothing.
    if (!footprint || (footprint->IsCircle() ? footprint->Radius <= 0.0f : footprint->Ring.size() < 3))
        return std::nullopt;
    FitBounds(*footprint);
    return footprint;
}

MarkupFootprintKey ReadMarkupFootprintKey(const ECS::World& world, ECS::EntityHandle entity)
{
    MarkupFootprintKey key;
    const auto* transform = world.GetComponent<Components::WorldTransform>(entity);
    if (!IsMarkupWithShape(world, entity) || !transform)
        return key;
    std::memcpy(key.Matrix, transform->matrix, sizeof(key.Matrix));
    if (world.GetComponent<Components::MarkupRegion>(entity))
    {
        key.Shape = MarkupFootprintKey::Kind::Region;
        const auto* spline = world.GetComponent<Components::SplineComponent>(entity);
        const SplineECS::SplineService* service = SplineECS::SplineService::TryGet();
        const Spline::SplineData* data =
            service ? service->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration))
                    : nullptr;
        key.SplineIndex = spline->SplineDataIndex;
        key.SplineGeneration = spline->SplineDataGeneration;
        key.SplineVersion = data ? data->Version : 0;
        return key;
    }
    key.Shape = world.GetComponent<Components::MarkupVolume>(entity)->Shape == Components::MarkupVolumeShape::Sphere
                    ? MarkupFootprintKey::Kind::Sphere
                    : MarkupFootprintKey::Kind::Box;
    return key;
}

std::optional<MarkupRegionArea> MarkupRegionArea::Read(const ECS::World& world, ECS::EntityHandle region)
{
    const auto* component = world.GetComponent<Components::MarkupRegion>(region);
    if (!component)
        return std::nullopt;
    std::optional<MarkupFootprint> base = ReadMarkupFootprint(world, region);
    if (!base)
        return std::nullopt;
    MarkupRegionArea area;
    area.m_Base = std::move(*base);
    const uint32 count = std::min(component->MemberCount, Components::kMaxRegionMembers);
    area.m_Members.reserve(count);
    for (uint32 i = 0; i < count; ++i)
    {
        const MarkupRegionMember& member = component->Members[i];
        Member& read = area.m_Members.emplace_back(Member{member.Entity, member.Mode, std::nullopt});
        // A region listing itself is refused on write; one that does anyway adds nothing.
        if (member.Entity != region)
            read.Footprint = ReadMarkupFootprint(world, member.Entity);
    }
    return area;
}

bool MarkupRegionArea::Contains(const Vector2& point) const
{
    bool inside = m_Base.Contains(point);
    for (const Member& member : m_Members)
    {
        if (!member.Footprint)
            continue;
        if (member.Mode == Components::MarkupMemberMode::Exclude)
        {
            if (member.Footprint->Contains(point))
                return false;
        }
        else if (!inside)
        {
            inside = member.Footprint->Contains(point);
        }
    }
    return inside;
}

std::optional<std::string> CheckRegionMembers(const ECS::World& world, ECS::EntityHandle region,
                                              std::span<const MarkupRegionMember> members)
{
    if (members.size() > Components::kMaxRegionMembers)
        return "A region holds at most " + std::to_string(Components::kMaxRegionMembers) + " members; it was given " +
               std::to_string(members.size());
    for (std::size_t i = 0; i < members.size(); ++i)
    {
        const ECS::EntityHandle entity = members[i].Entity;
        if (entity == region)
            return std::string("A region cannot list itself as a member");
        if (!IsMarkupWithShape(world, entity))
            return "Member " + std::to_string(entity.id) +
                   " is not a mark-up with a shape; members are boxes, spheres and regions from markup_list";
        for (std::size_t j = 0; j < i; ++j)
        {
            if (members[j].Entity == entity)
                return "Member " + std::to_string(entity.id) + " is listed twice; list each mark-up once";
        }
    }
    return std::nullopt;
}

} // namespace GameEngine::MarkupECS
