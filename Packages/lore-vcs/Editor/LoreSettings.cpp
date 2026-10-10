#include "LoreSettings.h"
#include "Editor/EditorPaths.h"
#include "Logger/Logger.h"

#include <algorithm>

namespace GameEngine::Editor
{

LoreSettings::LoreSettings()
    : m_SettingsStore(OpenEditorPreferences())
{
    // Set defaults
    m_StatusRefreshInterval = 10; // Default: 10 seconds
    m_ShowStatusIcons = true;
    m_PromptBeforeCommit = true;
    m_DefaultCommitMessage = "";
}

bool LoreSettings::Load()
{
    std::string error;
    if (!m_SettingsStore.Load(&error))
    {
        Logger::Log::Warning("Failed to load Lore settings: {}", error);
        return false;
    }

    LoadFromStore();
    return true;
}

bool LoreSettings::Save()
{
    SaveToStore();

    std::string error;
    if (!m_SettingsStore.Save(&error))
    {
        Logger::Log::Warning("Failed to save Lore settings: {}", error);
        return false;
    }
    return true;
}

void LoreSettings::SetLoreExecutable(const std::filesystem::path& path)
{
    m_LoreExecutable = path;
}

void LoreSettings::SetStatusRefreshInterval(int seconds)
{
    m_StatusRefreshInterval = std::max(1, seconds);
}

void LoreSettings::SetShowStatusIcons(bool enabled)
{
    m_ShowStatusIcons = enabled;
}

void LoreSettings::SetPromptBeforeCommit(bool enabled)
{
    m_PromptBeforeCommit = enabled;
}

void LoreSettings::SetDefaultCommitMessage(const std::string& message)
{
    m_DefaultCommitMessage = message;
}

void LoreSettings::SetColorWholeText(bool enabled)
{
    m_ColorWholeText = enabled;
}

void LoreSettings::SetColorDotOnly(bool enabled)
{
    m_ColorDotOnly = enabled;
}

void LoreSettings::SetHideDotForClean(bool enabled)
{
    m_HideDotForClean = enabled;
}

void LoreSettings::SetExternalDiffTool(const std::filesystem::path& path)
{
    m_ExternalDiffTool = path;
}

void LoreSettings::LoadFromStore()
{
    // Load Lore executable path
    std::string loreExecStr;
    if (m_SettingsStore.TryGetString("lore.executable", loreExecStr))
    {
        m_LoreExecutable = std::filesystem::path(loreExecStr);
    }

    // Load boolean settings
    m_SettingsStore.TryGetBool("lore.showStatusIcons", m_ShowStatusIcons);
    m_SettingsStore.TryGetBool("lore.promptBeforeCommit", m_PromptBeforeCommit);

    // Load integer settings
    int64_t refreshInterval = m_StatusRefreshInterval;
    if (m_SettingsStore.TryGetInt64("lore.statusRefreshInterval", refreshInterval))
    {
        m_StatusRefreshInterval = static_cast<int>(refreshInterval);
    }

    // Load string settings
    m_SettingsStore.TryGetString("lore.defaultCommitMessage", m_DefaultCommitMessage);

    // Load color mode
    m_SettingsStore.TryGetBool("lore.colorWholeText", m_ColorWholeText);
    m_SettingsStore.TryGetBool("lore.colorDotOnly", m_ColorDotOnly);
    m_SettingsStore.TryGetBool("lore.hideDotForClean", m_HideDotForClean);

    // Load external diff tool path
    std::string diffToolStr;
    if (m_SettingsStore.TryGetString("lore.externalDiffTool", diffToolStr))
    {
        m_ExternalDiffTool = std::filesystem::path(diffToolStr);
    }
}

void LoreSettings::SaveToStore()
{
    // Save Lore executable path
    if (!m_LoreExecutable.empty())
    {
        m_SettingsStore.SetString("lore.executable", m_LoreExecutable.string());
    }
    else
    {
        m_SettingsStore.Remove("lore.executable");
    }

    // Save boolean settings
    m_SettingsStore.SetBool("lore.showStatusIcons", m_ShowStatusIcons);
    m_SettingsStore.SetBool("lore.promptBeforeCommit", m_PromptBeforeCommit);

    // Save integer settings
    m_SettingsStore.SetInt64("lore.statusRefreshInterval", m_StatusRefreshInterval);

    // Save string settings
    if (!m_DefaultCommitMessage.empty())
    {
        m_SettingsStore.SetString("lore.defaultCommitMessage", m_DefaultCommitMessage);
    }
    else
    {
        m_SettingsStore.Remove("lore.defaultCommitMessage");
    }

    // Save color mode
    m_SettingsStore.SetBool("lore.colorWholeText", m_ColorWholeText);
    m_SettingsStore.SetBool("lore.colorDotOnly", m_ColorDotOnly);
    m_SettingsStore.SetBool("lore.hideDotForClean", m_HideDotForClean);

    // Save external diff tool path
    if (!m_ExternalDiffTool.empty())
    {
        m_SettingsStore.SetString("lore.externalDiffTool", m_ExternalDiffTool.string());
    }
    else
    {
        m_SettingsStore.Remove("lore.externalDiffTool");
    }
}

} // namespace GameEngine::Editor
