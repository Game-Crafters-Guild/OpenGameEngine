#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace GameEngine
{

struct Stylesheet;

namespace UI
{

// Helper for generating dynamic accent color CSS styles.
// Extracts the accent color styling logic from EditorApplication to keep it modular.
namespace AccentStyleHelper
{

// Default accent color (blue)
constexpr uint32_t kDefaultAccentColor = 0xFF3A8FFF;

// Editor preference key that stores the accent color (ARGB as int64).
constexpr const char* kPrefKeyAccentColor = "ui.accentColor";

// Generate the `:root` accent-variable overrides for the given accent color
// (ARGB format: 0xAARRGGBB). The accent-driven rules themselves live in the
// static theme stylesheets and consume these variables.
std::string GenerateAccentColorCSS(uint32_t accentColor);

// Build a parsed stylesheet for the given accent color.
// Returns nullptr if parsing fails.
std::shared_ptr<Stylesheet> BuildAccentColorStylesheet(uint32_t accentColor);

// Calculate hover variant of a color (lighter)
uint32_t CalculateHoverColor(uint32_t argb);

// Calculate pressed variant of a color (darker)
uint32_t CalculatePressedColor(uint32_t argb);

// Calculate the darker picker variants used by Search Dialog selections.
uint32_t CalculatePickerColor(uint32_t argb);
uint32_t CalculatePickerHoverColor(uint32_t argb);
uint32_t CalculatePickerPressedColor(uint32_t argb);

} // namespace AccentStyleHelper

} // namespace UI
} // namespace GameEngine
