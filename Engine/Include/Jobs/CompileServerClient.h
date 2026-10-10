#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Jobs/IHotReloadTransport.h"

namespace GameEngine {

struct CompileServerDiagnostic {
    std::string Severity; // "Warning" | "Error"
    std::string Code;
    std::string FileUtf8;
    int Line = 0;
    int Column = 0;
    std::string MessageUtf8;
};

struct CompileServerResponse {
    bool Success = false;
    std::vector<CompileServerDiagnostic> Warnings;
    std::vector<CompileServerDiagnostic> Errors;
    std::vector<uint8_t> AssemblyBytes;
    std::vector<uint8_t> PdbBytes;
    std::string OutputPathUtf8; // optional: server may return a file path instead of bytes
};

// Deletes the stale host pid files in pidDir (<pipe>.pid, line 1 the process id, line 2 the
// host's assembly path; written by Managed/CompileServerHost/Program.cs): files not in that
// format, files whose process has exited, and (Windows) files whose pid now belongs to a
// process started after the file was written. A host removes its own file on a clean exit;
// a killed or crashed one leaves it behind. The client runs this once per process over
// <temp>/GE_CompileServer before it first launches a host.
void PruneStaleCompileServerPidFiles(const std::filesystem::path& pidDir);

class CompileServerClient {
public:
    explicit CompileServerClient(std::unique_ptr<IHotReloadTransport> transport, std::string pipeNameUtf8);
    // Sends JSON request and parses JSON response into outResponse
    bool CompileJson(const std::string& requestJson, bool incremental);
    bool Compile(const std::string& requestJson, CompileServerResponse& outResponse);

    // Launches a CompileServerHost for the given pipe through the engine's real
    // spawn path (host discovery + ephemeral job-object treatment, issue #357).
    // Test fixtures must spawn through this instead of hand-rolling
    // CreateProcess so test-spawned hosts share production launch semantics.
    static bool StartServerForPipe(const std::string& pipeNameUtf8);

    // Lightweight counters for observability
    static int GetCompileServerSuccessCount();
    static int GetCompileServerFallbackCount();
    static void NoteFallback();
    static void ResetCounters();

    // Diagnostics observers — called once per diagnostic after each compile response is parsed.
    // Called on whichever thread runs Compile(), which may be a background thread.
    using DiagnosticsObserver = std::function<void(const CompileServerDiagnostic&)>;
    static uint64_t RegisterDiagnosticsObserver(DiagnosticsObserver callback);
    static void UnregisterDiagnosticsObserver(uint64_t id);

    // The assembly name both observers receive is the request's "AssemblyName" field, or
    // "GameEngine.Scripts" when the request has none (the hot-reload pipeline's
    // project-scripts request).

    // Batch diagnostics observers — called once per compile response that has diagnostics,
    // with the request's assembly name and all warnings followed by all errors. Called on
    // whichever thread runs Compile().
    using DiagnosticsBatchObserver =
        std::function<void(const std::string& assemblyName, const std::vector<CompileServerDiagnostic>&)>;
    static uint64_t RegisterDiagnosticsBatchObserver(DiagnosticsBatchObserver callback);
    static void UnregisterDiagnosticsBatchObserver(uint64_t id);

    // Compile-started observers — called at the start of each Compile() call (before the
    // response) with the request's assembly name, so a consumer can drop that assembly's
    // previous diagnostics and keep every other assembly's.
    using CompileStartedObserver = std::function<void(const std::string& assemblyName)>;
    static uint64_t RegisterCompileStartedObserver(CompileStartedObserver callback);
    static void UnregisterCompileStartedObserver(uint64_t id);

private:
    bool StartServerIfNeeded();
    static bool FindHostPath(std::string& outPath);
    static std::wstring WidenAscii(const std::string& s);
    bool ParseResponse(const std::string& json, CompileServerResponse& outResponse);

    static void NotifyCompileStarted(const std::string& assemblyName);
    static void NotifyDiagnostics(const std::string& assemblyName, const CompileServerResponse& response);

    std::unique_ptr<IHotReloadTransport> m_Transport;
    std::string m_PipeNameUtf8;
    // True when the transport was injected by the caller (tests). Injected
    // transports own the "server" end, so Compile() must never spawn or wait
    // on a real CompileServerHost process for them.
    bool m_ExternalTransport = false;
};

} // namespace GameEngine
