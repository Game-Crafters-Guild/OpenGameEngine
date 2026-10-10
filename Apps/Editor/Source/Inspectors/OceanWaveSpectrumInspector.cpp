#include "Inspectors/OceanWaveSpectrumInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "Components/ComponentRegistration.h"
#include "Components/Rendering/Ocean.h"
#include "Ocean/OceanTypes.h"
#include "UI/StyleProperties.h"
#include "UndoRedo/UndoRedoService.h"

#include <array>
#include <cstddef>
#include <sstream>
#include <string>

namespace GameEngine
{

using Components::OceanWaveSpectrum;

namespace
{

// Spectrum-editor octave-to-wavelength mapping (the reference's per-band labels):
// octave i represents a wavelength of kSpectrumBaseWavelengthMeters * 2^i meters,
// so the 14 bands span ~6 cm ripples (i=0) to ~512 m swell (i=13).
constexpr float kSpectrumBaseWavelengthMeters = 0.0625f;

// Per-octave power slider domain. Matches the SpectrumPower field range in
// OceanFieldRanges.cpp (the default inspector would draw 0..2 sliders for the raw
// array; this custom view reproduces that range as a labelled per-band row).
constexpr float kSpectrumPowerMin = 0.0f;
constexpr float kSpectrumPowerMax = 2.0f;

// Compact value-field width so the slider keeps the row's horizontal space.
constexpr float kSpectrumValueFieldWidthPx = 56.0f;

// Dim the slider + value of a muted octave so a disabled band reads as inert.
constexpr float kDisabledOctaveOpacity = 0.4f;
constexpr float kEnabledOctaveOpacity = 1.0f;

std::string FormatWavelengthLabel(std::size_t octave)
{
    float wavelength = kSpectrumBaseWavelengthMeters;
    for (std::size_t i = 0; i < octave; ++i)
        wavelength *= 2.0f;

    std::ostringstream oss;
    if (wavelength < 1.0f)
    {
        // Sub-metre bands keep up to four decimals (0.0625, 0.125, 0.25, 0.5),
        // trimmed of trailing zeros.
        oss.setf(std::ios::fixed, std::ios::floatfield);
        oss.precision(4);
        oss << wavelength;
        std::string s = oss.str();
        const std::size_t dot = s.find('.');
        if (dot != std::string::npos)
        {
            std::size_t last = s.find_last_not_of('0');
            if (last == dot)
                --last;
            s.erase(last + 1);
        }
        return s + " m";
    }

    oss << static_cast<int>(wavelength + 0.5f) << " m";
    return oss.str();
}

void ApplySpectrumValueFieldStyle(FloatField* field)
{
    if (!field)
        return;
    field->Overrides()
        .Set(Style::Width, StyleLength::Px(kSpectrumValueFieldWidthPx))
        .Set(Style::FlexShrink, 0.0f);
    // Display at most 3 decimals, trimming trailing zeros (1.000 -> "1",
    // 1.234567 -> "1.235").
    field->SetFormatFunction([](float v) -> std::string
    {
        if (v == 0.0f)
            return "0";
        std::ostringstream oss;
        oss.setf(std::ios::fixed, std::ios::floatfield);
        oss.precision(3);
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
        return s;
    });
}

void SetOctaveRowDimmed(Slider* slider, FloatField* valueField, bool disabled)
{
    const float opacity = disabled ? kDisabledOctaveOpacity : kEnabledOctaveOpacity;
    if (slider)
        slider->Overrides().Set(Style::Opacity, opacity);
    if (valueField)
        valueField->Overrides().Set(Style::Opacity, opacity);
}

void BuildOctaveRow(const InspectorContext& ctx, OceanWaveSpectrum& spec, std::size_t octave)
{
    using namespace InspectorDrag;

    ECS::World* w = ctx.World;
    ECS::EntityHandle e = ctx.Entity;
    Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
    Editor::UndoRedoService* undo = ctx.Undo;
    const auto extras = GetAdditionalEntities(ctx);

    UIElement* row = InspectorUI::AddRow(ctx.Parent);

    // Enable toggle — the same compact switch the default inspector uses for bool
    // fields. Wrapping the toggle in an .inspector-field-toggle cell keeps it at its
    // natural (small) size; a bare row toggle is stretched into a wide pill by the
    // .inspector-row > .toggle CSS. The cell is a fixed narrow width so the slider
    // gets the rest of the row.
    UIElement* toggleCell = InspectorUI::AddFieldContainer(row);
    toggleCell->AddClass("inspector-field-toggle");
    toggleCell->Overrides()
        .Set(Style::FlexGrow, 0.0f)
        .Set(Style::FlexShrink, 0.0f)
        .Set(Style::FlexBasis, StyleLength::Px(36.0f));
    Toggle* toggle = InspectorUI::AddToggle(toggleCell, !spec.OctaveDisabled[octave]);
    toggle->SetTooltip("Enable this wavelength band. Disabled bands contribute no energy.");

    // Wavelength label for the band. Fixed width so the bands align and the slider
    // starts at a consistent x.
    Label* label = InspectorUI::AddLabel(row, FormatWavelengthLabel(octave));
    if (label)
    {
        // Draggable (no inspector-label-no-drag) so SetupLabelDragSlider can scrub
        // the power and double-click can reset it.
        label->Overrides()
            .Set(Style::Width, StyleLength::Px(56.0f))
            .Set(Style::FlexGrow, 0.0f)
            .Set(Style::FlexShrink, 0.0f);
    }

    // Power slider fills the remaining row width (flex-grow on the field cell).
    UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
    fieldContainer->Overrides().Set(Style::FlexGrow, 1.0f);

    auto sliderOwned = std::make_unique<Slider>();
    Slider* slider = sliderOwned.get();
    slider->AddClass("property-slider");
    slider->SetMin(kSpectrumPowerMin);
    slider->SetMax(kSpectrumPowerMax);
    slider->SetShowValueBubble(true);
    slider->SetValueWithoutNotify(spec.SpectrumPower[octave]);
    fieldContainer->AddChild(std::move(sliderOwned));

    // Compact numeric value field on the far right.
    FloatField* valueField = InspectorUI::AddFloat(row, spec.SpectrumPower[octave]);
    valueField->SetValueWithoutNotify(spec.SpectrumPower[octave]);
    ApplySpectrumValueFieldStyle(valueField);

    SetOctaveRowDimmed(slider, valueField, spec.OctaveDisabled[octave]);

    toggle->SetOnValueChanged([w, e, n, undo, octave, slider, valueField](bool enabled) {
        CommitComponentWithUndo<OceanWaveSpectrum>(
            w, e, n, undo, "Toggle Spectrum Octave",
            [octave, enabled](OceanWaveSpectrum& s) { s.OctaveDisabled[octave] = !enabled; });
        SetOctaveRowDimmed(slider, valueField, !enabled);
    });

    auto handlers = MakeComponentInteractiveHandlers<OceanWaveSpectrum, float>(
        w,
        e,
        n,
        undo,
        "Edit Spectrum Power",
        [octave](OceanWaveSpectrum& spectrum, float value)
        {
            spectrum.SpectrumPower[octave] = std::clamp(value, kSpectrumPowerMin, kSpectrumPowerMax);
        },
        extras);
    auto syncControls = [slider, valueField](float value)
    {
        const float clamped = std::clamp(value, kSpectrumPowerMin, kSpectrumPowerMax);
        slider->SetValueWithoutNotify(clamped);
        valueField->SetValueWithoutNotify(clamped);
        return clamped;
    };

    // Slider and typed-field gestures share one interactive edit. The original
    // snapshot is captured before the first preview and committed once on release.
    slider->SetOnValueChanging([syncControls, preview = handlers.first](const float& value) mutable
    {
        preview(syncControls(value));
    });
    slider->SetOnValueChanged([syncControls, commit = handlers.second](const float& value) mutable
    {
        commit(syncControls(value));
    });

    // Drag the wavelength label to scrub the band's power; double-click it to reset
    // the band to the default power (1). The reset calls slider->NotifyValueChanged,
    // which fires the slider's OnValueChanged above (value field + undo commit), so
    // no extra callbacks are needed here.
    SetupLabelDragSlider(label, slider, nullptr, nullptr, /*defaultValue*/ 1.0f);

    valueField->SetOnValueChanging([syncControls, preview = handlers.first](const float& value) mutable
    {
        preview(syncControls(value));
    });
    valueField->SetOnValueChanged([syncControls, commit = handlers.second](const float& value) mutable
    {
        commit(syncControls(value));
    });
}

} // namespace

void RegisterOceanWaveSpectrumInspector()
{
    // The raw per-octave arrays would otherwise render as flat 14-element slider
    // lists in the default inspector. Hide them so the custom per-band rows below
    // are the only spectrum view. ChopScales/GravityScales have no per-band widget
    // yet; hiding them keeps the section focused on the power editor.
    using Components::SetReflectedFieldFlags;
    using ECS::FieldFlags;
    SetReflectedFieldFlags<OceanWaveSpectrum>("SpectrumPower", FieldFlags::Hidden);
    SetReflectedFieldFlags<OceanWaveSpectrum>("ChopScales", FieldFlags::Hidden);
    SetReflectedFieldFlags<OceanWaveSpectrum>("GravityScales", FieldFlags::Hidden);
    SetReflectedFieldFlags<OceanWaveSpectrum>("OctaveDisabled", FieldFlags::Hidden);

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        // Scalar fields (WindSpeed, Multiplier, Chop, Weight, ...) via the
        // reflection-driven default drawer; the hidden arrays are skipped.
        RenderDefaultComponentInspector(ctx, ECS::GetComponentTypeId<OceanWaveSpectrum>());

        auto* spec = ctx.World->GetComponentForWrite<OceanWaveSpectrum>(ctx.Entity);
        if (!spec)
            return;

        InspectorUI::AddTextBlock(ctx.Parent, "Spectrum (per wavelength)", "inspector-section-subheader");

        for (std::size_t octave = 0; octave < Ocean::kOceanSpectrumOctaves; ++octave)
            BuildOctaveRow(ctx, *spec, octave);
    };

    InspectorRegistry::Get().RegisterComponentInspector<OceanWaveSpectrum>(std::move(fn));
}

} // namespace GameEngine
