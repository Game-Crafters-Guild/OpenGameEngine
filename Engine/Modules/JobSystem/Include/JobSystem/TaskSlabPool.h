#pragma once

// TaskSlabPool — pooled fixed-size slabs for task envelopes (overhaul slice 6).
//
// Fire-and-forget and Submit-path envelopes are small (slim TaskBase +
// closure) and have a perfectly MPMC lifecycle: allocated on submitter
// threads, freed on worker threads after execution. Backing them with a
// moodycamel<void*> freelist removes the per-task heap round-trip that
// dominated EnqueueWork's publication cost (spec §1.3) — allocate = freelist
// pop, free = freelist push, MPMC by construction.
//
// Closure-size audit (every envelope that flows through EnqueueWork /
// EnqueueWorkBatch / Run / Submit(F&&)):
//   ParallelFor helper          16B  (RunBlock* + the lane's unstarted counter*)
//   ParallelFor run block      ≤128B (one slab; static_assert in ParallelForCore.cpp)
//   Run() pool stub              8B  (JobCounter::TaggedJobsRef)
//   JobCounter::Job<F>          16B + sizeof(F); typical Run captures ≤64B
//   Submit(F&&) lambdas         typical captures ≤64B (previously verified)
// Envelope bases are ≤40B (vptr + TaskId + flags [+ inline TaskData]), so
// the 128-byte slab covers every real caller with headroom; oversized
// closures fall back to the plain heap AT COMPILE TIME (MakeTaskEnvelope's
// if constexpr — both branches are real code paths, exercised by tests).

#include "JobSystem/Types.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <utility>

namespace JobSystem
{

// One slab per envelope. 128 bytes fits every in-tree closure (audit above)
// and two slabs share a 64-byte-line pair cleanly.
inline constexpr size_t kTaskSlabSize = 128;

// Freelist retention cap: 4096 slabs × 128B = 512KB ceiling. Frees beyond
// the cap fall through to ::operator delete, so a burst can never pin more
// than 512KB of recycled slabs.
inline constexpr size_t kTaskSlabPoolCapacity = 4096;

namespace Detail
{

/**
 * @brief Pop a recycled slab or heap-allocate a fresh kTaskSlabSize block.
 * Called on submitter threads (the freelist's consumers).
 */
void* AcquireTaskSlab();

/**
 * @brief Bulk acquire for batch staging (EnqueueWorkBatch): ONE bulk
 * freelist op instead of `count` singles — the per-item MPMC dequeue was
 * measured to double the 200-burst caller cost (22.3 -> 47us). Fills
 * `slabs[0..count)`: freelist bulk first, fresh heap blocks for the
 * remainder.
 */
void AcquireTaskSlabsBulk(void** slabs, size_t count);

/**
 * @brief Return a slab to the freelist; falls back to ::operator delete when
 * the freelist is at capacity. Non-allocating (try_enqueue) and noexcept —
 * called from worker threads, including during static destruction.
 */
void ReleaseTaskSlab(void* slab) noexcept;

/**
 * @brief Add `count` fresh heap slabs to the freelist, up to its retention
 * cap. A pool provisions the slabs its ParallelFor helpers can hold at once,
 * so its forks take no heap allocation once it exists.
 */
void ProvisionTaskSlabs(size_t count);

/** Test hooks (PoolReuseChurn / microbench). Counters are process-global. */
struct TaskSlabStatsSnapshot
{
    uint64 HeapAllocs;    // freelist misses -> ::operator new
    uint64 PoolReuses;    // freelist hits
    uint64 HeapFrees;     // freelist at capacity -> ::operator delete
    int64 Live;           // slabs acquired and not yet released
    int64 LiveHighWater;  // high-water mark of Live
};
TaskSlabStatsSnapshot GetTaskSlabStatsForTests();

} // namespace Detail

/**
 * @brief True when envelope E is served by the slab pool (fits a slab, not
 * over-aligned); false routes to the plain heap. The single compile-time
 * switch behind MakeTaskEnvelope and the batch staging path.
 */
template <typename E>
inline constexpr bool kIsSlabPooled =
    sizeof(E) <= kTaskSlabSize && alignof(E) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__;

/**
 * @brief Class-level pooled allocation for envelope type E.
 *
 * Derives from E adding no members, so sizeof(PooledEnvelope<E>) ==
 * sizeof(E); the deleting destructor of THIS most-derived type is what
 * routes a `delete base_ptr` (UniquePtr through any base with a virtual
 * destructor) back to the slab pool.
 */
template <typename E>
class PooledEnvelope final : public E
{
  public:
    using E::E;

    static void* operator new(size_t size)
    {
        static_assert(kIsSlabPooled<PooledEnvelope>,
                      "envelope exceeds the slab size — MakeTaskEnvelope must route it to the heap");
        assert(size == sizeof(PooledEnvelope));
        (void)size;
        return Detail::AcquireTaskSlab();
    }

    // Placement form for bulk-acquired slabs (MakeTaskEnvelopeInSlab).
    // Declaring ANY class-level operator new hides the global placement
    // form, so it must be re-declared here. The matching placement delete
    // (ctor-throw unwind) returns the slab to the pool.
    static void* operator new(size_t, void* slab) noexcept { return slab; }
    static void operator delete(void* slab, void*) noexcept { Detail::ReleaseTaskSlab(slab); }

    static void operator delete(void* slab) noexcept { Detail::ReleaseTaskSlab(slab); }
    static void operator delete(void* slab, size_t) noexcept { Detail::ReleaseTaskSlab(slab); }
};

/**
 * @brief Allocate envelope E from the slab pool when it fits, else from the
 * heap — decided at compile time on the envelope's size (spec §2.2). The
 * returned UniquePtr<E> deletes through E's virtual destructor, reaching the
 * pooled operator delete via PooledEnvelope's deleting destructor.
 */
template <typename E, typename... Args>
UniquePtr<E> MakeTaskEnvelope(Args&&... args)
{
    if constexpr (kIsSlabPooled<E>)
    {
        return UniquePtr<E>(new PooledEnvelope<E>(std::forward<Args>(args)...));
    }
    else
    {
        // Qualified: ADL on the closure's associated namespaces would
        // otherwise pull the caller's own MakeUnique into the overload set
        // (ambiguity observed with GameEngine::MakeUnique in Debug, where
        // fatter closures take this branch).
        return JobSystem::MakeUnique<E>(std::forward<Args>(args)...);
    }
}

/**
 * @brief Construct pooled envelope E in a bulk-acquired slab
 * (Detail::AcquireTaskSlabsBulk). Deletion is identical to MakeTaskEnvelope's
 * pooled branch — the deleting destructor returns the slab to the freelist.
 * Only valid for kIsSlabPooled<E> types.
 */
template <typename E, typename... Args>
UniquePtr<E> MakeTaskEnvelopeInSlab(void* slab, Args&&... args)
{
    static_assert(kIsSlabPooled<E>, "bulk slab staging requires a slab-pooled envelope");
    return UniquePtr<E>(new (slab) PooledEnvelope<E>(std::forward<Args>(args)...));
}

} // namespace JobSystem
