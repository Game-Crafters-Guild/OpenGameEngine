#pragma once

#include <cstddef>

namespace GameEngine {
namespace ECS {

struct QueryPolicy {
    // Default minimum batch size for `Parallel` (per-entity callback). Smaller
    // batches keep threads saturated when per-entity work is non-trivial; the
    // job-system overhead amortizes over the entity callback cost.
    std::size_t MinBatchSizeDefault = 1000;

    // Default minimum batch size for `ParallelBatchEach` (per-batch callback,
    // typical SIMD/memory-bound workloads). A measured sweep on 1M entities
    // running a SIMD FMA kernel across 32 cores shows this default is ~6x
    // faster than sharing MinBatchSizeDefault=1000:
    //   minBatch=1000:   2.173 ms  (shared default)
    //   minBatch=10000:  0.361 ms  (this default)
    //   minBatch=25000:  0.263 ms
    //   minBatch=100000: 0.216 ms  (diminishing returns)
    // 10000 sits at ~60% of best-case while keeping enough task parallelism for
    // smaller populations (100K+ entities still split across all workers).
    std::size_t MinBatchSizeDefaultBatch = 10000;

    // Backpressure control. Re-tuned 2026-07-10 against the HONEST pool
    // signals (Normal-lane GetApproximateQueueSize / GetPendingTasksApprox —
    // the original constants were calibrated when those included phantom and
    // Background counts). Measured basis: Tests/ECS/BackpressureRetuneBench
    // (GE_BACKPRESSURE_BENCH_FULL=1), Release, 8-worker pool, 3 full runs,
    // interleaved config sweep, 100k-entity waves under a 3-thread
    // competing-wave storm (the self-pressure regime — the only regime the
    // constants can reach; see the floor comment below):
    //
    //   external-caller wave (parks):     off=7.8ms  floor4096=4.5ms
    //                                     floor32768=1.8ms   (medians of 3)
    //   worker-caller wave (participates): off=1.0ms floor4096=2.3ms(!)
    //                                     floor32768=1.0ms, and p99 tails
    //                                     4096: 11-29ms vs 32768: 2.7-3.0ms
    //
    // Re-verified 2026-07-11 after the #363 global-queue fairness fix
    // (per-worker consumer tokens; the 2026-07-10 numbers above were taken
    // on the pre-fix queue). Ranking unchanged — wave-storm 100k, same
    // bench, medians of 3 full runs:
    //   external: off=2.9ms floor4096=3.0ms floor8192=2.6ms floor32768=1.15ms
    //   worker:   off=1.8ms floor4096=2.1ms floor8192=2.4ms floor32768=1.08ms
    // Fairness removed 4096's old 11-29ms worker tail (now ~3.4ms p99);
    // 32768 p99 is 2.2-2.4ms and it remains the only floor that beats
    // backpressure-off in both call shapes.
    bool EnableBackpressure = true;
    // If queue size exceeds workerCount * QueuePressureFactor, increase batch
    // size. Engagement at this trigger correlated correctly with the regime
    // where coarsening matters (competing waves, avgQ 30-200 on 8 workers);
    // the bench gave no basis for moving it.
    std::size_t QueuePressureFactor = 8;
    // If pending tasks exceed workerCount * PendingPressureFactor, increase batch size
    std::size_t PendingPressureFactor = 16;

    // When under backpressure, raise minBatch at least to this value.
    // 32768 (was 4096) is the measured-best floor in BOTH call shapes: under
    // a wave storm it beat 4096 by 2.5x for parked external callers and
    // removed the participating shape's 2.3x median / 10x tail penalty —
    // 4096 was the worst measured middle ground (fine enough to keep every
    // wave contending in the global-queue scan window, coarse enough to
    // wreck the participant's load balance). At 32768 a pressured 100k-
    // entity wave is ~4 envelopes (near-serial is the right answer while
    // the pool is genuinely contended); quiet-pool behavior is unchanged
    // (the floor applies only past the trigger). Note: the floor now also
    // coarsens ParallelBatchEach under pressure (its 10000 default was
    // above the old floor) — consistent with its own sweep, where coarser
    // batches measured faster throughout.
    //
    // What the constants still cannot improve (re-measured 2026-07-11, same
    // bench): under a sustained external Normal-lane flood the wave no
    // longer starves — #363's per-worker consumer tokens made the global
    // MPMC dequeue fair, and every previously-STARVED flood regime now
    // completes at every floor — but wall time there is set by fair sharing
    // with the flood, not by batch size: saturated-flood medians are a wash
    // across off/2048/4096/8192/32768 (32768 only trims the external p99
    // tail, ~42ms -> ~26ms), and in the marginal 'engaged' flood regime no
    // floor beats leaving backpressure off (the flood holds the workers
    // either way). The floor earns its keep in the self-pressure wave-storm
    // regime above.
    std::size_t PressuredMinBatchFloor = 32768;
};

} // namespace ECS
} // namespace GameEngine



// Enable simple scheduler diagnostics
#ifndef ECS_SCHEDULER_DIAGNOSTICS
#define ECS_SCHEDULER_DIAGNOSTICS 0
#endif
