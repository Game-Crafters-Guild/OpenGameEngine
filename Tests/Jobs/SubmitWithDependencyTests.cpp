// Submit(F&&, std::span<const TaskHandle>) — the lambda form of the
// dependency-graph path (overhaul spec §2.1, API-polish batch).
//
// Covers: lambda chains and fan-in (including the mirror-node bridge for
// plain-Submit lambda dependencies), F8 already-terminal deps, failure
// cascade from a lambda dependency, cancel cascade to lambda dependents,
// cancel of a parked dependent, mixed lambda/Task-subclass dependencies,
// mirror-node erasure (the B4 leak tripwire extended to mirrors), and the
// deps overload racing Shutdown (F13 coverage).

#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace GameEngine::Tests {

namespace {

std::span<const JobSystem::TaskHandle> DepSpan(const JobSystem::TaskHandle& handle)
{
    return {&handle, 1};
}

// Minimal Task subclass for the mixed lambda/subclass dependency test.
class ScriptedGraphTask final : public JobSystem::Task
{
  public:
    ScriptedGraphTask(JobSystem::WorkStealingThreadPool* pool, std::function<void()> fn)
        : Task(pool), m_Fn(std::move(fn))
    {
    }

    void Execute() override { m_Fn(); }

  private:
    std::function<void()> m_Fn;
};

// Bounded terminal poll: a strand must FAIL the test, not hang the suite
// (TaskHandle::Wait on a stranded dependent never returns).
bool PollDone(const JobSystem::TaskHandle& handle,
              std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!handle.IsDone())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

} // namespace

// A -> B -> C, all lambdas. A is a plain Submit (dependency-exempt before
// this overload); B and C ride the graph and must observe strict ordering
// and the dependency results.
TEST(SubmitWithDependencyTest, LambdaChainRunsInOrder)
{
    JobSystem::WorkStealingThreadPool pool(4);

    std::atomic<int> stage{0};
    auto a = pool.Submit(
        [&stage]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            int expected = 0;
            stage.compare_exchange_strong(expected, 1);
            return 10;
        });
    auto b = pool.Submit(
        [&stage]
        {
            int expected = 1;
            return stage.compare_exchange_strong(expected, 2) ? 20 : -1;
        },
        DepSpan(a));
    auto c = pool.Submit(
        [&stage]
        {
            int expected = 2;
            return stage.compare_exchange_strong(expected, 3) ? 30 : -1;
        },
        DepSpan(b));

    c.Wait();
    EXPECT_EQ(stage.load(), 3);

    int result = 0;
    ASSERT_TRUE(c.TryGetResult(result));
    EXPECT_EQ(result, 30);
    ASSERT_TRUE(b.TryGetResult(result));
    EXPECT_EQ(result, 20);
    EXPECT_TRUE(a.IsCompleted());

    // Graph nodes (including A's mirror) are erased eagerly at terminal.
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

// Fan-in: the join runs only after EVERY lambda dependency completed.
TEST(SubmitWithDependencyTest, FanInWaitsForAllLambdaDeps)
{
    JobSystem::WorkStealingThreadPool pool(4);

    constexpr int kDeps = 8;
    std::atomic<int> completed{0};
    std::vector<JobSystem::TaskHandle> deps;
    deps.reserve(kDeps);
    for (int i = 0; i < kDeps; ++i)
    {
        deps.push_back(pool.Submit(
            [&completed, i]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1 + (i % 4)));
                completed.fetch_add(1, std::memory_order_relaxed);
            }));
    }

    std::atomic<int> seenAtJoin{-1};
    auto join = pool.Submit(
        [&completed, &seenAtJoin] { seenAtJoin.store(completed.load()); },
        std::span<const JobSystem::TaskHandle>(deps.data(), deps.size()));

    join.Wait();
    EXPECT_EQ(seenAtJoin.load(), kDeps);
    EXPECT_TRUE(join.IsCompleted());
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

// F8: already-terminal dependencies (Completed, Failed, and a born-terminal
// CreateCompletedHandle) add no edge — the dependent RUNS and observes the
// outcomes through the handles. This must also stay quiet under the F7
// tombstone diagnostic (terminal lambda ids never had graph tombstones).
TEST(SubmitWithDependencyTest, AlreadyTerminalDependenciesRunDependent)
{
    JobSystem::WorkStealingThreadPool pool(2);

    auto done = pool.Submit([] { return 1; });
    done.Wait();
    auto failed = pool.Submit([]() -> int { throw std::runtime_error("intentional dep failure"); });
    failed.Wait();
    ASSERT_TRUE(failed.HasFailed());
    auto born = pool.CreateCompletedHandle(7);

    const std::array<JobSystem::TaskHandle, 3> deps{done, failed, born};
    std::atomic<bool> ran{false};
    auto dependent =
        pool.Submit([&ran] { ran.store(true); }, std::span<const JobSystem::TaskHandle>(deps));

    dependent.Wait();
    EXPECT_TRUE(ran.load());
    EXPECT_TRUE(dependent.IsCompleted());
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

// F12 cancel cascade: cancelling a still-queued lambda dependency cascades
// Cancelled to its graph dependents, synchronously on the cancelling thread.
TEST(SubmitWithDependencyTest, CancelCascadesToLambdaDependents)
{
    JobSystem::WorkStealingThreadPool pool(1);

    std::atomic<bool> release{false};
    pool.EnqueueWork(
        [&release]
        {
            while (!release.load())
            {
                std::this_thread::yield();
            }
        });

    std::atomic<bool> depRan{false};
    std::atomic<bool> dependentRan{false};
    auto dep = pool.Submit([&depRan] { depRan.store(true); }); // queued behind the blocker
    auto dependent = pool.Submit([&dependentRan] { dependentRan.store(true); }, DepSpan(dep));

    ASSERT_TRUE(dep.Cancel()); // Pending — the canceller wins the arbitration

    // The cascade fires on the cancelling thread before Cancel's caller
    // regains control (F10 event processing after unlock, same thread).
    EXPECT_TRUE(dependent.IsDone());
    EXPECT_TRUE(dependent.HasFailed()); // Cancelled reads as failed
    EXPECT_FALSE(dependent.IsCompleted());

    release.store(true);
    dependent.Wait();
    EXPECT_FALSE(depRan.load());
    EXPECT_FALSE(dependentRan.load());
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

// Failure cascade: a lambda dependency that throws flips its parked
// dependents Failed (with the dependency-failure message), and the dependent
// body never runs.
TEST(SubmitWithDependencyTest, FailureCascadesFromLambdaDependency)
{
    JobSystem::WorkStealingThreadPool pool(1);

    std::atomic<bool> release{false};
    pool.EnqueueWork(
        [&release]
        {
            while (!release.load())
            {
                std::this_thread::yield();
            }
        });

    auto dep = pool.Submit([]() { throw std::runtime_error("dep failure"); });
    std::atomic<bool> dependentRan{false};
    auto dependent = pool.Submit([&dependentRan] { dependentRan.store(true); }, DepSpan(dep));

    release.store(true);
    dependent.Wait();

    EXPECT_TRUE(dep.HasFailed());
    EXPECT_TRUE(dependent.HasFailed());
    EXPECT_FALSE(dependent.IsCompleted());
    EXPECT_FALSE(dependentRan.load());
    EXPECT_NE(dependent.GetErrorMessage().find("Dependency task"), JobSystem::String::npos);
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

// Cancelling the PARKED dependent itself is the inherited graph behavior:
// the parked payload is retired, the dependency still completes normally.
TEST(SubmitWithDependencyTest, CancelOfParkedDependentIsInherited)
{
    JobSystem::WorkStealingThreadPool pool(1);

    std::atomic<bool> release{false};
    pool.EnqueueWork(
        [&release]
        {
            while (!release.load())
            {
                std::this_thread::yield();
            }
        });

    std::atomic<bool> depRan{false};
    std::atomic<bool> dependentRan{false};
    auto dep = pool.Submit([&depRan] { depRan.store(true); });
    auto dependent = pool.Submit([&dependentRan] { dependentRan.store(true); }, DepSpan(dep));

    ASSERT_TRUE(dependent.Cancel()); // parked — cancellable

    release.store(true);
    dep.Wait();
    EXPECT_TRUE(dep.IsCompleted());
    EXPECT_TRUE(depRan.load());
    EXPECT_TRUE(dependent.HasFailed());
    EXPECT_FALSE(dependentRan.load());
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

// Mixed dependencies: one plain-lambda handle (mirror-bridged) and one
// Task-subclass handle (natively tracked) gate the same dependent.
TEST(SubmitWithDependencyTest, MixedLambdaAndSubclassDependencies)
{
    JobSystem::WorkStealingThreadPool pool(1);

    std::atomic<bool> release{false};
    pool.EnqueueWork(
        [&release]
        {
            while (!release.load())
            {
                std::this_thread::yield();
            }
        });

    std::atomic<int> depsDone{0};
    auto lambdaDep =
        pool.Submit([&depsDone] { depsDone.fetch_add(1, std::memory_order_relaxed); });
    auto subclassDep = pool.Submit(JobSystem::MakeUnique<ScriptedGraphTask>(
        &pool, [&depsDone] { depsDone.fetch_add(1, std::memory_order_relaxed); }));

    std::atomic<int> seenAtRun{-1};
    const std::array<JobSystem::TaskHandle, 2> deps{lambdaDep, subclassDep};
    auto dependent = pool.Submit([&depsDone, &seenAtRun] { seenAtRun.store(depsDone.load()); },
                                 std::span<const JobSystem::TaskHandle>(deps));

    release.store(true);
    dependent.Wait();
    EXPECT_EQ(seenAtRun.load(), 2);
    EXPECT_TRUE(dependent.IsCompleted());
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

// Churn pin for the mirror-node bridge: the dependency frequently completes
// WHILE the dependent is being linked (the Dekker window), and every mirror
// must still be erased — the B4 leak tripwire extended to mirror nodes.
TEST(SubmitWithDependencyTest, MirrorNodesAreErasedUnderChurn)
{
    JobSystem::WorkStealingThreadPool pool(4);

    constexpr int kIterations = 2000;
    std::atomic<int> ran{0};
    for (int i = 0; i < kIterations; ++i)
    {
        auto a = pool.Submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        auto b = pool.Submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, DepSpan(a));
        b.Wait();
    }
    EXPECT_EQ(ran.load(), kIterations * 2);
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

// F13: the deps overload racing Shutdown must never strand a waiter — every
// valid handle reaches a terminal state (Completed or Cancelled), and
// post-gate submits return invalid handles.
TEST(SubmitWithDependencyTest, DepsOverloadRacingShutdownNeverStrands)
{
    for (int round = 0; round < 20; ++round)
    {
        auto pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);

        std::atomic<bool> stop{false};
        std::vector<JobSystem::TaskHandle> handles;
        std::thread submitter(
            [&]
            {
                JobSystem::TaskHandle prev;
                while (!stop.load())
                {
                    JobSystem::TaskHandle h;
                    if (prev.IsValid())
                    {
                        h = pool->Submit([] {}, DepSpan(prev));
                    }
                    else
                    {
                        h = pool->Submit([] { return 1; });
                    }
                    if (h.IsValid())
                    {
                        handles.push_back(h);
                    }
                    prev = h;
                }
            });

        std::this_thread::sleep_for(std::chrono::microseconds(200 + round * 137));
        pool->Shutdown();
        stop.store(true);
        submitter.join();

        for (auto& handle : handles)
        {
            handle.Wait(); // a strand = test-level timeout
            EXPECT_TRUE(handle.IsDone());
        }
        pool.reset();
    }
}

// The cascade-retire window: a GRAPH task's node is erased by a failure
// cascade UNDER the graph mutex (PropagateFailureLocked), but its TaskData
// flips terminal AFTERWARDS, outside the mutex (ProcessGraphActions' event
// loop). A Submit(fn, deps) that links such a handle inside that window sees
// node-absent + status-Pending — the one shape where "absent" does NOT mean
// terminal. The linker must NOT resurrect a mirror node for it: a graph
// task's completion path is already spent when its node dies, so nothing
// would ever complete the mirror and the dependent would park forever
// (audit find: mirror-resurrection strand). The dependent must always reach
// a terminal state — run under F8 or cascade, never hang.
//
// The window is widened deterministically: TWO graph tasks park behind a
// failing root; the cascade erases BOTH nodes in one critical section, then
// processes their Failed events serially — a slow OnFailure callback on each
// holds the worker mid-loop, leaving the OTHER task node-absent + Pending
// for the whole callback. Linking both from here lands one Submit squarely
// in the window every iteration.
TEST(SubmitWithDependencyTest, GraphDepCascadeRetireRaceDoesNotStrandDependent)
{
    JobSystem::WorkStealingThreadPool pool(2);

    constexpr int kIterations = 25;
    for (int i = 0; i < kIterations; ++i)
    {
        std::atomic<bool> releaseRoot{false};
        auto root = pool.Submit(
            [&releaseRoot]
            {
                while (!releaseRoot.load())
                {
                    std::this_thread::yield();
                }
                throw std::runtime_error("intentional root failure");
            });
        auto middleA = pool.Submit([] {}, DepSpan(root));
        auto middleB = pool.Submit([] {}, DepSpan(root));
        middleA.OnFailure(
            [](const JobSystem::String&) { std::this_thread::sleep_for(std::chrono::milliseconds(60)); });
        middleB.OnFailure(
            [](const JobSystem::String&) { std::this_thread::sleep_for(std::chrono::milliseconds(60)); });

        releaseRoot.store(true);
        // Land inside the first middle's failure callback: the other middle
        // is node-absent + still Pending right now.
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        auto dependentA = pool.Submit([] {}, DepSpan(middleA));
        auto dependentB = pool.Submit([] {}, DepSpan(middleB));

        ASSERT_TRUE(PollDone(dependentA)) << "iteration " << i
                                          << ": dependent stranded on a resurrected mirror node";
        ASSERT_TRUE(PollDone(dependentB)) << "iteration " << i
                                          << ": dependent stranded on a resurrected mirror node";
        ASSERT_TRUE(PollDone(middleA));
        ASSERT_TRUE(PollDone(middleB));
    }

    // All handles are terminal; the last worker-side node erase can lag the
    // terminal pre-store by nanoseconds — poll instead of a raw read.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (pool.GetGraphTaskCountForTests() != 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

} // namespace GameEngine::Tests
