// Slice-2 dependency-graph coverage: erase-on-terminal, the TOCTOU close,
// tombstones, absent-dependency semantics, and callbacks fired outside the lock.
//
// Two layers:
//  - TaskDependencyGraphTest: graph-unit tests driving Register/MarkCompleted/
//    CancelPending directly with pool-less tasks.
//  - TaskGraphPoolTest: pool-integration tests pinning the end-to-end
//    behaviors (leak tripwire, submit-vs-complete race, submit-from-callback,
//    failure propagation, cancellation).

#include "Logger/Logger.h"
#include "JobSystem/TaskDependencyGraph.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestPlatform.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <thread>
#include <vector>

#include "../TestEnvVar.h"

using namespace JobSystem;

namespace GameEngine::Tests {

namespace {

using SteadyClock = std::chrono::steady_clock;

/** Graph-unit helper: pool-less task with an explicit id. */
class GraphOnlyTask : public Task {
public:
    explicit GraphOnlyTask(TaskId id) : Task(nullptr, id) {}
    void Execute() override {}
};

/** Pool-level task whose body is supplied by the test. */
class ScriptedTask : public Task {
public:
    ScriptedTask(WorkStealingThreadPool* pool, std::function<void()> body)
        : Task(pool), m_Body(std::move(body)) {}

    void Execute() override {
        if (m_Body) {
            m_Body();
        }
    }

private:
    std::function<void()> m_Body;
};

bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    const auto deadline = SteadyClock::now() + timeout;
    while (!pred()) {
        if (SteadyClock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

bool WaitUntilDone(const TaskHandle& handle, std::chrono::milliseconds timeout) {
    return WaitUntil([&handle] { return handle.IsDone(); }, timeout);
}

// Sub-millisecond jitter for the B8 race; sleep_for cannot hit µs gaps on
// Windows.
void BusyWaitUs(int us) {
    if (us <= 0) {
        return;
    }
    const auto deadline = SteadyClock::now() + std::chrono::microseconds(us);
    while (SteadyClock::now() < deadline) {
        std::this_thread::yield();
    }
}

} // namespace

class TaskDependencyGraphTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    }

    TaskDependencyGraph graph;
};

TEST_F(TaskDependencyGraphTest, RegisterWithoutDependenciesIsImmediatelyReady) {
    auto ready = graph.Register(MakeUnique<GraphOnlyTask>(1));
    ASSERT_NE(ready, nullptr);
    EXPECT_TRUE(graph.TaskExists(1));
    EXPECT_EQ(graph.GetTrackedTaskCountForTests(), 1u);
}

TEST_F(TaskDependencyGraphTest, CompletedNodeIsErased) {
    auto ready = graph.Register(MakeUnique<GraphOnlyTask>(1));
    ASSERT_NE(ready, nullptr);

    auto actions = graph.MarkCompleted(1, true);
    ASSERT_EQ(actions.Events.size(), 1u);
    EXPECT_EQ(actions.Events[0].Id, 1u);
    EXPECT_EQ(actions.Events[0].NewStatus, TaskStatus::Completed);
    EXPECT_TRUE(actions.ReadyTasks.empty());
    EXPECT_TRUE(actions.RetiredTasks.empty());

    // B4: terminal nodes are erased in the same critical section.
    EXPECT_FALSE(graph.TaskExists(1));
    EXPECT_EQ(graph.GetTrackedTaskCountForTests(), 0u);
}

TEST_F(TaskDependencyGraphTest, DependentParksUntilDependencyCompletes) {
    ASSERT_NE(graph.Register(MakeUnique<GraphOnlyTask>(1)), nullptr);

    auto dependent = MakeUnique<GraphOnlyTask>(2);
    dependent->AddDependency(TaskId{1});
    EXPECT_EQ(graph.Register(std::move(dependent)), nullptr); // parked
    EXPECT_EQ(graph.GetTrackedTaskCountForTests(), 2u);

    auto actions = graph.MarkCompleted(1, true);
    ASSERT_EQ(actions.ReadyTasks.size(), 1u);
    EXPECT_EQ(actions.ReadyTasks[0]->GetTaskId(), 2u);
    EXPECT_FALSE(graph.TaskExists(1));
    EXPECT_TRUE(graph.TaskExists(2)); // stays tracked until its own terminal

    actions = graph.MarkCompleted(2, true);
    ASSERT_EQ(actions.Events.size(), 1u);
    EXPECT_EQ(graph.GetTrackedTaskCountForTests(), 0u);
}

TEST_F(TaskDependencyGraphTest, FanInReleasesAfterLastDependency) {
    ASSERT_NE(graph.Register(MakeUnique<GraphOnlyTask>(1)), nullptr);
    ASSERT_NE(graph.Register(MakeUnique<GraphOnlyTask>(2)), nullptr);

    auto dependent = MakeUnique<GraphOnlyTask>(3);
    dependent->AddDependency(TaskId{1});
    dependent->AddDependency(TaskId{2});
    EXPECT_EQ(graph.Register(std::move(dependent)), nullptr);

    auto actions = graph.MarkCompleted(1, true);
    EXPECT_TRUE(actions.ReadyTasks.empty()); // one dependency still pending

    actions = graph.MarkCompleted(2, true);
    ASSERT_EQ(actions.ReadyTasks.size(), 1u);
    EXPECT_EQ(actions.ReadyTasks[0]->GetTaskId(), 3u);
}

TEST_F(TaskDependencyGraphTest, FanOutReleasesAllDependents) {
    ASSERT_NE(graph.Register(MakeUnique<GraphOnlyTask>(1)), nullptr);

    for (TaskId id : {TaskId{2}, TaskId{3}}) {
        auto dependent = MakeUnique<GraphOnlyTask>(id);
        dependent->AddDependency(TaskId{1});
        EXPECT_EQ(graph.Register(std::move(dependent)), nullptr);
    }

    auto actions = graph.MarkCompleted(1, true);
    EXPECT_EQ(actions.ReadyTasks.size(), 2u);
}

TEST_F(TaskDependencyGraphTest, AbsentDependencyIsTreatedAsTerminal) {
    // F8: complete-and-erase the dependency first; the late dependent must
    // run instead of parking forever.
    ASSERT_NE(graph.Register(MakeUnique<GraphOnlyTask>(1)), nullptr);
    graph.MarkCompleted(1, true);

    auto dependent = MakeUnique<GraphOnlyTask>(2);
    dependent->AddDependency(TaskId{1});
    auto ready = graph.Register(std::move(dependent));
    ASSERT_NE(ready, nullptr);
    EXPECT_EQ(ready->GetTaskId(), 2u);
}

TEST_F(TaskDependencyGraphTest, FailurePropagatesToParkedDependentsAndErases) {
    ASSERT_NE(graph.Register(MakeUnique<GraphOnlyTask>(1)), nullptr);

    auto mid = MakeUnique<GraphOnlyTask>(2);
    mid->AddDependency(TaskId{1});
    ASSERT_EQ(graph.Register(std::move(mid)), nullptr);

    auto leaf = MakeUnique<GraphOnlyTask>(3);
    leaf->AddDependency(TaskId{2});
    ASSERT_EQ(graph.Register(std::move(leaf)), nullptr);

    auto actions = graph.MarkCompleted(1, false);
    ASSERT_EQ(actions.Events.size(), 3u);
    EXPECT_EQ(actions.Events[0].Id, 1u);
    EXPECT_EQ(actions.Events[0].NewStatus, TaskStatus::Failed);
    EXPECT_EQ(actions.Events[1].NewStatus, TaskStatus::Failed);
    EXPECT_EQ(actions.Events[2].NewStatus, TaskStatus::Failed);
    EXPECT_TRUE(actions.ReadyTasks.empty());
    EXPECT_EQ(actions.RetiredTasks.size(), 2u); // both parked payloads retired
    EXPECT_EQ(graph.GetTrackedTaskCountForTests(), 0u);
}

TEST_F(TaskDependencyGraphTest, CancelPendingRetiresPayloadAndCascades) {
    ASSERT_NE(graph.Register(MakeUnique<GraphOnlyTask>(1)), nullptr);

    auto mid = MakeUnique<GraphOnlyTask>(2);
    mid->AddDependency(TaskId{1});
    ASSERT_EQ(graph.Register(std::move(mid)), nullptr);

    auto leaf = MakeUnique<GraphOnlyTask>(3);
    leaf->AddDependency(TaskId{2});
    ASSERT_EQ(graph.Register(std::move(leaf)), nullptr);

    TaskGraphActions actions;
    ASSERT_TRUE(graph.CancelPending(2, actions));
    ASSERT_EQ(actions.Events.size(), 2u);
    EXPECT_EQ(actions.Events[0].Id, 2u);
    EXPECT_EQ(actions.Events[0].NewStatus, TaskStatus::Cancelled);
    EXPECT_EQ(actions.Events[1].Id, 3u);
    EXPECT_EQ(actions.Events[1].NewStatus, TaskStatus::Cancelled);
    EXPECT_EQ(actions.RetiredTasks.size(), 2u);
    EXPECT_EQ(graph.GetTrackedTaskCountForTests(), 1u); // only the dependency remains

    // Completing the dependency must not resurrect the cancelled dependents.
    auto completion = graph.MarkCompleted(1, true);
    EXPECT_TRUE(completion.ReadyTasks.empty());
    EXPECT_EQ(graph.GetTrackedTaskCountForTests(), 0u);
}

TEST_F(TaskDependencyGraphTest, CancelIsRefusedForRunningAndUnknownTasks) {
    ASSERT_NE(graph.Register(MakeUnique<GraphOnlyTask>(1)), nullptr);
    EXPECT_TRUE(graph.MarkRunning(1));

    TaskGraphActions actions;
    EXPECT_FALSE(graph.CancelPending(1, actions));  // running
    EXPECT_FALSE(graph.CancelPending(99, actions)); // never registered
    EXPECT_TRUE(actions.Events.empty());
}

TEST_F(TaskDependencyGraphTest, MarkRunningReportsMembership) {
    ASSERT_NE(graph.Register(MakeUnique<GraphOnlyTask>(1)), nullptr);
    EXPECT_TRUE(graph.MarkRunning(1));

    graph.MarkCompleted(1, true);
    EXPECT_FALSE(graph.MarkRunning(1)); // erased
    EXPECT_FALSE(graph.MarkRunning(42)); // never registered
}

#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
// F7: an absent dependency id must be RECENTLY-TERMINAL. A never-submitted id
// is a submit-order violation that would silently run the dependent early —
// the debug tombstone ring turns it into a loud abort.
TEST(TaskGraphDeathTest, NeverSubmittedDependencyTripsTombstoneAssert) {
    ASSERT_DEATH(
        {
            // Suppress the CRT abort dialog in the death-test child so the
            // stderr message is captured instead of hanging on a message box.
            GameEngine::Tests::DisableAbortDialogs();
            // The tombstone tripwire is loud-but-non-fatal by default (the
            // ring is a bounded heuristic; legal F8 usage must not crash a
            // Debug editor) — the hard abort is the opt-in strict mode.
            GameEngine::Tests::SetEnvVar("GE_JOB_TOMBSTONE_STRICT", "1");
            TaskDependencyGraph graph;
            auto dependent = MakeUnique<GraphOnlyTask>(2);
            dependent->AddDependency(TaskId{999});
            auto ready = graph.Register(std::move(dependent));
            (void)ready;
        },
        "submit-order violation");
}
#endif

// ========================================
// POOL-INTEGRATION COVERAGE
// ========================================

class TaskGraphPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Warning);
    }
};

/**
 * @brief B4 leak tripwire: 10k dependent chains must leave zero graph nodes.
 *
 * Pre-slice-2, the graph never erased nodes (~10s of MB per editor session)
 * and every completion rescanned every task ever submitted.
 */
TEST_F(TaskGraphPoolTest, GraphNodeCountReturnsToZeroAfterTenThousandChains) {
    WorkStealingThreadPool pool(4);

    constexpr int kChains = 10000;
    std::atomic<int> executed{0};
    std::vector<TaskHandle> tails;
    tails.reserve(kChains);

    for (int i = 0; i < kChains; ++i) {
        auto dep = MakeUnique<ScriptedTask>(&pool, [&executed] {
            executed.fetch_add(1, std::memory_order_relaxed);
        });
        auto dependent = MakeUnique<ScriptedTask>(&pool, [&executed] {
            executed.fetch_add(1, std::memory_order_relaxed);
        });
        dependent->AddDependency(dep->GetTaskId());

        pool.Submit(std::move(dep));
        tails.push_back(pool.Submit(std::move(dependent)));
    }

    for (auto& handle : tails) {
        handle.Wait();
    }

    EXPECT_EQ(executed.load(), kChains * 2);
    // Every dependent's node is erased before its handle flips Completed, so
    // once every tail has completed the graph must be empty.
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

/**
 * @brief Audit-finding pin: a dependent released because its dependency is
 * graph-terminal must observe the dependency's HANDLE as terminal too.
 *
 * The release decision (absent-dep at Register) is made under the graph
 * mutex, potentially by a different thread than the one that later flips the
 * dependency's TaskData in ProcessGraphActions — the guarded-TryGetResult /
 * IsCompleted contract at every production AddDependency site broke in that
 * gap. CompleteGraphTask's terminal-status pre-store closes it; this is the
 * probabilistic pin (back-to-back submits against 4 racing workers).
 */
TEST_F(TaskGraphPoolTest, DependencyHandleIsTerminalWhenReleasedDependentRuns) {
    WorkStealingThreadPool pool(4);

    constexpr int kIterations = 2000;
    std::atomic<int> violations{0};
    std::vector<TaskHandle> tails;
    tails.reserve(kIterations);

    for (int i = 0; i < kIterations; ++i) {
        auto dep = MakeUnique<ScriptedTask>(&pool, [] {});
        const TaskId depId = dep->GetTaskId();
        TaskHandle depHandle = pool.Submit(std::move(dep));

        auto dependent = MakeUnique<ScriptedTask>(&pool, [depHandle, &violations] {
            if (!depHandle.IsCompleted()) {
                violations.fetch_add(1, std::memory_order_relaxed);
            }
        });
        dependent->AddDependency(depId);
        tails.push_back(pool.Submit(std::move(dependent)));
    }

    for (auto& handle : tails) {
        handle.Wait();
    }
    EXPECT_EQ(violations.load(), 0);
}

/**
 * @brief Fan-in variant of the same pin: D depends on {A, B}. The worker
 * completing A can be preempted after erasing A's node but before flipping
 * A's TaskData, while the worker completing B extracts and enqueues D — D's
 * body must still observe BOTH dependency handles terminal.
 */
TEST_F(TaskGraphPoolTest, FanInDependencyHandlesAreTerminalWhenDependentRuns) {
    WorkStealingThreadPool pool(4);

    constexpr int kIterations = 1000;
    std::atomic<int> violations{0};
    std::vector<TaskHandle> tails;
    tails.reserve(kIterations);

    for (int i = 0; i < kIterations; ++i) {
        auto a = MakeUnique<ScriptedTask>(&pool, [] {});
        auto b = MakeUnique<ScriptedTask>(&pool, [] {});
        const TaskId aId = a->GetTaskId();
        const TaskId bId = b->GetTaskId();
        TaskHandle aHandle = pool.Submit(std::move(a));
        TaskHandle bHandle = pool.Submit(std::move(b));

        auto dependent = MakeUnique<ScriptedTask>(&pool, [aHandle, bHandle, &violations] {
            if (!aHandle.IsCompleted() || !bHandle.IsCompleted()) {
                violations.fetch_add(1, std::memory_order_relaxed);
            }
        });
        dependent->AddDependency(aId);
        dependent->AddDependency(bId);
        tails.push_back(pool.Submit(std::move(dependent)));
    }

    for (auto& handle : tails) {
        handle.Wait();
    }
    EXPECT_EQ(violations.load(), 0);
}

/**
 * @brief B8 race pin: a dependent submitted while its dependency completes
 * concurrently must ALWAYS run.
 *
 * Pre-slice-2 the dependent was rescued only by the per-completion global
 * rescan; with the rescan deleted, a TOCTOU regression parks it forever and
 * this test times out.
 */
TEST_F(TaskGraphPoolTest, SubmitDependentWhileDependencyCompletesStress) {
    WorkStealingThreadPool pool(4);

    constexpr int kIterations = 5000;
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> jitterUs(0, 3);

    for (int i = 0; i < kIterations; ++i) {
        std::atomic<bool> dependentRan{false};

        auto dep = MakeUnique<ScriptedTask>(&pool, [] {});
        const TaskId depId = dep->GetTaskId();

        auto dependent = MakeUnique<ScriptedTask>(&pool, [&dependentRan] {
            dependentRan.store(true, std::memory_order_release);
        });
        dependent->AddDependency(depId);

        // The dependency starts executing on a worker immediately; the jitter
        // straddles its completion so the dependent's Register races the
        // dependency's MarkCompleted from both sides.
        pool.Submit(std::move(dep));
        BusyWaitUs(jitterUs(rng));
        TaskHandle handle = pool.Submit(std::move(dependent));

        ASSERT_TRUE(WaitUntilDone(handle, std::chrono::seconds(10)))
            << "dependent stranded at iteration " << i;
        ASSERT_TRUE(dependentRan.load(std::memory_order_acquire))
            << "dependent marked done without running at iteration " << i;
    }

    // Eventually-consistent leak check: CompleteGraphTask pre-stores the
    // terminal status (handle reads done) BEFORE the graph critical section
    // erases the node, so the last dependent's node can still be tracked for
    // a moment after its handle flips. A genuine leak stays non-zero forever
    // and still fails here.
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetGraphTaskCountForTests() == 0; },
                          std::chrono::seconds(5)))
        << "graph retained " << pool.GetGraphTaskCountForTests() << " nodes after quiesce";
}

/**
 * @brief F10 pin: an OnComplete callback that Submits a dependency-managed
 * task must not deadlock.
 *
 * Pre-slice-2, completion callbacks fired UNDER the graph mutex; Submit from
 * a callback re-entered the graph and self-deadlocked.
 */
TEST_F(TaskGraphPoolTest, SubmitFromCompletionCallbackDoesNotDeadlock) {
    WorkStealingThreadPool pool(2);

    std::atomic<bool> gate{false};
    std::atomic<bool> innerRan{false};
    std::atomic<bool> innerSubmitted{false};
    TaskHandle innerHandle;

    auto outer = MakeUnique<ScriptedTask>(&pool, [&gate] {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    const TaskId outerId = outer->GetTaskId();
    TaskHandle outerHandle = pool.Submit(std::move(outer));

    // Registered before the gate opens, so the callback fires from the
    // worker's completion path.
    outerHandle.OnComplete([&, outerId] {
        auto inner = MakeUnique<ScriptedTask>(&pool, [&innerRan] {
            innerRan.store(true, std::memory_order_release);
        });
        // The outer task's node is erased by the time its callbacks fire —
        // this also exercises the absent-dependency path from a callback.
        inner->AddDependency(outerId);
        innerHandle = pool.Submit(std::move(inner));
        innerSubmitted.store(true, std::memory_order_release);
    });

    gate.store(true, std::memory_order_release);

    ASSERT_TRUE(WaitUntil([&] { return innerSubmitted.load(std::memory_order_acquire); },
                          std::chrono::seconds(10)))
        << "completion callback never ran (deadlock?)";
    ASSERT_TRUE(WaitUntilDone(innerHandle, std::chrono::seconds(10)));
    EXPECT_TRUE(innerRan.load(std::memory_order_acquire));
    EXPECT_FALSE(innerHandle.HasFailed());
}

/**
 * @brief F8: a dependent submitted after its dependency completed (and was
 * erased) runs immediately.
 */
TEST_F(TaskGraphPoolTest, DependentSubmittedAfterDependencyCompletedRunsImmediately) {
    WorkStealingThreadPool pool(2);

    auto dep = MakeUnique<ScriptedTask>(&pool, [] {});
    const TaskId depId = dep->GetTaskId();
    TaskHandle depHandle = pool.Submit(std::move(dep));
    depHandle.Wait();
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);

    std::atomic<bool> ran{false};
    auto dependent = MakeUnique<ScriptedTask>(&pool, [&ran] {
        ran.store(true, std::memory_order_release);
    });
    dependent->AddDependency(depId);
    TaskHandle handle = pool.Submit(std::move(dependent));

    ASSERT_TRUE(WaitUntilDone(handle, std::chrono::seconds(10)));
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_FALSE(handle.HasFailed());
}

/**
 * @brief Failure propagation preserved: a dependent registered BEFORE its
 * dependency fails observes the failure and never runs.
 */
TEST_F(TaskGraphPoolTest, DependentRegisteredBeforeDependencyFailureObservesFailure) {
    WorkStealingThreadPool pool(2);

    std::atomic<bool> gate{false};
    auto dep = MakeUnique<ScriptedTask>(&pool, [&gate] {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        throw std::runtime_error("dependency exploded");
    });
    const TaskId depId = dep->GetTaskId();
    TaskHandle depHandle = pool.Submit(std::move(dep));

    std::atomic<bool> ran{false};
    auto dependent = MakeUnique<ScriptedTask>(&pool, [&ran] {
        ran.store(true, std::memory_order_release);
    });
    dependent->AddDependency(depId);
    TaskHandle dependentHandle = pool.Submit(std::move(dependent)); // parked

    gate.store(true, std::memory_order_release);

    ASSERT_TRUE(WaitUntilDone(dependentHandle, std::chrono::seconds(10)));
    EXPECT_TRUE(depHandle.HasFailed());
    EXPECT_TRUE(dependentHandle.HasFailed());
    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    EXPECT_NE(dependentHandle.GetErrorMessage().find("Dependency task"), String::npos)
        << "propagated error message missing, got: " << dependentHandle.GetErrorMessage();
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

/**
 * @brief F8: a dependent submitted AFTER its dependency failed (and was
 * erased) runs anyway; its handle observes its own result, not the failure.
 */
TEST_F(TaskGraphPoolTest, DependentSubmittedAfterDependencyFailedRunsIndependently) {
    WorkStealingThreadPool pool(2);

    auto dep = MakeUnique<ScriptedTask>(&pool, [] {
        throw std::runtime_error("dependency exploded");
    });
    const TaskId depId = dep->GetTaskId();
    TaskHandle depHandle = pool.Submit(std::move(dep));
    ASSERT_TRUE(WaitUntilDone(depHandle, std::chrono::seconds(10)));
    EXPECT_TRUE(depHandle.HasFailed());
    // Poll, don't snapshot: the handle reads terminal via the status
    // PRE-STORE that precedes the graph critical section (CompleteGraphTask's
    // release-edge contract), so IsDone() can be observed a beat before the
    // executing worker's MarkCompleted erases the node. Under load that gap
    // is preemption-sized — an instantaneous count assert here is a flake.
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetGraphTaskCountForTests() == 0; },
                          std::chrono::seconds(10)))
        << "failed dependency's graph node never erased";

    std::atomic<bool> ran{false};
    auto dependent = MakeUnique<ScriptedTask>(&pool, [&ran] {
        ran.store(true, std::memory_order_release);
    });
    dependent->AddDependency(depId);
    TaskHandle handle = pool.Submit(std::move(dependent));

    ASSERT_TRUE(WaitUntilDone(handle, std::chrono::seconds(10)));
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_FALSE(handle.HasFailed());
    EXPECT_TRUE(handle.GetErrorMessage().empty());
}

/**
 * @brief Cancelling a parked dependent prevents its execution and does not
 * disturb the dependency's completion.
 */
TEST_F(TaskGraphPoolTest, CancelParkedDependentPreventsExecution) {
    WorkStealingThreadPool pool(2);

    std::atomic<bool> gate{false};
    auto dep = MakeUnique<ScriptedTask>(&pool, [&gate] {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    const TaskId depId = dep->GetTaskId();
    TaskHandle depHandle = pool.Submit(std::move(dep));

    std::atomic<bool> ran{false};
    auto dependent = MakeUnique<ScriptedTask>(&pool, [&ran] {
        ran.store(true, std::memory_order_release);
    });
    dependent->AddDependency(depId);
    const TaskId dependentId = dependent->GetTaskId();
    TaskHandle dependentHandle = pool.Submit(std::move(dependent)); // parked

    EXPECT_TRUE(pool.CancelTask(dependentId));
    EXPECT_TRUE(dependentHandle.IsDone());
    EXPECT_TRUE(dependentHandle.HasFailed()); // cancelled counts as failed

    gate.store(true, std::memory_order_release);
    depHandle.Wait();
    EXPECT_FALSE(depHandle.HasFailed());

    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);

    // Cancelling anything terminal or unknown reports false.
    EXPECT_FALSE(pool.CancelTask(dependentId));
    EXPECT_FALSE(pool.CancelTask(depId));
}

/**
 * @brief Perf evidence for the PR: 10k-task chained submit/complete.
 *
 * Pre-slice-2, every completion rescanned every task ever submitted under the
 * graph mutex (O(N²) overall — the B4 hot path); post-slice-2 each completion
 * touches only its direct dependents. The bound is deliberately generous for
 * loaded CI boxes; the printed timing is the real artifact.
 */
TEST_F(TaskGraphPoolTest, ChainedSubmitCompleteThroughputTenThousand) {
    WorkStealingThreadPool pool(4);

    constexpr int kChainLength = 10000;
    std::atomic<int> executed{0};

    const auto start = SteadyClock::now();

    TaskHandle last;
    TaskId previousId = 0;
    for (int i = 0; i < kChainLength; ++i) {
        auto task = MakeUnique<ScriptedTask>(&pool, [&executed] {
            executed.fetch_add(1, std::memory_order_relaxed);
        });
        if (previousId != 0) {
            task->AddDependency(previousId);
        }
        previousId = task->GetTaskId();
        last = pool.Submit(std::move(task));
    }
    last.Wait();

    const double ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();

    EXPECT_EQ(executed.load(), kChainLength);
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
    std::printf("[perf] %d-task chained submit/complete: %.1f ms\n", kChainLength, ms);
    EXPECT_LT(ms, 5000.0);
}

} // namespace GameEngine::Tests
