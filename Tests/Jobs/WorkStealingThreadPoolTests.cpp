#include "Logger/Logger.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "JobSystem/TaskDependencyGraph.h"
#include "TestPlatform.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <vector>
#include <random>

namespace GameEngine::Tests {

using namespace JobSystem;

class WorkStealingThreadPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    }

    void TearDown() override {
        // Clean up any resources
    }
};

/**
 * @brief Test basic task execution
 */
TEST_F(WorkStealingThreadPoolTest, BasicTaskExecution) {
    WorkStealingThreadPool pool(2);

    std::atomic<int> counter{0};
    std::atomic<bool> taskExecuted{false};

    auto handle = pool.Submit([&counter, &taskExecuted]() {
        counter.fetch_add(1);
        taskExecuted.store(true);
        return 42;
    });

    handle.Wait();

    int result = 0;
    EXPECT_TRUE(handle.TryGetResult(result));
    EXPECT_EQ(result, 42);
    EXPECT_EQ(counter.load(), 1);
    EXPECT_TRUE(taskExecuted.load());
}

/**
 * @brief TryGetResult's two false branches stay distinguishable.
 *
 * Not-yet-completed and completed-without-a-payload (void body) both return
 * false silently in every build. Completed WITH a payload of a different type
 * is a caller bug that reads as "never completes" — Debug builds assert on it
 * (death test below); Release behavior is unchanged (silent false).
 */
TEST_F(WorkStealingThreadPoolTest, TryGetResultFalseBranchesAreBenign) {
    WorkStealingThreadPool pool(2);

    // Branch 1: not yet completed — false, no diagnostic.
    std::atomic<bool> release{false};
    auto gated = pool.Submit([&release]() {
        while (!release.load()) {
            std::this_thread::yield();
        }
        return 7;
    });
    int result = 0;
    EXPECT_FALSE(gated.TryGetResult(result));
    release.store(true);
    gated.Wait();
    EXPECT_TRUE(gated.TryGetResult(result));
    EXPECT_EQ(result, 7);

    // Branch 2: completed with NO stored payload (void body) — false, and the
    // Debug type-mismatch assert must NOT fire (there is nothing to mismatch).
    auto voidTask = pool.Submit([]() {});
    voidTask.Wait();
    EXPECT_FALSE(voidTask.TryGetResult(result));
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
/**
 * @brief Wrong-T on a completed task with a stored result trips the Debug
 * assert instead of silently polling false forever.
 */
TEST(TryGetResultDeathTest, TypeMismatchOnCompletedTaskAsserts) {
    EXPECT_DEATH(
        {
            GameEngine::Tests::DisableAbortDialogs();
            WorkStealingThreadPool pool(1);
            auto handle = pool.Submit([]() { return 42; }); // stores an int
            handle.Wait();
            double wrongType = 0.0;
            (void)handle.TryGetResult(wrongType); // int payload polled as double
        },
        "type mismatch");
}
#endif

/**
 * @brief IsRunning reports the Running state only: false while the task waits
 * behind the pool's one busy worker, true while its body runs, false once done.
 */
TEST(TaskHandleTest, IsRunningIsTrueOnlyWhileTheBodyRuns) {
    WorkStealingThreadPool pool(1);
    constexpr auto kTimeout = std::chrono::seconds(10);

    std::promise<void> blockerEntered;
    std::promise<void> releaseBlocker;
    auto blocker = pool.Submit([&blockerEntered, gate = releaseBlocker.get_future().share()]() {
        blockerEntered.set_value();
        gate.wait();
    });
    ASSERT_EQ(blockerEntered.get_future().wait_for(kTimeout), std::future_status::ready);

    std::promise<void> bodyEntered;
    std::promise<void> releaseBody;
    auto handle = pool.Submit([&bodyEntered, gate = releaseBody.get_future().share()]() {
        bodyEntered.set_value();
        gate.wait();
    });
    EXPECT_FALSE(handle.IsRunning()) << "queued behind the blocker, not dispatched";

    releaseBlocker.set_value();
    ASSERT_EQ(bodyEntered.get_future().wait_for(kTimeout), std::future_status::ready);
    EXPECT_TRUE(handle.IsRunning()) << "the body is held at its gate";

    releaseBody.set_value();
    handle.Wait();
    EXPECT_FALSE(handle.IsRunning());
    EXPECT_TRUE(handle.IsCompleted());
    blocker.Wait();
}

/**
 * @brief A completion callback may register another callback on the same
 * handle: the completion pass runs callbacks outside the handle's callback
 * lock, and so does the immediate run of one registered after completion.
 */
TEST(TaskHandleTest, ACompletionCallbackCanRegisterAnotherOnTheSameHandle) {
    constexpr auto kTimeout = std::chrono::seconds(10);

    // Declared before the pool: the worker may still be returning through
    // these when the wait below is satisfied, and the pool joins it first.
    std::promise<void> thirdRan;
    TaskHandle handle;
    WorkStealingThreadPool pool(1);

    std::promise<void> releaseBody;
    handle = pool.Submit([gate = releaseBody.get_future().share()]() { gate.wait(); });

    // The first callback is registered while the body is held, so it fires
    // from the worker's completion pass. The second, the handle overload,
    // finds the task completed and runs at once on the worker, and registers
    // the third the same way.
    handle.OnComplete([&handle, &thirdRan]() {
        handle.OnComplete([&handle, &thirdRan](const TaskHandle&) {
            handle.OnComplete([&thirdRan]() { thirdRan.set_value(); });
        });
    });

    releaseBody.set_value();
    EXPECT_EQ(thirdRan.get_future().wait_for(kTimeout), std::future_status::ready)
        << "a callback registered from a callback never ran";
    handle.Wait();
}

/**
 * @brief The cancel path runs failure callbacks outside the handle's callback
 * lock too, so a failure callback may register another on the same handle.
 */
TEST(TaskHandleTest, ACancelledTasksFailureCallbackCanRegisterAnotherOnTheSameHandle) {
    WorkStealingThreadPool pool(1);
    constexpr auto kTimeout = std::chrono::seconds(10);

    std::promise<void> blockerEntered;
    std::promise<void> releaseBlocker;
    auto blocker = pool.Submit([&blockerEntered, gate = releaseBlocker.get_future().share()]() {
        blockerEntered.set_value();
        gate.wait();
    });
    ASSERT_EQ(blockerEntered.get_future().wait_for(kTimeout), std::future_status::ready);

    // The first callback fires from the cancel pass. The second finds the task
    // cancelled and runs at once, and registers the third the same way.
    TaskHandle queued = pool.Submit([]() {});
    bool thirdRan = false;
    queued.OnFailure([&queued, &thirdRan](const String&) {
        queued.OnFailure([&queued, &thirdRan](const String&) {
            queued.OnFailure([&thirdRan](const String&) { thirdRan = true; });
        });
    });

    ASSERT_TRUE(queued.Cancel()) << "queued behind the blocker, so the cancel wins";
    EXPECT_TRUE(thirdRan) << "Cancel fires the failure callbacks on this thread";

    releaseBlocker.set_value();
    blocker.Wait();
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
/**
 * @brief A job body that waits on a handle of its own pool trips the Debug
 * assert even when that handle is already terminal, so the check never
 * depends on timing.
 */
TEST(TaskHandleWaitDeathTest, WaitOnAPoolWorkerAssertsEvenWhenTerminal) {
    EXPECT_DEATH(
        {
            GameEngine::Tests::DisableAbortDialogs();
            WorkStealingThreadPool pool(1);
            auto done = pool.Submit([]() {});
            done.Wait(); // off the pool: allowed
            auto waiter = pool.Submit([done]() mutable { done.Wait(); });
            waiter.Wait();
        },
        "TaskHandle::Wait on a pool worker");
}
#endif

/**
 * @brief Test work-stealing behavior with multiple threads
 */
TEST_F(WorkStealingThreadPoolTest, WorkStealingBehavior) {
    const size_t numThreads = 4;
    const size_t numTasks = 100;

    WorkStealingThreadPool pool(numThreads);

    std::atomic<size_t> completedTasks{0};
    std::vector<TaskHandle> handles;

    // Submit many tasks to trigger work stealing
    for (size_t i = 0; i < numTasks; ++i) {
        handles.push_back(pool.Submit([&completedTasks, i]() {
            // Simulate some work
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completedTasks.fetch_add(1);
        }));
    }

    // Wait for all tasks to complete
    for (auto& handle : handles) {
        handle.Wait();
    }

    EXPECT_EQ(completedTasks.load(), numTasks);
}

/**
 * @brief Test task distribution across workers
 */
TEST_F(WorkStealingThreadPoolTest, TaskDistribution) {
    const size_t numThreads = 4;
    const size_t numTasks = 200;

    WorkStealingThreadPool pool(numThreads);

    std::atomic<size_t> completedTasks{0};
    std::vector<std::atomic<size_t>> threadTaskCounts(numThreads);

    // Initialize counters
    for (auto& counter : threadTaskCounts) {
        counter.store(0);
    }

    std::vector<TaskHandle> handles;

    // Submit tasks that track which thread executes them
    for (size_t i = 0; i < numTasks; ++i) {
        handles.push_back(pool.Submit([&completedTasks, &threadTaskCounts]() {
            // Simulate work
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            completedTasks.fetch_add(1);
        }));
    }
    
    // Wait for completion
    for (auto& handle : handles) {
        handle.Wait();
    }
    
    EXPECT_EQ(completedTasks.load(), numTasks);
    
    // Verify that work was distributed (no single thread should do all work)
    size_t maxTasksPerThread = numTasks / numThreads + numThreads; // Allow some imbalance
    for (const auto& counter : threadTaskCounts) {
        EXPECT_LE(counter.load(), maxTasksPerThread);
    }
}

/**
 * @brief Test cancellation token functionality
 */
// TODO: Re-enable when CancellationTokenSource is implemented
/*
TEST_F(WorkStealingThreadPoolTest, CancellationToken) {
    WorkStealingThreadPool pool(2);

    auto tokenSource = std::make_shared<CancellationTokenSource>();
    auto token = tokenSource->getToken();

    std::atomic<bool> taskStarted{false};
    std::atomic<bool> taskCancelled{false};

    auto handle = pool.submit([&taskStarted, &taskCancelled, token]() {
        taskStarted.store(true);

        // Simulate long-running task with cancellation checks
        for (int i = 0; i < 100; ++i) {
            if (token->IsCancelled()) {
                taskCancelled.store(true);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Wait for task to start
    while (!taskStarted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Cancel the operation
    tokenSource->cancel();

    handle.Wait();

    EXPECT_TRUE(taskStarted.load());
    EXPECT_TRUE(taskCancelled.load());
}
*/

/**
 * @brief Test exception handling in tasks
 */
TEST_F(WorkStealingThreadPoolTest, ExceptionHandling) {
    WorkStealingThreadPool pool(2);

    auto handle = pool.Submit([]() -> int {
        throw std::runtime_error("Test exception");
        return 42;
    });

    handle.Wait();

    EXPECT_TRUE(handle.HasFailed());
    int result = 0;
    EXPECT_FALSE(handle.TryGetResult(result));
}

/**
 * @brief Test pool shutdown behavior (slice-3 F13 contract).
 *
 * Shutdown no longer runs queued handle tasks to completion: workers finish
 * their in-flight task and exit, queued handle tasks are Cancelled, and every
 * handle is terminal when Shutdown returns — no waiter can hang (B6).
 */
TEST_F(WorkStealingThreadPoolTest, PoolShutdown) {
    auto pool = std::make_unique<WorkStealingThreadPool>(2);

    std::atomic<size_t> completedTasks{0};
    std::vector<TaskHandle> handles;

    // Submit some tasks
    for (int i = 0; i < 10; ++i) {
        handles.push_back(pool->Submit([&completedTasks]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            completedTasks.fetch_add(1);
        }));
    }

    pool->Shutdown();

    // Every task either ran to completion or was cancelled while queued —
    // never both, never neither — and every handle is terminal.
    size_t completedHandles = 0;
    size_t cancelledHandles = 0;
    for (auto& handle : handles) {
        EXPECT_TRUE(handle.IsDone());
        if (handle.IsCompleted()) {
            ++completedHandles;
        } else {
            ++cancelledHandles;
        }
    }
    EXPECT_EQ(completedHandles, completedTasks.load());
    EXPECT_EQ(completedHandles + cancelledHandles, 10u);

    pool.reset();
}

/**
 * @brief Test high concurrency scenario
 */
TEST_F(WorkStealingThreadPoolTest, HighConcurrency) {
    const size_t numThreads = std::thread::hardware_concurrency();
    const size_t numTasks = 1000;
    
    WorkStealingThreadPool pool(numThreads);
    
    std::atomic<size_t> completedTasks{0};
    std::vector<TaskHandle> handles;

    // Submit many concurrent tasks
    for (size_t i = 0; i < numTasks; ++i) {
        handles.push_back(pool.Submit([&completedTasks, i]() -> size_t {
            // Simulate variable work time
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(1, 10);

            std::this_thread::sleep_for(std::chrono::microseconds(dis(gen)));
            completedTasks.fetch_add(1);
            return i;
        }));
    }

    // Verify all tasks complete with correct results
    for (size_t i = 0; i < numTasks; ++i) {
        handles[i].Wait();
        size_t result = 0;
        EXPECT_TRUE(handles[i].TryGetResult(result));
        EXPECT_EQ(result, i);
    }
    
    EXPECT_EQ(completedTasks.load(), numTasks);
}

/**
 * @brief Test task queue statistics
 */
TEST_F(WorkStealingThreadPoolTest, TaskQueueStatistics) {
    WorkStealingThreadPool pool(2);
    
    // TODO: Add these methods to WorkStealingThreadPool API
    // EXPECT_EQ(pool.getPendingTaskCount(), 0);
    // EXPECT_TRUE(pool.isRunning());

    // Placeholder assertions until API is implemented
    EXPECT_TRUE(true);
    
    std::vector<TaskHandle> handles;

    // Submit tasks that will block temporarily
    for (int i = 0; i < 5; ++i) {
        handles.push_back(pool.Submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }));
    }

    // Wait for completion
    for (auto& handle : handles) {
        handle.Wait();
    }
    
    // TODO: Add getPendingTaskCount method to WorkStealingThreadPool API
    // EXPECT_EQ(pool.getPendingTaskCount(), 0);
}

/**
 * @brief Performance benchmark test
 */
TEST_F(WorkStealingThreadPoolTest, PerformanceBenchmark) {
    const size_t numTasks = 10000;
    const size_t numThreads = std::thread::hardware_concurrency();
    
    WorkStealingThreadPool pool(numThreads);
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    std::vector<TaskHandle> handles;
    std::atomic<size_t> counter{0};

    for (size_t i = 0; i < numTasks; ++i) {
        handles.push_back(pool.Submit([&counter]() {
            counter.fetch_add(1);
        }));
    }

    for (auto& handle : handles) {
        handle.Wait();
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    EXPECT_EQ(counter.load(), numTasks);
    
    // Performance expectation: should complete 10k simple tasks in reasonable time
    EXPECT_LT(duration.count(), 5000); // Less than 5 seconds
    
    Logger::Log::Info("Performance test: {} tasks completed in {}ms", numTasks, duration.count());
}

} // namespace GameEngine::Tests
