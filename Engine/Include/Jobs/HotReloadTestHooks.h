#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>


namespace GameEngine {
class IHotReloadTransport;

// Test-only seam for CompileServerCompiler to inject a custom transport factory.
// Production code should not set this; tests may set/unset around pipeline execution.
using CompileServerTransportFactory = std::function<std::unique_ptr<IHotReloadTransport>(const std::string& pipeName)>;

// Set a factory used by CompileServerCompiler when creating its transport. Pass nullptr to clear.
void SetCompileServerTransportFactoryForTests(CompileServerTransportFactory factory);

// Retrieve the currently-set factory (may be empty).

// Optional incremental compile hook: provide a set of changed files for the next server compile
void SetChangedFilesForNextCompile(const std::vector<std::string>& pathsUtf8);
// Retrieves and clears the pending changed files (single-consumer semantics)
std::vector<std::string> TakeChangedFilesForNextCompile();

// Test-only callback to observe the JSON request built for CompileServer
void SetOnCompileServerJsonBuiltForTests(std::function<void(const std::string&)> callback);

// Test-only: the kill-on-close job object that ephemeral CompileServerHost
// launches (GE_COMPILE_SERVER_EPHEMERAL=1) are assigned to on Windows.
// Returns the raw HANDLE as void*, or nullptr when the marker is unset,
// job creation failed, or on non-Windows platforms (issue #357).
void* GetCompileServerEphemeralJobHandleForTests();
// Debug-only: record which default compiler was chosen last
void ResetLastDefaultCompilerSelection();
bool WasLastDefaultCompilerServer();


CompileServerTransportFactory GetCompileServerTransportFactoryForTests();

} // namespace GameEngine

