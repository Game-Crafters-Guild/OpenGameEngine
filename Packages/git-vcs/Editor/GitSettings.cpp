#include "GitSettings.h"
#include "Editor/EditorPaths.h"
#include "Logger/Logger.h"

namespace GameEngine::Editor
{

GitSettings::GitSettings()
    : m_SettingsStore(OpenEditorPreferences())
{
    // Set defaults
    m_AutoAddOnCreate = false;
    m_AutoRemoveOnDelete = true;
    m_AutoMoveOnRename = true;
    m_StatusRefreshInterval = 10; // Default: 10 seconds
    m_ShowStatusIcons = true;
    m_AutoFetch = true;
    m_FetchInterval = 300;
    m_PromptBeforeCommit = true;
    m_DefaultCommitMessage = "";
}

bool GitSettings::Load()
{
    std::string error;
    if (!m_SettingsStore.Load(&error))
    {
        Logger::Log::Warning("Failed to load Git settings: {}", error);
        return false;
    }

    LoadFromStore();
    return true;
}

bool GitSettings::Save()
{
    SaveToStore();
    
    std::string error;
    if (!m_SettingsStore.Save(&error))
    {
        Logger::Log::Warning("Failed to save Git settings: {}", error);
        return false;
    }
    return true;
}

void GitSettings::SetGitExecutable(const std::filesystem::path& path)
{
    m_GitExecutable = path;
}

void GitSettings::SetAutoAddOnCreate(bool enabled)
{
    m_AutoAddOnCreate = enabled;
}

void GitSettings::SetAutoRemoveOnDelete(bool enabled)
{
    m_AutoRemoveOnDelete = enabled;
}

void GitSettings::SetAutoMoveOnRename(bool enabled)
{
    m_AutoMoveOnRename = enabled;
}

void GitSettings::SetStatusRefreshInterval(int seconds)
{
    m_StatusRefreshInterval = std::max(1, seconds);
}

void GitSettings::SetShowStatusIcons(bool enabled)
{
    m_ShowStatusIcons = enabled;
}

void GitSettings::SetAutoFetch(bool enabled)
{
    m_AutoFetch = enabled;
}

void GitSettings::SetFetchInterval(int seconds)
{
    m_FetchInterval = std::max(60, seconds);
}

void GitSettings::SetPromptBeforeCommit(bool enabled)
{
    m_PromptBeforeCommit = enabled;
}

void GitSettings::SetDefaultCommitMessage(const std::string& message)
{
    m_DefaultCommitMessage = message;
}

void GitSettings::SetExternalDiffTool(const std::filesystem::path& path)
{
    m_ExternalDiffTool = path;
}

void GitSettings::SetColorWholeText(bool enabled)
{
    m_ColorWholeText = enabled;
}

void GitSettings::SetColorDotOnly(bool enabled)
{
    m_ColorDotOnly = enabled;
}

void GitSettings::SetHideDotForClean(bool enabled)
{
    m_HideDotForClean = enabled;
}

void GitSettings::LoadFromStore()
{
    // Load git executable path
    std::string gitExecStr;
    if (m_SettingsStore.TryGetString("git.executable", gitExecStr))
    {
        m_GitExecutable = std::filesystem::path(gitExecStr);
    }

    // Load boolean settings
    m_SettingsStore.TryGetBool("git.autoAddOnCreate", m_AutoAddOnCreate);
    m_SettingsStore.TryGetBool("git.autoRemoveOnDelete", m_AutoRemoveOnDelete);
    m_SettingsStore.TryGetBool("git.autoMoveOnRename", m_AutoMoveOnRename);
    m_SettingsStore.TryGetBool("git.showStatusIcons", m_ShowStatusIcons);
    m_SettingsStore.TryGetBool("git.autoFetch", m_AutoFetch);
    m_SettingsStore.TryGetBool("git.promptBeforeCommit", m_PromptBeforeCommit);

    // Load integer settings
    int64_t refreshInterval = m_StatusRefreshInterval;
    if (m_SettingsStore.TryGetInt64("git.statusRefreshInterval", refreshInterval))
    {
        m_StatusRefreshInterval = static_cast<int>(refreshInterval);
    }

    int64_t fetchInterval = m_FetchInterval;
    if (m_SettingsStore.TryGetInt64("git.fetchInterval", fetchInterval))
    {
        m_FetchInterval = static_cast<int>(fetchInterval);
    }

    // Load string settings
    m_SettingsStore.TryGetString("git.defaultCommitMessage", m_DefaultCommitMessage);
    
    // Load external diff tool path
    std::string diffToolStr;
    if (m_SettingsStore.TryGetString("git.externalDiffTool", diffToolStr))
    {
        m_ExternalDiffTool = std::filesystem::path(diffToolStr);
    }
    
    // Load color mode
    m_SettingsStore.TryGetBool("git.colorWholeText", m_ColorWholeText);
    m_SettingsStore.TryGetBool("git.colorDotOnly", m_ColorDotOnly);
    m_SettingsStore.TryGetBool("git.hideDotForClean", m_HideDotForClean);
}

void GitSettings::SaveToStore()
{
    // Save git executable path
    if (!m_GitExecutable.empty())
    {
        m_SettingsStore.SetString("git.executable", m_GitExecutable.string());
    }
    else
    {
        m_SettingsStore.Remove("git.executable");
    }

    // Save boolean settings
    m_SettingsStore.SetBool("git.autoAddOnCreate", m_AutoAddOnCreate);
    m_SettingsStore.SetBool("git.autoRemoveOnDelete", m_AutoRemoveOnDelete);
    m_SettingsStore.SetBool("git.autoMoveOnRename", m_AutoMoveOnRename);
    m_SettingsStore.SetBool("git.showStatusIcons", m_ShowStatusIcons);
    m_SettingsStore.SetBool("git.autoFetch", m_AutoFetch);
    m_SettingsStore.SetBool("git.promptBeforeCommit", m_PromptBeforeCommit);

    // Save integer settings
    m_SettingsStore.SetInt64("git.statusRefreshInterval", m_StatusRefreshInterval);
    m_SettingsStore.SetInt64("git.fetchInterval", m_FetchInterval);

    // Save string settings
    if (!m_DefaultCommitMessage.empty())
    {
        m_SettingsStore.SetString("git.defaultCommitMessage", m_DefaultCommitMessage);
    }
    else
    {
        m_SettingsStore.Remove("git.defaultCommitMessage");
    }

    // Save external diff tool path
    if (!m_ExternalDiffTool.empty())
    {
        m_SettingsStore.SetString("git.externalDiffTool", m_ExternalDiffTool.string());
    }
    else
    {
        m_SettingsStore.Remove("git.externalDiffTool");
    }
    
    // Save color mode
    m_SettingsStore.SetBool("git.colorWholeText", m_ColorWholeText);
    m_SettingsStore.SetBool("git.colorDotOnly", m_ColorDotOnly);
    m_SettingsStore.SetBool("git.hideDotForClean", m_HideDotForClean);
}

} // namespace GameEngine::Editor
