#include "Editor/Vcs/VcsStatusUi.h"

#include "Editor/Settings/SettingsStore.h"

#include <atomic>
#include <mutex>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kSceneDiffIndicatorsPreference =
    "vcs.showHierarchyInspectorDots";

std::atomic_bool g_SceneDiffIndicatorsVisible{true};
std::once_flag g_SceneDiffIndicatorsLoaded;

void LoadSceneDiffIndicatorsPreference()
{
    auto preferences = OpenEditorPreferences();
    std::string error;
    (void)preferences.Load(&error);
    bool visible = true;
    (void)preferences.TryGetBool(kSceneDiffIndicatorsPreference, visible);
    g_SceneDiffIndicatorsVisible.store(visible, std::memory_order_relaxed);
}
} // namespace

const char* VcsStatusToDisplayString(VCSFileStatus status)
{
    switch (status)
    {
    case VCSFileStatus::Unversioned:
        return "Untracked";
    case VCSFileStatus::Modified:
        return "Modified";
    case VCSFileStatus::Added:
        return "Added";
    case VCSFileStatus::Deleted:
        return "Deleted";
    case VCSFileStatus::Conflict:
        return "Conflict";
    case VCSFileStatus::LockedByMe:
        return "Locked";
    case VCSFileStatus::LockedByOthers:
        return "Locked (other)";
    case VCSFileStatus::ServerHasChanges:
        return "Outdated";
    case VCSFileStatus::Ignored:
        return "Ignored";
    case VCSFileStatus::Clean:
        return "Clean";
    case VCSFileStatus::NotConfigured:
        return "configure!";
    default:
        return "-";
    }
}

const char* VcsStatusToColor(VCSFileStatus status)
{
    switch (status)
    {
    case VCSFileStatus::Modified:
        return "#549BFF"; // Blue
    case VCSFileStatus::Added:
        return "#9DFF00"; // Green
    case VCSFileStatus::Conflict:
        return "#FF6254"; // Red
    case VCSFileStatus::Deleted:
        return "#FF9500"; // Orange
    case VCSFileStatus::Unversioned:
        return "#888888"; // Gray
    case VCSFileStatus::LockedByMe:
        return "#FFD700"; // Gold
    case VCSFileStatus::LockedByOthers:
        return "#FF00FF"; // Magenta
    case VCSFileStatus::ServerHasChanges:
        return "#00BFFF"; // Sky blue
    case VCSFileStatus::Ignored:
        return "#666666"; // Dark gray
    case VCSFileStatus::Clean:
        return "#44AA44"; // Green (dimmer)
    case VCSFileStatus::NotConfigured:
        return "#FFAA00"; // Orange/Yellow (warning)
    default:
        return "#888888"; // Gray
    }
}

bool AreVcsSceneDiffIndicatorsVisible()
{
    std::call_once(g_SceneDiffIndicatorsLoaded,
                   LoadSceneDiffIndicatorsPreference);
    return g_SceneDiffIndicatorsVisible.load(std::memory_order_relaxed);
}

bool SetVcsSceneDiffIndicatorsVisible(bool visible, std::string* outError)
{
    std::call_once(g_SceneDiffIndicatorsLoaded,
                   LoadSceneDiffIndicatorsPreference);
    g_SceneDiffIndicatorsVisible.store(visible, std::memory_order_relaxed);

    auto preferences = OpenEditorPreferences();
    std::string loadError;
    (void)preferences.Load(&loadError);
    preferences.SetBool(kSceneDiffIndicatorsPreference, visible);
    return preferences.Save(outError);
}

} // namespace GameEngine::Editor
