#include "SVNSettings.h"
#include "Editor/EditorPaths.h"
#include "Logger/Logger.h"

namespace GameEngine::Editor
{

SVNSettings::SVNSettings()
    : m_SettingsStore(OpenEditorPreferences())
{
    // Set defaults
    m_AutoAddOnCreate = false;
    m_AutoRemoveOnDelete = true;
    m_AutoMoveOnRename = true;
    m_StatusRefreshInterval = 10; // Default: 10 seconds
    m_ShowStatusIcons = true;
    m_PromptBeforeCommit = true;
    m_DefaultCommitMessage = "";
}

bool SVNSettings::Load()
{
    std::string error;
    if (!m_SettingsStore.Load(&error))
    {
        Logger::Log::Warning("Failed to load SVN settings: {}", error);
        return false;
    }

    LoadFromStore();
    return true;
}

bool SVNSettings::Save()
{
    SaveToStore();
    
    std::string error;
    if (!m_SettingsStore.Save(&error))
    {
        Logger::Log::Warning("Failed to save SVN settings: {}", error);
        return false;
    }
    return true;
}

void SVNSettings::SetSVNExecutable(const std::filesystem::path& path)
{
    m_SVNExecutable = path;
}

void SVNSettings::SetAutoAddOnCreate(bool enabled)
{
    m_AutoAddOnCreate = enabled;
}

void SVNSettings::SetAutoRemoveOnDelete(bool enabled)
{
    m_AutoRemoveOnDelete = enabled;
}

void SVNSettings::SetAutoMoveOnRename(bool enabled)
{
    m_AutoMoveOnRename = enabled;
}

void SVNSettings::SetStatusRefreshInterval(int seconds)
{
    m_StatusRefreshInterval = std::max(1, seconds);
}

void SVNSettings::SetShowStatusIcons(bool enabled)
{
    m_ShowStatusIcons = enabled;
}

void SVNSettings::SetPromptBeforeCommit(bool enabled)
{
    m_PromptBeforeCommit = enabled;
}

void SVNSettings::SetDefaultCommitMessage(const std::string& message)
{
    m_DefaultCommitMessage = message;
}

void SVNSettings::SetColorWholeText(bool enabled)
{
    m_ColorWholeText = enabled;
}

void SVNSettings::SetColorDotOnly(bool enabled)
{
    m_ColorDotOnly = enabled;
}

void SVNSettings::SetHideDotForClean(bool enabled)
{
    m_HideDotForClean = enabled;
}

void SVNSettings::SetExternalDiffTool(const std::filesystem::path& path)
{
    m_ExternalDiffTool = path;
}

void SVNSettings::LoadFromStore()
{
    // Load SVN executable path
    std::string svnExecStr;
    if (m_SettingsStore.TryGetString("svn.executable", svnExecStr))
    {
        m_SVNExecutable = std::filesystem::path(svnExecStr);
    }

    // Load boolean settings
    m_SettingsStore.TryGetBool("svn.autoAddOnCreate", m_AutoAddOnCreate);
    m_SettingsStore.TryGetBool("svn.autoRemoveOnDelete", m_AutoRemoveOnDelete);
    m_SettingsStore.TryGetBool("svn.autoMoveOnRename", m_AutoMoveOnRename);
    m_SettingsStore.TryGetBool("svn.showStatusIcons", m_ShowStatusIcons);
    m_SettingsStore.TryGetBool("svn.promptBeforeCommit", m_PromptBeforeCommit);

    // Load integer settings
    int64_t refreshInterval = m_StatusRefreshInterval;
    if (m_SettingsStore.TryGetInt64("svn.statusRefreshInterval", refreshInterval))
    {
        m_StatusRefreshInterval = static_cast<int>(refreshInterval);
    }

    // Load string settings
    m_SettingsStore.TryGetString("svn.defaultCommitMessage", m_DefaultCommitMessage);
    
    // Load color mode
    m_SettingsStore.TryGetBool("svn.colorWholeText", m_ColorWholeText);
    m_SettingsStore.TryGetBool("svn.colorDotOnly", m_ColorDotOnly);
    m_SettingsStore.TryGetBool("svn.hideDotForClean", m_HideDotForClean);
    
    // Load external diff tool path
    std::string diffToolStr;
    if (m_SettingsStore.TryGetString("svn.externalDiffTool", diffToolStr))
    {
        m_ExternalDiffTool = std::filesystem::path(diffToolStr);
    }
}

void SVNSettings::SaveToStore()
{
    // Save SVN executable path
    if (!m_SVNExecutable.empty())
    {
        m_SettingsStore.SetString("svn.executable", m_SVNExecutable.string());
    }
    else
    {
        m_SettingsStore.Remove("svn.executable");
    }

    // Save boolean settings
    m_SettingsStore.SetBool("svn.autoAddOnCreate", m_AutoAddOnCreate);
    m_SettingsStore.SetBool("svn.autoRemoveOnDelete", m_AutoRemoveOnDelete);
    m_SettingsStore.SetBool("svn.autoMoveOnRename", m_AutoMoveOnRename);
    m_SettingsStore.SetBool("svn.showStatusIcons", m_ShowStatusIcons);
    m_SettingsStore.SetBool("svn.promptBeforeCommit", m_PromptBeforeCommit);

    // Save integer settings
    m_SettingsStore.SetInt64("svn.statusRefreshInterval", m_StatusRefreshInterval);

    // Save string settings
    if (!m_DefaultCommitMessage.empty())
    {
        m_SettingsStore.SetString("svn.defaultCommitMessage", m_DefaultCommitMessage);
    }
    else
    {
        m_SettingsStore.Remove("svn.defaultCommitMessage");
    }
    
    // Save color mode
    m_SettingsStore.SetBool("svn.colorWholeText", m_ColorWholeText);
    m_SettingsStore.SetBool("svn.colorDotOnly", m_ColorDotOnly);
    m_SettingsStore.SetBool("svn.hideDotForClean", m_HideDotForClean);
    
    // Save external diff tool path
    if (!m_ExternalDiffTool.empty())
    {
        m_SettingsStore.SetString("svn.externalDiffTool", m_ExternalDiffTool.string());
    }
    else
    {
        m_SettingsStore.Remove("svn.externalDiffTool");
    }
}

} // namespace GameEngine::Editor
