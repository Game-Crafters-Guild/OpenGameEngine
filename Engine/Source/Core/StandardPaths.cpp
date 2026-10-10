#include "Core/StandardPaths.h"

#include "Core/Application.h" // PathUtils

namespace GameEngine
{
static std::string SanitizeName(std::string_view appName)
{
    return PathUtils::SanitizeForFolderName(appName);
}

std::filesystem::path StandardPaths::UserDataRoot(std::string_view appName)
{
    const std::string safe = SanitizeName(appName);
    return (PathUtils::GetUserDataDirectory() / safe).lexically_normal();
}

std::filesystem::path StandardPaths::UserCacheRoot(std::string_view appName)
{
    const std::string safe = SanitizeName(appName);
    return (PathUtils::GetUserCacheDirectory() / safe).lexically_normal();
}

std::filesystem::path StandardPaths::UserLogsRoot(std::string_view appName)
{
    return (UserDataRoot(appName) / "Logs").lexically_normal();
}

std::filesystem::path StandardPaths::GameSavesRoot(std::string_view appName)
{
    const std::string safe = SanitizeName(appName);

#if defined(_WIN32)
    // Conventional Windows location for games.
    return (PathUtils::GetUserDocumentsDirectory() / "My Games" / safe).lexically_normal();
#elif defined(__APPLE__)
    return (UserDataRoot(appName) / "Saves").lexically_normal();
#else
    return (UserDataRoot(appName) / "saves").lexically_normal();
#endif
}

} // namespace GameEngine

