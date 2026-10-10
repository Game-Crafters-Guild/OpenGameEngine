#include "Editor/Settings/GameUIScaleSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/UIScaleProjectSettingsWriter.h"
#include "Engine/GameUI/UIScaleProjectSettings.h"
#include "Logger/Logger.h"

#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
constexpr float kReferenceSizeStep = 1.0f;

const std::filesystem::path& WorkspaceRoot()
{
    return EngineCore::GetInstance().GetWorkspaceRoot();
}

UI::UIScaleSettings LoadSettings()
{
    return UIScaleProjectSettings::Load(WorkspaceRoot());
}

void Commit(const UI::UIScaleSettings& settings, const char* what)
{
    if (!SaveUIScaleProjectSettings(WorkspaceRoot(), settings))
        Logger::Log::Warning("GameUIScaleSettings: cannot save {} -- no writable project settings", what);
}

std::vector<SettingsFieldDescriptor::DropdownField::Option> ModeOptions()
{
    using Option = SettingsFieldDescriptor::DropdownField::Option;
    return {
        Option{UIScaleProjectSettings::ToModeToken(UI::UIScaleMode::Platform), "Platform DPI"},
        Option{UIScaleProjectSettings::ToModeToken(UI::UIScaleMode::Width), "Match Width"},
        Option{UIScaleProjectSettings::ToModeToken(UI::UIScaleMode::Height), "Match Height"},
        Option{UIScaleProjectSettings::ToModeToken(UI::UIScaleMode::Fit), "Fit"},
        Option{UIScaleProjectSettings::ToModeToken(UI::UIScaleMode::Fill), "Fill"},
    };
}

SettingsFieldDescriptor ReferenceSizeRow(const char* label, const char* tooltip, const char* keywords,
                                         float UI::UIScaleSettings::*member, float defaultValue,
                                         const char* what)
{
    SettingsFieldDescriptor row;
    row.Label = label;
    row.Tooltip = tooltip;
    row.SearchKeywords = keywords;
    SettingsFieldDescriptor::SliderField slider;
    slider.DefaultValue = defaultValue;
    slider.MinValue = UIScaleProjectSettings::kMinReferenceSize;
    slider.MaxValue = UIScaleProjectSettings::kMaxReferenceSize;
    slider.Step = kReferenceSizeStep;
    slider.Get = [member]() { return LoadSettings().*member; };
    slider.Set = [member, what](float value)
    {
        UI::UIScaleSettings settings = LoadSettings();
        // The row fires once with the value it was seeded from: opening the
        // page is not an edit.
        if (settings.*member == value)
            return;
        settings.*member = value;
        Commit(settings, what);
    };
    row.Control = slider;
    return row;
}
} // namespace

void RegisterGameUIScaleSettingsCategory()
{
    const UI::UIScaleSettings defaults{};

    SettingsCategoryDescriptor page;
    page.CategoryId = "gameUiScaling";
    page.Title = "Game UI Scaling";
    page.Group = SettingsCategoryGroup::ProjectSettings;
    page.TreeRowClass = "game-ui-scaling-row";
    page.Description =
        "How the game's UI documents scale with the size of the view they draw into. Platform DPI "
        "follows the display's scale factor. The other modes treat the UI as authored at the "
        "reference size and scale the whole interface -- text, spacing, borders and effects -- "
        "by the ratio of the view to that size: Match Width and Match Height use one axis, Fit "
        "the smaller ratio so the reference canvas stays visible, Fill the larger so it covers "
        "the view. The Game View and the Scene View's game UI preview apply it as you edit, and "
        "a build ships the same setting.";

    SettingsFieldDescriptor mode;
    mode.Label = "Scale Mode";
    mode.Tooltip = "Which ratio of view size to reference size scales the game UI.";
    mode.SearchKeywords = "game ui hud scale scaling reference resolution dpi fit fill width height";
    SettingsFieldDescriptor::DropdownField modeDropdown;
    modeDropdown.OptionsProvider = ModeOptions;
    modeDropdown.DefaultValue = UIScaleProjectSettings::ToModeToken(defaults.mode);
    modeDropdown.Get = []() -> std::string { return UIScaleProjectSettings::ToModeToken(LoadSettings().mode); };
    modeDropdown.Set = [](const std::string& token)
    {
        UI::UIScaleSettings settings = LoadSettings();
        const UI::UIScaleMode selected = UIScaleProjectSettings::ParseModeToken(token);
        if (settings.mode == selected)
            return;
        settings.mode = selected;
        Commit(settings, "game UI scale mode");
    };
    mode.Control = modeDropdown;
    page.Fields.push_back(std::move(mode));

    page.Fields.push_back(ReferenceSizeRow(
        "Reference Width",
        "The view width, in UI pixels, the game UI is authored at. Used by Match Width, Fit and "
        "Fill.",
        "game ui hud reference width resolution scale", &UI::UIScaleSettings::referenceWidth,
        defaults.referenceWidth, "game UI reference width"));
    page.Fields.push_back(ReferenceSizeRow(
        "Reference Height",
        "The view height, in UI pixels, the game UI is authored at. Used by Match Height, Fit and "
        "Fill.",
        "game ui hud reference height resolution scale", &UI::UIScaleSettings::referenceHeight,
        defaults.referenceHeight, "game UI reference height"));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(page));
}

} // namespace GameEngine::Editor
