// Exactness pin for the completion notify-fence gate (waiter-count Dekker,
// TaskData::Waiters): locks/task on the completion path.
//
//  - With NO registered waiter, completion must take the CompletionMutex
//    exactly ZERO times (the gate's whole point — pre-gate code paid the
//    fence unconditionally, 1 lock per completed handle task).
//  - With a parked waiter, completion must take it exactly ONCE per task.
//
// The fence census (Detail::GetCompletionFenceStatsForTests) is compiled
// only where GE_DEBUG_INSTRUMENTATION is 1 (Debug and DebugFast) so the
// Release hot path pays nothing for this pin; elsewhere the test skips. Run
// it via the true-Debug tripwire gate.

#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace GameEngine::Tests {

#if !GE_DEBUG_INSTRUMENTATION

TEST(CompletionFencePin, FenceCountsExact)
{
    GTEST_SKIP() << "fence census is compiled only where GE_DEBUG_INSTRUMENTATION is 1";
}

#else

TEST(CompletionFencePin, FenceCountsExact)
{
    using SteadyClock = std::chrono::steady_clock;
    constexpr size_t kNoWaiterTasks = 2000;
    constexpr size_t kWaitedRounds = 200;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);
    JobSystem::WorkStealingThreadPool pool(2);

    // ---- Phase 1: no waiter ever registers -> zero fences. ----
    {
        std::atomic<size_t> executed{0};
        const auto before = JobSystem::Detail::GetCompletionFenceStatsForTests();
        for (size_t i = 0; i < kNoWaiterTasks; ++i)
        {
            pool.Submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
        }
        const auto deadline = SteadyClock::now() + std::chrono::seconds(60);
        while (executed.load(std::memory_order_relaxed) < kNoWaiterTasks &&
               SteadyClock::now() < deadline)
        {
            std::this_thread::yield();
        }
        ASSERT_EQ(executed.load(std::memory_order_relaxed), kNoWaiterTasks);
        const auto after = JobSystem::Detail::GetCompletionFenceStatsForTests();

        EXPECT_EQ(after.Fences - before.Fences, 0u)
            << "completion took the CompletionMutex with no waiter registered";
        EXPECT_EQ(after.Skips - before.Skips, kNoWaiterTasks)
            << "every waiter-less completion must take the skip arm exactly once";
    }

    // ---- Phase 2: a parked waiter on every task -> exactly one fence each. ----
    {
        const auto before = JobSystem::Detail::GetCompletionFenceStatsForTests();
        for (size_t round = 0; round < kWaitedRounds; ++round)
        {
            std::atomic<bool> release{false};
            auto handle = pool.Submit([&release] {
                while (!release.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
            });
            ASSERT_TRUE(handle.IsValid());

            std::atomic<bool> aboutToWait{false};
            std::thread waiter([&handle, &aboutToWait] {
                aboutToWait.store(true, std::memory_order_release);
                handle.Wait();
            });
            while (!aboutToWait.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            // Generous window for the waiter's registration + park: the task
            // body cannot complete until `release` flips, so the completion
            // is guaranteed to observe the registered waiter.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            release.store(true, std::memory_order_release);
            waiter.join();
            ASSERT_TRUE(handle.IsDone());
        }
        const auto after = JobSystem::Detail::GetCompletionFenceStatsForTests();

        EXPECT_EQ(after.Fences - before.Fences, kWaitedRounds)
            << "each waited completion must take the fence exactly once";
        EXPECT_EQ(after.Skips - before.Skips, 0u)
            << "a completion skipped the fence while a waiter was parked";
    }

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    pool.Shutdown();
}

#endif // !GE_DEBUG_INSTRUMENTATION

} // namespace GameEngine::Tests
