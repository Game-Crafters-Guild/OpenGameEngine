#pragma once

// CompileConcurrencyGate: a global admission gate that bounds how many shader
// compile tasks run concurrently on the job pool.
//
// Cold scene/model loads submit one prewarm task per material (each compiling a
// material's full variant set sequentially) plus per-material base compiles and
// draw-time variant misses. Left unbounded, ~one-task-per-material saturates
// every logical core with 10-30s shaderc compiles, so the (non-worker) main
// thread and the extraction/streaming workers starve and the editor stops
// responding to IPC for tens of seconds.
//
// The gate caps the number of in-flight compile tasks at a fixed value. It
// deliberately does NOT block a worker to enforce the cap (that would convert
// compile oversubscription into worker-pool occupancy by sleepers and starve
// extraction of worker lanes). Instead it admits up to `cap` tasks to the pool
// and holds the remainder in a CPU-side pending queue, draining it as running
// tasks finish. No worker ever sleeps waiting for a slot, and the submitting
// (main) thread never blocks — Submit returns immediately whether the task ran
// now or was queued.
//
// Owned by MaterialSystem; every MaterialSystem::SubmitTrackedPrewarm call
// routes through it. That covers the cold-load background shaderc work — variant
// prewarm, base compile, and the publish-gate variant-miss warm-up. It does NOT
// cover MaterialCompiler::RequestCompile (the editor material-inspector preview
// build), which compiles straight through BuildMaterialToShaderPackage by
// design: that path is user-paced and low-volume, not a cold-load burst.

#include "JobSystem/TaskTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

namespace GameEngine
{
namespace Engine::Renderer
{

// Physical-core-derived concurrency cap for shader compiles: enough parallelism
// to keep shaderc busy without pinning every core, leaving headroom for the main
// thread and extraction/streaming. Honors the GE_SHADER_COMPILE_CAP env override
// (an explicit positive integer) for A/B tuning without a rebuild.
uint32_t ComputeShaderCompileConcurrencyCap();

class CompileConcurrencyGate
{
  public:
    // Enqueues `work` onto the pool. `priority` selects the pool lane; the
    // gate preserves it for queued work so a background variant-miss stays off
    // the Normal lane when it is finally dispatched.
    using DispatchFn = std::function<void(std::function<void()>, JobSystem::JobPriority)>;

    CompileConcurrencyGate(uint32_t cap, DispatchFn dispatch)
        : m_Cap(cap == 0 ? 1 : cap), m_Dispatch(std::move(dispatch))
    {
    }

    // Admit `work` if fewer than `cap` tasks are running, else hold it in the
    // pending queue for its lane. Admitted work is dispatched via the injected
    // executor and, on completion, frees its slot and pumps the next pending
    // task (Background lane first). Never runs `work` inline on the calling
    // thread. If the dispatch executor throws while admitting (task-envelope
    // allocation failure), the reserved slot is released, any concurrently
    // stranded pending job is pumped, and the exception propagates.
    void Submit(std::function<void()> work, JobSystem::JobPriority priority);

    uint32_t Cap() const { return m_Cap; }
    uint32_t Running() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Running;
    }
    // Peak concurrent running count observed — the invariant the stress test
    // asserts (never exceeds Cap()).
    uint32_t HighWater() const
    {
        std::lock_guard lock(m_Mutex);
        return m_HighWater;
    }
    size_t PendingCount() const
    {
        std::lock_guard lock(m_Mutex);
        return m_PendingBackground.size() + m_PendingNormal.size();
    }

  private:
    // Wrap `work` so its completion releases the slot and pumps the queue, then
    // hand it to the dispatch executor. May throw if the executor throws.
    void DispatchTracked(std::function<void()> work, JobSystem::JobPriority priority);
    // Called from a dispatched task's completion: release one slot and promote
    // the next pending job (if any). noexcept — runs from a destructor context.
    void OnJobFinished() noexcept;

    const uint32_t m_Cap;
    DispatchFn m_Dispatch;

    mutable std::mutex m_Mutex;
    uint32_t m_Running = 0;
    uint32_t m_HighWater = 0;
    // Two lanes mirroring the pool's two-class contract instead of flattening
    // it: Background holds user-visible draw-time variant misses, Normal holds
    // bulk prewarm/base compiles. OnJobFinished drains Background first so a
    // material that a draw is waiting on jumps ahead of the prewarm backlog.
    std::deque<std::function<void()>> m_PendingBackground;
    std::deque<std::function<void()>> m_PendingNormal;
};

} // namespace Engine::Renderer
} // namespace GameEngine
