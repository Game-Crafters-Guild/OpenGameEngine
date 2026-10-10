#include "Inspectors/BloomEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/BloomEffect.h"
#include "Core/Engine.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Assets/AssetRegistry.h" // AssetIndexRecord — the lens-dirt metadata filter
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Types/StringUtils.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/EnumField.h"
#include "UI/StyleProperties.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
using Bloom = Components::BloomEffect;

constexpr EnumEntry<int32_t> kBloomOctaves[] = {
    {3, "3 — Performance"},
    {4, "4"},
    {5, "5"},
    {6, "6 — Balanced"},
    {7, "7"},
    {8, "8 — Quality"},
};

constexpr float kBloomRadiusMin = 1.0f;
constexpr float kBloomRadiusMax = 7.0f;
constexpr float kBloomRadiusDefault = 2.5f;

constexpr float kVignetteSwatchSize = 20.0f;
constexpr float kVignetteSwatchBorderRadius = 3.0f;
constexpr uint32_t kVignetteSwatchBorderColor = 0xFF555555u;

uint32_t VignetteColorToArgb(const float (&color)[3])
{
    const auto channel = [](float value)
    {
        return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (0xFFu << 24) |
           (static_cast<uint32_t>(channel(color[0])) << 16) |
           (static_cast<uint32_t>(channel(color[1])) << 8) |
           static_cast<uint32_t>(channel(color[2]));
}

void ArgbToVignetteColor(uint32_t argb, float (&out)[3])
{
    out[0] = ((argb >> 16) & 0xFF) / 255.0f;
    out[1] = ((argb >> 8) & 0xFF) / 255.0f;
    out[2] = (argb & 0xFF) / 255.0f;
}

std::string FormatVignetteColor(const float (&color)[3])
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", color[0], color[1], color[2]);
    return buffer;
}

void StyleVignetteSwatch(UIElement* swatch, uint32_t argb)
{
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(kVignetteSwatchSize))
        .Set(Style::Height, StyleLength::Px(kVignetteSwatchSize))
        .Set(Style::MinWidth, StyleLength::Px(kVignetteSwatchSize))
        .Set(Style::MinHeight, StyleLength::Px(kVignetteSwatchSize))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{
            kVignetteSwatchBorderRadius, kVignetteSwatchBorderRadius,
            kVignetteSwatchBorderRadius, kVignetteSwatchBorderRadius})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{
            kVignetteSwatchBorderColor, kVignetteSwatchBorderColor,
            kVignetteSwatchBorderColor, kVignetteSwatchBorderColor})
        .Set(Style::BackgroundColor, argb)
        .Set(Style::Cursor, CursorStyle::Pointer);
}

void AddBloomColorRow(
    UIElement* parent, const char* labelText, const char* tooltip,
    const char* snapshotLabel, const char* undoLabel,
    float32 (Bloom::*colorMember)[3], ECS::World* world, ECS::EntityHandle entity,
    Editor::EditorChangeNotifications* notifications, Editor::UndoRedoService* undo,
    OpenColorPickerWindowFn openPicker, const float (&initialColor)[3])
{
    UIElement* row = InspectorUI::AddRow(parent);
    Label* label = InspectorUI::AddLabel(row, labelText, tooltip);
    if (label)
        label->AddClass("inspector-label-no-drag");

    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    StyleVignetteSwatch(swatchRaw, VignetteColorToArgb(initialColor));
    field->AddChild(std::move(swatch));

    auto rgbLabel = std::make_unique<Label>();
    Label* rgbLabelRaw = rgbLabel.get();
    rgbLabelRaw->AddClass("inspector-text");
    rgbLabelRaw->SetText(FormatVignetteColor(initialColor));
    rgbLabelRaw->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    field->AddChild(std::move(rgbLabel));

    // The picker's callbacks outlive an inspector rebuild: resolve the row through weak
    // refs instead of holding the freed widgets.
    auto refresh = [swatchRef = UIElement::MakeWeakRef(swatchRaw),
                    rgbLabelRef = UIElement::MakeWeakRef(rgbLabelRaw), colorMember, world, entity]()
    {
        const auto* bloom = world->GetComponent<Bloom>(entity);
        UIElement* swatch = swatchRef.Get();
        Label* rgbLabel = rgbLabelRef.Get();
        if (!bloom || !swatch || !rgbLabel)
            return;
        const auto& color = bloom->*colorMember;
        StyleVignetteSwatch(swatch, VignetteColorToArgb(color));
        rgbLabel->SetText(FormatVignetteColor(color));
    };

    auto onClick = [world, entity, notifications, undo, openPicker, refresh,
                    snapshotLabel, undoLabel, colorMember](UIEvent& event)
    {
        if (event.Button != 0)
            return;
        event.Stop();
        if (!openPicker)
            return;
        const auto* bloom = world->GetComponent<Bloom>(entity);
        if (!bloom)
            return;
        const Bloom original = *bloom;

        using Edit = Editor::UndoRedoService::InteractiveEdit;
        auto edit = std::make_shared<Edit>();
        if (undo)
        {
            auto target = InspectorDrag::MakeComponentSnapshotTarget<Bloom>(
                world, entity, notifications, snapshotLabel);
            *edit = undo->BeginInteractiveEdit(undoLabel, std::move(target));
        }

        auto applyColor = [world, entity, notifications, edit, refresh,
                           colorMember](uint32_t argb, bool commit)
        {
            if (*edit)
            {
                edit->Preview([&]
                {
                    if (auto* value = world->GetComponentForWrite<Bloom>(entity))
                        ArgbToVignetteColor(argb, value->*colorMember);
                });
                if (commit)
                    edit->Commit();
            }
            else if (const auto* current = world->GetComponent<Bloom>(entity))
            {
                Bloom updated = *current;
                ArgbToVignetteColor(argb, updated.*colorMember);
                if (commit)
                    Editor::CommitComponentUpdate(world, entity, notifications, updated);
                else
                    Editor::PreviewComponentUpdate(world, entity, notifications, updated);
            }
            refresh();
        };

        ColorPickerCallbacks callbacks;
        callbacks.onApply = [applyColor](uint32_t argb, float) { applyColor(argb, true); };
        callbacks.onValueChanging = [applyColor](uint32_t argb, float) { applyColor(argb, false); };
        callbacks.onCancel = [world, entity, notifications, edit, original, refresh]()
        {
            if (*edit)
                edit->Cancel();
            else
                Editor::PreviewComponentUpdate(world, entity, notifications, original);
            refresh();
        };
        openPicker(VignetteColorToArgb(bloom->*colorMember), 1.0f, std::move(callbacks));
    };

    swatchRaw->RegisterEventHandler(kEventMouseDown, onClick);
    rgbLabelRaw->RegisterEventHandler(kEventMouseDown, onClick);
}

float EvaluateBloomResponse(float brightness, float threshold, float knee)
{
    const float safeKnee = std::max(knee, 1e-5f);
    float soft = std::clamp((brightness - threshold + safeKnee) / (2.0f * safeKnee), 0.0f, 1.0f);
    soft = soft * soft * safeKnee;
    return std::min(std::max(brightness - threshold, soft), 100.0f);
}

void UpdateBloomResponseCurve(CurveField& graph, float threshold, float knee)
{
    threshold = std::max(threshold, 0.0f);
    knee = std::clamp(knee, 0.0f, 1.0f);
    const float inputMax = std::max(2.0f, threshold * 2.0f + std::max(2.0f * knee, 0.5f));
    const float outputMax = std::max(1.0f, EvaluateBloomResponse(inputMax, threshold, knee));

    std::vector<Math::CurveKey> keys;
    constexpr int kCurveSamples = 32;
    keys.reserve(kCurveSamples);
    for (int i = 0; i < kCurveSamples; ++i)
    {
        const float x = inputMax * static_cast<float>(i) / static_cast<float>(kCurveSamples - 1);
        Math::CurveKey key{};
        key.Time = x;
        key.Value = EvaluateBloomResponse(x, threshold, knee);
        key.Interp = Math::CurveInterp::Linear;
        keys.push_back(key);
    }

    CurveField::Config config;
    config.TimeMin = 0.0f;
    config.TimeMax = inputMax;
    config.ValueMin = 0.0f;
    config.ValueMax = outputMax;
    config.ReadOnly = true;
    config.AllowTimeDrag = false;
    config.AllowAddRemove = false;
    config.MinKeys = kCurveSamples;
    config.MaxKeys = kCurveSamples;
    graph.SetConfig(config);
    graph.SetKeys(keys);
}

CurveField* AddBloomResponseCurve(UIElement* parent, float threshold, float knee)
{
    UIElement* row = InspectorUI::AddRow(parent);
    row->Overrides().Set(Style::AlignItems, AlignItems::FlexStart);
    if (Label* label = InspectorUI::AddLabel(
            row, "Brightness Response (linear)",
            "Live scene-linear bright-pass response produced by Threshold and Knee"))
        label->AddClass("inspector-label-no-drag");

    UIElement* field = InspectorUI::AddFieldContainer(row);
    field->Overrides()
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::AlignItems, AlignItems::Stretch);

    auto graph = std::make_unique<CurveField>();
    CurveField* graphRaw = graph.get();
    UpdateBloomResponseCurve(*graph, threshold, knee);
    graph->Overrides()
        .Set(Style::Height, StyleLength::Px(110.0f))
        .Set(Style::MinHeight, StyleLength::Px(110.0f))
        .Set(Style::MaxHeight, StyleLength::Px(110.0f))
        .Set(Style::MinWidth, StyleLength::Px(160.0f));
    field->AddChild(std::move(graph));
    return graphRaw;
}
}

void RegisterBloomEffectInspector()
{
    using namespace InspectorDrag;
    using Bloom = Components::BloomEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<Bloom>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;

        struct ResponseCurveState
        {
            CurveField* Graph = nullptr;
            float Threshold = 0.0f;
            float Knee = 0.0f;
        };
        auto response = std::make_shared<ResponseCurveState>();
        response->Threshold = std::max(effect->Threshold, 0.0f);
        response->Knee = std::clamp(effect->Knee, 0.0f, 1.0f);
        auto refreshResponse = [response]()
        {
            if (response->Graph)
                UpdateBloomResponseCurve(*response->Graph, response->Threshold, response->Knee);
        };

        {
            auto thresholdHandlers = MakeComponentInteractiveHandlers<Bloom, float>(
                w, e, n, undo, "Change Bloom Threshold",
                [](Bloom& u, float v) { u.Threshold = std::max(0.0f, v); }, {});
            AddFloatRowWithDrag(ctx.Parent, "Threshold", effect->Threshold,
                [response, refreshResponse, preview = std::move(thresholdHandlers.first)](float value)
                {
                    response->Threshold = std::max(value, 0.0f);
                    refreshResponse();
                    preview(value);
                },
                [response, refreshResponse, commit = std::move(thresholdHandlers.second)](float value)
                {
                    response->Threshold = std::max(value, 0.0f);
                    refreshResponse();
                    commit(value);
                },
                1.0f, "Luminance threshold for bloom bright-pass", 0.0f);

            auto kneeHandlers = MakeComponentInteractiveHandlers<Bloom, float>(
                w, e, n, undo, "Change Bloom Knee",
                [](Bloom& u, float v) { u.Knee = std::clamp(v, 0.0f, 1.0f); }, {});
            AddFloatRowWithDrag(ctx.Parent, "Knee", effect->Knee,
                [response, refreshResponse, preview = std::move(kneeHandlers.first)](float value)
                {
                    response->Knee = std::clamp(value, 0.0f, 1.0f);
                    refreshResponse();
                    preview(value);
                },
                [response, refreshResponse, commit = std::move(kneeHandlers.second)](float value)
                {
                    response->Knee = std::clamp(value, 0.0f, 1.0f);
                    refreshResponse();
                    commit(value);
                },
                0.1f, "Soft knee width for bloom threshold", 0.0f, 1.0f);
        }

        AddToggleRow(ctx.Parent, "Anti Flicker", effect->AntiFlicker,
            [w, e, n, undo](bool value)
            {
                CommitComponentWithUndo<Bloom>(w, e, n, undo, "Change Bloom Anti Flicker",
                    [value](Bloom& u) { u.AntiFlicker = value; });
            },
            "Suppress isolated hot outliers while preserving small colored highlights.");

        response->Graph = AddBloomResponseCurve(ctx.Parent, response->Threshold, response->Knee);

        AddComponentFloatRowWithDrag<Bloom>(ctx.Parent, "Intensity", effect->Intensity, w, e, n,
            undo, "Change Bloom Intensity",
            [](Bloom& u, float v) { u.Intensity = std::max(0.0f, v); },
            1.0f, "Additive gain of filtered highlights; zero leaves only Diffusion and any depth veil.");
        AddBloomColorRow(
            ctx.Parent, "Tint", "Scene-linear color tint applied to the highlight bloom pyramid",
            "Bloom Tint", "Change Bloom Tint", &Bloom::Tint,
            w, e, n, undo, openPicker, effect->Tint);

        AddComponentFloatRowWithDrag<Bloom>(ctx.Parent, "Diffusion",
            std::clamp(effect->ScatteringAmount, 0.0f, 1.0f), w, e, n, undo,
            "Change Bloom Diffusion",
            [](Bloom& u, float v) { u.ScatteringAmount = std::clamp(v, 0.0f, 1.0f); },
            0.0f, "Mixes a soft, threshold-free haze of the frame over the whole image. Energy-conserving: it "
            "moves light rather than adding it. 0 turns it off and skips its passes. Independent of Intensity.",
            {}, 0.0f, 1.0f);

        // Bounded slider: Radius reads better as a slider than a bare float
        // field. Undo mirrors the drag rows above: MakeComponentInteractiveHandlers
        // coalesces a drag into one "Change Bloom Radius" entry, and a label
        // double-click reset (commit with no prior preview) still records one entry.
        auto radiusHandlers = MakeComponentInteractiveHandlers<Bloom, float>(
            w, e, n, undo, "Change Bloom Radius",
            [](Bloom& u, float v) { u.Radius = std::clamp(v, kBloomRadiusMin, kBloomRadiusMax); }, {});
        auto [radiusLabel, radiusSlider] = AddSliderRow(ctx.Parent, "Radius", effect->Radius,
            kBloomRadiusMin, kBloomRadiusMax,
            "Resolution-independent bloom veil extent; fractional values change smoothly. "
            "Widening is capped by Octaves: the veil keeps spreading until Radius fills the "
            "Octaves ceiling (the full pyramid by default). Lower Octaves to cap the spread.");
        radiusSlider->SetOnValueChanging(
            [preview = std::move(radiusHandlers.first)](const float& value) { preview(value); });
        radiusSlider->SetOnValueChanged(
            [commit = std::move(radiusHandlers.second)](const float& value) { commit(value); });
        SetupLabelDragSlider(radiusLabel, radiusSlider, nullptr, nullptr, kBloomRadiusDefault);

        auto* octaveField = InspectorUI::AddEnumRow(ctx.Parent, "Octaves", kBloomOctaves,
            std::clamp(effect->Octaves, 3, 8),
            "Maximum core bloom pyramid levels (quality/perf ceiling on veil width). Radius selects the "
            "active count for each render resolution; the default is the full pyramid so Radius stays "
            "responsive across its range. Lower it to cap depth for perf. Lens dirt keeps its own wide gather.");
        octaveField->SetOnValueChanged([w, e, n, undo](int32_t value)
        {
            CommitComponentWithUndo<Bloom>(w, e, n, undo, "Change Bloom Octaves",
                [value](Bloom& u) { u.Octaves = std::clamp(value, 3, 8); });
        });

        AddComponentFloatRowWithDrag<Bloom>(ctx.Parent, "Spread",
            std::clamp(effect->Scatter, 0.0f, 1.0f), w, e, n,
            undo, "Change Bloom Spread",
            [](Bloom& u, float v) { u.Scatter = std::clamp(v, 0.0f, 1.0f); },
            0.5f, "How far highlight bloom reaches, from a tight glow to a wide halo; shapes the Diffusion haze the same way",
            {}, 0.0f, 1.0f);
        {
            auto depthVeilControls = std::make_unique<UIElement>();
            UIElement* depthVeilControlsRaw = depthVeilControls.get();
            depthVeilControlsRaw->Overrides()
                .Set(Style::Display, effect->DepthVeilEnabled ? DisplayMode::Flex : DisplayMode::None)
                .Set(Style::FlexDir, FlexDirection::Column)
                .Set(Style::AlignItems, AlignItems::Stretch);

            AddToggleRow(ctx.Parent, "Depth Veil", effect->DepthVeilEnabled,
                [w, e, n, undo, depthVeilControlsRaw](bool value)
                {
                    depthVeilControlsRaw->Overrides().Set(
                        Style::Display, value ? DisplayMode::Flex : DisplayMode::None);
                    CommitComponentWithUndo<Bloom>(w, e, n, undo, "Toggle Bloom Depth Veil",
                        [value](Bloom& u) { u.DepthVeilEnabled = value; });
                },
                "Create a broad atmospheric bloom source from scene depth instead of brightness");

            AddComponentFloatRowWithDrag<Bloom>(depthVeilControlsRaw, "Veil Intensity",
                std::clamp(effect->DepthVeilIntensity, 0.0f, 10.0f), w, e, n,
                undo, "Change Bloom Depth Veil Intensity",
                [](Bloom& u, float v) { u.DepthVeilIntensity = std::clamp(v, 0.0f, 10.0f); },
                1.0f, "Brightness of the depth-driven veil before the bloom pyramid",
                {}, 0.0f, 10.0f);

            AddComponentFloatRowWithDrag<Bloom>(depthVeilControlsRaw, "Distance Start",
                std::max(effect->DepthVeilStart, 0.0f), w, e, n,
                undo, "Change Bloom Depth Veil Start",
                [](Bloom& u, float v) { u.DepthVeilStart = std::clamp(v, 0.0f, 100000.0f); },
                25.0f, "Camera-space distance where the atmospheric veil begins",
                {}, 0.0f, 100000.0f);

            AddComponentFloatRowWithDrag<Bloom>(depthVeilControlsRaw, "Distance End",
                std::max(effect->DepthVeilEnd, 0.0f), w, e, n,
                undo, "Change Bloom Depth Veil End",
                [](Bloom& u, float v) { u.DepthVeilEnd = std::clamp(v, 0.0f, 100000.0f); },
                500.0f, "Camera-space distance where the atmospheric veil reaches full strength",
                {}, 0.0f, 100000.0f);

            AddBloomColorRow(
                depthVeilControlsRaw, "Veil Tint", "Color of the depth-driven atmospheric veil",
                "Bloom Depth Veil Tint", "Change Bloom Depth Veil Tint", &Bloom::DepthVeilTint,
                w, e, n, undo, openPicker, effect->DepthVeilTint);
            ctx.Parent->AddChild(std::move(depthVeilControls));
        }

        auto lensDirtControls = std::make_unique<UIElement>();
        UIElement* lensDirtControlsRaw = lensDirtControls.get();
        lensDirtControlsRaw->Overrides()
            .Set(Style::Display, effect->LensDirtEnabled ? DisplayMode::Flex : DisplayMode::None)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::AlignItems, AlignItems::Stretch);

        AddToggleRow(ctx.Parent, "Lens Dirt", effect->LensDirtEnabled,
            [w, e, n, undo, lensDirtControlsRaw](bool value)
            {
                lensDirtControlsRaw->Overrides().Set(
                    Style::Display, value ? DisplayMode::Flex : DisplayMode::None);
                CommitComponentWithUndo<Bloom>(w, e, n, undo, "Toggle Bloom Lens Dirt",
                    [value](Bloom& u) { u.LensDirtEnabled = value; });
            },
            "Enable lens-dirt modulation independently of its authored intensity");

        auto lensDirtVignetteControls = std::make_unique<UIElement>();
        UIElement* lensDirtVignetteControlsRaw = lensDirtVignetteControls.get();
        lensDirtVignetteControlsRaw->Overrides()
            .Set(Style::Display, effect->LensDirtVignette ? DisplayMode::Flex : DisplayMode::None)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::AlignItems, AlignItems::Stretch);

        AddToggleRow(lensDirtControlsRaw, "Lens Dirt Vignette", effect->LensDirtVignette,
            [w, e, n, undo, lensDirtVignetteControlsRaw](bool value)
            {
                lensDirtVignetteControlsRaw->Overrides().Set(
                    Style::Display, value ? DisplayMode::Flex : DisplayMode::None);
                CommitComponentWithUndo<Bloom>(w, e, n, undo, "Toggle Bloom Lens Dirt Vignette",
                    [value](Bloom& u) { u.LensDirtVignette = value; });
            },
            "Suppress lens dirt at the image center and reveal it toward the edges");

        AddComponentFloatRowWithDrag<Bloom>(lensDirtVignetteControlsRaw, "Vignette Intensity",
            std::clamp(effect->LensDirtVignetteIntensity, 0.0f, 1.0f), w, e, n,
            undo, "Change Bloom Lens Dirt Vignette Intensity",
            [](Bloom& u, float v) { u.LensDirtVignetteIntensity = std::clamp(v, 0.0f, 1.0f); },
            1.0f, "Strength of the clean-center suppression",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Bloom>(lensDirtVignetteControlsRaw, "Vignette Radius",
            std::clamp(effect->LensDirtVignetteRadius, 0.0f, 1.0f), w, e, n,
            undo, "Change Bloom Lens Dirt Vignette Radius",
            [](Bloom& u, float v) { u.LensDirtVignetteRadius = std::clamp(v, 0.0f, 1.0f); },
            0.25f, "Radius of the clean center before lens dirt begins",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Bloom>(lensDirtVignetteControlsRaw, "Vignette Smoothness",
            std::clamp(effect->LensDirtVignetteSmoothness, 0.0f, 1.0f), w, e, n,
            undo, "Change Bloom Lens Dirt Vignette Smoothness",
            [](Bloom& u, float v) { u.LensDirtVignetteSmoothness = std::clamp(v, 0.0f, 1.0f); },
            0.2f, "Falloff sharpness between the clean center and dirt-covered sides",
            {}, 0.0f, 1.0f);

        AddToggleRow(lensDirtVignetteControlsRaw, "Vignette Rounded", effect->LensDirtVignetteRounded,
            [w, e, n, undo](bool value)
            {
                CommitComponentWithUndo<Bloom>(w, e, n, undo, "Change Bloom Lens Dirt Vignette Rounded",
                    [value](Bloom& u) { u.LensDirtVignetteRounded = value; });
            },
            "Aspect-correct the dirt vignette to a circle");

        AddBloomColorRow(
            lensDirtVignetteControlsRaw, "Vignette Color",
            "Tint applied to the lens dirt revealed at the image sides",
            "Bloom Lens Dirt Vignette Color", "Change Bloom Lens Dirt Vignette Color",
            &Bloom::LensDirtVignetteColor, w, e, n, undo, openPicker,
            effect->LensDirtVignetteColor);
        lensDirtControlsRaw->AddChild(std::move(lensDirtVignetteControls));

        AddComponentFloatRowWithDrag<Bloom>(lensDirtControlsRaw, "Lens Dirt Intensity",
            std::clamp(effect->LensDirtIntensity, 0.0f, 10.0f), w, e, n,
            undo, "Change Bloom Lens Dirt Intensity",
            [](Bloom& u, float v) { u.LensDirtIntensity = std::clamp(v, 0.0f, 10.0f); },
            0.0f, "Adds bloom through the lens dirt texture without darkening the scene",
            {}, 0.0f, 10.0f);

        auto& assetManager = EngineCore::GetInstance().GetAssetManager();
        GUID displayedLensDirtGuid = effect->LensDirtTexture.ToGuid();
        if (displayedLensDirtGuid.IsNull())
            displayedLensDirtGuid = assetManager.ResolveAssetGuid("Textures/Bloom/lensDirt1.png");

        AssetField* lensDirtField = InspectorUI::AddAssetFieldRow(
            lensDirtControlsRaw, "Lens Dirt Texture", displayedLensDirtGuid,
            {AssetType::Texture},
            &assetManager.GetRegistry(),
            [w, e, n, undo](const GUID& guid)
            {
                CommitComponentWithUndo<Bloom>(w, e, n, undo, "Change Bloom Lens Dirt Texture",
                    [guid](Bloom& u) { u.LensDirtTexture.Set(guid); });
            },
            ctx.Thumbnails,
            "Lens dirt textures only; defaults to the built-in Sonic Ether texture");
        lensDirtField->SetMetadataFilter([](const AssetIndexRecord& meta)
        {
            return ContainsIgnoreCase(meta.Name, "dirt") ||
                   ContainsIgnoreCase(meta.Path.generic_string(), "dirt");
        });

        AddComponentFloatRowWithDrag<Bloom>(lensDirtControlsRaw, "Lens Dirt Spread",
            std::clamp(effect->LensDirtScatter, 0.0f, 1.0f), w, e, n,
            undo, "Change Bloom Lens Dirt Spread",
            [](Bloom& u, float v) { u.LensDirtScatter = std::clamp(v, 0.0f, 1.0f); },
            0.5f, "Biases lens dirt illumination toward narrow or broad bloom octaves",
            {}, 0.0f, 1.0f);

        ctx.Parent->AddChild(std::move(lensDirtControls));
    };

    InspectorRegistry::Get().RegisterComponentInspector<Bloom>(std::move(fn));
}

} // namespace GameEngine
