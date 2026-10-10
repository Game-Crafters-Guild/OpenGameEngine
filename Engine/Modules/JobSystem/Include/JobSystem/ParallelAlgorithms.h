#pragma once

// Cross-platform parallel primitives backed by WorkStealingThreadPool.
// Replaces all uses of std::execution::par_unseq (unavailable on Apple Clang)
// and ad-hoc vector<TaskHandle> fire-and-wait patterns.
//
// Key guarantees:
//   - No dependency on <execution>
//   - No nested parallelism (jobs never spawn sub-jobs that Wait)
//   - Sequential fallback when pool is null or range is below threshold
//   - Exception-safe: user function exceptions are caught and do not hang callers
//
// Slice 5: the completion barrier is a JobCounter (chunks Decrement(), the
// caller joins via pool->Wait()). The chunks are still published as bare
// tasks through EnqueueWorkBatch — the slice-4 bulk publication path — so
// these barriers are join-only counters: they must be joined from a
// non-worker thread (Detail::AssertNotOnWorkerThread, unchanged). The
// stack-lifetime discipline that used to live in Detail::SignalCompletion
// moved into JobCounter::Decrement verbatim.

#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <type_traits>

namespace JobSystem
{

namespace Detail
{

// Assert that the calling thread is NOT a worker in the given pool.
// ParallelFor-style barriers park without participating (their chunks are
// bare pool tasks, not counter-tagged) — blocking a worker on one would
// deadlock a bounded pool. Fork-join that must wait on a worker goes through
// Run(fn, counter) + Wait(counter) instead.
inline void AssertNotOnWorkerThread([[maybe_unused]] WorkStealingThreadPool* pool)
{
    assert((!pool || pool->GetCurrentWorkerId() == SIZE_MAX) &&
           "ParallelAlgorithms: blocking wait called from a worker thread (deadlock risk)");
}

} // namespace Detail

// ---------------------------------------------------------------------------
// ParallelFor
// ---------------------------------------------------------------------------
// Splits [begin, end) into chunks of at least minBatchSize elements.
// Dispatches one job per chunk, blocks until all complete.
// Sequential fallback when pool == nullptr or range <= minBatchSize.

template <typename Func>
void ParallelFor(WorkStealingThreadPool* pool,
                 size_t begin, size_t end,
                 Func&& func,
                 size_t minBatchSize = 1024)
{
    if (begin >= end)
        return;

    const size_t count = end - begin;
    minBatchSize = std::max<size_t>(1, minBatchSize);

    // F13b: a shutting-down pool is treated as no pool — sequential fallback
    // on the caller. A shutdown that lands AFTER this check is covered by
    // EnqueueWork's self-drain: chunks always execute, the barrier below
    // always releases (B7).
    if (!pool || pool->IsShuttingDown() || count <= minBatchSize)
    {
        func(begin, end);
        return;
    }

    Detail::AssertNotOnWorkerThread(pool);

    const size_t workerCount = std::max<size_t>(1, pool->GetWorkerCount());
    // +1 lane: the caller executes the first chunk itself instead of idling
    // on the condvar, so one more chunk than workers is profitable. The
    // fan-out is additionally clamped to kStackBatch lanes so the offloaded
    // chunks (chunkCount - 1) always fit the stack staging array below.
    const size_t chunkCount = std::min({workerCount + 1,
                                        (count + minBatchSize - 1) / minBatchSize,
                                        WorkStealingThreadPool::kStackBatch});
    const size_t chunkSize = (count + chunkCount - 1) / chunkCount;

    JobCounter counter;

    // Slice 4: one homogeneous closure type per chunk, staged on the stack
    // and published with ONE EnqueueWorkBatch call (bulk queue op + bounded
    // batch wake) instead of chunkCount-1 push+notify pairs. Empty chunks
    // are simply never counted into the barrier (the pre-slice-5 code
    // pre-signaled them; only staged chunks are Add()ed below — same
    // observable semantics).
    struct Chunk
    {
        std::remove_reference_t<Func>* Fn = nullptr;
        size_t Begin = 0;
        size_t End = 0;
        JobCounter* Counter = nullptr;

        void operator()() const
        {
            try { (*Fn)(Begin, End); }
            catch (...) { Logger::Log::Error("ParallelFor: chunk threw; result dropped"); }
            Counter->Decrement();
        }
    };

    Chunk staging[WorkStealingThreadPool::kStackBatch];
    size_t staged = 0;
    for (size_t c = 1; c < chunkCount; ++c)
    {
        const size_t chunkBegin = begin + c * chunkSize;
        const size_t chunkEnd = std::min(chunkBegin + chunkSize, end);
        if (chunkBegin >= end)
            continue;

        staging[staged++] = Chunk{&func, chunkBegin, chunkEnd, &counter};
    }
    if (staged > 0)
    {
        counter.Add(static_cast<uint32>(staged)); // F16: count BEFORE publish
        pool->EnqueueWorkBatch(staging, staged);
    }

    // Caller's chunk. Exceptions are swallowed to match worker semantics —
    // a throw must not skip the barrier below (the enqueued chunks reference
    // this frame's locals).
    try { func(begin, std::min(begin + chunkSize, end)); }
    catch (...) { Logger::Log::Error("ParallelFor: caller chunk threw; result dropped"); }

    pool->Wait(counter);
}

// ---------------------------------------------------------------------------
// ParallelForEach
// ---------------------------------------------------------------------------
// Iterator-based: splits [begin, end) by distance, dispatches per-chunk.
// func is called per element.

template <typename Iter, typename Func>
void ParallelForEach(WorkStealingThreadPool* pool,
                     Iter begin, Iter end,
                     Func&& func,
                     size_t minBatchSize = 1024)
{
    const auto count = static_cast<size_t>(std::distance(begin, end));

    if (count == 0)
        return;

    minBatchSize = std::max<size_t>(1, minBatchSize);

    // F13b: sequential fallback on a shutting-down pool (see ParallelFor).
    if (!pool || pool->IsShuttingDown() || count <= minBatchSize)
    {
        for (auto it = begin; it != end; ++it)
            func(*it);
        return;
    }

    Detail::AssertNotOnWorkerThread(pool);

    const size_t workerCount = std::max<size_t>(1, pool->GetWorkerCount());
    // +1 lane: the caller executes the first chunk itself (see ParallelFor).
    // Fan-out clamped to kStackBatch lanes for the stack staging array.
    const size_t chunkCount = std::min({workerCount + 1,
                                        (count + minBatchSize - 1) / minBatchSize,
                                        WorkStealingThreadPool::kStackBatch});
    const size_t chunkSize = (count + chunkCount - 1) / chunkCount;

    JobCounter counter;

    // Slice 4: homogeneous chunk closures published via one EnqueueWorkBatch
    // call (see ParallelFor).
    struct Chunk
    {
        std::remove_reference_t<Func>* Fn = nullptr;
        Iter Begin{};
        Iter End{};
        JobCounter* Counter = nullptr;

        void operator()() const
        {
            try {
                for (auto it = Begin; it != End; ++it)
                    (*Fn)(*it);
            } catch (...) { Logger::Log::Error("ParallelForEach: chunk threw; result dropped"); }
            Counter->Decrement();
        }
    };

    Chunk staging[WorkStealingThreadPool::kStackBatch];
    size_t staged = 0;
    for (size_t c = 1; c < chunkCount; ++c)
    {
        const size_t offset = c * chunkSize;
        if (offset >= count)
            continue;
        const size_t len = std::min(chunkSize, count - offset);
        Iter chunkBegin = std::next(begin, static_cast<std::ptrdiff_t>(offset));
        Iter chunkEnd = std::next(chunkBegin, static_cast<std::ptrdiff_t>(len));

        staging[staged++] = Chunk{&func, chunkBegin, chunkEnd, &counter};
    }
    if (staged > 0)
    {
        counter.Add(static_cast<uint32>(staged)); // F16: count BEFORE publish
        pool->EnqueueWorkBatch(staging, staged);
    }

    {
        Iter callerEnd = std::next(begin, static_cast<std::ptrdiff_t>(std::min(chunkSize, count)));
        try {
            for (auto it = begin; it != callerEnd; ++it)
                func(*it);
        } catch (...) { Logger::Log::Error("ParallelForEach: caller chunk threw; result dropped"); }
    }

    pool->Wait(counter);
}

// ---------------------------------------------------------------------------
// ParallelSort
// ---------------------------------------------------------------------------
// Parallel chunk-sort + sequential merge. Does NOT use std::execution::par_unseq.
// Does NOT create nested parallelism.
//
// Strategy: split into N chunks (one per worker), sort each chunk on a worker,
// then merge on the calling thread via iterative inplace_merge.

template <typename Iter, typename Compare,
          typename = std::enable_if_t<std::is_invocable_v<Compare,
              typename std::iterator_traits<Iter>::value_type,
              typename std::iterator_traits<Iter>::value_type>>>
void ParallelSort(WorkStealingThreadPool* pool,
                  Iter begin, Iter end,
                  Compare comp,
                  size_t threshold = 5000)
{
    const auto count = static_cast<size_t>(std::distance(begin, end));

    if (count < 2)
        return;

    // F13b: sequential fallback on a shutting-down pool (see ParallelFor).
    if (!pool || pool->IsShuttingDown() || count < threshold)
    {
        std::sort(begin, end, comp);
        return;
    }

    Detail::AssertNotOnWorkerThread(pool);

    const size_t workerCount = std::max<size_t>(1, pool->GetWorkerCount());
    // All chunks are offloaded here (the caller only waits), so the fan-out
    // is clamped to kStackBatch for the stack staging array.
    const size_t chunkCount = std::min({workerCount, count,
                                        WorkStealingThreadPool::kStackBatch});
    const size_t chunkSize = (count + chunkCount - 1) / chunkCount;

    // Phase 1: Sort each chunk in parallel.
    JobCounter counter;

    // Slice 4: homogeneous chunk closures published via one EnqueueWorkBatch
    // call (see ParallelFor).
    struct Chunk
    {
        Iter Begin{};
        Iter End{};
        Compare* Comp = nullptr;
        JobCounter* Counter = nullptr;

        void operator()() const
        {
            try { std::sort(Begin, End, *Comp); }
            catch (...) { Logger::Log::Error("ParallelSort: chunk threw; result dropped"); }
            Counter->Decrement();
        }
    };

    Chunk staging[WorkStealingThreadPool::kStackBatch];
    size_t staged = 0;
    for (size_t c = 0; c < chunkCount; ++c)
    {
        const size_t chunkOffset = c * chunkSize;
        if (chunkOffset >= count)
            continue;
        Iter chunkBegin = std::next(begin, static_cast<std::ptrdiff_t>(chunkOffset));
        Iter chunkEnd = std::next(begin, static_cast<std::ptrdiff_t>(std::min(chunkOffset + chunkSize, count)));

        staging[staged++] = Chunk{chunkBegin, chunkEnd, &comp, &counter};
    }
    if (staged > 0)
    {
        counter.Add(static_cast<uint32>(staged)); // F16: count BEFORE publish
        pool->EnqueueWorkBatch(staging, staged);
    }

    pool->Wait(counter);

    // Phase 2: Merge sorted chunks on calling thread.
    // Iteratively merge adjacent pairs of chunks in-place.
    for (size_t stride = chunkSize; stride < count; stride *= 2)
    {
        for (size_t i = 0; i + stride < count; i += stride * 2)
        {
            Iter lo = std::next(begin, static_cast<std::ptrdiff_t>(i));
            Iter mid = std::next(begin, static_cast<std::ptrdiff_t>(std::min(i + stride, count)));
            Iter hi = std::next(begin, static_cast<std::ptrdiff_t>(std::min(i + stride * 2, count)));
            std::inplace_merge(lo, mid, hi, comp);
        }
    }
}

// Convenience overload with default std::less.
template <typename Iter>
void ParallelSort(WorkStealingThreadPool* pool, Iter begin, Iter end, size_t threshold = 5000)
{
    ParallelSort(pool, begin, end,
                 std::less<typename std::iterator_traits<Iter>::value_type>{}, threshold);
}

// ---------------------------------------------------------------------------
// DispatchAndWait
// ---------------------------------------------------------------------------
// Lightweight fire-and-wait: dispatches N tasks via EnqueueWorkBatch, blocks
// until all complete on a JobCounter barrier.

void DispatchAndWait(WorkStealingThreadPool* pool,
                     std::function<void()>* tasks, uint32_t count);

} // namespace JobSystem
