#pragma once

#include <cstdio>
#include <filesystem>
#include <memory>

namespace GameEngine::Editor::Telemetry
{
// Opens a CSV file and appends one row per RG compile+execute pair.
// Column schema mirrors Engine/Modules/Rendering/Benchmarks/RenderGraphBenchmarks.cpp
// so the Editor sidecar captures can be diffed against the headless harness output.
//
// Ownership: one instance per Editor session. The caller measures rgCompileMs /
// rgExecuteMs via chrono and passes them through Append(); the sidecar writes one
// row per frame and flushes per row so a crash mid-session still yields a usable file.
class RGCsvSidecar
{
public:
    // Returns nullptr if the aggregate file can't be opened. Logs an error via Logger in that case.
    // A sibling per-pass CSV is opened at `{csvPath}.passes.csv` for per-pass cpuMs/gpuSpanMs timings;
    // if it can't be opened the sidecar still records aggregate rows (warning logged).
    static std::unique_ptr<RGCsvSidecar> Open(const std::filesystem::path& csvPath,
                                              std::string_view commitSha,
                                              std::string_view workload);

    ~RGCsvSidecar();

    // Append one CSV row for the most recent compile+execute pair.
    // compileMs / executeMs are measured by the caller (RenderServices wraps the calls).
    // frameIdx is the caller-supplied monotonic frame counter.
    void Append(int frameIdx,
                double compileMs,
                double executeMs);

    // Optional: record caller-side AppUpdate sub-phase breakdown to include in the
    // next Append() row. Caller sets per-frame values; if never set, columns are zero.
    // Useful for attributing the AppUpdateMs bucket to editor sub-phases (UI,
    // panels, pre-UI bookkeeping, tail) without a second sidecar file.
    void SetAppUpdateSubPhases(double preUiMs, double debugPanelsMs,
                               double uiWindowsMs, double tailMs);

private:
    RGCsvSidecar(std::FILE* file,
                 std::FILE* passesFile,
                 std::string commitSha,
                 std::string workload,
                 std::uint64_t runId);
    RGCsvSidecar(const RGCsvSidecar&) = delete;
    RGCsvSidecar& operator=(const RGCsvSidecar&) = delete;

    std::FILE* m_File;
    std::FILE* m_PassesFile;
    std::string m_CommitSha;
    std::string m_Workload;
    std::uint64_t m_RunId;
    // AppUpdate sub-phase breakdown (set per frame by caller before Append).
    double m_AppPreUiMs = 0.0;
    double m_AppDebugPanelsMs = 0.0;
    double m_AppUiWindowsMs = 0.0;
    double m_AppTailMs = 0.0;
};
} // namespace GameEngine::Editor::Telemetry
