#pragma once

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

namespace GameEngine
{
namespace NativeScripting
{

// Locate an executable on $PATH WITHOUT spawning a process. Forking (e.g. via
// `command -v`) from a job worker during engine init races the main thread's libc
// state and can deadlock on macOS — a fork in a multithreaded process snapshots held
// locks, so a concurrent getcwd()/current_path() can hang forever. A direct PATH scan
// does the same job, fork-free. Returns empty if not found.
inline std::filesystem::path FindExecutableOnPath(const std::string& name)
{
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv)
        return {};
#if defined(_WIN32)
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif
    std::stringstream ss(pathEnv);
    std::string dir;
    std::error_code ec;
    while (std::getline(ss, dir, kSep))
    {
        if (dir.empty())
            continue;
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec))
            return candidate;
#if defined(_WIN32)
        // A bare tool name ("dotnet", "cmake") is stored extension-less; the
        // on-disk executable carries .exe.
        if (!candidate.has_extension())
        {
            candidate.replace_extension(".exe");
            if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec))
                return candidate;
        }
#endif
    }
    return {};
}

} // namespace NativeScripting
} // namespace GameEngine
