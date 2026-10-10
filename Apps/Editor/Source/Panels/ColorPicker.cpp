#include "Panels/ColorPicker.h"
#include "UI/Layout/ElementOverrideHelpers.h"

#include "Editor/Settings/SettingsStore.h"
#include "Platform/SystemMetrics.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"
#include "UI/UiContext.h"
#include "UI/UIPrimitive.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/TextField.h"
#include "Types/ColorUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace GameEngine
{

ColorPickerValue ColorPickerValue::FromColorHSV(const ColorHSV& hsv, float intensity)
{
    ColorPickerValue value;
    value.hsv = hsv;
    value.intensity = intensity;
    return value;
}

namespace
{
constexpr float kPi = 3.14159265358979323846f;

// Sentinel for an empty palette slot (keeps layout when color is "removed")
constexpr uint32_t kPaletteEmptySlot = 0u;
constexpr size_t kPaletteIconsPerRow = 19;
constexpr size_t kPaletteMaxRows = 4;
constexpr size_t kPaletteMaxSlots = kPaletteIconsPerRow * kPaletteMaxRows;
constexpr float kEyedropperIconSizePx = 20.0f; // square aspect ratio

// Default palette: preset colors at startup (black, white, gray + spectrum) - 19 colors for one row
std::vector<uint32_t> DefaultPalette()
{
    return {
        0xFF000000, 0xFFFFFFFF, 0xFF808080, // Black, White, 50% Gray
        0xFFF44336, 0xFFE91E63, 0xFF9C27B0, 0xFF673AB7, 0xFF3F51B5, 0xFF2196F3,
        0xFF03A9F4, 0xFF00BCD4, 0xFF009688, 0xFF4CAF50, 0xFF8BC34A, 0xFFCDDC39,
        0xFFFFEB3B, 0xFFFFC107, 0xFFFF9800, 0xFFFF5722,
    };
}

static void ApplyPaletteSwatchStyle(UIElement* swatch, uint32_t argb)
{
    if (!swatch)
        return;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(20.0f))
        .Set(Style::Height, StyleLength::Px(20.0f))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
        .Set(Style::Cursor, CursorStyle::Pointer);
    if (argb == kPaletteEmptySlot)
    {
        swatch->Overrides()
            .Set(Style::BackgroundColor, (uint32_t)0x00000000)
            .Set(Style::BorderWidth, Box4{0.0f, 0.0f, 0.0f, 0.0f})
            .Set(Style::BorderColor, BorderColorsTRBL{0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u});
    }
    else
    {
        swatch->Overrides()
            .Set(Style::BackgroundColor, (uint32_t)argb)
            .Set(Style::BorderWidth, Box4{0.0f, 0.0f, 0.0f, 0.0f})
            .Set(Style::BorderColor, BorderColorsTRBL{0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u});
    }
}
} // namespace

// -----------------------------------------------------------------------------
// ARGB <-> ColorPickerValue (public API for persistence / UI)
// -----------------------------------------------------------------------------

ColorPickerValue ColorPickerValueFromArgb(uint32_t argb)
{
    ColorPickerValue out;
    out.hsv = ColorHSV::FromARGB(argb);
    out.intensity = 1.0f;
    return out;
}

uint32_t ColorPickerValueToArgb(const ColorPickerValue& val)
{
    return val.hsv.ToARGB();
}

// -----------------------------------------------------------------------------
// ColorWheelElement
// -----------------------------------------------------------------------------

ColorWheelElement::ColorWheelElement()
{
    AddClass("colorpicker-wheel");
    
    // Create marker element
    auto marker = std::make_unique<UIElement>();
    marker->SetId("colorpicker-wheel-marker");
    marker->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::Width, StyleLength::Px(12.0f))
        .Set(Style::Height, StyleLength::Px(12.0f))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{6.0f, 6.0f, 6.0f, 6.0f})
        .Set(Style::BackgroundColor, (uint32_t)0xFFFFFFFF)
        .Set(Style::BorderWidth, Box4{2.0f, 2.0f, 2.0f, 2.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u})
        .Set(Style::PointerEvents, false);
    m_Marker = marker.get();
    AddChild(std::move(marker));
}

void ColorWheelElement::SetHue(float h)
{
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360.0f;
    if (m_Hue != h)
    {
        m_Hue = h;
        UpdateMarkerPosition();
        MarkDirty(VisualDirty);
    }
}

void ColorWheelElement::SetSaturation(float s)
{
    s = std::clamp(s, 0.0f, 1.0f);
    if (m_Saturation != s)
    {
        m_Saturation = s;
        UpdateMarkerPosition();
        MarkDirty(VisualDirty);
    }
}

void ColorWheelElement::UpdateMarkerPosition()
{
    if (!m_Marker)
        return;
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    if (W <= 0.0f || H <= 0.0f)
        return;
    constexpr float kMarkerSize = 12.0f;
    constexpr float kHalfMarker = kMarkerSize * 0.5f;
    const float cx = W * 0.5f;
    const float cy = H * 0.5f;
    const float radius = std::min(W, H) * 0.5f * 0.92f;
    const float angle = m_Hue * (kPi / 180.0f);
    const float r = m_Saturation * radius;
    const float mx = cx + r * std::cos(angle) - kHalfMarker;
    const float my = cy + r * std::sin(angle) - kHalfMarker;
    UI::Layout::SetAbsolutePosition(*m_Marker,
                                    Mathematics::Rect{mx, my, kMarkerSize, kMarkerSize},
                                    /*positionOnlyFastPath=*/false);
}

void ColorWheelElement::OnPostLayout()
{
    UpdateMarkerPosition();
}

void ColorWheelElement::OnEvent(UIEvent& e)
{
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    const float cx = GetLayoutX() + W * 0.5f;
    const float cy = GetLayoutY() + H * 0.5f;
    const float radius = std::min(W, H) * 0.5f * 0.92f;
    if (radius <= 0.0f)
        return;

    const float dx = e.X - cx;
    const float dy = e.Y - cy;
    const float dist = std::sqrt(dx * dx + dy * dy);

    auto updateFromPos = [this, radius, dx, dy, dist]()
    {
        float angle = std::atan2(dy, dx) * (180.0f / kPi);
        if (angle < 0) angle += 360.0f;
        float h = angle;
        float s = std::min(1.0f, dist / radius);
        SetHue(h);
        SetSaturation(s);
        if (m_OnChange)
            m_OnChange(m_Hue, m_Saturation);
    };

    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        m_Dragging = true;
        e.Capture(this);
        updateFromPos();
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseUp && e.Button == 0)
    {
        if (m_Dragging)
        {
            m_Dragging = false;
            if (m_OnChangeEnd)
                m_OnChangeEnd();
            e.Stop();
        }
        return;
    }
    if (e.Id == kEventMouseMove && m_Dragging)
    {
        updateFromPos();
        e.Stop();
    }
}

void ColorWheelElement::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                                             float x, float y, float w, float h)
{
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker — escalate
    // and let the drain re-emit this element on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    if (w <= 0.0f || h <= 0.0f)
        return;

    constexpr float kWheelInset = 0.92f;
    const float radius = std::min(w, h) * 0.5f * kWheelInset;
    const float diameter = radius * 2.0f;
    if (radius <= 0.0f)
        return;

    const float wheelX = x + (w - diameter) * 0.5f;
    const float wheelY = y + (h - diameter) * 0.5f;

    auto p = UI::MakeRect(wheelX, wheelY, diameter, diameter,
                          UI::PackColor(1.0f, 1.0f, 1.0f, 1.0f),
                          radius, radius, radius, radius);
    const UI::GradientMode wheelGradient = m_GradingStyle
                                               ? UI::GradientMode::PolarHSVGrading
                                               : UI::GradientMode::PolarHSV;
    UI::AddGradient(p, wheelGradient, 0, 0);
    ctx.Emit(p);
}

// -----------------------------------------------------------------------------
// ColorSVBoxElement - Saturation-Value box
// -----------------------------------------------------------------------------

ColorSVBoxElement::ColorSVBoxElement()
{
    AddClass("colorpicker-svbox");
    
    // Create marker element
    auto marker = std::make_unique<UIElement>();
    marker->SetId("colorpicker-svbox-marker");
    marker->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::Width, StyleLength::Px(12.0f))
        .Set(Style::Height, StyleLength::Px(12.0f))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{6.0f, 6.0f, 6.0f, 6.0f})
        .Set(Style::BackgroundColor, (uint32_t)0xFFFFFFFF)
        .Set(Style::BorderWidth, Box4{2.0f, 2.0f, 2.0f, 2.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u})
        .Set(Style::PointerEvents, false);
    m_Marker = marker.get();
    AddChild(std::move(marker));
}

void ColorSVBoxElement::SetHue(float h)
{
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360.0f;
    if (m_Hue != h)
    {
        m_Hue = h;
        MarkDirty(VisualDirty);
    }
}

void ColorSVBoxElement::SetSaturation(float s)
{
    s = std::clamp(s, 0.0f, 1.0f);
    if (m_Saturation != s)
    {
        m_Saturation = s;
        UpdateMarkerPosition();
        MarkDirty(VisualDirty);
    }
}

void ColorSVBoxElement::SetValue(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    if (m_Value != v)
    {
        m_Value = v;
        UpdateMarkerPosition();
        MarkDirty(VisualDirty);
    }
}

void ColorSVBoxElement::UpdateMarkerPosition()
{
    if (!m_Marker)
        return;
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    if (W <= 0.0f || H <= 0.0f)
        return;
    constexpr float kMarkerSize = 12.0f;
    constexpr float kHalfMarker = kMarkerSize * 0.5f;
    const float mx = m_Saturation * W - kHalfMarker;
    const float my = (1.0f - m_Value) * H - kHalfMarker;
    UI::Layout::SetAbsolutePosition(*m_Marker,
                                    Mathematics::Rect{mx, my, kMarkerSize, kMarkerSize},
                                    /*positionOnlyFastPath=*/false);
}

void ColorSVBoxElement::OnPostLayout()
{
    UpdateMarkerPosition();
}

void ColorSVBoxElement::OnEvent(UIEvent& e)
{
    const float W = GetLayoutWidth();
    const float H = GetLayoutHeight();
    const float lx = GetLayoutX();
    const float ly = GetLayoutY();
    if (W <= 0.0f || H <= 0.0f)
        return;

    auto updateFromPos = [this, W, H, lx, ly](float mx, float my)
    {
        float s = std::clamp((mx - lx) / W, 0.0f, 1.0f);
        float v = std::clamp(1.0f - (my - ly) / H, 0.0f, 1.0f); // Top = 1, Bottom = 0
        SetSaturation(s);
        SetValue(v);
        if (m_OnChange)
            m_OnChange(m_Saturation, m_Value);
    };

    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        m_Dragging = true;
        e.Capture(this);
        updateFromPos(e.X, e.Y);
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseUp && e.Button == 0)
    {
        if (m_Dragging)
        {
            m_Dragging = false;
            e.Stop();
        }
        return;
    }
    if (e.Id == kEventMouseMove && m_Dragging)
    {
        updateFromPos(e.X, e.Y);
        e.Stop();
    }
}

void ColorSVBoxElement::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                                             float x, float y, float w, float h)
{
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker — escalate
    // and let the drain re-emit this element on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    if (w <= 0.0f || h <= 0.0f)
        return;

    float hr, hg, hb;
    ColorUtils::HsvToRgb(m_Hue, 1.0f, 1.0f, hr, hg, hb);

    const uint32_t white    = UI::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
    const uint32_t hueColor = UI::PackColor(hr, hg, hb, 1.0f);
    const uint32_t black    = UI::PackColor(0.0f, 0.0f, 0.0f, 1.0f);

    auto p = UI::MakeRect(x, y, w, h, white);
    UI::AddFourCornerGradient(p, white, hueColor, black, black);
    ctx.Emit(p);
}

// -----------------------------------------------------------------------------
// HueBarElement - Vertical hue spectrum strip
// -----------------------------------------------------------------------------

HueBarElement::HueBarElement()
{
    AddClass("colorpicker-huebar");
    
    // Create marker element (horizontal bar indicator)
    auto marker = std::make_unique<UIElement>();
    marker->SetId("colorpicker-huebar-marker");
    marker->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::BackgroundColor, (uint32_t)0xFFFFFFFF)
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u})
        .Set(Style::PointerEvents, false);
    m_Marker = marker.get();
    AddChild(std::move(marker));
}

void HueBarElement::SetHue(float h)
{
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360.0f;
    if (m_Hue != h)
    {
        m_Hue = h;
        UpdateMarkerPosition();
        MarkDirty(VisualDirty);
    }
}

void HueBarElement::UpdateMarkerPosition()
{
    if (!m_Marker)
        return;
    const float H = GetLayoutHeight();
    if (H <= 0.0f)
        return;
    constexpr float kMarkerW = 28.0f;
    constexpr float kMarkerH = 4.0f;
    const float my = (m_Hue / 360.0f) * H - kMarkerH * 0.5f;
    UI::Layout::SetAbsolutePosition(*m_Marker,
                                    Mathematics::Rect{-2.0f, my, kMarkerW, kMarkerH},
                                    /*positionOnlyFastPath=*/false);
}

void HueBarElement::OnPostLayout()
{
    UpdateMarkerPosition();
}

void HueBarElement::OnEvent(UIEvent& e)
{
    const float H = GetLayoutHeight();
    const float ly = GetLayoutY();
    if (H <= 0.0f)
        return;

    auto updateFromPos = [this, H, ly](float my)
    {
        float t = std::clamp((my - ly) / H, 0.0f, 1.0f);
        float h = t * 360.0f; // Top = 0, Bottom = 360
        SetHue(h);
        if (m_OnChange)
            m_OnChange(m_Hue);
    };

    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        m_Dragging = true;
        e.Capture(this);
        updateFromPos(e.Y);
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseUp && e.Button == 0)
    {
        if (m_Dragging)
        {
            m_Dragging = false;
            e.Stop();
        }
        return;
    }
    if (e.Id == kEventMouseMove && m_Dragging)
    {
        updateFromPos(e.Y);
        e.Stop();
    }
}

void HueBarElement::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                                         float x, float y, float w, float h)
{
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker — escalate
    // and let the drain re-emit this element on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    if (w <= 0.0f || h <= 0.0f)
        return;

    auto p = UI::MakeRect(x, y, w, h, UI::PackColor(1.0f, 1.0f, 1.0f, 1.0f));
    UI::AddGradient(p, UI::GradientMode::HueVertical, 0, 0);
    ctx.Emit(p);
}

// -----------------------------------------------------------------------------
// ValueBarElement - Vertical brightness bar for wheel mode
// -----------------------------------------------------------------------------

ValueBarElement::ValueBarElement()
{
    AddClass("colorpicker-valuebar");

    auto marker = std::make_unique<UIElement>();
    marker->SetId("colorpicker-valuebar-marker");
    marker->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::BackgroundColor, (uint32_t)0xFFFFFFFF)
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u})
        .Set(Style::PointerEvents, false);
    m_Marker = marker.get();
    AddChild(std::move(marker));
}

void ValueBarElement::SetHue(float h)
{
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360.0f;
    if (m_Hue != h)
    {
        m_Hue = h;
        MarkDirty(VisualDirty);
    }
}

void ValueBarElement::SetSaturation(float s)
{
    s = std::clamp(s, 0.0f, 1.0f);
    if (m_Saturation != s)
    {
        m_Saturation = s;
        MarkDirty(VisualDirty);
    }
}

void ValueBarElement::SetValue(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    if (m_Value != v)
    {
        m_Value = v;
        UpdateMarkerPosition();
        MarkDirty(VisualDirty);
    }
}

void ValueBarElement::UpdateMarkerPosition()
{
    if (!m_Marker)
        return;
    const float H = GetLayoutHeight();
    if (H <= 0.0f)
        return;
    constexpr float kMarkerW = 28.0f;
    constexpr float kMarkerH = 4.0f;
    const float my = (1.0f - m_Value) * H - kMarkerH * 0.5f;
    UI::Layout::SetAbsolutePosition(*m_Marker,
                                    Mathematics::Rect{-2.0f, my, kMarkerW, kMarkerH},
                                    /*positionOnlyFastPath=*/false);
}

void ValueBarElement::OnPostLayout()
{
    UpdateMarkerPosition();
}

void ValueBarElement::OnEvent(UIEvent& e)
{
    const float H = GetLayoutHeight();
    const float ly = GetLayoutY();
    if (H <= 0.0f)
        return;

    auto updateFromPos = [this, H, ly](float my)
    {
        float t = std::clamp((my - ly) / H, 0.0f, 1.0f);
        float v = 1.0f - t;
        SetValue(v);
        if (m_OnChange)
            m_OnChange(m_Value);
    };

    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        m_Dragging = true;
        e.Capture(this);
        updateFromPos(e.Y);
        e.Stop();
        return;
    }
    if (e.Id == kEventMouseUp && e.Button == 0)
    {
        if (m_Dragging)
        {
            m_Dragging = false;
            e.Stop();
        }
        return;
    }
    if (e.Id == kEventMouseMove && m_Dragging)
    {
        updateFromPos(e.Y);
        e.Stop();
    }
}

void ValueBarElement::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle&,
                                           float x, float y, float w, float h)
{
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker — escalate
    // and let the drain re-emit this element on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    if (w <= 0.0f || h <= 0.0f)
        return;

    float hr, hg, hb;
    ColorUtils::HsvToRgb(m_Hue, m_Saturation, 1.0f, hr, hg, hb);

    const uint32_t hueColor = UI::PackColor(hr, hg, hb, 1.0f);
    const uint32_t black    = UI::PackColor(0.0f, 0.0f, 0.0f, 1.0f);

    auto p = UI::MakeRect(x, y, w, h, hueColor);
    UI::AddGradient(p, UI::GradientMode::Vertical, hueColor, black);
    ctx.Emit(p);
}

// -----------------------------------------------------------------------------
// ColorPicker
// -----------------------------------------------------------------------------

void ColorPicker::HsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
    ColorUtils::HsvToRgb(h, s, v, r, g, b);
}

void ColorPicker::RgbToHsv(float r, float g, float b, float& h, float& s, float& v)
{
    ColorUtils::RgbToHsv(r, g, b, h, s, v);
}

ColorPicker::ColorPicker()
{
    AddClass("colorpicker-view");
    m_PaletteColors = DefaultPalette();
    // Load saved custom palette from editor preferences (always keep at least one row)
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    if (prefs.Load(&err) && prefs.Contains("colorpicker.palette"))
    {
        const auto& j = prefs.Json();
        const auto it = j.find("colorpicker.palette");
        if (it != j.end() && it->is_array())
        {
            std::vector<uint32_t> loaded;
            loaded.reserve(it->size());
            for (const auto& v : *it)
            {
                if (v.is_number_unsigned())
                    loaded.push_back(static_cast<uint32_t>(v.get<uint64_t>()));
                else if (v.is_number_integer())
                    loaded.push_back(static_cast<uint32_t>(v.get<int64_t>()));
                else if (v.is_number_float())
                    loaded.push_back(static_cast<uint32_t>(v.get<double>()));
            }
            // Ensure at least one row, but no more than max rows
            while (loaded.size() < kPaletteIconsPerRow)
                loaded.push_back(kPaletteEmptySlot);
            if (loaded.size() > kPaletteMaxSlots)
                loaded.resize(kPaletteMaxSlots);
            if (!loaded.empty())
                m_PaletteColors = std::move(loaded);
        }
    }
    // Defensive: if the saved palette is all empty slots, restore defaults so it remains visible.
    {
        bool anyColor = false;
        for (uint32_t c : m_PaletteColors)
        {
            if (c != kPaletteEmptySlot)
            {
                anyColor = true;
                break;
            }
        }
        if (!anyColor)
            m_PaletteColors = DefaultPalette();
    }
    // Always ensure at least one row so the palette is visible
    if (m_PaletteColors.size() < kPaletteIconsPerRow)
        m_PaletteColors.resize(kPaletteIconsPerRow, kPaletteEmptySlot);
    
    // Restore persisted picker mode when available.
    {
        auto modePrefs = Editor::OpenEditorPreferences();
        std::string modeErr;
        if (modePrefs.Load(&modeErr))
        {
            int64_t modeRaw = static_cast<int64_t>(m_Mode);
            if (modePrefs.TryGetInt64("colorpicker.mode", modeRaw))
            {
                if (modeRaw == static_cast<int64_t>(ColorPickerMode::WheelTriangle))
                    m_Mode = ColorPickerMode::WheelTriangle;
                else if (modeRaw == static_cast<int64_t>(ColorPickerMode::BoxHueBar))
                    m_Mode = ColorPickerMode::BoxHueBar;
            }
        }
    }
    
    BuildUI();
}

void ColorPicker::SetValue(const ColorPickerValue& value)
{
    m_Value = value;
    m_Value.hsv.h = std::fmod(m_Value.hsv.h, 360.0f);
    if (m_Value.hsv.h < 0)
        m_Value.hsv.h += 360.0f;
    m_Value.hsv.s = std::clamp(m_Value.hsv.s, 0.0f, 1.0f);
    m_Value.hsv.v = std::clamp(m_Value.hsv.v, 0.0f, 1.0f);
    m_Value.hsv.a = std::clamp(m_Value.hsv.a, 0.0f, 1.0f);
    if (m_Value.intensity < 1.0f)
        m_Value.intensity = 1.0f;
    SyncFromValue();
}

void ColorPicker::SetColorFromArgb(uint32_t argb)
{
    ColorPickerValue val = ColorPickerValueFromArgb(argb);
    val.intensity = m_Value.intensity; // preserve HDR intensity
    SetValue(val);
    NotifyChange();
}

void ColorPicker::SetMode(ColorPickerMode mode)
{
    if (m_Mode == mode)
        return;
    m_Mode = mode;
    
    // Save mode to preferences
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.SetInt64("colorpicker.mode", static_cast<int64_t>(m_Mode));
        prefs.Save(&err);
    }
    
    RebuildColorArea();
    SyncFromValue();
}

void ColorPicker::RebuildColorArea()
{
    if (!m_ColorAreaContainer)
        return;
    
    // Collect children to remove first (to avoid modifying while iterating)
    std::vector<UIElement*> toRemove;
    for (const auto& child : m_ColorAreaContainer->GetChildren())
        toRemove.push_back(child.get());
    for (UIElement* child : toRemove)
        m_ColorAreaContainer->RemoveChild(child);
    
    m_Wheel = nullptr;
    m_ValueBar = nullptr;
    m_SvBox = nullptr;
    m_HueBar = nullptr;
    
    if (m_Mode == ColorPickerMode::WheelTriangle)
    {
        // Wheel mode: wheel + value slider
        auto wheel = std::make_unique<ColorWheelElement>();
        wheel->SetId("colorpicker-wheel");
        wheel->Overrides()
            .Set(Style::Width, StyleLength::Px(200.0f))
            .Set(Style::Height, StyleLength::Px(200.0f))
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
            .Set(Style::OverflowProp, Overflow::Hidden);
        wheel->SetHue(m_Value.hsv.h);
        wheel->SetSaturation(m_Value.hsv.s);
        wheel->SetOnChange([this](float h, float s) { OnWheelChange(h, s); });
        m_Wheel = wheel.get();
        m_ColorAreaContainer->AddChild(std::move(wheel));
        
        // Value bar for wheel mode (brightness gradient: hue color -> black)
        auto valueBar = std::make_unique<ValueBarElement>();
        valueBar->SetId("colorpicker-valuebar");
        valueBar->Overrides()
            .Set(Style::Width, StyleLength::Px(24.0f))
            .Set(Style::Height, StyleLength::Px(200.0f))
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::Cursor, CursorStyle::Pointer)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
            .Set(Style::OverflowProp, Overflow::Hidden);
        valueBar->SetHue(m_Value.hsv.h);
        valueBar->SetValue(m_Value.hsv.v);
        valueBar->SetOnChange([this](float v) { OnValueSliderChange(v); });
        m_ValueBar = valueBar.get();
        m_ColorAreaContainer->AddChild(std::move(valueBar));
    }
    else // BoxHueBar mode
    {
        // SV Box
        auto svBox = std::make_unique<ColorSVBoxElement>();
        svBox->SetId("colorpicker-svbox");
        svBox->Overrides()
            .Set(Style::Width, StyleLength::Px(200.0f))
            .Set(Style::Height, StyleLength::Px(200.0f))
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::Cursor, CursorStyle::Crosshair)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
            .Set(Style::OverflowProp, Overflow::Hidden);
        svBox->SetHue(m_Value.hsv.h);
        svBox->SetSaturation(m_Value.hsv.s);
        svBox->SetValue(m_Value.hsv.v);
        svBox->SetOnChange([this](float s, float v) { OnSVBoxChange(s, v); });
        m_SvBox = svBox.get();
        m_ColorAreaContainer->AddChild(std::move(svBox));
        
        // Hue bar
        auto hueBar = std::make_unique<HueBarElement>();
        hueBar->SetId("colorpicker-huebar");
        hueBar->Overrides()
            .Set(Style::Width, StyleLength::Px(24.0f))
            .Set(Style::Height, StyleLength::Px(200.0f))
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::Cursor, CursorStyle::Pointer)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
            .Set(Style::OverflowProp, Overflow::Hidden);
        hueBar->SetHue(m_Value.hsv.h);
        hueBar->SetOnChange([this](float h) { OnHueBarChange(h); });
        m_HueBar = hueBar.get();
        m_ColorAreaContainer->AddChild(std::move(hueBar));
    }
}

void ColorPicker::BuildUI()
{
    m_PaletteRow = nullptr;
    Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::PaddingTop, StyleLength::Px(12.0f))
        .Set(Style::PaddingRight, StyleLength::Px(12.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(12.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(12.0f))
        .Set(Style::Gap, StyleLength::Px(10.0f))
        .Set(Style::BackgroundColor, (uint32_t)0xFF252526)
        .Set(Style::BorderWidth, Box4{0.0f, 0.0f, 0.0f, 0.0f})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{0.0f, 0.0f, 0.0f, 0.0f})
        .Set(Style::MinWidth, StyleLength::Px(320.0f));

    // Mode toggle button row
    auto modeRow = std::make_unique<UIElement>();
    modeRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::JustifyContent, JustifyContent::FlexEnd)
        .Set(Style::MarginBottom, StyleLength::Px(4.0f));
    auto modeBtn = std::make_unique<Button>();
    modeBtn->SetId("colorpicker-mode-toggle");
    modeBtn->SetText(m_Mode == ColorPickerMode::WheelTriangle ? "Wheel" : "Box");
    modeBtn->Overrides()
        .Set(Style::Width, StyleLength::Px(70.0f))
        .Set(Style::PaddingTop, StyleLength::Px(4.0f))
        .Set(Style::PaddingRight, StyleLength::Px(12.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(4.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(12.0f))
        .Set(Style::BackgroundColor, (uint32_t)0xFF3C3C3C)
        .Set(Style::Color, (uint32_t)0xFFFFFFFF)
        .Set(Style::BorderWidth, Box4{0.0f, 0.0f, 0.0f, 0.0f})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
        .Set(Style::FontSize, StyleLength::Px(12.0f));
    modeBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent& e) {
        Button& btn = static_cast<Button&>(*e.CurrentTarget);
        SetMode(m_Mode == ColorPickerMode::WheelTriangle ? ColorPickerMode::BoxHueBar : ColorPickerMode::WheelTriangle);
        btn.SetText(m_Mode == ColorPickerMode::WheelTriangle ? "Wheel" : "Box");
    });
    m_ModeToggleBtn = modeBtn.get();
    modeRow->AddChild(std::move(modeBtn));
    AddChild(std::move(modeRow));

    // Row: color area (left) + controls (right)
    auto row = std::make_unique<UIElement>();
    row->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(16.0f))
        .Set(Style::AlignItems, AlignItems::FlexStart);

    // Color area container (will hold wheel or box+hue bar based on mode)
    auto colorArea = std::make_unique<UIElement>();
    colorArea->SetId("colorpicker-color-area");
    colorArea->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(8.0f))
        .Set(Style::AlignItems, AlignItems::Stretch);
    m_ColorAreaContainer = colorArea.get();
    row->AddChild(std::move(colorArea));
    
    // Build the color area based on current mode
    RebuildColorArea();

    auto rightCol = std::make_unique<UIElement>();
    rightCol->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::Gap, StyleLength::Px(6.0f))
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::FlexShrink, 1.0f)
        .Set(Style::FlexBasis, StyleLength::Auto())
        .Set(Style::MinWidth, StyleLength::Px(140.0f));

    // Alpha slider row
    auto alphaLabelRow = std::make_unique<UIElement>();
    alphaLabelRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));
    auto alphaLabel = std::make_unique<Label>();
    alphaLabel->SetText("Alpha");
    alphaLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFFFFFFFF)
        .Set(Style::FontSize, StyleLength::Px(13.0f));
    alphaLabelRow->AddChild(std::move(alphaLabel));
    auto alphaValueLabel = std::make_unique<Label>();
    alphaValueLabel->SetId("colorpicker-alpha-value");
    alphaValueLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFFFFFFFF)
        .Set(Style::FontSize, StyleLength::Px(13.0f));
    m_AlphaValueLabel = alphaValueLabel.get();
    alphaLabelRow->AddChild(std::move(alphaValueLabel));
    rightCol->AddChild(std::move(alphaLabelRow));
    
    auto alphaSlider = std::make_unique<Slider>();
    alphaSlider->SetMin(0.0f);
    alphaSlider->SetMax(1.0f);
    alphaSlider->SetValue(m_Value.hsv.a);
    alphaSlider->SetOnValueChanging([this](const float& a) { OnAlphaSliderChange(a); });
    alphaSlider->SetOnValueChanged([this](const float& a) { OnAlphaSliderChange(a); });
    alphaSlider->Overrides()
        .Set(Style::Height, StyleLength::Px(18.0f));
    m_AlphaSlider = alphaSlider.get();
    rightCol->AddChild(std::move(alphaSlider));

    // HDR intensity slider
    auto hdrLabelRow = std::make_unique<UIElement>();
    hdrLabelRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));
    auto intensityLabel = std::make_unique<Label>();
    intensityLabel->SetText("HDR");
    intensityLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFFFFFFFF)
        .Set(Style::FontSize, StyleLength::Px(13.0f));
    hdrLabelRow->AddChild(std::move(intensityLabel));
    auto intensityValueLabel = std::make_unique<Label>();
    intensityValueLabel->SetId("colorpicker-hdr-value");
    intensityValueLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFFFFFFFF)
        .Set(Style::FontSize, StyleLength::Px(13.0f));
    m_IntensityValueLabel = intensityValueLabel.get();
    hdrLabelRow->AddChild(std::move(intensityValueLabel));
    rightCol->AddChild(std::move(hdrLabelRow));
    
    auto intensitySlider = std::make_unique<Slider>();
    intensitySlider->SetMin(1.0f);
    intensitySlider->SetMax(8.0f);
    intensitySlider->SetValue(m_Value.intensity);
    intensitySlider->SetOnValueChanging([this](const float& i) { OnIntensitySliderChange(i); });
    intensitySlider->SetOnValueChanged([this](const float& i) { OnIntensitySliderChange(i); });
    intensitySlider->Overrides()
        .Set(Style::Height, StyleLength::Px(18.0f));
    m_IntensitySlider = intensitySlider.get();
    rightCol->AddChild(std::move(intensitySlider));

    // Swatch row: color swatch + eyedropper button
    auto swatchRow = std::make_unique<UIElement>();
    swatchRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(6.0f))
        .Set(Style::AlignItems, AlignItems::Center);

    // Color swatch showing the current color (on the left)
    auto swatch = std::make_unique<UIElement>();
    swatch->SetId("colorpicker-swatch");
    swatch->AddClass("colorpicker-swatch");
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(180.0f))
        .Set(Style::Height, StyleLength::Px(28.0f))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
        .Set(Style::Cursor, CursorStyle::Pointer)
        .Set(Style::BackgroundColor, (uint32_t)0xFFFF0000);
    swatch->RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev) {
        if (ev.Button == 0)
        {
            m_DraggingFromSwatch = true;
            AddColorToPalette(ColorPickerValueToArgb(m_Value));
        }
    });
    m_SwatchEl = swatch.get();
    swatchRow->AddChild(std::move(swatch));

    // Eyedropper button (on the right)
    auto eyedropperBtn = std::make_unique<Button>();
    eyedropperBtn->SetId("colorpicker-eyedropper");
    eyedropperBtn->SetText("");
    eyedropperBtn->Overrides()
        .Set(Style::Width, StyleLength::Px(28.0f))
        .Set(Style::Height, StyleLength::Px(28.0f))
        .Set(Style::MinWidth, StyleLength::Px(28.0f))
        .Set(Style::MinHeight, StyleLength::Px(28.0f))
        .Set(Style::FlexGrow, 0.0f)
        .Set(Style::FlexShrink, 0.0f)
        .Set(Style::PaddingTop, StyleLength::Px(0.0f))
        .Set(Style::PaddingRight, StyleLength::Px(0.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(0.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(0.0f))
        .Set(Style::BackgroundColor, (uint32_t)0xFF3C3C3C)
        .Set(Style::BorderWidth, Box4{0.0f, 0.0f, 0.0f, 0.0f})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
        .Set(Style::BackgroundImage, BackgroundImageSource{BackgroundImageSource::SourceKind::Path, GUID::Null(), "Icons/colorpicker.png"})
        .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Explicit, kEyedropperIconSizePx, false, kEyedropperIconSizePx, false})
        .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
        .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true});
    eyedropperBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_EyedropperHandler)
            m_EyedropperHandler();
    });
    swatchRow->AddChild(std::move(eyedropperBtn));
    rightCol->AddChild(std::move(swatchRow));

    // Hex field
    auto hexField = std::make_unique<TextField>();
    hexField->SetId("colorpicker-hex");
    hexField->Overrides()
        .Set(Style::PaddingTop, StyleLength::Px(4.0f))
        .Set(Style::PaddingRight, StyleLength::Px(6.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(4.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(6.0f))
        .Set(Style::FontSize, StyleLength::Px(13.0f))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
        .Set(Style::Color, (uint32_t)0xFFFFFFFF)
        .Set(Style::BackgroundColor, (uint32_t)0xFF3C3C3C)
        .Set(Style::BorderWidth, Box4{0.0f, 0.0f, 0.0f, 0.0f});
    hexField->SetValue("#000000");
    hexField->SetOnValueChanged([this](const std::string&) { OnHexFieldChange(); });
    m_HexField = hexField.get();
    rightCol->AddChild(std::move(hexField));

    // HSV fields row
    auto hsvRow = std::make_unique<UIElement>();
    hsvRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(4.0f))
        .Set(Style::AlignItems, AlignItems::Center);
    auto hsvLabel = std::make_unique<Label>();
    hsvLabel->SetText("H");
    hsvLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFF888888)
        .Set(Style::FontSize, StyleLength::Px(12.0f))
        .Set(Style::Width, StyleLength::Px(12.0f));
    hsvRow->AddChild(std::move(hsvLabel));
    auto makeField = [](const char* id) -> std::unique_ptr<TextField>
    {
        auto f = std::make_unique<TextField>();
        f->SetId(id);
        f->Overrides()
            .Set(Style::Width, StyleLength::Px(36.0f))
            .Set(Style::PaddingTop, StyleLength::Px(3.0f))
            .Set(Style::PaddingRight, StyleLength::Px(4.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(3.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(4.0f))
            .Set(Style::FontSize, StyleLength::Px(12.0f))
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f})
            .Set(Style::Color, (uint32_t)0xFFFFFFFF)
            .Set(Style::BackgroundColor, (uint32_t)0xFF3C3C3C)
            .Set(Style::BorderWidth, Box4{0.0f, 0.0f, 0.0f, 0.0f});
        return f;
    };
    auto hField = makeField("cp-h");
    m_HField = hField.get();
    hField->SetOnValueChanged([this](const std::string&) { OnHsvFieldChange(); });
    hsvRow->AddChild(std::move(hField));
    auto sLabel = std::make_unique<Label>();
    sLabel->SetText("S");
    sLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFF888888)
        .Set(Style::FontSize, StyleLength::Px(12.0f))
        .Set(Style::Width, StyleLength::Px(12.0f));
    hsvRow->AddChild(std::move(sLabel));
    auto sField = makeField("cp-s");
    m_SField = sField.get();
    sField->SetOnValueChanged([this](const std::string&) { OnHsvFieldChange(); });
    hsvRow->AddChild(std::move(sField));
    auto vLabel = std::make_unique<Label>();
    vLabel->SetText("V");
    vLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFF888888)
        .Set(Style::FontSize, StyleLength::Px(12.0f))
        .Set(Style::Width, StyleLength::Px(12.0f));
    hsvRow->AddChild(std::move(vLabel));
    auto vField = makeField("cp-v");
    m_VField = vField.get();
    vField->SetOnValueChanged([this](const std::string&) { OnHsvFieldChange(); });
    hsvRow->AddChild(std::move(vField));
    rightCol->AddChild(std::move(hsvRow));

    // RGBA fields row
    auto rgbaRow = std::make_unique<UIElement>();
    rgbaRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::Gap, StyleLength::Px(4.0f))
        .Set(Style::AlignItems, AlignItems::Center);
    auto rLabel = std::make_unique<Label>();
    rLabel->SetText("R");
    rLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFF888888)
        .Set(Style::FontSize, StyleLength::Px(12.0f))
        .Set(Style::Width, StyleLength::Px(12.0f));
    rgbaRow->AddChild(std::move(rLabel));
    auto rField = makeField("cp-r");
    m_RField = rField.get();
    rField->SetOnValueChanged([this](const std::string&) { OnRgbFieldChange(); });
    rgbaRow->AddChild(std::move(rField));
    auto gLabel = std::make_unique<Label>();
    gLabel->SetText("G");
    gLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFF888888)
        .Set(Style::FontSize, StyleLength::Px(12.0f))
        .Set(Style::Width, StyleLength::Px(12.0f));
    rgbaRow->AddChild(std::move(gLabel));
    auto gField = makeField("cp-g");
    m_GField = gField.get();
    gField->SetOnValueChanged([this](const std::string&) { OnRgbFieldChange(); });
    rgbaRow->AddChild(std::move(gField));
    auto bLabel = std::make_unique<Label>();
    bLabel->SetText("B");
    bLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFF888888)
        .Set(Style::FontSize, StyleLength::Px(12.0f))
        .Set(Style::Width, StyleLength::Px(12.0f));
    rgbaRow->AddChild(std::move(bLabel));
    auto bField = makeField("cp-b");
    m_BField = bField.get();
    bField->SetOnValueChanged([this](const std::string&) { OnRgbFieldChange(); });
    rgbaRow->AddChild(std::move(bField));
    auto aLabel = std::make_unique<Label>();
    aLabel->SetText("A");
    aLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFF888888)
        .Set(Style::FontSize, StyleLength::Px(12.0f))
        .Set(Style::Width, StyleLength::Px(12.0f));
    rgbaRow->AddChild(std::move(aLabel));
    auto aField = makeField("cp-a");
    m_AField = aField.get();
    aField->SetOnValueChanged([this](const std::string&) { OnAlphaFieldChange(); });
    rgbaRow->AddChild(std::move(aField));
    rightCol->AddChild(std::move(rgbaRow));

    row->AddChild(std::move(rightCol));
    AddChild(std::move(row));

    // Palette / color library (always at least one row of 18 slots)
    auto paletteLabel = std::make_unique<Label>();
    paletteLabel->SetText("Palette");
    paletteLabel->SetId("colorpicker-palette-label");
    paletteLabel->Overrides()
        .Set(Style::Color, (uint32_t)0xFFFFFFFF)
        .Set(Style::FontSize, StyleLength::Px(14.0f))
        .Set(Style::MarginTop, StyleLength::Px(9.0f))
        .Set(Style::Cursor, CursorStyle::Pointer);
    paletteLabel->RegisterEventHandler(kEventMouseDown, [this](UIEvent& ev) {
        using clock = std::chrono::steady_clock;
        static clock::time_point lastClick;
        auto now = clock::now();
        bool isDoubleClick = (now - lastClick) < GameEngine::Platform::GetDoubleClickInterval();
        lastClick = now;
        if (isDoubleClick)
        {
            if (m_CanUndoPaletteReset)
                UndoPaletteReset();
            else
                ResetPalette();
        }
        ev.Stop();
    });
    AddChild(std::move(paletteLabel));
    auto paletteRow = std::make_unique<UIElement>();
    paletteRow->SetId("colorpicker-palette");
    // 19 columns per row: width = 19*20 + 18*4 = 452px
    paletteRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::FlexWrap, true)
        .Set(Style::Gap, StyleLength::Px(4.0f))
        .Set(Style::Width, StyleLength::Px(452.0f))
        .Set(Style::PaddingTop, StyleLength::Px(4.0f))
        .Set(Style::PaddingRight, StyleLength::Px(0.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(4.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(0.0f))
        .Set(Style::MinHeight, StyleLength::Px(28.0f));
    m_PaletteRow = paletteRow.get();
    const size_t paletteSize = std::max(m_PaletteColors.size(), kPaletteIconsPerRow);
    for (size_t i = 0; i < paletteSize; ++i)
    {
        const uint32_t argb = (i < m_PaletteColors.size()) ? m_PaletteColors[i] : kPaletteEmptySlot;
        auto paletteSwatch = std::make_unique<UIElement>();
        ApplyPaletteSwatchStyle(paletteSwatch.get(), argb);
        BindPaletteSwatchHandler(paletteSwatch.get());
        paletteRow->AddChild(std::move(paletteSwatch));
    }
    AddChild(std::move(paletteRow));

    // MouseUp on ColorPicker: if we were dragging from swatch and released over palette, add color
    RegisterEventHandler(kEventMouseUp, [this](UIEvent& ev) {
        if (!m_DraggingFromSwatch)
            return;
        UIElement* palette = FindById("colorpicker-palette");
        if (!palette)
        {
            m_DraggingFromSwatch = false;
            return;
        }
        for (UIElement* p = ev.Target; p && p != this; p = p->GetParent())
        {
            if (p == palette)
            {
                AddColorToPalette(ColorPickerValueToArgb(m_Value));
                break;
            }
        }
        m_DraggingFromSwatch = false;
    });

    SyncFromValue();
}

void ColorPicker::SyncFromValue()
{
    // Wheel mode elements
    if (m_Wheel)
    {
        m_Wheel->SetHue(m_Value.hsv.h);
        m_Wheel->SetSaturation(m_Value.hsv.s);
    }
    if (m_ValueBar)
    {
        m_ValueBar->SetHue(m_Value.hsv.h);
        m_ValueBar->SetSaturation(m_Value.hsv.s);
        m_ValueBar->SetValue(m_Value.hsv.v);
    }

    // Box + Hue Bar mode elements
    if (m_SvBox)
    {
        m_SvBox->SetHue(m_Value.hsv.h);
        m_SvBox->SetSaturation(m_Value.hsv.s);
        m_SvBox->SetValue(m_Value.hsv.v);
    }
    if (m_HueBar)
        m_HueBar->SetHue(m_Value.hsv.h);
    
    // Common elements
    if (m_AlphaSlider)
        m_AlphaSlider->SetValue(m_Value.hsv.a);
    if (m_IntensitySlider)
        m_IntensitySlider->SetValue(m_Value.intensity);
    UpdateSwatchAndFields();
}

void ColorPicker::SyncToValue()
{
    // Called when fields are committed - parse HSV or RGB and update m_Value
    OnHsvFieldChange();
}

void ColorPicker::NotifyChange()
{
    if (m_OnChange)
        m_OnChange(m_Value);
}

void ColorPicker::UpdateSwatchAndFields()
{
    float r, g, b;
    HsvToRgb(m_Value.hsv.h, m_Value.hsv.s, m_Value.hsv.v, r, g, b);
    uint8_t aInt = static_cast<uint8_t>(std::clamp(m_Value.hsv.a, 0.0f, 1.0f) * 255.0f);
    uint8_t rInt = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
    uint8_t gInt = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
    uint8_t bInt = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
    
    // Apply HDR intensity to swatch color (clamped to displayable range)
    float hdrR = std::clamp(r * m_Value.intensity, 0.0f, 1.0f);
    float hdrG = std::clamp(g * m_Value.intensity, 0.0f, 1.0f);
    float hdrB = std::clamp(b * m_Value.intensity, 0.0f, 1.0f);
    uint8_t hdrRInt = static_cast<uint8_t>(hdrR * 255.0f);
    uint8_t hdrGInt = static_cast<uint8_t>(hdrG * 255.0f);
    uint8_t hdrBInt = static_cast<uint8_t>(hdrB * 255.0f);
    
    // Update swatch with HDR-adjusted color and alpha
    if (m_SwatchEl)
    {
        const uint32_t swatchArgb =
            0xFF000000u | (static_cast<uint32_t>(hdrRInt) << 16) |
            (static_cast<uint32_t>(hdrGInt) << 8) | static_cast<uint32_t>(hdrBInt);
        m_SwatchEl->Overrides()
            .Set(Style::BackgroundColor, (uint32_t)swatchArgb)
            .Set(Style::Opacity, m_Value.hsv.a);
        m_SwatchEl->MarkDirty(VisualDirty);
    }
    
    // Update hex field (include alpha if not fully opaque)
    char hex[16];
    if (aInt < 255)
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X", aInt, rInt, gInt, bInt);
    else
        std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", rInt, gInt, bInt);
    if (m_HexField)
        m_HexField->SetValue(hex);

    // HSV fields
    if (m_HField)
        m_HField->SetValue(std::to_string(static_cast<int>(std::round(m_Value.hsv.h))));
    if (m_SField)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", m_Value.hsv.s);
        m_SField->SetValue(buf);
    }
    if (m_VField)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", m_Value.hsv.v);
        m_VField->SetValue(buf);
    }
    
    // RGB fields
    if (m_RField)
        m_RField->SetValue(std::to_string(rInt));
    if (m_GField)
        m_GField->SetValue(std::to_string(gInt));
    if (m_BField)
        m_BField->SetValue(std::to_string(bInt));
    if (m_AField)
        m_AField->SetValue(std::to_string(aInt));
    
    // Alpha value label
    if (m_AlphaValueLabel)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f%%", m_Value.hsv.a * 100.0f);
        m_AlphaValueLabel->SetText(buf);
    }
    
    // HDR value label
    if (m_IntensityValueLabel)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", m_Value.intensity);
        m_IntensityValueLabel->SetText(buf);
    }
}

void ColorPicker::OnWheelChange(float h, float s)
{
    m_Value.hsv.h = h;
    m_Value.hsv.s = s;
    if (m_ValueBar)
    {
        m_ValueBar->SetHue(h);
        m_ValueBar->SetSaturation(s);
    }
    UpdateSwatchAndFields();
    NotifyChange();
}

void ColorPicker::OnSVBoxChange(float s, float v)
{
    m_Value.hsv.s = s;
    m_Value.hsv.v = v;
    if (m_ValueBar)
        m_ValueBar->SetValue(v);
    UpdateSwatchAndFields();
    NotifyChange();
}

void ColorPicker::OnHueBarChange(float h)
{
    m_Value.hsv.h = h;
    if (m_SvBox)
        m_SvBox->SetHue(h);
    UpdateSwatchAndFields();
    NotifyChange();
}

void ColorPicker::OnValueSliderChange(float v)
{
    m_Value.hsv.v = v;
    UpdateSwatchAndFields();
    NotifyChange();
}

void ColorPicker::OnAlphaSliderChange(float alpha)
{
    m_Value.hsv.a = alpha;
    UpdateSwatchAndFields();
    NotifyChange();
}

void ColorPicker::OnIntensitySliderChange(float intensity)
{
    m_Value.intensity = intensity;
    UpdateSwatchAndFields();
    NotifyChange();
}

void ColorPicker::OnHsvFieldChange()
{
    if (!m_HField || !m_SField || !m_VField)
        return;
    try
    {
        float h = static_cast<float>(std::stod(m_HField->GetValue()));
        float s = static_cast<float>(std::stod(m_SField->GetValue()));
        float v = static_cast<float>(std::stod(m_VField->GetValue()));
        m_Value.hsv.h = std::fmod(h, 360.0f);
        if (m_Value.hsv.h < 0)
            m_Value.hsv.h += 360.0f;
        m_Value.hsv.s = std::clamp(s, 0.0f, 1.0f);
        m_Value.hsv.v = std::clamp(v, 0.0f, 1.0f);
        SyncFromValue();
        NotifyChange();
    }
    catch (...) {}
}

void ColorPicker::OnAlphaFieldChange()
{
    if (!m_AField)
        return;
    try
    {
        int ai = std::stoi(m_AField->GetValue());
        m_Value.hsv.a = std::clamp(ai / 255.0f, 0.0f, 1.0f);
        SyncFromValue();
        NotifyChange();
    }
    catch (...) {}
}

void ColorPicker::OnHexFieldChange()
{
    if (!m_HexField)
        return;
    std::string hexStr = m_HexField->GetValue();
    if (hexStr.empty())
        return;
    // Allow #AARRGGBB, #RRGGBB, AARRGGBB, or RRGGBB (with optional #)
    if (hexStr.size() >= 1 && hexStr[0] == '#')
        hexStr = hexStr.substr(1);
    if (hexStr.size() != 6 && hexStr.size() != 8)
        return;
    try
    {
        unsigned int value = 0;
        for (char c : hexStr)
        {
            int v = 0;
            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else
                return;
            value = (value << 4) | v;
        }
        float a, r, g, b;
        if (hexStr.size() == 8)
        {
            // AARRGGBB format
            a = ((value >> 24) & 0xFF) / 255.0f;
            r = ((value >> 16) & 0xFF) / 255.0f;
            g = ((value >> 8) & 0xFF) / 255.0f;
            b = (value & 0xFF) / 255.0f;
        }
        else
        {
            // RRGGBB format (alpha = 1)
            a = 1.0f;
            r = ((value >> 16) & 0xFF) / 255.0f;
            g = ((value >> 8) & 0xFF) / 255.0f;
            b = (value & 0xFF) / 255.0f;
        }
        float h, s, v;
        RgbToHsv(r, g, b, h, s, v);
        m_Value.hsv.h = h;
        m_Value.hsv.s = s;
        m_Value.hsv.v = v;
        m_Value.hsv.a = a;
        SyncFromValue();
        NotifyChange();
    }
    catch (...) {}
}

void ColorPicker::OnRgbFieldChange()
{
    if (!m_RField || !m_GField || !m_BField)
        return;
    try
    {
        int ri = std::stoi(m_RField->GetValue());
        int gi = std::stoi(m_GField->GetValue());
        int bi = std::stoi(m_BField->GetValue());
        float r = std::clamp(ri / 255.0f, 0.0f, 1.0f);
        float g = std::clamp(gi / 255.0f, 0.0f, 1.0f);
        float b = std::clamp(bi / 255.0f, 0.0f, 1.0f);
        float h, s, v;
        RgbToHsv(r, g, b, h, s, v);
        m_Value.hsv.h = h;
        m_Value.hsv.s = s;
        m_Value.hsv.v = v;
        SyncFromValue();
        NotifyChange();
    }
    catch (...) {}
}

void ColorPicker::RemovePaletteColor(uint32_t index)
{
    if (index >= m_PaletteColors.size())
        return;
    if (m_PaletteColors[index] == kPaletteEmptySlot)
        return;
    UIElement* paletteRow = m_PaletteRow ? m_PaletteRow : FindById("colorpicker-palette");
    m_PaletteRow = paletteRow;
    if (!paletteRow || index >= paletteRow->GetChildren().size())
        return;
    m_PaletteColors[index] = kPaletteEmptySlot;
    // Compact: move all non-empty left, all empty right
    std::vector<uint32_t> compacted;
    compacted.reserve(m_PaletteColors.size());
    for (uint32_t c : m_PaletteColors)
        if (c != kPaletteEmptySlot)
            compacted.push_back(c);
    for (size_t i = compacted.size(); i < m_PaletteColors.size(); ++i)
        compacted.push_back(kPaletteEmptySlot);
    m_PaletteColors = std::move(compacted);

    // Remove trailing full empty rows, but always keep at least one row (kPaletteIconsPerRow slots)
    size_t trailing_empty = 0;
    for (size_t i = m_PaletteColors.size(); i > 0 && m_PaletteColors[i - 1] == kPaletteEmptySlot; --i)
        ++trailing_empty;
    const size_t full_empty_rows = trailing_empty / kPaletteIconsPerRow;
    const size_t new_size = std::max(kPaletteIconsPerRow,
                                    m_PaletteColors.size() - full_empty_rows * kPaletteIconsPerRow);
    if (new_size < m_PaletteColors.size())
    {
        m_PaletteColors.resize(new_size);
        const auto& children = paletteRow->GetChildren();
        std::vector<UIElement*> toRemove;
        for (size_t i = new_size; i < children.size(); ++i)
            toRemove.push_back(children[i].get());
        for (UIElement* p : toRemove)
            paletteRow->RemoveChild(p);
    }

    // Update each swatch style to match new order
    const auto& children = paletteRow->GetChildren();
    for (size_t i = 0; i < children.size() && i < m_PaletteColors.size(); ++i)
    {
        ApplyPaletteSwatchStyle(children[i].get(), m_PaletteColors[i]);
    }
    SavePaletteToPreferences();
}

void ColorPicker::SavePaletteToPreferences()
{
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    nlohmann::json arr = nlohmann::json::array();
    for (uint32_t c : m_PaletteColors)
        arr.push_back(static_cast<uint64_t>(c));
    prefs.SetJson("colorpicker.palette", arr);
    (void)prefs.Save(&err);
}

void ColorPicker::OnPaletteClick(uint32_t index)
{
    if (index >= m_PaletteColors.size())
        return;
    uint32_t argb = m_PaletteColors[index];
    if (argb == kPaletteEmptySlot)
        return;
    float r = ((argb >> 16) & 0xFF) / 255.0f;
    float g = ((argb >> 8) & 0xFF) / 255.0f;
    float b = (argb & 0xFF) / 255.0f;
    float h, s, v;
    RgbToHsv(r, g, b, h, s, v);
    m_Value.hsv.h = h;
    m_Value.hsv.s = s;
    m_Value.hsv.v = v;
    SyncFromValue();
    NotifyChange();
}

uint32_t ColorPicker::FindPaletteIndexForSwatch(const UIElement* swatch)
{
    UIElement* paletteRow = m_PaletteRow ? m_PaletteRow : FindById("colorpicker-palette");
    if (!paletteRow || !swatch)
        return static_cast<uint32_t>(-1);
    const auto& children = paletteRow->GetChildren();
    for (size_t i = 0; i < children.size(); ++i)
    {
        if (children[i].get() == swatch)
            return static_cast<uint32_t>(i);
    }
    return static_cast<uint32_t>(-1);
}

void ColorPicker::BindPaletteSwatchHandler(UIElement* swatch)
{
    if (!swatch)
        return;
    swatch->RegisterEventHandler(kEventMouseDown, [this, swatch](UIEvent& ev)
    {
        const uint32_t index = FindPaletteIndexForSwatch(swatch);
        if (index == static_cast<uint32_t>(-1))
            return;
        if (ev.Button == 1 || ev.Button == 2) // Right-click (RMB; 1=Win/Linux, 2=some macOS)
        {
            RemovePaletteColor(index);
            ev.Stop();
            return;
        }
        if (ev.Button != 0)
            return;
        if (index < m_PaletteColors.size() && m_PaletteColors[index] != kPaletteEmptySlot)
            OnPaletteClick(index);
        ev.Stop();
    });
}

void ColorPicker::AddColorToPalette(uint32_t argb)
{
    UIElement* paletteRow = m_PaletteRow ? m_PaletteRow : FindById("colorpicker-palette");
    m_PaletteRow = paletteRow;
    if (!paletteRow)
        return;
    const auto& children = paletteRow->GetChildren();
    for (size_t i = 0; i < m_PaletteColors.size() && i < children.size(); ++i)
    {
        if (m_PaletteColors[i] == kPaletteEmptySlot)
        {
            m_PaletteColors[i] = argb;
            ApplyPaletteSwatchStyle(children[i].get(), argb);
            SavePaletteToPreferences();
            return;
        }
    }
    // Don't add more if we've reached the max
    if (m_PaletteColors.size() >= kPaletteMaxSlots)
        return;
    m_PaletteColors.push_back(argb);
    auto swatch = std::make_unique<UIElement>();
    ApplyPaletteSwatchStyle(swatch.get(), argb);
    BindPaletteSwatchHandler(swatch.get());
    paletteRow->AddChild(std::move(swatch));
    SavePaletteToPreferences();
}

void ColorPicker::ResetPalette()
{
    // Store current palette for undo
    m_PaletteUndoState = m_PaletteColors;
    m_CanUndoPaletteReset = true;
    
    // Reset to default palette
    m_PaletteColors = DefaultPalette();
    RebuildPaletteUI();
    SavePaletteToPreferences();
}

void ColorPicker::UndoPaletteReset()
{
    if (!m_CanUndoPaletteReset)
        return;
    
    // Restore previous palette
    m_PaletteColors = std::move(m_PaletteUndoState);
    m_CanUndoPaletteReset = false;
    
    RebuildPaletteUI();
    SavePaletteToPreferences();
}

void ColorPicker::RebuildPaletteUI()
{
    UIElement* paletteRow = m_PaletteRow ? m_PaletteRow : FindById("colorpicker-palette");
    m_PaletteRow = paletteRow;
    if (!paletteRow)
        return;
    
    // Remove all existing swatches
    std::vector<UIElement*> toRemove;
    for (const auto& child : paletteRow->GetChildren())
        toRemove.push_back(child.get());
    for (UIElement* p : toRemove)
        paletteRow->RemoveChild(p);
    
    // Rebuild swatches
    const size_t paletteSize = std::max(m_PaletteColors.size(), kPaletteIconsPerRow);
    for (size_t i = 0; i < paletteSize; ++i)
    {
        const uint32_t argb = (i < m_PaletteColors.size()) ? m_PaletteColors[i] : kPaletteEmptySlot;
        auto swatch = std::make_unique<UIElement>();
        ApplyPaletteSwatchStyle(swatch.get(), argb);
        BindPaletteSwatchHandler(swatch.get());
        paletteRow->AddChild(std::move(swatch));
    }
}

} // namespace GameEngine

namespace
{
[[maybe_unused]] auto s_reg_colorpicker = GameEngine::UIRegistration::RegisterWithFactory<GameEngine::ColorPicker>(
    "ColorPicker",
    []() { return std::make_unique<GameEngine::ColorPicker>(); });
}
