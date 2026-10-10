// Inline-mode (0 workers) stack-bound tests: a job tree that spawns jobs from
// jobs must complete without recursing the caller stack past the trampoline
// bound. Without ExecuteInlineBounded, RecursiveChain's ~300k nested
// execute-at-publish frames overflow the stack.

#include "JobSystem/WorkStealingThreadPool.h"
#include "JobSystem/JobCounter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <chrono>
#include <functional>
#include <thread>

namespace {

void SpawnChain(JobSystem::WorkStealingThreadPool& pool, std::atomic<size_t>& counter,
                size_t remaining)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    if (remaining > 0)
    {
        pool.EnqueueWork([&pool, &counter, remaining] { SpawnChain(pool, counter, remaining - 1); });
    }
}

void SpawnTree(JobSystem::WorkStealingThreadPool& pool, std::atomic<size_t>& counter, int depth)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    if (depth > 0)
    {
        for (int child = 0; child < 2; ++child)
        {
            pool.EnqueueWork([&pool, &counter, depth] { SpawnTree(pool, counter, depth - 1); });
        }
    }
}

} // namespace

TEST(InlineModeTrampoline, RecursiveChainDoesNotOverflowStack)
{
    JobSystem::WorkStealingThreadPool pool(0);
    ASSERT_TRUE(pool.IsInlineMode());

    constexpr size_t kChainLength = 300'000;
    std::atomic<size_t> counter{0};
    pool.EnqueueWork([&pool, &counter] { SpawnChain(pool, counter, kChainLength); });

    // Inline mode executes at publish: the whole chain has run by the time
    // EnqueueWork returns.
    EXPECT_EQ(counter.load(), kChainLength + 1);
}

TEST(InlineModeTrampoline, RecursiveTreeRunsEveryTask)
{
    JobSystem::WorkStealingThreadPool pool(0);
    ASSERT_TRUE(pool.IsInlineMode());

    constexpr int kDepth = 16; // 2^17 - 1 tasks
    std::atomic<size_t> counter{0};
    pool.EnqueueWork([&pool, &counter] { SpawnTree(pool, counter, kDepth); });

    EXPECT_EQ(counter.load(), (size_t{1} << (kDepth + 1)) - 1);
}

TEST(InlineModeTrampoline, RecursiveBatchDoesNotOverflowStack)
{
    JobSystem::WorkStealingThreadPool pool(0);
    ASSERT_TRUE(pool.IsInlineMode());

    constexpr size_t kChainLength = 300'000;
    std::atomic<size_t> counter{0};

    struct ChainStep
    {
        JobSystem::WorkStealingThreadPool* Pool;
        std::atomic<size_t>* Counter;
        size_t Remaining;

        void operator()() const
        {
            Counter->fetch_add(1, std::memory_order_relaxed);
            if (Remaining > 0)
            {
                ChainStep next{Pool, Counter, Remaining - 1};
                Pool->EnqueueWorkBatch(&next, 1);
            }
        }
    };

    ChainStep root{&pool, &counter, kChainLength};
    pool.EnqueueWorkBatch(&root, 1);

    EXPECT_EQ(counter.load(), kChainLength + 1);
}

TEST(InlineModeTrampoline, DeferredChildCanBeJoinedBeforeTheOuterTaskReturns)
{
    JobSystem::WorkStealingThreadPool pool(0);
    bool childRan = false;
    std::function<void(int)> descend;
    descend = [&](int depth) {
        if (depth > 0)
        {
            pool.EnqueueWork([&, depth] { descend(depth - 1); });
            return;
        }
        auto child = pool.Submit([&] { childRan = true; });
        // A bounded watchdog turns an inline join deadlock into a failed
        // assertion; cancellation wakes the waiter and allows safe teardown.
        std::atomic<bool> joined{false};
        std::thread watchdog([&] {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (!joined.load() && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!joined.load()) child.Cancel();
        });
        child.Wait();
        joined.store(true);
        watchdog.join();
        EXPECT_TRUE(child.IsCompleted());
    };
    // The existing fire-and-forget chain crosses this same trampoline bound.
    pool.EnqueueWork([&] { descend(63); });
    EXPECT_TRUE(childRan);
}

TEST(InlineModeTrampoline, ShutdownConsumesOnlyItsOwnDeferredWork)
{
    JobSystem::WorkStealingThreadPool outer(0);
    JobSystem::WorkStealingThreadPool inner(0);
    int innerRuns = 0;
    int outerRuns = 0;
    std::function<void(int)> descend;
    descend = [&](int depth) {
        if (depth > 0)
        {
            outer.EnqueueWork([&, depth] { descend(depth - 1); });
            return;
        }
        inner.EnqueueWork([&] { ++innerRuns; });
        outer.EnqueueWork([&] { ++outerRuns; });
        auto cancelled = inner.Submit([] {});
        inner.Shutdown();
        EXPECT_EQ(innerRuns, 1);
        EXPECT_EQ(outerRuns, 0);
        EXPECT_TRUE(cancelled.IsDone());
        EXPECT_TRUE(cancelled.HasFailed());
    };
    outer.EnqueueWork([&] { descend(63); });
    EXPECT_EQ(innerRuns, 1);
    EXPECT_EQ(outerRuns, 1);
}

TEST(InlineModeTrampoline, DeferredCounterChildCanBeJoined)
{
    JobSystem::WorkStealingThreadPool pool(0);
    bool childRan = false;
    std::function<void(int)> descend;
    descend = [&](int depth) {
        if (depth > 0)
        {
            pool.EnqueueWork([&, depth] { descend(depth - 1); });
            return;
        }
        JobSystem::JobCounter counter;
        pool.Run([&] { childRan = true; }, counter);
        pool.Wait(counter);
    };
    pool.EnqueueWork([&] { descend(63); });
    EXPECT_TRUE(childRan);
}
