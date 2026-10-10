#include "Editor/Settings/UIScaleProjectSettingsWriter.h"

#include "Editor/Settings/SettingsStore.h"
#include "Engine/GameUI/UIScaleProjectSettings.h"
#include "Logger/Logger.h"

#include <string>

namespace GameEngine::Editor
{

bool SaveUIScaleProjectSettings(const std::filesystem::path& workspaceRoot,
                                const UI::UIScaleSettings& settings)
{
    if (workspaceRoot.empty())
        return false;

    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);

    auto& root = store.Json();
    if (!root.is_object())
        root = nlohmann::json::object();
    UIScaleProjectSettings::WriteTo(settings, root[UIScaleProjectSettings::kUIScaleKey]);

    if (!store.Save(&err))
    {
        Logger::Log::Error("UIScaleProjectSettings: failed to save project settings: {}", err);
        return false;
    }
    UIScaleProjectSettingsSaved().Invoke();
    return true;
}

Event<>& UIScaleProjectSettingsSaved()
{
    static Event<> saved;
    return saved;
}

} // namespace GameEngine::Editor
