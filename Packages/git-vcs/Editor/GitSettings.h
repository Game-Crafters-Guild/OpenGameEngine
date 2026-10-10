#pragma once

#include "Editor/Settings/SettingsStore.h"
#include <filesystem>
#include <string>
#include <chrono>

namespace GameEngine::Editor
{

// Git integration settings stored in editor preferences
class GitSettings
{
public:
    GitSettings();
    ~GitSettings() = default;

    // Load settings from preferences
    bool Load();
    
    // Save settings to preferences
    bool Save();

    // Git executable path (empty = auto-detect)
    std::filesystem::path GetGitExecutable() const { return m_GitExecutable; }
    void SetGitExecutable(const std::filesystem::path& path);

    // Auto-add files when created
    bool GetAutoAddOnCreate() const { return m_AutoAddOnCreate; }
    void SetAutoAddOnCreate(bool enabled);

    // Auto-remove files when deleted
    bool GetAutoRemoveOnDelete() const { return m_AutoRemoveOnDelete; }
    void SetAutoRemoveOnDelete(bool enabled);

    // Auto-move files when renamed
    bool GetAutoMoveOnRename() const { return m_AutoMoveOnRename; }
    void SetAutoMoveOnRename(bool enabled);

    // Status refresh interval in seconds
    int GetStatusRefreshInterval() const { return m_StatusRefreshInterval; }
    void SetStatusRefreshInterval(int seconds);

    // Show status icons in asset browser
    bool GetShowStatusIcons() const { return m_ShowStatusIcons; }
    void SetShowStatusIcons(bool enabled);

    // Auto-fetch remote changes
    bool GetAutoFetch() const { return m_AutoFetch; }
    void SetAutoFetch(bool enabled);

    // Fetch interval in seconds
    int GetFetchInterval() const { return m_FetchInterval; }
    void SetFetchInterval(int seconds);

    // Prompt before committing
    bool GetPromptBeforeCommit() const { return m_PromptBeforeCommit; }
    void SetPromptBeforeCommit(bool enabled);

    // Default commit message template
    std::string GetDefaultCommitMessage() const { return m_DefaultCommitMessage; }
    void SetDefaultCommitMessage(const std::string& message);

    // External diff tool path (empty = use git's configured difftool)
    std::filesystem::path GetExternalDiffTool() const { return m_ExternalDiffTool; }
    void SetExternalDiffTool(const std::filesystem::path& path);

    // Color mode: true = color whole text, false = color only dot
    bool GetColorWholeText() const { return m_ColorWholeText; }
    void SetColorWholeText(bool enabled);

    // Color dot only: true = only dot is colored, status text uses default color
    bool GetColorDotOnly() const { return m_ColorDotOnly; }
    void SetColorDotOnly(bool enabled);

    // Hide dot for clean status files
    bool GetHideDotForClean() const { return m_HideDotForClean; }
    void SetHideDotForClean(bool enabled);

private:
    SettingsStore m_SettingsStore;

    // Settings values
    std::filesystem::path m_GitExecutable;
    bool m_AutoAddOnCreate = false;
    bool m_AutoRemoveOnDelete = true;
    bool m_AutoMoveOnRename = true;
    int m_StatusRefreshInterval = 10; // seconds (default: 10)
    bool m_ShowStatusIcons = true;
    bool m_AutoFetch = true;
    int m_FetchInterval = 300; // 5 minutes
    bool m_PromptBeforeCommit = true;
    std::string m_DefaultCommitMessage;
    std::filesystem::path m_ExternalDiffTool;
    bool m_ColorWholeText = false; // false = color only dot, true = color whole text
    bool m_ColorDotOnly = true;    // true = only dot colored, text uses default color
    bool m_HideDotForClean = true; // true = don't show dot for clean status files

    void LoadFromStore();
    void SaveToStore();
};

} // namespace GameEngine::Editor
