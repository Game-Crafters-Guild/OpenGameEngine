#include "DiversionCommandExecutor.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace GameEngine
{

DiversionCommandExecutor::Result DiversionCommandExecutor::Execute(
    const std::filesystem::path& dvExecutable,
    const std::filesystem::path& workingDir,
    const std::vector<std::string>& args,
    bool captureOutput,
    const VCSCommandExecutor::Environment& environment)
{
    if (dvExecutable.empty() || !std::filesystem::exists(dvExecutable))
    {
        Result result;
        result.success = false;
        result.error = "Diversion executable not found";
        return result;
    }

    return VCSCommandExecutor::Execute(dvExecutable, workingDir, args, captureOutput, environment);
}

std::filesystem::path DiversionCommandExecutor::FindDiversionExecutable()
{
#ifdef _WIN32
    // Get user profile directory
    const char* userProfile = std::getenv("USERPROFILE");
    std::string userProfileStr = userProfile ? userProfile : "";

    // Try common locations
    std::vector<std::filesystem::path> paths;
    
    if (!userProfileStr.empty())
    {
        paths.push_back(std::filesystem::path(userProfileStr) / ".diversion" / "bin" / "dv.exe");
    }
    paths.push_back("C:\\Program Files\\Diversion\\dv.exe");
    paths.push_back("C:\\Program Files (x86)\\Diversion\\dv.exe");

    // Also check PATH
    wchar_t dvPath[MAX_PATH];
    DWORD found = SearchPathW(nullptr, L"dv.exe", nullptr, MAX_PATH, dvPath, nullptr);
    if (found > 0 && found < MAX_PATH)
    {
        return std::filesystem::path(dvPath);
    }

    for (const auto& path : paths)
    {
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }
#else
    // Get home directory
    const char* homeDir = std::getenv("HOME");
    std::string homeDirStr = homeDir ? homeDir : "";

    // Check common locations
    std::vector<std::filesystem::path> paths;
    
    if (!homeDirStr.empty())
    {
        paths.push_back(std::filesystem::path(homeDirStr) / ".diversion" / "bin" / "dv");
    }
    paths.push_back("/usr/local/bin/dv");
    paths.push_back("/opt/homebrew/bin/dv");  // macOS Homebrew on Apple Silicon
    paths.push_back("/usr/bin/dv");

    for (const auto& path : paths)
    {
        if (std::filesystem::exists(path))
        {
            return path;
        }
    }

    // Check PATH using which
    FILE* pipe = popen("which dv", "r");
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
