#pragma once

#include "Platform/ContextMenu.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

// Small formatting pieces shared by the editor's preset menus (a submenu of float
// presets with the current one checked, a color row with its swatch).
namespace GameEngine::Editor::ContextMenuLabels
{

// "#RRGGBB" for a menu swatch from a picker's ARGB value.
inline std::string ArgbToHexRGB(uint32_t argb)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", argb & 0xFFFFFFu);
    return buf;
}

// A preset's row label: the shortest text that reads back as the value.
inline std::string FloatLabel(float v)
{
    std::ostringstream label;
    label << v;
    return label.str();
}

constexpr uint32_t CheckedFlag(bool v)
{
    return v ? MenuItemFlag_Checked : MenuItemFlag_None;
}

// Whether a preset is the current value, within the menus' display precision.
inline bool NearlyEqual(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

} // namespace GameEngine::Editor::ContextMenuLabels
