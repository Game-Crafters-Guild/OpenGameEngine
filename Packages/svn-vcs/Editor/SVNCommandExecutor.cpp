#include "SVNCommandExecutor.h"
#include "VCSIntegration/VCSCommandExecutor.h"
#include "Logger/Logger.h"

#include <filesystem>
#include <sstream>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#else
#include <unistd.h>
#endif

namespace GameEngine
{
SVNCommandExecutor::Result SVNCommandExecutor::Execute(
    const std::filesystem::path& svnExecutable,
    const std::filesystem::path& workingDir,
    const std::vector<std::string>& args,
    bool captureOutput)
{
    if (svnExecutable.empty() || !std::filesystem::exists(svnExecutable))
    {
        Result result;
        result.success = false;
        result.error = "SVN executable not found";
        return result;
    }

    return VCSCommandExecutor::Execute(svnExecutable, workingDir, args, captureOutput);
}

std::filesystem::path SVNCommandExecutor::FindSVNExecutable()
{
#ifdef _WIN32
    // Try common locations
    std::vector<std::filesystem::path> paths = {
        "C:\\Program Files\\TortoiseSVN\\bin\\svn.exe",
        "C:\\Program Files (x86)\\TortoiseSVN\\bin\\svn.exe",
        "C:\\Program Files\\CollabNet\\Subversion Client\\svn.exe",
    };

    // Also check PATH
    wchar_t svnPath[MAX_PATH];
    DWORD found = SearchPathW(nullptr, L"svn.exe", nullptr, MAX_PATH, svnPath, nullptr);
    if (found > 0 && found < MAX_PATH)
    {
        return std::filesystem::path(svnPath);
    }

    for (const auto& path : paths)
    {
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }
#else
    // Check common locations
    std::vector<std::filesystem::path> paths = {
        "/usr/bin/svn",
        "/usr/local/bin/svn",
        "/opt/homebrew/bin/svn", // macOS Homebrew on Apple Silicon
        "/opt/local/bin/svn",    // macOS MacPorts
    };

    for (const auto& path : paths)
    {
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }

    // Check PATH using which
    FILE* pipe = popen("which svn", "r");
    if (pipe)
    {
        char buffer[256];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
            std::string path(buffer);
            // Remove trailing newline
            if (!path.empty() && path.back() == '\n')
            {
                path.pop_back();
            }
            if (!path.empty() && std::filesystem::exists(path))
            {
                pclose(pipe);
                return std::filesystem::path(path);
            }
        }
        pclose(pipe);
    }
#endif

    return std::filesystem::path();
}

} // namespace GameEngine
