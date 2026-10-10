#include "Inspectors/HeightFogEffectInspector.h"

#include "InspectorRegistry.h"
#include "Platform/SystemMetrics.h"

#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffectPresets.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine
{

namespace
{

using namespace InspectorDrag;
using HF = Components::HeightFogEffect;
using Components::HeightFogAxisMode;
using Components::HeightFogGradientMode;
using Components::HeightFogLayerMode;
using Components::HeightFogPreset;

static constexpr EnumEntry<HeightFogPreset> kHeightFogPresets[] = {
    {HeightFogPreset::Custom, "Custom"},
    {HeightFogPreset::MorningHaze, "Morning Haze"},
    {HeightFogPreset::GroundMist, "Ground Mist"},
    {HeightFogPreset::MountainValley, "Mountain Valley"},
    {HeightFogPreset::HorizonPollution, "Horizon Pollution"},
    {HeightFogPreset::Day, "Day"},
    {HeightFogPreset::Night, "Night"},
};

static constexpr EnumEntry<HeightFogAxisMode> kHeightFogAxisModes[] = {
    {HeightFogAxisMode::WorldY, "World Y"},
    {HeightFogAxisMode::WorldX, "World X"},
    {HeightFogAxisMode::WorldZ, "World Z"},
    {HeightFogAxisMode::Custom, "Custom"},
};

static constexpr EnumEntry<HeightFogGradientMode> kHeightFogGradientModes[] = {
    {HeightFogGradientMode::None, "None"},
    {HeightFogGradientMode::Distance, "Distance"},
    {HeightFogGradientMode::Height, "Height"},
    {HeightFogGradientMode::ScreenY, "Screen Y"},
    {HeightFogGradientMode::MainLight, "Main Light"},
};

static constexpr EnumEntry<HeightFogLayerMode> kHeightFogLayerModes[] = {
    {HeightFogLayerMode::Dominant, "Dominant"},
    {HeightFogLayerMode::Additive, "Additive"},
};

static constexpr EnumEntry<Components::FogGlowQuality> kFogGlowQualities[] = {
    {Components::FogGlowQuality::Fast, "Fast (3x3)"},
    {Components::FogGlowQuality::Balanced, "Balanced"},
    {Components::FogGlowQuality::High, "High"},
    {Components::FogGlowQuality::Cinematic, "Cinematic"},
};

constexpr float kMaxFogColorPickerIntensity = 8.0f;
constexpr float kSwatchSize = 20.0f;
constexpr float kSwatchBorderRadius = 3.0f;
constexpr uint32_t kSwatchBorderColor = 0xFF555555u;

static void FogRgbToPickerState(const float* c, uint32_t& outArgb, float& outIntensity)
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    const float m = std::max(std::max(c[0], c[1]), c[2]);
    if (m <= 1e-6f)
    {
        outIntensity = 1.0f;
        outArgb = 0xFF000000u;
        return;
    }

    outIntensity = std::min(std::max(1.0f, m), kMaxFogColorPickerIntensity);
    const float inv = 1.0f / outIntensity;
    const uint8_t r = static_cast<uint8_t>(clamp01(c[0] * inv) * 255.0f + 0.5f);
    const uint8_t g = static_cast<uint8_t>(clamp01(c[1] * inv) * 255.0f + 0.5f);
    const uint8_t b = static_cast<uint8_t>(clamp01(c[2] * inv) * 255.0f + 0.5f);
    outArgb = (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static void PickerToFogRgb(uint32_t argb, float intensity, float* out)
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    const float i = std::min(std::max(1.0f, intensity), kMaxFogColorPickerIntensity);
    out[0] = clamp01(static_cast<float>((argb >> 16) & 0xFF) / 255.0f) * i;
    out[1] = clamp01(static_cast<float>((argb >> 8) & 0xFF) / 255.0f) * i;
    out[2] = clamp01(static_cast<float>(argb & 0xFF) / 255.0f) * i;
}

static std::string FormatFogRgb(const float* c)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", c[0], c[1], c[2]);
    return buf;
}

static void StyleFogSwatch(UIElement* swatch, uint32_t argb)
{
    if (!swatch)
        return;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(kSwatchSize))
        .Set(Style::Height, StyleLength::Px(kSwatchSize))
        .Set(Style::MinWidth, StyleLength::Px(kSwatchSize))
        .Set(Style::MinHeight, StyleLength::Px(kSwatchSize))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{kSwatchBorderRadius, kSwatchBorderRadius, kSwatchBorderRadius, kSwatchBorderRadius})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{kSwatchBorderColor, kSwatchBorderColor, kSwatchBorderColor, kSwatchBorderColor})
        .Set(Style::BackgroundColor, argb)
        .Set(Style::Cursor, CursorStyle::Pointer);
}

static void AddFogColorRow(
    UIElement* parent,
    const char* label,
    const char* tooltip,
    ECS::World* world,
    ECS::EntityHandle entity,
    Editor::EditorChangeNotifications* notifications,
    Editor::UndoRedoService* undo,
    OpenColorPickerWindowFn openPicker,
    float (HF::*member)[3],
    std::array<float, 3> defaultValue,
    const char* changeLabel)
{
    if (!parent || !world || !entity.IsValid())
        return;

    auto* effect = world->GetComponent<HF>(entity);
    if (!effect)
        return;

    UIElement* row = InspectorUI::AddRow(parent);
    Label* colorLabel = InspectorUI::AddLabel(row, label, tooltip);
    if (colorLabel)
        colorLabel->AddClass("inspector-label-no-drag");
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    uint32_t argb = 0;
    float intensity = 1.0f;
    FogRgbToPickerState(effect->*member, argb, intensity);

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    StyleFogSwatch(swatchRaw, argb);
    fieldContainer->AddChild(std::move(swatch));

    auto rgbLabel = std::make_unique<Label>();
    rgbLabel->AddClass("inspector-text");
    rgbLabel->SetText(FormatFogRgb(effect->*member));
    rgbLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    Label* rgbLabelRaw = rgbLabel.get();
    fieldContainer->AddChild(std::move(rgbLabel));

    // The picker's callbacks outlive an inspector rebuild: resolve the row through weak
    // refs instead of holding the freed widgets.
    auto updateUI = [world, entity, member, swatchRef = UIElement::MakeWeakRef(swatchRaw),
                     rgbLabelRef = UIElement::MakeWeakRef(rgbLabelRaw)]() {
        auto* c = world->GetComponent<HF>(entity);
        UIElement* swatch = swatchRef.Get();
        Label* rgbLabel = rgbLabelRef.Get();
        if (!c || !swatch || !rgbLabel)
            return;
        uint32_t nextArgb = 0;
        float nextIntensity = 1.0f;
        FogRgbToPickerState(c->*member, nextArgb, nextIntensity);
        StyleFogSwatch(swatch, nextArgb);
        rgbLabel->SetText(FormatFogRgb(c->*member));
    };

    auto clickHandler = [world, entity, notifications, undo, openPicker, member, updateUI, changeLabel](UIEvent& ev) {
        if (ev.Button != 0)
            return;
        ev.Stop();
        if (!openPicker)
            return;

        auto* comp = world->GetComponent<HF>(entity);
        if (!comp)
            return;

        uint32_t currentArgb = 0;
        float currentIntensity = 1.0f;
        FogRgbToPickerState(comp->*member, currentArgb, currentIntensity);

        using Edit = Editor::UndoRedoService::InteractiveEdit;
        auto edit = std::make_shared<Edit>();
        if (undo)
        {
            auto target = MakeComponentSnapshotTarget<HF>(world, entity, notifications, changeLabel);
            *edit = undo->BeginInteractiveEdit(changeLabel, std::move(target));
        }

        ColorPickerCallbacks cbs;
        cbs.onValueChanging = [world, entity, notifications, edit, member, updateUI](uint32_t newArgb, float newIntensity) {
            if (*edit)
            {
                edit->Preview([&] {
                    auto* c = world->GetComponentForWrite<HF>(entity);
                    if (c)
                        PickerToFogRgb(newArgb, newIntensity, c->*member);
                });
            }
            else
            {
                auto* c = world->GetComponent<HF>(entity);
                if (!c)
                    return;
                HF updated = *c;
                PickerToFogRgb(newArgb, newIntensity, updated.*member);
                Editor::PreviewComponentUpdate(world, entity, notifications, updated);
            }
            updateUI();
        };
        cbs.onApply = [world, entity, notifications, edit, member, updateUI](uint32_t newArgb, float newIntensity) {
            if (*edit)
            {
                edit->Preview([&] {
                    auto* c = world->GetComponentForWrite<HF>(entity);
                    if (c)
                        PickerToFogRgb(newArgb, newIntensity, c->*member);
                });
                edit->Commit();
            }
            else
            {
                auto* c = world->GetComponent<HF>(entity);
                if (!c)
                    return;
                HF updated = *c;
                PickerToFogRgb(newArgb, newIntensity, updated.*member);
                Editor::CommitComponentUpdate(world, entity, notifications, updated);
            }
            updateUI();
        };
        cbs.onCancel = [edit, updateUI]() {
            if (*edit)
                edit->Cancel();
            updateUI();
        };
        openPicker(currentArgb, currentIntensity, std::move(cbs));
    };

    swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
    rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);

    if (colorLabel)
    {
        auto lastClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
        colorLabel->RegisterEventHandler(kEventMouseDown, [world, entity, notifications, undo, member, defaultValue, changeLabel, updateUI, lastClickTime](UIEvent& ev) {
            if (ev.Button != 0)
                return;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastClickTime);
            if (elapsed > std::chrono::milliseconds::zero() && elapsed < GameEngine::Platform::GetDoubleClickInterval())
            {
                ev.Stop();
                CommitComponentWithUndo<HF>(world, entity, notifications, undo, changeLabel,
                    [member, defaultValue](HF& u) {
                        (u.*member)[0] = defaultValue[0];
                        (u.*member)[1] = defaultValue[1];
                        (u.*member)[2] = defaultValue[2];
                    });
                *lastClickTime = std::chrono::steady_clock::time_point{};
                updateUI();
                return;
            }
            *lastClickTime = now;
        });
    }
}

template<typename MutateFn>
void AddIndexedFloatRow(
    UIElement* parent,
    const char* label,
    float currentValue,
    ECS::World* world,
    ECS::EntityHandle entity,
    Editor::EditorChangeNotifications* notifications,
    Editor::UndoRedoService* undo,
    const char* undoLabel,
    MutateFn&& mutate,
    float dragStep,
    const char* tooltip,
    float minValue = -100000.0f,
    float maxValue = 100000.0f)
{
    AddComponentFloatRowWithDrag<HF>(parent, label, currentValue, world, entity, notifications,
        undo, undoLabel,
        [mutate = std::forward<MutateFn>(mutate), minValue, maxValue](HF& u, float v) {
            mutate(u, std::clamp(v, minValue, maxValue));
        },
        dragStep, tooltip, {}, minValue, maxValue);
}

} // namespace

void RegisterHeightFogEffectInspector()
{
    using namespace InspectorDrag;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<HF>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        auto* presetField = InspectorUI::AddEnumRow(ctx.Parent, "Preset", kHeightFogPresets, effect->Preset,
            "Apply a starting fog configuration.");
        presetField->SetOnValueChanged([w, e, n, undo](HeightFogPreset preset) {
            CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Preset",
                [preset](HF& u) {
                    if (preset == HeightFogPreset::Custom)
                    {
                        u.Preset = HeightFogPreset::Custom;
                        return;
                    }
                    Components::ApplyHeightFogPreset(u, preset);
                });
        });

        AddToggleRow(ctx.Parent, "Time Of Day", effect->UseTimeOfDay,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Time Of Day",
                    [v](HF& u) { u.UseTimeOfDay = v; });
            },
            "Interpolate between Day and Night presets using SkyEnvironment time.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Intensity", effect->Intensity, w, e, n,
            undo, "Change Height Fog Intensity",
            [](HF& u, float v) { u.Intensity = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.05f, "0 = off, 1 = full fog blend.",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Density", effect->Density, w, e, n,
            undo, "Change Height Fog Density",
            [](HF& u, float v) { u.Density = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Artist-scaled fog thickness.",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Max Opacity", effect->MaxOpacity, w, e, n,
            undo, "Change Height Fog Max Opacity",
            [](HF& u, float v) { u.MaxOpacity = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Caps the final fog opacity.",
            {}, 0.0f, 1.0f);

        InspectorUI::AddTextBlock(ctx.Parent, "Distance Fog", "inspector-section-subheader");

        AddToggleRow(ctx.Parent, "Distance Fog", effect->DistanceFogEnabled,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Distance Enabled",
                    [v](HF& u) { u.DistanceFogEnabled = v; u.Preset = HeightFogPreset::Custom; });
            },
            "Enable camera-distance fog ramp.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Min Distance", effect->MinDistance, w, e, n,
            undo, "Change Height Fog Min Distance",
            [](HF& u, float v) { u.MinDistance = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            0.5f, "Distance from camera before fog starts.",
            {}, 0.0f, 10000.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Smooth Length", effect->SmoothLength, w, e, n,
            undo, "Change Height Fog Smooth Length",
            [](HF& u, float v) { u.SmoothLength = std::max(v, 0.0001f); u.Preset = HeightFogPreset::Custom; },
            1.0f, "Near-camera fade-in distance.",
            {}, 0.0001f, 10000.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Max Distance", effect->MaxDistance, w, e, n,
            undo, "Change Height Fog Max Distance",
            [](HF& u, float v) { u.MaxDistance = std::max(v, 0.01f); u.Preset = HeightFogPreset::Custom; },
            10.0f, "Maximum fog ray distance.",
            {}, 0.01f, 100000.0f);

        auto* layerModeField = InspectorUI::AddEnumRow(ctx.Parent, "Layer Mode", kHeightFogLayerModes, effect->LayerMode,
            "How distance and height optical depth combine.");
        layerModeField->SetOnValueChanged([w, e, n, undo](HeightFogLayerMode mode) {
            CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Layer Mode",
                [mode](HF& u) { u.LayerMode = mode; u.Preset = HeightFogPreset::Custom; });
        });

        InspectorUI::AddTextBlock(ctx.Parent, "Height Fog", "inspector-section-subheader");

        AddToggleRow(ctx.Parent, "Height Fog", effect->HeightFogEnabled,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Height Enabled",
                    [v](HF& u) { u.HeightFogEnabled = v; u.Preset = HeightFogPreset::Custom; });
            },
            "Enable height-based fog falloff.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Base Height", effect->BaseHeight, w, e, n,
            undo, "Change Height Fog Base Height",
            [](HF& u, float v) { u.BaseHeight = v; u.Preset = HeightFogPreset::Custom; },
            0.5f, "World height where fog begins along the selected axis.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Transition Length", effect->TransitionLength, w, e, n,
            undo, "Change Height Fog Transition Length",
            [](HF& u, float v) { u.TransitionLength = std::max(v, 0.01f); u.Preset = HeightFogPreset::Custom; },
            1.0f, "How quickly fog increases with height.",
            {}, 0.01f, 10000.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Horizon Height Offset", effect->HorizonHeightOffset, w, e, n,
            undo, "Change Height Fog Horizon Height Offset",
            [](HF& u, float v) { u.HorizonHeightOffset = v; u.Preset = HeightFogPreset::Custom; },
            1.0f, "Raises or lowers the effective fog base toward the horizon.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Horizon Blend Start", effect->HorizonHeightBlendStart, w, e, n,
            undo, "Change Height Fog Horizon Blend Start",
            [](HF& u, float v) { u.HorizonHeightBlendStart = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            10.0f, "Distance where horizon height offset begins.",
            {}, 0.0f, 100000.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Horizon Blend End", effect->HorizonHeightBlendEnd, w, e, n,
            undo, "Change Height Fog Horizon Blend End",
            [](HF& u, float v) { u.HorizonHeightBlendEnd = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            10.0f, "Distance where horizon height offset reaches full strength.",
            {}, 0.0f, 100000.0f);

        auto* axisField = InspectorUI::AddEnumRow(ctx.Parent, "Fog Axis", kHeightFogAxisModes, effect->AxisMode,
            "Axis used for height fog and sky fill.");
        axisField->SetOnValueChanged([w, e, n, undo](HeightFogAxisMode mode) {
            CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Axis",
                [mode](HF& u) { u.AxisMode = mode; u.Preset = HeightFogPreset::Custom; });
        });

        if (effect->AxisMode == HeightFogAxisMode::Custom)
        {
            AddIndexedFloatRow(ctx.Parent, "Custom Axis X", effect->CustomAxis[0], w, e, n, undo,
                "Change Height Fog Custom Axis X",
                [](HF& u, float v) { u.CustomAxis[0] = v; u.Preset = HeightFogPreset::Custom; },
                0.01f, "Custom height fog axis.");
            AddIndexedFloatRow(ctx.Parent, "Custom Axis Y", effect->CustomAxis[1], w, e, n, undo,
                "Change Height Fog Custom Axis Y",
                [](HF& u, float v) { u.CustomAxis[1] = v; u.Preset = HeightFogPreset::Custom; },
                0.01f, "Custom height fog axis.");
            AddIndexedFloatRow(ctx.Parent, "Custom Axis Z", effect->CustomAxis[2], w, e, n, undo,
                "Change Height Fog Custom Axis Z",
                [](HF& u, float v) { u.CustomAxis[2] = v; u.Preset = HeightFogPreset::Custom; },
                0.01f, "Custom height fog axis.");
        }

        InspectorUI::AddTextBlock(ctx.Parent, "Color", "inspector-section-subheader");

        AddFogColorRow(ctx.Parent, "Emissive", "Base fog color", w, e, n, undo, ctx.OpenColorPickerWindow,
            &HF::Emissive, {0.48f, 0.54f, 0.60f}, "Change Height Fog Emissive");

        auto* gradientField = InspectorUI::AddEnumRow(ctx.Parent, "Gradient Mode", kHeightFogGradientModes, effect->GradientMode,
            "Dual-color fog gradient driver.");
        gradientField->SetOnValueChanged([w, e, n, undo](HeightFogGradientMode mode) {
            CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Gradient Mode",
                [mode](HF& u) { u.GradientMode = mode; u.Preset = HeightFogPreset::Custom; });
        });

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Gradient Strength", effect->GradientStrength, w, e, n,
            undo, "Change Height Fog Gradient Strength",
            [](HF& u, float v) { u.GradientStrength = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.05f, "Blend weight between low and high fog colors.",
            {}, 0.0f, 1.0f);

        AddFogColorRow(ctx.Parent, "Gradient Low", "Near / low fog tint", w, e, n, undo, ctx.OpenColorPickerWindow,
            &HF::GradientLowColor, {0.48f, 0.54f, 0.60f}, "Change Height Fog Gradient Low");

        AddFogColorRow(ctx.Parent, "Gradient High", "Far / high fog tint", w, e, n, undo, ctx.OpenColorPickerWindow,
            &HF::GradientHighColor, {0.72f, 0.78f, 0.85f}, "Change Height Fog Gradient High");

        InspectorUI::AddTextBlock(ctx.Parent, "Directional Light", "inspector-section-subheader");

        AddToggleRow(ctx.Parent, "Track Sun Light", effect->TrackDirectionalLight,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Sun Tracking",
                    [v](HF& u) { u.TrackDirectionalLight = v; u.Preset = HeightFogPreset::Custom; });
            },
            "Use the scene directional light for fog sun direction, color, and intensity.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Sun Intensity Scale", effect->SunIntensityScale, w, e, n,
            undo, "Change Height Fog Sun Intensity Scale",
            [](HF& u, float v) { u.SunIntensityScale = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            0.05f, "Multiplier applied when tracking the directional light.",
            {}, 0.0f, 8.0f);

        if (!effect->TrackDirectionalLight)
        {
            AddIndexedFloatRow(ctx.Parent, "Sun Dir X", effect->SunDirection[0], w, e, n, undo,
                "Change Height Fog Sun Direction X",
                [](HF& u, float v) { u.SunDirection[0] = v; u.Preset = HeightFogPreset::Custom; },
                0.01f, "Manual sun direction.");
            AddIndexedFloatRow(ctx.Parent, "Sun Dir Y", effect->SunDirection[1], w, e, n, undo,
                "Change Height Fog Sun Direction Y",
                [](HF& u, float v) { u.SunDirection[1] = v; u.Preset = HeightFogPreset::Custom; },
                0.01f, "Manual sun direction.");
            AddIndexedFloatRow(ctx.Parent, "Sun Dir Z", effect->SunDirection[2], w, e, n, undo,
                "Change Height Fog Sun Direction Z",
                [](HF& u, float v) { u.SunDirection[2] = v; u.Preset = HeightFogPreset::Custom; },
                0.01f, "Manual sun direction.");
        }

        AddFogColorRow(ctx.Parent, "Sun Color", "Sun in-scatter tint", w, e, n, undo, ctx.OpenColorPickerWindow,
            &HF::SunColor, {1.0f, 0.82f, 0.58f}, "Change Height Fog Sun Color");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Sun Intensity", effect->SunIntensity, w, e, n,
            undo, "Change Height Fog Sun Intensity",
            [](HF& u, float v) { u.SunIntensity = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            0.05f, "Sun in-scatter strength when not tracking a light.",
            {}, 0.0f, 8.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Phase", effect->Phase, w, e, n,
            undo, "Change Height Fog Phase",
            [](HF& u, float v) { u.Phase = std::clamp(v, -0.95f, 0.95f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Henyey-Greenstein anisotropy for sun scattering.",
            {}, -0.95f, 0.95f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Phase Weight 0", effect->PhaseWeight0, w, e, n,
            undo, "Change Height Fog Phase Weight 0",
            [](HF& u, float v) { u.PhaseWeight0 = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Primary HG lobe weight.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Phase Weight 1", effect->PhaseWeight1, w, e, n,
            undo, "Change Height Fog Phase Weight 1",
            [](HF& u, float v) { u.PhaseWeight1 = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Secondary multi-lobe weight.");

        InspectorUI::AddTextBlock(ctx.Parent, "Noise", "inspector-section-subheader");

        AddToggleRow(ctx.Parent, "Noise", effect->NoiseEnabled,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Noise",
                    [v](HF& u) { u.NoiseEnabled = v; u.Preset = HeightFogPreset::Custom; });
            },
            "Apply animated 3D procedural density variation.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Noise Scale", effect->NoiseScale, w, e, n,
            undo, "Change Height Fog Noise Scale",
            [](HF& u, float v) { u.NoiseScale = std::max(v, 0.01f); u.Preset = HeightFogPreset::Custom; },
            1.0f, "World scale of procedural fog noise.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Noise Strength", effect->NoiseStrength, w, e, n,
            undo, "Change Height Fog Noise Strength",
            [](HF& u, float v) { u.NoiseStrength = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Amount of noise modulation.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Noise Contrast", effect->NoiseContrast, w, e, n,
            undo, "Change Height Fog Noise Contrast",
            [](HF& u, float v) { u.NoiseContrast = std::max(v, 0.05f); u.Preset = HeightFogPreset::Custom; },
            0.05f, "Contrast of the noise pattern.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Noise Min", effect->NoiseMin, w, e, n,
            undo, "Change Height Fog Noise Min",
            [](HF& u, float v) { u.NoiseMin = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Lower bound for remapping noise density.",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Noise Max", effect->NoiseMax, w, e, n,
            undo, "Change Height Fog Noise Max",
            [](HF& u, float v) { u.NoiseMax = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Upper bound for remapping noise density.",
            {}, 0.0f, 1.0f);

        AddIndexedFloatRow(ctx.Parent, "Noise Vel X", effect->NoiseVelocity[0], w, e, n, undo,
            "Change Height Fog Noise Velocity X",
            [](HF& u, float v) { u.NoiseVelocity[0] = v; u.Preset = HeightFogPreset::Custom; },
            0.01f, "Animated noise scroll speed.");
        AddIndexedFloatRow(ctx.Parent, "Noise Vel Y", effect->NoiseVelocity[1], w, e, n, undo,
            "Change Height Fog Noise Velocity Y",
            [](HF& u, float v) { u.NoiseVelocity[1] = v; u.Preset = HeightFogPreset::Custom; },
            0.01f, "Animated noise scroll speed.");
        AddIndexedFloatRow(ctx.Parent, "Noise Vel Z", effect->NoiseVelocity[2], w, e, n, undo,
            "Change Height Fog Noise Velocity Z",
            [](HF& u, float v) { u.NoiseVelocity[2] = v; u.Preset = HeightFogPreset::Custom; },
            0.01f, "Animated noise scroll speed.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Noise Fade Start", effect->NoiseFadeStart, w, e, n,
            undo, "Change Height Fog Noise Fade Start",
            [](HF& u, float v) { u.NoiseFadeStart = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            10.0f, "Distance where noise begins fading out.",
            {}, 0.0f, 100000.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Noise Fade End", effect->NoiseFadeEnd, w, e, n,
            undo, "Change Height Fog Noise Fade End",
            [](HF& u, float v) { u.NoiseFadeEnd = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            10.0f, "Distance where noise is fully neutral.",
            {}, 0.0f, 100000.0f);

        InspectorUI::AddTextBlock(ctx.Parent, "Sky", "inspector-section-subheader");

        AddToggleRow(ctx.Parent, "Sky Enabled", effect->SkyEnabled,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Sky Enabled",
                    [v](HF& u) { u.SkyEnabled = v; u.Preset = HeightFogPreset::Custom; });
            },
            "Apply fog to sky pixels for horizon haze.");

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Sky Power", effect->SkyPower, w, e, n,
            undo, "Change Height Fog Sky Power",
            [](HF& u, float v) { u.SkyPower = std::max(v, 0.001f); u.Preset = HeightFogPreset::Custom; },
            0.05f, "Sky fill falloff sharpness.",
            {}, 0.001f, 16.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Sky Fill Start", effect->SkyFillStart, w, e, n,
            undo, "Change Height Fog Sky Fill Start",
            [](HF& u, float v) { u.SkyFillStart = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Horizon blend start along fog axis.",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Sky Fill End", effect->SkyFillEnd, w, e, n,
            undo, "Change Height Fog Sky Fill End",
            [](HF& u, float v) { u.SkyFillEnd = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Horizon blend end along fog axis.",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Sky Horizon Offset", effect->SkyHorizonOffset, w, e, n,
            undo, "Change Height Fog Sky Horizon Offset",
            [](HF& u, float v) { u.SkyHorizonOffset = std::clamp(v, -1.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Offsets the sky fog horizon along the fog axis.",
            {}, -1.0f, 1.0f);

        AddComponentFloatRowWithDrag<HF>(ctx.Parent, "Sky Bottom Strength", effect->SkyBottomStrength, w, e, n,
            undo, "Change Height Fog Sky Bottom Strength",
            [](HF& u, float v) { u.SkyBottomStrength = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.01f, "Adds fog coverage below the horizon.",
            {}, 0.0f, 1.0f);

        InspectorUI::AddTextBlock(ctx.Parent, "Multiple Scattering", "inspector-section-subheader");

        auto glowControls = std::make_unique<UIElement>();
        UIElement* glowControlsRaw = glowControls.get();
        glowControlsRaw->Overrides()
            .Set(Style::Display, effect->FogGlowEnabled ? DisplayMode::Flex : DisplayMode::None)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::AlignItems, AlignItems::Stretch);

        AddToggleRow(ctx.Parent, "Fog Glow", effect->FogGlowEnabled,
            [w, e, n, undo, glowControlsRaw](bool v) {
                glowControlsRaw->Overrides().Set(Style::Display, v ? DisplayMode::Flex : DisplayMode::None);
                CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Glow",
                    [v](HF& u) { u.FogGlowEnabled = v; u.Preset = HeightFogPreset::Custom; });
            }, "Approximate higher-order scattering with the shared fog pyramid.");

        AddComponentFloatRowWithDrag<HF>(glowControlsRaw, "Intensity", effect->FogGlowIntensity, w, e, n,
            undo, "Change Height Fog Glow Intensity",
            [](HF& u, float v) { u.FogGlowIntensity = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            0.0f, "Strength of the reconstructed scattering veil", {}, 0.0f, 10.0f);

        AddComponentFloatRowWithDrag<HF>(glowControlsRaw, "Radius", effect->FogGlowRadius, w, e, n,
            undo, "Change Height Fog Glow Radius",
            [](HF& u, float v) { u.FogGlowRadius = std::clamp(v, 1.0f, 7.0f); u.Preset = HeightFogPreset::Custom; },
            3.0f, "Continuous multiscale radius", {}, 1.0f, 7.0f);

        auto pyramidOnlyControls = std::make_unique<UIElement>();
        UIElement* pyramidOnlyControlsRaw = pyramidOnlyControls.get();
        pyramidOnlyControlsRaw->Overrides()
            .Set(Style::Display,
                 effect->FogGlowQualityLevel == Components::FogGlowQuality::Fast
                     ? DisplayMode::None
                     : DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::AlignItems, AlignItems::Stretch);

        auto* qualityField = InspectorUI::AddEnumRow(glowControlsRaw, "Quality", kFogGlowQualities,
                                        effect->FogGlowQualityLevel,
                                        "Fast uses one 3x3 pass; higher modes use 4, 6, or 8 pyramid octaves.");
        qualityField->SetOnValueChanged([w, e, n, undo, pyramidOnlyControlsRaw](Components::FogGlowQuality quality) {
            pyramidOnlyControlsRaw->Overrides().Set(
                Style::Display,
                quality == Components::FogGlowQuality::Fast ? DisplayMode::None : DisplayMode::Flex);
            CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Glow Quality",
                [quality](HF& u) { u.FogGlowQualityLevel = quality; u.Preset = HeightFogPreset::Custom; });
        });

        AddComponentFloatRowWithDrag<HF>(pyramidOnlyControlsRaw, "Scatter", effect->FogGlowScatter, w, e, n,
            undo, "Change Height Fog Glow Scatter",
            [](HF& u, float v) { u.FogGlowScatter = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.7f, "Bias between narrow and broad pyramid levels", {}, 0.0f, 1.0f);
        glowControlsRaw->AddChild(std::move(pyramidOnlyControls));

        AddComponentFloatRowWithDrag<HF>(glowControlsRaw, "HDR Threshold", effect->FogGlowThreshold, w, e, n,
            undo, "Change Height Fog Glow Threshold",
            [](HF& u, float v) { u.FogGlowThreshold = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            0.0f, "Scene-linear brightness required to enter the veil", {}, 0.0f, 100.0f);

        AddComponentFloatRowWithDrag<HF>(glowControlsRaw, "Soft Knee", effect->FogGlowKnee, w, e, n,
            undo, "Change Height Fog Glow Knee",
            [](HF& u, float v) { u.FogGlowKnee = std::max(v, 0.0f); u.Preset = HeightFogPreset::Custom; },
            0.5f, "Soft transition around the HDR threshold", {}, 0.0f, 10.0f);

        AddComponentFloatRowWithDrag<HF>(glowControlsRaw, "Fade Start", effect->FogGlowFadeStart, w, e, n,
            undo, "Change Height Fog Glow Fade Start",
            [](HF& u, float v) { u.FogGlowFadeStart = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.05f, "Fog opacity where scattering begins", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<HF>(glowControlsRaw, "Fade End", effect->FogGlowFadeEnd, w, e, n,
            undo, "Change Height Fog Glow Fade End",
            [](HF& u, float v) { u.FogGlowFadeEnd = std::clamp(v, 0.0f, 1.0f); u.Preset = HeightFogPreset::Custom; },
            0.8f, "Fog opacity where scattering reaches full strength", {}, 0.0f, 1.0f);

        AddFogColorRow(glowControlsRaw, "Tint", "HDR tint for scattered fog", w, e, n, undo,
            ctx.OpenColorPickerWindow, &HF::FogGlowTint, {1.0f, 1.0f, 1.0f}, "Change Height Fog Glow Tint");

        AddToggleRow(glowControlsRaw, "Anti Flicker", effect->FogGlowAntiFlicker,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<HF>(w, e, n, undo, "Change Height Fog Glow Anti Flicker",
                    [v](HF& u) { u.FogGlowAntiFlicker = v; u.Preset = HeightFogPreset::Custom; });
            }, "Median stabilization for Fast mode; median and Karis stabilization for pyramid modes.");

        ctx.Parent->AddChild(std::move(glowControls));
    };

    InspectorRegistry::Get().RegisterComponentInspector<HF>(std::move(fn));
}

} // namespace GameEngine
