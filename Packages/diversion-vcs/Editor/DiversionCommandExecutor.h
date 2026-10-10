#pragma once

#include "VCSIntegration/VCSCommandExecutor.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

// Helper class for executing Diversion CLI commands
class DiversionCommandExecutor
{
public:
    using Result = VCSCommandExecutor::Result;

    // Runs dv with args as its argument vector, no shell in between;
    // environment adds variables (the CLI token) to the inherited environment.
    static Result Execute(const std::filesystem::path& dvExecutable,
                         const std::filesystem::path& workingDir,
                         const std::vector<std::string>& args,
                         bool captureOutput = true,
                         const VCSCommandExecutor::Environment& environment = {});

    // Find Diversion executable in PATH and common locations
    static std::filesystem::path FindDiversionExecutable();
};

} // namespace GameEngine
