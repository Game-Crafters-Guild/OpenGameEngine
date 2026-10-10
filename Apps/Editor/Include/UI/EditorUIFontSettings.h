#pragma once

#include <string>
#include <vector>

namespace GameEngine
{

class UIManager;

namespace Engine::UI
{
class FontResolver;
}

namespace Editor
{

// Preference keys — preset ids (see EditorUIFontSettings.cpp), not raw CSS.
constexpr const char* kPrefUiFontPreset = "ui.fontFamilyPreset";
constexpr const char* kPrefScriptFontPreset = "ui.fontFamilyScriptPreset";

struct EditorFontDropdownOption
{
    std::string value;
    std::string label;
};

void ApplySavedEditorFontPreferences(UIManager* ui);
void PrefetchEditorFontOptions(UIManager* ui);

// The family stack the Script Editor renders with, in CSS order and with
// quotes stripped: `scriptFontPreset`'s stack (see kPrefScriptFontPreset), or
// the theme default when it is empty or unknown. Callers that need the resolved
// face's metrics rather than its name can feed this straight to
// UIManager::ResolveFontForStyle.
std::vector<std::string> GetScriptFontFamilyStack(const std::string& scriptFontPreset);

// Build dropdown rows from presets whose primary font exists (assets or OS). Always includes
// theme / system stacks (e.g. default, system_ui, system_mono).
std::vector<EditorFontDropdownOption> BuildEditorUiFontDropdownOptions(Engine::UI::FontResolver& resolver);
std::vector<EditorFontDropdownOption> BuildScriptFontDropdownOptions(Engine::UI::FontResolver& resolver);
std::string SanitizeFontPresetAgainstOptions(const std::string& stored, const std::vector<EditorFontDropdownOption>& options);

} // namespace Editor

} // namespace GameEngine
