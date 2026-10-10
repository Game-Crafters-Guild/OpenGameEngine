#include "Markups/MarkupKind.h"

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "Markups/MarkupEditorBridge.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Vector2.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "UI/UIElement.h"

#include <vector>

namespace GameEngine::Editor
{

MarkupKind ReadMarkupKind(const ECS::World& world, ECS::EntityHandle entity)
{
    if (world.GetComponent<Components::MarkupRegion>(entity))
        return MarkupKind::Region;
    if (const auto* volume = world.GetComponent<Components::MarkupVolume>(entity))
        return volume->Shape == Components::MarkupVolumeShape::Sphere ? MarkupKind::Sphere : MarkupKind::Box;
    if (world.GetComponent<Components::Markup>(entity) && world.GetComponent<Components::SplineComponent>(entity))
        return MarkupKind::Path;
    return MarkupKind::None;
}

const char* MarkupKindName(MarkupKind kind)
{
    switch (kind)
    {
    case MarkupKind::Box:
        return "Box";
    case MarkupKind::Sphere:
        return "Sphere";
    case MarkupKind::Region:
        return "Region";
    case MarkupKind::Path:
        return "Path";
    case MarkupKind::None:
        break;
    }
    return "";
}

std::unique_ptr<UIElement> BuildMarkupKindGlyph(MarkupKind kind)
{
    auto glyph = std::make_unique<UIElement>();
    glyph->AddClass("markup-kind-glyph");
    switch (kind)
    {
    case MarkupKind::Box:
        glyph->AddClass("markup-kind-box");
        break;
    case MarkupKind::Sphere:
        glyph->AddClass("markup-kind-sphere");
        break;
    case MarkupKind::Region:
        glyph->AddClass("markup-kind-region");
        break;
    case MarkupKind::Path:
        glyph->AddClass("markup-kind-path");
        break;
    case MarkupKind::None:
        break;
    }
    glyph->SetTooltip(MarkupKindName(kind));
    return glyph;
}

SplineOwnerClaim QueryMarkupSpline(const ECS::World& world, ECS::EntityHandle entity)
{
    if (!world.GetComponent<Components::Markup>(entity))
        return SplineOwnerClaim{};
    const MarkupEditorBridge* bridge = MarkupEditorBridge::TryGet();
    return SplineOwnerClaim{true, bridge && bridge->IsHidden(world, entity),
                            world.GetComponent<Components::MarkupRegion>(entity) ? "A region's outline is always closed"
                                                                                 : "A path stays open"};
}

RegionOutlineProblem ReadRegionOutlineProblem(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* spline = world.GetComponent<Components::SplineComponent>(entity);
    const SplineECS::SplineService* splines = SplineECS::SplineService::TryGet();
    if (!world.GetComponent<Components::MarkupRegion>(entity) || !spline || !splines)
        return RegionOutlineProblem::None;
    const Spline::SplineData* data =
        splines->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    if (!data || data->Points.size() < Spline::SplineData::kMinClosedPointCount)
        return RegionOutlineProblem::TooFewPoints;
    const std::size_t count = data->Points.size();
    std::vector<Mathematics::Vector2> knots;
    knots.reserve(count);
    for (const Spline::SplineControlPoint& point : data->Points)
        knots.emplace_back(point.Position.x, point.Position.z);
    for (std::size_t i = 0; i < count; ++i)
    {
        for (std::size_t j = i + 2; j < count; ++j)
        {
            if ((j + 1) % count == i)
                continue;
            if (Mathematics::SegmentsIntersect(knots[i], knots[(i + 1) % count], knots[j], knots[(j + 1) % count]))
                return RegionOutlineProblem::Crosses;
        }
    }
    return RegionOutlineProblem::None;
}

const char* RegionOutlineProblemBadge(RegionOutlineProblem problem)
{
    switch (problem)
    {
    case RegionOutlineProblem::TooFewPoints:
        return "outline needs three points";
    case RegionOutlineProblem::Crosses:
        return "crosses itself";
    case RegionOutlineProblem::None:
        break;
    }
    return "";
}

const char* RegionOutlineProblemFix(RegionOutlineProblem problem)
{
    switch (problem)
    {
    case RegionOutlineProblem::TooFewPoints:
        return "The outline needs three points to enclose an area. Add a point, or delete the mark-up.";
    case RegionOutlineProblem::Crosses:
        return "The outline crosses itself, so the region has no lid. Move a point so no two edges cross.";
    case RegionOutlineProblem::None:
        break;
    }
    return "";
}

} // namespace GameEngine::Editor
