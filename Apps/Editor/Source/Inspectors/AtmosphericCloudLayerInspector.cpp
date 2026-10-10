#include "Inspectors/AtmosphericCloudLayerInspector.h"

#include "InspectorRegistry.h"
#include "Platform/SystemMetrics.h"

#include "Components/Rendering/PostProcessEffects/AtmosphericCloudLayer.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine
{

namespace
{
constexpr float kSwatchSize = 20.0f;
constexpr float kSwatchBorderRadius = 3.0f;
constexpr uint32_t kSwatchBorderColor = 0xFF555555u;

static void CloudRgbToPickerState(const float (&c)[3], uint32_t& outArgb, float& outIntensity)
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    const float maxChannel = std::max(std::max(c[0], c[1]), c[2]);
    outIntensity = std::clamp(maxChannel, 1.0f,
                              Components::kAtmosphericCloudColorMaxIntensity);
    const float invIntensity = 1.0f / outIntensity;
    const uint8_t r = static_cast<uint8_t>(clamp01(c[0] * invIntensity) * 255.0f + 0.5f);
    const uint8_t g = static_cast<uint8_t>(clamp01(c[1] * invIntensity) * 255.0f + 0.5f);
    const uint8_t b = static_cast<uint8_t>(clamp01(c[2] * invIntensity) * 255.0f + 0.5f);
    outArgb = (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) |
              (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static void PickerStateToCloudRgb(uint32_t argb, float intensity, float (&out)[3])
{
    const float resolvedIntensity = std::clamp(
        intensity, 1.0f, Components::kAtmosphericCloudColorMaxIntensity);
    out[0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f * resolvedIntensity;
    out[1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f * resolvedIntensity;
    out[2] = static_cast<float>(argb & 0xFFu) / 255.0f * resolvedIntensity;
}

static uint32_t CloudRgbToArgb(const float (&c)[3])
{
    uint32_t argb = 0;
    float intensity = 1.0f;
    CloudRgbToPickerState(c, argb, intensity);
    return argb;
}

static std::string FormatRgb(const float (&c)[3])
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", c[0], c[1], c[2]);
    return buf;
}

static void StyleSwatch(UIElement* swatch, uint32_t argb)
{
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

} // namespace

void RegisterAtmosphericCloudLayerInspector()
{
    using namespace InspectorDrag;
    using Layer = Components::AtmosphericCloudLayer;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* layer = ctx.World->GetComponent<Layer>(ctx.Entity);
        if (!layer)
        {
            InspectorUI::AddLine(ctx.Parent, "(AtmosphericCloudLayer missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "SkyFill", layer->SkyFill, w, e, n,
            undo, "Change Cloud Sky Fill",
            [](Layer& u, float v) { u.SkyFill = std::clamp(v, 0.0f, 1.0f); },
            0.48f, "How much of the open sky can be occupied", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "VaporMass", layer->VaporMass, w, e, n,
            undo, "Change Cloud Vapor Mass",
            [](Layer& u, float v) { u.VaporMass = std::clamp(v, 0.0f, 1.0f); },
            0.62f, "Overall visual weight of the layer", {}, 0.0f, 1.0f);

        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            Label* colorLabel = InspectorUI::AddLabel(row, "CloudColor", "HDR cloud scattering color (click to open color picker; double-click to reset to white)");
            if (colorLabel) colorLabel->AddClass("inspector-label-no-drag");
            UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
            fieldContainer->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(6.0f));

            const uint32_t argb = CloudRgbToArgb(layer->CloudColor);

            auto swatch = std::make_unique<UIElement>();
            UIElement* swatchRaw = swatch.get();
            StyleSwatch(swatchRaw, argb);
            fieldContainer->AddChild(std::move(swatch));

            auto rgbLabel = std::make_unique<Label>();
            rgbLabel->AddClass("inspector-text");
            rgbLabel->SetText(FormatRgb(layer->CloudColor));
            rgbLabel->Overrides()
                .Set(Style::Cursor, CursorStyle::Pointer);
            Label* rgbLabelRaw = rgbLabel.get();
            fieldContainer->AddChild(std::move(rgbLabel));

            // The picker's callbacks outlive an inspector rebuild: resolve the row through
            // weak refs instead of holding the freed widgets.
            auto updateUI = [swatchRef = UIElement::MakeWeakRef(swatchRaw),
                             rgbLabelRef = UIElement::MakeWeakRef(rgbLabelRaw), w, e]() {
                auto* c = w->GetComponent<Layer>(e);
                UIElement* swatch = swatchRef.Get();
                Label* rgbLabel = rgbLabelRef.Get();
                if (!c || !swatch || !rgbLabel) return;
                StyleSwatch(swatch, CloudRgbToArgb(c->CloudColor));
                rgbLabel->SetText(FormatRgb(c->CloudColor));
            };

            auto clickHandler = [w, e, n, undo, openPicker, updateUI](UIEvent& ev) {
                if (ev.Button != 0)
                    return;
                ev.Stop();

                if (!openPicker)
                    return;

                auto* comp = w->GetComponent<Layer>(e);
                if (!comp)
                    return;

                uint32_t currentArgb = 0;
                float currentIntensity = 1.0f;
                CloudRgbToPickerState(comp->CloudColor, currentArgb, currentIntensity);

                using Edit = Editor::UndoRedoService::InteractiveEdit;
                auto edit = std::make_shared<Edit>();

                if (undo)
                {
                    auto target = MakeComponentSnapshotTarget<Layer>(w, e, n, "Atmospheric Cloud Color");
                    *edit = undo->BeginInteractiveEdit("Change Atmospheric Cloud Color", std::move(target));
                }

                ColorPickerCallbacks cbs;
                cbs.onApply = [w, e, n, edit, updateUI](uint32_t newArgb, float newIntensity) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<Layer>(e);
                            if (c) PickerStateToCloudRgb(newArgb, newIntensity, c->CloudColor);
                        });
                        edit->Commit();
                    }
                    else
                    {
                        auto* c = w->GetComponent<Layer>(e);
                        if (!c) return;
                        Layer updated = *c;
                        PickerStateToCloudRgb(newArgb, newIntensity, updated.CloudColor);
                        Editor::CommitComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                cbs.onCancel = [edit, updateUI]() {
                    if (*edit) edit->Cancel();
                    updateUI();
                };
                cbs.onValueChanging = [w, e, n, edit, updateUI](uint32_t newArgb, float newIntensity) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<Layer>(e);
                            if (c) PickerStateToCloudRgb(newArgb, newIntensity, c->CloudColor);
                        });
                    }
                    else
                    {
                        auto* c = w->GetComponent<Layer>(e);
                        if (!c) return;
                        Layer updated = *c;
                        PickerStateToCloudRgb(newArgb, newIntensity, updated.CloudColor);
                        Editor::PreviewComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                openPicker(currentArgb, currentIntensity, std::move(cbs));
            };

            swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
            rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);

            auto lastColorClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
            if (colorLabel)
            {
                colorLabel->RegisterEventHandler(kEventMouseDown, [w, e, n, undo, updateUI, lastColorClickTime](UIEvent& ev) {
                    if (ev.Button != 0)
                        return;
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastColorClickTime);
                    if (elapsed > std::chrono::milliseconds::zero() && elapsed < GameEngine::Platform::GetDoubleClickInterval())
                    {
                        ev.Stop();
                        CommitComponentWithUndo<Layer>(w, e, n, undo, "Reset Atmospheric Cloud Color",
                            [](Layer& u) { u.CloudColor[0] = 1.0f; u.CloudColor[1] = 1.0f; u.CloudColor[2] = 1.0f; });
                        updateUI();
                        *lastColorClickTime = std::chrono::steady_clock::time_point{};
                        return;
                    }
                    *lastColorClickTime = now;
                });
            }
        }

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "Opacity", layer->Opacity, w, e, n,
            undo, "Change Cloud Opacity",
            [](Layer& u, float v) { u.Opacity = std::clamp(v, 0.0f, 8.0f); },
            1.0f, "Optical thickness independent of coverage; raise it to suppress background color showing through the clouds", {}, 0.0f, 8.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "FloorHeight", layer->FloorHeight, w, e, n,
            undo, "Change Cloud Floor Height",
            [](Layer& u, float v) { u.FloorHeight = std::max(0.0f, v); },
            900.0f, "Lower visual altitude for the volume", {}, 0.0f, 20000.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "LayerDepth", layer->LayerDepth, w, e, n,
            undo, "Change Cloud Layer Depth",
            [](Layer& u, float v) { u.LayerDepth = std::max(1.0f, v); },
            1800.0f, "Vertical span of the volume", {}, 1.0f, 40000.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "BodyFrequency", layer->BodyFrequency, w, e, n,
            undo, "Change Cloud Body Frequency",
            [](Layer& u, float v) { u.BodyFrequency = std::max(0.01f, v); },
            0.72f, "Large form pattern frequency", {}, 0.01f, 12.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "EdgeFrequency", layer->EdgeFrequency, w, e, n,
            undo, "Change Cloud Edge Frequency",
            [](Layer& u, float v) { u.EdgeFrequency = std::max(0.01f, v); },
            3.2f, "Smaller breakup pattern frequency", {}, 0.01f, 24.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "EdgeBreakup", layer->EdgeBreakup, w, e, n,
            undo, "Change Cloud Edge Breakup",
            [](Layer& u, float v) { u.EdgeBreakup = std::clamp(v, 0.0f, 1.0f); },
            0.58f, "How strongly small patterns carve the edges", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "DriftAngle", layer->DriftAngle, w, e, n,
            undo, "Change Cloud Drift Angle",
            [](Layer& u, float v) { u.DriftAngle = v; },
            24.0f, "Direction of procedural layer drift");

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "DriftRate", layer->DriftRate, w, e, n,
            undo, "Change Cloud Drift Rate",
            [](Layer& u, float v) { u.DriftRate = v; },
            0.08f, "Amount of drift offset applied to the pattern");

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "SunFade", layer->SunFade, w, e, n,
            undo, "Change Cloud Sun Fade",
            [](Layer& u, float v) { u.SunFade = std::clamp(v, 0.0f, 1.0f); },
            0.55f, "Backlit fade near the upper sky direction", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "SkyBounce", layer->SkyBounce, w, e, n,
            undo, "Change Cloud Sky Bounce",
            [](Layer& u, float v) { u.SkyBounce = std::clamp(v, 0.0f, 2.0f); },
            0.32f, "Ambient sky light mixed into the cloud color", {}, 0.0f, 2.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "RimBoost", layer->RimBoost, w, e, n,
            undo, "Change Cloud Rim Boost",
            [](Layer& u, float v) { u.RimBoost = std::clamp(v, 0.0f, 2.0f); },
            0.42f, "Brightening on thinner lit edges", {}, 0.0f, 2.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "Occlusion", layer->Occlusion, w, e, n,
            undo, "Change Cloud Occlusion",
            [](Layer& u, float v) { u.Occlusion = std::clamp(v, 0.0f, 1.0f); },
            0.55f, "Self-shadowing amount for heavier regions", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "HistoryWeight", layer->HistoryWeight, w, e, n,
            undo, "Change Cloud History Weight",
            [](Layer& u, float v) { u.HistoryWeight = std::clamp(v, 0.0f, 0.99f); },
            0.90f, "Reserved blend weight for temporal smoothing", {}, 0.0f, 0.99f);

        AddComponentFloatRowWithDrag<Layer>(ctx.Parent, "PixelScale", layer->PixelScale, w, e, n,
            undo, "Change Cloud Pixel Scale",
            [](Layer& u, float v) { u.PixelScale = std::clamp(v, 0.25f, 1.0f); },
            0.75f, "Reserved internal resolution scale", {}, 0.25f, 1.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Layer>(std::move(fn));
}

} // namespace GameEngine
