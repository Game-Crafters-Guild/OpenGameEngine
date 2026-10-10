#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

// Base class for executing VCS commands with common execution patterns
class VCSCommandExecutor
{
public:
    struct Result
    {
        bool success = false;
        std::string output;
        std::string error;
        int exitCode = -1;
    };

    // Variables set in the child's environment on top of the editor's own, as
    // (name, value) pairs.
    using Environment = std::vector<std::pair<std::string, std::string>>;

    // Execute a VCS command with no shell in between: each entry of args
    // reaches the child as exactly one argv entry, so spaces, quotes and shell
    // metacharacters in paths or commit messages need no escaping.
    // executable: path to the VCS executable (git, svn, etc.)
    // workingDir: working directory for the command
    // args: command arguments (without the executable name)
    // captureOutput: whether to capture stdout/stderr
    // environment: variables added to or replaced in the inherited environment
    static Result Execute(const std::filesystem::path& executable,
                         const std::filesystem::path& workingDir,
                         const std::vector<std::string>& args,
                         bool captureOutput = true,
                         const Environment& environment = {});
};

} // namespace GameEngine
