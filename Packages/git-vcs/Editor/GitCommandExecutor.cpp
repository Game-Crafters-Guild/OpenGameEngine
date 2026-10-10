#include "GitCommandExecutor.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace GameEngine
{

GitCommandExecutor::Result GitCommandExecutor::Execute(
    const std::filesystem::path& gitExecutable,
    const std::filesystem::path& workingDir,
    const std::vector<std::string>& args,
    bool captureOutput)
{
    if (gitExecutable.empty() || !std::filesystem::exists(gitExecutable))
    {
        Result result;
        result.success = false;
        result.error = "Git executable not found";
        return result;
    }

    return VCSCommandExecutor::Execute(gitExecutable, workingDir, args, captureOutput);
}

std::filesystem::path GitCommandExecutor::FindGitExecutable()
{
#ifdef _WIN32
    // Try common locations
    std::vector<std::filesystem::path> paths = {
        "C:\\Program Files\\Git\\cmd\\git.exe",
        "C:\\Program Files (x86)\\Git\\cmd\\git.exe",
    };

    // Also check PATH
    wchar_t gitPath[MAX_PATH];
    DWORD found = SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, gitPath, nullptr);
    if (found > 0 && found < MAX_PATH)
    {
        return std::filesystem::path(gitPath);
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
        "/usr/bin/git",
        "/usr/local/bin/git",
        "/opt/homebrew/bin/git", // macOS Homebrew on Apple Silicon
    };

    for (const auto& path : paths)
    {
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }

    // Check PATH using which
    FILE* pipe = popen("which git", "r");
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
