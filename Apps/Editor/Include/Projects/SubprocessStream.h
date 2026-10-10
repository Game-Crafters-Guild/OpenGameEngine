#pragma once

// PathArgUtf8 — the UTF-8 path-to-arg helper this runner's consumers pair
// with — lives in the std-only converter seam header so package modules
// (which cannot link Editor.exe-resident code) share the one definition.
#include "Editor/Assets/UnityImportHostedConverter.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace GameEngine {

// Result of running a child process to completion.
struct SubprocessResult {
    bool Spawned = false;   // false if the process could not be launched
    int ExitCode = -1;      // child exit code (valid when Spawned)
    std::string StderrTail; // bounded tail of the child's stderr (diagnostics)
};

// Run `exe args...`, invoking onStdoutLine for each complete newline-terminated
// stdout line as it arrives (called on the CALLING thread — run this on a worker
// thread and marshal UI updates back via UIElement::PostAction). stderr is drained
// concurrently into a bounded tail so a chatty child never deadlocks on a full
// pipe. Blocks until the child exits. Serves the project picker's git
// invocations (clone / sparse-checkout); the Unity importer that originally
// hosted this runner now executes its converter in-process.
SubprocessResult RunSubprocessStreaming(
    const std::filesystem::path& exe,
    const std::vector<std::string>& args,
    const std::function<void(const std::string&)>& onStdoutLine);

} // namespace GameEngine
