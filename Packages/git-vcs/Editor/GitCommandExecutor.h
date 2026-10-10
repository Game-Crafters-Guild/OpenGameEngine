#pragma once

#include "VCSIntegration/VCSCommandExecutor.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

// Helper class for executing git commands
class GitCommandExecutor
{
public:
    using Result = VCSCommandExecutor::Result;

    // Runs git with args as its argument vector, no shell in between.
    static Result Execute(const std::filesystem::path& gitExecutable,
                          const std::filesystem::path& workingDir,
                          const std::vector<std::string>& args,
                          bool captureOutput = true);

    // Find git executable in PATH
    static std::filesystem::path FindGitExecutable();
};

} // namespace GameEngine
