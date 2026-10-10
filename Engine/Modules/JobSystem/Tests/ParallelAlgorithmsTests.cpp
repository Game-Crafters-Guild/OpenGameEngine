#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <random>
#include <stdexcept>
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
    JobSystem::ParallelFor(m_Pool.get(), 0, 0,
        [&](size_t, size_t) { called = true; });
    EXPECT_FALSE(called);
}

TEST_F(ParallelAlgorithmsTest, ParallelFor_SmallRangeRunsSequentially)
{
    std::vector<int> data(100, 0);
    JobSystem::ParallelFor(m_Pool.get(), 0, data.size(),
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

    JobSystem::ParallelFor(m_Pool.get(), 0, kCount,
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
    JobSystem::ParallelFor(nullptr, 0, data.size(),
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                data[i] = 42;
        }, 100);
    for (int v : data)
        EXPECT_EQ(v, 42);
}

// ---------------------------------------------------------------------------
// ParallelForEach
// ---------------------------------------------------------------------------

TEST_F(ParallelAlgorithmsTest, ParallelForEach_AllElementsVisited)
{
    constexpr size_t kCount = 50000;
    std::vector<std::atomic<int>> data(kCount);
    for (auto& a : data) a.store(0);

    // Create index vector to iterate over
    std::vector<size_t> indices(kCount);
    std::iota(indices.begin(), indices.end(), 0);

    JobSystem::ParallelForEach(m_Pool.get(), indices.begin(), indices.end(),
        [&](size_t idx) {
            data[idx].fetch_add(1, std::memory_order_relaxed);
        }, 1024);

    for (size_t i = 0; i < kCount; ++i)
        EXPECT_EQ(data[i].load(), 1) << "index " << i;
}

TEST_F(ParallelAlgorithmsTest, ParallelForEach_EmptyRange)
{
    std::vector<int> v;
    bool called = false;
    JobSystem::ParallelForEach(m_Pool.get(), v.begin(), v.end(),
        [&](int&) { called = true; });
    EXPECT_FALSE(called);
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
// DispatchAndWait
// ---------------------------------------------------------------------------

TEST_F(ParallelAlgorithmsTest, DispatchAndWait_ZeroTasks)
{
    JobSystem::DispatchAndWait(m_Pool.get(), nullptr, 0);
    // Should not hang or crash.
}

TEST_F(ParallelAlgorithmsTest, DispatchAndWait_SingleTask)
{
    int result = 0;
    std::function<void()> task = [&]() { result = 42; };
    JobSystem::DispatchAndWait(m_Pool.get(), &task, 1);
    EXPECT_EQ(result, 42);
}

TEST_F(ParallelAlgorithmsTest, DispatchAndWait_MultipleTasks)
{
    constexpr uint32_t kCount = 16;
    std::atomic<int> counter{0};
    std::vector<std::function<void()>> tasks(kCount);
    for (auto& t : tasks)
        t = [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); };

    JobSystem::DispatchAndWait(m_Pool.get(), tasks.data(), kCount);
    EXPECT_EQ(counter.load(), static_cast<int>(kCount));
}

TEST_F(ParallelAlgorithmsTest, DispatchAndWait_NullPool)
{
    int result = 0;
    std::function<void()> task = [&]() { result = 7; };
    JobSystem::DispatchAndWait(nullptr, &task, 1);
    EXPECT_EQ(result, 7);
}

TEST_F(ParallelAlgorithmsTest, DispatchAndWait_NullTasksSkipped)
{
    std::atomic<int> counter{0};
    std::vector<std::function<void()>> tasks(4);
    tasks[0] = [&]() { counter.fetch_add(1); };
    tasks[1] = nullptr;
    tasks[2] = [&]() { counter.fetch_add(1); };
    tasks[3] = nullptr;

    JobSystem::DispatchAndWait(m_Pool.get(), tasks.data(), 4);
    EXPECT_EQ(counter.load(), 2);
}

TEST_F(ParallelAlgorithmsTest, DispatchAndWait_AllCompleteBeforeReturn)
{
    constexpr uint32_t kCount = 64;
    std::vector<std::atomic<int>> flags(kCount);
    for (auto& f : flags) f.store(0);

    std::vector<std::function<void()>> tasks(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
    {
        tasks[i] = [&flags, i]() {
            // Simulate some work
            volatile int dummy = 0;
            for (int j = 0; j < 1000; ++j)
                dummy += j;
            (void)dummy;
            flags[i].store(1, std::memory_order_release);
        };
    }

    JobSystem::DispatchAndWait(m_Pool.get(), tasks.data(), kCount);

    for (uint32_t i = 0; i < kCount; ++i)
        EXPECT_EQ(flags[i].load(std::memory_order_acquire), 1) << "task " << i;
}

TEST_F(ParallelAlgorithmsTest, DispatchAndWait_ConcurrentFromMultipleThreads)
{
    // Two threads each do their own DispatchAndWait — verify no deadlock.
    constexpr uint32_t kTasksPerThread = 32;
    std::atomic<int> globalCounter{0};

    auto threadFunc = [&]() {
        std::vector<std::function<void()>> tasks(kTasksPerThread);
        for (auto& t : tasks)
            t = [&globalCounter]() { globalCounter.fetch_add(1, std::memory_order_relaxed); };
        JobSystem::DispatchAndWait(m_Pool.get(), tasks.data(), kTasksPerThread);
    };

    std::thread t1(threadFunc);
    std::thread t2(threadFunc);
    t1.join();
    t2.join();

    EXPECT_EQ(globalCounter.load(), static_cast<int>(kTasksPerThread * 2));
}

// ---------------------------------------------------------------------------
// Additional edge-case tests from audit
// ---------------------------------------------------------------------------

// ParallelFor: inverted range (begin > end) should be a no-op.
TEST_F(ParallelAlgorithmsTest, ParallelFor_InvertedRange)
{
    bool called = false;
    JobSystem::ParallelFor(m_Pool.get(), 10, 5,
        [&](size_t, size_t) { called = true; });
    EXPECT_FALSE(called);
}

// ParallelFor: range exactly at minBatchSize boundary runs sequentially.
TEST_F(ParallelAlgorithmsTest, ParallelFor_ExactBatchSizeBoundary)
{
    constexpr size_t kBatch = 512;
    std::vector<int> data(kBatch, 0);
    JobSystem::ParallelFor(m_Pool.get(), 0, kBatch,
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

    JobSystem::ParallelFor(m_Pool.get(), 0, kCount,
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

    JobSystem::ParallelFor(m_Pool.get(), 0, kCount,
        [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i)
                data[i].fetch_add(1, std::memory_order_relaxed);
        }, 1000);

    for (size_t i = 0; i < kCount; ++i)
        EXPECT_EQ(data[i].load(), 1) << "index " << i;
}

// ParallelForEach: null pool fallback.
TEST_F(ParallelAlgorithmsTest, ParallelForEach_NullPool)
{
    std::vector<int> data = {1, 2, 3, 4, 5};
    int sum = 0;
    JobSystem::ParallelForEach(static_cast<JobSystem::WorkStealingThreadPool*>(nullptr),
        data.begin(), data.end(),
        [&sum](int v) { sum += v; }, 2);
    EXPECT_EQ(sum, 15);
}

// ParallelForEach: single element.
TEST_F(ParallelAlgorithmsTest, ParallelForEach_SingleElement)
{
    std::vector<int> data = {42};
    int seen = 0;
    JobSystem::ParallelForEach(m_Pool.get(), data.begin(), data.end(),
        [&seen](int v) { seen = v; });
    EXPECT_EQ(seen, 42);
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

// DispatchAndWait: all-null tasks array.
TEST_F(ParallelAlgorithmsTest, DispatchAndWait_AllNullTasks)
{
    std::vector<std::function<void()>> tasks(8, nullptr);
    JobSystem::DispatchAndWait(m_Pool.get(), tasks.data(), 8);
    // Should complete without hanging.
}

// DispatchAndWait: large task count under contention.
TEST_F(ParallelAlgorithmsTest, DispatchAndWait_LargeTaskCount)
{
    constexpr uint32_t kCount = 512;
    std::atomic<int> counter{0};
    std::vector<std::function<void()>> tasks(kCount);
    for (auto& t : tasks)
        t = [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); };

    JobSystem::DispatchAndWait(m_Pool.get(), tasks.data(), kCount);
    EXPECT_EQ(counter.load(), static_cast<int>(kCount));
}

// DispatchAndWait: rapid repeated calls (no resource leak).
TEST_F(ParallelAlgorithmsTest, DispatchAndWait_RepeatedCalls)
{
    std::atomic<int> counter{0};
    std::function<void()> task = [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); };

    for (int iter = 0; iter < 200; ++iter)
        JobSystem::DispatchAndWait(m_Pool.get(), &task, 1);

    EXPECT_EQ(counter.load(), 200);
}

// ---------------------------------------------------------------------------
// Exception safety: verify algorithms return without deadlock when user throws
// ---------------------------------------------------------------------------

TEST_F(ParallelAlgorithmsTest, ParallelFor_ExceptionDoesNotHang)
{
    std::atomic<int> chunksCalled{0};
    JobSystem::ParallelFor(m_Pool.get(), 0, 10000,
        [&](size_t begin, size_t /*end*/) {
            chunksCalled.fetch_add(1, std::memory_order_relaxed);
            if (begin == 0)
                throw std::runtime_error("test");
        }, 1000);
    // Must reach here without deadlock. At least the throwing chunk ran.
    EXPECT_GE(chunksCalled.load(), 1);
}

TEST_F(ParallelAlgorithmsTest, ParallelForEach_ExceptionDoesNotHang)
{
    std::vector<int> data(5000);
    std::iota(data.begin(), data.end(), 0);
    std::atomic<int> visited{0};
    JobSystem::ParallelForEach(m_Pool.get(), data.begin(), data.end(),
        [&](int v) {
            visited.fetch_add(1, std::memory_order_relaxed);
            if (v == 0)
                throw std::runtime_error("test");
        }, 500);
    EXPECT_GE(visited.load(), 1);
}

TEST_F(ParallelAlgorithmsTest, DispatchAndWait_ExceptionDoesNotHang)
{
    std::atomic<int> counter{0};
    std::vector<std::function<void()>> tasks(8);
    for (auto& t : tasks)
        t = [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); };
    tasks[0] = [&counter]() {
        counter.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("test");
    };
    JobSystem::DispatchAndWait(m_Pool.get(), tasks.data(), 8);
    // Must return without deadlock. All 8 tasks dispatched.
    EXPECT_EQ(counter.load(), 8);
}

} // namespace
