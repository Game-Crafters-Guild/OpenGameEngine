#include "Inspectors/VolumetricFogEffectInspector.h"

#include "InspectorRegistry.h"
#include "Platform/SystemMetrics.h"

#include "Components/Rendering/PostProcessEffects/VolumetricFogEffect.h"
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
constexpr float kMaxFogColorPickerIntensity = 8.0f;
constexpr float kSwatchSize = 20.0f;
constexpr float kSwatchBorderRadius = 3.0f;
constexpr uint32_t kSwatchBorderColor = 0xFF555555u;

static constexpr EnumEntry<Components::VolumetricFogDensityMode> kDensityModes[] = {
    {Components::VolumetricFogDensityMode::Additive, "Additive"},
    {Components::VolumetricFogDensityMode::Subtractive, "Subtractive"},
    {Components::VolumetricFogDensityMode::Override, "Override"},
};

static constexpr EnumEntry<Components::VolumetricFogGradientMode> kGradientModes[] = {
    {Components::VolumetricFogGradientMode::None, "None"},
    {Components::VolumetricFogGradientMode::LocalX, "Local X"},
    {Components::VolumetricFogGradientMode::LocalY, "Local Y"},
    {Components::VolumetricFogGradientMode::LocalZ, "Local Z"},
    {Components::VolumetricFogGradientMode::ScreenY, "Screen Y"},
    {Components::VolumetricFogGradientMode::MainLight, "Main Light"},
};

static constexpr EnumEntry<Components::FogGlowQuality> kFogGlowQualities[] = {
    {Components::FogGlowQuality::Fast, "Fast (3x3)"},
    {Components::FogGlowQuality::Balanced, "Balanced"},
    {Components::FogGlowQuality::High, "High"},
    {Components::FogGlowQuality::Cinematic, "Cinematic"},
};

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
    float (Components::VolumetricFogEffect::*member)[3],
    std::array<float, 3> defaultValue,
    const char* changeLabel)
{
    using Fog = Components::VolumetricFogEffect;
    if (!parent || !world || !entity.IsValid())
        return;

    auto* effect = world->GetComponent<Fog>(entity);
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

    auto updateUI = [world, entity, member, swatchRaw, rgbLabelRaw]() {
        auto* c = world->GetComponent<Fog>(entity);
        if (!c)
            return;
        uint32_t nextArgb = 0;
        float nextIntensity = 1.0f;
        FogRgbToPickerState(c->*member, nextArgb, nextIntensity);
        StyleFogSwatch(swatchRaw, nextArgb);
        rgbLabelRaw->SetText(FormatFogRgb(c->*member));
    };

    auto clickHandler = [world, entity, notifications, undo, openPicker, member, updateUI, changeLabel](UIEvent& ev) {
        if (ev.Button != 0)
            return;
        ev.Stop();
        if (!openPicker)
            return;

        auto* comp = world->GetComponent<Fog>(entity);
        if (!comp)
            return;

        uint32_t currentArgb = 0;
        float currentIntensity = 1.0f;
        FogRgbToPickerState(comp->*member, currentArgb, currentIntensity);

        using Edit = Editor::UndoRedoService::InteractiveEdit;
        auto edit = std::make_shared<Edit>();
        if (undo)
        {
            auto target = InspectorDrag::MakeComponentSnapshotTarget<Fog>(world, entity, notifications, changeLabel);
            *edit = undo->BeginInteractiveEdit(changeLabel, std::move(target));
        }

        ColorPickerCallbacks cbs;
        cbs.onValueChanging = [world, entity, notifications, edit, member, updateUI](uint32_t newArgb, float newIntensity) {
            if (*edit)
            {
                edit->Preview([&] {
                    auto* c = world->GetComponentForWrite<Fog>(entity);
                    if (c)
                        PickerToFogRgb(newArgb, newIntensity, c->*member);
                });
            }
            else
            {
                auto* c = world->GetComponent<Fog>(entity);
                if (!c)
                    return;
                Fog updated = *c;
                PickerToFogRgb(newArgb, newIntensity, updated.*member);
                Editor::PreviewComponentUpdate(world, entity, notifications, updated);
            }
            updateUI();
        };
        cbs.onApply = [world, entity, notifications, edit, member, updateUI](uint32_t newArgb, float newIntensity) {
            if (*edit)
            {
                edit->Preview([&] {
                    auto* c = world->GetComponentForWrite<Fog>(entity);
                    if (c)
                        PickerToFogRgb(newArgb, newIntensity, c->*member);
                });
                edit->Commit();
            }
            else
            {
                auto* c = world->GetComponent<Fog>(entity);
                if (!c)
                    return;
                Fog updated = *c;
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
                InspectorDrag::CommitComponentWithUndo<Fog>(world, entity, notifications, undo, changeLabel,
                    [member, defaultValue](Fog& u) {
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
} // namespace

void RegisterVolumetricFogEffectInspector()
{
    using namespace InspectorDrag;
    using Fog = Components::VolumetricFogEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<Fog>(ctx.Entity);
        if (!effect)
        {
            InspectorUI::AddLine(ctx.Parent, "(VolumetricFogEffect missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        auto addFogFloatSlider =
            [w, e, n, undo](
                UIElement* parent,
                const char* label,
                float initial,
                float minValue,
                float maxValue,
                float step,
                float defaultValue,
                const std::string& undoName,
                auto apply,
                const char* tooltip) {
                const float initialClamped = std::clamp(initial, minValue, maxValue);
                auto sliderRow = AddSliderWithFloatValueRow(parent, label, initialClamped, minValue, maxValue, tooltip);
                Label* rowLabel = sliderRow.Label;
                Slider* slider = sliderRow.Slider;
                FloatField* valueField = sliderRow.ValueField;
                if (!slider || !valueField)
                    return;

                slider->SetStep(step);
                slider->SetShowValueBubble(false);

                auto sync = [slider, valueField, minValue, maxValue](float rawValue) {
                    const float v = std::clamp(rawValue, minValue, maxValue);
                    slider->SetValueWithoutNotify(v);
                    if (valueField)
                        valueField->SetValueWithoutNotify(v);
                    return v;
                };

                auto handlers = MakeComponentInteractiveHandlers<Fog, float>(
                    w, e, n, undo, undoName, apply, {});
                auto previewValue = [sync, preview = handlers.first](float rawValue) mutable {
                    preview(sync(rawValue));
                };
                auto commitValue = [sync, commit = handlers.second](float rawValue) mutable {
                    commit(sync(rawValue));
                };

                valueField->SetOnValueChanging(previewValue);
                valueField->SetOnValueChanged(commitValue);
                slider->SetOnValueChanging(previewValue);
                slider->SetOnValueChanged(commitValue);
                SetupLabelDragSlider(
                    rowLabel,
                    slider,
                    nullptr,
                    nullptr,
                    std::clamp(defaultValue, minValue, maxValue));
            };

        auto addFogIntSlider =
            [w, e, n, undo](
                UIElement* parent,
                const char* label,
                int initial,
                int minValue,
                int maxValue,
                int defaultValue,
                const std::string& undoName,
                auto apply,
                const char* tooltip) {
                const int initialClamped = std::clamp(initial, minValue, maxValue);
                auto sliderRow = AddSliderWithIntValueRow(parent, label, initialClamped, minValue, maxValue, tooltip);
                Label* rowLabel = sliderRow.Label;
                Slider* slider = sliderRow.Slider;
                IntField* valueField = sliderRow.ValueField;
                if (!slider || !valueField)
                    return;

                slider->SetStep(1.0f);
                slider->SetShowValueBubble(false);

                auto sync = [slider, valueField, minValue, maxValue](float rawValue) {
                    const int v = std::clamp(static_cast<int>(std::lround(rawValue)), minValue, maxValue);
                    slider->SetValueWithoutNotify(static_cast<float>(v));
                    if (valueField)
                        valueField->SetValueWithoutNotify(v);
                    return v;
                };

                auto handlers = MakeComponentInteractiveHandlers<Fog, int>(
                    w, e, n, undo, undoName, apply, {});
                auto previewValue = [preview = handlers.first](int value) mutable { preview(value); };
                auto commitValue = [commit = handlers.second](int value) mutable { commit(value); };

                valueField->SetOnValueChanging([sync, previewValue](const int& value) mutable {
                    previewValue(sync(static_cast<float>(value)));
                });
                valueField->SetOnValueChanged([sync, commitValue](const int& value) mutable {
                    commitValue(sync(static_cast<float>(value)));
                });
                slider->SetOnValueChanging([sync, previewValue](const float& value) mutable {
                    previewValue(sync(value));
                });
                slider->SetOnValueChanged([sync, commitValue](const float& value) mutable {
                    commitValue(sync(value));
                });
                SetupLabelDragSlider(
                    rowLabel,
                    slider,
                    nullptr,
                    nullptr,
                    static_cast<float>(std::clamp(defaultValue, minValue, maxValue)));
            };

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Intensity", effect->Intensity, w, e, n,
            undo, "Change Fog Intensity",
            [](Fog& u, float v) { u.Intensity = std::max(0.0f, v); },
            1.0f, "Overall fog contribution");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Density", effect->Density, w, e, n,
            undo, "Change Fog Density",
            [](Fog& u, float v) { u.Density = std::max(0.0f, v); },
            0.035f, "Participating media density");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Max Distance", effect->MaxDistance, w, e, n,
            undo, "Change Fog Distance",
            [](Fog& u, float v) { u.MaxDistance = std::max(0.01f, v); },
            260.0f, "Maximum fog ray distance");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Base Height", effect->BaseHeight, w, e, n,
            undo, "Change Fog Base Height",
            [](Fog& u, float v) { u.BaseHeight = v; },
            0.0f, "World-space height where fog starts");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Height Falloff", effect->HeightFalloff, w, e, n,
            undo, "Change Fog Height Falloff",
            [](Fog& u, float v) { u.HeightFalloff = std::max(0.01f, v); },
            24.0f, "Exponential height falloff distance");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Sky Fade", effect->SkyFade, w, e, n,
            undo, "Change Fog Sky Fade",
            [](Fog& u, float v) { u.SkyFade = std::clamp(v, 0.0f, 1.0f); },
            0.5f, "Softness of the distant fog-to-sky transition above the horizon (0 = hard edge)");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Anisotropy", effect->Anisotropy, w, e, n,
            undo, "Change Fog Anisotropy",
            [](Fog& u, float v) { u.Anisotropy = std::clamp(v, -0.95f, 0.95f); },
            0.55f, "Mie scattering directionality");

        InspectorUI::AddTextBlock(ctx.Parent, "Scattering", "inspector-section-subheader");

        AddToggleRow(ctx.Parent, "Track Sun Light", effect->TrackDirectionalLight,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<Fog>(w, e, n, undo, "Change Fog Sun Tracking",
                    [v](Fog& u) { u.TrackDirectionalLight = v; });
            }, "Use the scene directional light for fog sun direction, color, and intensity.");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Sun Intensity", effect->SunIntensityScale, w, e, n,
            undo, "Change Fog Sun Intensity",
            [](Fog& u, float v) { u.SunIntensityScale = std::max(0.0f, v); },
            1.0f, "Multiplier applied to directional-light scattering.");

        AddFogColorRow(ctx.Parent, "Albedo", "Fog scattering albedo color", w, e, n, undo, ctx.OpenColorPickerWindow,
            &Fog::Albedo, {0.82f, 0.78f, 0.72f}, "Change Fog Albedo");

        AddFogColorRow(ctx.Parent, "Emission", "Fog self-emission color", w, e, n, undo, ctx.OpenColorPickerWindow,
            &Fog::Emission, {0.0f, 0.0f, 0.0f}, "Change Fog Emission");

        AddFogColorRow(ctx.Parent, "Sun Tint", "Directional-light scattering tint", w, e, n, undo, ctx.OpenColorPickerWindow,
            &Fog::SunScatteringTint, {1.0f, 0.72f, 0.42f}, "Change Fog Sun Tint");

        AddFogColorRow(ctx.Parent, "Ambient Tint", "Ambient scattering tint", w, e, n, undo, ctx.OpenColorPickerWindow,
            &Fog::AmbientScatteringTint, {0.32f, 0.38f, 0.48f}, "Change Fog Ambient Tint");

        InspectorUI::AddTextBlock(ctx.Parent, "Grid", "inspector-section-subheader");

        addFogIntSlider(ctx.Parent, "Cell Pixels", effect->XYCellSizePixels, 1, 64, 8,
            "Change Fog Cell Pixels",
            [](Fog& u, int v) { u.XYCellSizePixels = std::clamp(v, 1, 64); },
            "Froxel grid XY cell size in pixels. Lower values reduce blockiness and cost more.");

        addFogIntSlider(ctx.Parent, "Z Slices", effect->ZSliceCount, 8, 256, 128,
            "Change Fog Z Slices",
            [](Fog& u, int v) { u.ZSliceCount = std::clamp(v, 8, 256); },
            "Froxel grid depth slices. More slices reduce depth stepping and cost more.");

        addFogFloatSlider(ctx.Parent, "Depth Distribution", effect->DepthDistribution, 0.5f, 4.0f, 0.0f, 1.6f,
            "Change Fog Depth Distribution",
            [](Fog& u, float v) { u.DepthDistribution = std::max(0.05f, v); },
            "Non-linear depth slice distribution. Lower values spread slices more evenly.");

        InspectorUI::AddTextBlock(ctx.Parent, "Noise", "inspector-section-subheader");

        AddToggleRow(ctx.Parent, "Noise", effect->NoiseEnabled,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<Fog>(w, e, n, undo, "Change Fog Noise",
                    [v](Fog& u) { u.NoiseEnabled = v; });
            }, "Apply animated procedural density variation.");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Noise Scale", effect->NoiseScale, w, e, n,
            undo, "Change Fog Noise Scale",
            [](Fog& u, float v) { u.NoiseScale = std::max(0.01f, v); },
            80.0f, "World scale of procedural fog noise");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Noise Strength", effect->NoiseStrength, w, e, n,
            undo, "Change Fog Noise Strength",
            [](Fog& u, float v) { u.NoiseStrength = std::clamp(v, 0.0f, 1.0f); },
            0.28f, "Amount of noise modulation");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Noise Contrast", effect->NoiseContrast, w, e, n,
            undo, "Change Fog Noise Contrast",
            [](Fog& u, float v) { u.NoiseContrast = std::max(0.01f, v); },
            1.25f, "Contrast applied to procedural fog noise");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Noise R", effect->NoiseChannelWeights[0], w, e, n,
            undo, "Change Fog Noise R Weight",
            [](Fog& u, float v) { u.NoiseChannelWeights[0] = std::max(0.0f, v); },
            1.0f, "Density noise atlas red channel weight");
        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Noise G", effect->NoiseChannelWeights[1], w, e, n,
            undo, "Change Fog Noise G Weight",
            [](Fog& u, float v) { u.NoiseChannelWeights[1] = std::max(0.0f, v); },
            0.0f, "Density noise atlas green channel weight");
        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Noise B", effect->NoiseChannelWeights[2], w, e, n,
            undo, "Change Fog Noise B Weight",
            [](Fog& u, float v) { u.NoiseChannelWeights[2] = std::max(0.0f, v); },
            0.0f, "Density noise atlas blue channel weight");
        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Noise A", effect->NoiseChannelWeights[3], w, e, n,
            undo, "Change Fog Noise A Weight",
            [](Fog& u, float v) { u.NoiseChannelWeights[3] = std::max(0.0f, v); },
            0.0f, "Density noise atlas alpha channel weight");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Density Threshold", effect->DensityThreshold, w, e, n,
            undo, "Change Fog Density Threshold",
            [](Fog& u, float v) { u.DensityThreshold = std::clamp(v, 0.0f, 1.0f); },
            0.0f, "Cuts low density values from generated 3D noise");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Threshold Softness", effect->DensityThresholdSoftness, w, e, n,
            undo, "Change Fog Threshold Softness",
            [](Fog& u, float v) { u.DensityThresholdSoftness = std::max(0.0001f, v); },
            0.15f, "Soft transition width for density threshold");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Velocity X", effect->NoiseVelocity[0], w, e, n,
            undo, "Change Fog Noise Velocity X",
            [](Fog& u, float v) { u.NoiseVelocity[0] = v; },
            0.0f, "Procedural fog noise velocity X");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Velocity Y", effect->NoiseVelocity[1], w, e, n,
            undo, "Change Fog Noise Velocity Y",
            [](Fog& u, float v) { u.NoiseVelocity[1] = v; },
            0.0f, "Procedural fog noise velocity Y");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Velocity Z", effect->NoiseVelocity[2], w, e, n,
            undo, "Change Fog Noise Velocity Z",
            [](Fog& u, float v) { u.NoiseVelocity[2] = v; },
            0.0f, "Procedural fog noise velocity Z");

        InspectorUI::AddTextBlock(ctx.Parent, "Local Volume", "inspector-section-subheader");

        auto* densityMode = InspectorUI::AddEnumRow(ctx.Parent, "Density Mode", kDensityModes, effect->DensityMode,
            "Local fog density operation for non-global volumes");
        densityMode->SetOnValueChanged([w, e, n, undo](Components::VolumetricFogDensityMode v) {
            CommitComponentWithUndo<Fog>(w, e, n, undo, "Change Fog Density Mode",
                [v](Fog& u) { u.DensityMode = v; });
        });

        auto* gradientMode = InspectorUI::AddEnumRow(ctx.Parent, "Gradient Mode", kGradientModes, effect->GradientMode,
            "How local-volume color gradients are mapped");
        gradientMode->SetOnValueChanged([w, e, n, undo](Components::VolumetricFogGradientMode v) {
            CommitComponentWithUndo<Fog>(w, e, n, undo, "Change Fog Gradient Mode",
                [v](Fog& u) { u.GradientMode = v; });
        });

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Gradient Strength", effect->GradientStrength, w, e, n,
            undo, "Change Fog Gradient Strength",
            [](Fog& u, float v) { u.GradientStrength = std::clamp(v, 0.0f, 1.0f); },
            0.0f, "Blend from albedo toward the gradient tint");

        AddFogColorRow(ctx.Parent, "Gradient Low", "Gradient low tint", w, e, n, undo, ctx.OpenColorPickerWindow,
            &Fog::GradientLowTint, {1.0f, 1.0f, 1.0f}, "Change Fog Gradient Low");

        AddFogColorRow(ctx.Parent, "Gradient High", "Gradient high tint", w, e, n, undo, ctx.OpenColorPickerWindow,
            &Fog::GradientHighTint, {1.0f, 1.0f, 1.0f}, "Change Fog Gradient High");

        InspectorUI::AddTextBlock(ctx.Parent, "Temporal", "inspector-section-subheader");

        AddToggleRow(ctx.Parent, "Temporal", effect->TemporalEnabled,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<Fog>(w, e, n, undo, "Change Fog Temporal",
                    [v](Fog& u) { u.TemporalEnabled = v; });
            }, "Accumulate fog history for smoother raymarching.");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Temporal Blend", effect->TemporalBlend, w, e, n,
            undo, "Change Fog Temporal Blend",
            [](Fog& u, float v) { u.TemporalBlend = std::clamp(v, 0.0f, 0.99f); },
            0.92f, "Fog history blend factor");

        addFogFloatSlider(ctx.Parent, "Jitter Scale", effect->JitterStrength, 0.0f, 1.0f, 0.0f, 0.45f,
            "Change Fog Jitter Scale",
            [](Fog& u, float v) { u.JitterStrength = std::clamp(v, 0.0f, 1.0f); },
            "Froxel jitter amplitude. Lower values are calmer; higher values hide stepping better.");

        AddToggleRow(ctx.Parent, "Jitter Motion", effect->JitterMotion,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<Fog>(w, e, n, undo, "Change Fog Jitter Motion",
                    [v](Fog& u) { u.JitterMotion = v; });
            }, "Animate the froxel jitter sequence.");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Composite Depth Bias", effect->CompositeDepthBias, w, e, n,
            undo, "Change Fog Composite Depth Bias",
            [](Fog& u, float v) { u.CompositeDepthBias = v; },
            0.0f, "Depth bias used when compositing fog at scene silhouettes");

        AddComponentFloatRowWithDrag<Fog>(ctx.Parent, "Shadow Bias", effect->ShadowBias, w, e, n,
            undo, "Change Fog Shadow Bias",
            [](Fog& u, float v) { u.ShadowBias = v; },
            0.0f, "Extra world-space bias toward the light for every fog shadow lookup: the sun's cascades and each spot and point light's shadow map");

        InspectorUI::AddTextBlock(ctx.Parent, "Glow", "inspector-section-subheader");

        auto glowControls = std::make_unique<UIElement>();
        UIElement* glowControlsRaw = glowControls.get();
        glowControlsRaw->Overrides()
            .Set(Style::Display, effect->FogGlowEnabled ? DisplayMode::Flex : DisplayMode::None)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::AlignItems, AlignItems::Stretch);

        AddToggleRow(ctx.Parent, "Fog Glow", effect->FogGlowEnabled,
            [w, e, n, undo, glowControlsRaw](bool v) {
                glowControlsRaw->Overrides().Set(Style::Display, v ? DisplayMode::Flex : DisplayMode::None);
                CommitComponentWithUndo<Fog>(w, e, n, undo, "Change Fog Glow",
                    [v](Fog& u) { u.FogGlowEnabled = v; });
            }, "Approximate higher-order scattering with a shared multiscale veil.");

        AddComponentFloatRowWithDrag<Fog>(glowControlsRaw, "Intensity", effect->FogGlowIntensity, w, e, n,
            undo, "Change Fog Glow Intensity",
            [](Fog& u, float v) { u.FogGlowIntensity = std::max(0.0f, v); },
            0.0f, "Strength of the reconstructed scattering veil", {}, 0.0f, 10.0f);

        AddComponentFloatRowWithDrag<Fog>(glowControlsRaw, "Radius", effect->FogGlowRadius, w, e, n,
            undo, "Change Fog Glow Radius",
            [](Fog& u, float v) { u.FogGlowRadius = std::clamp(v, 1.0f, 7.0f); },
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
            CommitComponentWithUndo<Fog>(w, e, n, undo, "Change Fog Glow Quality",
                [quality](Fog& u) { u.FogGlowQualityLevel = quality; });
        });

        AddComponentFloatRowWithDrag<Fog>(pyramidOnlyControlsRaw, "Scatter", effect->FogGlowScatter, w, e, n,
            undo, "Change Fog Glow Scatter",
            [](Fog& u, float v) { u.FogGlowScatter = std::clamp(v, 0.0f, 1.0f); },
            0.7f, "Bias between narrow and broad pyramid levels", {}, 0.0f, 1.0f);
        glowControlsRaw->AddChild(std::move(pyramidOnlyControls));

        AddComponentFloatRowWithDrag<Fog>(glowControlsRaw, "HDR Threshold", effect->FogGlowThreshold, w, e, n,
            undo, "Change Fog Glow Threshold",
            [](Fog& u, float v) { u.FogGlowThreshold = std::max(v, 0.0f); },
            0.0f, "Scene-linear brightness required to enter the veil", {}, 0.0f, 100.0f);

        AddComponentFloatRowWithDrag<Fog>(glowControlsRaw, "Soft Knee", effect->FogGlowKnee, w, e, n,
            undo, "Change Fog Glow Knee",
            [](Fog& u, float v) { u.FogGlowKnee = std::max(v, 0.0f); },
            0.5f, "Soft transition around the HDR threshold", {}, 0.0f, 10.0f);

        AddComponentFloatRowWithDrag<Fog>(glowControlsRaw, "Fade Start", effect->FogGlowFadeStart, w, e, n,
            undo, "Change Fog Glow Fade Start",
            [](Fog& u, float v) { u.FogGlowFadeStart = std::clamp(v, 0.0f, 1.0f); },
            0.05f, "Fog opacity where scattering begins", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Fog>(glowControlsRaw, "Fade End", effect->FogGlowFadeEnd, w, e, n,
            undo, "Change Fog Glow Fade End",
            [](Fog& u, float v) { u.FogGlowFadeEnd = std::clamp(v, 0.0f, 1.0f); },
            0.8f, "Fog opacity where scattering reaches full strength", {}, 0.0f, 1.0f);

        AddFogColorRow(glowControlsRaw, "Tint", "HDR tint for scattered fog", w, e, n, undo,
            ctx.OpenColorPickerWindow, &Fog::FogGlowTint, {1.0f, 1.0f, 1.0f}, "Change Fog Glow Tint");

        AddToggleRow(glowControlsRaw, "Anti Flicker", effect->FogGlowAntiFlicker,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<Fog>(w, e, n, undo, "Change Fog Glow Anti Flicker",
                    [v](Fog& u) { u.FogGlowAntiFlicker = v; });
            }, "Median stabilization for Fast mode; median and Karis stabilization for pyramid modes.");

        ctx.Parent->AddChild(std::move(glowControls));

    };

    InspectorRegistry::Get().RegisterComponentInspector<Fog>(std::move(fn));
}

} // namespace GameEngine
