#include "UIAccentStyleHelper.h"

#include <cstdio>

#include "UI/Parsers/CSSParser.h"
#include "UI/UIStyle.h"

namespace GameEngine
{
namespace UI
{
namespace AccentStyleHelper
{

uint32_t CalculateHoverColor(uint32_t argb)
{
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;

    constexpr float kLightenFactor = 0.30f;
    r = static_cast<uint8_t>(r + static_cast<int>((255 - r) * kLightenFactor));
    g = static_cast<uint8_t>(g + static_cast<int>((255 - g) * kLightenFactor));
    b = static_cast<uint8_t>(b + static_cast<int>((255 - b) * kLightenFactor));

    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

uint32_t CalculatePressedColor(uint32_t argb)
{
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;

    constexpr float kDarkenFactor = 0.25f;
    r = static_cast<uint8_t>(static_cast<int>(r * (1.0f - kDarkenFactor)));
    g = static_cast<uint8_t>(static_cast<int>(g * (1.0f - kDarkenFactor)));
    b = static_cast<uint8_t>(static_cast<int>(b * (1.0f - kDarkenFactor)));

    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

namespace
{
uint32_t ScaleRgb(uint32_t argb, float factor)
{
    const uint8_t a = (argb >> 24) & 0xFF;
    const uint8_t r = static_cast<uint8_t>(((argb >> 16) & 0xFF) * factor);
    const uint8_t g = static_cast<uint8_t>(((argb >> 8) & 0xFF) * factor);
    const uint8_t b = static_cast<uint8_t>((argb & 0xFF) * factor);
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}
}

uint32_t CalculatePickerColor(uint32_t argb)
{
    return ScaleRgb(argb, 0.675f);
}

uint32_t CalculatePickerHoverColor(uint32_t argb)
{
    return ScaleRgb(argb, 0.75f);
}

uint32_t CalculatePickerPressedColor(uint32_t argb)
{
    return ScaleRgb(argb, 0.60f);
}

std::string GenerateAccentColorCSS(uint32_t accentColor)
{
    const uint8_t r = (accentColor >> 16) & 0xFF;
    const uint8_t g = (accentColor >> 8) & 0xFF;
    const uint8_t b = accentColor & 0xFF;

    const uint32_t hoverColor = CalculateHoverColor(accentColor);
    const uint32_t pressedColor = CalculatePressedColor(accentColor);
    const uint32_t pickerColor = CalculatePickerColor(accentColor);
    const uint32_t pickerHoverColor = CalculatePickerHoverColor(accentColor);
    const uint32_t pickerPressedColor = CalculatePickerPressedColor(accentColor);

    const uint8_t rHover = (hoverColor >> 16) & 0xFF;
    const uint8_t gHover = (hoverColor >> 8) & 0xFF;
    const uint8_t bHover = hoverColor & 0xFF;
    const uint8_t rPressed = (pressedColor >> 16) & 0xFF;
    const uint8_t gPressed = (pressedColor >> 8) & 0xFF;
    const uint8_t bPressed = pressedColor & 0xFF;
    const uint8_t rPicker = (pickerColor >> 16) & 0xFF;
    const uint8_t gPicker = (pickerColor >> 8) & 0xFF;
    const uint8_t bPicker = pickerColor & 0xFF;
    const uint8_t rPickerHover = (pickerHoverColor >> 16) & 0xFF;
    const uint8_t gPickerHover = (pickerHoverColor >> 8) & 0xFF;
    const uint8_t bPickerHover = pickerHoverColor & 0xFF;
    const uint8_t rPickerPressed = (pickerPressedColor >> 16) & 0xFF;
    const uint8_t gPickerPressed = (pickerPressedColor >> 8) & 0xFF;
    const uint8_t bPickerPressed = pickerPressedColor & 0xFF;

    // Only the :root token overrides are dynamic. Every accent-driven rule
    // lives in the static theme stylesheets and consumes these variables
    // (tokens.css declares the defaults), so changing the accent is a
    // variable redefinition, never a second copy of the rules.
    char cssText[1024];
    std::snprintf(cssText, sizeof(cssText),
        ":root { "
        "--ui_color_accent: #%02X%02X%02X; "
        "--ui_color_accent_highlight: rgba(%d, %d, %d, 0.3); "
        "--ui_color_accent_translucent: rgba(%d, %d, %d, 0.2); "
        "--ui_color_accent_blue: #%02X%02X%02X; "
        "--ui_color_accent_blue_hover: #%02X%02X%02X; "
        "--ui_color_accent_blue_pressed: #%02X%02X%02X; "
        "--ui_color_picker_accent: #%02X%02X%02X; "
        "--ui_color_picker_accent_hover: #%02X%02X%02X; "
        "--ui_color_picker_accent_pressed: #%02X%02X%02X; "
        "}",
        r, g, b,                                    // --ui_color_accent
        r, g, b,                                    // --ui_color_accent_highlight
        r, g, b,                                    // --ui_color_accent_translucent
        r, g, b,                                    // --ui_color_accent_blue
        rHover, gHover, bHover,                     // --ui_color_accent_blue_hover
        rPressed, gPressed, bPressed,               // --ui_color_accent_blue_pressed
        rPicker, gPicker, bPicker,                  // --ui_color_picker_accent
        rPickerHover, gPickerHover, bPickerHover,   // --ui_color_picker_accent_hover
        rPickerPressed, gPickerPressed, bPickerPressed); // --ui_color_picker_accent_pressed

    return std::string(cssText);
}

std::shared_ptr<Stylesheet> BuildAccentColorStylesheet(uint32_t accentColor)
{
    std::string cssText = GenerateAccentColorCSS(accentColor);
    auto sheet = std::make_shared<Stylesheet>();
    if (!UIParsing::CSSParser::ParseStylesFromString(cssText.c_str(), *sheet))
        return nullptr;
    return sheet;
}

} // namespace AccentStyleHelper
} // namespace UI
} // namespace GameEngine
