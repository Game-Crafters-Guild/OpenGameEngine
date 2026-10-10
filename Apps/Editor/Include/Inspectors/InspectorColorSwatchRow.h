#pragma once

#include "Inspectors/InspectorUIHelpers.h"
#include "Types/Color.h"
#include "Types/ColorUtils.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine::InspectorUI
{

/// The inspector's colour chip: one size and border for every colour row, whatever owns the
/// value behind it. Callers hand it sRGB-encoded ARGB — the picker and the CSS palette both
/// speak sRGB bytes, so a linear value is encoded on the way in.
inline constexpr float kColorSwatchSizePx = 20.0f;
inline constexpr float kColorSwatchBorderRadiusPx = 3.0f;
inline constexpr float kColorSwatchGapPx = 6.0f;
inline constexpr std::uint32_t kColorSwatchBorderArgb = 0xFF555555u;

inline void StyleColorSwatch(UIElement* swatch, std::uint32_t argb)
{
    if (!swatch)
        return;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(kColorSwatchSizePx))
        .Set(Style::Height, StyleLength::Px(kColorSwatchSizePx))
        .Set(Style::MinWidth, StyleLength::Px(kColorSwatchSizePx))
        .Set(Style::MinHeight, StyleLength::Px(kColorSwatchSizePx))
        .Set(Style::BorderRadius,
             CornerRadiiTLTRBRBL{kColorSwatchBorderRadiusPx, kColorSwatchBorderRadiusPx,
                                 kColorSwatchBorderRadiusPx, kColorSwatchBorderRadiusPx})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor,
             BorderColorsTRBL{kColorSwatchBorderArgb, kColorSwatchBorderArgb,
                              kColorSwatchBorderArgb, kColorSwatchBorderArgb})
        .Set(Style::BackgroundColor, argb)
        .Set(Style::Cursor, CursorStyle::Pointer);
}

/// What the colour picker opens on for an HDR linear colour: a normalized swatch and an intensity
/// that multiplies it back. The intensity is the brightest RGB channel, clamped to
/// [1, maxIntensity]; the swatch is the colour divided by it, as rounded ARGB bytes.
struct ColorPickerState
{
    std::uint32_t Argb = 0xFFFFFFFFu;
    float Intensity = 1.0f;
};

inline ColorPickerState HdrColorToPickerState(const ColorLinear& color, float maxIntensity)
{
    const float intensity = std::clamp(std::max({color.r, color.g, color.b}), 1.0f, maxIntensity);
    return {ColorUtils::PackArgbRounded(color.r / intensity, color.g / intensity, color.b / intensity, color.a),
            intensity};
}

/// The HDR linear colour a picker state stands for; the inverse of HdrColorToPickerState.
inline ColorLinear PickerStateToHdrColor(std::uint32_t argb, float intensity, float maxIntensity)
{
    const float resolved = std::clamp(intensity, 1.0f, maxIntensity);
    const ColorLinear swatch = ColorUtils::UnpackArgb(argb);
    return {swatch.r * resolved, swatch.g * resolved, swatch.b * resolved, swatch.a};
}

/// How every inspector prints a colour beside its chip.
inline std::string FormatColorRgb(float r, float g, float b)
{
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "(%.2f, %.2f, %.2f)", r, g, b);
    return buffer;
}

struct ColorSwatchRow
{
    UIElement* Swatch = nullptr;
    Label* Value = nullptr;
};

/// A labelled colour row: chip plus its value text. Both halves are click targets for the
/// picker — the caller registers the handler, because what an edit commits to (and how it
/// undoes) belongs to whoever owns the value, not to the row.
inline ColorSwatchRow AddColorSwatchRow(UIElement* parent, const std::string& labelText,
                                        const char* tooltip, std::uint32_t argb,
                                        const std::string& valueText)
{
    ColorSwatchRow row;
    if (!parent)
        return row;

    UIElement* rowElement = InspectorUI::AddRow(parent);
    if (Label* label = InspectorUI::AddLabel(rowElement, labelText, tooltip))
        label->AddClass("inspector-label-no-drag");

    UIElement* field = InspectorUI::AddFieldContainer(rowElement);
    if (!field)
        return row;
    field->Overrides()
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(kColorSwatchGapPx));

    auto swatch = std::make_unique<UIElement>();
    row.Swatch = swatch.get();
    StyleColorSwatch(row.Swatch, argb);
    field->AddChild(std::move(swatch));

    auto value = std::make_unique<Label>();
    row.Value = value.get();
    row.Value->AddClass("inspector-text");
    row.Value->SetText(valueText);
    row.Value->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
    field->AddChild(std::move(value));

    return row;
}

} // namespace GameEngine::InspectorUI
