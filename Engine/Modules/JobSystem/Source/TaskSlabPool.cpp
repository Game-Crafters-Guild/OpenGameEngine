#include "JobSystem/TaskSlabPool.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 6326 6011)
#endif

#if __has_include(<concurrentqueue/moodycamel/concurrentqueue.h>)
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#elif __has_include(<concurrentqueue/concurrentqueue.h>)
#include <concurrentqueue/concurrentqueue.h>
#elif __has_include(<concurrentqueue.h>)
#include <concurrentqueue.h>
#else
#error "moodycamel concurrentqueue headers not found - ensure vcpkg concurrentqueue package is installed"
#endif

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace JobSystem::Detail
{

namespace
{

struct SlabStats
{
    std::atomic<uint64> HeapAllocs{0};
    std::atomic<uint64> PoolReuses{0};
    std::atomic<uint64> HeapFrees{0};
    std::atomic<int64> Live{0};
    std::atomic<int64> LiveHighWater{0};
};

// Trivially destructible (atomics of integers), so reads/writes during
// static destruction are safe: the storage persists and no destructor runs.
SlabStats s_Stats;

using SlabFreelist = moodycamel::ConcurrentQueue<void*>;

SlabFreelist& Freelist()
{
    // IMMORTAL by design: worker threads free envelopes during static
    // destruction (thread_local teardown, other modules' static pools) — a
    // destructed freelist would be a use-after-free, so it is heap-allocated
    // once and never destroyed (the OS reclaims it at process exit).
    //
    // The constructed capacity is the retention cap: try_enqueue never
    // allocates new blocks, so the freelist can never hold more than
    // kTaskSlabPoolCapacity slabs (512KB) — overflow frees go to the heap.
    static SlabFreelist* const s_Freelist = new SlabFreelist(kTaskSlabPoolCapacity);
    return *s_Freelist;
}

void UpdateLiveHighWater(int64 live)
{
    int64 seen = s_Stats.LiveHighWater.load(std::memory_order_relaxed);
    while (live > seen &&
           !s_Stats.LiveHighWater.compare_exchange_weak(seen, live, std::memory_order_relaxed))
    {
    }
}

} // namespace

void* AcquireTaskSlab()
{
    const int64 live = s_Stats.Live.fetch_add(1, std::memory_order_relaxed) + 1;
    UpdateLiveHighWater(live);

    void* slab = nullptr;
    if (Freelist().try_dequeue(slab))
    {
        s_Stats.PoolReuses.fetch_add(1, std::memory_order_relaxed);
        return slab;
    }

    // Freelist empty (cold start, or every slab is in flight). moodycamel's
    // try_dequeue can also fail spuriously under concurrent consumers; both
    // cases take a plain heap allocation, which the freelist absorbs back on
    // release — the pool converges after warmup.
    s_Stats.HeapAllocs.fetch_add(1, std::memory_order_relaxed);
    return ::operator new(kTaskSlabSize);
}

void AcquireTaskSlabsBulk(void** slabs, size_t count)
{
    const int64 live =
        s_Stats.Live.fetch_add(static_cast<int64>(count), std::memory_order_relaxed) +
        static_cast<int64>(count);
    UpdateLiveHighWater(live);

    const size_t fromPool = Freelist().try_dequeue_bulk(slabs, count);
    if (fromPool > 0)
    {
        s_Stats.PoolReuses.fetch_add(fromPool, std::memory_order_relaxed);
    }
    if (fromPool < count)
    {
        s_Stats.HeapAllocs.fetch_add(count - fromPool, std::memory_order_relaxed);
        for (size_t i = fromPool; i < count; ++i)
        {
            slabs[i] = ::operator new(kTaskSlabSize);
        }
    }
}

void ReleaseTaskSlab(void* slab) noexcept
{
    if (slab == nullptr)
    {
        return;
    }

    s_Stats.Live.fetch_sub(1, std::memory_order_relaxed);

    // Non-allocating enqueue: bounded retention (see Freelist()), and safe
    // from noexcept context — on a full freelist the slab goes back to the
    // heap.
    if (Freelist().try_enqueue(slab))
    {
        return;
    }

    s_Stats.HeapFrees.fetch_add(1, std::memory_order_relaxed);
    ::operator delete(slab);
}

TaskSlabStatsSnapshot GetTaskSlabStatsForTests()
{
    return TaskSlabStatsSnapshot{
        s_Stats.HeapAllocs.load(std::memory_order_relaxed),
        s_Stats.PoolReuses.load(std::memory_order_relaxed),
        s_Stats.HeapFrees.load(std::memory_order_relaxed),
        s_Stats.Live.load(std::memory_order_relaxed),
        s_Stats.LiveHighWater.load(std::memory_order_relaxed),
    };
}

} // namespace JobSystem::Detail
