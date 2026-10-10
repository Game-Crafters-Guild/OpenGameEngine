#include "Inspectors/VignetteEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/VignetteEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UndoRedo/UndoRedoService.h"
#include "UI/StyleProperties.h"

#include <algorithm>
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
constexpr Components::VignetteEffect kDefaults{};

uint32_t ColorToArgb(const float (&c)[3])
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    uint8_t r = static_cast<uint8_t>(clamp01(c[0]) * 255.0f + 0.5f);
    uint8_t g = static_cast<uint8_t>(clamp01(c[1]) * 255.0f + 0.5f);
    uint8_t b = static_cast<uint8_t>(clamp01(c[2]) * 255.0f + 0.5f);
    return (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

void ArgbToColor(uint32_t argb, float (&out)[3])
{
    out[0] = ((argb >> 16) & 0xFF) / 255.0f;
    out[1] = ((argb >> 8) & 0xFF) / 255.0f;
    out[2] = (argb & 0xFF) / 255.0f;
}

std::string FormatRgb(const float (&c)[3])
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", c[0], c[1], c[2]);
    return buf;
}

void StyleSwatch(UIElement* swatch, uint32_t argb)
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

void RegisterVignetteEffectInspector()
{
    using namespace InspectorDrag;
    using V = Components::VignetteEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<V>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;

        AddComponentFloatRowWithDrag<V>(ctx.Parent, "Intensity",
            std::clamp(effect->Intensity, 0.0f, 1.0f), w, e, n, undo,
            "Change Vignette Intensity",
            [](V& u, float v) { u.Intensity = std::clamp(v, 0.0f, 1.0f); },
            kDefaults.Intensity,
            "Corner darkening amount (0 = off).",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<V>(ctx.Parent, "Smoothness",
            std::clamp(effect->Smoothness, 0.0f, 1.0f), w, e, n, undo,
            "Change Vignette Smoothness",
            [](V& u, float v) { u.Smoothness = std::clamp(v, 0.0f, 1.0f); },
            kDefaults.Smoothness,
            "Falloff sharpness toward the corners.",
            {}, 0.0f, 1.0f);

        AddToggleRow(ctx.Parent, "Rounded", effect->Rounded,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<V>(w, e, n, undo, "Change Vignette Rounded",
                    [v](V& u) { u.Rounded = v; });
            },
            "Aspect-correct the falloff to a circle (off = follows screen aspect).");

        // Color swatch + RGB label; click opens the color picker.
        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            Label* colorLabel = InspectorUI::AddLabel(row, "Color", "Tint the corners fade toward (click to pick; double-click to reset to black)");
            if (colorLabel) colorLabel->AddClass("inspector-label-no-drag");
            UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
            fieldContainer->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(6.0f));

            auto swatch = std::make_unique<UIElement>();
            UIElement* swatchRaw = swatch.get();
            StyleSwatch(swatchRaw, ColorToArgb(effect->Color));
            fieldContainer->AddChild(std::move(swatch));

            auto rgbLabel = std::make_unique<Label>();
            rgbLabel->AddClass("inspector-text");
            rgbLabel->SetText(FormatRgb(effect->Color));
            rgbLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
            Label* rgbLabelRaw = rgbLabel.get();
            fieldContainer->AddChild(std::move(rgbLabel));

            auto updateUI = [swatchRaw, rgbLabelRaw, w, e]() {
                auto* c = w->GetComponent<V>(e);
                if (!c) return;
                StyleSwatch(swatchRaw, ColorToArgb(c->Color));
                rgbLabelRaw->SetText(FormatRgb(c->Color));
            };

            auto clickHandler = [w, e, n, undo, openPicker, updateUI](UIEvent& ev) {
                if (ev.Button != 0)
                    return;
                ev.Stop();
                if (!openPicker)
                    return;
                auto* comp = w->GetComponent<V>(e);
                if (!comp)
                    return;

                using Edit = Editor::UndoRedoService::InteractiveEdit;
                auto edit = std::make_shared<Edit>();
                if (undo)
                {
                    auto target = MakeComponentSnapshotTarget<V>(w, e, n, "Vignette Color");
                    *edit = undo->BeginInteractiveEdit("Change Vignette Color", std::move(target));
                }

                ColorPickerCallbacks cbs;
                cbs.onApply = [w, e, n, edit, updateUI](uint32_t newArgb, float) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<V>(e);
                            if (c) ArgbToColor(newArgb, c->Color);
                        });
                        edit->Commit();
                    }
                    else
                    {
                        auto* c = w->GetComponent<V>(e);
                        if (!c) return;
                        V updated = *c;
                        ArgbToColor(newArgb, updated.Color);
                        Editor::CommitComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                cbs.onCancel = [edit, updateUI]() {
                    if (*edit) edit->Cancel();
                    updateUI();
                };
                cbs.onValueChanging = [w, e, n, edit, updateUI](uint32_t newArgb, float) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<V>(e);
                            if (c) ArgbToColor(newArgb, c->Color);
                        });
                    }
                    else
                    {
                        auto* c = w->GetComponent<V>(e);
                        if (!c) return;
                        V updated = *c;
                        ArgbToColor(newArgb, updated.Color);
                        Editor::PreviewComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                openPicker(ColorToArgb(comp->Color), 1.0f, std::move(cbs));
            };

            swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
            rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
        }
    };

    InspectorRegistry::Get().RegisterComponentInspector<V>(std::move(fn));
}

} // namespace GameEngine
