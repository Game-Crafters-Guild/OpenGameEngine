#include "Markups/MarkupInspectorRows.h"

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/MarkupEditorBridge.h"
#include "SceneViewController.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace GameEngine::Editor
{

namespace
{

// What a shape block shows depends on: the mark-up's spline and its version, its region's members,
// and the other mark-ups (the scene's mark-up revision moves with every mark-up made or changed).
struct ShapeSignature
{
    uint64 MarkupRevision = 0;
    uint32 SplineIndex = 0;
    uint32 SplineGeneration = 0;
    uint64 SplineVersion = 0;
    Components::MarkupRegion Region{};

    bool operator==(const ShapeSignature& other) const
    {
        return MarkupRevision == other.MarkupRevision && SplineIndex == other.SplineIndex &&
               SplineGeneration == other.SplineGeneration && SplineVersion == other.SplineVersion &&
               std::memcmp(&Region, &other.Region, sizeof(Components::MarkupRegion)) == 0;
    }
};

ShapeSignature ReadShapeSignature(const ECS::World& world, ECS::EntityHandle entity)
{
    ShapeSignature signature;
    if (const MarkupECS::MarkupService* service = MarkupECS::MarkupService::TryGet())
        signature.MarkupRevision = service->GetRevision(world);
    if (const auto* region = world.GetComponent<Components::MarkupRegion>(entity))
    {
        signature.Region = *region;
        // The height has its own slider, which follows it without a rebuild (a rebuild mid-drag
        // would take the slider from under the pointer).
        signature.Region.ExtrudeHeight = 0.0f;
    }
    const auto* spline = world.GetComponent<Components::SplineComponent>(entity);
    const SplineECS::SplineService* splines = SplineECS::SplineService::TryGet();
    if (!spline || !splines)
        return signature;
    signature.SplineIndex = spline->SplineDataIndex;
    signature.SplineGeneration = spline->SplineDataGeneration;
    if (const Spline::SplineData* data =
            splines->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration)))
        signature.SplineVersion = data->Version;
    return signature;
}

} // namespace

std::string FormatMeters(float32 meters)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f m", meters);
    return text;
}

void AddValueRow(UIElement* parent, const char* labelText, const std::string& value, const char* tooltip)
{
    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, labelText, tooltip);
    auto text = std::make_unique<Label>();
    text->AddClass("markup-region-value");
    text->SetText(value);
    InspectorUI::AddFieldContainer(row)->AddChild(std::move(text));
}

void AddEditKnotsRow(const InspectorContext& ctx, MarkupEditorBridge& bridge, const char* text, const char* cssClass,
                     const char* tooltip)
{
    UIElement* cell = InspectorUI::AddActionRow(ctx.Parent, "markup-region-actions");
    auto button = std::make_unique<Button>();
    button->SetText(text);
    button->AddClass("small");
    button->AddClass(cssClass);
    button->SetTooltip(tooltip);
    button->RegisterEventHandler(kEventButtonClick, [ctx, &bridge](UIEvent&) {
        if (bridge.IsInPlayMode())
            return;
        bridge.SelectMarkup(*ctx.World, ctx.Entity, false);
        if (SceneViewController* view = bridge.GetSceneView())
            view->SetActiveRegisteredTool("spline");
    });
    Button* raw = button.get();
    cell->AddChild(std::move(button));
    if (bridge.IsInPlayMode())
        InspectorUI::SetRowOfControlEnabled(raw, false);
}

void FollowMarkupShapeChanges(const InspectorContext& ctx, std::function<void()> eachFrame)
{
    if (!ctx.FrameRefreshCallbacks || !ctx.RequestInspectorRefresh)
        return;
    auto requested = std::make_shared<bool>(false);
    ctx.FrameRefreshCallbacks->push_back([world = ctx.World, entity = ctx.Entity,
                                          built = ReadShapeSignature(*ctx.World, ctx.Entity), requested,
                                          eachFrame = std::move(eachFrame), host = ctx.Parent,
                                          refresh = ctx.RequestInspectorRefresh]() {
        if (*requested || !world->IsValid(entity))
            return;
        if (eachFrame)
            eachFrame();
        if (ReadShapeSignature(*world, entity) == built)
            return;
        *requested = true;
        host->PostAction(refresh);
    });
}

} // namespace GameEngine::Editor
