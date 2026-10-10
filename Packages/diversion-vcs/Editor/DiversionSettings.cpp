#include "DiversionSettings.h"
#include "Editor/EditorPaths.h"
#include "TokenEncryption.h"
#include "Logger/Logger.h"

namespace GameEngine::Editor
{

DiversionSettings::DiversionSettings()
    : m_SettingsStore(OpenEditorPreferences())
{
    // Set defaults
    m_StatusRefreshInterval = 10;
    m_ShowStatusIcons = true;
    m_PromptBeforeCommit = true;
    m_DefaultCommitMessage = "";
    m_ColorWholeText = false;
    m_ColorDotOnly = true;
    m_HideDotForClean = true;
    m_AutoRefreshEnabled = false;
}

bool DiversionSettings::Load()
{
    std::string error;
    if (!m_SettingsStore.Load(&error))
    {
        Logger::Log::Warning("Failed to load Diversion settings: {}", error);
        return false;
    }

    LoadFromStore();
    return true;
}

bool DiversionSettings::Save()
{
    SaveToStore();
    
    std::string error;
    if (!m_SettingsStore.Save(&error))
    {
        Logger::Log::Warning("Failed to save Diversion settings: {}", error);
        return false;
    }
    return true;
}

void DiversionSettings::SetDiversionExecutable(const std::filesystem::path& path)
{
    m_DiversionExecutable = path;
}

void DiversionSettings::SetRepoId(const std::string& repoId)
{
    m_RepoId = repoId;
}

void DiversionSettings::SetWorkspaceId(const std::string& workspaceId)
{
    m_WorkspaceId = workspaceId;
}

void DiversionSettings::SetRefreshToken(const std::string& token)
{
    m_RefreshToken = token;
}

void DiversionSettings::SetAccessToken(const std::string& token)
{
    m_AccessToken = token;
}

void DiversionSettings::SetStatusRefreshInterval(int seconds)
{
    m_StatusRefreshInterval = std::max(1, seconds);
}

void DiversionSettings::SetShowStatusIcons(bool enabled)
{
    m_ShowStatusIcons = enabled;
}

void DiversionSettings::SetPromptBeforeCommit(bool enabled)
{
    m_PromptBeforeCommit = enabled;
}

void DiversionSettings::SetDefaultCommitMessage(const std::string& message)
{
    m_DefaultCommitMessage = message;
}

void DiversionSettings::SetColorWholeText(bool enabled)
{
    m_ColorWholeText = enabled;
}

void DiversionSettings::SetColorDotOnly(bool enabled)
{
    m_ColorDotOnly = enabled;
}

void DiversionSettings::SetHideDotForClean(bool enabled)
{
    m_HideDotForClean = enabled;
}

void DiversionSettings::SetAutoRefreshEnabled(bool enabled)
{
    m_AutoRefreshEnabled = enabled;
}

void DiversionSettings::SetExternalDiffTool(const std::filesystem::path& path)
{
    m_ExternalDiffTool = path;
}

void DiversionSettings::LoadFromStore()
{
    // Load Diversion executable path
    std::string dvExecStr;
    if (m_SettingsStore.TryGetString("diversion.executable", dvExecStr))
    {
        m_DiversionExecutable = std::filesystem::path(dvExecStr);
    }

    // Load Diversion-specific settings
    m_SettingsStore.TryGetString("diversion.repoId", m_RepoId);
    m_SettingsStore.TryGetString("diversion.workspaceId", m_WorkspaceId);
    
    // Load and decrypt tokens
    std::string encryptedRefreshToken;
    if (m_SettingsStore.TryGetString("diversion.refreshToken", encryptedRefreshToken))
    {
        if (!encryptedRefreshToken.empty())
        {
            m_RefreshToken = TokenEncryption::Decrypt(encryptedRefreshToken);
            if (m_RefreshToken.empty() && !encryptedRefreshToken.empty() && encryptedRefreshToken.substr(0, 4) == "ENC:")
            {
                Logger::Log::Warning("Diversion: Failed to decrypt refresh token (encrypted length: {})", encryptedRefreshToken.length());
            }
        }
    }
    
    std::string encryptedAccessToken;
    if (m_SettingsStore.TryGetString("diversion.accessToken", encryptedAccessToken))
    {
        if (!encryptedAccessToken.empty())
        {
            m_AccessToken = TokenEncryption::Decrypt(encryptedAccessToken);
        }
    }

    // Load boolean settings
    m_SettingsStore.TryGetBool("diversion.showStatusIcons", m_ShowStatusIcons);
    m_SettingsStore.TryGetBool("diversion.promptBeforeCommit", m_PromptBeforeCommit);
    m_SettingsStore.TryGetBool("diversion.autoRefreshEnabled", m_AutoRefreshEnabled);

    // Load integer settings
    int64_t refreshInterval = m_StatusRefreshInterval;
    if (m_SettingsStore.TryGetInt64("diversion.statusRefreshInterval", refreshInterval))
    {
        m_StatusRefreshInterval = static_cast<int>(refreshInterval);
    }

    // Load string settings
    m_SettingsStore.TryGetString("diversion.defaultCommitMessage", m_DefaultCommitMessage);
    
    // Load color mode
    m_SettingsStore.TryGetBool("diversion.colorWholeText", m_ColorWholeText);
    m_SettingsStore.TryGetBool("diversion.colorDotOnly", m_ColorDotOnly);
    m_SettingsStore.TryGetBool("diversion.hideDotForClean", m_HideDotForClean);
    
    // Load external diff tool path
    std::string diffToolStr;
    if (m_SettingsStore.TryGetString("diversion.externalDiffTool", diffToolStr))
    {
        m_ExternalDiffTool = std::filesystem::path(diffToolStr);
    }
}

void DiversionSettings::SaveToStore()
{
    // Save Diversion executable path
    if (!m_DiversionExecutable.empty())
    {
        m_SettingsStore.SetString("diversion.executable", m_DiversionExecutable.string());
    }
    else
    {
        m_SettingsStore.Remove("diversion.executable");
    }

    // Save Diversion-specific settings
    if (!m_RepoId.empty())
    {
        m_SettingsStore.SetString("diversion.repoId", m_RepoId);
    }
    else
    {
        m_SettingsStore.Remove("diversion.repoId");
    }

    if (!m_WorkspaceId.empty())
    {
        m_SettingsStore.SetString("diversion.workspaceId", m_WorkspaceId);
    }
    else
    {
        m_SettingsStore.Remove("diversion.workspaceId");
    }

    // Encrypt and save tokens
    if (!m_RefreshToken.empty())
    {
        std::string encrypted = TokenEncryption::Encrypt(m_RefreshToken);
        m_SettingsStore.SetString("diversion.refreshToken", encrypted);
    }
    else
    {
        m_SettingsStore.Remove("diversion.refreshToken");
    }

    if (!m_AccessToken.empty())
    {
        std::string encrypted = TokenEncryption::Encrypt(m_AccessToken);
        m_SettingsStore.SetString("diversion.accessToken", encrypted);
    }
    else
    {
        m_SettingsStore.Remove("diversion.accessToken");
    }

    // Save boolean settings
    m_SettingsStore.SetBool("diversion.showStatusIcons", m_ShowStatusIcons);
    m_SettingsStore.SetBool("diversion.promptBeforeCommit", m_PromptBeforeCommit);
    m_SettingsStore.SetBool("diversion.autoRefreshEnabled", m_AutoRefreshEnabled);

    // Save integer settings
    m_SettingsStore.SetInt64("diversion.statusRefreshInterval", m_StatusRefreshInterval);

    // Save string settings
    if (!m_DefaultCommitMessage.empty())
    {
        m_SettingsStore.SetString("diversion.defaultCommitMessage", m_DefaultCommitMessage);
    }
    else
    {
        m_SettingsStore.Remove("diversion.defaultCommitMessage");
    }
    
    // Save color mode
    m_SettingsStore.SetBool("diversion.colorWholeText", m_ColorWholeText);
    m_SettingsStore.SetBool("diversion.colorDotOnly", m_ColorDotOnly);
    m_SettingsStore.SetBool("diversion.hideDotForClean", m_HideDotForClean);
    
    // Save external diff tool path
    if (!m_ExternalDiffTool.empty())
    {
        m_SettingsStore.SetString("diversion.externalDiffTool", m_ExternalDiffTool.string());
    }
    else
    {
        m_SettingsStore.Remove("diversion.externalDiffTool");
    }
}

} // namespace GameEngine::Editor
