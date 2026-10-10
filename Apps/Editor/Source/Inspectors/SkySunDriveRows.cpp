#include "Inspectors/SkySunDriveRows.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunDrive.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/SkyDayCurveRow.h"
#include "Inspectors/LuxText.h"
#include "Inspectors/SkySunCurveReference.h"
#include "Sky/SkyEnvironmentEdit.h"
#include "Sky/SkySunIlluminanceSwitch.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/CurveField.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
namespace SunDrive = Components::SkySunDrive;
namespace SunIlluminance = Components::SkySunIlluminance;
using Components::SkySunIlluminanceSource;

constexpr const char* kDriveTooltip =
    "Set the linked sun light from the time of day: its direction always (the sun by day, the moon by "
    "night), and the fields below. Turning it off hands every field it set back to the light.";
constexpr const char* kColourTooltip =
    "Set the sun light's colour from the sky: the colour of the sun through the air, warm at a low sun, and "
    "the moon's at night. Turned off, the light keeps the colour you give it. Without a custom illuminance "
    "curve the sky always sets the colour, because the colour is what carries the dimming and the night.";
constexpr const char* kCustomCurveTooltip =
    "Off: the sun light's own Intensity is the sun's illuminance with the sun overhead, and the sky dims it "
    "over the day through the light's colour. On: the sky writes the light's Intensity from the curve below, "
    "in lux over the day, and the colour becomes your choice.";
constexpr const char* kNotDrivingTooltip =
    "Link a directional light with no parent in Sun Light and turn on Sky drives sun light to use this.";
constexpr const char* kCurveTooltip =
    "The sun's illuminance on a surface facing it, in lux, over the day. The sky writes it into the sun "
    "light. Below the horizon the moon takes over; set Moonlight for the night.";
constexpr const char* kReplaceTooltip =
    "Replace this curve with what the physical model delivers for the current sun path and date, for a "
    "clear sun of 100 000 lx: the grey line behind the curve. Undo restores your curve.";
constexpr const char* kMoonlightTooltip =
    "The moon's illuminance on a surface facing it, in lux, with a full moon high in the sky: the sun "
    "light's brightness at night. It also sets how bright the night sky and the moon are. 5.16 lx is a "
    "bright night that still reads as night; a real full moon gives about 0.25 lx. Dimming the sun leaves "
    "it where it is.";
// The curve's value axis: from a lit twilight to a bright noon, marked every two decades.
constexpr float kCurveAxisMinimumLux = 1.0f;
constexpr float kCurveAxisMaximumLux = 200000.0f;
constexpr float kCurveAxisMarksLux[] = {1.0f, 100.0f, 10000.0f, 100000.0f};
constexpr const char* kMoonSpanLabel = "moon";

// The spans of the day where the light hands over to the moon, shaded on the graph and labelled.
std::vector<CurveField::ShadedSpan> MoonSpans(const Components::SkyEnvironment& sky)
{
    float sunStart = 0.0f;
    float sunEnd = 0.0f;
    if (!SunDrive::SunOnlyHours(sky, sunStart, sunEnd))
        return {{0.0f, 24.0f, kMoonSpanLabel}};
    std::vector<CurveField::ShadedSpan> spans;
    if (sunStart > 0.0f)
        spans.push_back({0.0f, sunStart, kMoonSpanLabel});
    if (sunEnd < 24.0f)
        spans.push_back({sunEnd, 24.0f, kMoonSpanLabel});
    return spans;
}

// The physical reference line and the moon spans, redrawn only when what they are drawn from changed.
struct CurveReference
{
    CurveField* Graph = nullptr;
    SkySunCurveReferenceInputs Shown{};
    bool HasShown = false;

    void Refresh(const Components::SkyEnvironment& sky)
    {
        const SkySunCurveReferenceInputs inputs = ReadSkySunCurveReferenceInputs(sky);
        if (HasShown && inputs == Shown)
            return;
        Shown = inputs;
        HasShown = true;
        Graph->SetReferenceLine(SunDrive::PhysicalCurveSamples(sky, Components::kClearNoonSunIlluminanceLux));
        Graph->SetShadedSpans(MoonSpans(sky));
    }
};

// True while the sky has a light to drive: the drive set's rows mean something only then.
bool SkyDrivesALight(const ECS::World& world, const Components::SkyEnvironment& sky)
{
    return SunIlluminance::LightSkyWouldDrive(world, sky).IsValid();
}

} // namespace

void AddSkySunDriveRows(UIElement* parent, const InspectorContext& ctx)
{
    ECS::World* w = ctx.World;
    const ECS::EntityHandle e = ctx.Entity;
    const auto* sky = w ? w->GetComponent<Components::SkyEnvironment>(e) : nullptr;
    if (!parent || !sky)
        return;
    const auto extras = InspectorDrag::GetAdditionalEntities(ctx);
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;

    InspectorDrag::AddToggleRow(parent, "Sky drives sun light", sky->TimeOfDayDrivesSunLight,
                 [w, e, n, undo, extras](bool v) {
                     Editor::CommitSkyEnvironmentEdit(w, e, extras, n, undo, "Change Sky Drives Sun Light",
                                                      [v](Components::SkyEnvironment& u) { u.TimeOfDayDrivesSunLight = v; });
                 },
                 kDriveTooltip);

    const bool customCurve = sky->SunIlluminanceSource == SkySunIlluminanceSource::Curve;
    Toggle* curveSwitch = InspectorDrag::AddToggleRow(
        parent, "Custom illuminance curve", customCurve,
        [w, e, n, undo, extras](bool on) {
            Editor::SwitchSkySunIlluminanceSource(w, e, extras, n, undo,
                                                  on ? SkySunIlluminanceSource::Curve : SkySunIlluminanceSource::Light);
        },
        kCustomCurveTooltip);
    const bool drivesALight = SkyDrivesALight(*w, *sky);
    if (!drivesALight)
        InspectorUI::DisableRowOfControl(curveSwitch, kNotDrivingTooltip);
    if (!customCurve)
        return;

    // The colour is the author's choice only under the curve: from the light it carries the dimming
    // and the night, so the sky always sets it and the switch is not shown.
    Toggle* colour = InspectorDrag::AddToggleRow(parent, "Sky sets the light's colour", sky->DriveSunColor,
                                                 [w, e, n, undo, extras](bool v) {
                                                     Editor::CommitSkyEnvironmentEdit(
                                                         w, e, extras, n, undo, "Change Sky Drives Sun Colour",
                                                         [v](Components::SkyEnvironment& u) { u.DriveSunColor = v; });
                                                 },
                                                 kColourTooltip);
    if (!drivesALight)
        InspectorUI::DisableRowOfControl(colour, kNotDrivingTooltip);
}

void AddSkySunCurveRows(UIElement* parent, const InspectorContext& ctx, std::function<void()> refreshTimeOfDayControls,
                        std::shared_ptr<bool> curvePlaybackScrubActive)
{
    ECS::World* w = ctx.World;
    const ECS::EntityHandle e = ctx.Entity;
    const auto* sky = w ? w->GetComponent<Components::SkyEnvironment>(e) : nullptr;
    if (!parent || !sky)
        return;
    const auto extras = InspectorDrag::GetAdditionalEntities(ctx);

    SkyDayCurveRowOptions options;
    options.LogarithmicFloor = SunDrive::kCurveFloorLux;
    options.Presets = false;
    options.TimeAxisLabels = CurveField::TimeLabels::HoursOfDay;
    options.ValueAxisMarks.assign(std::begin(kCurveAxisMarksLux), std::end(kCurveAxisMarksLux));
    options.ValueLabel = [](float lux) { return FormatLux(lux); };
    options.GraphClass = "sky-day-curve-graph-tall";
    options.ValueSuffix = "lx";
    CurveField* graph = AddSkyDayCurveRow(
        parent, "Sun illuminance curve", &Components::SkyEnvironment::SunIlluminanceCurve, options, w, e,
        ctx.ChangeNotifications, ctx.Undo, "Change Sun Illuminance Curve", [](float v) { return std::max(0.0f, v); },
        kCurveAxisMinimumLux, kCurveAxisMaximumLux, kCurveTooltip, extras, ctx.Window, ctx.FrameRefreshCallbacks,
        ctx.SimulationRefreshCallbacks, std::move(refreshTimeOfDayControls), std::move(curvePlaybackScrubActive));

    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, "", nullptr);
    auto replace = std::make_unique<Button>();
    replace->SetText("Replace with the physical curve");
    replace->SetTooltip(kReplaceTooltip);
    replace->AddClass("inspector-button");
    replace->RegisterEventHandler(kEventButtonClick, [w, e, extras, n = ctx.ChangeNotifications, undo = ctx.Undo](UIEvent&) {
        Editor::ReplaceSkySunIlluminanceCurve(w, e, extras, n, undo);
    });
    Button* replaceRaw = replace.get();
    InspectorUI::AddFieldContainer(row)->AddChild(std::move(replace));
    if (!SkyDrivesALight(*w, *sky))
        InspectorUI::DisableRowOfControl(replaceRaw, kNotDrivingTooltip);

    if (!graph)
        return;
    auto reference = std::make_shared<CurveReference>();
    reference->Graph = graph;
    reference->Refresh(*sky);
    if (!ctx.SimulationRefreshCallbacks)
        return;
    ctx.SimulationRefreshCallbacks->push_back([getWorld = ctx.GetWorld, e, reference]() {
        ECS::World* world = getWorld ? getWorld() : nullptr;
        const auto* current = world && world->IsValid(e) ? world->GetComponent<Components::SkyEnvironment>(e) : nullptr;
        if (current)
            reference->Refresh(*current);
    });
}

void AddSkyMoonlightRow(UIElement* parent, const InspectorContext& ctx)
{
    ECS::World* w = ctx.World;
    const ECS::EntityHandle e = ctx.Entity;
    const auto* sky = w ? w->GetComponent<Components::SkyEnvironment>(e) : nullptr;
    if (!parent || !sky)
        return;
    InspectorDrag::AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
        parent, "Moonlight (lx)", sky->MoonlightIlluminance, w, e, ctx.ChangeNotifications, ctx.Undo,
        "Change Sky Moonlight",
        [](Components::SkyEnvironment& u, float v) { u.MoonlightIlluminance = SunDrive::SanitisedMoonlightLux(v); },
        Components::kDefaultMoonlightIlluminanceLux, kMoonlightTooltip, InspectorDrag::GetAdditionalEntities(ctx), 0.0f,
        Components::kMoonlightIlluminanceMaxLux);
}

} // namespace GameEngine
