#pragma once

#include <string>
#include <string_view>

namespace GameEngine
{
namespace UI
{
class IPlatformApi;
}

namespace Editor
{

// Applies `ui.hidpiUseSystemScale` and `ui.hidpiContentScaleMultiplier` from
// editor preferences to the per-window UI::IPlatformApi. Implementations that
// don't override the setters (default IPlatformApi) silently ignore them.
void ApplySavedHiDpiPlatformSettings(UI::IPlatformApi* platform);

// Reads `ui.hidpiContentScaleMultiplier` from editor preferences and returns it
// normalized to a supported preset (1.0 if unset). Used by standalone windows
// (e.g. color picker) that need to size themselves accounting for the user's
// additional UI scale.
float GetSavedHiDpiContentScaleMultiplier();

// Additional UI scale is limited to 100% / 125% / 150% / 200% (multipliers 1.0–2.0).
float NormalizeHiDpiContentScaleMultiplier(double storedValue);
// Preset keys for preferences / tooling: "1", "1.25", "1.5", "2"
std::string HiDpiContentScaleMultiplierPresetKey(double storedValue);
double HiDpiContentScaleMultiplierFromPresetKey(std::string_view key);

} // namespace Editor
} // namespace GameEngine
