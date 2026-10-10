#include "Markups/MarkupDrawList.h"

#include "Components/Markup/Markup.h"
#include "Components/Transform.h"
#include "Core/CpuProfiler.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Components/Spline/SplineComponent.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupRegionDisplay.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector4.h"
#include "Rendering/Common/Frustum.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace GameEngine::Editor
{

using Components::Markup;
using Components::MarkupRegion;
using Components::MarkupVolume;
using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

// What one collection reads per mark-up.
struct ItemSource
{
    ECS::World& World;
    const MarkupEditorBridge& Bridge;
    const MarkupHighlightState& Highlight;
    Vector3 CameraPos;
    const Mathematics::Vector4* FrustumPlanes;
    uint64 Frame;
    std::vector<MarkupDrawItem>& Items;
    // Read on the first region that draws (the bridge's, once per frame), then shared.
    const MarkupRegionFrameGround* RegionGround = nullptr;
};

void AddDrawItem(const ItemSource& source, ECS::EntityHandle entity, const Markup& markup, const MarkupVolume& shape,
                 const Components::WorldTransform& transform)
{
    if (source.Bridge.IsHidden(source.World, entity))
        return;
    const std::optional<MarkupWorldVolume> volume = MarkupWorldVolumeFromMatrix(shape.Shape, transform.matrix);
    if (!volume)
        return;
    MarkupDrawItem& item = source.Items.emplace_back();
    item.Entity = entity;
    item.Volume = *volume;
    item.Rgb = MarkupDisplayRgb(markup);
    item.DistanceSq = (volume->Center - source.CameraPos).LengthSquared();
    item.Selected = source.Highlight.IsSelected(entity);
    item.Hovered = source.Highlight.IsHovered(entity);
    item.CameraInside = MarkupVolumeContains(*volume, source.CameraPos);
    item.OutOfView = source.FrustumPlanes &&
                     !Rendering::TestSphereFrustum(volume->Center, volume->BoundingRadius, source.FrustumPlanes);
}

// A sphere holding everything a region's or a path's display can reach, from its knots alone
// (before its display is built): the knots' circle on the ground plane, and above and below it as
// far again plus the extrusion, which holds any ground the outline drapes over up to a 45-degree
// slope.
bool SplineShapeMayBeInView(const ItemSource& source, ECS::EntityHandle entity, float32 extrudeHeight,
                            const Components::WorldTransform& transform)
{
    if (!source.FrustumPlanes)
        return true;
    const auto* spline = source.World.GetComponent<Components::SplineComponent>(entity);
    const SplineECS::SplineService* splines = SplineECS::SplineService::TryGet();
    const Spline::SplineData* data =
        spline && splines
            ? splines->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration))
            : nullptr;
    if (!data || data->Points.empty())
        return false;
    const Mathematics::Matrix4x4 matrix = Mathematics::Matrix4x4::FromColumnMajor(transform.matrix);
    Vector3 min = matrix.TransformPoint(data->Points.front().Position);
    Vector3 max = min;
    for (const auto& point : data->Points)
    {
        const Vector3 world = matrix.TransformPoint(point.Position);
        min = Vector3(std::min(min.x, world.x), std::min(min.y, world.y), std::min(min.z, world.z));
        max = Vector3(std::max(max.x, world.x), std::max(max.y, world.y), std::max(max.z, world.z));
    }
    const Vector3 center = (min + max) * 0.5f;
    const float32 groundRadius = std::sqrt((max.x - min.x) * (max.x - min.x) + (max.z - min.z) * (max.z - min.z)) * 0.5f;
    const float32 radius = groundRadius * 2.0f + extrudeHeight;
    return Rendering::TestSphereFrustum(center, radius, source.FrustumPlanes);
}

// Whether `point` stands inside the region's prism: inside its area and below its highest top.
bool InsideRegion(const MarkupECS::MarkupRegionDisplayCache::Entry& display, const Vector3& point)
{
    const MarkupECS::MarkupRegionGround& ground = display.Ground;
    return point.y < ground.Top + display.ExtrudeHeight && ground.Contains(Vector2(point.x, point.z));
}

// A region (its walls and lid) or a path (`path`: its draped line), whose display the bridge's
// cache holds.
void AddSplineShapeItem(ItemSource& source, ECS::EntityHandle entity, const Markup& markup, float32 extrudeHeight,
                        bool path, const Components::WorldTransform& transform)
{
    if (source.Bridge.IsHidden(source.World, entity) ||
        !SplineShapeMayBeInView(source, entity, extrudeHeight, transform))
        return;
    if (!source.RegionGround)
        source.RegionGround = &source.Bridge.RegionFrameGroundOf(source.World, source.Frame);
    const MarkupECS::MarkupRegionDisplayCache::Entry* display =
        ResolveMarkupRegionDisplay(source.World, source.Bridge.RegionDisplaysOf(source.World), entity,
                                   *source.RegionGround, source.Frame);
    if (!display || display->Ground.Outline.empty())
        return;
    MarkupDrawItem& item = source.Items.emplace_back();
    item.Entity = entity;
    item.Region = display;
    item.Path = path;
    const MarkupECS::MarkupRegionGround& ground = display->Ground;
    const float32 halfHeight = display->ExtrudeHeight * 0.5f;
    item.Volume.Center = ground.Center + Vector3(0.0f, halfHeight, 0.0f);
    item.Volume.BoundingRadius = std::sqrt(ground.Radius * ground.Radius + halfHeight * halfHeight) + halfHeight;
    item.Volume.TopY = display->Mesh.Label.y;
    item.Rgb = MarkupDisplayRgb(markup);
    item.DistanceSq = (item.Volume.Center - source.CameraPos).LengthSquared();
    item.Selected = source.Highlight.IsSelected(entity);
    item.Hovered = source.Highlight.IsHovered(entity);
    item.CameraInside = InsideRegion(*display, source.CameraPos);
    item.OutOfView = source.FrustumPlanes && !Rendering::TestSphereFrustum(item.Volume.Center, item.Volume.BoundingRadius,
                                                                          source.FrustumPlanes);
}

// Marks the items that are an Exclude member of a region the viewer has not hidden; `excluded` is
// scratch, kept by the caller to reuse its allocation.
void MarkExcludeMembers(ECS::World& world, const MarkupEditorBridge& bridge, std::vector<MarkupDrawItem>& items,
                        std::vector<uint32>& excluded)
{
    excluded.clear();
    world.Query<ECS::Read<Markup>, ECS::Read<MarkupRegion>>().Each(
        [&world, &bridge, &excluded](ECS::EntityHandle region, const Markup&, const MarkupRegion& component) {
            if (bridge.IsHidden(world, region))
                return;
            for (uint32 i = 0; i < std::min(component.MemberCount, Components::kMaxRegionMembers); ++i)
            {
                if (component.Members[i].Mode == Components::MarkupMemberMode::Exclude)
                    excluded.push_back(component.Members[i].Entity.id);
            }
        });
    if (excluded.empty())
        return;
    std::sort(excluded.begin(), excluded.end());
    for (MarkupDrawItem& item : items)
        item.ExcludeMember = std::binary_search(excluded.begin(), excluded.end(), item.Entity.id);
}

// The body cap's order: selected and hovered mark-ups first, then the nearest; the ones that
// draw no body (the camera inside, out of view, or an Exclude member at rest) last.
bool BodiesFirst(const MarkupDrawItem& a, const MarkupDrawItem& b)
{
    if (a.DrawsBody() != b.DrawsBody())
        return a.DrawsBody();
    const bool aHighlighted = a.Selected || a.Hovered;
    if (aHighlighted != (b.Selected || b.Hovered))
        return aHighlighted;
    return a.DistanceSq < b.DistanceSq;
}

} // namespace

std::size_t CollectMarkupDrawItems(ECS::World& world, const MarkupEditorBridge& bridge,
                                   const MarkupHighlightState& highlight, const Vector3& cameraPos,
                                   const Mathematics::Vector4* frustumPlanes, uint64 frame,
                                   std::vector<MarkupDrawItem>& items, std::vector<uint32>& excludedScratch)
{
    GE_CPU_PROFILE_SCOPE("Markups.CollectDrawItems");
    items.clear();
    // Mark-ups are editor-only and edited in the edit world: play draws none.
    if (bridge.IsInPlayMode())
        return 0;
    ItemSource source{world, bridge, highlight, cameraPos, frustumPlanes, frame, items};
    world.Query<ECS::Read<Markup>, ECS::Read<MarkupVolume>, ECS::Read<Components::WorldTransform>>().Each(
        [&source](ECS::EntityHandle entity, const Markup& markup, const MarkupVolume& shape,
                  const Components::WorldTransform& transform) { AddDrawItem(source, entity, markup, shape, transform); });
    world.Query<ECS::Read<Markup>, ECS::Read<MarkupRegion>, ECS::Read<Components::WorldTransform>>().Each(
        [&source](ECS::EntityHandle entity, const Markup& markup, const MarkupRegion& region,
                  const Components::WorldTransform& transform) {
            AddSplineShapeItem(source, entity, markup, region.ExtrudeHeight, false, transform);
        });
    // A path: a mark-up on an open spline, with no other shape.
    world.Query<ECS::Read<Markup>, ECS::Read<Components::SplineComponent>, ECS::Read<Components::WorldTransform>>().Each(
        [&source](ECS::EntityHandle entity, const Markup& markup, const Components::SplineComponent&,
                  const Components::WorldTransform& transform) {
            if (!source.World.GetComponent<MarkupRegion>(entity) && !source.World.GetComponent<MarkupVolume>(entity))
                AddSplineShapeItem(source, entity, markup, 0.0f, true, transform);
        });
    bridge.RegionDisplaysOf(world).EvictUnseen(frame);
    MarkExcludeMembers(world, bridge, items, excludedScratch);

    const std::size_t bodies = std::min(items.size(), kMarkupBodyCap);
    if (bodies < items.size())
        std::nth_element(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(bodies), items.end(), BodiesFirst);
    return bodies;
}

} // namespace GameEngine::Editor
