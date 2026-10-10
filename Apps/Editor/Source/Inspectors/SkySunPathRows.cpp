#include "Inspectors/SkySunPathRows.h"

#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunDrive.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "Components/Rendering/SkySunPath.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "Inspectors/DayOfYearText.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/SkySunPathCaptions.h"
#include "Sky/SkySunDayKind.h"
#include "Sky/SkySunPathSwitch.h"
#include "Mathematics/Curve.h"
#include "Rendering/Sky/SolarPath.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/EnumField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
namespace SunPath = Components::SkySunPath;
using Components::SkySunPathKind;

constexpr EnumEntry<SkySunPathKind> kSunPathKinds[] = {
    {SkySunPathKind::Earth, "Earth"},
    {SkySunPathKind::Custom, "Custom"},
};

constexpr const char* kSunPathTooltip =
    "Where the sun's path comes from. Earth: the path the sun takes at a place on Earth on a day of "
    "the year, set by Latitude, Day of year and North. Custom: set the path directly, for a world "
    "that is not Earth: the axis the sun circles once a day and how high it stands at noon. Each "
    "choice keeps its own settings, so switching back finds them as you left them.";
constexpr const char* kCustomPathHint =
    "The sun circles the axis once a day, and at 12:00 it stands where Height at noon puts it.";
// A Height at noon at either end of its reach puts the sun where the axis points or straight
// opposite, where it has no circle.
constexpr const char* kStillSunHint = "The sun sits where the axis points, or straight opposite, and does not move.";
constexpr const char* kLatitudeTooltip =
    "How far north (positive) or south of the equator the scene is, in degrees. Sets the sun's "
    "height at noon and the length of the day.";
constexpr const char* kDayOfYearTooltip =
    "The day of the year, from 1 (1 January) to 365 (31 December), in a year without 29 February. "
    "With Latitude it sets the season: the sun's height at noon and the length of the day. Type a date "
    "such as 21 June, 21/6 (day first) or 2026-06-21 to set the day it falls on.";
constexpr const char* kNorthTooltip =
    "Which way north points in the scene, in degrees, turning clockwise seen from above: at 0 north "
    "points along\xC2\xA0+Z, at 90 along\xC2\xA0+X, at 180 along\xC2\xA0\xE2\x80\x91" "Z and at 270 "
    "along\xC2\xA0\xE2\x80\x91" "X. Turning it turns the whole "
    "sun path, for example to set the sun over the sea; the sun rises to the east of north. The "
    "entity's own rotation does not affect the sun.";
constexpr const char* kAxisHeadingTooltip =
    "Which way the sun's axis leans, in degrees, turning clockwise seen from above: at 0 it leans "
    "toward +Z, at 90 toward +X. The sun circles this axis once a day and stands at noon on the side "
    "away from it (on Earth the axis leans toward north). The Scene View draws the axis while this "
    "entity is selected.";
constexpr const char* kAxisAltitudeTooltip =
    "How far the sun's axis rises above the horizon, in degrees, from -90 (straight down) to 90 "
    "(straight up). Level (0) gives a 12-hour day with the sun rising and setting straight up and "
    "down; straight up, the sun circles the sky at one height and never sets. On Earth the axis "
    "stands at the latitude.";
constexpr const char* kNoonHeightTooltip =
    "How high the sun stands at noon, in degrees, measured from the horizon on the side away from the "
    "axis (the far side) and carrying on over the top of the sky: 90 is straight overhead, and 120 is 60 "
    "degrees above the horizon on the axis side. Below 0, or above 180, the noon sun stays under the horizon "
    "and the sun never rises. How far it can go either way depends on Axis altitude.";
constexpr const char* kOverTheDayTooltip =
    "The sun's illuminance on a surface facing it, in lux, from midnight to midnight, on a scale from "
    "0 to the sun light's illuminance. The sun path and the sun light's illuminance move it. "
    "Read-only.";

constexpr float kHoursPerDay = 24.0f;
// The playhead moves only when the hour moves by more than this, so an idle scene redraws nothing.
constexpr float kPlayheadRefreshHours = 1e-4f;
constexpr float kUnboundedDegrees = std::numeric_limits<float>::infinity();


// The sky's path with its heading turned to 0: a heading turns the path without changing any
// elevation, so the preview and the day's figures leave it out.
Rendering::SolarPathAngles ElevationAngles(const Components::SkyEnvironment& sky)
{
    Rendering::SolarPathAngles angles = SunPath::PathAngles(sky);
    angles.PoleHeadingRadians = 0.0;
    return angles;
}

// Everything the "Over the day" graph depends on.
struct PreviewInputs
{
    Rendering::SolarPathAngles Angles{};
    float SunLux = 0.0f;
    Components::SkyScalarDayKeys KeyTimes{};
    Components::SkyVec3DayKeys SunTint{};

    bool operator==(const PreviewInputs& other) const
    {
        const auto sameKeys = [](const Components::SkyScalarDayKeys& a, const Components::SkyScalarDayKeys& b) {
            return a.Midnight == b.Midnight && a.Dawn == b.Dawn && a.Midday == b.Midday && a.Sunset == b.Sunset;
        };
        const auto sameColors = [](const Components::SkyVec3DayKeys& a, const Components::SkyVec3DayKeys& b) {
            return std::equal(a.Midnight, a.Midnight + 3, b.Midnight) && std::equal(a.Dawn, a.Dawn + 3, b.Dawn) &&
                   std::equal(a.Midday, a.Midday + 3, b.Midday) && std::equal(a.Sunset, a.Sunset + 3, b.Sunset);
        };
        return Angles.PoleAltitudeRadians == other.Angles.PoleAltitudeRadians &&
               Angles.DeclinationRadians == other.Angles.DeclinationRadians && SunLux == other.SunLux &&
               sameKeys(KeyTimes, other.KeyTimes) && sameColors(SunTint, other.SunTint);
    }
};

PreviewInputs ReadPreviewInputs(ECS::World& world, const Components::SkyEnvironment& sky)
{
    PreviewInputs inputs;
    inputs.Angles = ElevationAngles(sky);
    inputs.SunLux = Components::SkySunIlluminance::ClearSunLux(world, sky);
    inputs.KeyTimes = sky.DayKeyTimesHours;
    inputs.SunTint = sky.SunTintKeys;
    return inputs;
}

// The value axis is fixed from 0 to the sun light's illuminance, the value an overhead sun delivers,
// so the height of the curve is the season: a winter day stays low, a summer day nearly reaches
// the top. Scaling to each day's own peak would draw every day the same height. The hours run under
// the graph, and the playback line, its time and its dot on the curve mark now, so the graph leaves
// out the value marker on its left edge.
void ConfigurePreview(CurveField& graph, const std::vector<Math::CurveKey>& keys, float sunLux)
{
    CurveField::Config config;
    config.TimeMin = 0.0f;
    config.TimeMax = kHoursPerDay;
    config.ValueMin = 0.0f;
    config.ValueMax = sunLux > 0.0f ? sunLux : 1.0f;
    config.AllowTimeDrag = false;
    config.AllowAddRemove = false;
    config.ShowPlaybackIndicator = true;
    config.ShowPlaybackValueMarker = false;
    config.TimeAxisLabels = CurveField::TimeLabels::HoursOfDay;
    config.ReadOnly = true;
    graph.SetConfig(config);
    graph.SetKeys(keys);
}

void SetPlayhead(CurveField& graph, const Components::SkyEnvironment& sky, float sunLux)
{
    const float hours = std::clamp(sky.TimeOfDayHours, 0.0f, kHoursPerDay);
    graph.SetPlaybackIndicator(hours, Components::SkySunDrive::PhysicalIlluminanceLux(sky, sunLux, hours));
}

// A field of the rows. It shows what the sky stores, which is not always what was typed (a heading
// of 365 is stored as 5), except while it is being edited.
struct ShownField
{
    FloatField* Field = nullptr;
    // True from an edit's first preview to its commit. The inspector's poll runs between keystrokes,
    // and the field then keeps what is being typed or dragged.
    bool Editing = false;
    // False until the field has shown the sky's value, and again when an edit ends: the text is
    // rewritten only when what it shows moves, and an edit's previews have moved it already.
    bool HasShown = false;

    void ShowValue(float value)
    {
        if (!Editing && Field->GetValue() != value)
            Field->SetValueWithoutNotify(value);
        HasShown = true;
    }
};

// A degrees field whose number carries a caption in its own suffix.
struct CaptionedField : ShownField
{
    std::string (*Caption)(float degrees) = nullptr;
    float Shown = 0.0f;

    void Refresh(float value)
    {
        if (!Field || (HasShown && value == Shown))
            return;
        ShowValue(value);
        Shown = value;
        Field->SetSuffix(Caption(value));
    }
};

// The Height at noon field: its range and its caption both depend on the axis altitude. It shows
// the height the path uses, the stored one brought into the axis's reach, and clamps typed input
// to that reach, so it never shows a height the path clamps away; a vertical axis has no sides for
// the caption to name.
struct NoonHeightField : ShownField
{
    float ShownHeight = 0.0f;
    float ShownAltitude = 0.0f;

    void Refresh(float storedHeight, float axisAltitude)
    {
        const float height = Rendering::ReachableNoonHeightDegrees(storedHeight, axisAltitude);
        if (!Field || (HasShown && height == ShownHeight && axisAltitude == ShownAltitude))
            return;
        ShowValue(height);
        ShownHeight = height;
        ShownAltitude = axisAltitude;
        const Rendering::NoonHeightRange reach = Rendering::ReachableNoonHeights(axisAltitude);
        Field->SetValueRange(reach.Lowest, reach.Highest);
        Field->SetSuffix(NoonHeightCaption(height, axisAltitude));
    }
};

// What the rows show, re-read at every refresh; the graph is rebuilt only when its inputs change.
// Only the active path's fields exist; the others stay null.
struct SunPathWidgets
{
    CaptionedField Latitude;
    IntField* Day = nullptr;
    CaptionedField North;
    CaptionedField AxisHeading;
    CaptionedField AxisAltitude;
    NoonHeightField NoonHeight;
    // The Custom path's hint, which says what the path does: null on the Earth path.
    EditorUI::CollapsibleInfoCard* CustomHint = nullptr;
    CurveField* Graph = nullptr;

    // What the rows last showed, so a refresh only formats text and rebuilds the graph when an input
    // moved.
    int32_t ShownDayOfYear = 0;
    PreviewInputs ShownInputs;
    float ShownPlayheadHours = -1.0f;
};

void Refresh(SunPathWidgets& widgets, ECS::World& world, const Components::SkyEnvironment& sky)
{
    widgets.Latitude.Refresh(sky.Latitude);
    if (widgets.Day && sky.DayOfYear != widgets.ShownDayOfYear)
    {
        widgets.ShownDayOfYear = sky.DayOfYear;
        widgets.Day->SetSuffix("(" + DateOfDay(sky.DayOfYear) + ")");
    }
    widgets.North.Refresh(sky.NorthHeading);
    widgets.AxisHeading.Refresh(sky.CustomAxisHeading);
    widgets.AxisAltitude.Refresh(sky.CustomAxisAltitude);
    widgets.NoonHeight.Refresh(sky.CustomNoonHeight, sky.CustomAxisAltitude);
    if (widgets.CustomHint)
    {
        const std::string hint =
            Editor::ClassifySkySunDay(sky) == Editor::SkySunDayKind::StandsStill ? kStillSunHint : kCustomPathHint;
        if (widgets.CustomHint->GetFullText() != hint)
            widgets.CustomHint->SetText(hint);
    }

    if (!widgets.Graph)
        return;
    const PreviewInputs inputs = ReadPreviewInputs(world, sky);
    const bool rebuilt = !(inputs == widgets.ShownInputs);
    if (rebuilt)
    {
        widgets.ShownInputs = inputs;
        ConfigurePreview(*widgets.Graph, Components::SkySunDrive::PhysicalCurveSamples(sky, inputs.SunLux),
                         inputs.SunLux);
    }
    if (rebuilt || std::abs(sky.TimeOfDayHours - widgets.ShownPlayheadHours) > kPlayheadRefreshHours)
    {
        widgets.ShownPlayheadHours = sky.TimeOfDayHours;
        SetPlayhead(*widgets.Graph, sky, inputs.SunLux);
    }
}

// Shows the sky `entity` carries now; nothing when it carries none.
void RefreshFromWorld(SunPathWidgets& widgets, ECS::World* world, ECS::EntityHandle entity)
{
    const auto* sky =
        world && world->IsValid(entity) ? world->GetComponent<Components::SkyEnvironment>(entity) : nullptr;
    if (sky)
        Refresh(widgets, *world, *sky);
}

using SkyFieldSetter = void (*)(Components::SkyEnvironment&, float);

// What a degrees row edits: what an edit in flight writes and what its commit writes, which differ
// only where the commit settles another field.
struct DegreesRow
{
    const char* Label = nullptr;
    const char* UndoLabel = nullptr;
    const char* Tooltip = nullptr;
    SkyFieldSetter Preview = nullptr;
    SkyFieldSetter Commit = nullptr;
    float Default = 0.0f;
    float Minimum = -kUnboundedDegrees;
    float Maximum = kUnboundedDegrees;
};

// A degrees row on the sky, one undo step per edit, with its caption in the field's own suffix.
// `shown` is the row's entry in `widgets`. An edit marks it from its first preview to its commit;
// when it ends the rows show what the sky stores, and the field gets its caption, which it holds
// back after a commit until it is given the new one.
void AddDegreesRow(UIElement* parent, const InspectorContext& ctx, const std::shared_ptr<SunPathWidgets>& widgets,
                   ShownField& shown, const DegreesRow& row, float value)
{
    auto handlers = InspectorDrag::MakeComponentInteractiveHandlers<Components::SkyEnvironment, float>(
        ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, row.UndoLabel, row.Preview, row.Commit,
        InspectorDrag::GetAdditionalEntities(ctx));
    ShownField* edited = &shown;
    shown.Field = InspectorDrag::AddFloatRowWithDrag(
        parent, row.Label, value,
        [widgets, edited, preview = std::move(handlers.first)](float degrees) {
            edited->Editing = true;
            preview(degrees);
        },
        [widgets, edited, world = ctx.World, entity = ctx.Entity, commit = std::move(handlers.second)](float degrees) {
            commit(degrees);
            edited->Editing = false;
            edited->HasShown = false;
            RefreshFromWorld(*widgets, world, entity);
        },
        row.Default, row.Tooltip, row.Minimum, row.Maximum);
    shown.Field->SetSuffixAlignment(SuffixAlignment::AfterText);
    shown.Field->SetSuffixRole(SuffixRole::Caption);
}

void AddEarthRows(UIElement* parent, const InspectorContext& ctx, const Components::SkyEnvironment& sky,
                  const std::shared_ptr<SunPathWidgets>& shared)
{
    SunPathWidgets& widgets = *shared;
    widgets.Latitude.Caption = LatitudeCaption;
    AddDegreesRow(parent, ctx, shared, widgets.Latitude,
                  {.Label = "Latitude (\xC2\xB0)",
                   .UndoLabel = "Change Sky Latitude",
                   .Tooltip = kLatitudeTooltip,
                   .Preview = SunPath::SetLatitude,
                   .Commit = SunPath::SetLatitude,
                   .Minimum = -Rendering::kMaximumLatitudeDegrees,
                   .Maximum = Rendering::kMaximumLatitudeDegrees},
                  sky.Latitude);

    widgets.Day = InspectorDrag::AddComponentIntRowWithDrag<Components::SkyEnvironment>(
        parent, "Day of year", sky.DayOfYear, ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo,
        "Change Sky Day Of Year",
        [](Components::SkyEnvironment& u, int day) { u.DayOfYear = std::clamp(day, 1, Rendering::kDaysInCalendarYear); },
        Rendering::kMarchEquinoxDay, kDayOfYearTooltip, InspectorDrag::GetAdditionalEntities(ctx));
    widgets.Day->SetRange(1, Rendering::kDaysInCalendarYear);
    AcceptDateEntry(*widgets.Day);
    widgets.Day->SetSuffixAlignment(SuffixAlignment::AfterText);
    widgets.Day->SetSuffixRole(SuffixRole::Caption);

    widgets.North.Caption = HeadingCaption;
    AddDegreesRow(parent, ctx, shared, widgets.North,
                  {.Label = "North (\xC2\xB0)",
                   .UndoLabel = "Change Sky North",
                   .Tooltip = kNorthTooltip,
                   .Preview = SunPath::SetNorthHeading,
                   .Commit = SunPath::SetNorthHeading},
                  sky.NorthHeading);
}

void AddCustomRows(UIElement* parent, const InspectorContext& ctx, const Components::SkyEnvironment& sky,
                   const std::shared_ptr<SunPathWidgets>& shared)
{
    SunPathWidgets& widgets = *shared;
    widgets.AxisHeading.Caption = HeadingCaption;
    AddDegreesRow(parent, ctx, shared, widgets.AxisHeading,
                  {.Label = "Axis heading (\xC2\xB0)",
                   .UndoLabel = "Change Sky Axis Heading",
                   .Tooltip = kAxisHeadingTooltip,
                   .Preview = SunPath::SetCustomAxisHeading,
                   .Commit = SunPath::SetCustomAxisHeading},
                  sky.CustomAxisHeading);
    // Moving the axis moves the heights it reaches, and the commit alone brings the stored Height at
    // noon into them: a preview that did would cut it to the reach of an altitude passed on the way.
    widgets.AxisAltitude.Caption = AxisAltitudeCaption;
    AddDegreesRow(parent, ctx, shared, widgets.AxisAltitude,
                  {.Label = "Axis altitude (\xC2\xB0)",
                   .UndoLabel = "Change Sky Axis Altitude",
                   .Tooltip = kAxisAltitudeTooltip,
                   .Preview = SunPath::PreviewCustomAxisAltitude,
                   .Commit = SunPath::SetCustomAxisAltitude,
                   .Minimum = -Rendering::kMaximumLatitudeDegrees,
                   .Maximum = Rendering::kMaximumLatitudeDegrees},
                  sky.CustomAxisAltitude);
    AddDegreesRow(parent, ctx, shared, widgets.NoonHeight,
                  {.Label = "Height at noon (\xC2\xB0)",
                   .UndoLabel = "Change Sky Height At Noon",
                   .Tooltip = kNoonHeightTooltip,
                   .Preview = SunPath::SetCustomNoonHeight,
                   .Commit = SunPath::SetCustomNoonHeight,
                   .Default = Rendering::kOverheadNoonHeightDegrees,
                   .Minimum = Rendering::kLowestNoonHeightDegrees,
                   .Maximum = Rendering::kHighestNoonHeightDegrees},
                  Rendering::ReachableNoonHeightDegrees(sky.CustomNoonHeight, sky.CustomAxisAltitude));
}
} // namespace

void AddSkySunPathRows(UIElement* parent, const InspectorContext& ctx)
{
    ECS::World* w = ctx.World;
    const ECS::EntityHandle e = ctx.Entity;
    const auto* sky = w ? w->GetComponent<Components::SkyEnvironment>(e) : nullptr;
    if (!parent || !sky)
        return;

    auto* pathField = InspectorUI::AddEnumRow(parent, "Sun path", kSunPathKinds, sky->SunPath, kSunPathTooltip);
    pathField->SetOnValueChanged([w, e, extras = InspectorDrag::GetAdditionalEntities(ctx), n = ctx.ChangeNotifications,
                                  undo = ctx.Undo](SkySunPathKind kind) { Editor::SwitchSkySunPath(w, e, extras, n, undo, kind); });

    auto widgets = std::make_shared<SunPathWidgets>();
    if (sky->SunPath == SkySunPathKind::Custom)
    {
        auto hint = std::make_unique<EditorUI::CollapsibleInfoCard>(kCustomPathHint);
        widgets->CustomHint = hint.get();
        parent->AddChild(std::move(hint));
        AddCustomRows(parent, ctx, *sky, widgets);
    }
    else
        AddEarthRows(parent, ctx, *sky, widgets);

    // Under a sun illuminance curve the curve's own graph draws the physical day behind it
    // (AddSkySunCurveRows), so this one is only for the light's illuminance.
    if (sky->SunIlluminanceSource == Components::SkySunIlluminanceSource::Light)
    {
        UIElement* graphRow = InspectorUI::AddRow(parent);
        InspectorUI::AddLabel(graphRow, "Over the day", kOverTheDayTooltip);
        auto graph = std::make_unique<CurveField>();
        widgets->Graph = graph.get();
        InspectorUI::AddFieldContainer(graphRow)->AddChild(std::move(graph));
    }

    Refresh(*widgets, *w, *sky);

    if (!ctx.SimulationRefreshCallbacks)
        return;
    ctx.SimulationRefreshCallbacks->push_back(
        [getWorld = ctx.GetWorld, e, widgets]() { RefreshFromWorld(*widgets, getWorld ? getWorld() : nullptr, e); });
}

} // namespace GameEngine
