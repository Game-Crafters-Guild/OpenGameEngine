#include "Editor/EditorPaths.h"

#include "Core/Application.h" // PathUtils
#include "Core/Engine.h"
#include "Jobs/WorkspaceId.h"

#include <system_error>
#include <cstdlib>

namespace GameEngine::Editor
{
namespace fs = std::filesystem;

EditorGlobalPaths GetEditorGlobalPaths()
{
    EditorGlobalPaths out{};

    // Preserve existing conventions used by EditorPathPolicy (avoid migration in this pass).
    out.userDataRoot = (GameEngine::PathUtils::GetUserDataDirectory() / "GameEngine" / "Editor").lexically_normal();
    // Historical layout: caches live under <UserCache>/GameEngine rather than
    // .../GameEngine/Editor. An isolated session override must take the cache
    // with it, or two editors still share compiled shaders and derived files.
    out.userCacheRoot = (GameEngine::PathUtils::GetUserCacheDirectory() / "GameEngine").lexically_normal();
    if (const char* root = std::getenv("GE_EDITOR_USER_DATA_ROOT"); root && *root)
    {
        const fs::path overrideRoot(root);
        if (overrideRoot.is_absolute())
        {
            out.userDataRoot = overrideRoot.lexically_normal();
            out.userCacheRoot = (out.userDataRoot / "Cache").lexically_normal();
        }
    }

    out.userLogsRoot = (out.userDataRoot / "Logs").lexically_normal();

    out.installAssetsRoot = GameEngine::PathUtils::GetInstallAssetsRoot();
    out.userAssetsRoot = (out.userDataRoot / "EditorAssets").lexically_normal();
    // Keep a validation/development session's writable shader mirror isolated
    // from other editor builds refreshing the shared user assets on launch.
    if (const char* assets = std::getenv("GE_EDITOR_ASSETS_ROOT"); assets && *assets)
    {
        const fs::path overrideRoot(assets);
        if (overrideRoot.is_absolute())
            out.userAssetsRoot = overrideRoot.lexically_normal();
    }
    out.installTemplatesRoot =
        (out.installAssetsRoot.parent_path() / "ProjectTemplates").lexically_normal();
    out.installToolsRoot = (out.installAssetsRoot.parent_path() / "Tools").lexically_normal();

    out.preferencesFile = (out.userDataRoot / "Preferences.json").lexically_normal();
    out.projectsRoot = (out.userDataRoot / "Projects").lexically_normal();
    out.communityCacheRoot = (out.userDataRoot / "CommunityCache").lexically_normal();
    out.defaultProjectRoot = (out.userDataRoot / "DefaultProject").lexically_normal();

    return out;
}

EditorProjectPaths GetEditorProjectPaths(const fs::path& workspaceRoot, const EditorGlobalPaths& global)
{
    EditorProjectPaths out{};

    std::error_code ec;
    fs::path root = workspaceRoot;
    if (!root.empty() && !root.is_absolute())
    {
        root = fs::absolute(root, ec);
        ec.clear();
    }
    if (!root.empty())
    {
        root = root.lexically_normal();
    }

    out.workspaceRoot = root;
    out.projectRoot = root;

    if (!root.empty())
    {
        out.projectCacheRoot = (root / ".Cache").lexically_normal();
        out.projectEditorRoot = (root / ".Editor").lexically_normal();
        out.thumbnailsRoot = (out.projectEditorRoot / "Thumbnails").lexically_normal();

        out.projectSettingsFile = (out.projectEditorRoot / "ProjectSettings.json").lexically_normal();

        // Per-user per-project settings live outside the project, keyed by workspace id.
        const std::string wid = GameEngine::WorkspaceId::Compute(root.string());
        out.userProjectSettingsFile = (global.projectsRoot / wid / "UserSettings.json").lexically_normal();
    }

    return out;
}

EditorProjectPaths GetCurrentEditorProjectPaths()
{
    const auto& ws = GameEngine::EngineCore::GetInstance().GetWorkspaceRoot();
    const EditorGlobalPaths global = GetEditorGlobalPaths();
    return GetEditorProjectPaths(ws, global);
}

} // namespace GameEngine::Editor
