#pragma once

// The ParallelFor overload for fine-grained ranges, and ParallelSort, on the
// unit form of ParallelFor (WorkStealingThreadPool.h).
//
// Both are callable from any thread, a pool worker included: the caller
// claims chunks like a helper, then waits only for the chunks other threads
// have started, never for a helper still queued. The width is
// min(workers + 1, chunks) threads (the caller plus at most kStackBatch
// helpers), helpers publish at the class of the body that forks, and a call
// makes no heap allocation at steady state. A chunk that throws stops the
// run (chunks not yet started do not run) and its exception is rethrown on
// the caller once the chunks in flight have finished. With no pool, or a
// range at or under its batch size, the work runs sequentially on the caller.

#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <type_traits>

namespace JobSystem
{

/**
 * @brief The chunked overload: run `body(chunkBegin, chunkEnd)` over
 * [0, count) split into at most min(workers + 1, ceil(count / minBatchSize),
 * 64) chunks of equal size, one unit of the unit form per chunk; a range at or
 * under `minBatchSize` runs on the caller. Returns once every chunk has run.
 * For work too
 * fine to claim one element at a time, or a body that sets up per-chunk
 * state.
 * @throws The first exception a chunk threw (see the file comment).
 */
template <typename Body>
    requires std::invocable<Body&, size_t, size_t>
void ParallelFor(WorkStealingThreadPool* pool, size_t count, Body&& body, size_t minBatchSize)
{
    if (count == 0)
        return;

    minBatchSize = std::max<size_t>(1, minBatchSize);
    if (!pool || count <= minBatchSize)
    {
        body(size_t{0}, count);
        return;
    }

    const size_t workerCount = std::max<size_t>(1, pool->GetWorkerCount());
    const size_t lanes = std::min({workerCount + 1, (count + minBatchSize - 1) / minBatchSize,
                                   WorkStealingThreadPool::kStackBatch});
    const size_t chunkSize = (count + lanes - 1) / lanes;
    const size_t chunkCount = (count + chunkSize - 1) / chunkSize;

    ParallelFor(pool, chunkCount,
                [&body, count, chunkSize](size_t chunk)
                {
                    const size_t chunkBegin = chunk * chunkSize;
                    body(chunkBegin, std::min(chunkBegin + chunkSize, count));
                });
}

/**
 * @brief Sort [begin, end) with `comp`: the range is split into one chunk per
 * worker, the chunks are sorted in parallel, then merged on the caller.
 * Ranges under `threshold` elements are sorted sequentially.
 * @throws The first exception a chunk's sort threw (see the file comment).
 */
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

    if (!pool || count < threshold)
    {
        std::sort(begin, end, comp);
        return;
    }

    const size_t workerCount = std::max<size_t>(1, pool->GetWorkerCount());
    const size_t chunkCount = std::min({workerCount, count, WorkStealingThreadPool::kStackBatch});
    const size_t chunkSize = (count + chunkCount - 1) / chunkCount;
    const size_t sortedChunks = (count + chunkSize - 1) / chunkSize;

    // Phase 1: sort each chunk; the caller sorts chunks beside the helpers.
    const auto sortChunk = [begin, count, chunkSize, &comp](size_t chunk)
    {
        const size_t chunkOffset = chunk * chunkSize;
        std::sort(std::next(begin, static_cast<std::ptrdiff_t>(chunkOffset)),
                  std::next(begin, static_cast<std::ptrdiff_t>(std::min(chunkOffset + chunkSize, count))), comp);
    };
    ParallelFor(pool, sortedChunks, sortChunk);

    // Phase 2: merge adjacent sorted chunks on the calling thread.
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

/** @brief ParallelSort with std::less. */
template <typename Iter>
void ParallelSort(WorkStealingThreadPool* pool, Iter begin, Iter end, size_t threshold = 5000)
{
    ParallelSort(pool, begin, end,
                 std::less<typename std::iterator_traits<Iter>::value_type>{}, threshold);
}

} // namespace JobSystem
