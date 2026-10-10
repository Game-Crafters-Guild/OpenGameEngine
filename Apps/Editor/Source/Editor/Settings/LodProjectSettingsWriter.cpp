#include "Editor/Settings/LodProjectSettingsWriter.h"

#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/LodProjectSettings.h"
#include "Logger/Logger.h"

#include <string>

namespace GameEngine::Editor
{

bool SaveLodProjectSettings(const std::filesystem::path& workspaceRoot,
                            const Rendering::LodProjectSettings& settings)
{
    if (workspaceRoot.empty())
        return false;

    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);

    auto& root = store.Json();
    if (!root.is_object())
        root = nlohmann::json::object();
    auto& rendering = root[Rendering::LodProjectSettings::kRenderingKey];
    if (!rendering.is_object())
        rendering = nlohmann::json::object();

    settings.WriteTo(rendering);

    if (!store.Save(&err))
    {
        Logger::Log::Error("LodProjectSettings: failed to save project settings: {}", err);
        return false;
    }
    return true;
}

} // namespace GameEngine::Editor
