#pragma once

namespace GameEngine::Profiling
{

/// RAII NVTX range: pushes on construction, pops on destruction.
///
/// Out-of-line by design. The NVTX headers and the GE_ENABLE_NVTX compile gate
/// live entirely inside NvtxRange.cpp, so this declaration carries no #if and
/// has one layout everywhere. That matters because CpuProfiler.h embeds it in
/// ScopedCpuProfile, which five modules outside Engine compile — MeshPicking,
/// UI, Scene, TerrainECS and CBTTerrainECS. None of them can link Engine, so
/// each replays Engine's INTERFACE compile definitions to keep
/// GE_ENABLE_CPU_PROFILING (and hence ScopedCpuProfile's layout) consistent.
/// A gate in this header would add a second way for that layout to diverge.
///
/// Because the ctor/dtor are defined in Engine, any binary linking one of those
/// modules' objects must also link Engine — MeshPickingTests learned this the
/// hard way (LNK2019).
///
/// Emitted independently of CpuProfiler::IsEnabled(): an external trace must not
/// depend on someone remembering to arm the in-engine profiler first.
///
/// Thread affinity: any thread. NVTX keeps a per-thread range stack, so ranges
/// must nest LIFO within each thread. The enable gate is written once by
/// InitializeNvtx during Application::Initialize, before any worker thread
/// starts, and is read-only afterwards.
class ScopedNvtxRange
{
  public:
    /// @param name NUL-terminated range label. Must outlive the range; callers
    ///             pass string literals or other statically-stored strings.
    explicit ScopedNvtxRange(const char* name);
    ~ScopedNvtxRange();

    ScopedNvtxRange(const ScopedNvtxRange&) = delete;
    ScopedNvtxRange& operator=(const ScopedNvtxRange&) = delete;

  private:
    bool m_Active;
};

/// Resolves the GE_NVTX environment gate (truthy unless "0") and registers the
/// NVTX domain. Call once from Application::Initialize before workers start;
/// ranges constructed before this are inert.
void InitializeNvtx();

} // namespace GameEngine::Profiling
