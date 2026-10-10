#include "Inspectors/ColorFilterEffectInspector.h"

#include "InspectorRegistry.h"
#include "Platform/SystemMetrics.h"

#include "Components/Rendering/PostProcessEffects/ColorFilterEffect.h"
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
using Components::ColorFilterBlendMode;

constexpr float kSwatchSize = 20.0f;
constexpr float kSwatchBorderRadius = 3.0f;
constexpr uint32_t kSwatchBorderColor = 0xFF555555u;

static constexpr EnumEntry<ColorFilterBlendMode> kBlendModes[] = {
    {ColorFilterBlendMode::Multiply, "Multiply"},
    {ColorFilterBlendMode::Add,      "Add"},
    {ColorFilterBlendMode::Screen,   "Screen"},
    {ColorFilterBlendMode::SoftLight,"Soft Light"},
};

static uint32_t ColorToArgb(const float (&c)[3])
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    uint8_t r = static_cast<uint8_t>(clamp01(c[0]) * 255.0f + 0.5f);
    uint8_t g = static_cast<uint8_t>(clamp01(c[1]) * 255.0f + 0.5f);
    uint8_t b = static_cast<uint8_t>(clamp01(c[2]) * 255.0f + 0.5f);
    return (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

static void ArgbToColor(uint32_t argb, float (&out)[3])
{
    out[0] = ((argb >> 16) & 0xFF) / 255.0f;
    out[1] = ((argb >> 8) & 0xFF) / 255.0f;
    out[2] = (argb & 0xFF) / 255.0f;
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

void RegisterColorFilterEffectInspector()
{
    using namespace InspectorDrag;
    using CF = Components::ColorFilterEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<CF>(ctx.Entity);
        if (!effect)
        {
            InspectorUI::AddLine(ctx.Parent, "(ColorFilterEffect missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;

        auto* blendField = InspectorUI::AddEnumRow(ctx.Parent, "BlendMode", kBlendModes, effect->BlendMode,
            "How tint is combined with scene color");
        blendField->SetOnValueChanged([w, e, n, undo](ColorFilterBlendMode v) {
            CommitComponentWithUndo<CF>(w, e, n, undo, "Change ColorFilter Blend Mode",
                [v](CF& u) { u.BlendMode = v; });
        });

        // Color swatch + RGB label, click opens color picker.
        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            Label* colorLabel = InspectorUI::AddLabel(row, "Color", "Tint color (click to open color picker; double-click to reset to white)");
            if (colorLabel) colorLabel->AddClass("inspector-label-no-drag");
            UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
            fieldContainer->Overrides()
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center)
                .Set(Style::Gap, StyleLength::Px(6.0f));

            const uint32_t argb = ColorToArgb(effect->Color);

            auto swatch = std::make_unique<UIElement>();
            UIElement* swatchRaw = swatch.get();
            StyleSwatch(swatchRaw, argb);
            fieldContainer->AddChild(std::move(swatch));

            auto rgbLabel = std::make_unique<Label>();
            rgbLabel->AddClass("inspector-text");
            rgbLabel->SetText(FormatRgb(effect->Color));
            rgbLabel->Overrides()
                .Set(Style::Cursor, CursorStyle::Pointer);
            Label* rgbLabelRaw = rgbLabel.get();
            fieldContainer->AddChild(std::move(rgbLabel));

            // The picker's callbacks outlive an inspector rebuild: resolve the row through
            // weak refs instead of holding the freed widgets.
            auto clickHandler = [w, e, n, undo, openPicker,
                                 swatchRef = UIElement::MakeWeakRef(swatchRaw),
                                 rgbLabelRef = UIElement::MakeWeakRef(rgbLabelRaw)](UIEvent& ev) {
                if (ev.Button != 0)
                    return;
                ev.Stop();

                if (!openPicker)
                    return;

                auto* comp = w->GetComponent<CF>(e);
                if (!comp)
                    return;

                uint32_t currentArgb = ColorToArgb(comp->Color);

                using Edit = Editor::UndoRedoService::InteractiveEdit;
                auto edit = std::make_shared<Edit>();

                if (undo)
                {
                    auto target = MakeComponentSnapshotTarget<CF>(w, e, n, "ColorFilter Color");
                    *edit = undo->BeginInteractiveEdit("Change ColorFilter Color", std::move(target));
                }

                auto updateUI = [swatchRef, rgbLabelRef, w, e]() {
                    auto* c = w->GetComponent<CF>(e);
                    UIElement* swatch = swatchRef.Get();
                    Label* rgbLabel = rgbLabelRef.Get();
                    if (!c || !swatch || !rgbLabel) return;
                    StyleSwatch(swatch, ColorToArgb(c->Color));
                    rgbLabel->SetText(FormatRgb(c->Color));
                };

                ColorPickerCallbacks cbs;
                cbs.onApply = [w, e, n, edit, updateUI](uint32_t newArgb, float) {
                    if (*edit)
                    {
                        edit->Preview([&] {
                            auto* c = w->GetComponentForWrite<CF>(e);
                            if (c) ArgbToColor(newArgb, c->Color);
                        });
                        edit->Commit();
                    }
                    else
                    {
                        auto* c = w->GetComponent<CF>(e);
                        if (!c) return;
                        CF updated = *c;
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
                            auto* c = w->GetComponentForWrite<CF>(e);
                            if (c) ArgbToColor(newArgb, c->Color);
                        });
                    }
                    else
                    {
                        auto* c = w->GetComponent<CF>(e);
                        if (!c) return;
                        CF updated = *c;
                        ArgbToColor(newArgb, updated.Color);
                        Editor::PreviewComponentUpdate(w, e, n, updated);
                    }
                    updateUI();
                };
                openPicker(currentArgb, 1.0f, std::move(cbs));
            };

            swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
            rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);

            // Double-click on the "Color" label resets to white.
            auto lastColorClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
            colorLabel->RegisterEventHandler(kEventMouseDown, [w, e, n, undo, lastColorClickTime](UIEvent& ev) {
                if (ev.Button != 0)
                    return;
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastColorClickTime);
                if (elapsed > std::chrono::milliseconds::zero() && elapsed < GameEngine::Platform::GetDoubleClickInterval())
                {
                    ev.Stop();
                    CommitComponentWithUndo<CF>(w, e, n, undo, "Reset ColorFilter Color",
                        [](CF& u) { u.Color[0] = 1.0f; u.Color[1] = 1.0f; u.Color[2] = 1.0f; });
                    *lastColorClickTime = std::chrono::steady_clock::time_point{};
                    return;
                }
                *lastColorClickTime = now;
            });
        }

        AddComponentFloatRowWithDrag<CF>(ctx.Parent, "Intensity", effect->Intensity, w, e, n,
            undo, "Change ColorFilter Intensity",
            [](CF& u, float v) { u.Intensity = std::clamp(v, 0.0f, 1.0f); },
            1.0f, "Blend amount: 0 = pass-through, 1 = full tint");
    };

    InspectorRegistry::Get().RegisterComponentInspector<CF>(std::move(fn));
}

} // namespace GameEngine
