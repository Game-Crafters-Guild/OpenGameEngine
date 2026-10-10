#include "Markups/MarkupPathInspector.h"

#include "Components/Spline/SplineComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorRegistry.h"
#include "MarkupECS/MarkupRegionMesh.h"
#include "Markups/MarkupInspectorRows.h"
#include "Markups/MarkupKind.h"
#include "Markups/MarkupRegionDisplay.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "UI/Controls/Label.h"

#include <memory>
#include <string>

namespace GameEngine::Editor
{

namespace
{

// A path's knot count and its length along the ground.
void AddPathRows(const InspectorContext& ctx, const MarkupEditorBridge& bridge)
{
    const auto* spline = ctx.World->GetComponent<Components::SplineComponent>(ctx.Entity);
    const SplineECS::SplineService* splines = SplineECS::SplineService::TryGet();
    const Spline::SplineData* data =
        spline && splines
            ? splines->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration))
            : nullptr;
    AddValueRow(ctx.Parent, "Points", std::to_string(data ? data->Points.size() : 0u),
                "The path's knots; Edit path moves, adds and deletes them");
    const MarkupECS::MarkupRegionDisplayCache::Entry* display = ResolveMarkupDisplayNow(*ctx.World, bridge, ctx.Entity);
    AddValueRow(ctx.Parent, "Length", FormatMeters(display ? MarkupECS::PolylineLength(display->Ground.Outline) : 0.0f),
                "The path's length along the ground, as the Scene View draws it");
}

} // namespace

void AddMarkupPathBlock(const InspectorContext& ctx, MarkupEditorBridge& bridge)
{
    if (!ctx.Parent || !ctx.World || ReadMarkupKind(*ctx.World, ctx.Entity) != MarkupKind::Path)
        return;
    auto heading = std::make_unique<Label>();
    heading->AddClass("markup-path-heading");
    heading->SetText("Path");
    ctx.Parent->AddChild(std::move(heading));
    AddPathRows(ctx, bridge);
    AddEditKnotsRow(ctx, bridge, "Edit path", "markup-path-edit",
                    "Drag, add and delete the path's points in the Scene View");
    FollowMarkupShapeChanges(ctx, {});
}

} // namespace GameEngine::Editor
