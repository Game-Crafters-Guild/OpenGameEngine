#pragma once

#include <filesystem>
#include <string_view>

namespace GameEngine
{
// Standard, OS-correct directories for user data/caches/saves.
//
// These APIs are intentionally *paths-only*. Higher-level systems can build
// preferences/settings persistence on top as needed.
class StandardPaths
{
  public:
    // User-scoped application support data.
    // Windows: %APPDATA%/<AppName>
    // macOS:   ~/Library/Application Support/<AppName>
    // Linux:   $XDG_DATA_HOME/<AppName> (fallback ~/.local/share/<AppName>)
    static std::filesystem::path UserDataRoot(std::string_view appName);

    // User-scoped derived data/caches.
    // Windows: %LOCALAPPDATA%/<AppName>
    // macOS:   ~/Library/Caches/<AppName>
    // Linux:   $XDG_CACHE_HOME/<AppName> (fallback ~/.cache/<AppName>)
    static std::filesystem::path UserCacheRoot(std::string_view appName);

    // User-scoped logs.
    // Default: <UserDataRoot>/Logs
    static std::filesystem::path UserLogsRoot(std::string_view appName);

    // User-facing save-games directory.
    // Windows: <Documents>/My Games/<AppName>
    // macOS:   <UserDataRoot>/Saves
    // Linux:   <UserDataRoot>/saves
    static std::filesystem::path GameSavesRoot(std::string_view appName);
};
} // namespace GameEngine

