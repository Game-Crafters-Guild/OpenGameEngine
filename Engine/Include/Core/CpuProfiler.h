#pragma once

#include "Core/NvtxRange.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Compile-time switch. When 0 the entire profiler collapses to inlinable
// no-ops so scope macros, registry calls, and RAII scopes optimize out.
// CMake: -DENABLE_CPU_PROFILING=OFF → GE_ENABLE_CPU_PROFILING=0.
#ifndef GE_ENABLE_CPU_PROFILING
#define GE_ENABLE_CPU_PROFILING 1
#endif

namespace GameEngine::Profiling
{

#if GE_ENABLE_CPU_PROFILING

// Lightweight CPU profiler intended for quick, opt-in editor diagnostics.
// - Disabled by default.
// - Uses simple inclusive-time accumulation per named scope.
// - Designed to be safe to call from any thread (internal mutex), but expected
//   to be used primarily on the main thread.
//
// Inline here: accessors that only read or write a scalar or atomic member.
// In CpuProfiler.cpp: everything touching owned storage — the sample map, the
// call tree, the frame history, the mutex — or the thread-local scope stack, so
// one process has one set of state. See the Get() definition for why.
class CpuProfiler
{
  public:
    using Clock = std::chrono::steady_clock;

    struct Sample
    {
        double totalMs = 0.0;
        std::uint32_t count = 0;
        // How many of `count` were recorded from a thread other than the main
        // one. Zero means the scope only ever ran on the main thread; equal to
        // count means it never did (an ECS system dispatched to a job worker);
        // in between means both. Off-main scopes reach this aggregate and
        // nothing else — the call tree is main-thread only — so without this a
        // reader cannot tell a worker's cost from the main thread's.
        std::uint32_t offMainCount = 0;
    };

    // Node in the per-frame main-thread call tree. Children are kept in a
    // vector (stable pointers for the scope stack) plus an index by scope name
    // so repeated calls to the same scope at the same level merge into a
    // single node with accumulated totalMs/callCount.
    struct CallNode
    {
        std::string_view name;
        double totalMs = 0.0;
        std::uint32_t callCount = 0;
        std::unordered_map<std::string_view, std::size_t> childIndex;
        std::vector<std::unique_ptr<CallNode>> children;

        CallNode() = default;
        CallNode(CallNode&&) noexcept = default;
        CallNode& operator=(CallNode&&) noexcept = default;
        CallNode(const CallNode&) = delete;
        CallNode& operator=(const CallNode&) = delete;
    };

    struct TreeRow
    {
        std::string name;
        double totalMs = 0.0;
        double selfMs = 0.0;
        std::uint32_t callCount = 0;
        int depth = 0;
    };

    // One entry per frame in the ring buffer: the flattened call tree plus
    // the sum of top-level totalMs (used to feed the panel's frame chart).
    struct FrameHistoryEntry
    {
        double totalMs = 0.0;
        std::vector<TreeRow> rows;
    };

    static constexpr std::size_t kFrameHistorySize = 600;

    // Defined in CpuProfiler.cpp: one instance per process, exported from
    // Engine.dll. See that definition before making this inline again.
    static CpuProfiler& Get();

    // Main-thread detection. The first BeginFrame() call records its thread id
    // as "the main thread"; scope tracking only records tree nodes when called
    // from that thread (other threads fall back to the flat sample map).
    bool IsMainThread() const
    {
        if (!m_MainThreadKnown.load(std::memory_order_acquire))
            return false;
        return std::this_thread::get_id() == m_MainThreadId;
    }

    // Disabling freezes the history in place rather than clearing it, so the
    // user can still scrub after flipping off. The next enable's first
    // BeginFrame resets the tree but leaves history until a new frame pushes it
    // out.
    void SetEnabled(bool enabled) { m_Enabled.store(enabled, std::memory_order_relaxed); }

    bool IsEnabled() const { return m_Enabled.load(std::memory_order_relaxed); }

    // Called by ScopedCpuProfile on the main thread only. No lock needed: tree
    // mutations and the panel-side reader both happen on the main thread, and
    // BeginFrame resets the tree before any scopes open in the new frame.
    CallNode* PushScope(std::string_view name);
    void PopScope(double totalMs);

    // Copy the last completed frame's call tree into a flat, sorted, preorder
    // list for display. Walks the tree held in m_LastFrameRoot — populated at
    // the start of each frame by BeginFrame(), so readers always see a
    // finished frame with no open scopes.
    void CopyFrameTree(std::vector<TreeRow>& out);

    // Per-frame history accessors. History is only appended while the profiler
    // is enabled; each entry stores the flattened tree for that frame plus its
    // summed top-level totalMs, which also drives the panel's frame chart.
    std::size_t GetFrameHistorySize() const;
    void CopyFrameHistoryTotals(std::vector<float>& out) const;
    void CopyFrameTreeAtHistoryIndex(std::size_t idx, std::vector<TreeRow>& out) const;

    // Snapshot the entire history in one pass (cheaper than calling
    // CopyFrameTreeAtHistoryIndex repeatedly for the panel's freeze step).
    void SnapshotFrameHistory(std::vector<FrameHistoryEntry>& out) const;

    void ClearFrameHistory();

    // Called once per frame by the application loop (best-effort).
    void BeginFrame();

    // Request a console/log dump. Works regardless of enabled state — the dump
    // is serviced as long as there's a captured worst-frame snapshot to emit.
    void RequestDump();

    // Request a dump and attach a contextual line (e.g., current perf overlay).
    void RequestDump(std::string contextLine);

    // Called once per frame by the application loop (best-effort).
    // Returns true if a dump was requested and consumed this frame.
    bool EndFrameAndTryDumpToLog(std::function<void(std::string_view)> logLine);

    /// Accumulates into the live frame's flat aggregate.
    /// @param offMainThread whether the scope ran off the main thread, decided
    ///        where the scope opened (see Sample::offMainCount). The caller
    ///        hands over the same answer it used to decide whether to open a
    ///        tree node, so one scope cannot be a tree node and an off-main
    ///        sample both — which asking again here would allow for a scope
    ///        that opened before the first BeginFrame anchored the main thread.
    void AddSample(std::string_view name, double ms, bool offMainThread);

    // Copy the last completed frame's flat aggregate (inclusive times). Reads
    // the buffer BeginFrame swaps aside, not the live frame, because every
    // caller wants a whole frame: the debug server is pumped at the top of the
    // frame microseconds after that swap, the panel's fallback describes a
    // frame, and the replay runner samples mid-frame. Frozen while the profiler
    // is disabled, like the call tree and the history.
    void CopyFrameSamples(std::vector<std::pair<std::string_view, Sample>>& out);

  private:
    CpuProfiler() = default;

    // Preorder walk of the tree emitting a flat row per node. Children are
    // sorted by totalMs descending so hot paths land at the top of each
    // subtree. selfMs is derived (totalMs - sum of children's totalMs).
    void FlattenTreeInto(const CallNode& node, int depth, std::vector<TreeRow>& out);

    std::atomic<bool> m_Enabled{false};
    std::atomic<bool> m_DumpRequested{false};

    mutable std::mutex m_Mutex;

    // Flat per-scope aggregate, double-buffered for the same reason as the
    // call tree below: m_FrameSamples is the live frame, m_LastFrameSamples is
    // the completed one every reader gets. BeginFrame swaps them rather than
    // clearing, so both bucket arrays stay alive and a steady-state frame
    // allocates nothing; a frame that adds a scope name still rehashes.
    std::unordered_map<std::string_view, Sample> m_FrameSamples;
    std::unordered_map<std::string_view, Sample> m_LastFrameSamples;

    // Main-thread call tree. m_FrameRoot is the live tree for the current
    // frame; m_LastFrameRoot is the previous frame's tree, kept stable for
    // panel readers until the next BeginFrame swap.
    CallNode m_FrameRoot;
    CallNode m_LastFrameRoot;
    std::thread::id m_MainThreadId{};
    std::atomic<bool> m_MainThreadKnown{false};

    // Ring history of flattened frames. Bounded by kFrameHistorySize.
    std::deque<FrameHistoryEntry> m_FrameHistory;

    // Scope stack used by ScopedCpuProfile, written only from the main thread
    // (it skips the tree path off-main). Defined in CpuProfiler.cpp: one stack
    // per thread per process, and keep its only users — PushScope, PopScope,
    // BeginFrame — defined there with it.
    static thread_local std::vector<CallNode*> t_ScopeStack;

    // Dump context (attached at request time, printed when the dump occurs).
    std::string m_PendingDumpContext;

    // Worst-frame tracking since last dump.
    bool m_HasWorstFrame = false;
    double m_WorstFrameScore = 0.0;
    double m_WorstFrameMaxMs = 0.0;
    std::string m_WorstFrameMaxName;
    std::vector<std::pair<std::string_view, Sample>> m_WorstFrameSnap;
};

#else // GE_ENABLE_CPU_PROFILING == 0 — shipping/user build stub.

class CpuProfiler
{
  public:
    using Clock = std::chrono::steady_clock;

    struct Sample
    {
        double totalMs = 0.0;
        std::uint32_t count = 0;
        std::uint32_t offMainCount = 0;
    };

    struct CallNode
    {
        std::string_view name;
        double totalMs = 0.0;
        std::uint32_t callCount = 0;
    };

    struct TreeRow
    {
        std::string name;
        double totalMs = 0.0;
        double selfMs = 0.0;
        std::uint32_t callCount = 0;
        int depth = 0;
    };

    struct FrameHistoryEntry
    {
        double totalMs = 0.0;
        std::vector<TreeRow> rows;
    };

    static constexpr std::size_t kFrameHistorySize = 600;

    // Out-of-line for the same single-instance reason as the enabled build; the
    // stub still needs one shared object to hang the no-op calls off of.
    static CpuProfiler& Get();

    void SetEnabled(bool) {}
    bool IsEnabled() const { return false; }
    bool IsMainThread() const { return false; }
    void BeginFrame() {}
    void RequestDump() {}
    void RequestDump(std::string) {}
    bool EndFrameAndTryDumpToLog(std::function<void(std::string_view)>) { return false; }
    void AddSample(std::string_view, double, bool) {}
    CallNode* PushScope(std::string_view) { return nullptr; }
    void PopScope(double) {}
    void CopyFrameSamples(std::vector<std::pair<std::string_view, Sample>>& out) { out.clear(); }
    void CopyFrameTree(std::vector<TreeRow>& out) { out.clear(); }

    std::size_t GetFrameHistorySize() const { return 0; }
    void CopyFrameHistoryTotals(std::vector<float>& out) const { out.clear(); }
    void CopyFrameTreeAtHistoryIndex(std::size_t, std::vector<TreeRow>& out) const { out.clear(); }
    void SnapshotFrameHistory(std::vector<FrameHistoryEntry>& out) const { out.clear(); }
    void ClearFrameHistory() {}

  private:
    CpuProfiler() = default;
};

#endif // GE_ENABLE_CPU_PROFILING

#if GE_ENABLE_CPU_PROFILING
// RAII scope for CPU profiling. Prefer using GE_CPU_PROFILE_SCOPE("name").
class ScopedCpuProfile
{
  public:
    /// @param name NUL-terminated, statically-stored label. It is retained as a
    ///             string_view map key that outlives the frame and handed to
    ///             NVTX, so a std::string temporary would dangle both.
    explicit ScopedCpuProfile(const char* name)
        : m_Nvtx(name)
        , m_Name(name)
    {
        auto& prof = CpuProfiler::Get();
        if (!prof.IsEnabled())
            return;
        m_Active = true;
        m_Start = CpuProfiler::Clock::now();
        // Main-thread scopes also populate the call tree; off-main-thread
        // scopes only feed the flat sample map.
        if (prof.IsMainThread())
        {
            prof.PushScope(m_Name);
            m_InTree = true;
        }
    }

    ~ScopedCpuProfile()
    {
        if (!m_Active)
            return;
        const auto end = CpuProfiler::Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - m_Start).count();
        auto& prof = CpuProfiler::Get();
        if (m_InTree)
            prof.PopScope(ms);
        prof.AddSample(m_Name, ms, !m_InTree);
    }

    ScopedCpuProfile(const ScopedCpuProfile&) = delete;
    ScopedCpuProfile& operator=(const ScopedCpuProfile&) = delete;

  private:
    // First member: constructed before and destroyed after the timing logic, so
    // the NVTX range brackets the whole scope. Constructed unconditionally —
    // NVTX emission is deliberately independent of CpuProfiler::IsEnabled().
    ScopedNvtxRange m_Nvtx;
    std::string_view m_Name;
    CpuProfiler::Clock::time_point m_Start{};
    bool m_Active = false;
    bool m_InTree = false;
};

#else // GE_ENABLE_CPU_PROFILING == 0

// GE_CPU_PROFILE_SCOPE expands to nothing in this configuration, so this stub
// exists only for the handful of call sites that construct the type directly.
// It carries no ScopedNvtxRange: NVTX would then be emitted by those few sites
// and by no macro site, which is less useful than emitting nothing. Building
// with ENABLE_CPU_PROFILING=OFF and ENABLE_NVTX=ON therefore yields only the
// top-level "Frame" range.
class ScopedCpuProfile
{
  public:
    explicit ScopedCpuProfile(const char*) {}
    ScopedCpuProfile(const ScopedCpuProfile&) = delete;
    ScopedCpuProfile& operator=(const ScopedCpuProfile&) = delete;
};

#endif // GE_ENABLE_CPU_PROFILING

} // namespace GameEngine::Profiling

// Macro helpers
#define GE__CPU_PROF_CONCAT_INNER(a, b) a##b
#define GE__CPU_PROF_CONCAT(a, b) GE__CPU_PROF_CONCAT_INNER(a, b)

// Usage:
//   GE_CPU_PROFILE_SCOPE("SceneViewPanel.MouseMoveDrag");
#if GE_ENABLE_CPU_PROFILING
#define GE_CPU_PROFILE_SCOPE(nameLiteral) \
    ::GameEngine::Profiling::ScopedCpuProfile GE__CPU_PROF_CONCAT(__ge_cpu_prof_, __LINE__)(nameLiteral)
#else
#define GE_CPU_PROFILE_SCOPE(nameLiteral) ((void)0)
#endif
