#include "Inspectors/MeasureInspector.h"

#include "InspectorRegistry.h"
#include "Platform/SystemMetrics.h"

#include "Components/Measure/MeasureComponent.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UndoRedo/UndoRedoService.h"

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
constexpr float kDefaultMeasureColor[4] = {1.0f, 0.78f, 0.22f, 1.0f};

uint32_t MeasureColorToArgb(const float (&c)[4])
{
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    const uint8_t r = static_cast<uint8_t>(clamp01(c[0]) * 255.0f + 0.5f);
    const uint8_t g = static_cast<uint8_t>(clamp01(c[1]) * 255.0f + 0.5f);
    const uint8_t b = static_cast<uint8_t>(clamp01(c[2]) * 255.0f + 0.5f);
    const uint8_t a = static_cast<uint8_t>(clamp01(c[3]) * 255.0f + 0.5f);
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

void ArgbToMeasureColor(uint32_t argb, float (&out)[4])
{
    out[0] = ((argb >> 16) & 0xFF) / 255.0f;
    out[1] = ((argb >> 8) & 0xFF) / 255.0f;
    out[2] = (argb & 0xFF) / 255.0f;
    out[3] = ((argb >> 24) & 0xFF) / 255.0f;
}

std::string FormatRgb(const float (&c)[4])
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", c[0], c[1], c[2]);
    return buf;
}

void StyleSwatch(UIElement* swatch, uint32_t argb)
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

} // namespace

void RegisterMeasureInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* measure = ctx.World->GetComponent<Components::MeasureComponent>(ctx.Entity);
        if (!measure)
        {
            InspectorUI::AddLine(ctx.Parent, "(Measure missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        OpenColorPickerWindowFn openPicker = ctx.OpenColorPickerWindow;
        using namespace InspectorDrag;

        UIElement* row = InspectorUI::AddRow(ctx.Parent);
        Label* colorLabel = InspectorUI::AddLabel(row, "Color", "Measure color (click to open color picker; double-click to reset)");
        if (colorLabel)
            colorLabel->AddClass("inspector-label-no-drag");

        UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
        fieldContainer->Overrides()
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::Gap, StyleLength::Px(6.0f));

        const uint32_t argb = MeasureColorToArgb(measure->Color);

        auto swatch = std::make_unique<UIElement>();
        UIElement* swatchRaw = swatch.get();
        StyleSwatch(swatchRaw, argb);
        fieldContainer->AddChild(std::move(swatch));

        auto rgbLabel = std::make_unique<Label>();
        rgbLabel->AddClass("inspector-text");
        rgbLabel->SetText(FormatRgb(measure->Color));
        rgbLabel->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
        Label* rgbLabelRaw = rgbLabel.get();
        fieldContainer->AddChild(std::move(rgbLabel));

        // The picker's callbacks outlive an inspector rebuild: resolve the row through weak
        // refs instead of holding the freed widgets.
        auto updateUI = [swatchRef = UIElement::MakeWeakRef(swatchRaw),
                         rgbLabelRef = UIElement::MakeWeakRef(rgbLabelRaw), w, e]()
        {
            auto* c = w->GetComponent<Components::MeasureComponent>(e);
            UIElement* swatch = swatchRef.Get();
            Label* rgbLabel = rgbLabelRef.Get();
            if (!c || !swatch || !rgbLabel)
                return;
            StyleSwatch(swatch, MeasureColorToArgb(c->Color));
            rgbLabel->SetText(FormatRgb(c->Color));
        };

        auto clickHandler = [w, e, n, undo, openPicker, updateUI](UIEvent& ev)
        {
            if (ev.Button != 0)
                return;
            ev.Stop();

            if (!openPicker)
                return;

            auto* comp = w->GetComponent<Components::MeasureComponent>(e);
            if (!comp)
                return;

            const uint32_t currentArgb = MeasureColorToArgb(comp->Color);

            using Edit = Editor::UndoRedoService::InteractiveEdit;
            auto edit = std::make_shared<Edit>();
            if (undo)
            {
                auto target = MakeComponentSnapshotTarget<Components::MeasureComponent>(w, e, n, "Measure Color");
                *edit = undo->BeginInteractiveEdit("Change Measure Color", std::move(target));
            }

            ColorPickerCallbacks cbs;
            cbs.onApply = [w, e, n, edit, updateUI](uint32_t newArgb, float)
            {
                if (*edit)
                {
                    edit->Preview([&]
                    {
                        auto* c = w->GetComponentForWrite<Components::MeasureComponent>(e);
                        if (c)
                            ArgbToMeasureColor(newArgb, c->Color);
                    });
                    edit->Commit();
                }
                else
                {
                    auto* c = w->GetComponent<Components::MeasureComponent>(e);
                    if (!c)
                        return;
                    Components::MeasureComponent updated = *c;
                    ArgbToMeasureColor(newArgb, updated.Color);
                    Editor::CommitComponentUpdate(w, e, n, updated);
                }
                updateUI();
            };
            cbs.onCancel = [edit, updateUI]()
            {
                if (*edit)
                    edit->Cancel();
                updateUI();
            };
            cbs.onValueChanging = [w, e, n, edit, updateUI](uint32_t newArgb, float)
            {
                if (*edit)
                {
                    edit->Preview([&]
                    {
                        auto* c = w->GetComponentForWrite<Components::MeasureComponent>(e);
                        if (c)
                            ArgbToMeasureColor(newArgb, c->Color);
                    });
                }
                else
                {
                    auto* c = w->GetComponent<Components::MeasureComponent>(e);
                    if (!c)
                        return;
                    Components::MeasureComponent updated = *c;
                    ArgbToMeasureColor(newArgb, updated.Color);
                    Editor::PreviewComponentUpdate(w, e, n, updated);
                }
                updateUI();
            };
            openPicker(currentArgb, 1.0f, std::move(cbs));
        };

        swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);
        rgbLabelRaw->RegisterEventHandler(kEventMouseDown, clickHandler);

        auto lastColorClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
        colorLabel->RegisterEventHandler(kEventMouseDown, [w, e, n, undo, updateUI, lastColorClickTime](UIEvent& ev)
        {
            if (ev.Button != 0)
                return;

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastColorClickTime);
            if (elapsed > std::chrono::milliseconds::zero() && elapsed < GameEngine::Platform::GetDoubleClickInterval())
            {
                ev.Stop();
                CommitComponentWithUndo<Components::MeasureComponent>(w, e, n, undo, "Reset Measure Color",
                    [](Components::MeasureComponent& u)
                    {
                        u.Color[0] = kDefaultMeasureColor[0];
                        u.Color[1] = kDefaultMeasureColor[1];
                        u.Color[2] = kDefaultMeasureColor[2];
                        u.Color[3] = kDefaultMeasureColor[3];
                    });
                updateUI();
                *lastColorClickTime = std::chrono::steady_clock::time_point{};
                return;
            }
            *lastColorClickTime = now;
        });
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::MeasureComponent>(std::move(fn));
}

} // namespace GameEngine
