// Slice-6 allocation-diet tests: pooled task slabs (TaskSlabPool), the
// oversized-closure heap fallback, and the Submit-path cost/alloc microbench
// that feeds the #242 perf-threshold re-baseline.
//
// Static-teardown safety is NOT unit-testable from here (it needs envelope
// frees during static destruction, after gtest has unwound); the reasoning
// lives on the immortal freelist in TaskSlabPool.cpp and is code-review
// verified: the freelist is a function-local static POINTER to a
// heap-allocated queue that is never deleted, and the stats block is
// trivially destructible, so ReleaseTaskSlab is safe from any thread at any
// point of process teardown.

#include "HeldSlabFreelist.h"

#include "JobSystem/TaskSlabPool.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Memory/AllocationCountScope.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{

using SteadyClock = std::chrono::steady_clock;

bool WaitForCount(const std::atomic<size_t>& counter, size_t expected, int timeoutSeconds)
{
    const auto deadline = SteadyClock::now() + std::chrono::seconds(timeoutSeconds);
    while (counter.load(std::memory_order_relaxed) < expected)
    {
        if (SteadyClock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// Acquires kTaskSlabPoolCapacity slabs, then releases them all on this thread.
void AcquireThenReleaseACapOfSlabs()
{
    std::vector<void*> slabs(JobSystem::kTaskSlabPoolCapacity);
    for (void*& slab : slabs)
    {
        slab = JobSystem::Detail::AcquireTaskSlab();
    }
    for (void* slab : slabs)
    {
        JobSystem::Detail::ReleaseTaskSlab(slab);
    }
}

} // namespace

// 8 submitters × 100k pooled fire-and-forget tasks with worker-side frees —
// the freelist's MPMC shape (allocate on submitters, free on workers).
// Submission is throttled to a bounded in-flight window so "cap + in-flight"
// is a meaningful high-water bound.
TEST(TaskSlabPoolTest, PoolReuseChurn)
{
    constexpr size_t kSubmitters = 8;
    constexpr size_t kTasksPerSubmitter = 100'000;
    constexpr size_t kTotalTasks = kSubmitters * kTasksPerSubmitter;
    constexpr size_t kInFlightWindow = 8192;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    JobSystem::WorkStealingThreadPool pool(4);

    std::atomic<size_t> submitted{0};
    std::atomic<size_t> executed{0};

    const auto before = JobSystem::Detail::GetTaskSlabStatsForTests();

    {
        std::vector<std::thread> threads;
        threads.reserve(kSubmitters);
        for (size_t s = 0; s < kSubmitters; ++s)
        {
            threads.emplace_back([&] {
                for (size_t i = 0; i < kTasksPerSubmitter; ++i)
                {
                    // Bounded in-flight window: keeps live envelopes (and
                    // therefore the slab high-water) bounded regardless of
                    // worker/submitter speed ratio.
                    while (submitted.load(std::memory_order_relaxed) -
                               executed.load(std::memory_order_relaxed) >=
                           kInFlightWindow)
                    {
                        std::this_thread::yield();
                    }
                    submitted.fetch_add(1, std::memory_order_relaxed);
                    pool.EnqueueWork(
                        [&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
                }
            });
        }
        for (auto& t : threads)
        {
            t.join();
        }
    }

    ASSERT_TRUE(WaitForCount(executed, kTotalTasks, 120)) << "churn did not drain";

    // The executed counter increments inside Execute(), but the slab is
    // released only when the envelope's UniquePtr dies at the worker loop's
    // scope exit — a preempted worker can hold its last slab past the count.
    // Shutdown() joins the workers, making the Live equality deterministic
    // (audit fix: this suite must not contain its own racy tripwire).
    pool.Shutdown();

    const auto after = JobSystem::Detail::GetTaskSlabStatsForTests();
    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);

    const auto heapAllocs = after.HeapAllocs - before.HeapAllocs;
    const auto reuses = after.PoolReuses - before.PoolReuses;
    std::printf("[slab] churn: %zu tasks, heapAllocs=%llu reuses=%llu heapFrees=%llu "
                "liveHW=%lld (before %lld)\n",
                kTotalTasks, static_cast<unsigned long long>(heapAllocs),
                static_cast<unsigned long long>(reuses),
                static_cast<unsigned long long>(after.HeapFrees - before.HeapFrees),
                static_cast<long long>(after.LiveHighWater),
                static_cast<long long>(before.LiveHighWater));

    // Every envelope died on execution: net-zero live-slab growth.
    EXPECT_EQ(after.Live, before.Live);

    // High-water ≤ in-flight window + workers-in-flight + slack (the window
    // check is racy by one task per submitter). Global monotonic counter, so
    // compare against the pre-test mark too.
    const long long highWaterBound =
        static_cast<long long>(kInFlightWindow + kSubmitters + pool.GetWorkerCount() + 64);
    EXPECT_LE(after.LiveHighWater,
              std::max(before.LiveHighWater, before.Live + highWaterBound));

    // After warmup the freelist serves (nearly) everything: heap allocations
    // are bounded by the in-flight window (each concurrently-live envelope
    // may need a distinct slab once) plus spurious try_dequeue-miss noise —
    // NOT by the task count. 90%+ reuse is the regression tripwire. The
    // ratio is calibrated on the Windows bench box; Apple Silicon's higher
    // try_dequeue miss rate sits at ~85-92% and crosses the line under
    // whole-suite load, so off Windows the ratios are REPORT-only (the Live
    // and high-water invariants above assert everywhere).
#if defined(_WIN32)
    EXPECT_GE(reuses, (kTotalTasks * 9) / 10);
    EXPECT_LE(heapAllocs, kTotalTasks / 10);
#endif
}

// A closure too large for a slab takes the compile-time heap branch of
// MakeTaskEnvelope and still runs. Both branches of the if constexpr are
// therefore exercised by this suite (every other test's closures are pooled).
TEST(TaskSlabPoolTest, OversizedClosureFallbackStillRuns)
{
    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    JobSystem::WorkStealingThreadPool pool(2);

    struct BigPayload
    {
        char Bytes[192];
    };
    BigPayload payload{};
    payload.Bytes[0] = 42;
    payload.Bytes[191] = 7;

    std::atomic<size_t> result{0};
    auto oversized = [payload, &result] {
        result.fetch_add(static_cast<size_t>(payload.Bytes[0]) +
                             static_cast<size_t>(payload.Bytes[191]),
                         std::memory_order_relaxed);
    };
    static_assert(sizeof(oversized) > JobSystem::kTaskSlabSize,
                  "closure must exceed the slab size to exercise the heap branch");

    const auto before = JobSystem::Detail::GetTaskSlabStatsForTests();
    pool.EnqueueWork(oversized);
    ASSERT_TRUE(WaitForCount(result, 49, 30)) << "oversized closure never ran";

    // The oversized envelope never touched the slab pool.
    const auto after = JobSystem::Detail::GetTaskSlabStatsForTests();
    EXPECT_EQ(after.HeapAllocs - before.HeapAllocs, 0u);
    EXPECT_EQ(after.PoolReuses - before.PoolReuses, 0u);

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    pool.Shutdown();
}

// Oversized batch closures take the same heap branch through
// EnqueueWorkBatch's staging loop.
TEST(TaskSlabPoolTest, OversizedBatchFallbackStillRuns)
{
    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    JobSystem::WorkStealingThreadPool pool(2);

    struct BigPayload
    {
        char Bytes[192];
    };

    struct BigChunk
    {
        BigPayload Payload{};
        std::atomic<size_t>* Ran = nullptr;
        void operator()() const { Ran->fetch_add(1, std::memory_order_relaxed); }
    };
    static_assert(sizeof(BigChunk) > JobSystem::kTaskSlabSize);

    std::atomic<size_t> ran{0};
    constexpr size_t kBatch = 32;
    std::vector<BigChunk> chunks(kBatch);
    for (auto& c : chunks)
    {
        c.Ran = &ran;
    }

    pool.EnqueueWorkBatch(chunks.data(), chunks.size());
    ASSERT_TRUE(WaitForCount(ran, kBatch, 30));

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    pool.Shutdown();
}

// One thread releasing a full cap's worth of slabs keeps nearly all of them:
// the freelist's per-thread sub-queue indexes the whole cap, so retention
// does not depend on how many threads release. What it cannot reach are the
// blocks other threads' sub-queues hold partly used (one block, 32 slabs,
// each), so the bound leaves a quarter of the cap for them; a sub-queue
// limited to the default index would send three quarters to the heap.
TEST(TaskSlabPoolTest, OneReleasingThreadFillsTheFreelistToItsCap)
{
    const GameEngine::Tests::HeldSlabFreelist held;
    const auto before = JobSystem::Detail::GetTaskSlabStatsForTests();
    std::thread releaser(AcquireThenReleaseACapOfSlabs);
    releaser.join();
    const auto after = JobSystem::Detail::GetTaskSlabStatsForTests();
    EXPECT_LE(after.HeapFrees - before.HeapFrees, JobSystem::kTaskSlabPoolCapacity / 4)
        << "slabs released by one thread went back to the heap";
}

// Submit-path microbench (slice 6 gate: ~1 pooled alloc + ≤1 mutex on the
// fast path; feeds the #242 re-baseline table). Reports caller-side publish
// cost, end-to-end throughput, and end-to-end heap-allocations per task
// (process-wide operator-new count across submit→drain, workers included).
TEST(JobSystemBench, BENCHMARK_SubmitPathCost)
{
    constexpr size_t kWarmup = 20'000;
    constexpr size_t kTasks = 50'000;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    JobSystem::WorkStealingThreadPool pool(8);
    std::atomic<size_t> executed{0};

    for (size_t i = 0; i < kWarmup; ++i)
    {
        pool.Submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
    }
    ASSERT_TRUE(WaitForCount(executed, kWarmup, 60));

    executed.store(0, std::memory_order_relaxed);
    const auto slabBefore = JobSystem::Detail::GetTaskSlabStatsForTests();
    const GameEngine::Memory::AllocationCountScope allocations(GameEngine::Memory::CountWindow::Process);

    const auto submitStart = SteadyClock::now();
    for (size_t i = 0; i < kTasks; ++i)
    {
        pool.Submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
    }
    const auto submitEnd = SteadyClock::now();
    const auto newAfterSubmit = allocations.Count();

    ASSERT_TRUE(WaitForCount(executed, kTasks, 60));
    const auto doneEnd = SteadyClock::now();

    const auto newAfterDrain = allocations.Count();
    const auto slabAfter = JobSystem::Detail::GetTaskSlabStatsForTests();
    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);

    const double submitNs = std::chrono::duration<double, std::nano>(submitEnd - submitStart)
                                .count() /
                            static_cast<double>(kTasks);
    const double endToEndMs =
        std::chrono::duration<double, std::milli>(doneEnd - submitStart).count();
    const double tasksPerSec = static_cast<double>(kTasks) / (endToEndMs / 1000.0);
    // Submit-window count approximates caller-side allocs (workers overlap
    // the window and add their execute-side allocs to it — upper bound).
    const double allocsPerTaskSubmit =
        static_cast<double>(newAfterSubmit) / static_cast<double>(kTasks);
    const double allocsPerTaskTotal =
        static_cast<double>(newAfterDrain) / static_cast<double>(kTasks);
    const double pooledPerTask =
        static_cast<double>((slabAfter.PoolReuses + slabAfter.HeapAllocs) -
                            (slabBefore.PoolReuses + slabBefore.HeapAllocs)) /
        static_cast<double>(kTasks);

    std::printf("[bench] Submit fast path: %.0f ns/submit (caller), %.0f tasks/sec end-to-end\n",
                submitNs, tasksPerSec);
    std::printf("[bench]   heap allocs/task: %.2f submit-window, %.2f end-to-end; "
                "pooled slabs/task: %.2f\n",
                allocsPerTaskSubmit, allocsPerTaskTotal, pooledPerTask);

    // Diet gates (2x the measured post-diet cost — headroom, not aspiration).
    // Measured on the diet build (DebugFast, warm): 1.01 heap allocs/task
    // end-to-end (the registry map node) + 1.00 pooled slab. Pre-diet this
    // path cost 4.00 allocs/task (envelope, TaskData, map node, cleanup
    // churn).
    EXPECT_EQ(executed.load(), kTasks);
    EXPECT_LE(pooledPerTask, 1.05); // at most one pooled envelope per task
    EXPECT_LE(allocsPerTaskTotal, 2.0);
}

// Fire-and-forget alloc floor: after warmup an EnqueueWork task is ONE
// recycled slab and zero heap allocations.
TEST(JobSystemBench, BENCHMARK_EnqueueWorkAllocFloor)
{
    constexpr size_t kWarmup = 10'000;
    constexpr size_t kTasks = 100'000;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    JobSystem::WorkStealingThreadPool pool(4);
    std::atomic<size_t> executed{0};

    for (size_t i = 0; i < kWarmup; ++i)
    {
        pool.EnqueueWork([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
    }
    ASSERT_TRUE(WaitForCount(executed, kWarmup, 60));

    executed.store(0, std::memory_order_relaxed);
    const GameEngine::Memory::AllocationCountScope allocations(GameEngine::Memory::CountWindow::Process);
    const auto submitStart = SteadyClock::now();
    for (size_t i = 0; i < kTasks; ++i)
    {
        pool.EnqueueWork([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
    }
    ASSERT_TRUE(WaitForCount(executed, kTasks, 60));
    const auto doneEnd = SteadyClock::now();
    const auto newAfter = allocations.Count();

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);

    const double allocsPerTask =
        static_cast<double>(newAfter) / static_cast<double>(kTasks);
    const double tasksPerSec =
        static_cast<double>(kTasks) /
        (std::chrono::duration<double>(doneEnd - submitStart).count());
    std::printf("[bench] EnqueueWork floor: %.3f heap allocs/task end-to-end, %.0f tasks/sec\n",
                allocsPerTask, tasksPerSec);

    EXPECT_EQ(executed.load(), kTasks);
    // Slab reuse serves the envelope; nothing else on this path allocates.
    // Slack covers freelist warm-up misses and spurious try_dequeue failures.
    EXPECT_LE(allocsPerTask, 0.1);
}
