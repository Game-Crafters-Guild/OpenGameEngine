#include "Telemetry/RGCsvSidecar.h"

#include "Logger/Logger.h"

#include <chrono>

namespace GameEngine::Editor::Telemetry
{
namespace
{
    std::FILE* OpenForWrite(const std::string& path)
    {
        std::FILE* file = nullptr;
#if defined(_MSC_VER)
        if (fopen_s(&file, path.c_str(), "wb") != 0)
            file = nullptr;
#else
        file = std::fopen(path.c_str(), "wb");
#endif
        return file;
    }

    std::string DerivePassesPath(const std::filesystem::path& aggregate)
    {
        std::filesystem::path p = aggregate;
        p += ".passes.csv";
        return p.string();
    }
} // namespace

std::unique_ptr<RGCsvSidecar> RGCsvSidecar::Open(const std::filesystem::path& csvPath,
                                                 std::string_view commitSha,
                                                 std::string_view workload)
{
    const std::string pathStr = csvPath.string();
    std::FILE* file = OpenForWrite(pathStr);
    if (!file)
    {
        Logger::Log::Error("RGCsvSidecar: failed to open '{}' for writing", pathStr);
        return nullptr;
    }

    std::fputs(
        "commit_sha,workload,run_id,frame_idx,rgCompileMs,rgExecuteMs,"
        "preambleMs,logicalMs,collectMs,topoMs,setupMs,hashMs,aliasMs,"
        "fastPathMs,fullPathMs,validationMs,postMs,"
        "passCount,transientResourceCount,persistentResourceCount,invalidationReason,compileCategory,"
        "topoCacheHit,topoCacheKey,"
        "appPreUiMs,appDebugPanelsMs,appUiWindowsMs,appTailMs\n",
        file);
    std::fflush(file);

    const std::string passesPathStr = DerivePassesPath(csvPath);
    std::FILE* passesFile = OpenForWrite(passesPathStr);
    if (!passesFile)
    {
        Logger::Log::Warning("RGCsvSidecar: failed to open per-pass CSV '{}'; aggregate capture continues",
                             passesPathStr);
    }
    else
    {
        std::fputs(
            "commit_sha,workload,run_id,frame_idx,pass_id,pass_name,phase,cpuMs,gpuSpanMs\n",
            passesFile);
        std::fflush(passesFile);
    }

    const std::uint64_t runId = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    Logger::Log::Info("RGCsvSidecar: writing RG compile trace to '{}' (run_id={})", pathStr, runId);
    if (passesFile)
        Logger::Log::Info("RGCsvSidecar: writing per-pass trace to '{}'", passesPathStr);

    return std::unique_ptr<RGCsvSidecar>(
        new RGCsvSidecar(file, passesFile, std::string(commitSha), std::string(workload), runId));
}

RGCsvSidecar::RGCsvSidecar(std::FILE* file,
                           std::FILE* passesFile,
                           std::string commitSha,
                           std::string workload,
                           std::uint64_t runId)
    : m_File(file)
    , m_PassesFile(passesFile)
    , m_CommitSha(std::move(commitSha))
    , m_Workload(std::move(workload))
    , m_RunId(runId)
{
}

void RGCsvSidecar::SetAppUpdateSubPhases(double preUiMs, double debugPanelsMs,
                                         double uiWindowsMs, double tailMs)
{
    m_AppPreUiMs = preUiMs;
    m_AppDebugPanelsMs = debugPanelsMs;
    m_AppUiWindowsMs = uiWindowsMs;
    m_AppTailMs = tailMs;
}

RGCsvSidecar::~RGCsvSidecar()
{
    if (m_File)
    {
        std::fflush(m_File);
        std::fclose(m_File);
    }
    if (m_PassesFile)
    {
        std::fflush(m_PassesFile);
        std::fclose(m_PassesFile);
    }
}

void RGCsvSidecar::Append(int frameIdx,
                          double compileMs,
                          double executeMs)
{
    if (!m_File) return;

    // Compile-phase breakdown, compile stats, invalidation reason/category, and the
    // topology cache hit signal were retained-graph introspection. RenderGraph (immediate
    // mode) has no equivalent compile-phase mirror, so those columns are stubbed to
    // zero/empty to keep the column schema diffable against the headless harness.
    // rgCompileMs / rgExecuteMs come from caller chrono and remain authoritative.
    std::fprintf(m_File,
        "%s,%s,%llu,%d,%.4f,%.4f,"
        "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
        "%.4f,%.4f,%.4f,%.4f,"
        "%zu,%zu,%zu,%s,%s,"
        "%d,%llu,"
        "%.4f,%.4f,%.4f,%.4f\n",
        m_CommitSha.c_str(),
        m_Workload.c_str(),
        static_cast<unsigned long long>(m_RunId),
        frameIdx,
        compileMs,
        executeMs,
        0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0,
        0.0, 0.0,
        static_cast<std::size_t>(0), static_cast<std::size_t>(0), static_cast<std::size_t>(0),
        "",
        "",
        0,
        static_cast<unsigned long long>(0),
        m_AppPreUiMs, m_AppDebugPanelsMs, m_AppUiWindowsMs, m_AppTailMs);
    std::fflush(m_File);

    // Per-pass cpuMs/gpuSpanMs rows were sourced from the retained graph's per-pass
    // profiler. The passes CSV header is kept (harmless text) but no per-pass rows
    // are emitted until an RenderGraph per-pass introspection mirror is wired in here.
}
} // namespace GameEngine::Editor::Telemetry
