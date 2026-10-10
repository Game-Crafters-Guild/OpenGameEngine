#pragma once

#include "Editor/Settings/SettingsStore.h"
#include <filesystem>
#include <string>

namespace GameEngine::Editor
{

// SVN integration settings stored in editor preferences
class SVNSettings
{
public:
    SVNSettings();
    ~SVNSettings() = default;

    // Load settings from preferences
    bool Load();
    
    // Save settings to preferences
    bool Save();

    // SVN executable path (empty = auto-detect)
    std::filesystem::path GetSVNExecutable() const { return m_SVNExecutable; }
    void SetSVNExecutable(const std::filesystem::path& path);

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

    // Prompt before committing
    bool GetPromptBeforeCommit() const { return m_PromptBeforeCommit; }
    void SetPromptBeforeCommit(bool enabled);

    // Default commit message template
    std::string GetDefaultCommitMessage() const { return m_DefaultCommitMessage; }
    void SetDefaultCommitMessage(const std::string& message);

    // Color mode: true = color whole text, false = color only dot
    bool GetColorWholeText() const { return m_ColorWholeText; }
    void SetColorWholeText(bool enabled);

    // Color dot only: true = only dot is colored, status text uses default color
    bool GetColorDotOnly() const { return m_ColorDotOnly; }
    void SetColorDotOnly(bool enabled);

    // Hide dot for clean status files
    bool GetHideDotForClean() const { return m_HideDotForClean; }
    void SetHideDotForClean(bool enabled);

    // External diff tool path (empty = use svn diff)
    std::filesystem::path GetExternalDiffTool() const { return m_ExternalDiffTool; }
    void SetExternalDiffTool(const std::filesystem::path& path);

private:
    SettingsStore m_SettingsStore;

    // Settings values
    std::filesystem::path m_SVNExecutable;
    bool m_AutoAddOnCreate = false;
    bool m_AutoRemoveOnDelete = true;
    bool m_AutoMoveOnRename = true;
    int m_StatusRefreshInterval = 10; // seconds
    bool m_ShowStatusIcons = true;
    bool m_PromptBeforeCommit = true;
    std::string m_DefaultCommitMessage;
    bool m_ColorWholeText = false; // false = color only dot, true = color whole text
    bool m_ColorDotOnly = true;    // true = only dot colored, text uses default color
    bool m_HideDotForClean = true; // true = don't show dot for clean status files
    std::filesystem::path m_ExternalDiffTool;

    void LoadFromStore();
    void SaveToStore();
};

} // namespace GameEngine::Editor
