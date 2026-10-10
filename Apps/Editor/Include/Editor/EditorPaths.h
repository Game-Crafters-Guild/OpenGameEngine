#pragma once

#include <filesystem>
#include <string_view>

namespace GameEngine
{
class EngineCore;
}

namespace GameEngine::Editor
{
struct EditorGlobalPaths
{
    // User-writable roots.
    std::filesystem::path userDataRoot;  // <UserData>/GameEngine/Editor
    std::filesystem::path userCacheRoot; // <UserCache>/GameEngine, or <userDataRoot>/Cache when isolated
    std::filesystem::path userLogsRoot;  // <userDataRoot>/Logs

    // Editor assets: install-provided vs user-writable (seeded).
    std::filesystem::path installAssetsRoot; // PathUtils::GetInstallAssetsRoot(): Editor.app Resources/Assets or <exe>/Assets
    std::filesystem::path userAssetsRoot;    // <userDataRoot>/EditorAssets

    // Project templates staged with the build. Sibling of installAssetsRoot so
    // template scene payloads never enter the editor asset mount.
    std::filesystem::path installTemplatesRoot; // Resources/ProjectTemplates (mac) or <exe>/ProjectTemplates

    // Scripts staged with the build for the editor to run, one folder per
    // user (Tools/SteamDeck). Sibling of installAssetsRoot.
    std::filesystem::path installToolsRoot; // Resources/Tools (mac) or <exe>/Tools

    // Common files/roots.
    std::filesystem::path preferencesFile; // <userDataRoot>/Preferences.json
    std::filesystem::path projectsRoot;    // <userDataRoot>/Projects

    // Cache for the fetched community-projects catalog (manifest, thumbnails,
    // transient clone staging).
    std::filesystem::path communityCacheRoot; // <userDataRoot>/CommunityCache

    // Default/fallback project root (used when no project selected yet).
    std::filesystem::path defaultProjectRoot; // <userDataRoot>/DefaultProject
};

struct EditorProjectPaths
{
    // Resolved workspace/project root for this Editor session.
    std::filesystem::path workspaceRoot; // absolute, normalized
    std::filesystem::path projectRoot;   // alias of workspaceRoot (for clarity)

    // Project-owned roots.
    std::filesystem::path projectCacheRoot;  // <projectRoot>/.Cache
    std::filesystem::path projectEditorRoot; // <projectRoot>/.Editor
    std::filesystem::path thumbnailsRoot;    // <projectRoot>/.Editor/Thumbnails

    // Settings files.
    std::filesystem::path projectSettingsFile;     // <projectRoot>/.Editor/ProjectSettings.json
    std::filesystem::path userProjectSettingsFile; // <EditorUserDataRoot>/Projects/<WorkspaceId>/UserSettings.json
};

// Compute Editor-global user/install paths. These are not project-specific.
EditorGlobalPaths GetEditorGlobalPaths();

// Compute project-specific paths from an explicit workspace root.
EditorProjectPaths GetEditorProjectPaths(const std::filesystem::path& workspaceRoot,
                                        const EditorGlobalPaths& global = GetEditorGlobalPaths());

// Convenience: compute project paths for the current Engine workspace.
EditorProjectPaths GetCurrentEditorProjectPaths();

} // namespace GameEngine::Editor

