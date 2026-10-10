#pragma once

#include <string>

namespace GameEngine
{

// Command used to launch the .NET host ("dotnet") for compile-server and
// script-build child processes. Resolves once per process via the engine's
// shared dotnet discovery (DOTNET_ROOT, PATH, standard install locations) and
// returns the absolute path; falls back to the bare name "dotnet" when nothing
// resolves so the OS-level lookup of the spawn call gets the final word.
//
// PATH alone is not enough: agent/CI shells and Finder/Dock launches routinely
// run with a PATH that lacks dotnet even though the SDK is installed.
const std::string& DotnetHostCommand();

// True when `DotnetHostCommand() --version` exits 0 and prints a version, i.e.
// a .NET SDK is installed and runnable. Probes once per process (one child
// process spawn) and caches the answer, like DotnetHostCommand.
bool IsDotnetSdkAvailable();

} // namespace GameEngine
