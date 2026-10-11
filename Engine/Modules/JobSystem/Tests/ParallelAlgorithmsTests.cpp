#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

class ParallelAlgorithmsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(4);
    }

    void TearDown() override
    {
        m_Pool.reset();
    }

    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
};

// ---------------------------------------------------------------------------
// ParallelFor
// ---------------------------------------------------------------------------

TEST_F(ParallelAlgorithmsTest, ParallelFor_EmptyRange)
{
    bool called = false;
    JobSystem::ParallelFor(m_Pool.get(), 0,
        [&](size_t, size_t) { called = true; }, 1);
    EXPECT_FALSE(called);
}

TEST_F(ParallelAlgorithmsTest, ParallelFor_SmallRangeRunsSequentially)
{
    std::vector<int> data(100, 0);
    JobSystem::ParallelFor(m_Pool.get(), data.size(),
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                data[i] = 1;
        }, 1024);
    for (int v : data)
        EXPECT_EQ(v, 1);
}

TEST_F(ParallelAlgorithmsTest, ParallelFor_LargeRangeCoversAll)
{
    constexpr size_t kCount = 100000;
    std::vector<std::atomic<int>> data(kCount);
    for (auto& a : data) a.store(0);

    JobSystem::ParallelFor(m_Pool.get(), kCount,
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                data[i].fetch_add(1, std::memory_order_relaxed);
        }, 1024);

    for (size_t i = 0; i < kCount; ++i)
        EXPECT_EQ(data[i].load(), 1) << "index " << i;
}

TEST_F(ParallelAlgorithmsTest, ParallelFor_NullPoolRunsSequentially)
{
    std::vector<int> data(5000, 0);
    JobSystem::ParallelFor(nullptr, data.size(),
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                data[i] = 42;
        }, 100);
    for (int v : data)
        EXPECT_EQ(v, 42);
}

// ---------------------------------------------------------------------------
// ParallelSort
// ---------------------------------------------------------------------------

TEST_F(ParallelAlgorithmsTest, ParallelSort_MatchesStdSort)
{
    constexpr size_t kCount = 50000;
    std::mt19937 rng(42);
    std::vector<int> data(kCount);
    std::generate(data.begin(), data.end(), rng);

    auto reference = data;
    std::sort(reference.begin(), reference.end());

    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(),
                            std::less<int>{}, 5000);

    EXPECT_EQ(data, reference);
}

TEST_F(ParallelAlgorithmsTest, ParallelSort_BelowThresholdUsesStdSort)
{
    std::vector<int> data = {5, 3, 1, 4, 2};
    auto reference = data;
    std::sort(reference.begin(), reference.end());

    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(),
                            std::less<int>{}, 10000);

    EXPECT_EQ(data, reference);
}

TEST_F(ParallelAlgorithmsTest, ParallelSort_NullPoolFallsBack)
{
    std::mt19937 rng(123);
    std::vector<int> data(10000);
    std::generate(data.begin(), data.end(), rng);

    auto reference = data;
    std::sort(reference.begin(), reference.end());

    JobSystem::ParallelSort(static_cast<JobSystem::WorkStealingThreadPool*>(nullptr),
                            data.begin(), data.end(), std::less<int>{}, 5000);

    EXPECT_EQ(data, reference);
}

TEST_F(ParallelAlgorithmsTest, ParallelSort_CustomComparator)
{
    constexpr size_t kCount = 20000;
    std::mt19937 rng(99);
    std::vector<int> data(kCount);
    std::generate(data.begin(), data.end(), rng);

    auto reference = data;
    std::sort(reference.begin(), reference.end(), std::greater<int>{});

    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(),
                            std::greater<int>{}, 5000);

    EXPECT_EQ(data, reference);
}

TEST_F(ParallelAlgorithmsTest, ParallelSort_AlreadySorted)
{
    constexpr size_t kCount = 20000;
    std::vector<int> data(kCount);
    std::iota(data.begin(), data.end(), 0);

    auto reference = data;
    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(),
                            std::less<int>{}, 5000);

    EXPECT_EQ(data, reference);
}

TEST_F(ParallelAlgorithmsTest, ParallelSort_DefaultComparator)
{
    constexpr size_t kCount = 20000;
    std::mt19937 rng(77);
    std::vector<int> data(kCount);
    std::generate(data.begin(), data.end(), rng);

    auto reference = data;
    std::sort(reference.begin(), reference.end());

    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(), 5000);

    EXPECT_EQ(data, reference);
}

// ---------------------------------------------------------------------------
// ParallelFor, one unit per index
// ---------------------------------------------------------------------------

TEST_F(ParallelAlgorithmsTest, ParallelForUnits_ZeroUnits)
{
    bool called = false;
    EXPECT_TRUE(JobSystem::ParallelFor(m_Pool.get(), 0, [&](size_t) { called = true; }));
    EXPECT_FALSE(called);
}

TEST_F(ParallelAlgorithmsTest, ParallelForUnits_SingleUnit)
{
    int result = 0;
    EXPECT_TRUE(JobSystem::ParallelFor(m_Pool.get(), 1, [&](size_t) { result = 42; }));
    EXPECT_EQ(result, 42);
}

TEST_F(ParallelAlgorithmsTest, ParallelForUnits_MultipleUnits)
{
    constexpr size_t kCount = 16;
    std::atomic<int> counter{0};
    JobSystem::ParallelFor(m_Pool.get(), kCount,
                           [&counter](size_t) { counter.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(counter.load(), static_cast<int>(kCount));
}

// With no pool the caller runs the units in order, and a false stops the run.
TEST_F(ParallelAlgorithmsTest, ParallelForUnits_NullPoolRunsInOrderAndStopsOnFalse)
{
    std::vector<size_t> ran;
    const bool finished = JobSystem::ParallelFor(nullptr, 5,
                                                 [&ran](size_t unit)
                                                 {
                                                     ran.push_back(unit);
                                                     return unit != 2;
                                                 });
    EXPECT_FALSE(finished);
    EXPECT_EQ(ran, (std::vector<size_t>{0, 1, 2}));
}

TEST_F(ParallelAlgorithmsTest, ParallelForUnits_AllCompleteBeforeReturn)
{
    constexpr size_t kCount = 64;
    std::vector<std::atomic<int>> flags(kCount);
    for (auto& f : flags) f.store(0);

    JobSystem::ParallelFor(m_Pool.get(), kCount,
                           [&flags](size_t i)
                           {
                               volatile int dummy = 0;
                               for (int j = 0; j < 1000; ++j)
                                   dummy += j;
                               (void)dummy;
                               flags[i].store(1, std::memory_order_release);
                           });

    for (size_t i = 0; i < kCount; ++i)
        EXPECT_EQ(flags[i].load(std::memory_order_acquire), 1) << "unit " << i;
}

TEST_F(ParallelAlgorithmsTest, ParallelForUnits_ConcurrentFromMultipleThreads)
{
    // Two threads each fork their own run: neither deadlocks.
    constexpr size_t kUnitsPerThread = 32;
    std::atomic<int> globalCounter{0};

    auto threadFunc = [&]() {
        JobSystem::ParallelFor(m_Pool.get(), kUnitsPerThread,
                               [&globalCounter](size_t) { globalCounter.fetch_add(1, std::memory_order_relaxed); });
    };

    std::thread t1(threadFunc);
    std::thread t2(threadFunc);
    t1.join();
    t2.join();

    EXPECT_EQ(globalCounter.load(), static_cast<int>(kUnitsPerThread * 2));
}

TEST_F(ParallelAlgorithmsTest, ParallelForUnits_LargeUnitCount)
{
    constexpr size_t kCount = 512;
    std::atomic<int> counter{0};
    JobSystem::ParallelFor(m_Pool.get(), kCount,
                           [&counter](size_t) { counter.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(counter.load(), static_cast<int>(kCount));
}

TEST_F(ParallelAlgorithmsTest, ParallelForUnits_RepeatedCalls)
{
    std::atomic<int> counter{0};
    for (int iter = 0; iter < 200; ++iter)
        JobSystem::ParallelFor(m_Pool.get(), 1,
                               [&counter](size_t) { counter.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_EQ(counter.load(), 200);
}

// ---------------------------------------------------------------------------
// Additional edge-case tests from audit
// ---------------------------------------------------------------------------

// ParallelFor: range exactly at minBatchSize boundary runs sequentially.
TEST_F(ParallelAlgorithmsTest, ParallelFor_ExactBatchSizeBoundary)
{
    constexpr size_t kBatch = 512;
    std::vector<int> data(kBatch, 0);
    JobSystem::ParallelFor(m_Pool.get(), kBatch,
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                data[i] = 1;
        }, kBatch);
    for (int v : data)
        EXPECT_EQ(v, 1);
}

// ParallelFor: range = minBatchSize + 1 goes parallel (smallest parallel range).
TEST_F(ParallelAlgorithmsTest, ParallelFor_MinBatchSizePlusOne)
{
    constexpr size_t kBatch = 256;
    constexpr size_t kCount = kBatch + 1;
    std::vector<std::atomic<int>> data(kCount);
    for (auto& a : data) a.store(0);

    JobSystem::ParallelFor(m_Pool.get(), kCount,
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                data[i].fetch_add(1, std::memory_order_relaxed);
        }, kBatch);

    for (size_t i = 0; i < kCount; ++i)
        EXPECT_EQ(data[i].load(), 1) << "index " << i;
}

// ParallelFor: non-power-of-2 count (last chunk is smaller).
TEST_F(ParallelAlgorithmsTest, ParallelFor_NonPowerOf2Count)
{
    constexpr size_t kCount = 10007;
    std::vector<std::atomic<int>> data(kCount);
    for (auto& a : data) a.store(0);

    JobSystem::ParallelFor(m_Pool.get(), kCount,
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                data[i].fetch_add(1, std::memory_order_relaxed);
        }, 1000);

    for (size_t i = 0; i < kCount; ++i)
        EXPECT_EQ(data[i].load(), 1) << "index " << i;
}

// ParallelSort: reverse-sorted input.
TEST_F(ParallelAlgorithmsTest, ParallelSort_ReverseSorted)
{
    constexpr size_t kCount = 20000;
    std::vector<int> data(kCount);
    std::iota(data.rbegin(), data.rend(), 0); // descending

    auto reference = data;
    std::sort(reference.begin(), reference.end());

    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(),
                            std::less<int>{}, 5000);

    EXPECT_EQ(data, reference);
}

// ParallelSort: all-duplicate elements.
TEST_F(ParallelAlgorithmsTest, ParallelSort_AllDuplicates)
{
    constexpr size_t kCount = 20000;
    std::vector<int> data(kCount, 7);
    auto reference = data;

    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(),
                            std::less<int>{}, 5000);

    EXPECT_EQ(data, reference);
}

// ParallelSort: many duplicates from a small value range.
TEST_F(ParallelAlgorithmsTest, ParallelSort_ManyDuplicates)
{
    constexpr size_t kCount = 50000;
    std::mt19937 rng(55);
    std::vector<int> data(kCount);
    for (auto& v : data)
        v = static_cast<int>(rng() % 50);

    auto reference = data;
    std::sort(reference.begin(), reference.end());

    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(),
                            std::less<int>{}, 5000);

    EXPECT_EQ(data, reference);
}

// ParallelSort: single and two elements.
TEST_F(ParallelAlgorithmsTest, ParallelSort_SingleElement)
{
    std::vector<int> data = {42};
    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(),
                            std::less<int>{}, 1);
    EXPECT_EQ(data[0], 42);
}

TEST_F(ParallelAlgorithmsTest, ParallelSort_TwoElements)
{
    std::vector<int> data = {5, 3};
    JobSystem::ParallelSort(m_Pool.get(), data.begin(), data.end(),
                            std::less<int>{}, 1);
    EXPECT_EQ(data[0], 3);
    EXPECT_EQ(data[1], 5);
}

// ---------------------------------------------------------------------------
// Exceptions: the first chunk's throw reaches the caller once the chunks in
// flight have finished; chunks not yet started do not run.
// ---------------------------------------------------------------------------

// Counts the chunks running at once, so a test can check none is still
// running when the throw reaches the caller.
struct RunningChunks
{
    void Enter() { Running.fetch_add(1); }
    void Leave() { Running.fetch_sub(1); }
    std::atomic<int> Running{0};
};

TEST_F(ParallelAlgorithmsTest, ParallelFor_ChunkThrowReachesTheCaller)
{
    RunningChunks running;
    std::string thrown;
    try
    {
        JobSystem::ParallelFor(m_Pool.get(), 10000,
            [&](size_t begin, size_t /*end*/) {
                running.Enter();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                running.Leave();
                if (begin == 0)
                    throw std::runtime_error("chunk zero threw");
            }, 1000);
    }
    catch (const std::runtime_error& e)
    {
        thrown = e.what();
    }
    EXPECT_EQ(thrown, "chunk zero threw");
    EXPECT_EQ(running.Running.load(), 0) << "ParallelFor threw while a chunk was still running";
}

TEST_F(ParallelAlgorithmsTest, ParallelForUnits_UnitThrowReachesTheCaller)
{
    std::atomic<int> counter{0};
    EXPECT_THROW(JobSystem::ParallelFor(m_Pool.get(), 8,
                                        [&counter](size_t unit)
                                        {
                                            counter.fetch_add(1, std::memory_order_relaxed);
                                            if (unit == 0)
                                                throw std::runtime_error("unit zero threw");
                                        }),
                 std::runtime_error);
    EXPECT_GE(counter.load(), 1);
    EXPECT_LE(counter.load(), 8);
}

} // namespace
