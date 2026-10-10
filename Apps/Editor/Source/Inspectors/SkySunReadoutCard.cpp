#include "Inspectors/SkySunReadoutCard.h"

#include "Components/Name.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "Components/Rendering/SkySunPath.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "InspectorRegistry.h"
#include "Inspectors/SkySunReadoutText.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SolarPath.h"
#include "Sky/SkySunDayKind.h"
#include "UI/Controls/Label.h"
#include "UI/InfoCard.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace GameEngine
{
namespace
{
namespace SunIlluminance = Components::SkySunIlluminance;
namespace SunPath = Components::SkySunPath;

constexpr const char* kHiddenClass = "hidden";
constexpr float kRadiansToDegrees = 57.29577951f;
constexpr float kNoonHours = 12.0f;
constexpr const char* kCardTooltip =
    "Times are solar time: 12:00 is when the sun is highest, wherever the scene is, with no time zone. "
    "Sunrise and sunset are when the middle of the sun crosses a flat horizon; a published almanac, "
    "which allows for the air bending the light, gives a day a few minutes longer. Illuminance is the "
    "sun light's alone, on a surface facing the sun or the moon: the sun's direct light, carried on through "
    "twilight until the sun is about 10\xC2\xB0 below the horizon, then the moon's. The sky's own light is not "
    "included.";

Label* AddLine(UIElement* card)
{
    auto line = std::make_unique<Label>();
    EditorUI::StyleInfoCardText(line.get());
    Label* raw = line.get();
    card->AddChild(std::move(line));
    return raw;
}

void ShowLine(Label* line, const std::string& text)
{
    if (line->GetText() != text)
        line->SetText(text);
    if (text.empty())
        line->AddClass(kHiddenClass);
    else
        line->RemoveClass(kHiddenClass);
}

std::string LightName(const ECS::World& world, ECS::EntityHandle light)
{
    const auto* name = world.GetComponent<Components::Name>(light);
    return name && name->value[0] != '\0' ? std::string(name->value) : std::string("Directional Light");
}

SkySunDay DescribeDay(ECS::World& world, const Components::SkyEnvironment& sky)
{
    const Rendering::SolarPathAngles angles = SunPath::PathAngles(sky);
    SkySunDay day;
    day.CustomPath = sky.SunPath == Components::SkySunPathKind::Custom;
    day.Kind = Editor::ClassifySkySunDay(sky);
    day.AxisHeadingDegrees = sky.CustomAxisHeading;
    day.DayLengthHours = Rendering::SolarDayLengthHours(angles);
    day.NoonElevationDegrees = Rendering::SolarNoonElevationDegrees(angles);
    day.MidnightElevationDegrees = Rendering::SolarMidnightElevationDegrees(angles);
    day.NoonAzimuthDegrees = Rendering::SolarPositionAtHour(Rendering::MakeSolarFrame(angles), kNoonHours).AzimuthDegrees;
    // What the driven light gives from the sun at noon, the Now line's quantity, so the two agree at 12:00.
    day.NoonLux = SunPath::DrivenSunShareLux(sky, day.NoonElevationDegrees, SunIlluminance::ClearSunLux(world, sky),
                                             kNoonHours);
    return day;
}

// The sun's elevation in the sky the renderer is drawing this frame, in degrees. False when no sky
// is being drawn.
bool TryCurrentSunElevationDegrees(float& outDegrees)
{
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    const auto* sky = renderServices ? renderServices->GetFeature<Engine::Renderer::SkyRenderFeature>() : nullptr;
    if (!sky || !sky->HasActiveSettings())
        return false;
    const float up = std::clamp(sky->GetSettings().scatteringSunDir[1], -1.0f, 1.0f);
    outDegrees = std::asin(up) * kRadiansToDegrees;
    return true;
}

// What the Now lines say, read once per refresh; the text is rebuilt only when this changes.
struct NowState
{
    // This sky is the one rendered and it has a sun to speak of; otherwise there are no Now lines.
    bool Shown = false;
    bool Driving = false;
    SkySunNotDriving Reason = SkySunNotDriving::NotLinked;
    ECS::EntityHandle Light;
    SkySunNow Now;

    bool operator==(const NowState&) const = default;
};

NowState ReadNow(ECS::World& world, ECS::EntityHandle skyEntity, const Components::SkyEnvironment& sky)
{
    NowState state;
    const ECS::EntityHandle resolved = SunIlluminance::ResolvedSunLight(world, sky);
    if (SunIlluminance::RenderedSky(world) != skyEntity || !resolved.IsValid())
        return state;
    state.Shown = true;

    const ECS::EntityHandle driven = SunIlluminance::DrivenSunLight(world);
    const auto* light = driven.IsValid() ? world.GetComponent<Components::Light>(driven) : nullptr;
    if (!light)
    {
        const bool linked = SunIlluminance::EditableSunLight(world, sky).IsValid();
        state.Light = resolved;
        state.Reason = !linked                          ? SkySunNotDriving::NotLinked
                       : !sky.TimeOfDayDrivesSunLight ? SkySunNotDriving::DriveOff
                                                        : SkySunNotDriving::LightHasParent;
        return state;
    }
    // A driven light with no sky drawn this frame has no hour to describe.
    float elevation = 0.0f;
    if (!TryCurrentSunElevationDegrees(elevation))
    {
        state.Shown = false;
        return state;
    }

    state.Driving = true;
    state.Light = driven;
    state.Now.Hours = sky.TimeOfDayHours;
    state.Now.SunElevationDegrees = elevation;
    state.Now.SunLux =
        SunPath::DrivenSunShareLux(sky, elevation, SunIlluminance::ClearSunLux(world, sky), sky.TimeOfDayHours);
    state.Now.NightLux = std::max(0.0f, SunIlluminance::DeliveredLux(*light) - state.Now.SunLux);
    state.Now.HandoverWeight = SunPath::DrivenMoonBlend(sky, elevation);
    state.Now.MidnightHandoverWeight =
        SunPath::DrivenMoonBlend(sky, Rendering::SolarMidnightElevationDegrees(SunPath::PathAngles(sky)));
    state.Now.FullNightLux = SunPath::FullNightMoonLux(sky);
    state.Now.MoonRises = SunPath::MoonHighestElevationDegrees(sky) > 0.0f;
    state.Now.MoonHidden = !sky.ShowMoon;
    state.Now.HandsOverToMoon = sky.AutoSunMoon;
    return state;
}

// The card's lines, and what they were built from, so a refresh formats text only when a figure moved.
struct ReadoutState
{
    SkySunReadoutCard Card;
    SkySunDay Day;
    NowState Now;
    SkySunReadoutLines Lines;
    bool HasShown = false;

    explicit ReadoutState(UIElement* parent)
        : Card(parent)
    {
    }
};

void Refresh(ReadoutState& state, ECS::World& world, ECS::EntityHandle skyEntity, const Components::SkyEnvironment& sky)
{
    const SkySunDay day = DescribeDay(world, sky);
    const NowState now = ReadNow(world, skyEntity, sky);
    if (state.HasShown && day == state.Day && now == state.Now)
        return;

    state.Lines.Day = SkySunDayLine(day);
    // At 12:00 the Now line says what the noon line would: the card shows that one sentence.
    const bool nowIsNoon = now.Driving && SkySunNowIsNoon(now.Now);
    state.Lines.Noon = nowIsNoon ? std::string() : SkySunNoonLine(day);
    state.Lines.Now.clear();
    state.Lines.Moon.clear();
    if (now.Driving)
    {
        state.Lines.Now = SkySunNowLine(day, now.Now);
        state.Lines.Moon = SkyMoonNowLine(day, now.Now);
    }
    else if (now.Shown)
    {
        state.Lines.Now = SkySunNotDrivingLine(now.Reason, LightName(world, now.Light));
    }
    state.HasShown = true;
    state.Day = day;
    state.Now = now;
    state.Card.Show(state.Lines);
}
} // namespace

SkySunReadoutCard::SkySunReadoutCard(UIElement* parent)
{
    auto card = std::make_unique<UIElement>();
    EditorUI::StyleInfoCard(card.get());
    EditorUI::MarkInfoCardAlwaysVisible(card.get());
    card->SetTooltip(kCardTooltip);
    m_Card = card.get();
    m_Day = AddLine(m_Card);
    m_Noon = AddLine(m_Card);
    m_Now = AddLine(m_Card);
    m_Moon = AddLine(m_Card);
    parent->AddChild(std::move(card));
}

void SkySunReadoutCard::Show(const SkySunReadoutLines& lines)
{
    ShowLine(m_Day, lines.Day);
    ShowLine(m_Noon, lines.Noon);
    ShowLine(m_Now, lines.Now);
    ShowLine(m_Moon, lines.Moon);
}

void AddSkySunReadoutCard(UIElement* parent, const InspectorContext& ctx)
{
    ECS::World* w = ctx.World;
    const ECS::EntityHandle e = ctx.Entity;
    const auto* sky = w ? w->GetComponent<Components::SkyEnvironment>(e) : nullptr;
    if (!parent || !sky)
        return;

    auto state = std::make_shared<ReadoutState>(parent);
    Refresh(*state, *w, e, *sky);

    if (!ctx.SimulationRefreshCallbacks)
        return;
    ctx.SimulationRefreshCallbacks->push_back([getWorld = ctx.GetWorld, e, state]() {
        ECS::World* world = getWorld ? getWorld() : nullptr;
        const auto* current = world && world->IsValid(e) ? world->GetComponent<Components::SkyEnvironment>(e) : nullptr;
        if (!current)
            return;
        Refresh(*state, *world, e, *current);
    });
}

} // namespace GameEngine
