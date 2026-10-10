#include "Inspectors/SkyEnvironmentInspector.h"

#include "InspectorRegistry.h"
#include "Platform/SystemMetrics.h"

#include "Components/Rendering/Light.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"

#include "Inspectors/InspectorColorSwatchRow.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorComponentSection.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/SkyDayCurveRow.h"
#include "Inspectors/SkySunDriveRows.h"
#include "Inspectors/SkySunIlluminanceRow.h"
#include "Sky/SkyEnvironmentComponentTraits.h"
#include "Sky/SkyEnvironmentEdit.h"
#include "Sky/SkyPathGizmo.h"
#include "Inspectors/SkySunPathRows.h"
#include "Inspectors/SkySunReadoutCard.h"
#include "UI/EditorIcons.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/SkyDayKeyGradientBar.h"
#include "UI/Controls/Foldout.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

namespace GameEngine
{

namespace
{

constexpr float kMaxGroundPickerIntensity = 8.0f;
constexpr float kMinSkyTimeOfDayCycleSecondsField = 0.25f;
constexpr std::array<const char*, 4> kSkyDayKeyNames = {"Midnight", "Dawn", "Midday", "Sunset"};
// Lowercase stop captions shared by the gradient-bar rows and the scalar day-key rows.
// Distinct from kSkyDayKeyNames (PascalCase, used for undo change-labels).
constexpr std::array<const char*, 4> kSkyDayKeyCaptions = {"night", "dawn", "day", "sunset"};

// The three ambient-tint rows are built twice — once in the Gradient sky's own section and once
// in Physical's Stylistic section — for the same three fields. One copy of the help text, so the
// two cannot drift apart into describing the same control differently.
constexpr const char* kAmbientTintSkyTooltip =
    "Tints the up-facing diffuse ambient — the sky end of the gradient. Multiplied into the baked "
    "irradiance; specular reflections stay physical. Double-click the label to reset to white.";
constexpr const char* kAmbientTintEquatorTooltip =
    "Tints the horizon-facing diffuse ambient — the middle of the gradient. Double-click the label "
    "to reset to white.";
constexpr const char* kAmbientTintGroundTooltip =
    "Tints the down-facing diffuse ambient — the ground end of the gradient. Double-click the "
    "label to reset to white.";

// Default ground horizon night rim: sRGB #020305 (scene-linear).
constexpr float kDefaultGroundHorizonNightColorLinear[3] = {0.000607054f, 0.000910581f, 0.001517635f};
// Default matches the procedural night gradient horizon in sky_render.
constexpr float kDefaultNightSkyHorizonColorLinear[3] = {0.12f, 0.035f, 0.19f};

// Below-horizon bake-mode dropdown options (mirrors Rendering::SkyBelowHorizonMode).
constexpr EnumEntry<Rendering::SkyBelowHorizonMode> kBelowHorizonModes[] = {
    {Rendering::SkyBelowHorizonMode::ContinueHorizon, "Continue Horizon"},
    {Rendering::SkyBelowHorizonMode::PlanetGround, "Planet Ground"},
    {Rendering::SkyBelowHorizonMode::StylizedGround, "Stylized Ground"},
};

// Sky-type dropdown options (mirrors Components::SkyMode). Switching rebuilds the inspector so
// the physical vs gradient control sets show/hide.
constexpr EnumEntry<Components::SkyMode> kSkyModes[] = {
    {Components::SkyMode::Physical, "Physical (atmosphere)"},
    {Components::SkyMode::Gradient, "Gradient"},
};

static void LinearRgbToPickerState(const float* c, uint32_t& outArgb, float& outIntensity)
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    const float m = std::max(std::max(c[0], c[1]), c[2]);
    if (m <= 1e-6f)
    {
        outIntensity = 1.0f;
        outArgb = 0xFF000000u;
        return;
    }
    if (m <= 1.0f)
    {
        outIntensity = 1.0f;
        const uint8_t r = static_cast<uint8_t>(clamp01(c[0]) * 255.0f + 0.5f);
        const uint8_t g = static_cast<uint8_t>(clamp01(c[1]) * 255.0f + 0.5f);
        const uint8_t b = static_cast<uint8_t>(clamp01(c[2]) * 255.0f + 0.5f);
        outArgb = (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        return;
    }
    outIntensity = std::min(m, kMaxGroundPickerIntensity);
    const float inv = 1.0f / outIntensity;
    const uint8_t r = static_cast<uint8_t>(clamp01(c[0] * inv) * 255.0f + 0.5f);
    const uint8_t g = static_cast<uint8_t>(clamp01(c[1] * inv) * 255.0f + 0.5f);
    const uint8_t b = static_cast<uint8_t>(clamp01(c[2] * inv) * 255.0f + 0.5f);
    outArgb = (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static void PickerToLinearRgb(uint32_t argb, float intensity, float* out)
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    const float r = clamp01(static_cast<float>((argb >> 16) & 0xFF) / 255.0f);
    const float g = clamp01(static_cast<float>((argb >> 8) & 0xFF) / 255.0f);
    const float b = clamp01(static_cast<float>(argb & 0xFF) / 255.0f);
    const float i = std::max(1.0f, std::min(intensity, kMaxGroundPickerIntensity));
    out[0] = r * i;
    out[1] = g * i;
    out[2] = b * i;
}

static std::string FormatSkyInspectorFloat(float v, int decimals)
{
    if (v == 0.0f)
        return "0";

    std::ostringstream oss;
    oss.setf(std::ios::fixed, std::ios::floatfield);
    oss.precision(std::max(0, decimals));
    oss << v;

    std::string s = oss.str();
    const std::size_t dot = s.find('.');
    if (dot != std::string::npos)
    {
        std::size_t last = s.find_last_not_of('0');
        if (last == dot)
            --last;
        s.erase(last + 1);
    }
    if (s == "-0")
        return "0";
    return s;
}

using SkyDayColorAccessor = std::function<float* (Components::SkyEnvironment&, size_t)>;
using SkyDayTimeAccessor = std::function<float* (Components::SkyEnvironment&, size_t)>;
using SkyDayDefaultColors = std::array<std::array<float, 3>, 4>;

static SkyDayColorAccessor MakeSkyDayColorAccessor(Components::SkyVec3DayKeys Components::SkyEnvironment::*keysMember)
{
    return [keysMember](Components::SkyEnvironment& sky, size_t stopIndex) -> float*
    {
        Components::SkyVec3DayKeys& keys = sky.*keysMember;
        switch (stopIndex)
        {
            case 0: return keys.Midnight;
            case 1: return keys.Dawn;
            case 2: return keys.Midday;
            case 3: return keys.Sunset;
            default: return nullptr;
        }
    };
}

static SkyDayTimeAccessor MakeSkyDayTimeAccessor(Components::SkyScalarDayKeys Components::SkyEnvironment::*timesMember)
{
    return [timesMember](Components::SkyEnvironment& sky, size_t stopIndex) -> float*
    {
        Components::SkyScalarDayKeys& keys = sky.*timesMember;
        switch (stopIndex)
        {
            case 0: return &keys.Midnight;
            case 1: return &keys.Dawn;
            case 2: return &keys.Midday;
            case 3: return &keys.Sunset;
            default: return nullptr;
        }
    };
}

static SkyDayDefaultColors MakeSkyDayDefaultColors(const SkyDayColorAccessor& accessStopRgb)
{
    Components::SkyEnvironment defaults{};
    SkyDayDefaultColors colors{};
    for (size_t i = 0; i < colors.size(); ++i)
    {
        if (float* rgb = accessStopRgb(defaults, i))
        {
            colors[i][0] = rgb[0];
            colors[i][1] = rgb[1];
            colors[i][2] = rgb[2];
        }
    }
    return colors;
}

static float ClampSkyDayKeyHour(const Components::SkyScalarDayKeys& times, size_t stopIndex, float requestedHour)
{
    constexpr float kMinGapHours = 0.1f;
    constexpr float kMaxHour = 24.0f - 0.001f;
    const float hour = std::clamp(requestedHour, 0.0f, kMaxHour);
    if (stopIndex == 0)
        return std::clamp(hour, 0.0f, times.Dawn - kMinGapHours);
    if (stopIndex == 1)
        return std::clamp(hour, times.Midnight + kMinGapHours, times.Midday - kMinGapHours);
    if (stopIndex == 2)
        return std::clamp(hour, times.Dawn + kMinGapHours, times.Sunset - kMinGapHours);
    return std::clamp(hour, times.Midday + kMinGapHours, kMaxHour);
}

// Link `sun` as the sky's sun light (invalid clears the link) as one undo step, and rebuild the
// sky's section: which light the Sun illuminance row edits, or whether it is read-only, follows the
// link. The rebuild is posted by the panel when this runs inside a click.
static void LinkSkySunLight(ECS::World* w, ECS::EntityHandle sky, const std::vector<ECS::EntityHandle>& extras,
                            Editor::EditorChangeNotifications* n, Editor::UndoRedoService* undo,
                            ECS::EntityHandle sun)
{
    Editor::CommitSkyEnvironmentEdit(w, sky, extras, n, undo, "Change Sky Sun Light",
                              [sun](Components::SkyEnvironment& u) { u.SunLight = sun; });
    if (!n)
        return;
    Editor::EditorChangeNotifications::ComponentChangedEvent rebuild{};
    rebuild.world = w;
    rebuild.entity = sky;
    rebuild.componentType = ECS::GetComponentTypeId<Components::SkyEnvironment>();
    rebuild.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
    n->NotifyComponentChanged(rebuild);
}

// One of this component's sections, with the note that says what kind of knob lives in it. Section
// titles repeat across panels ("Physical" is not a word this inspector owns), so the key that
// remembers whether a section is open is scoped to the component rather than the title alone.
static Foldout* AddSkySection(UIElement* parent, std::string_view title, const char* note,
                              bool showNote)
{
    Foldout* section = InspectorUI::AddComponentSection(
        parent, "SkyEnvironment/" + std::string(title), title);
    if (section && note && showNote)
        InspectorUI::AddInfoCard(section->GetContentContainer(), note);
    return section;
}

static void AddSkyEnvironmentDayKeyGradientRow(
    UIElement* parent,
    const char* label,
    const char* tooltip,
    ECS::World* world,
    Editor::EditorChangeNotifications* notifications,
    Editor::UndoRedoService* undo,
    OpenColorPickerWindowFn openPicker,
    const std::shared_ptr<std::vector<ECS::EntityHandle>>& targets,
    const SkyDayColorAccessor& accessStopRgb,
    const SkyDayTimeAccessor& accessStopHour,
    const SkyDayDefaultColors& defaultColors,
    const char* changeLabel)
{
    if (!parent || !world || !targets || targets->empty())
        return;

    UIElement* row = InspectorUI::AddRow(parent);
    Label* rowLabel = InspectorUI::AddLabel(row, label, tooltip);
    if (rowLabel)
        rowLabel->AddClass("inspector-label-no-drag");
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->Overrides()
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::AlignItems, AlignItems::Stretch)
        .Set(Style::Gap, StyleLength::Px(4.0f));

    auto bar = std::make_unique<SkyDayKeyGradientBar>();
    bar->Overrides()
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MinWidth, StyleLength::Px(120.0f));
    SkyDayKeyGradientBar* barRaw = bar.get();
    fieldContainer->AddChild(std::move(bar));

    auto labelsRow = std::make_unique<UIElement>();
    labelsRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::FlexStart)
        .Set(Style::MinHeight, StyleLength::Px(12.0f));
    for (size_t i = 0; i < kSkyDayKeyCaptions.size(); ++i)
    {
        auto stopLabel = std::make_unique<Label>();
        stopLabel->SetText(kSkyDayKeyCaptions[i]);
        stopLabel->AddClass("inspector-text");
        stopLabel->AddClass("sky-day-key-label");
        stopLabel->Overrides()
            .Set(Style::FontSize, StyleLength::Px(10.0f))
            .Set(Style::FlexGrow, 1.0f)
            .Set(Style::MinWidth, StyleLength::Px(0.0f))
            .Set(Style::TextAlignProp, i == 0 ? TextAlign::Left : TextAlign::Center);
        labelsRow->AddChild(std::move(stopLabel));
    }
    fieldContainer->AddChild(std::move(labelsRow));

    auto accessColorCopy = std::make_shared<SkyDayColorAccessor>(accessStopRgb);
    auto accessTimeCopy = std::make_shared<SkyDayTimeAccessor>(accessStopHour);
    auto defaultColorsCopy = std::make_shared<SkyDayDefaultColors>(defaultColors);
    auto selectedStop = std::make_shared<size_t>(0);
    auto syncUi = [world, targets, barRaw, accessColorCopy, accessTimeCopy, selectedStop]() {
        if (!targets || targets->empty() || !barRaw)
            return;
        auto* comp = world->GetComponentForWrite<Components::SkyEnvironment>((*targets)[0]);
        if (!comp)
            return;
        for (size_t i = 0; i < 4; ++i)
        {
            float* rgb = (*accessColorCopy)(*comp, i);
            if (!rgb)
                continue;
            uint32_t argb = 0;
            float intensity = 1.0f;
            LinearRgbToPickerState(rgb, argb, intensity);
            barRaw->SetStopColor(i, argb);
            if (float* keyHour = (*accessTimeCopy)(*comp, i))
                barRaw->SetStopPosition(i, std::clamp(*keyHour / 24.0f, 0.0f, 1.0f));
        }
    };
    syncUi();

    if (rowLabel)
    {
        auto lastClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
        rowLabel->RegisterEventHandler(
            kEventMouseDown,
            [world, notifications, undo, targets, accessColorCopy, defaultColorsCopy, syncUi, changeLabel, lastClickTime](UIEvent& e) {
                if (e.Button != 0)
                    return;

                const auto now = std::chrono::steady_clock::now();
                const bool isDoubleClick =
                    lastClickTime->time_since_epoch().count() != 0 &&
                    (now - *lastClickTime) < GameEngine::Platform::GetDoubleClickInterval();
                *lastClickTime = now;
                if (!isDoubleClick || !targets || targets->empty())
                    return;

                const std::string resetLabel = std::string("Reset ") + changeLabel;
                auto applyDefaults = [accessColorCopy, defaultColorsCopy](Components::SkyEnvironment& env) {
                    for (size_t i = 0; i < defaultColorsCopy->size(); ++i)
                    {
                        float* rgb = (*accessColorCopy)(env, i);
                        if (!rgb)
                            continue;
                        rgb[0] = (*defaultColorsCopy)[i][0];
                        rgb[1] = (*defaultColorsCopy)[i][1];
                        rgb[2] = (*defaultColorsCopy)[i][2];
                    }
                };

                if (undo)
                {
                    const bool multi = targets->size() > 1u;
                    auto edit = multi
                        ? undo->BeginInteractiveEdit(
                              resetLabel.c_str(),
                              InspectorDrag::MakeMultiComponentSnapshotTarget<Components::SkyEnvironment>(
                                  world,
                                  (*targets)[0],
                                  std::vector<ECS::EntityHandle>(targets->begin() + 1, targets->end()),
                                  notifications,
                                  resetLabel.c_str()))
                        : undo->BeginInteractiveEdit(
                              resetLabel.c_str(),
                              InspectorDrag::MakeComponentSnapshotTarget<Components::SkyEnvironment>(
                                  world, (*targets)[0], notifications, resetLabel.c_str()));

                    for (ECS::EntityHandle h : *targets)
                    {
                        auto* c = world->GetComponent<Components::SkyEnvironment>(h);
                        if (!c)
                            continue;
                        Components::SkyEnvironment updated = *c;
                        applyDefaults(updated);
                        world->AddComponentImmediate(h, updated);
                    }
                    edit.Commit();
                    if (notifications)
                    {
                        for (ECS::EntityHandle h : *targets)
                            notifications->NotifyComponentCommit<Components::SkyEnvironment>(world, h);
                    }
                }
                else
                {
                    for (ECS::EntityHandle h : *targets)
                    {
                        auto* c = world->GetComponent<Components::SkyEnvironment>(h);
                        if (!c)
                            continue;
                        Components::SkyEnvironment updated = *c;
                        applyDefaults(updated);
                        Editor::CommitComponentUpdate(world, h, notifications, updated);
                    }
                }

                syncUi();
                e.Stop();
            });
    }

    barRaw->SetOnStopSelected([selectedStop, syncUi](size_t stopIndex) {
        *selectedStop = std::min(stopIndex, size_t{3});
        syncUi();
    });

    using OptEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
    auto keyTimeDragEdit = std::make_shared<OptEdit>();

    barRaw->SetOnStopPositionChanging(
        [world, notifications, undo, targets, accessTimeCopy, selectedStop, syncUi, keyTimeDragEdit](size_t stopIndex, float normalized) {
            const size_t stop = std::min(stopIndex, size_t{3});
            *selectedStop = stop;
            const float requestedHour = std::clamp(normalized, 0.0f, 1.0f) * 24.0f;

            if (!targets || targets->empty())
                return;

            if (undo && !(*keyTimeDragEdit))
            {
                const bool multi = targets->size() > 1u;
                *keyTimeDragEdit = multi
                    ? undo->BeginInteractiveEdit(
                          "Change Sky Day Key Time",
                          InspectorDrag::MakeMultiComponentSnapshotTarget<Components::SkyEnvironment>(
                              world,
                              (*targets)[0],
                              std::vector<ECS::EntityHandle>(targets->begin() + 1, targets->end()),
                              notifications,
                              "Change Sky Day Key Time"))
                    : undo->BeginInteractiveEdit(
                          "Change Sky Day Key Time",
                          InspectorDrag::MakeComponentSnapshotTarget<Components::SkyEnvironment>(
                              world, (*targets)[0], notifications, "Change Sky Day Key Time"));
            }

            auto applyOne = [accessTimeCopy, stop, requestedHour](Components::SkyEnvironment& env) {
                const float clamped = ClampSkyDayKeyHour(env.DayKeyTimesHours, stop, requestedHour);
                float* keyHour = (*accessTimeCopy)(env, stop);
                if (keyHour)
                    *keyHour = clamped;
            };

            if (*keyTimeDragEdit)
            {
                (*keyTimeDragEdit)->Preview([&] {
                    for (ECS::EntityHandle h : *targets)
                    {
                        auto* c = world->GetComponentForWrite<Components::SkyEnvironment>(h);
                        if (!c)
                            continue;
                        applyOne(*c);
                    }
                });
            }
            else
            {
                for (ECS::EntityHandle h : *targets)
                {
                    auto* c = world->GetComponent<Components::SkyEnvironment>(h);
                    if (!c)
                        continue;
                    Components::SkyEnvironment updated = *c;
                    applyOne(updated);
                    Editor::PreviewComponentUpdate(world, h, notifications, updated);
                }
            }
            syncUi();
        });

    barRaw->SetOnStopPositionChanged(
        [world, notifications, targets, accessTimeCopy, selectedStop, syncUi, keyTimeDragEdit](size_t stopIndex, float normalized) {
            const size_t stop = std::min(stopIndex, size_t{3});
            *selectedStop = stop;
            const float requestedHour = std::clamp(normalized, 0.0f, 1.0f) * 24.0f;

            auto applyOne = [accessTimeCopy, stop, requestedHour](Components::SkyEnvironment& env) {
                const float clamped = ClampSkyDayKeyHour(env.DayKeyTimesHours, stop, requestedHour);
                float* keyHour = (*accessTimeCopy)(env, stop);
                if (keyHour)
                    *keyHour = clamped;
            };

            if (*keyTimeDragEdit)
            {
                (*keyTimeDragEdit)->Preview([&] {
                    for (ECS::EntityHandle h : *targets)
                    {
                        auto* c = world->GetComponentForWrite<Components::SkyEnvironment>(h);
                        if (!c)
                            continue;
                        applyOne(*c);
                    }
                });
                (*keyTimeDragEdit)->Commit();
                keyTimeDragEdit->reset();
            }
            else
            {
                for (ECS::EntityHandle h : *targets)
                {
                    auto* c = world->GetComponent<Components::SkyEnvironment>(h);
                    if (!c)
                        continue;
                    Components::SkyEnvironment updated = *c;
                    applyOne(updated);
                    Editor::CommitComponentUpdate(world, h, notifications, updated);
                }
            }
            syncUi();
        });

    barRaw->SetOnStopActivated([world, notifications, undo, openPicker, targets, accessColorCopy, selectedStop, syncUi, changeLabel](size_t stopIndex) {
        *selectedStop = std::min(stopIndex, size_t{3});
        syncUi();
        if (!openPicker || !targets || targets->empty())
            return;

        auto* primaryComp = world->GetComponentForWrite<Components::SkyEnvironment>((*targets)[0]);
        if (!primaryComp)
            return;
        float* primaryRgb = (*accessColorCopy)(*primaryComp, *selectedStop);
        if (!primaryRgb)
            return;

        uint32_t currentArgb = 0;
        float currentIntensity = 1.0f;
        LinearRgbToPickerState(primaryRgb, currentArgb, currentIntensity);

        using Edit = Editor::UndoRedoService::InteractiveEdit;
        auto edit = std::make_shared<Edit>();
        if (undo)
        {
            std::string fullChangeLabel = std::string(changeLabel) + " (" + kSkyDayKeyNames[*selectedStop] + ")";
            const bool multi = targets->size() > 1u;
            *edit = multi
                        ? undo->BeginInteractiveEdit(
                              fullChangeLabel.c_str(),
                              InspectorDrag::MakeMultiComponentSnapshotTarget<Components::SkyEnvironment>(
                                  world,
                                  (*targets)[0],
                                  std::vector<ECS::EntityHandle>(targets->begin() + 1, targets->end()),
                                  notifications,
                                  fullChangeLabel.c_str()))
                        : undo->BeginInteractiveEdit(
                              fullChangeLabel.c_str(),
                              InspectorDrag::MakeComponentSnapshotTarget<Components::SkyEnvironment>(
                                  world, (*targets)[0], notifications, fullChangeLabel.c_str()));
        }

        ColorPickerCallbacks cbs;
        cbs.onValueChanging = [world, notifications, edit, targets, accessColorCopy, selectedStop, syncUi](uint32_t newArgb, float intensity) {
            const size_t stop = *selectedStop;
            if (*edit)
            {
                edit->Preview([&] {
                    for (ECS::EntityHandle h : *targets)
                    {
                        auto* c = world->GetComponentForWrite<Components::SkyEnvironment>(h);
                        if (!c)
                            continue;
                        float* rgb = (*accessColorCopy)(*c, stop);
                        if (!rgb)
                            continue;
                        PickerToLinearRgb(newArgb, intensity, rgb);
                    }
                });
            }
            else
            {
                for (ECS::EntityHandle h : *targets)
                {
                    auto* c = world->GetComponent<Components::SkyEnvironment>(h);
                    if (!c)
                        continue;
                    Components::SkyEnvironment updated = *c;
                    float* rgb = (*accessColorCopy)(updated, stop);
                    if (!rgb)
                        continue;
                    PickerToLinearRgb(newArgb, intensity, rgb);
                    Editor::PreviewComponentUpdate(world, h, notifications, updated);
                }
            }
            syncUi();
        };
        cbs.onApply = [world, notifications, edit, targets, accessColorCopy, selectedStop, syncUi](uint32_t newArgb, float intensity) {
            const size_t stop = *selectedStop;
            if (*edit)
            {
                edit->Preview([&] {
                    for (ECS::EntityHandle h : *targets)
                    {
                        auto* c = world->GetComponentForWrite<Components::SkyEnvironment>(h);
                        if (!c)
                            continue;
                        float* rgb = (*accessColorCopy)(*c, stop);
                        if (!rgb)
                            continue;
                        PickerToLinearRgb(newArgb, intensity, rgb);
                    }
                });
                edit->Commit();
            }
            else
            {
                for (ECS::EntityHandle h : *targets)
                {
                    auto* c = world->GetComponent<Components::SkyEnvironment>(h);
                    if (!c)
                        continue;
                    Components::SkyEnvironment updated = *c;
                    float* rgb = (*accessColorCopy)(updated, stop);
                    if (!rgb)
                        continue;
                    PickerToLinearRgb(newArgb, intensity, rgb);
                    Editor::CommitComponentUpdate(world, h, notifications, updated);
                }
            }
            syncUi();
        };
        cbs.onCancel = [edit, syncUi]() {
            if (*edit)
                edit->Cancel();
            syncUi();
        };
        openPicker(currentArgb, currentIntensity, std::move(cbs));
    });
}

bool ActivePipelineHasSkyRenderNode()
{
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return false;
    // RG renders through per-view pipeline instances (SceneViewRenderCoordinator);
    // there is no single active pipeline to introspect here. Assume a sky pass is
    // present (it renders via the view's own pipeline) rather than show a
    // guaranteed-false "no sky pass" warning. If a central/per-view introspection
    // accessor is added later, a real check can run here.
    return true;
}

} // namespace

void RegisterSkyEnvironmentInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* sky = ctx.World->GetComponent<Components::SkyEnvironment>(ctx.Entity);
        if (!sky)
        {
            InspectorUI::AddLine(ctx.Parent, "(Sky Environment missing)");
            return;
        }

        if (!ActivePipelineHasSkyRenderNode())
        {
            ctx.Parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
                "No SkyRender pass in the active render pipeline. "
                "Add one via the Render Pipeline inspector for the sky to be visible."));
        }

        // Capture ctx.GetWorld so callbacks that survive past the current
        // inspector tick can re-resolve the world at fire time.
        auto getWorld = ctx.GetWorld;
        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;
        using namespace InspectorDrag;
        auto extras = GetAdditionalEntities(ctx);
        auto allSkyTargets = std::make_shared<std::vector<ECS::EntityHandle>>();
        allSkyTargets->push_back(e);
        allSkyTargets->insert(allSkyTargets->end(), extras.begin(), extras.end());
        auto curvePlaybackScrubActive = std::make_shared<bool>(false);
        auto refreshTimeOfDayControls = std::make_shared<std::function<void()>>();
        auto refreshTimeOfDayControlsNow = [refreshTimeOfDayControls]() {
            if (refreshTimeOfDayControls && *refreshTimeOfDayControls)
                (*refreshTimeOfDayControls)();
        };

        // Shared single-color swatch row for a SkyEnvironment float[3] member (HDR-capable via the
        // picker-intensity helpers; double-click the label resets to white). Used by both the
        // ambient gradient tint (diffuse-irradiance wash) and the gradient-sky colors.
        using SkyColorMember = float (Components::SkyEnvironment::*)[3];
        auto addSkyColorRow = [w, e, n, undo, openPicker](
            UIElement* parent, const char* label, const char* tooltip, const char* changeLabel,
            SkyColorMember member)
        {
            auto formatRgb = [](const float* rgb) {
                return std::string("(") + FormatSkyInspectorFloat(rgb[0], 3) + ", " +
                       FormatSkyInspectorFloat(rgb[1], 3) + ", " + FormatSkyInspectorFloat(rgb[2], 3) + ")";
            };

            UIElement* row = InspectorUI::AddRow(parent);
            Label* colorLabel = InspectorUI::AddLabel(row, label, tooltip);
            if (colorLabel) colorLabel->AddClass("inspector-label-no-drag");
            UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
            fieldContainer->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(6.0f));

            uint32_t argb = 0xFFFFFFFFu;
            float unusedIntensity = 1.0f;
            if (auto* c = w->GetComponent<Components::SkyEnvironment>(e))
                LinearRgbToPickerState(c->*member, argb, unusedIntensity);

            auto swatch = std::make_unique<UIElement>();
            UIElement* swatchRaw = swatch.get();
            InspectorUI::StyleColorSwatch(swatchRaw, argb);
            fieldContainer->AddChild(std::move(swatch));

            auto rgbLabel = std::make_unique<Label>();
            rgbLabel->AddClass("inspector-text");
            if (auto* c = w->GetComponent<Components::SkyEnvironment>(e))
                rgbLabel->SetText(formatRgb(c->*member));
            rgbLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
            Label* rgbLabelRaw = rgbLabel.get();
            fieldContainer->AddChild(std::move(rgbLabel));

            auto updateUI = [swatchRaw, rgbLabelRaw, w, e, member, formatRgb]() {
                auto* c = w->GetComponent<Components::SkyEnvironment>(e);
                if (!c) return;
                uint32_t a = 0; float inten = 1.0f;
                LinearRgbToPickerState(c->*member, a, inten);
                InspectorUI::StyleColorSwatch(swatchRaw, a);
                rgbLabelRaw->SetText(formatRgb(c->*member));
            };

            auto clickHandler = [w, e, n, undo, openPicker, member, updateUI, changeLabel](UIEvent& ev) {
                if (ev.Button != 0) return;
                ev.Stop();
                if (!openPicker) return;
                auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
                if (!comp) return;
                uint32_t currentArgb = 0; float currentIntensity = 1.0f;
                LinearRgbToPickerState(comp->*member, currentArgb, currentIntensity);

                using Edit = Editor::UndoRedoService::InteractiveEdit;
                auto edit = std::make_shared<Edit>();
                if (undo)
                {
                    auto target = MakeComponentSnapshotTarget<Components::SkyEnvironment>(w, e, n, changeLabel);
                    *edit = undo->BeginInteractiveEdit(changeLabel, std::move(target));
                }
                ColorPickerCallbacks cbs;
                cbs.onValueChanging = [w, e, n, edit, member, updateUI](uint32_t newArgb, float intensity) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<Components::SkyEnvironment>(e);
                            if (c) PickerToLinearRgb(newArgb, intensity, c->*member);
                        });
                    }
                    else
                    {
                        auto* c = w->GetComponent<Components::SkyEnvironment>(e);
                        if (!c) return;
                        Components::SkyEnvironment updated = *c;
                        PickerToLinearRgb(newArgb, intensity, updated.*member);
                        Editor::PreviewComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                cbs.onApply = [w, e, n, edit, member, updateUI](uint32_t newArgb, float intensity) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<Components::SkyEnvironment>(e);
                            if (c) PickerToLinearRgb(newArgb, intensity, c->*member);
                        });
                        edit->Commit();
                    }
                    else
                    {
                        auto* c = w->GetComponent<Components::SkyEnvironment>(e);
                        if (!c) return;
                        Components::SkyEnvironment updated = *c;
                        PickerToLinearRgb(newArgb, intensity, updated.*member);
                        Editor::CommitComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                cbs.onCancel = [edit, updateUI]() {
                    if (*edit) edit->Cancel();
                    updateUI();
                };
                openPicker(currentArgb, currentIntensity, std::move(cbs));
            };

            swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
            rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);

            auto lastClick = std::make_shared<std::chrono::steady_clock::time_point>();
            if (colorLabel)
            {
                colorLabel->RegisterEventHandler(kEventMouseDown, [w, e, n, undo, member, updateUI, lastClick, changeLabel](UIEvent& ev) {
                    if (ev.Button != 0) return;
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastClick);
                    if (elapsed > std::chrono::milliseconds::zero() && elapsed < GameEngine::Platform::GetDoubleClickInterval())
                    {
                        ev.Stop();
                        CommitComponentWithUndo<Components::SkyEnvironment>(w, e, n, undo, changeLabel,
                            [member](Components::SkyEnvironment& u) { (u.*member)[0] = 1.0f; (u.*member)[1] = 1.0f; (u.*member)[2] = 1.0f; });
                        updateUI();
                        *lastClick = std::chrono::steady_clock::time_point{};
                        return;
                    }
                    *lastClick = now;
                });
            }
        };

        // Sky type selector (Physical atmosphere vs stylized Gradient). Switching rebuilds the
        // inspector so the two control sets show/hide (mirrors the BelowHorizon rebuild pattern).
        {
            auto* skyModeField = InspectorUI::AddEnumRow(
                ctx.Parent, "Sky Mode", kSkyModes, sky->Mode,
                "Physical: the full atmosphere model. Gradient: a stylized 3-color vertical gradient "
                "that is both the sky background and the ambient/reflection source.");
            skyModeField->SetOnValueChanged(
                [w, e, n, undo, extras](Components::SkyMode v) { Editor::SwitchSkyMode(w, e, extras, n, undo, v); });
        }

        // Gradient sky: show ONLY the gradient controls + the lighting knobs that still apply (IBL
        // scale, ambient tint). The early return hides every physical atmosphere / sun-disc / star /
        // moon / time-of-day control.
        if (sky->Mode == Components::SkyMode::Gradient)
        {
            InspectorUI::AddTextBlock(ctx.Parent, "Gradient sky", "inspector-section-subheader");
            addSkyColorRow(ctx.Parent, "Top color",
                "Sky color straight up (view direction +Y). Authored linear RGB; scaled by Sky intensity.",
                "Change Gradient Sky Top", &Components::SkyEnvironment::GradientSkyTopColor);
            addSkyColorRow(ctx.Parent, "Horizon color",
                "Sky color at the horizon (view direction level).",
                "Change Gradient Sky Horizon", &Components::SkyEnvironment::GradientSkyHorizonColor);
            addSkyColorRow(ctx.Parent, "Bottom color",
                "Sky color straight down (view direction -Y).",
                "Change Gradient Sky Bottom", &Components::SkyEnvironment::GradientSkyBottomColor);
            AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
                ctx.Parent, "Sky intensity (nits)", sky->GradientSkyIntensity, w, e, n, undo,
                "Change Gradient Sky Intensity",
                [](Components::SkyEnvironment& u, float v) { u.GradientSkyIntensity = std::max(0.0f, v); },
                Components::kDefaultGradientSkyIntensityNits,
                "Gradient luminance in nits on the 203-nit reference-white anchor. Scales both the "
                "visible dome and the gradient-derived ambient/reflections.",
                extras, 0.0f, 40000.0f);

            InspectorUI::AddTextBlock(ctx.Parent, "Lighting", "inspector-section-subheader");
            AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
                ctx.Parent, "IBL intensity", sky->IblIntensity, w, e, n, undo, "Change Sky IBL Intensity",
                [](Components::SkyEnvironment& u, float v) { u.IblIntensity = std::max(0.0f, v); },
                1.0f, "Multiplier on the gradient-derived ambient lighting and reflections.",
                extras, 0.0f, 4.0f);
            AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
                ctx.Parent, "IBL ground darkening", sky->IblLowerHemisphereDarkness, w, e, n, undo,
                "Change Sky IBL Ground Darkening",
                [](Components::SkyEnvironment& u, float v) { u.IblLowerHemisphereDarkness = std::clamp(v, 0.0f, 1.0f); },
                0.3f, "Darkens the lower hemisphere of the baked ambient only.",
                extras, 0.0f, 1.0f);

            InspectorUI::AddTextBlock(ctx.Parent, "Ambient gradient tint (diffuse only)", "inspector-section-subheader");
            addSkyColorRow(ctx.Parent, "Sky tint", kAmbientTintSkyTooltip,
                "Change Ambient Tint Sky", &Components::SkyEnvironment::AmbientTintSky);
            addSkyColorRow(ctx.Parent, "Equator tint", kAmbientTintEquatorTooltip,
                "Change Ambient Tint Equator", &Components::SkyEnvironment::AmbientTintEquator);
            addSkyColorRow(ctx.Parent, "Ground tint", kAmbientTintGroundTooltip,
                "Change Ambient Tint Ground", &Components::SkyEnvironment::AmbientTintGround);
            return;
        }

        // The physical sky's rows split by WHAT KIND OF KNOB they are, because the two kinds fail
        // differently and an author needs to know which one they are reaching for. A Physical row
        // describes the world or the camera in units that mean something outside this engine — the
        // hour, the sun's illuminance, a reflectance, an EV trim — and moving it stays consistent
        // with itself. A Stylistic row exists because a scene wants to look a certain way; it has
        // no physical referent and nothing will re-derive it.
        //
        // Stylistic does not mean fake: the levers in that section change the light going INTO the
        // atmosphere, and the scattering, extinction, ambient and fog that follow are the same
        // physical transport either way. That is why they are grouped rather than hidden.
        Foldout* physicalSection = AddSkySection(
            ctx.Parent, "Physical",
            "The world and the camera, in units that mean something outside this engine: the hour, "
            "the sun's illuminance, a reflectance, an exposure trim in stops. The defaults are the "
            "physically correct answer.",
            ctx.ShowInfoCards);
        Foldout* stylisticSection = AddSkySection(
            ctx.Parent, "Stylistic",
            "Art direction. Every lever here changes the light going INTO the atmosphere, so the "
            "scattering, the aerial perspective, the ambient and the fog all follow from it "
            "physically — a tinted sun gives a tinted sky, key light and shadow fill together, from "
            "one multiply at the source. Defaults are neutral, so an untouched scene renders the "
            "physical sky exactly.",
            ctx.ShowInfoCards);
        UIElement* physical = physicalSection ? physicalSection->GetContentContainer() : ctx.Parent;
        UIElement* stylistic = stylisticSection ? stylisticSection->GetContentContainer() : ctx.Parent;

        // Three sections after Stylistic, each a whole subject an author works on alone — the
        // night sky's stars, its meteors, and the knobs that only the 2D backdrop reads — so each
        // collapses on its own and remembers that it is collapsed. Creating them here, after the
        // Stylistic section, is what fixes their position after the whole Stylistic block: a row
        // appended to Stylistic further down still renders above them.
        //
        // Horizon rim and Below-horizon band stay text sub-headers inside Stylistic because they
        // are built inside a Below-horizon mode branch, and a section that appears and disappears
        // with a dropdown is the churn this split avoids. Ambient gradient tint stays because it
        // is a diffuse-only tint trio that belongs with the ambient rows it sits under.
        Foldout* fallingStarsSection = AddSkySection(
            ctx.Parent, "Falling stars",
            "Occasional meteors across the night sky. Visible only at night, and only while the "
            "star field is drawn.",
            ctx.ShowInfoCards);
        Foldout* starsSection = AddSkySection(
            ctx.Parent, "Stars",
            "The night star field: how many, how bright, how large, and how they twinkle. Faded "
            "out by day and by the moon.",
            ctx.ShowInfoCards);
        Foldout* backdrop2DSection = AddSkySection(
            ctx.Parent, "2D backdrop",
            "Sizes and offsets read ONLY by the orthographic 2D-backdrop path, for 2D games. "
            "Every row here is inert in a perspective scene view.",
            ctx.ShowInfoCards);
        UIElement* fallingStars = fallingStarsSection ? fallingStarsSection->GetContentContainer() : ctx.Parent;
        UIElement* stars = starsSection ? starsSection->GetContentContainer() : ctx.Parent;
        UIElement* backdrop2D = backdrop2DSection ? backdrop2DSection->GetContentContainer() : ctx.Parent;

        // The two ways to author the sun's colour, first in the section and next to each other,
        // because an author reaching for one wants to see the other. Everything below them in this
        // section restyles a part of the sky; these two restyle the light the whole sky is made of.
        const SkyDayColorAccessor sunTintAccess =
            MakeSkyDayColorAccessor(&Components::SkyEnvironment::SunTintKeys);
        const SkyDayTimeAccessor sunTintTimeAccess =
            MakeSkyDayTimeAccessor(&Components::SkyEnvironment::DayKeyTimesHours);
        AddSkyEnvironmentDayKeyGradientRow(
            stylistic, "Sun tint (stylistic)",
            "Tints the SUN above the atmosphere, over the day. White = off, and the sky is the "
            "physical one. The tinted light is what the atmosphere then scatters, so the dome, the "
            "aerial perspective, the key light, the ambient and the fog all take the color from "
            "one multiply at the source — this is not a grade over the finished sky. Warm the "
            "sunset key for an orange evening. The moon is never tinted (its disc is not), so the "
            "night keys do nothing once the moon owns the sky. Click a point to edit its color key.",
            w, n, undo, openPicker, allSkyTargets, sunTintAccess, sunTintTimeAccess,
            MakeSkyDayDefaultColors(sunTintAccess), "Change Sky Sun Tint");

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            stylistic, "Sun size", sky->SunSize, w, e, n, undo, "Change Sky Sun Size",
            [](Components::SkyEnvironment& u, float v) {
                u.SunSize = std::clamp(v, Components::kSunSizeMin, Components::kSunSizeMax);
            },
            1.0f,
            "How large the sun disk is DRAWN, over its physical 0.53 degree apparent diameter. "
            "1 = physical. This is a look dial, not a light: the key light comes from the linked "
            "directional light and the ambient/reflection capture leaves the disk out, so neither "
            "changes with it — but a larger disk does feed the bloom around the sun more, and the "
            "glare halo keeps the real sun's width. Also scales the sun in the 2D backdrop, which "
            "Sun size 2D trims further — but the 2D sun has a minimum size, so it only draws "
            "smaller than the physical sun once Sun size x Sun size 2D drops under about 0.74.",
            extras, Components::kSunSizeMin, Components::kSunSizeMax);

        // The whole section's escape hatch, on the header rather than as a button row: the actions
        // a section offers are chrome, and a stacked bar at the bottom of the block reads as one
        // more setting. Resets only what this section owns.
        if (stylisticSection)
        {
            InspectorUI::AddSectionHeaderMenu(
                stylisticSection, ctx.Window,
                {{"Reset to physical", EditorIcons::kReset, true, "",
                  [w, e, n, undo, extras] {
                      Editor::CommitSkyEnvironmentEdit(
                          w, e, extras, n, undo, "Reset Sky Stylistic Levers",
                          [](Components::SkyEnvironment& u) {
                              Components::SkyVec3DayKeysSetUniform(u.SunTintKeys, 1.0f, 1.0f, 1.0f);
                              u.SunSize = 1.0f;
                          });
                  }}});
        }

        {
            using OptEdit = std::optional<Editor::UndoRedoService::InteractiveEdit>;
            auto todEditPtr = std::make_shared<OptEdit>();
            const float todDefaultResetHours = sky->TimeOfDayHours;

            UIElement* todRow = InspectorUI::AddRow(physical);
            Label* todLabel = InspectorUI::AddLabel(
                todRow,
                "Time of Day",
                "Hour of day (0–24). Scrub continuously for in-between hours.");
            UIElement* todWrap = InspectorUI::AddFieldContainer(todRow);
            InspectorDrag::ApplySliderWithValueContainerStyle(todWrap);
            todWrap->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(4.0f));

            Toggle* animToggle = InspectorUI::AddToggle(todWrap, sky->AnimateTimeOfDay);
            animToggle->SetTooltip("Animate time.");
            animToggle->SetOnValueChanged([w, e, n, undo, extras](bool v) {
                if (undo)
                {
                    auto target =
                        extras.empty()
                            ? MakeComponentSnapshotTarget<Components::SkyEnvironment>(
                                  w,
                                  e,
                                  n,
                                  "Change Sky Animate Time Of Day")
                            : MakeMultiComponentSnapshotTarget<Components::SkyEnvironment>(
                                  w,
                                  e,
                                  extras,
                                  n,
                                  "Change Sky Animate Time Of Day");
                    auto edit = undo->BeginInteractiveEdit("Change Sky Animate Time Of Day", std::move(target));
                    auto* prim = w->GetComponent<Components::SkyEnvironment>(e);
                    if (!prim)
                    {
                        edit.Cancel();
                        return;
                    }
                    Components::SkyEnvironment updated = *prim;
                    updated.AnimateTimeOfDay = v;
                    w->AddComponentImmediate(e, updated);
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                        if (!c)
                            continue;
                        Components::SkyEnvironment u = *c;
                        u.AnimateTimeOfDay = v;
                        w->AddComponentImmediate(ex, u);
                    }
                    edit.Commit();
                    return;
                }

                auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
                if (!comp)
                    return;
                Components::SkyEnvironment updated = *comp;
                updated.AnimateTimeOfDay = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                    if (!c)
                        continue;
                    Components::SkyEnvironment u = *c;
                    u.AnimateTimeOfDay = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
            });

            auto wrapTimeHours = [](float rawHours) -> float
            { return WrapSkyTimeOfDayHours(rawHours); };

            Slider* todSnapSliderRaw = InspectorDrag::AddInspectorSlider(
                todWrap,
                wrapTimeHours(sky->TimeOfDayHours),
                0.0f,
                24.0f,
                0.0f,
                true,
                InspectorDrag::kInspectorWideSliderMinWidthPx);

            FloatField* todField = InspectorUI::AddFloat(todWrap, sky->TimeOfDayHours);
            InspectorDrag::ApplySliderFloatValueFieldStyle(todField);
            todField->SetFormatFunction([](float v) { return FormatSkyInspectorFloat(v, 3); });
            todField->SetValueWithoutNotify(sky->TimeOfDayHours);

            auto refreshTimeOfDayDependentControls = [
                frameCallbacks = ctx.FrameRefreshCallbacks,
                simCallbacks = ctx.SimulationRefreshCallbacks]()
            {
                auto runCallbacks = [](const std::vector<std::function<void()>>* callbacks)
                {
                    if (!callbacks)
                        return;
                    for (const auto& callback : *callbacks)
                    {
                        if (callback)
                            callback();
                    }
                };
                runCallbacks(frameCallbacks);
                runCallbacks(simCallbacks);
            };

            auto onTimeOfDayChanging = [
                w,
                e,
                n,
                undo,
                extras,
                todEditPtr,
                wrapTimeHours,
                todSnapSliderRaw,
                refreshTimeOfDayDependentControls](float rawHours)
            {
                auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
                if (!comp)
                    return;

                float wrappedHours = wrapTimeHours(rawHours);
                // Keep the slider thumb responsive while label-drag updates are applying.
                todSnapSliderRaw->SetValueWithoutNotify(wrappedHours);

                if (!todEditPtr->has_value() && undo)
                {
                    auto target =
                        extras.empty()
                            ? MakeComponentSnapshotTarget<Components::SkyEnvironment>(
                                  w,
                                  e,
                                  n,
                                  "Change Sky Time Of Day")
                            : MakeMultiComponentSnapshotTarget<Components::SkyEnvironment>(
                                  w,
                                  e,
                                  extras,
                                  n,
                                  "Change Sky Time Of Day");
                    todEditPtr->emplace(undo->BeginInteractiveEdit("Change Sky Time Of Day", std::move(target)));
                }

                Components::SkyEnvironment updated = *comp;
                updated.TimeOfDayHours = wrappedHours;

                if (todEditPtr->has_value() && todEditPtr->value())
                {
                    Components::SkyEnvironment captured = updated;
                    todEditPtr->value().Preview([w, e, extras, captured]()
                                                {
                                                    w->AddComponentImmediate(e, captured);
                                                    for (auto& ex : extras)
                                                    {
                                                        auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                                                        if (!c)
                                                            continue;
                                                        Components::SkyEnvironment u = *c;
                                                        u.TimeOfDayHours = captured.TimeOfDayHours;
                                                        w->AddComponentImmediate(ex, u);
                                                    }
                                                });
                    if (n)
                    {
                        n->NotifyComponentChange<Components::SkyEnvironment>(
                            w,
                            e,
                            Editor::EditorChangeNotifications::ChangeKind::Preview);
                        for (auto& ex : extras)
                            n->NotifyComponentChange<Components::SkyEnvironment>(
                                w,
                                ex,
                                Editor::EditorChangeNotifications::ChangeKind::Preview);
                    }
                }
                else
                {
                    Editor::PreviewComponentUpdate(w, e, n, updated);
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                        if (!c)
                            continue;
                        Components::SkyEnvironment u = *c;
                        u.TimeOfDayHours = wrappedHours;
                        Editor::PreviewComponentUpdate(w, ex, n, u);
                    }
                }

                refreshTimeOfDayDependentControls();
            };

            auto onTimeOfDayChanged = [
                w,
                e,
                n,
                undo,
                extras,
                todEditPtr,
                wrapTimeHours,
                todSnapSliderRaw,
                refreshTimeOfDayDependentControls](float rawHours)
            {
                auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
                if (!comp)
                    return;

                float wrappedHours = wrapTimeHours(rawHours);
                todSnapSliderRaw->SetValueWithoutNotify(wrappedHours);
                Components::SkyEnvironment updated = *comp;
                updated.TimeOfDayHours = wrappedHours;

                if (todEditPtr->has_value() && todEditPtr->value())
                {
                    w->AddComponentImmediate(e, updated);
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                        if (!c)
                            continue;
                        Components::SkyEnvironment u = *c;
                        u.TimeOfDayHours = wrappedHours;
                        w->AddComponentImmediate(ex, u);
                    }
                    todEditPtr->value().Commit();
                    if (n)
                    {
                        n->NotifyComponentCommit<Components::SkyEnvironment>(w, e);
                        for (auto& ex : extras)
                            n->NotifyComponentCommit<Components::SkyEnvironment>(w, ex);
                    }
                    todEditPtr->reset();
                }
                else
                {
                    CommitComponentWithUndo<Components::SkyEnvironment>(
                        w,
                        e,
                        n,
                        undo,
                        "Change Sky Time Of Day",
                        [updated](Components::SkyEnvironment& u) { u = updated; });

                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                        if (!c)
                            continue;
                        Components::SkyEnvironment u = *c;
                        u.TimeOfDayHours = wrappedHours;
                        Editor::CommitComponentUpdate(w, ex, n, u);
                    }
                }

                refreshTimeOfDayDependentControls();
            };

            todField->SetOnValueChanging(onTimeOfDayChanging);
            todField->SetOnValueChanged(onTimeOfDayChanged);

            auto applySlider = [todSnapSliderRaw, todField, wrapTimeHours, onTimeOfDayChanging, onTimeOfDayChanged](
                                   float rawFromSlider,
                                   bool isFinal) {
                const float wrapped = wrapTimeHours(rawFromSlider);
                todSnapSliderRaw->SetValueWithoutNotify(wrapped);
                todField->SetValue(wrapped);
                if (isFinal)
                    onTimeOfDayChanged(wrapped);
                else
                    onTimeOfDayChanging(wrapped);
            };
            todSnapSliderRaw->SetOnValueChanging([applySlider](const float& v) mutable { applySlider(v, false); });
            todSnapSliderRaw->SetOnValueChanged([applySlider](const float& v) mutable { applySlider(v, true); });

            *refreshTimeOfDayControls = [getWorld, e, todField, todSnapSliderRaw, todEditPtr]() {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world || !e.IsValid() || !world->IsValid(e) || !todField)
                    return;
                if (todEditPtr && todEditPtr->has_value())
                    return;

                auto* comp = world->GetComponent<Components::SkyEnvironment>(e);
                if (!comp)
                    return;

                UIManager* ui = todField->GetOwnerManager();
                static const std::string kEmptyFocusId;
                const std::string& focusId = ui ? ui->GetFocusedElementId() : kEmptyFocusId;
                if (InspectorUI::ContainsFocusedElement(todField, focusId) ||
                    InspectorUI::ContainsFocusedElement(todSnapSliderRaw, focusId))
                    return;

                if (std::fabs(todField->GetValue() - comp->TimeOfDayHours) > 0.001f)
                    todField->SetValueWithoutNotify(comp->TimeOfDayHours);
                if (todSnapSliderRaw &&
                    std::fabs(todSnapSliderRaw->GetValue() - comp->TimeOfDayHours) > 0.001f)
                    todSnapSliderRaw->SetValueWithoutNotify(comp->TimeOfDayHours);
            };

            if (ctx.SimulationRefreshCallbacks)
                ctx.SimulationRefreshCallbacks->push_back(refreshTimeOfDayControlsNow);

            auto changingCopy = onTimeOfDayChanging;
            auto changedCopy = onTimeOfDayChanged;
            SetupLabelDragFloat(
                todLabel,
                todField,
                [todField, todSnapSliderRaw, wrapTimeHours, changingCopy]()
                {
                    const float wrappedHours = wrapTimeHours(todField->GetValue());
                    todSnapSliderRaw->SetValueWithoutNotify(wrappedHours);
                    changingCopy(wrappedHours);
                },
                [todField, todSnapSliderRaw, wrapTimeHours, changedCopy]()
                {
                    const float wrappedHours = wrapTimeHours(todField->GetValue());
                    todSnapSliderRaw->SetValueWithoutNotify(wrappedHours);
                    changedCopy(wrappedHours);
                },
                todDefaultResetHours,
                0.0f,
                24.0f);

            auto dayStopHour = [](const Components::SkyEnvironment& env, int stopIndex) -> float
            {
                switch (std::clamp(stopIndex, 0, 3))
                {
                    case 0: return env.DayKeyTimesHours.Midnight;
                    case 1: return env.DayKeyTimesHours.Dawn;
                    case 2: return env.DayKeyTimesHours.Midday;
                    default: return env.DayKeyTimesHours.Sunset;
                }
            };
            auto nearestDayStop = [dayStopHour](const Components::SkyEnvironment& env, float hours) -> int
            {
                int best = 0;
                float bestDistance = std::numeric_limits<float>::max();
                for (int i = 0; i < 4; ++i)
                {
                    const float distance = std::fabs(hours - dayStopHour(env, i));
                    if (distance < bestDistance)
                    {
                        best = i;
                        bestDistance = distance;
                    }
                }
                return best;
            };
            auto setDayStopLabel = [](Label* label, int stopIndex)
            {
                if (label)
                    label->SetText(kSkyDayKeyNames[static_cast<size_t>(std::clamp(stopIndex, 0, 3))]);
            };

            UIElement* dayStopRow = InspectorUI::AddRow(physical);
            InspectorUI::AddLabel(
                dayStopRow,
                "Day time stop",
                "Snap Time of Day to one of the four day-key stops. The continuous slider above is unchanged.");
            UIElement* dayStopWrap = InspectorUI::AddFieldContainer(dayStopRow);
            dayStopWrap->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(8.0f));

            const int initialDayStop = nearestDayStop(*sky, wrapTimeHours(sky->TimeOfDayHours));
            Slider* dayStopSliderRaw = InspectorDrag::AddInspectorSlider(
                dayStopWrap,
                static_cast<float>(initialDayStop),
                0.0f,
                3.0f,
                1.0f,
                false,
                InspectorDrag::kInspectorWideSliderMinWidthPx);

            auto dayStopValueLabel = std::make_unique<Label>();
            dayStopValueLabel->AddClass("inspector-value");
            dayStopValueLabel->Overrides()
                .Set(Style::Width, StyleLength::Px(64.0f))
                .Set(Style::PaddingLeft, StyleLength::Px(8.0f))
                .Set(Style::PaddingRight, StyleLength::Px(8.0f));
            Label* dayStopValueLabelRaw = dayStopValueLabel.get();
            setDayStopLabel(dayStopValueLabelRaw, initialDayStop);
            dayStopWrap->AddChild(std::move(dayStopValueLabel));

            auto applyDayStop = [
                                    w,
                                    e,
                                    dayStopHour,
                                    setDayStopLabel,
                                    dayStopSliderRaw,
                                    dayStopValueLabelRaw,
                                    onTimeOfDayChanging,
                                    onTimeOfDayChanged](float rawStop,
                                                        bool isFinal) mutable
            {
                auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
                if (!comp)
                    return;

                const int stopIndex = std::clamp(static_cast<int>(std::round(rawStop)), 0, 3);
                dayStopSliderRaw->SetValueWithoutNotify(static_cast<float>(stopIndex));
                setDayStopLabel(dayStopValueLabelRaw, stopIndex);
                const float stopHour = dayStopHour(*comp, stopIndex);
                if (isFinal)
                    onTimeOfDayChanged(stopHour);
                else
                    onTimeOfDayChanging(stopHour);
            };
            dayStopSliderRaw->SetOnValueChanging([applyDayStop](const float& v) mutable { applyDayStop(v, false); });
            dayStopSliderRaw->SetOnValueChanged([applyDayStop](const float& v) mutable { applyDayStop(v, true); });

            if (ctx.SimulationRefreshCallbacks)
            {
                ctx.SimulationRefreshCallbacks->push_back([
                    getWorld,
                    e,
                    dayStopSliderRaw,
                    dayStopValueLabelRaw,
                    todEditPtr,
                    nearestDayStop,
                    setDayStopLabel]() {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world || !e.IsValid() || !world->IsValid(e) || !dayStopSliderRaw)
                        return;
                    if (todEditPtr && todEditPtr->has_value())
                        return;

                    auto* comp = world->GetComponent<Components::SkyEnvironment>(e);
                    if (!comp || !comp->AnimateTimeOfDay)
                        return;

                    const int stopIndex = nearestDayStop(*comp, comp->TimeOfDayHours);
                    if (std::fabs(dayStopSliderRaw->GetValue() - static_cast<float>(stopIndex)) > 0.001f)
                        dayStopSliderRaw->SetValueWithoutNotify(static_cast<float>(stopIndex));
                    setDayStopLabel(dayStopValueLabelRaw, stopIndex);
                });
            }
        }

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            physical,
            "Day cycle (seconds)",
            sky->TimeOfDayCycleSeconds,
            w,
            e,
            n,
            undo,
            "Change Sky Day Cycle Duration",
            [](Components::SkyEnvironment& u, float v)
            {
                u.TimeOfDayCycleSeconds = std::max(kMinSkyTimeOfDayCycleSecondsField, v);
            },
            sky->TimeOfDayCycleSeconds,
            "Seconds per full day.",
            extras);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            physical,
            "Sky Exposure Trim",
            sky->SkyExposureTrim,
            w,
            e,
            n,
            undo,
            "Change Sky Exposure Trim",
            [](Components::SkyEnvironment& u, float v) {
                u.SkyExposureTrim = std::clamp(v, -Components::kSkyExposureTrimLimitEv, Components::kSkyExposureTrimLimitEv);
            },
            sky->SkyExposureTrim,
            "Flat sky-vs-object/ambient trim in stops (+/-2). 0 = physical; the day/night arc is "
            "owned by the atmosphere + camera auto-exposure, not this knob.",
            extras);

        InspectorUI::AddTextBlock(physical, "Ground (below horizon)", "inspector-section-subheader");

        {
            auto* modeField = InspectorUI::AddEnumRow(
                physical, "Below Horizon", kBelowHorizonModes, sky->BelowHorizonMode,
                "How the sky is handled below the horizon:\n"
                "  Continue Horizon: the atmospheric horizon color continues smoothly down.\n"
                "  Planet Ground: a lit ground floor whose lit side tracks the sun.\n"
                "  Stylized Ground: the stylized ground/dark composite (legacy look).");
            modeField->SetOnValueChanged([w, e, n, undo, extras](Rendering::SkyBelowHorizonMode v) {
                CommitComponentWithUndo<Components::SkyEnvironment>(
                    w, e, n, undo, "Change Sky Below Horizon Mode",
                    [v](Components::SkyEnvironment& u) { u.BelowHorizonMode = v; });
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                    if (!c)
                        continue;
                    Components::SkyEnvironment u = *c;
                    u.BelowHorizonMode = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
                // The mode shows/hides its own knob sections (Ground Haze for Planet
                // Ground; the rim + band sections for Stylized Ground), so refresh the
                // inspector layout against the new mode.
                if (n)
                {
                    n->NotifyComponentChange<Components::SkyEnvironment>(
                        w, e, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
                    for (auto& ex : extras)
                        n->NotifyComponentChange<Components::SkyEnvironment>(
                            w, ex, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
                }
            });

            if (sky->BelowHorizonMode == Rendering::SkyBelowHorizonMode::PlanetGround)
            {
                AddComponentFloatRowWithDrag<Components::SkyEnvironment>(physical, "Ground Haze", sky->GroundHazeStrength, w, e, n,
                    undo, "Change Sky Ground Haze",
                    [](Components::SkyEnvironment& u, float v) { u.GroundHazeStrength = std::max(0.0f, v); },
                    sky->GroundHazeStrength, "Planet Ground only: how far the lit floor stays crisp before fading into the horizon haze (1 = physical).", extras);
            }
        }

        const SkyDayColorAccessor groundAlbedoAccess = MakeSkyDayColorAccessor(&Components::SkyEnvironment::GroundAlbedoKeys);
        const SkyDayColorAccessor groundNightAccess = MakeSkyDayColorAccessor(&Components::SkyEnvironment::GroundNightColorKeys);
        const SkyDayColorAccessor rimDayAccess = MakeSkyDayColorAccessor(&Components::SkyEnvironment::GroundHorizonColorKeys);
        const SkyDayColorAccessor rimNightAccess = MakeSkyDayColorAccessor(&Components::SkyEnvironment::GroundHorizonNightColorKeys);
        const SkyDayColorAccessor nightSkyHorizonAccess = MakeSkyDayColorAccessor(&Components::SkyEnvironment::NightSkyHorizonColorKeys);
        const SkyDayColorAccessor belowHorizonDarkAccess = MakeSkyDayColorAccessor(&Components::SkyEnvironment::BelowHorizonDarkColorKeys);
        const SkyDayTimeAccessor dayKeyTimeAccess = MakeSkyDayTimeAccessor(&Components::SkyEnvironment::DayKeyTimesHours);
        const SkyDayDefaultColors groundAlbedoDefaults = MakeSkyDayDefaultColors(groundAlbedoAccess);
        const SkyDayDefaultColors groundNightDefaults = MakeSkyDayDefaultColors(groundNightAccess);
        const SkyDayDefaultColors rimDayDefaults = MakeSkyDayDefaultColors(rimDayAccess);
        const SkyDayDefaultColors rimNightDefaults = MakeSkyDayDefaultColors(rimNightAccess);
        const SkyDayDefaultColors nightSkyHorizonDefaults = MakeSkyDayDefaultColors(nightSkyHorizonAccess);
        const SkyDayDefaultColors belowHorizonDarkDefaults = MakeSkyDayDefaultColors(belowHorizonDarkAccess);

        // The lit ground (albedo + brightness) is read in every Below Horizon mode: the baked
        // environment lighting carries the ground's bounce below the horizon (the ground floor in
        // Continue Horizon and Planet Ground, the authored composite in Stylized Ground; see
        // sky_capture_cube.frag), and Planet Ground and Stylized Ground also draw it as the visible
        // ground. So both rows are shown whatever the mode.
        AddSkyEnvironmentDayKeyGradientRow(
            physical, "Ground albedo ramp",
            "The ground's color through the day. It colors the light the ground bounces back up, which "
            "reaches every surface facing sideways or down, and the visible ground in Planet Ground and "
            "Stylized Ground. Click a point to edit its color key.",
            w, n, undo, openPicker, allSkyTargets,
            groundAlbedoAccess, dayKeyTimeAccess, groundAlbedoDefaults, "Change Sky Ground Albedo");

        AddSkyDayCurveRow(
            physical, "Ground brightness",
            &Components::SkyEnvironment::GroundBrightnessKeys,
            SkyDayCurveRowOptions{&Components::SkyEnvironment::GroundBrightnessShapeMode,
                                  &Components::SkyEnvironment::GroundBrightnessBezier},
            w, e, n, undo,
            "Change Sky Ground Brightness",
            [](float v) { return std::max(0.0f, v); },
            0.0f, 1.0f,
            "How bright the ground is through the day. It scales the light the ground bounces back up, which "
            "reaches every surface facing sideways or down, and the visible ground in Planet Ground and "
            "Stylized Ground.",
            extras, ctx.Window, ctx.FrameRefreshCallbacks, ctx.SimulationRefreshCallbacks,
            refreshTimeOfDayControlsNow, curvePlaybackScrubActive);

        // The night ground tint is read in every Below Horizon mode: the ground bounce mixes toward it
        // as night falls (GE_GroundBounceRadiance in sky_composite.glsl), in the baked environment
        // lighting of Continue Horizon and Planet Ground and in the visible Planet Ground floor, and
        // the Stylized Ground composite draws it. So the row is shown whatever the mode.
        AddSkyEnvironmentDayKeyGradientRow(
            stylistic, "Ground night ramp",
            "The ground's color at night. It replaces the lit ground color as night falls, in the light "
            "the ground bounces back up and in the visible ground in Planet Ground and Stylized Ground. "
            "Click a point to edit its color key.",
            w, n, undo, openPicker, allSkyTargets,
            groundNightAccess, dayKeyTimeAccess, groundNightDefaults, "Change Sky Ground Night");

        if (sky->BelowHorizonMode == Rendering::SkyBelowHorizonMode::StylizedGround)
        {
            AddSkyDayCurveRow(
                stylistic, "Horizon ground mix",
                &Components::SkyEnvironment::BelowHorizonBlendSharpnessKeys,
                SkyDayCurveRowOptions{&Components::SkyEnvironment::BelowHorizonBlendSharpnessShapeMode,
                                      &Components::SkyEnvironment::BelowHorizonBlendSharpnessBezier},
                w, e, n, undo,
                "Change Sky Below Horizon Blend",
                [](float v) { return std::max(0.001f, v); },
                0.0f, 4.0f,
                "Below-horizon blend across the day.", extras, ctx.Window, ctx.FrameRefreshCallbacks, ctx.SimulationRefreshCallbacks,
                refreshTimeOfDayControlsNow, curvePlaybackScrubActive);

            InspectorUI::AddTextBlock(stylistic, "Horizon rim (dark fringe under sky)", "inspector-section-subheader");

            AddSkyEnvironmentDayKeyGradientRow(
                stylistic, "Rim day ramp", "Click a point to edit its color key.", w, n, undo, openPicker, allSkyTargets,
                rimDayAccess, dayKeyTimeAccess, rimDayDefaults, "Change Sky Rim Day");

            AddSkyEnvironmentDayKeyGradientRow(
                stylistic, "Rim night ramp", "Click a point to edit its color key.", w, n, undo, openPicker, allSkyTargets,
                rimNightAccess, dayKeyTimeAccess, rimNightDefaults, "Change Sky Rim Night");
        }

        AddSkyEnvironmentDayKeyGradientRow(
            stylistic, "Night sky horizon ramp", "Click a point to edit its color key.", w, n, undo, openPicker, allSkyTargets,
            nightSkyHorizonAccess, dayKeyTimeAccess, nightSkyHorizonDefaults, "Change Sky Night Horizon");

        if (sky->BelowHorizonMode == Rendering::SkyBelowHorizonMode::StylizedGround)
        {
            AddSkyDayCurveRow(
                stylistic, "Rim width day",
                &Components::SkyEnvironment::GroundHorizonCosWidthKeys,
                SkyDayCurveRowOptions{&Components::SkyEnvironment::GroundHorizonCosWidthShapeMode,
                                      &Components::SkyEnvironment::GroundHorizonCosWidthBezier},
                w, e, n, undo,
                "Change Sky Rim Width Day",
                [](float v) { return std::clamp(v, 0.001f, 0.2f); },
                0.0f, 0.2f,
                "Day rim width across the day.", extras, ctx.Window, ctx.FrameRefreshCallbacks, ctx.SimulationRefreshCallbacks,
                refreshTimeOfDayControlsNow, curvePlaybackScrubActive);

            AddSkyDayCurveRow(
                stylistic, "Rim width night",
                &Components::SkyEnvironment::GroundHorizonNightCosWidthKeys,
                SkyDayCurveRowOptions{&Components::SkyEnvironment::GroundHorizonNightCosWidthShapeMode,
                                      &Components::SkyEnvironment::GroundHorizonNightCosWidthBezier},
                w, e, n, undo,
                "Change Sky Rim Width Night",
                [](float v) { return std::clamp(v, 0.001f, 0.2f); },
                0.0f, 0.2f,
                "Night rim width across the day.", extras, ctx.Window, ctx.FrameRefreshCallbacks, ctx.SimulationRefreshCallbacks,
                refreshTimeOfDayControlsNow, curvePlaybackScrubActive);

            InspectorUI::AddTextBlock(stylistic, "Below-horizon band (dark sky LUT)", "inspector-section-subheader");

            AddSkyEnvironmentDayKeyGradientRow(
                stylistic, "Horizon band ramp", "Click a point to edit its color key.", w, n, undo, openPicker, allSkyTargets,
                belowHorizonDarkAccess, dayKeyTimeAccess, belowHorizonDarkDefaults, "Change Sky Below Horizon Dark");

            AddSkyDayCurveRow(
                stylistic, "Horizon band strength",
                &Components::SkyEnvironment::BelowHorizonDarknessKeys,
                SkyDayCurveRowOptions{&Components::SkyEnvironment::BelowHorizonDarknessShapeMode,
                                      &Components::SkyEnvironment::BelowHorizonDarknessBezier},
                w, e, n, undo,
                "Change Sky Horizon Darkness",
                [](float v) { return std::clamp(v, 0.0f, 1.0f); },
                0.0f, 1.0f,
                "Below-horizon band strength across the day.", extras, ctx.Window, ctx.FrameRefreshCallbacks, ctx.SimulationRefreshCallbacks,
                refreshTimeOfDayControlsNow, curvePlaybackScrubActive);
        }

        auto addSkyFloatSlider =
            [w, e, n, undo, extras](
                UIElement* parent,
                const char* label,
                float value,
                float minValue,
                float maxValue,
                const char* undoLabel,
                float Components::SkyEnvironment::*field,
                const char* tooltip) {
                auto sliderRow = InspectorDrag::AddSliderWithFloatValueRow(
                    parent,
                    label,
                    std::clamp(value, minValue, maxValue),
                    minValue,
                    maxValue,
                    tooltip);
                Label* rowLabel = sliderRow.Label;
                Slider* slider = sliderRow.Slider;
                FloatField* valueField = sliderRow.ValueField;

                if (!slider || !valueField)
                    return;

                auto handlers = InspectorDrag::MakeComponentInteractiveHandlers<Components::SkyEnvironment, float>(
                    w,
                    e,
                    n,
                    undo,
                    undoLabel,
                    [field, minValue, maxValue](Components::SkyEnvironment& updated, float rawValue)
                    {
                        updated.*field = std::clamp(rawValue, minValue, maxValue);
                    },
                    extras);
                auto syncControls = [slider, valueField, minValue, maxValue](float rawValue) {
                    const float clamped = std::clamp(rawValue, minValue, maxValue);
                    slider->SetValueWithoutNotify(clamped);
                    valueField->SetValueWithoutNotify(clamped);
                };

                valueField->SetOnValueChanging(
                    [syncControls, preview = handlers.first](const float& v) mutable
                    {
                        syncControls(v);
                        preview(v);
                    });
                valueField->SetOnValueChanged(
                    [syncControls, commit = handlers.second](const float& v) mutable
                    {
                        syncControls(v);
                        commit(v);
                    });
                slider->SetOnValueChanging(
                    [syncControls, preview = handlers.first](const float& v) mutable
                    {
                        syncControls(v);
                        preview(v);
                    });
                slider->SetOnValueChanged(
                    [syncControls, commit = handlers.second](const float& v) mutable
                    {
                        syncControls(v);
                        commit(v);
                    });
                Components::SkyEnvironment defaults{};
                InspectorDrag::SetupLabelDragSlider(
                    rowLabel,
                    slider,
                    nullptr,
                    nullptr,
                    std::clamp(defaults.*field, minValue, maxValue));
            };

        addSkyFloatSlider(
            physical,
            "IBL intensity",
            sky->IblIntensity,
            0.0f,
            4.0f,
            "Change Sky IBL Intensity",
            &Components::SkyEnvironment::IblIntensity,
            "Multiplier on the sky's image-based ambient lighting and reflections. "
            "Lower it to tame an over-bright or over-blue sky fill.");

        addSkyFloatSlider(
            stylistic,
            "IBL ground darkening",
            sky->IblLowerHemisphereDarkness,
            0.0f,
            1.0f,
            "Change Sky IBL Ground Darkening",
            &Components::SkyEnvironment::IblLowerHemisphereDarkness,
            "Darkens the lower hemisphere of the baked IBL only, so surfaces stop being "
            "lit from below by the open sky. Leaves the visible sky unchanged. 0 = off, "
            "1 = fully dark below the horizon.");

        // Ambient gradient tint (diffuse-irradiance wash) — visible in Physical mode; the shared
        // addSkyColorRow helper is defined at the top of this inspector (Gradient mode shows the
        // same three rows in its own focused section).
        InspectorUI::AddTextBlock(stylistic, "Ambient gradient tint (diffuse only)", "inspector-section-subheader");
        addSkyColorRow(stylistic, "Sky tint", kAmbientTintSkyTooltip,
            "Change Ambient Tint Sky", &Components::SkyEnvironment::AmbientTintSky);
        addSkyColorRow(stylistic, "Equator tint", kAmbientTintEquatorTooltip,
            "Change Ambient Tint Equator", &Components::SkyEnvironment::AmbientTintEquator);
        addSkyColorRow(stylistic, "Ground tint", kAmbientTintGroundTooltip,
            "Change Ambient Tint Ground", &Components::SkyEnvironment::AmbientTintGround);

        // One Sun group: the light, its illuminance, the sun's path over the day, what it delivers
        // now, and the toggles for the light and the disc.
        InspectorUI::AddTextBlock(physical, "Sun", "inspector-section-subheader");
        InspectorUI::AddEntityFieldRow(
            physical, "Sun Light", sky->SunLight, w,
            [w, e, n, undo, extras](ECS::EntityHandle h) { LinkSkySunLight(w, e, extras, n, undo, h); },
            {ECS::GetComponentTypeId<Components::Light>()},
            "Link a directional light to two-way sync with the sun: rotating the light sets "
            "Time of Day (and the sun direction); changing Time of Day rotates the light.");

        AddSkySunDriveRows(physical, ctx);
        if (sky->SunIlluminanceSource == Components::SkySunIlluminanceSource::Light)
            AddSkySunIlluminanceRow(physical, ctx, [w, e, n, undo, extras](ECS::EntityHandle sun) {
                LinkSkySunLight(w, e, extras, n, undo, sun);
            });
        else
            AddSkySunCurveRows(physical, ctx, refreshTimeOfDayControlsNow, curvePlaybackScrubActive);

        AddSkySunPathRows(physical, ctx);
        AddSkySunReadoutCard(physical, ctx);

        auto addSkyDiskToggle = [w, e, n, undo, extras](
                                    UIElement* parent,
                                    const char* label,
                                    bool initial,
                                    bool Components::SkyEnvironment::*field,
                                    const char* undoLabel,
                                    const char* tooltip) {
            AddToggleRow(parent, label, initial,
                [w, e, n, undo, extras, field, undoLabel](bool v) {
                    Editor::CommitSkyEnvironmentEdit(w, e, extras, n, undo, undoLabel,
                                              [field, v](Components::SkyEnvironment& u) { u.*field = v; });
                },
                tooltip);
        };

        AddToggleRow(physical, "Auto Sun/Moon", sky->AutoSunMoon,
            [w, e, n, undo, extras](bool v) {
                if (undo)
                {
                    auto target = extras.empty()
                        ? MakeComponentSnapshotTarget<Components::SkyEnvironment>(w, e, n, "Change Sky Auto Sun Moon")
                        : MakeMultiComponentSnapshotTarget<Components::SkyEnvironment>(
                              w, e, extras, n, "Change Sky Auto Sun Moon");
                    auto edit = undo->BeginInteractiveEdit("Change Sky Auto Sun Moon", std::move(target));
                    auto* prim = w->GetComponent<Components::SkyEnvironment>(e);
                    if (!prim)
                    {
                        edit.Cancel();
                        return;
                    }
                    {
                        Components::SkyEnvironment updated = *prim;
                        updated.AutoSunMoon = v;
                        w->AddComponentImmediate(e, updated);
                    }
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                        if (!c) continue;
                        Components::SkyEnvironment u = *c;
                        u.AutoSunMoon = v;
                        w->AddComponentImmediate(ex, u);
                    }
                    edit.Commit();
                    return;
                }

                auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
                if (!comp) return;
                Components::SkyEnvironment updated = *comp;
                updated.AutoSunMoon = v;
                Editor::CommitComponentUpdate(w, e, n, updated);
                for (auto& ex : extras)
                {
                    auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                    if (!c) continue;
                    Components::SkyEnvironment u = *c;
                    u.AutoSunMoon = v;
                    Editor::CommitComponentUpdate(w, ex, n, u);
                }
            }, "Use Time of Day.");

        addSkyDiskToggle(physical, "Show Sun", sky->ShowSun, &Components::SkyEnvironment::ShowSun, "Change Sky Show Sun", "Show the visible sun disk.");

        InspectorUI::AddTextBlock(physical, "Moon", "inspector-section-subheader");
        addSkyDiskToggle(physical, "Show Moon", sky->ShowMoon, &Components::SkyEnvironment::ShowMoon, "Change Sky Show Moon", "Show the moon: the visible disk AND its night light contribution to the linked directional light.");
        AddSkyMoonlightRow(physical, ctx);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            physical,
            "Moon size",
            sky->MoonSize,
            w,
            e,
            n,
            undo,
            "Change Sky Moon Size",
            [](Components::SkyEnvironment& u, float v) { u.MoonSize = std::clamp(v, 0.1f, 6.0f); },
            sky->MoonSize,
            "Angular scale on the moon disk (~1 ≈ plausible real size); available with Auto Sun/Moon off for manual sun/moon setup.",
            extras,
            0.1f,
            6.0f);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            physical,
            "Moon exposure",
            sky->MoonExposureEV,
            w,
            e,
            n,
            undo,
            "Change Sky Moon Exposure",
            [](Components::SkyEnvironment& u, float v) { u.MoonExposureEV = std::clamp(v, -8.0f, 8.0f); },
            sky->MoonExposureEV,
            "Exposure offset applied only to the visible moon disk.",
            extras,
            -8.0f,
            8.0f);

        if (sky->AutoSunMoon)
        {
            AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
                physical,
                "Moon phase",
                sky->MoonPhase01,
                w,
                e,
                n,
                undo,
                "Change Sky Moon Phase",
                [](Components::SkyEnvironment& u, float v) { u.MoonPhase01 = std::clamp(v, 0.0f, 1.0f); },
                sky->MoonPhase01,
                "Visible moon illumination only: 0=new, 0.25=first quarter, 0.5=full, 0.75=last quarter.",
                extras,
                0.0f,
                1.0f);

            AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
                physical,
                "Moon arc position",
                sky->MoonArcPosition,
                w,
                e,
                n,
                undo,
                "Change Sky Moon Cycle",
                [](Components::SkyEnvironment& u, float v) { u.MoonArcPosition = std::clamp(v, 0.0f, 1.0f); },
                sky->MoonArcPosition,
                "Where the moon is in its movement cycle around the sky: 0=new position, 0.5=full-moon position.",
                extras,
                0.0f,
                1.0f);

            AddToggleRow(physical,
                "Auto moon cycle",
                sky->AutoMoonArc,
                [w, e, n, undo, extras](bool v) {
                    if (undo)
                    {
                        auto target = extras.empty()
                            ? MakeComponentSnapshotTarget<Components::SkyEnvironment>(w,
                                                                                       e,
                                                                                       n,
                                                                                       "Change Sky Auto Moon Cycle")
                            : MakeMultiComponentSnapshotTarget<Components::SkyEnvironment>(
                                  w,
                                  e,
                                  extras,
                                  n,
                                  "Change Sky Auto Moon Cycle");
                        auto edit = undo->BeginInteractiveEdit("Change Sky Auto Moon Cycle", std::move(target));
                        auto* prim = w->GetComponent<Components::SkyEnvironment>(e);
                        if (!prim)
                        {
                            edit.Cancel();
                            return;
                        }
                        {
                            Components::SkyEnvironment updated = *prim;
                            updated.AutoMoonArc = v;
                            w->AddComponentImmediate(e, updated);
                        }
                        for (auto& ex : extras)
                        {
                            auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                            if (!c) continue;
                            Components::SkyEnvironment u = *c;
                            u.AutoMoonArc = v;
                            w->AddComponentImmediate(ex, u);
                        }
                        edit.Commit();
                        return;
                    }

                    auto* comp = w->GetComponent<Components::SkyEnvironment>(e);
                    if (!comp) return;
                    Components::SkyEnvironment updated = *comp;
                    updated.AutoMoonArc = v;
                    Editor::CommitComponentUpdate(w, e, n, updated);
                    for (auto& ex : extras)
                    {
                        auto* c = w->GetComponent<Components::SkyEnvironment>(ex);
                        if (!c) continue;
                        Components::SkyEnvironment u = *c;
                        u.AutoMoonArc = v;
                        Editor::CommitComponentUpdate(w, ex, n, u);
                    }
                },
                "When Time of Day is animating, advance the moon movement cycle. This does not change the visible moon phase slider.");

            AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
                physical,
                "Moon cycle length",
                sky->MoonCycleDays,
                w,
                e,
                n,
                undo,
                "Change Sky Moon Cycle Days",
                [](Components::SkyEnvironment& u, float v) { u.MoonCycleDays = std::clamp(v, 1.0f, 365.0f); },
                sky->MoonCycleDays,
                "Simulated days for one full lunar cycle from New Moon to New Moon. Real lunar month is about 29.5 days.",
                extras,
                1.0f,
                365.0f);
        }

        addSkyDiskToggle(fallingStars, "Falling stars", sky->FallingStarsEnabled, &Components::SkyEnvironment::FallingStarsEnabled, "Change Sky Falling Stars", "Enable occasional falling stars in the night sky.");
        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            fallingStars,
            "Falling amount",
            sky->FallingStarAmount,
            w,
            e,
            n,
            undo,
            "Change Sky Falling Star Amount",
            [](Components::SkyEnvironment& u, float v) { u.FallingStarAmount = std::clamp(v, 0.0f, 1.0f); },
            sky->FallingStarAmount,
            "How many falling stars may appear at once.",
            extras,
            0.0f,
            1.0f);
        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            fallingStars,
            "Falling frequency",
            sky->FallingStarFrequency,
            w,
            e,
            n,
            undo,
            "Change Sky Falling Star Frequency",
            [](Components::SkyEnvironment& u, float v) { u.FallingStarFrequency = std::clamp(v, 0.0f, 4.0f); },
            sky->FallingStarFrequency,
            "How often falling stars streak across the night sky.",
            extras,
            0.0f,
            4.0f);
        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            fallingStars,
            "Falling speed",
            sky->FallingStarSpeed,
            w,
            e,
            n,
            undo,
            "Change Sky Falling Star Speed",
            [](Components::SkyEnvironment& u, float v) { u.FallingStarSpeed = std::clamp(v, 0.2f, 50.0f); },
            sky->FallingStarSpeed,
            "How fast each falling star crosses the sky.",
            extras,
            0.2f,
            50.0f);
        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            fallingStars,
            "Falling length",
            sky->FallingStarLength,
            w,
            e,
            n,
            undo,
            "Change Sky Falling Star Length",
            [](Components::SkyEnvironment& u, float v) { u.FallingStarLength = std::clamp(v, 0.2f, 4.0f); },
            sky->FallingStarLength,
            "How long each falling star trail is.",
            extras,
            0.2f,
            4.0f);
        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            fallingStars,
            "Falling thickness",
            sky->FallingStarThickness,
            w,
            e,
            n,
            undo,
            "Change Sky Falling Star Thickness",
            [](Components::SkyEnvironment& u, float v) { u.FallingStarThickness = std::clamp(v, 0.1f, 4.0f); },
            sky->FallingStarThickness,
            "How wide each falling star trail is.",
            extras,
            0.1f,
            4.0f);
        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            fallingStars,
            "Falling dot size",
            sky->FallingStarDotSize,
            w,
            e,
            n,
            undo,
            "Change Sky Falling Star Dot Size",
            [](Components::SkyEnvironment& u, float v) { u.FallingStarDotSize = std::clamp(v, 0.1f, 4.0f); },
            sky->FallingStarDotSize,
            "How large the bright falling star tip is.",
            extras,
            0.1f,
            4.0f);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(stars, "Star density", sky->StarDensity, w, e, n,
            undo, "Change Sky Star Density",
            [](Components::SkyEnvironment& u, float v) { u.StarDensity = std::clamp(v, 0.0f, 1.0f); },
            sky->StarDensity,
            "Star density.", extras);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(stars, "Brightness", sky->StarBrightness, w, e, n,
            undo, "Change Sky Star Brightness",
            [](Components::SkyEnvironment& u, float v) { u.StarBrightness = std::clamp(v, 0.0f, 2.0f); },
            sky->StarBrightness, "Star brightness.", extras);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(stars, "Size", sky->StarSize, w, e, n,
            undo, "Change Sky Star Size",
            [](Components::SkyEnvironment& u, float v) { u.StarSize = std::clamp(v, 0.1f, 3.0f); },
            sky->StarSize, "Star size.", extras);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(stars, "Diamond Shape", sky->StarDiamondShape, w, e, n,
            undo, "Change Sky Star Diamond Shape",
            [](Components::SkyEnvironment& u, float v) { u.StarDiamondShape = std::clamp(v, 0.0f, 1.0f); },
            sky->StarDiamondShape,
            "Diamond spike strength.", extras);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(stars, "Core Size", sky->StarCoreSize, w, e, n,
            undo, "Change Sky Star Core Size",
            [](Components::SkyEnvironment& u, float v) { u.StarCoreSize = std::clamp(v, 0.0f, 0.6f); },
            sky->StarCoreSize, "Star core size.", extras);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(stars, "Glow Falloff", sky->StarGlowFalloff, w, e, n,
            undo, "Change Sky Star Glow Falloff",
            [](Components::SkyEnvironment& u, float v) { u.StarGlowFalloff = std::clamp(v, 0.5f, 16.0f); },
            sky->StarGlowFalloff, "Star glow falloff.", extras);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(stars, "Twinkle Speed", sky->TwinkleSpeed, w, e, n,
            undo, "Change Sky Twinkle Speed",
            [](Components::SkyEnvironment& u, float v) { u.TwinkleSpeed = std::clamp(v, 0.0f, 3.0f); },
            sky->TwinkleSpeed, "Twinkle speed.", extras);

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(stars, "Twinkle Intensity", sky->TwinkleIntensity, w, e, n,
            undo, "Change Sky Twinkle Intensity",
            [](Components::SkyEnvironment& u, float v) { u.TwinkleIntensity = std::clamp(v, 0.0f, 1.0f); },
            sky->TwinkleIntensity, "Twinkle amount.", extras);

        if (!sky->AutoSunMoon)
        {
            AddComponentFloatRowWithDrag<Components::SkyEnvironment>(physical, "Sun Dir X", sky->SunDirOverride[0], w, e, n,
                undo, "Change Sky Sun Dir X",
                [](Components::SkyEnvironment& u, float v) { u.SunDirOverride[0] = v; },
                sky->SunDirOverride[0], "Sun direction X.", extras);
            AddComponentFloatRowWithDrag<Components::SkyEnvironment>(physical, "Sun Dir Y", sky->SunDirOverride[1], w, e, n,
                undo, "Change Sky Sun Dir Y",
                [](Components::SkyEnvironment& u, float v) { u.SunDirOverride[1] = v; },
                sky->SunDirOverride[1], "Sun direction Y.", extras);
            AddComponentFloatRowWithDrag<Components::SkyEnvironment>(physical, "Sun Dir Z", sky->SunDirOverride[2], w, e, n,
                undo, "Change Sky Sun Dir Z",
                [](Components::SkyEnvironment& u, float v) { u.SunDirOverride[2] = v; },
                sky->SunDirOverride[2], "Sun direction Z.", extras);
        }

        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            backdrop2D, "Sun size 2D", sky->SunSize2D, w, e, n, undo, "Change Sky 2D Sun Size",
            [](Components::SkyEnvironment& u, float v) { u.SunSize2D = std::clamp(v, 0.1f, 6.0f); },
            sky->SunSize2D, "Sun disk scale used only by orthographic/2D sky rendering.", extras, 0.1f, 6.0f);
        addSkyFloatSlider(
            backdrop2D,
            "Sky pan 2D", sky->SkyPan2D, -1.0f, 1.0f, "Change Sky 2D Pan",
            &Components::SkyEnvironment::SkyPan2D, "Vertical offset for the 2D sky and ground backdrop.");
        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            backdrop2D, "Moon size 2D", sky->MoonSize2D, w, e, n, undo, "Change Sky 2D Moon Size",
            [](Components::SkyEnvironment& u, float v) { u.MoonSize2D = std::clamp(v, 0.1f, 6.0f); },
            sky->MoonSize2D, "Moon disk scale used only by orthographic/2D sky rendering.", extras, 0.1f, 6.0f);
        AddComponentFloatRowWithDrag<Components::SkyEnvironment>(
            backdrop2D, "Falling dot size 2D", sky->FallingStarDotSize2D, w, e, n, undo, "Change Sky Falling Star Dot Size 2D",
            [](Components::SkyEnvironment& u, float v) { u.FallingStarDotSize2D = std::clamp(v, 0.1f, 4.0f); },
            sky->FallingStarDotSize2D, "How large the bright falling star tip is in 2D mode.", extras, 0.1f, 4.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::SkyEnvironment>(std::move(fn));
    Editor::RegisterSkyEnvironmentComponentTraits();
    Editor::RegisterSkyPathGizmo();
}

} // namespace GameEngine
