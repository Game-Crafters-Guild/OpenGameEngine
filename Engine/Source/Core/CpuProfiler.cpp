#include "Core/CpuProfiler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace GameEngine::Profiling
{

// Defined here rather than header-inline so the function-local static has
// exactly ONE storage location: inside Engine.dll's data segment, exported via
// the generated exports.def. Every module that links Engine's import lib
// (Editor.exe, its panels, the debug server) resolves this same instance, so
// scopes recorded from Engine.dll — UIManager, SceneTlas, AssetManager,
// RenderServices, and every terrain system — are visible to the editor-side
// panel and get_cpu_profiler handler that read them. When Get() was
// header-inline each linked image compiled its own function-local static: the
// editor armed and read its own instance while Application::MainLoop cleared
// frames on Engine.dll's, so the flat list held only Apps/Editor scopes and
// accumulated forever, and the call tree stayed empty because BeginFrame never
// ran on the instance that was armed. Same failure and same fix as
// DebugMetrics::Get() (see DebugMetrics.cpp).
//
// KNOWN LIMIT: GameEngine.Native.dll statically links its own copy of Engine
// (GE_SCRIPTING_STATIC), so code executing inside that DLL still sees a separate
// CpuProfiler instance. This is the pre-existing dual-static family; if a
// script-side producer ever needs to publish into the shared instance, the
// GE_SetHost* redirection pattern would apply. Intentionally not addressed here.
CpuProfiler& CpuProfiler::Get()
{
    static CpuProfiler s;
    return s;
}

#if GE_ENABLE_CPU_PROFILING

// One definition, in Engine.dll, for the same reason as Get(): a header-inline
// PushScope would give each image its own thread_local stack, so a nested
// engine scope would attach to the tree root instead of the editor scope that
// called it.
thread_local std::vector<CpuProfiler::CallNode*> CpuProfiler::t_ScopeStack{};

CpuProfiler::CallNode* CpuProfiler::PushScope(std::string_view name)
{
    CallNode* parent = t_ScopeStack.empty() ? &m_FrameRoot : t_ScopeStack.back();
    auto it = parent->childIndex.find(name);
    CallNode* node = nullptr;
    if (it == parent->childIndex.end())
    {
        auto nn = std::make_unique<CallNode>();
        nn->name = name;
        node = nn.get();
        parent->childIndex.emplace(name, parent->children.size());
        parent->children.push_back(std::move(nn));
    }
    else
    {
        node = parent->children[it->second].get();
    }
    t_ScopeStack.push_back(node);
    return node;
}

void CpuProfiler::PopScope(double totalMs)
{
    if (t_ScopeStack.empty())
        return;
    CallNode* node = t_ScopeStack.back();
    t_ScopeStack.pop_back();
    node->totalMs += totalMs;
    node->callCount += 1;
}

void CpuProfiler::CopyFrameTree(std::vector<TreeRow>& out)
{
    out.clear();
    FlattenTreeInto(m_LastFrameRoot, 0, out);
}

std::size_t CpuProfiler::GetFrameHistorySize() const
{
    std::scoped_lock lock(m_Mutex);
    return m_FrameHistory.size();
}

void CpuProfiler::CopyFrameHistoryTotals(std::vector<float>& out) const
{
    std::scoped_lock lock(m_Mutex);
    out.clear();
    out.reserve(m_FrameHistory.size());
    for (const auto& e : m_FrameHistory)
        out.push_back(static_cast<float>(e.totalMs));
}

void CpuProfiler::CopyFrameTreeAtHistoryIndex(std::size_t idx, std::vector<TreeRow>& out) const
{
    out.clear();
    std::scoped_lock lock(m_Mutex);
    if (idx >= m_FrameHistory.size())
        return;
    out = m_FrameHistory[idx].rows;
}

void CpuProfiler::SnapshotFrameHistory(std::vector<FrameHistoryEntry>& out) const
{
    std::scoped_lock lock(m_Mutex);
    out.assign(m_FrameHistory.begin(), m_FrameHistory.end());
}

void CpuProfiler::ClearFrameHistory()
{
    std::scoped_lock lock(m_Mutex);
    m_FrameHistory.clear();
}

void CpuProfiler::BeginFrame()
{
    // Record the main thread id the first time BeginFrame runs. The app
    // loop is single-threaded and calls this from the main thread, which
    // gives us a stable anchor for IsMainThread() without an explicit
    // initialization step.
    if (!m_MainThreadKnown.load(std::memory_order_acquire))
    {
        m_MainThreadId = std::this_thread::get_id();
        m_MainThreadKnown.store(true, std::memory_order_release);
    }

    if (!IsEnabled())
        return;
    std::scoped_lock lock(m_Mutex);

    // Swap, don't clear: readers get the frame that just finished, because
    // every one of them wants a whole frame and the debug server is pumped at
    // the very top of the frame — microseconds after this point, with nothing
    // yet recorded in the live map. The swap also hands m_FrameSamples the
    // other map's buckets, so the clear below keeps that capacity and a
    // steady-state frame costs no allocation.
    m_LastFrameSamples.swap(m_FrameSamples);
    m_FrameSamples.clear();

    // Move the just-finished frame into the readable buffer so panels
    // always see a fully-popped tree. Reset the live tree for the new
    // frame. The scope stack should be empty at this point — BeginFrame
    // is called between frames, outside any profiled scope.
    m_LastFrameRoot = std::move(m_FrameRoot);
    m_FrameRoot = CallNode{};
    t_ScopeStack.clear();

    // Push the just-finished frame into the ring history so panels can
    // scrub across past frames. Skip the very first BeginFrame after
    // enable: at that moment m_LastFrameRoot is empty and would add a
    // zero-entry spike.
    if (!m_LastFrameRoot.children.empty())
    {
        FrameHistoryEntry entry;
        FlattenTreeInto(m_LastFrameRoot, 0, entry.rows);
        for (const auto& child : m_LastFrameRoot.children)
            entry.totalMs += child->totalMs;
        m_FrameHistory.push_back(std::move(entry));
        while (m_FrameHistory.size() > kFrameHistorySize)
            m_FrameHistory.pop_front();
    }
}

void CpuProfiler::RequestDump()
{
    m_DumpRequested.store(true, std::memory_order_relaxed);
}

void CpuProfiler::RequestDump(std::string contextLine)
{
    {
        std::scoped_lock lock(m_Mutex);
        m_PendingDumpContext = std::move(contextLine);
    }
    m_DumpRequested.store(true, std::memory_order_relaxed);
}

bool CpuProfiler::EndFrameAndTryDumpToLog(std::function<void(std::string_view)> logLine)
{
    const bool enabled = IsEnabled();
    const bool dumpPending = m_DumpRequested.load(std::memory_order_relaxed);
    if (!enabled && !dumpPending)
        return false;

    // Track worst frame since last dump (so users can dump *after* an interaction
    // and still capture the spike).
    std::vector<std::pair<std::string_view, Sample>> worstSnap;
    double worstScore = 0.0;
    std::string worstMaxName;
    double worstMaxMs = 0.0;
    std::string pendingContext;
    bool doDump = false;
    {
        std::scoped_lock lock(m_Mutex);

        if (enabled)
        {
            // Compute score for the current frame. Prefer overall EditorApplication.Update time
            // when present; otherwise use max single-scope time.
            double frameMaxMs = 0.0;
            std::string_view frameMaxName;
            for (const auto& kv : m_FrameSamples)
            {
                if (kv.second.totalMs > frameMaxMs)
                {
                    frameMaxMs = kv.second.totalMs;
                    frameMaxName = kv.first;
                }
            }
            double frameScore = frameMaxMs;
            if (auto it = m_FrameSamples.find("EditorApplication.Update"); it != m_FrameSamples.end())
            {
                frameScore = it->second.totalMs;
            }

            if (!m_HasWorstFrame || frameScore > m_WorstFrameScore)
            {
                m_HasWorstFrame = true;
                m_WorstFrameScore = frameScore;
                m_WorstFrameMaxMs = frameMaxMs;
                m_WorstFrameMaxName.assign(frameMaxName.data(), frameMaxName.size());
                m_WorstFrameSnap.clear();
                m_WorstFrameSnap.reserve(m_FrameSamples.size());
                for (const auto& kv : m_FrameSamples)
                {
                    m_WorstFrameSnap.emplace_back(kv.first, kv.second);
                }
            }
        }

        // Only consume the pending flag once there's data to dump, so a
        // dump requested with an empty history doesn't get silently eaten.
        if (m_DumpRequested.load(std::memory_order_relaxed) && m_HasWorstFrame)
        {
            m_DumpRequested.store(false, std::memory_order_relaxed);
            doDump = true;
        }
        if (doDump)
        {
            worstSnap = m_WorstFrameSnap;
            worstScore = m_WorstFrameScore;
            worstMaxName = m_WorstFrameMaxName;
            worstMaxMs = m_WorstFrameMaxMs;
            pendingContext = std::move(m_PendingDumpContext);

            // Reset worst tracking so the next dump captures the next spike window.
            m_HasWorstFrame = false;
            m_WorstFrameScore = 0.0;
            m_WorstFrameMaxMs = 0.0;
            m_WorstFrameMaxName.clear();
            m_WorstFrameSnap.clear();
            m_PendingDumpContext.clear();
        }
    }

    if (!doDump)
        return false;

    if (!logLine)
        return false;

    if (!pendingContext.empty())
    {
        logLine(pendingContext);
    }

    // Sort worst frame snapshot by total time (descending).
    std::sort(worstSnap.begin(), worstSnap.end(),
              [](const auto& a, const auto& b) { return a.second.totalMs > b.second.totalMs; });

    {
        char hdr[256];
        std::snprintf(hdr, sizeof(hdr),
                      "[CPU Profiler] ---- worst frame since last dump (score=%.2f ms, max=%s %.2f ms) ----",
                      worstScore,
                      worstMaxName.empty() ? "<none>" : worstMaxName.c_str(),
                      worstMaxMs);
        logLine(hdr);
    }

    // Limit output by default to keep logs usable; can be overridden with
    // GE_CPU_PROF_MAX_LINES (e.g., 200). Use 0 to print all.
    std::size_t maxLines = 64;
    if (const char* env = std::getenv("GE_CPU_PROF_MAX_LINES"))
    {
        if (env[0] != '\0')
        {
            char* end = nullptr;
            const long v = std::strtol(env, &end, 10);
            if (end != env && v >= 0)
            {
                maxLines = (std::size_t)v;
            }
        }
    }

    const std::size_t n = (maxLines == 0) ? worstSnap.size() : std::min<std::size_t>(worstSnap.size(), maxLines);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& [name, s] = worstSnap[i];
        // Format without iostream (avoid allocations); caller can provide a logger that formats.
        // We emit a simple line that can be pasted back to the assistant.
        // Example: "  12.34 ms  x120  TransformTool.ApplyTranslationDelta"
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %7.2f ms  x%u  %.*s",
                      s.totalMs,
                      (unsigned)s.count,
                      (int)name.size(),
                      name.data());
        logLine(buf);
    }
    if (worstSnap.size() > n)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  ... (%zu more)", worstSnap.size() - n);
        logLine(buf);
    }
    logLine("[CPU Profiler] ------------------------------------------------");
    return true;
}

void CpuProfiler::AddSample(std::string_view name, double ms, bool offMainThread)
{
    if (!IsEnabled())
        return;
    if (name.empty())
        return;
    std::scoped_lock lock(m_Mutex);
    Sample& s = m_FrameSamples[name];
    s.totalMs += ms;
    s.count += 1;
    if (offMainThread)
        s.offMainCount += 1;
}

void CpuProfiler::CopyFrameSamples(std::vector<std::pair<std::string_view, Sample>>& out)
{
    out.clear();
    std::scoped_lock lock(m_Mutex);
    out.reserve(m_LastFrameSamples.size());
    for (const auto& kv : m_LastFrameSamples)
    {
        out.emplace_back(kv.first, kv.second);
    }
}

void CpuProfiler::FlattenTreeInto(const CallNode& node, int depth, std::vector<TreeRow>& out)
{
    std::vector<const CallNode*> sorted;
    sorted.reserve(node.children.size());
    for (const auto& c : node.children)
        sorted.push_back(c.get());
    std::sort(sorted.begin(), sorted.end(),
              [](const CallNode* a, const CallNode* b) { return a->totalMs > b->totalMs; });

    for (const auto* child : sorted)
    {
        double childrenSum = 0.0;
        for (const auto& gc : child->children)
            childrenSum += gc->totalMs;
        TreeRow row;
        row.name.assign(child->name.data(), child->name.size());
        row.totalMs = child->totalMs;
        row.selfMs = child->totalMs - childrenSum;
        if (row.selfMs < 0.0)
            row.selfMs = 0.0;
        row.callCount = child->callCount;
        row.depth = depth;
        out.push_back(std::move(row));
        FlattenTreeInto(*child, depth + 1, out);
    }
}

#endif // GE_ENABLE_CPU_PROFILING

} // namespace GameEngine::Profiling
