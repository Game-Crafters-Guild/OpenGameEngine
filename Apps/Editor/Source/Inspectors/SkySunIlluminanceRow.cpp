#include "Inspectors/SkySunIlluminanceRow.h"

#include "Components/Name.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/LuxText.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine
{
namespace
{
namespace SunIlluminance = Components::SkySunIlluminance;

constexpr const char* kSunIlluminanceLabel = "Sun illuminance (lx)";
constexpr const char* kSunIlluminanceTooltip =
    "Illuminance of the sun on a surface facing it, with the sun overhead on a clear day, in lux. "
    "A clear overhead sun is about 100 000 lx; overcast is about 10 000 lx. The sky dims and reddens "
    "it toward the horizon and hands over to moonlight at night; this value does not change. Stored "
    "on the linked sun light.";
constexpr const char* kNoSunText = "No sun: add or enable a directional light";
constexpr const char* kLinkTooltip = "Link this light as the sky's sun, so this row edits it.";
// A driven sun under this fraction of the clear noon sun is almost certainly not what its author
// meant: most often a scene saved at night by a build that wrote the night's dimming into the light.
constexpr float kDimmedSunFraction = 0.1f;
// A readout changes on screen only when it moves by more than this, in lux, so an idle scene does
// not rewrite its field.
constexpr float kFieldRefreshToleranceLux = 0.5f;

std::string LightName(const ECS::World& world, ECS::EntityHandle light)
{
    const auto* name = world.GetComponent<Components::Name>(light);
    return name && name->value[0] != '\0' ? std::string(name->View()) : std::string("Directional Light");
}

// Everything the row's state lines say, read once per refresh. The lines are rebuilt only when this
// changes, so an idle scene formats no strings.
struct SunReadout
{
    bool Rendered = false;       // this sky is the one the sky system renders
    ECS::EntityHandle Resolved;  // the light this sky takes the sun's brightness from
    ECS::EntityHandle Driven;    // the light the sky system drives this frame
    float ResolvedLux = 0.0f;    // the resolved light's authored illuminance
    float DrivenLux = 0.0f;      // the authored illuminance of the driven light

    bool operator==(const SunReadout&) const = default;
};

SunReadout ReadSunState(ECS::World& world, ECS::EntityHandle skyEntity, const Components::SkyEnvironment& sky)
{
    SunReadout readout;
    readout.Rendered = SunIlluminance::RenderedSky(world) == skyEntity;
    readout.Resolved = SunIlluminance::ResolvedSunLight(world, sky);
    if (const auto* resolved = readout.Resolved.IsValid() ? world.GetComponent<Components::Light>(readout.Resolved) : nullptr)
        readout.ResolvedLux = SunIlluminance::LuxFromLightIntensity(resolved->Intensity, resolved->IntensityUnit);
    if (readout.Rendered)
    {
        readout.Driven = SunIlluminance::DrivenSunLight(world);
        if (const auto* driven = readout.Driven.IsValid() ? world.GetComponent<Components::Light>(readout.Driven) : nullptr)
            readout.DrivenLux = SunIlluminance::LuxFromLightIntensity(driven->Intensity, driven->IntensityUnit);
    }
    return readout;
}

// Under the row while the sky drives a sun far dimmer than a clear one: how dim, and how to reset it.
// Empty otherwise.
std::string DimmedSunText(const SunReadout& readout)
{
    if (!readout.Driven.IsValid())
        return {};
    const float fraction = readout.DrivenLux / Components::kClearNoonSunIlluminanceLux;
    if (!(fraction < kDimmedSunFraction))
        return {};
    char text[128];
    std::snprintf(text, sizeof(text), "%.2g %% of a clear sun. Double-click the label to reset to %s.",
                  fraction * 100.0f, FormatLux(Components::kClearNoonSunIlluminanceLux).c_str());
    return text;
}

// Show or hide an element that only applies in some states.
void SetShown(UIElement* element, bool shown)
{
    element->Overrides().Set(Style::Display, shown ? DisplayMode::Flex : DisplayMode::None);
}

// A state line under the row: shown with `text`, hidden when there is nothing to say.
void ShowStateLine(Label* line, const std::string& text)
{
    if (line->GetText() != text)
        line->SetText(text);
    SetShown(line, !text.empty());
}

// The unlinked row's value: the sun the sky reads and where it comes from, or that there is none.
std::string ReadOnlySunText(ECS::World& world, const SunReadout& readout)
{
    if (!readout.Resolved.IsValid())
        return kNoSunText;
    return FormatLux(readout.ResolvedLux) + " from \"" + LightName(world, readout.Resolved) + "\" (read-only)";
}

// The unlinked row's widgets, which the refresh keeps current: the scene's sun can change, appear
// or go away while nothing is linked.
struct ReadOnlySunRow
{
    Label* Value = nullptr;
    Button* Link = nullptr;
};

void RefreshReadOnlySunRow(const ReadOnlySunRow& row, ECS::World& world, const SunReadout& readout)
{
    const std::string text = ReadOnlySunText(world, readout);
    if (row.Value->GetText() != text)
        row.Value->SetText(text);
    SetShown(row.Link, readout.Resolved.IsValid());
}

// No light is linked: the sun the sky reads, read-only, with the action that links it.
ReadOnlySunRow AddReadOnlySunRow(UIElement* parent, ECS::World& world, ECS::EntityHandle skyEntity,
                                 const SunReadout& readout, std::function<void(ECS::EntityHandle)> linkSun)
{
    UIElement* row = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(row, kSunIlluminanceLabel, kSunIlluminanceTooltip);
    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->AddClass("inspector-field-inline-action");

    ReadOnlySunRow widgets;
    auto value = std::make_unique<Label>();
    value->AddClass("inspector-text");
    widgets.Value = value.get();
    field->AddChild(std::move(value));

    // The light to link is resolved at the click: it is whichever the sky reads then.
    auto link = std::make_unique<Button>();
    link->SetText("Link");
    link->SetTooltip(kLinkTooltip);
    link->RegisterEventHandler(kEventButtonClick, [linkSun = std::move(linkSun), &world, skyEntity](UIEvent&) {
        const auto* current = world.GetComponent<Components::SkyEnvironment>(skyEntity);
        const ECS::EntityHandle resolved =
            current ? SunIlluminance::ResolvedSunLight(world, *current) : ECS::EntityHandle{};
        if (linkSun && resolved.IsValid())
            linkSun(resolved);
    });
    widgets.Link = link.get();
    field->AddChild(std::move(link));

    RefreshReadOnlySunRow(widgets, world, readout);
    return widgets;
}
} // namespace

void AddSkySunIlluminanceRow(UIElement* parent, const InspectorContext& ctx,
                             std::function<void(ECS::EntityHandle)> linkSun)
{
    ECS::World* w = ctx.World;
    const ECS::EntityHandle e = ctx.Entity;
    const auto* sky = w ? w->GetComponent<Components::SkyEnvironment>(e) : nullptr;
    if (!parent || !sky)
        return;

    FloatField* field = nullptr;
    ReadOnlySunRow readOnly;
    const SunReadout readout = ReadSunState(*w, e, *sky);
    const ECS::EntityHandle sun = SunIlluminance::EditableSunLight(*w, *sky);
    if (const auto* light = sun.IsValid() ? w->GetComponent<Components::Light>(sun) : nullptr)
    {
        field = InspectorDrag::AddComponentFloatRowWithDrag<Components::Light>(
            parent, kSunIlluminanceLabel,
            SunIlluminance::LuxFromLightIntensity(light->Intensity, light->IntensityUnit), w, sun,
            ctx.ChangeNotifications, ctx.Undo, "Change Sun Illuminance",
            [](Components::Light& u, float lux) {
                u.Intensity = SunIlluminance::LightIntensityFromLux(std::max(0.0f, lux), u.IntensityUnit);
            },
            Components::kClearNoonSunIlluminanceLux, kSunIlluminanceTooltip, {}, 0.0f);
    }
    else
    {
        readOnly = AddReadOnlySunRow(parent, *w, e, readout, std::move(linkSun));
    }

    auto dimmed = std::make_unique<Label>();
    dimmed->AddClass("inspector-text");
    Label* dimmedRaw = dimmed.get();
    ShowStateLine(dimmedRaw, DimmedSunText(readout));
    parent->AddChild(std::move(dimmed));

    if (!ctx.SimulationRefreshCallbacks)
        return;
    auto lastReadout = std::make_shared<SunReadout>(readout);
    ctx.SimulationRefreshCallbacks->push_back([getWorld = ctx.GetWorld, e, sun, field, readOnly, dimmedRaw,
                                               lastReadout]() {
        ECS::World* world = getWorld ? getWorld() : nullptr;
        const auto* current = world && world->IsValid(e) ? world->GetComponent<Components::SkyEnvironment>(e) : nullptr;
        if (!current)
            return;
        const SunReadout now = ReadSunState(*world, e, *current);
        if (!(now == *lastReadout))
        {
            *lastReadout = now;
            ShowStateLine(dimmedRaw, DimmedSunText(now));
            if (readOnly.Value)
                RefreshReadOnlySunRow(readOnly, *world, now);
        }

        // The light's value can change elsewhere (its own inspector, undo); follow it unless the
        // author is typing here.
        const auto* light = field && world->IsValid(sun) ? world->GetComponent<Components::Light>(sun) : nullptr;
        if (!light || field->IsInFocusChain())
            return;
        const float lux = SunIlluminance::LuxFromLightIntensity(light->Intensity, light->IntensityUnit);
        if (std::abs(field->GetValue() - lux) > kFieldRefreshToleranceLux)
            field->SetValueWithoutNotify(lux);
    });
}

} // namespace GameEngine
