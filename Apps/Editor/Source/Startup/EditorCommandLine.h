#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace GameEngine::Editor::Startup
{
struct EditorCommandLineArgs
{
    // Absolute, canonicalized when possible.
    std::optional<std::filesystem::path> projectRoot;

    // Absolute path when provided.
    std::optional<std::filesystem::path> logFile;

    // UI automation / replay harness (Editor-only).
    // When set, the editor will run a scripted UI input timeline and emit telemetry to uiReplayLogFile.
    std::optional<std::filesystem::path> uiReplayScenario;
    std::optional<std::filesystem::path> uiReplayLogFile;
    // Auto-exit after N frames (useful to prevent hanging automation runs).
    std::optional<std::uint64_t> exitAfterFrames;

    // Phase B/C RG overhaul: emit one CSV row per RenderGraph::Compile() call to this path.
    // Column schema matches Engine/Modules/Rendering/Benchmarks/RenderGraphBenchmarks.cpp.
    // Intended for capturing W2 (cold start), W3 (scene load), W6 (skinned many) workloads
    // that can't be exercised from the headless harness. Disabled when unset.
    std::optional<std::filesystem::path> benchRgCsv;

    // Free text from --session-label, verbatim. Announces what this editor session is
    // FOR — which measurement arm, which lane — so a human deciding whether an editor
    // is broken, or an agent deciding whether it may touch one, does not have to
    // reconstruct that from the process table. Never inferred: absent means unlabelled,
    // and the session then identifies itself by worktree instead.
    std::optional<std::string> sessionLabel;

    // MCP debug-server port (--debug-port N). Takes precedence over
    // GE_EDITOR_DEBUG_PORT: an explicit flag beats the ambient environment.
    // Unset leaves the port to the env var, then the built-in default.
    std::optional<std::uint16_t> debugPort;

    // Dash-prefixed arguments this parser did not consume, verbatim and in
    // order. Reported as a Warning by the caller once logging is up, because a
    // silently swallowed flag reads as "applied" — a mistyped --debug-port used
    // to bind the default port instead, which is another developer's editor.
    // Engine flags (--engine-*) are consumed by ParseEngineArgs and excluded.
    std::vector<std::string> unrecognizedArgs;
};

EditorCommandLineArgs ParseEditorCommandLine(int argc, char** argv);
} // namespace GameEngine::Editor::Startup

