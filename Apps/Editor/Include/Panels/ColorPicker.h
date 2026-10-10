#pragma once

#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "Types/Color.h" // for ColorHSV type
#include <functional>
#include <vector>

namespace GameEngine
{

class Label;
class Slider;
class TextField;
class UIElement;

// HSV (0-360, 0-1, 0-1) + HDR intensity (>= 1 for emissive).
// Note: For simple HSV/RGB conversion without intensity, use `ColorHSV` from Types/Color.h.
struct ColorPickerValue
{
    ColorHSV hsv;            // hue/sat/value/alpha
    float intensity = 1.0f;  // HDR multiplier, >= 1 allows values > 1

    ColorHSV ToColorHSV() const { return hsv; }
    static ColorPickerValue FromColorHSV(const ColorHSV& hsv, float intensity = 1.0f);
};

// Color picker display mode
enum class ColorPickerMode
{
    WheelTriangle,  // Traditional wheel (H=angle, S=radius) + value slider
    BoxHueBar       // SV box + vertical hue bar
};

// Convert between ARGB (0xAARRGGBB) and ColorPickerValue (for persistence / UI).
ColorPickerValue ColorPickerValueFromArgb(uint32_t argb);
uint32_t ColorPickerValueToArgb(const ColorPickerValue& v);

// A polar hue/saturation wheel (angle = hue, radius = saturation/strength) with a
// draggable puck. It has TWO render modes that share the same element, puck, input
// math, and undo path — only the paint differs:
//
//   Default (GradientMode::PolarHSV) — ABSOLUTE color picking. A full HSV wheel
//   (fully-saturated rim, white center). Used by the color-picker popup alongside
//   the SV box, which carries brightness; the full-saturation fill is correct here.
//
//   Grading style (GradientMode::PolarHSVGrading, via SetGradingStyle) — a RELATIVE
//   grading trackball. angle = hue direction, radius = push strength, center = no
//   change. Brightness is intentionally NOT on the wheel — it lives on the band's
//   lightness / Value slider. Painted as a hollow hue annulus (transparent center,
//   thin saturated rim) per the Unity SMH / Unreal grading-wheel convention.
//
// Because only the paint differs, changes to input handling apply to BOTH modes;
// changes to the paint must check m_GradingStyle to target the intended mode.
class ColorWheelElement : public UIElement
{
public:
    ColorWheelElement();
    void SetHue(float h);   // 0..360
    void SetSaturation(float s); // 0..1
    float GetHue() const { return m_Hue; }
    float GetSaturation() const { return m_Saturation; }
    void SetOnChange(std::function<void(float h, float s)> fn) { m_OnChange = std::move(fn); }
    // Fired once when a drag gesture ends (mouse up after a press). Lets callers
    // coalesce a whole drag into a single committed edit: OnChange previews each
    // tick, OnChangeEnd commits once.
    void SetOnChangeEnd(std::function<void()> fn) { m_OnChangeEnd = std::move(fn); }
    // Opt in to the grading-trackball paint (hollow hue annulus: transparent
    // center, thin saturated rim) instead of the default full-saturation
    // color-picker disc. The color-grade band wheels set this; the picker popup
    // does not. See the class comment for the two-mode contract.
    void SetGradingStyle(bool grading) { m_GradingStyle = grading; }
    void OnEvent(UIEvent& e) override;
    void OnPostLayout() override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    void UpdateMarkerPosition();
    float m_Hue = 0.0f;
    float m_Saturation = 1.0f;
    std::function<void(float h, float s)> m_OnChange;
    std::function<void()> m_OnChangeEnd;
    bool m_Dragging = false;
    bool m_GradingStyle = false;
    UIElement* m_Marker = nullptr;
};

// Saturation-Value box: X = Saturation (0..1), Y = Value (1 at top, 0 at bottom)
class ColorSVBoxElement : public UIElement
{
public:
    ColorSVBoxElement();
    void SetHue(float h);   // 0..360 - determines the color of the box
    void SetSaturation(float s); // 0..1
    void SetValue(float v); // 0..1
    float GetHue() const { return m_Hue; }
    float GetSaturation() const { return m_Saturation; }
    float GetValue() const { return m_Value; }
    void SetOnChange(std::function<void(float s, float v)> fn) { m_OnChange = std::move(fn); }
    void OnEvent(UIEvent& e) override;
    void OnPostLayout() override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    void UpdateMarkerPosition();
    float m_Hue = 0.0f;
    float m_Saturation = 1.0f;
    float m_Value = 1.0f;
    std::function<void(float s, float v)> m_OnChange;
    bool m_Dragging = false;
    UIElement* m_Marker = nullptr;
};

// Vertical hue bar: shows full spectrum, draggable to select hue
class HueBarElement : public UIElement
{
public:
    HueBarElement();
    void SetHue(float h);   // 0..360
    float GetHue() const { return m_Hue; }
    void SetOnChange(std::function<void(float h)> fn) { m_OnChange = std::move(fn); }
    void OnEvent(UIEvent& e) override;
    void OnPostLayout() override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    void UpdateMarkerPosition();
    float m_Hue = 0.0f;
    std::function<void(float h)> m_OnChange;
    bool m_Dragging = false;
    UIElement* m_Marker = nullptr;
};

// Vertical value/brightness bar: shows current hue from full brightness (top) to black (bottom)
class ValueBarElement : public UIElement
{
public:
    ValueBarElement();
    void SetHue(float h);
    void SetSaturation(float s);
    void SetValue(float v);
    float GetHue() const { return m_Hue; }
    float GetSaturation() const { return m_Saturation; }
    float GetValue() const { return m_Value; }
    void SetOnChange(std::function<void(float v)> fn) { m_OnChange = std::move(fn); }
    void OnEvent(UIEvent& e) override;
    void OnPostLayout() override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    void UpdateMarkerPosition();
    float m_Hue = 0.0f;
    float m_Saturation = 1.0f;
    float m_Value = 1.0f;
    std::function<void(float v)> m_OnChange;
    bool m_Dragging = false;
    UIElement* m_Marker = nullptr;
};

// Color picker UI element: wheel or box+hue bar, value slider, alpha, HDR intensity, HSV/RGB fields, palettes.
// To show it in a window/popup, use ColorPickerPopup (separate wrapper).
class ColorPicker : public UIElement
{
public:
    ColorPicker();
    void SetValue(const ColorPickerValue& value);
    ColorPickerValue GetValue() const { return m_Value; }
    void SetOnChange(std::function<void(const ColorPickerValue&)> fn) { m_OnChange = std::move(fn); }

    // Eyedropper support: host sets handler that will be called when eyedropper button is clicked.
    // The handler should change cursor to crosshair and capture the next click, then call SetColorFromArgb.
    void SetEyedropperHandler(std::function<void()> fn) { m_EyedropperHandler = std::move(fn); }
    void SetColorFromArgb(uint32_t argb); // applies color from eyedropper

    // Mode switching
    void SetMode(ColorPickerMode mode);
    ColorPickerMode GetMode() const { return m_Mode; }

private:
    void BuildUI();
    void RebuildColorArea(); // rebuilds wheel or box+hue bar based on mode
    void SyncFromValue();
    void SyncToValue();
    void NotifyChange();
    void UpdateSwatchAndFields();
    void OnWheelChange(float h, float s);
    void OnSVBoxChange(float s, float v);
    void OnHueBarChange(float h);
    void OnValueSliderChange(float v);
    void OnAlphaSliderChange(float alpha);
    void OnIntensitySliderChange(float intensity);
    void OnHsvFieldChange();
    void OnRgbFieldChange();
    void OnAlphaFieldChange();
    void OnHexFieldChange();
    void OnPaletteClick(uint32_t index);
    void RemovePaletteColor(uint32_t index);
    void AddColorToPalette(uint32_t argb);
    void SavePaletteToPreferences();
    void ResetPalette();
    void UndoPaletteReset();
    void RebuildPaletteUI();
    void BindPaletteSwatchHandler(UIElement* swatch);
    uint32_t FindPaletteIndexForSwatch(const UIElement* swatch);
    static void HsvToRgb(float h, float s, float v, float& r, float& g, float& b);
    static void RgbToHsv(float r, float g, float b, float& h, float& s, float& v);

    ColorPickerMode m_Mode = ColorPickerMode::WheelTriangle; // Default to wheel mode
    ColorPickerValue m_Value;
    std::function<void(const ColorPickerValue&)> m_OnChange;
    std::function<void()> m_EyedropperHandler;
    
    // Wheel mode elements
    ColorWheelElement* m_Wheel = nullptr;
    ValueBarElement* m_ValueBar = nullptr;
    
    // Box + Hue Bar mode elements
    ColorSVBoxElement* m_SvBox = nullptr;
    HueBarElement* m_HueBar = nullptr;
    UIElement* m_ColorAreaContainer = nullptr; // container for wheel or box+bar
    
    // Common elements
    Slider* m_AlphaSlider = nullptr;
    Label* m_AlphaValueLabel = nullptr;
    TextField* m_AlphaField = nullptr;
    Slider* m_IntensitySlider = nullptr;
    Label* m_IntensityValueLabel = nullptr;
    TextField* m_HField = nullptr;
    TextField* m_SField = nullptr;
    TextField* m_VField = nullptr;
    TextField* m_RField = nullptr;
    TextField* m_GField = nullptr;
    TextField* m_BField = nullptr;
    TextField* m_AField = nullptr; // Alpha text field in RGBA row
    UIElement* m_SwatchEl = nullptr;
    UIElement* m_SwatchCheckerboard = nullptr; // for alpha visualization
    TextField* m_HexField = nullptr;
    UIElement* m_ModeToggleBtn = nullptr;
    UIElement* m_PaletteRow = nullptr;
    std::vector<uint32_t> m_PaletteColors; // ARGB
    std::vector<uint32_t> m_PaletteUndoState; // For undo after reset
    bool m_CanUndoPaletteReset = false;
    bool m_DraggingFromSwatch = false;
};

} // namespace GameEngine
