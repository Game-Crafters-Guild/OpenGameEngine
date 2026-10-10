#include "Scripting/ScriptManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <vector>

using namespace GameEngine;

TEST(ScriptManagerMainThreadTasks, TaskCanEnqueueFollowUpWork)
{
    ScriptManager manager;
    std::vector<int> executionOrder;

    manager.QueueMainThreadTask([&]()
    {
        executionOrder.push_back(1);
        manager.QueueMainThreadTask([&]()
        {
            executionOrder.push_back(2);
        });
    });

    EXPECT_EQ(manager.ProcessMainThreadTasks(), 2u);
    EXPECT_EQ(executionOrder, (std::vector<int>{1, 2}));
}

TEST(ScriptManagerMainThreadTasks, TaskCanReentrantlyPumpFollowUpWork)
{
    ScriptManager manager;
    std::vector<int> executionOrder;
    size_t nestedTasksProcessed = 0;

    manager.QueueMainThreadTask([&]()
    {
        executionOrder.push_back(1);
        manager.QueueMainThreadTask([&]()
        {
            executionOrder.push_back(2);
        });
        nestedTasksProcessed = manager.ProcessMainThreadTasks();
        executionOrder.push_back(3);
    });

    EXPECT_EQ(manager.ProcessMainThreadTasks(), 1u);
    EXPECT_EQ(nestedTasksProcessed, 1u);
    EXPECT_EQ(executionOrder, (std::vector<int>{1, 2, 3}));
}

TEST(ScriptManagerMainThreadTasks, ConcurrentProcessorsDoNotOverlapTaskExecution)
{
    using namespace std::chrono_literals;

    ScriptManager manager;
    std::promise<void> firstTaskStarted;
    std::promise<void> releaseFirstTask;
    std::promise<void> secondProcessorStarted;
    std::promise<void> secondTaskStarted;
    std::shared_future<void> releaseFirstTaskFuture = releaseFirstTask.get_future().share();

    manager.QueueMainThreadTask([&]()
    {
        firstTaskStarted.set_value();
        releaseFirstTaskFuture.wait();
    });
    manager.QueueMainThreadTask([&]()
    {
        secondTaskStarted.set_value();
    });

    auto firstProcessor = std::async(std::launch::async, [&]()
    {
        return manager.ProcessMainThreadTasks();
    });

    auto firstTaskStartedFuture = firstTaskStarted.get_future();
    EXPECT_EQ(firstTaskStartedFuture.wait_for(1s), std::future_status::ready);

    auto secondProcessor = std::async(std::launch::async, [&]()
    {
        secondProcessorStarted.set_value();
        return manager.ProcessMainThreadTasks();
    });

    auto secondProcessorStartedFuture = secondProcessorStarted.get_future();
    EXPECT_EQ(secondProcessorStartedFuture.wait_for(1s), std::future_status::ready);

    auto secondTaskStartedFuture = secondTaskStarted.get_future();
    EXPECT_EQ(secondTaskStartedFuture.wait_for(100ms), std::future_status::timeout);

    releaseFirstTask.set_value();

    EXPECT_EQ(secondTaskStartedFuture.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(firstProcessor.get() + secondProcessor.get(), 2u);
}

TEST(ScriptManagerMainThreadTasks, PumpReturnsImmediatelyWhileAnotherThreadIsPumping)
{
    using namespace std::chrono_literals;

    ScriptManager manager;
    std::promise<void> firstTaskStarted;
    std::promise<void> releaseFirstTask;
    std::future<void> releaseFirstTaskFuture = releaseFirstTask.get_future();
    std::atomic<bool> secondTaskRan{false};

    manager.QueueMainThreadTask([&]()
    {
        firstTaskStarted.set_value();
        releaseFirstTaskFuture.wait();
    });
    manager.QueueMainThreadTask([&]()
    {
        secondTaskRan = true;
    });

    auto firstProcessor = std::async(std::launch::async, [&]()
    {
        return manager.ProcessMainThreadTasks();
    });

    ASSERT_EQ(firstTaskStarted.get_future().wait_for(1s), std::future_status::ready);

    // A pump from another thread must not block behind the executing task, must
    // not steal queued tasks, and must report 0 so callers can retry/timeout.
    const auto busyPumpStart = std::chrono::steady_clock::now();
    const size_t busyResult = manager.ProcessMainThreadTasks();
    const auto busyPumpElapsed = std::chrono::steady_clock::now() - busyPumpStart;

    EXPECT_EQ(busyResult, 0u);
    EXPECT_LT(busyPumpElapsed, 500ms);
    EXPECT_FALSE(secondTaskRan.load());

    releaseFirstTask.set_value();

    EXPECT_EQ(firstProcessor.get(), 2u);
    EXPECT_TRUE(secondTaskRan.load());
}

TEST(ScriptManagerMainThreadTasks, MarkMainThreadDoesNotPreventFallbackPumping)
{
    ScriptManager manager;
    manager.MarkMainThread();

    bool taskRan = false;
    manager.QueueMainThreadTask([&]() { taskRan = true; });

    // Affinity is diagnostic-only: a pump from a non-main thread (the hot-reload
    // fallback for hosts without a main pump) still executes tasks.
    auto fallbackProcessor = std::async(std::launch::async, [&]()
    {
        return manager.ProcessMainThreadTasks();
    });

    EXPECT_EQ(fallbackProcessor.get(), 1u);
    EXPECT_TRUE(taskRan);
}
