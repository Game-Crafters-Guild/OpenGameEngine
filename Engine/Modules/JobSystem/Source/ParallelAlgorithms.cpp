#include "JobSystem/ParallelAlgorithms.h"

namespace JobSystem
{

namespace
{

// Offloaded-task closure for DispatchAndWait. A named aggregate rather than a
// lambda so the dispatch loop can stage a stack ARRAY of one homogeneous type
// for EnqueueWorkBatch (lambdas are not default-constructible). The counter
// lives on the blocked caller's stack frame — JobCounter's under-mutex
// last-decrement discipline is what makes that safe.
struct DispatchChunk
{
    std::function<void()>* Task = nullptr;
    JobCounter* Counter = nullptr;

    void operator()() const
    {
        try { (*Task)(); }
        catch (...) { Logger::Log::Error("DispatchAndWait: task threw; result dropped"); }
        Counter->Decrement();
    }
};

} // namespace

void DispatchAndWait(WorkStealingThreadPool* pool,
                     std::function<void()>* tasks, uint32_t count)
{
    if (count == 0)
        return;

    // Sequential fallback when no pool is available, the pool is shutting
    // down (F13b — a late dispatch against a dead pool must not park the
    // caller on a barrier no worker will release, B7), or there is nothing to
    // hand off besides the caller's own task. A shutdown that lands after
    // this check is covered by EnqueueWork's self-drain.
    if (!pool || pool->IsShuttingDown() || count == 1)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (tasks[i])
                tasks[i]();
        }
        return;
    }

    Detail::AssertNotOnWorkerThread(pool);

    // The caller executes tasks[0] itself instead of idling on the condvar —
    // one extra lane of throughput at every fork-join site.
    JobCounter counter;

    // Slice 4: stage the offloaded tasks into runs of one homogeneous closure
    // type and publish each run with ONE EnqueueWorkBatch call (bulk queue op
    // + bounded batch wake) instead of count-1 individual push+notify pairs.
    // Null tasks are simply never counted into the barrier (pre-slice-5 code
    // pre-signaled them — same observable semantics). Each run is Add()ed
    // BEFORE its publish (F16).
    DispatchChunk staging[WorkStealingThreadPool::kStackBatch];
    size_t staged = 0;
    for (uint32_t i = 1; i < count; ++i)
    {
        if (!tasks[i])
            continue;

        staging[staged++] = DispatchChunk{&tasks[i], &counter};
        if (staged == WorkStealingThreadPool::kStackBatch)
        {
            counter.Add(static_cast<uint32>(staged));
            pool->EnqueueWorkBatch(staging, staged);
            staged = 0;
        }
    }
    if (staged > 0)
    {
        counter.Add(static_cast<uint32>(staged));
        pool->EnqueueWorkBatch(staging, staged);
    }

    if (tasks[0])
    {
        // Exceptions are swallowed to match worker semantics — a throw must
        // not skip the barrier (enqueued tasks reference this frame's stack).
        try { tasks[0](); }
        catch (...) { Logger::Log::Error("DispatchAndWait: caller task threw; result dropped"); }
    }

    pool->Wait(counter);
}

} // namespace JobSystem
