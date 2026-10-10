#pragma once

#include "VCSIntegration/VCSCommandExecutor.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

// Helper class for executing SVN commands
class SVNCommandExecutor
{
public:
    using Result = VCSCommandExecutor::Result;

    // Runs svn with args as its argument vector, no shell in between.
    static Result Execute(const std::filesystem::path& svnExecutable,
                         const std::filesystem::path& workingDir,
                         const std::vector<std::string>& args,
                         bool captureOutput = true);

    // Find SVN executable in PATH
    static std::filesystem::path FindSVNExecutable();
};

} // namespace GameEngine
