#include "Editor/Settings/BuildSettingsStore.h"

#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"

namespace GameEngine::Editor
{
namespace
{

constexpr const char* kGlobalBuildScenesPrefKey = "build.globalScenes";

SettingsStore OpenBuildSettingsStore()
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    if (!root.empty())
        return OpenProjectSettings(root);
    return OpenEditorPreferences();
}

// One newline-separated scene list, trimmed of blank and whitespace-only lines
// so a stray trailing newline never becomes an empty scene entry.
std::vector<std::string> ParseSceneList(const std::string& raw)
{
    std::vector<std::string> out;
    for (size_t start = 0; start < raw.size();)
    {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos)
            end = raw.size();
        std::string line = raw.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t'))
            ++first;
        if (first < line.size())
            line = line.substr(first);
        if (!line.empty())
            out.push_back(std::move(line));
        start = end + (end < raw.size() ? 1u : 0u);
    }
    return out;
}

std::string JoinSceneList(const std::vector<std::string>& scenes)
{
    std::string raw;
    for (size_t i = 0; i < scenes.size(); ++i)
    {
        if (i)
            raw += '\n';
        raw += scenes[i];
    }
    return raw;
}

std::vector<std::string> LoadSceneList(const std::string& prefKey)
{
    const std::string raw = LoadBuildStringSetting(prefKey);
    if (raw.empty())
        return {};
    return ParseSceneList(raw);
}
} // namespace

std::string LoadBuildStringSetting(const std::string& prefKey, const std::string& defaultValue)
{
    auto prefs = OpenBuildSettingsStore();
    std::string err;
    prefs.Load(&err);
    std::string value;
    if (!prefs.TryGetString(prefKey, value) || value.empty())
        return defaultValue;
    return value;
}

void SaveBuildStringSetting(const std::string& prefKey, const std::string& value)
{
    auto prefs = OpenBuildSettingsStore();
    std::string err;
    prefs.Load(&err);
    prefs.SetString(prefKey, value);
    prefs.Save(&err);
}

bool LoadBuildBoolSetting(const std::string& prefKey, bool defaultValue)
{
    auto prefs = OpenBuildSettingsStore();
    std::string err;
    prefs.Load(&err);
    bool value = defaultValue;
    prefs.TryGetBool(prefKey, value);
    return value;
}

void SaveBuildBoolSetting(const std::string& prefKey, bool value)
{
    auto prefs = OpenBuildSettingsStore();
    std::string err;
    prefs.Load(&err);
    prefs.SetBool(prefKey, value);
    prefs.Save(&err);
}

std::string BuildPlatformPrefKey(const std::string& platformName, const std::string& leaf)
{
    return "build.platform." + platformName + "." + leaf;
}

std::vector<std::string> LoadBuildScenes(const std::string& platformName)
{
    return LoadSceneList(BuildPlatformPrefKey(platformName, "scenes"));
}

void SaveBuildScenes(const std::string& platformName, const std::vector<std::string>& scenes)
{
    SaveBuildStringSetting(BuildPlatformPrefKey(platformName, "scenes"), JoinSceneList(scenes));
}

std::vector<std::string> LoadGlobalBuildScenes()
{
    return LoadSceneList(kGlobalBuildScenesPrefKey);
}

void SaveGlobalBuildScenes(const std::vector<std::string>& scenes)
{
    SaveBuildStringSetting(kGlobalBuildScenesPrefKey, JoinSceneList(scenes));
}

bool LoadBuildPlatformUseGlobalScenes(const std::string& platformName)
{
    // Default on: a fresh project ships every platform from the global list.
    return LoadBuildBoolSetting(BuildPlatformPrefKey(platformName, "useGlobalScenes"), true);
}

std::string LoadBuildGlobalIcon()
{
    return LoadBuildStringSetting(kGlobalBuildIconPrefKey);
}

std::string LoadBuildPlatformIcon(const std::string& platformName)
{
    return LoadBuildStringSetting(BuildPlatformPrefKey(platformName, "icon"));
}

bool LoadBuildPlatformUseGlobalIcon(const std::string& platformName)
{
    return LoadBuildBoolSetting(BuildPlatformPrefKey(platformName, "useGlobalIcon"), true);
}

} // namespace GameEngine::Editor
