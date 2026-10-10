#include "LoreCommandExecutor.h"
#include "VCSIntegration/VCSCommandExecutor.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace GameEngine
{
namespace
{

// Global flags for an unattended host: no pager on stdout, no per-link commit
// prompts on stdin. Both are clap globals and precede the subcommand.
constexpr const char* kGlobalFlags[] = {"--no-pager", "--non-interactive"};

// The global flags, then extraFlags, then args.
std::vector<std::string> BuildArguments(std::initializer_list<const char*> extraFlags,
                                        const std::vector<std::string>& args)
{
    std::vector<std::string> arguments(std::begin(kGlobalFlags), std::end(kGlobalFlags));
    arguments.insert(arguments.end(), extraFlags.begin(), extraFlags.end());
    arguments.insert(arguments.end(), args.begin(), args.end());
    return arguments;
}

LoreCommandExecutor::Result Run(const std::filesystem::path& loreExecutable,
                                const std::filesystem::path& workingDir,
                                const std::vector<std::string>& arguments,
                                bool captureOutput)
{
    if (loreExecutable.empty() || !std::filesystem::exists(loreExecutable))
    {
        LoreCommandExecutor::Result result;
        result.success = false;
        result.error = "Lore executable not found";
        return result;
    }

    return VCSCommandExecutor::Execute(loreExecutable, workingDir, arguments, captureOutput);
}

std::filesystem::path FirstExisting(const std::vector<std::filesystem::path>& candidates)
{
    std::error_code ec;
    for (const auto& path : candidates)
    {
        if (!path.empty() && std::filesystem::exists(path, ec))
            return path;
    }
    return {};
}

} // namespace

LoreCommandExecutor::Result LoreCommandExecutor::Execute(
    const std::filesystem::path& loreExecutable,
    const std::filesystem::path& workingDir,
    const std::vector<std::string>& args,
    bool captureOutput)
{
    return Run(loreExecutable, workingDir, BuildArguments({}, args), captureOutput);
}

LoreCommandExecutor::Result LoreCommandExecutor::ExecuteJson(
    const std::filesystem::path& loreExecutable,
    const std::filesystem::path& workingDir,
    const std::vector<std::string>& args)
{
    return Run(loreExecutable, workingDir, BuildArguments({"--json"}, args), true);
}

std::filesystem::path LoreCommandExecutor::FindLoreExecutable()
{
    // PATH first: the upstream install scripts put their directory on PATH.
    // The fallbacks are those scripts' defaults (~/.local/bin, %USERPROFILE%\bin)
    // and the documented from-source destinations (/usr/local/bin, ~/bin).
#ifdef _WIN32
    wchar_t lorePath[MAX_PATH];
    const DWORD found = SearchPathW(nullptr, L"lore.exe", nullptr, MAX_PATH, lorePath, nullptr);
    if (found > 0 && found < MAX_PATH)
        return std::filesystem::path(lorePath);

    const char* userProfile = std::getenv("USERPROFILE");
    std::vector<std::filesystem::path> candidates;
    if (userProfile && *userProfile)
        candidates.push_back(std::filesystem::path(userProfile) / "bin" / "lore.exe");
    return FirstExisting(candidates);
#else
    if (FILE* pipe = popen("command -v lore", "r"))
    {
        char buffer[1024];
        std::string path;
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr)
            path = buffer;
        pclose(pipe);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
            path.pop_back();
        std::error_code ec;
        if (!path.empty() && std::filesystem::exists(path, ec))
            return std::filesystem::path(path);
    }

    const char* home = std::getenv("HOME");
    std::vector<std::filesystem::path> candidates;
    if (home && *home)
    {
        candidates.push_back(std::filesystem::path(home) / ".local" / "bin" / "lore");
        candidates.push_back(std::filesystem::path(home) / "bin" / "lore");
    }
    candidates.push_back("/usr/local/bin/lore");
    return FirstExisting(candidates);
#endif
}

} // namespace GameEngine
