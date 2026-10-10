#include "Editor/Settings/ExperimentalRenderingSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "Rendering/Passes/TemporalAAOptions.h"
#include "Rendering/Passes/TemporalDither.h"

#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

// Preference keys. The panel persists a row's value under PrefKey itself; these
// same keys are read back at startup below, because the row's Set only fires
// once the page is opened and the renderer must not wait for that.
constexpr const char* kMovieDitherPrefKey = "experimental.temporalMovieDither";
constexpr const char* kScreenDitherPrefKey = "experimental.temporalScreenDither";
constexpr const char* kTaaSubpixelCorrectionPrefKey = "experimental.taaSubpixelCorrection";

SettingsFieldDescriptor MakeToggle(const char* label, const char* prefKey, const char* tooltip,
                                   const char* searchKeywords, bool (*read)(), void (*write)(bool),
                                   bool defaultValue = false)
{
    SettingsFieldDescriptor field;
    field.Label = label;
    field.Tooltip = tooltip;
    field.SearchKeywords = searchKeywords;
    field.PrefKey = prefKey;
    SettingsFieldDescriptor::ToggleField toggle;
    toggle.DefaultValue = defaultValue;
    // Reads the live renderer state, so the row shows what frames are actually
    // doing rather than what the file last said.
    toggle.Get = read;
    toggle.Set = write;
    field.Control = std::move(toggle);
    return field;
}

// Startup application of the stored values. Without it the toggles would only
// take effect after the user visited the page, which makes a persisted "on"
// silently inert for a whole session.
void ApplyStoredValues()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);

    bool movieDither = false;
    if (prefs.TryGetBool(kMovieDitherPrefKey, movieDither))
        Rendering::Passes::SetTemporalMovieDitherEnabled(movieDither);
    bool screenDither = false;
    if (prefs.TryGetBool(kScreenDitherPrefKey, screenDither))
        Rendering::Passes::SetTemporalScreenDitherEnabled(screenDither);
    bool taaSubpixelCorrection = true;
    if (prefs.TryGetBool(kTaaSubpixelCorrectionPrefKey, taaSubpixelCorrection))
        Rendering::Passes::SetTaaSubpixelCorrectionEnabled(taaSubpixelCorrection);
}

} // namespace

void RegisterExperimentalRenderingSettingsCategory()
{
    SettingsCategoryDescriptor experimental;
    experimental.CategoryId = "experimental";
    experimental.Title = "Experimental";
    experimental.Group = SettingsCategoryGroup::UserSettings;
    experimental.TreeRowClass = "experimental-row";
    experimental.SearchKeywords = "experimental temporal dither banding grain noise taa subpixel";
    experimental.Description =
        "Renderer switches under evaluation. They are per-user (defaults noted per switch) and "
        "may change or be removed once their A/B lands.";

    experimental.Fields.push_back(MakeToggle(
        "Temporal Movie Dither", kMovieDitherPrefKey,
        "Advances the recording's dither pattern once per captured frame instead of holding it "
        "still. Only does anything when the recording re-quantizes — a 10-bit screen recorded to "
        "an 8-bit movie — and nothing on screen changes either way. Costs bitrate: per-frame noise "
        "is harder to compress than a still pattern.",
        "temporal movie dither recording banding grain video",
        &Rendering::Passes::IsTemporalMovieDitherEnabled,
        &Rendering::Passes::SetTemporalMovieDitherEnabled));

    experimental.Fields.push_back(MakeToggle(
        "Temporal Screen Dither", kScreenDitherPrefKey,
        "Advances the on-screen dither pattern once per rendered frame. Banding a still 1-LSB "
        "dither cannot break can dissolve as the eye averages over frames; the trade is that flat "
        "areas can read as shimmering rather than still.",
        "temporal screen dither banding grain shimmer viewport",
        &Rendering::Passes::IsTemporalScreenDitherEnabled,
        &Rendering::Passes::SetTemporalScreenDitherEnabled));

    experimental.Fields.push_back(MakeToggle(
        "TAA Sub-Pixel Correction", kTaaSubpixelCorrectionPrefKey,
        "Reduces TAA history trust as a pixel's motion phase moves away from texel alignment — "
        "the worst case for bilinear-alike history resampling — which reduces blur under slow "
        "sub-pixel motion. The reference implementation this was ported from notes a possible "
        "square-pattern artifact as the trade; on by default to match it, turn off to compare.",
        "taa temporal antialiasing subpixel correction blur sharpness motion",
        &Rendering::Passes::IsTaaSubpixelCorrectionEnabled,
        &Rendering::Passes::SetTaaSubpixelCorrectionEnabled,
        /*defaultValue=*/true));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(experimental));
    ApplyStoredValues();
}

} // namespace GameEngine::Editor
