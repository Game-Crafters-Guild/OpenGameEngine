#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

// Performance regression suite — re-baselined for the post-overhaul pool
// (issue #242, slice 6).
//
// History: the previous version of this file asserted thresholds measured
// against the pre-overhaul (pre-slice-1) scheduler and told a dead story
// about a 2024-era "object pooling 9x regression". At the slice-5 HEAD,
// 5 of its 9 tests failed on a healthy build — the thresholds were noise,
// not gates (#242). This rewrite re-baselines every threshold against the
// slice-6 allocation-diet costs and deletes the tests that asserted nothing
// (PooledVsNonPooledAllocation, MemoryAllocationAnalysis — superseded by the
// alloc-counting benches in TaskSlabPoolTests.cpp) or duplicated another
// test's shape with a flaky millisecond-resolution relative assert
// (BatchSubmissionComparison, and the ObjectPoolingRegressionPrevention /
// UltraPerformanceMicroBenchmark near-duplicates, merged into
// WarmSubmitStormThroughput below).
//
// Budget basis (2026-07-10, slice-6 diet build, DebugFast, 32-thread desktop,
// idle-ish box; exact measured run recorded in the slice-6 PR):
//   10k warm Submit(lambda)+Wait      ~10-16 ms end-to-end (~0.9M tasks/s)
//   10k EnqueueWork (fire-and-forget) ~3-6 ms end-to-end   (~3M tasks/s)
//   10k EnqueueWorkBatch              ~3-5 ms end-to-end
//   1k  submit->start latency         avg ~10-60 us, p95 ~100-300 us
// Budgets are set at >=3x the observed worst so scheduler/allocation
// regressions fail loudly while machine noise does not. These are DebugFast
// numbers — the suite is not built for true-Debug perf gating.

namespace {

// >= 3x headroom over the measured-worst values in the header table.
constexpr long long kSubmitStorm10kBudgetMs = 60;      // measured ~10-16ms
constexpr long long kFireAndForget10kBudgetMs = 30;    // measured ~3-6ms
constexpr long long kBatch10kBudgetMs = 30;            // measured ~3-5ms
constexpr double kAvgSubmitLatencyBudgetUs = 300.0;    // measured avg ~10-60us
constexpr double kP95SubmitLatencyBudgetUs = 1200.0;   // measured p95 ~100-300us
constexpr long long kLatencyRunBudgetMs = 30;          // measured ~2-4ms
constexpr long long kPerWidthStorm10kBudgetMs = 100;   // measured ~10-30ms/width

} // namespace

class PerformanceRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Error);
    }

    void TearDown() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    }
};

/**
 * @brief Cold-pool Submit throughput: 10k lambda submissions with handles,
 * joined by Wait. No warmup — this intentionally includes first-touch costs
 * (slab freelist fill, registry growth), the shape an editor hits on its
 * first burst after startup.
 */
TEST_F(PerformanceRegressionTest, PureTaskExecutionPerformance) {
    const size_t numTasks = 10000;
    const size_t numThreads = 4;

    JobSystem::WorkStealingThreadPool pool(numThreads);

    std::atomic<size_t> counter{0};
    std::vector<JobSystem::TaskHandle> handles;
    handles.reserve(numTasks);

    auto startTime = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < numTasks; ++i) {
        handles.push_back(pool.Submit([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& handle : handles) {
        handle.Wait();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    EXPECT_EQ(counter.load(), numTasks);
    EXPECT_LT(duration.count(), kSubmitStorm10kBudgetMs)
        << "cold Submit storm exceeded budget (basis: file header table)";

    Logger::Log::Info("Pure task execution: {} tasks completed in {}ms", numTasks,
                      duration.count());
}

/**
 * @brief Submit->execution-start latency distribution under a storm.
 * Percentile budgets pin the wake path: a lost-wakeup regression shows up as
 * multi-millisecond backstop rescues in the tail, orders of magnitude over
 * these budgets.
 */
TEST_F(PerformanceRegressionTest, TaskLatencyDistribution) {
    const size_t numTasks = 1000;
    const size_t numThreads = 4;

    JobSystem::WorkStealingThreadPool pool(numThreads);

    std::vector<std::chrono::high_resolution_clock::time_point> startTimes(numTasks);
    std::vector<std::chrono::high_resolution_clock::time_point> endTimes(numTasks);
    std::vector<JobSystem::TaskHandle> handles;
    handles.reserve(numTasks);

    auto globalStart = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < numTasks; ++i) {
        startTimes[i] = std::chrono::high_resolution_clock::now();
        handles.push_back(pool.Submit([&endTimes, i]() {
            endTimes[i] = std::chrono::high_resolution_clock::now();
        }));
    }

    for (auto& handle : handles) {
        handle.Wait();
    }

    auto globalEnd = std::chrono::high_resolution_clock::now();

    std::vector<double> latencies;
    latencies.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        auto latency =
            std::chrono::duration_cast<std::chrono::microseconds>(endTimes[i] - startTimes[i]);
        latencies.push_back(static_cast<double>(latency.count()));
    }
    std::sort(latencies.begin(), latencies.end());

    double avgLatency = std::accumulate(latencies.begin(), latencies.end(), 0.0) /
                        static_cast<double>(latencies.size());
    double p50 = latencies[latencies.size() / 2];
    double p95 = latencies[static_cast<size_t>(static_cast<double>(latencies.size()) * 0.95)];
    double p99 = latencies[static_cast<size_t>(static_cast<double>(latencies.size()) * 0.99)];

    auto totalDuration =
        std::chrono::duration_cast<std::chrono::milliseconds>(globalEnd - globalStart);

    Logger::Log::Info("Latency distribution for {} tasks: total={}ms avg={:.2f}us p50={:.2f}us "
                      "p95={:.2f}us p99={:.2f}us",
                      numTasks, totalDuration.count(), avgLatency, p50, p95, p99);

    EXPECT_LT(avgLatency, kAvgSubmitLatencyBudgetUs);
    EXPECT_LT(p95, kP95SubmitLatencyBudgetUs);
    EXPECT_LT(totalDuration.count(), kLatencyRunBudgetMs);
}

/**
 * @brief Warm Submit-storm throughput. Merges the old
 * ObjectPoolingRegressionPrevention and UltraPerformanceMicroBenchmark tests
 * (identical 10k-trivial-Submit shapes, thresholds 15ms/10ms measured against
 * the pre-overhaul scheduler — both failed at the slice-5 HEAD, #242). After
 * warmup the fast path is: 1 pooled slab + 1 registry insert per task, ~1-2
 * mutex acquisitions total (slice-6 diet).
 */
TEST_F(PerformanceRegressionTest, WarmSubmitStormThroughput) {
    const size_t kWarmupTasks = 1000;
    const size_t kNumTasks = 10000;
    const size_t kThreads = std::min<size_t>(8, std::thread::hardware_concurrency());

    JobSystem::WorkStealingThreadPool pool(kThreads);

    // Warm the slab freelist, the registry, and the workers.
    {
        std::atomic<size_t> warm{0};
        for (size_t i = 0; i < kWarmupTasks; ++i) {
            pool.Submit([&warm]() { warm.fetch_add(1, std::memory_order_relaxed); });
        }
        while (warm.load(std::memory_order_relaxed) < kWarmupTasks) {
            std::this_thread::yield();
        }
    }

    std::vector<JobSystem::TaskHandle> handles;
    handles.reserve(kNumTasks);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < kNumTasks; ++i) {
        handles.push_back(pool.Submit([]() {
            volatile int result = 1;
            (void)result;
        }));
    }

    for (auto& handle : handles) {
        handle.Wait();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    Logger::Log::Info("Warm Submit storm: {} tasks completed in {}ms", kNumTasks,
                      duration.count());

    EXPECT_LT(duration.count(), kSubmitStorm10kBudgetMs)
        << "warm Submit storm exceeded budget (basis: file header table)";
}

/**
 * @brief Bulk publication throughput via EnqueueWorkBatch (the old
 * "BatchSubmissionPerformance" never called a batch API — it timed individual
 * Submits). One homogeneous closure type, one bulk publish per staging run,
 * bounded batch wake.
 */
TEST_F(PerformanceRegressionTest, BatchPublicationThroughput) {
    const size_t kNumTasks = 10000;

    JobSystem::WorkStealingThreadPool pool(std::thread::hardware_concurrency());

    std::atomic<size_t> counter{0};

    struct CountChunk {
        std::atomic<size_t>* Counter = nullptr;
        void operator()() const { Counter->fetch_add(1, std::memory_order_relaxed); }
    };

    std::vector<CountChunk> chunks(kNumTasks);
    for (auto& c : chunks) {
        c.Counter = &counter;
    }

    auto start = std::chrono::high_resolution_clock::now();

    pool.EnqueueWorkBatch(chunks.data(), chunks.size());

    while (counter.load(std::memory_order_relaxed) < kNumTasks) {
        std::this_thread::yield();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    Logger::Log::Info("Batch publication: {} tasks completed in {}ms", kNumTasks,
                      duration.count());

    EXPECT_EQ(counter.load(), kNumTasks);
    EXPECT_LT(duration.count(), kBatch10kBudgetMs)
        << "EnqueueWorkBatch storm exceeded budget (basis: file header table)";
}

/**
 * @brief Fire-and-forget (EnqueueWork: no handle, no TaskData, pooled bare
 * envelope) vs handle-tracked Submit. The old version compared Submit against
 * Submit (both arms paid full handle bookkeeping) and asserted on ~2ms of
 * millisecond-resolution noise — it failed on healthy builds (#242). The
 * paths now genuinely differ (~3x separation measured), so each arm gets an
 * absolute budget; the ratio is logged for tracking, not asserted.
 */
TEST_F(PerformanceRegressionTest, FireAndForgetVsTaskHandleComparison) {
    const size_t kNumTasks = 10000;

    JobSystem::WorkStealingThreadPool pool(std::thread::hardware_concurrency());

    // Arm 1: handle-tracked Submit + Wait.
    std::vector<JobSystem::TaskHandle> handles;
    handles.reserve(kNumTasks);

    auto start1 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < kNumTasks; ++i) {
        handles.push_back(pool.Submit([]() {
            volatile int result = 1;
            (void)result;
        }));
    }
    for (auto& handle : handles) {
        handle.Wait();
    }
    auto end1 = std::chrono::high_resolution_clock::now();
    auto taskHandleDuration = std::chrono::duration_cast<std::chrono::milliseconds>(end1 - start1);

    // Arm 2: true fire-and-forget.
    std::atomic<size_t> counter{0};

    auto start2 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < kNumTasks; ++i) {
        pool.EnqueueWork([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    while (counter.load(std::memory_order_relaxed) < kNumTasks) {
        std::this_thread::yield();
    }
    auto end2 = std::chrono::high_resolution_clock::now();
    auto fireAndForgetDuration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2);

    Logger::Log::Info("TaskHandle Submit: {} tasks in {}ms; EnqueueWork: {} tasks in {}ms",
                      kNumTasks, taskHandleDuration.count(), kNumTasks,
                      fireAndForgetDuration.count());

    EXPECT_EQ(counter.load(), kNumTasks);
    EXPECT_LT(taskHandleDuration.count(), kSubmitStorm10kBudgetMs);
    EXPECT_LT(fireAndForgetDuration.count(), kFireAndForget10kBudgetMs);
}

/**
 * @brief Parameterized per-width storm: the pool completes a fixed burst
 * within budget at every worker count. No scaling-efficiency expectation —
 * 10k trivial Submits are publication-bound, and bounded wake deliberately
 * leaves surplus workers parked.
 */
class ThreadCountOptimizationTest : public PerformanceRegressionTest,
                                    public ::testing::WithParamInterface<size_t> {};

TEST_P(ThreadCountOptimizationTest, TaskExecutionWithVariableThreadCount) {
    const size_t threadCount = GetParam();
    const size_t kNumTasks = 10000;

    JobSystem::WorkStealingThreadPool pool(threadCount);

    std::vector<JobSystem::TaskHandle> handles;
    handles.reserve(kNumTasks);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < kNumTasks; ++i) {
        handles.push_back(pool.Submit([]() {
            volatile int result = 1;
            (void)result;
        }));
    }

    for (auto& handle : handles) {
        handle.Wait();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    Logger::Log::Info("Storm with {} threads: {} tasks completed in {}ms", threadCount, kNumTasks,
                      duration.count());

    EXPECT_LT(duration.count(), kPerWidthStorm10kBudgetMs)
        << "width " << threadCount << " exceeded budget (basis: file header table)";
}

INSTANTIATE_TEST_SUITE_P(
    ThreadCountComparison,
    ThreadCountOptimizationTest,
    ::testing::Values(1, 2, 4, 8, std::thread::hardware_concurrency()),
    [](const ::testing::TestParamInfo<size_t>& info) {
        return std::to_string(info.param) + "Threads";
    }
);
