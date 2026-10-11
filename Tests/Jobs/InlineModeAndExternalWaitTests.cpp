// Inline mode (0 workers) and external waits. Inline mode executes on the
// submitting thread. External waiters park where the platform permits it;
// a browser main-thread waiter participates in its own tagged join.

#include "JobSystem/JobCounter.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/Thread.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using JobSystem::JobCounter;
using JobSystem::WorkStealingThreadPool;

TEST(InlineMode, ExecutesAtPublishOnCaller)
{
    WorkStealingThreadPool pool(0);
    EXPECT_TRUE(pool.IsInlineMode());
    EXPECT_EQ(pool.GetWorkerCount(), 0u);

    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> ran{false};
    std::thread::id ranOn{};
    pool.EnqueueWork([&] {
        ranOn = std::this_thread::get_id();
        ran.store(true, std::memory_order_release);
    });
    // Inline mode executes before EnqueueWork returns.
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_EQ(ranOn, caller);
}

TEST(InlineMode, SubmitCompletesSynchronously)
{
    WorkStealingThreadPool pool(0);
    std::atomic<int> value{0};
    auto handle = pool.Submit([&] { value.store(42, std::memory_order_release); });
    // The task already completed at publish; Wait must not block.
    handle.Wait();
    EXPECT_EQ(value.load(std::memory_order_acquire), 42);
}

TEST(InlineMode, NestedSubmissionRecurses)
{
    WorkStealingThreadPool pool(0);
    std::atomic<int> depth{0};
    pool.EnqueueWork([&] {
        depth.fetch_add(1);
        pool.EnqueueWork([&] {
            depth.fetch_add(1);
            pool.EnqueueWork([&] { depth.fetch_add(1); });
        });
    });
    EXPECT_EQ(depth.load(), 3);
}

TEST(InlineMode, ForkJoinCounterAlreadyZeroAtWait)
{
    WorkStealingThreadPool pool(0);
    JobCounter counter;
    std::atomic<int> sum{0};
    for (int i = 0; i < 8; ++i)
    {
        pool.Run([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); }, counter);
    }
    pool.Wait(counter);
    EXPECT_EQ(sum.load(), 0 + 1 + 2 + 3 + 4 + 5 + 6 + 7);
}

TEST(InlineMode, ParallelForCoversWholeRange)
{
    WorkStealingThreadPool pool(0);
    std::vector<std::atomic<int>> hits(4096);
    JobSystem::ParallelFor(&pool, hits.size(),
                                       [&](size_t begin, size_t end) {
                                           for (size_t i = begin; i < end; ++i)
                                               hits[i].fetch_add(1, std::memory_order_relaxed);
                                       },
                                       64);
    for (size_t i = 0; i < hits.size(); ++i)
    {
        ASSERT_EQ(hits[i].load(), 1) << "index " << i;
    }
}

TEST(ExternalWait, MainThreadRespectsBlockingCapability)
{
    // Two workers are parked on a gate. A participating waiter can finish
    // the tagged work before the gate releases; a native waiter must wait.
    WorkStealingThreadPool pool(2);

    std::atomic<bool> release{false};
    std::atomic<int> gateEntered{0};
    for (int i = 0; i < 2; ++i)
    {
        pool.EnqueueWork([&] {
            gateEntered.fetch_add(1);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    }
    // Both workers are inside the gate before the tagged work is published.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (gateEntered.load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(gateEntered.load(), 2) << "workers never entered the gate";

    const std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<int> ranOnMain{0};
    std::atomic<int> ran{0};
    JobCounter counter;
    constexpr int kJobs = 16;
    for (int i = 0; i < kJobs; ++i)
    {
        pool.Run(
            [&] {
                ran.fetch_add(1, std::memory_order_relaxed);
                if (std::this_thread::get_id() == mainThread)
                    ranOnMain.fetch_add(1, std::memory_order_relaxed);
            },
            counter);
    }

    const bool canBlock = GameEngine::Platform::CanBlockCurrentThread();
    // Native workers are released after a brief scheduling window. A host
    // thread that must participate gets a watchdog only as a failure backstop.
    std::atomic<bool> waited{false};
    std::thread watchdog([&] {
        const auto until = std::chrono::steady_clock::now() +
            (canBlock ? std::chrono::milliseconds(20) : std::chrono::milliseconds(20000));
        while (!waited.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        release.store(true, std::memory_order_release); // unblock regardless
    });

    pool.Wait(counter);
    waited.store(true, std::memory_order_release);
    const bool releasedBeforeWaitReturned = release.load(std::memory_order_acquire);

    watchdog.join();
    EXPECT_EQ(ran.load(), kJobs);
    EXPECT_EQ(releasedBeforeWaitReturned, canBlock);
    EXPECT_EQ(ranOnMain.load(), canBlock ? 0 : kJobs);
}
