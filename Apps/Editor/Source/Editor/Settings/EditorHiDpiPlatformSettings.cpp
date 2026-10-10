#include "Editor/Settings/EditorHiDpiPlatformSettings.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include "Editor/Settings/SettingsStore.h"
#include "UI/UIPlatform.h"

namespace GameEngine::Editor
{
namespace
{
struct HiDpiScalePreset
{
    const char* key; // Dropdown / prefs serialization
    double mult;
};

constexpr HiDpiScalePreset kHiDpiScalePresets[] = {
    {"1", 1.0},
    {"1.25", 1.25},
    {"1.5", 1.5},
    {"2", 2.0},
};
} // namespace

float NormalizeHiDpiContentScaleMultiplier(double storedValue)
{
    double best = kHiDpiScalePresets[0].mult;
    double bestDist = 1e9;
    for (const HiDpiScalePreset& p : kHiDpiScalePresets)
    {
        const double d = std::abs(storedValue - p.mult);
        if (d < bestDist)
        {
            bestDist = d;
            best = p.mult;
        }
    }
    return static_cast<float>(best);
}

std::string HiDpiContentScaleMultiplierPresetKey(double storedValue)
{
    const float n = NormalizeHiDpiContentScaleMultiplier(storedValue);
    for (const HiDpiScalePreset& p : kHiDpiScalePresets)
    {
        if (std::abs(static_cast<double>(n) - p.mult) < 1e-5)
            return p.key;
    }
    return kHiDpiScalePresets[0].key;
}

double HiDpiContentScaleMultiplierFromPresetKey(std::string_view key)
{
    for (const HiDpiScalePreset& p : kHiDpiScalePresets)
    {
        if (key == p.key)
            return p.mult;
    }
    return kHiDpiScalePresets[0].mult;
}

float GetSavedHiDpiContentScaleMultiplier()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);
    double mult = 1.0;
    if (!prefs.TryGetDouble("ui.hidpiContentScaleMultiplier", mult))
        mult = 1.0;
    return NormalizeHiDpiContentScaleMultiplier(mult);
}

void ApplySavedHiDpiPlatformSettings(UI::IPlatformApi* platform)
{
    if (!platform)
        return;

    SettingsStore prefs = OpenEditorPreferences();
    std::string err;
    (void)prefs.Load(&err);

    bool useSystemScale = true;
    (void)prefs.TryGetBool("ui.hidpiUseSystemScale", useSystemScale);
    double mult = 1.0;
    if (!prefs.TryGetDouble("ui.hidpiContentScaleMultiplier", mult))
        mult = 1.0;
    const float multF = NormalizeHiDpiContentScaleMultiplier(mult);

    platform->SetUseSystemContentScale(useSystemScale);
    platform->SetContentScaleMultiplier(multF);
}

} // namespace GameEngine::Editor
