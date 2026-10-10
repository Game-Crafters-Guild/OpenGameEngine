#include <gtest/gtest.h>
#include "Logger/Logger.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "JobSystem/Types.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// Scheduling-contract tests for the post-overhaul pool (issue #242 re-baseline,
// slice 6). The pre-overhaul versions of these tests asserted per-worker work
// SPREAD (every thread does some work, load imbalance < 50%, linear scaling
// efficiency > 70%). That was never the pool's contract, and since slice 1/4
// it is explicitly NOT the behavior: producers issue a BOUNDED number of
// wakes (min(need, sleepers, 4)) and worker-side propagation only extends the
// wake front while a backlog remains, so for cheap tasks a few workers
// legitimately drain everything while the rest stay parked. That is the
// design (no thundering herd), not a defect — asserting spread rejected the
// overhaul's central scheduling decision.
//
// What IS the contract, and what these tests pin:
//   1. Every published task executes, promptly — bounded wake + propagation
//      must reach enough width that a burst drains in bounded wall time.
//   2. Throughput has a floor — the wake protocol must not strand a backlog
//      behind parked workers (that failure mode shows up as multi-ms backstop
//      rescues and craters tasks/sec by an order of magnitude).
//
// Budget basis (2026-07-10, slice-6 diet build, DebugFast, 32-thread desktop,
// idle-ish box): 50k mixed-weight Submits complete in ~130-180ms; 20k
// uniform-weight Submits in ~40-70ms end-to-end; per-width 10k-task storms
// 10-30ms. Budgets are set at >=3x the observed worst so scheduler
// regressions (stranded backlogs, lost wakeups) fail loudly while machine
// noise does not. Thresholds re-verified against the measured run recorded
// in the slice-6 PR.

namespace GameEngine::Performance {

class WorkStealingTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Error);
    }

    void TearDown() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Info);
    }
};

// Mixed-weight burst: every task completes, and end-to-end throughput stays
// above a floor that a stranded backlog cannot meet. (Successor of the old
// "RingBasedWorkStealingEfficiency" — same workload, contract-shaped
// assertions.)
TEST_F(WorkStealingTest, MixedWeightBurstDrainsPromptly) {
    const size_t numThreads = std::thread::hardware_concurrency();
    const size_t numTasks = 50000;

    JobSystem::WorkStealingThreadPool pool(numThreads);

    std::vector<JobSystem::TaskHandle> handles;
    handles.reserve(numTasks);
    std::atomic<size_t> completedTasks{0};

    auto startTime = std::chrono::high_resolution_clock::now();

    // Variable work per task (every 10th is 100x heavier): the shape that
    // needs redistribution — heavy tasks pin some workers while the cheap
    // backlog must keep flowing through the rest.
    for (size_t i = 0; i < numTasks; ++i) {
        handles.push_back(pool.Submit([&completedTasks, i]() {
            size_t workAmount = (i % 10 == 0) ? 10000 : 100;
            volatile size_t sum = 0;
            for (size_t j = 0; j < workAmount; ++j) {
                sum += j;
            }
            completedTasks.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& handle : handles) {
        handle.Wait();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto durationMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    double tasksPerSecond = numTasks * 1000.0 / static_cast<double>(std::max<long long>(1, durationMs));
    Logger::Log::Info("MixedWeightBurst: {} tasks in {}ms ({:.0f} tasks/sec)",
                      numTasks, durationMs, tasksPerSecond);

    EXPECT_EQ(completedTasks.load(), numTasks) << "every published task must execute";
    // Basis: measured ~130-180ms (280-380k tasks/sec). A stranded backlog
    // (lost wakeup, dead propagation chain) runs into 1ms-per-task backstop
    // territory and misses this floor by orders of magnitude.
    EXPECT_LT(durationMs, 600) << "burst did not drain promptly - wake/propagation regression";
}

// Bounded-wake propagation: one external producer, a wide uniform burst.
// The producer issues at most a bounded number of kernel wakes per publish;
// worker-side propagation must still carry the wake front wide enough that
// the burst completes in bounded time. Deliberately NO per-worker spread
// assertion (see file header). (Successor of the old
// "LoadBalancingEffectiveness".)
TEST_F(WorkStealingTest, BoundedWakePropagationCompletesPromptly) {
    const size_t numThreads = std::thread::hardware_concurrency();
    const size_t numTasks = 20000;

    JobSystem::WorkStealingThreadPool pool(numThreads);

    std::atomic<size_t> completedTasks{0};
    std::vector<JobSystem::TaskHandle> handles;
    handles.reserve(numTasks);

    auto startTime = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < numTasks; ++i) {
        handles.push_back(pool.Submit([&completedTasks]() {
            volatile size_t sum = 0;
            for (size_t j = 0; j < 1000; ++j) {
                sum += j;
            }
            completedTasks.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& handle : handles) {
        handle.Wait();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto durationMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    Logger::Log::Info("BoundedWakePropagation: {} tasks in {}ms", numTasks, durationMs);

    EXPECT_EQ(completedTasks.load(), numTasks) << "every published task must execute";
    // Basis: measured ~40-70ms. See file header for the x3+ budget rationale.
    EXPECT_LT(durationMs, 300) << "propagation stalled - backlog waited on the backstop";

    // The queues must be fully drained once every handle returned.
    EXPECT_EQ(pool.GetApproximateQueueSize(), 0u);
}

// Per-width promptness: the pool completes a fixed storm within budget at
// every worker count. No scaling-efficiency assertion: 10k trivial Submits
// are publication-bound, not execution-bound, so added workers do not (and
// must not be expected to) produce linear speedups — the old >70% efficiency
// gate asserted exactly that and always failed post-slice-1. (Successor of
// the old "ScalabilityComparison".)
TEST_F(WorkStealingTest, StormCompletesWithinBudgetAtEveryWidth) {
    const size_t numTasks = 10000;
    std::vector<size_t> threadCounts = {1, 2, 4, 8, std::thread::hardware_concurrency()};

    for (size_t numThreads : threadCounts) {
        if (numThreads == 0 || numThreads > std::thread::hardware_concurrency()) {
            continue;
        }

        JobSystem::WorkStealingThreadPool pool(numThreads);
        std::atomic<size_t> completedTasks{0};

        auto startTime = std::chrono::high_resolution_clock::now();

        std::vector<JobSystem::TaskHandle> handles;
        handles.reserve(numTasks);
        for (size_t i = 0; i < numTasks; ++i) {
            handles.push_back(pool.Submit([&completedTasks]() {
                volatile size_t sum = 0;
                for (size_t j = 0; j < 1000; ++j) {
                    sum += j;
                }
                completedTasks.fetch_add(1, std::memory_order_relaxed);
            }));
        }

        for (auto& handle : handles) {
            handle.Wait();
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto durationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

        Logger::Log::Info("Storm width {}: {} tasks in {}ms", numThreads, numTasks, durationMs);

        EXPECT_EQ(completedTasks.load(), numTasks)
            << "width " << numThreads << ": every published task must execute";
        // Basis: measured 10-30ms per width. Budget x3+ (see file header).
        EXPECT_LT(durationMs, 150)
            << "width " << numThreads << ": storm exceeded budget - scheduling regression";
    }
}

} // namespace GameEngine::Performance
