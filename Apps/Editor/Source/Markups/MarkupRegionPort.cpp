#include "Markups/MarkupRegionPort.h"

#include "Components/Name.h"
#include "Components/SceneEntityTag.h"
#include "Components/Spline/SplineComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "MarkupECS/MarkupRegionArea.h"
#include "MarkupECS/MarkupRegionBoolean.h"
#include "MarkupECS/MarkupRegionOutline.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/MarkupPresentation.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Quaternion.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace GameEngine::Editor
{

namespace
{

using Components::MarkupMemberMode;
using Components::MarkupRegion;
using Components::MarkupRegionMember;
using Mathematics::Vector2;
using Mathematics::Vector3;
using nlohmann::json;

json XZJson(const Vector2& point)
{
    return json::array({point.x, point.y});
}

json RingJson(std::span<const Vector2> ring)
{
    json out = json::array();
    for (const Vector2& point : ring)
        out.push_back(XZJson(point));
    return out;
}

Spline::SplineData* SplineDataOf(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* spline = world.GetComponent<Components::SplineComponent>(entity);
    auto* service = SplineECS::SplineService::TryGet();
    if (!spline || !service)
        return nullptr;
    return service->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
}

void WriteKnots(Spline::SplineData& data, const Mathematics::Matrix4x4& worldToLocal, std::span<const Vector2> knots)
{
    if (knots.empty())
    {
        data.MarkDirty();
        Spline::RebuildSplineCache(data);
        return;
    }
    data.Points.clear();
    for (const Vector2& knot : knots)
    {
        const Vector3 local = worldToLocal.TransformPoint(Vector3(knot.x, 0.0f, knot.y));
        data.AddPoint(Vector3(local.x, 0.0f, local.z), 0.0f);
    }
    data.MarkDirty();
    Spline::RebuildSplineCache(data);
}

const char* TypeName(Spline::SplineType type)
{
    switch (type)
    {
    case Spline::SplineType::Linear: return "linear";
    case Spline::SplineType::CatmullRom: return "smooth";
    case Spline::SplineType::CubicBezier: return "bezier";
    }
    return "linear";
}

// The mark-up whose scene id is `tag`; invalid when none has it.
ECS::EntityHandle FindMarkupByTag(const ECS::World& world, const std::string& tag)
{
    for (const ECS::EntityHandle entity : MarkupECS::MarkupService::Get().GetMarkups(world))
    {
        const auto* entityTag = world.GetComponent<Components::SceneEntityTag>(entity);
        if (entityTag && entityTag->View() == tag)
            return entity;
    }
    return {};
}

std::optional<std::string> ReadMember(const ECS::World& world, const json& value, MarkupRegionMember& out)
{
    if (!value.is_object())
        return std::string("Each member is {entityId or tag, mode: \"include\" or \"exclude\"}");
    if (const auto id = value.find("entityId"); id != value.end())
    {
        if (!id->is_number_unsigned() || id->get<uint64>() > std::numeric_limits<uint32>::max())
            return std::string("A member's entityId is one markup_list returned");
        out.Entity = ECS::EntityHandle(id->get<uint32>());
    }
    else if (const auto tag = value.find("tag"); tag != value.end() && tag->is_string())
    {
        out.Entity = FindMarkupByTag(world, tag->get<std::string>());
        if (!out.Entity.IsValid())
            return "No mark-up has the tag \"" + tag->get<std::string>() + "\"; markup_list returns each one's tag";
    }
    else
    {
        return std::string("Each member names its mark-up by entityId or tag");
    }
    const auto mode = value.find("mode");
    if (mode == value.end() || (*mode != "include" && *mode != "exclude"))
        return std::string("A member's mode must be \"include\" or \"exclude\"");
    out.Mode = *mode == "exclude" ? MarkupMemberMode::Exclude : MarkupMemberMode::Include;
    return std::nullopt;
}

json FootprintJson(const MarkupECS::MarkupFootprint& footprint)
{
    if (footprint.IsCircle())
        return json{{"center", XZJson(footprint.Center)}, {"radius", footprint.Radius}};
    return json{{"outline", RingJson(footprint.Ring)}};
}

json MemberJson(const ECS::World& world, const MarkupECS::MarkupRegionArea::Member& member)
{
    json out{{"entityId", member.Entity.id}, {"mode", member.Mode == MarkupMemberMode::Exclude ? "exclude" : "include"}};
    if (!member.Footprint)
    {
        // Deleted (undo revives it), or no longer a mark-up with a shape: skipped by the area.
        out["deleted"] = true;
        return out;
    }
    const auto* name = world.GetComponent<Components::Name>(member.Entity);
    const auto* tag = world.GetComponent<Components::SceneEntityTag>(member.Entity);
    out["deleted"] = false;
    out["title"] = name ? std::string(name->View()) : std::string();
    out["tag"] = tag ? std::string(tag->View()) : std::string();
    out["kind"] = world.GetComponent<MarkupRegion>(member.Entity) ? "region" : "volume";
    out["footprint"] = FootprintJson(*member.Footprint);
    return out;
}

// The area and the perimeter of `area` with its members applied (CombineRegionArea, the area
// markup_contains tests and the display draws): the pieces' outer rings less their holes, and every
// ring's length.
std::pair<float32, float32> AreaAndPerimeterWithMembers(const MarkupECS::MarkupRegionArea& area)
{
    std::vector<MarkupECS::MarkupFootprint> includes;
    std::vector<MarkupECS::MarkupFootprint> excludes;
    for (const MarkupECS::MarkupRegionArea::Member& member : area.GetMembers())
    {
        if (member.Footprint)
            (member.Mode == MarkupMemberMode::Exclude ? excludes : includes).push_back(*member.Footprint);
    }
    float32 squareMeters = 0.0f;
    float32 meters = 0.0f;
    for (const MarkupECS::MarkupAreaPiece& piece : MarkupECS::CombineRegionArea(area.GetBase().Ring, includes, excludes))
    {
        squareMeters += MarkupECS::RegionOutlineArea(piece.Outer);
        meters += MarkupECS::RegionOutlinePerimeter(piece.Outer);
        for (const std::vector<Vector2>& hole : piece.Holes)
        {
            squareMeters -= MarkupECS::RegionOutlineArea(hole);
            meters += MarkupECS::RegionOutlinePerimeter(hole);
        }
    }
    return {squareMeters, meters};
}

} // namespace

std::optional<std::string> ReadRegionOutline(const json& value, std::vector<Vector2>& out)
{
    out.clear();
    if (!value.is_array())
        return std::string("outline must be an array of [x, z] points in meters, in order around the area");
    for (const json& point : value)
    {
        if (!point.is_array() || (point.size() != 2 && point.size() != 3) ||
            !std::all_of(point.begin(), point.end(), [](const json& part) { return part.is_number(); }))
            return std::string("Each outline point is [x, z] (or [x, y, z], y ignored) in meters");
        out.emplace_back(point[0].get<float32>(), point[point.size() - 1].get<float32>());
    }
    return MarkupECS::CheckRegionOutline(out);
}

std::optional<std::string> ReadRegionType(const json& value, Spline::SplineType& out)
{
    if (value == "linear")
        out = Spline::SplineType::Linear;
    else if (value == "smooth")
        out = Spline::SplineType::CatmullRom;
    else
        return std::string("type must be \"linear\" or \"smooth\"");
    return std::nullopt;
}

std::optional<std::string> ReadExtrudeHeight(const json& value, float32& out)
{
    if (!value.is_number() || !(value.get<float32>() >= kMinExtrudeHeight && value.get<float32>() <= kMaxExtrudeHeight))
        return std::string("extrudeHeight must be meters from 1 to 100");
    out = value.get<float32>();
    return std::nullopt;
}

std::optional<std::string> ReadRegionMembers(const ECS::World& world, ECS::EntityHandle region, const json& value,
                                             std::vector<MarkupRegionMember>& out)
{
    out.clear();
    if (!value.is_array())
        return std::string("members must be an array of {entityId or tag, mode}");
    for (const json& entry : value)
    {
        MarkupRegionMember member{};
        if (auto refusal = ReadMember(world, entry, member))
            return refusal;
        out.push_back(member);
    }
    return MarkupECS::CheckRegionMembers(world, region, out);
}

std::optional<std::string> ReadExclusions(const json& value, std::vector<std::vector<Vector2>>& out)
{
    out.clear();
    if (!value.is_array())
        return std::string("exclusions must be an array of outlines, each [[x, z], ...]");
    for (const json& polygon : value)
    {
        std::vector<Vector2> outline;
        if (auto refusal = ReadRegionOutline(polygon, outline))
            return "Exclusion " + std::to_string(out.size()) + ": " + *refusal;
        out.push_back(std::move(outline));
    }
    return std::nullopt;
}

RegionPlacement NewRegionPlacement(std::span<const Vector2> knots, const float32 (&parentWorld)[16],
                                   float32 groundY)
{
    const Vector2 label = MarkupECS::RegionLabelPoint(knots);
    const Components::Transform world = Components::Transform::FromTRS(
        Vector3(label.x, groundY, label.y), Mathematics::Quaternion::Identity(), Vector3(1.0f, 1.0f, 1.0f));
    const Mathematics::Matrix4x4 local = Mathematics::Inverse(Mathematics::Matrix4x4::FromColumnMajor(parentWorld)) *
                                         Mathematics::Matrix4x4::FromColumnMajor(world.matrix);
    RegionPlacement placement;
    std::copy_n(local.Data(), 16, placement.Local.matrix);
    std::copy(std::begin(world.matrix), std::end(world.matrix), std::begin(placement.World.matrix));
    return placement;
}

std::optional<BoxRegionShape> BoxAsRegion(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* volume = world.GetComponent<Components::MarkupVolume>(entity);
    if (!volume || volume->Shape != Components::MarkupVolumeShape::Box)
        return std::nullopt;
    const std::optional<MarkupECS::MarkupFootprint> footprint = MarkupECS::ReadMarkupFootprint(world, entity);
    const std::optional<MarkupWorldVolume> placed = ReadMarkupWorldVolume(world, entity);
    if (!footprint || !placed)
        return std::nullopt;
    return BoxRegionShape{footprint->Ring,
                          std::clamp(2.0f * placed->HalfExtents.y, kMinExtrudeHeight, kMaxExtrudeHeight)};
}

Mathematics::Matrix4x4 MarkupPlacement(const ECS::World& world, ECS::EntityHandle entity)
{
    if (const auto* placed = world.GetComponent<Components::WorldTransform>(entity))
        return Mathematics::Matrix4x4::FromColumnMajor(placed->matrix);
    if (const auto* local = world.GetComponent<Components::Transform>(entity))
        return Mathematics::Matrix4x4::FromColumnMajor(local->matrix);
    return Mathematics::Matrix4x4::Identity();
}

void AddRegionParts(ECS::World& world, ECS::EntityHandle entity, const float32 (&worldMatrix)[16],
                    std::span<const Vector2> knots, Spline::SplineType type, float32 extrudeHeight)
{
    SplineECS::SplineService& splines = SplineECS::SplineService::Get();
    const SplineECS::SplineHandle handle = splines.CreateSpline(type, true);
    WriteKnots(*splines.GetSplineData(handle),
               Mathematics::Inverse(Mathematics::Matrix4x4::FromColumnMajor(worldMatrix)), knots);
    Components::SplineComponent spline{};
    spline.SplineDataIndex = handle.Index();
    spline.SplineDataGeneration = handle.Generation();
    spline.DefaultRadius = 0.0f; // a region has no swept band
    Components::WorldTransform placed{};
    std::copy(std::begin(worldMatrix), std::end(worldMatrix), std::begin(placed.matrix));
    MarkupRegion region{};
    region.ExtrudeHeight = extrudeHeight;
    world.AddComponentImmediate(entity, placed);
    world.AddComponentImmediate(entity, spline);
    world.AddComponentImmediate(entity, region);
}

void RewriteRegionOutline(ECS::World& world, ECS::EntityHandle region, std::span<const Vector2> knots,
                          std::optional<Spline::SplineType> type)
{
    Spline::SplineData* data = SplineDataOf(world, region);
    if (!data)
        return;
    if (type)
        data->Type = *type;
    WriteKnots(*data, Mathematics::Inverse(MarkupPlacement(world, region)), knots);
}

void SetRegionMembers(ECS::World& world, ECS::EntityHandle region, std::span<const MarkupRegionMember> members)
{
    MarkupRegion updated = *world.GetComponent<MarkupRegion>(region);
    updated.MemberCount = static_cast<uint32>(std::min<std::size_t>(members.size(), Components::kMaxRegionMembers));
    std::fill(std::begin(updated.Members), std::end(updated.Members), MarkupRegionMember{});
    std::copy_n(members.begin(), updated.MemberCount, updated.Members);
    world.AddComponentImmediate(region, updated);
}

std::optional<RegionExtent> ReadRegionExtent(const ECS::World& world, ECS::EntityHandle region)
{
    const std::optional<MarkupECS::MarkupFootprint> base = MarkupECS::ReadMarkupFootprint(world, region);
    if (!base)
        return std::nullopt;
    const Vector2 label = MarkupECS::RegionLabelPoint(base->Ring);
    RegionExtent extent;
    extent.Center = Vector3(label.x, MarkupPlacement(world, region).TransformPoint(Vector3(0.0f, 0.0f, 0.0f)).y, label.y);
    for (const Vector2& point : base->Ring)
        extent.Radius = std::max(extent.Radius, std::hypot(point.x - label.x, point.y - label.y));
    return extent;
}

json RegionShapeJson(const ECS::World& world, ECS::EntityHandle region)
{
    const auto* component = world.GetComponent<MarkupRegion>(region);
    const Spline::SplineData* data = SplineDataOf(world, region);
    json shape{{"shape", "region"}, {"extrudeHeight", component ? component->ExtrudeHeight : 0.0f}};
    json knots = json::array();
    if (data)
    {
        const Mathematics::Matrix4x4 placement = MarkupPlacement(world, region);
        for (const Spline::SplineControlPoint& point : data->Points)
        {
            const Vector3 placed = placement.TransformPoint(point.Position);
            knots.push_back(json::array({placed.x, placed.z}));
        }
        shape["type"] = TypeName(data->Type);
    }
    shape["knots"] = std::move(knots);

    const std::optional<MarkupECS::MarkupRegionArea> area = MarkupECS::MarkupRegionArea::Read(world, region);
    if (!area)
    {
        // Fewer than three knots: no ring, so no area until the outline is fixed.
        shape["outline"] = json::array();
        shape["members"] = json::array();
        return shape;
    }
    const MarkupECS::MarkupFootprint& base = area->GetBase();
    // The ring MarkupRegionArea tests: the knots of a linear outline, else the decimated ring.
    shape["outline"] = RingJson(base.Ring);
    const auto [squareMeters, meters] = AreaAndPerimeterWithMembers(*area);
    shape["area"] = squareMeters;
    shape["perimeter"] = meters;
    shape["bounds"] = json{{"min", XZJson(base.Min)}, {"max", XZJson(base.Max)}};
    json members = json::array();
    for (const MarkupECS::MarkupRegionArea::Member& member : area->GetMembers())
        members.push_back(MemberJson(world, member));
    shape["members"] = std::move(members);
    return shape;
}

} // namespace GameEngine::Editor
