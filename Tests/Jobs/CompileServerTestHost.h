#pragma once
#include <cstdint>
#include <string>

// Test-side lifetime management for real CompileServerHost processes
// (issue #357). Hosts are resident services by design (workspace-keyed
// pipes, warm Roslyn caches), so tests must opt into ephemeral lifetime:
//
// 1. GE_COMPILE_SERVER_EPHEMERAL=1 is set for the whole test process (a
//    global gtest Environment does this before the first test), so every
//    host spawned through the engine's launch path — explicitly via
//    StartTestCompileServer or implicitly via pipeline/ScriptManager
//    compiles — lands in a Windows kill-on-close job object and dies with
//    the test process, even when the run crashes or is killed.
// 2. Fixture teardown (the same Environment) reaps registered pipes on
//    orderly exit: __shutdown__, bounded grace, then kill by PID.
namespace CompileServerTestHost {

// Sets GE_COMPILE_SERVER_EPHEMERAL=1 for this process (idempotent).
void EnsureEphemeralMarker();

// Spawns a host for the pipe through the engine's real launch path
// (CompileServerClient::StartServerForPipe) and registers the pipe for
// teardown reaping. Returns false when the host DLL cannot be found or the
// process failed to start.
bool StartTestCompileServer(const std::string& pipeName);

// PID self-reported (via __version__) by a host currently listening on the
// pipe; 0 when none is reachable.
uint32_t QueryHostPid(const std::string& pipeName, int connectWaitMs = 100);

// Shuts down every host listening on the pipe: __shutdown__ per instance,
// ~2s grace for orderly exit, then kill by PID.
void ReapHostsOnPipe(const std::string& pipeName);

// Reaps every pipe registered by StartTestCompileServer.
void ReapAllTestHosts();

// "GE_CompileServer_Test_<tag>_<pid>" — unique per run so tests never talk
// to a stale host from a previous (crashed) run.
std::string UniquePipeName(const char* tag);

} // namespace CompileServerTestHost
