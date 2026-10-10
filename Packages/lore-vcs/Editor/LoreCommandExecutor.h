#pragma once

#include "VCSIntegration/VCSCommandExecutor.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

// Runs the `lore` CLI with an argument vector, no shell in between. Every
// invocation carries the global flags an unattended GUI host needs
// (`--no-pager`, `--non-interactive`); ExecuteJson adds `--json` so the caller
// reads the CLI's event stream instead of its human text.
class LoreCommandExecutor
{
public:
    using Result = VCSCommandExecutor::Result;

    static Result Execute(const std::filesystem::path& loreExecutable,
                          const std::filesystem::path& workingDir,
                          const std::vector<std::string>& args,
                          bool captureOutput = true);

    // Machine-readable run: stdout holds one JSON event object per line.
    static Result ExecuteJson(const std::filesystem::path& loreExecutable,
                              const std::filesystem::path& workingDir,
                              const std::vector<std::string>& args);

    // Find `lore` on PATH, then in the install script's default directories.
    static std::filesystem::path FindLoreExecutable();
};

} // namespace GameEngine
