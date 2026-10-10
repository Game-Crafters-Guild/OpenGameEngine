// Slice-3 cancellation + shutdown-contract coverage: single-fire CAS cancel
// arbitration, the shutdown contract, and envelope destruction latency.
//
// Layers:
//  - CancellationTest: F12 — honest Cancel() for lambda-submitted tasks (B1),
//    cancelled-but-queued tasks never execute (B5), single-fire under racing
//    cancellers, the TileStreaming usage shape.
//  - ShutdownContractTest: F13 — waiters woken with Cancelled (B6), bare
//    barrier tasks executed not dropped (B7), sequential fallbacks, and the
//    create/storm/shutdown stress incl. mid-ParallelFor shutdown.

#include "Logger/Logger.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace JobSystem;

namespace GameEngine::Tests {

namespace {

using SteadyClock = std::chrono::steady_clock;

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

void BusyWaitUs(int us) {
    if (us <= 0) {
        return;
    }
    const auto deadline = SteadyClock::now() + std::chrono::microseconds(us);
    while (SteadyClock::now() < deadline) {
        std::this_thread::yield();
    }
}

// Opens a gate on scope exit so a failed ASSERT can never leave worker
// blockers spinning into the pool destructor's join.
struct GateGuard {
    std::atomic<bool>& Gate;
    ~GateGuard() { Gate.store(true, std::memory_order_release); }
};

/** Pool-level dependency-managed task whose body is supplied by the test. */
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

// Parks `count` workers on `gate`, returning once all of them are inside
// their blocker bodies (i.e. the pool cannot dequeue anything else).
void OccupyWorkers(WorkStealingThreadPool& pool, size_t count, std::atomic<bool>& gate,
                   std::atomic<size_t>& running) {
    for (size_t i = 0; i < count; ++i) {
        pool.EnqueueWork([&gate, &running] {
            running.fetch_add(1, std::memory_order_acq_rel);
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }
    ASSERT_TRUE(WaitUntil([&running, count] { return running.load() >= count; },
                          std::chrono::seconds(10)))
        << "workers never picked up the blocker tasks";
}

} // namespace

class CancellationTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Warning);
    }
};

/**
 * @brief B1 pin: Cancel() on a lambda-submitted (never graph-tracked) task is
 * real. Pre-slice-3 this was a silent no-op — the graph reported the id
 * unknown and the body always ran (TileStreamingManager's Cancel sites never
 * worked).
 */
TEST_F(CancellationTest, CancelQueuedLambdaTaskNeverExecutes) {
    WorkStealingThreadPool pool(2);

    // Occupy every worker so the victim tasks stay queued (Pending).
    std::atomic<bool> gate{false};
    GateGuard gateGuard{gate};
    std::atomic<size_t> blockersRunning{0};
    OccupyWorkers(pool, 2, gate, blockersRunning);

    constexpr int kTasks = 16;
    std::atomic<int> executed{0};
    std::array<std::atomic<int>, kTasks> failureFires{};
    std::array<std::atomic<int>, kTasks> completeFires{};
    std::vector<TaskHandle> handles;
    handles.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        handles.push_back(pool.Submit([&executed] { executed.fetch_add(1); }));
    }
    for (int i = 0; i < kTasks; ++i) {
        handles[static_cast<size_t>(i)].OnFailure(
            [&failureFires, i](const String&) { failureFires[static_cast<size_t>(i)].fetch_add(1); });
        handles[static_cast<size_t>(i)].OnComplete(
            [&completeFires, i] { completeFires[static_cast<size_t>(i)].fetch_add(1); });
    }

    for (auto& handle : handles) {
        EXPECT_TRUE(handle.Cancel()) << "Cancel refused a queued lambda task (B1 regression)";
    }
    // Second cancel loses the arbitration: the task is already Cancelled.
    for (auto& handle : handles) {
        EXPECT_FALSE(handle.Cancel());
    }
    for (auto& handle : handles) {
        EXPECT_TRUE(handle.IsDone());
        EXPECT_TRUE(handle.HasFailed());
        EXPECT_FALSE(handle.IsCompleted());
    }

    // Release the workers; they dequeue the cancelled envelopes, lose the
    // arbitration, and drop them (F14) — conserving both counters.
    gate.store(true, std::memory_order_release);
    EXPECT_TRUE(WaitUntil(
        [&pool] {
            return pool.GetPendingTasksApprox() == 0 && pool.GetApproximateQueueSize() == 0;
        },
        std::chrono::seconds(10)))
        << "cancelled envelopes did not drain: pending=" << pool.GetPendingTasksApprox()
        << " queued=" << pool.GetApproximateQueueSize();

    EXPECT_EQ(executed.load(), 0) << "a cancelled queued task executed anyway";
    for (int i = 0; i < kTasks; ++i) {
        EXPECT_EQ(failureFires[static_cast<size_t>(i)].load(), 1)
            << "Cancelled callbacks must fire exactly once (task " << i << ")";
        EXPECT_EQ(completeFires[static_cast<size_t>(i)].load(), 0);
    }

    // Debug builds: the Shutdown conservation tripwire must stay silent.
    pool.Shutdown();
}

/**
 * @brief F12 single-fire pin: N threads Cancel while workers dequeue. Every
 * task either executes normally XOR cancels with callbacks — never both,
 * never neither — and at most one Cancel() call per task reports success.
 */
TEST_F(CancellationTest, CancelRaceIsSingleFire) {
    WorkStealingThreadPool pool(4);

    constexpr int kRounds = 200;
    constexpr int kTasksPerRound = 32;
    constexpr int kCancellers = 4;

    for (int round = 0; round < kRounds; ++round) {
        std::array<std::atomic<int>, kTasksPerRound> ran{};
        std::array<std::atomic<int>, kTasksPerRound> cancelFires{};
        std::array<std::atomic<int>, kTasksPerRound> cancelWins{};
        std::vector<TaskHandle> handles;
        handles.reserve(kTasksPerRound);

        for (int i = 0; i < kTasksPerRound; ++i) {
            handles.push_back(
                pool.Submit([&ran, i] { ran[static_cast<size_t>(i)].fetch_add(1); }));
        }
        for (int i = 0; i < kTasksPerRound; ++i) {
            handles[static_cast<size_t>(i)].OnFailure([&cancelFires, i](const String&) {
                cancelFires[static_cast<size_t>(i)].fetch_add(1);
            });
        }

        std::vector<std::thread> cancellers;
        cancellers.reserve(kCancellers);
        for (int t = 0; t < kCancellers; ++t) {
            cancellers.emplace_back([&handles, &cancelWins] {
                for (size_t i = 0; i < handles.size(); ++i) {
                    TaskHandle handle = handles[i]; // copy: Cancel via shared TaskData
                    if (handle.Cancel()) {
                        cancelWins[i].fetch_add(1);
                    }
                }
            });
        }
        for (auto& t : cancellers) {
            t.join();
        }

        for (auto& handle : handles) {
            handle.Wait();
        }

        for (int i = 0; i < kTasksPerRound; ++i) {
            const size_t idx = static_cast<size_t>(i);
            const int executedCount = ran[idx].load();
            const int winCount = cancelWins[idx].load();
            const int fireCount = cancelFires[idx].load();
            ASSERT_LE(executedCount, 1) << "task executed twice (round " << round << ")";
            ASSERT_LE(winCount, 1) << "two Cancel() calls both won (round " << round << ")";
            ASSERT_LE(fireCount, 1) << "Cancelled callbacks double-fired (round " << round << ")";
            ASSERT_EQ(executedCount + winCount, 1)
                << "task " << i << " round " << round
                << ": executed=" << executedCount << " cancelWins=" << winCount
                << " — must be exactly one of {executed, cancelled}";
            ASSERT_EQ(winCount, fireCount)
                << "cancel win without callbacks (or callbacks without a win), task " << i;
        }
    }

    // Drop any still-queued cancelled envelopes through the workers, then
    // run the Debug conservation tripwire.
    EXPECT_TRUE(WaitUntil(
        [&pool] {
            return pool.GetPendingTasksApprox() == 0 && pool.GetApproximateQueueSize() == 0;
        },
        std::chrono::seconds(10)));
    pool.Shutdown();
}

/**
 * @brief The TileStreamingManager usage shape (B1's customer): a burst of
 * lambda jobs, half cancelled mid-flight during churn. Every job either ran or
 * reads Cancelled; a successful Cancel() means the body never ran; the pool
 * drains clean.
 */
TEST_F(CancellationTest, TileStreamingShapeCancelHalfMidFlight) {
    WorkStealingThreadPool pool(4);

    constexpr int kJobs = 64;
    std::array<std::atomic<int>, kJobs> ran{};
    std::vector<TaskHandle> handles;
    handles.reserve(kJobs);

    for (int i = 0; i < kJobs; ++i) {
        handles.push_back(pool.Submit([&ran, i] {
            BusyWaitUs(200); // tile-job-sized body so the queue stays busy
            ran[static_cast<size_t>(i)].fetch_add(1);
        }));
    }

    // Cancel every other job while the workers churn through the burst —
    // the TileStreamingManager camera-flythrough shape.
    std::array<bool, kJobs> cancelWon{};
    for (int i = 0; i < kJobs; i += 2) {
        cancelWon[static_cast<size_t>(i)] = handles[static_cast<size_t>(i)].Cancel();
    }

    for (auto& handle : handles) {
        handle.Wait();
    }
    EXPECT_TRUE(WaitUntil(
        [&pool] {
            return pool.GetPendingTasksApprox() == 0 && pool.GetApproximateQueueSize() == 0;
        },
        std::chrono::seconds(10)))
        << "pool did not drain clean after cancellation burst";

    int cancelled = 0;
    for (int i = 0; i < kJobs; ++i) {
        const size_t idx = static_cast<size_t>(i);
        if (cancelWon[idx]) {
            ++cancelled;
            EXPECT_EQ(ran[idx].load(), 0) << "cancelled-not-yet-running job " << i << " executed";
            EXPECT_TRUE(handles[idx].HasFailed());
            EXPECT_FALSE(handles[idx].IsCompleted());
        } else {
            EXPECT_EQ(ran[idx].load(), 1) << "job " << i << " lost without cancellation";
            EXPECT_TRUE(handles[idx].IsCompleted());
        }
    }
    std::printf("[TileStreamingShape] %d/%d cancel attempts won the race\n", cancelled, kJobs / 2);

    pool.Shutdown();
}

/**
 * @brief Slice-2 compose check: cancelling a parked graph dependent fires its
 * Cancelled callbacks exactly once under the new CAS arbitration, and the
 * second cancel is refused.
 */
TEST_F(CancellationTest, CancelParkedDependentFiresExactlyOnce) {
    WorkStealingThreadPool pool(2);

    std::atomic<bool> gate{false};
    GateGuard gateGuard{gate};
    auto dep = MakeUnique<ScriptedTask>(&pool, [&gate] {
        while (!gate.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    const TaskId depId = dep->GetTaskId();
    TaskHandle depHandle = pool.Submit(std::move(dep));

    std::atomic<bool> dependentRan{false};
    auto dependent = MakeUnique<ScriptedTask>(&pool, [&dependentRan] {
        dependentRan.store(true, std::memory_order_release);
    });
    dependent->AddDependency(depId);
    TaskHandle dependentHandle = pool.Submit(std::move(dependent)); // parked

    std::atomic<int> failureFires{0};
    dependentHandle.OnFailure([&failureFires](const String&) { failureFires.fetch_add(1); });

    EXPECT_TRUE(dependentHandle.Cancel());
    EXPECT_FALSE(dependentHandle.Cancel()); // arbitration already decided
    EXPECT_TRUE(dependentHandle.IsDone());
    EXPECT_TRUE(dependentHandle.HasFailed());
    EXPECT_EQ(failureFires.load(), 1);

    gate.store(true, std::memory_order_release);
    depHandle.Wait();
    EXPECT_FALSE(depHandle.HasFailed());
    EXPECT_FALSE(dependentRan.load(std::memory_order_acquire));
    EXPECT_EQ(failureFires.load(), 1) << "dependency completion re-fired the cancelled dependent";
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);

    pool.Shutdown();
}

class ShutdownContractTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::Log::SetLogLevel(Logger::LogLevel::Warning);
    }
};

/**
 * @brief B6 pin: waiters blocked in TaskHandle::Wait on queued AND
 * graph-parked tasks all return promptly with Cancelled when the pool shuts
 * down. Pre-fix, parked payloads were dropped without flipping their TaskData
 * and these waiters hung forever.
 */
TEST_F(ShutdownContractTest, ShutdownWakesAllWaiters) {
    WorkStealingThreadPool pool(2);

    // Blockers occupy both workers until shutdown begins, so the victim
    // tasks stay queued/parked. They poll the pool's own flag: Shutdown's
    // join then completes naturally.
    std::atomic<size_t> blockersRunning{0};
    for (int i = 0; i < 2; ++i) {
        pool.EnqueueWork([&pool, &blockersRunning] {
            blockersRunning.fetch_add(1, std::memory_order_acq_rel);
            while (!pool.IsShuttingDown()) {
                std::this_thread::yield();
            }
        });
    }
    ASSERT_TRUE(WaitUntil([&blockersRunning] { return blockersRunning.load() >= 2; },
                          std::chrono::seconds(10)));

    // Victim 1: queued lambda task (never dequeued — workers are busy).
    std::atomic<bool> queuedRan{false};
    TaskHandle queuedHandle = pool.Submit([&queuedRan] { queuedRan.store(true); });

    // Victims 2+3: a queued graph task and a dependent parked on it.
    auto dep = MakeUnique<ScriptedTask>(&pool, [] {});
    const TaskId depId = dep->GetTaskId();
    TaskHandle depHandle = pool.Submit(std::move(dep));
    std::atomic<bool> dependentRan{false};
    auto dependent = MakeUnique<ScriptedTask>(&pool, [&dependentRan] {
        dependentRan.store(true, std::memory_order_release);
    });
    dependent->AddDependency(depId);
    TaskHandle dependentHandle = pool.Submit(std::move(dependent)); // parked

    // Waiters block in TaskHandle::Wait before Shutdown is requested.
    std::array<std::atomic<bool>, 3> waiterReturned{};
    std::vector<std::thread> waiters;
    waiters.reserve(3);
    TaskHandle waitTargets[3] = {queuedHandle, depHandle, dependentHandle};
    for (int i = 0; i < 3; ++i) {
        waiters.emplace_back([&waiterReturned, i, handle = waitTargets[i]]() mutable {
            handle.Wait();
            waiterReturned[static_cast<size_t>(i)].store(true, std::memory_order_release);
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let waiters park

    const auto t0 = SteadyClock::now();
    pool.Shutdown();
    const bool allWoke = WaitUntil(
        [&waiterReturned] {
            return waiterReturned[0].load() && waiterReturned[1].load() && waiterReturned[2].load();
        },
        std::chrono::seconds(10));
    const double wakeMs =
        std::chrono::duration<double, std::milli>(SteadyClock::now() - t0).count();

    EXPECT_TRUE(allWoke) << "a waiter is still blocked after Shutdown (B6 regression)";
    std::printf("[ShutdownWakesAllWaiters] shutdown+wake completed in %.2fms\n", wakeMs);
    for (auto& t : waiters) {
        t.join(); // safe: allWoke false would have failed above; join to avoid terminate
    }

    EXPECT_FALSE(queuedRan.load());
    EXPECT_FALSE(dependentRan.load(std::memory_order_acquire));
    for (TaskHandle& handle : waitTargets) {
        EXPECT_TRUE(handle.IsDone());
        EXPECT_TRUE(handle.HasFailed());     // Cancelled reads as failed
        EXPECT_FALSE(handle.IsCompleted());  // ...and never as completed
    }
    EXPECT_EQ(pool.GetGraphTaskCountForTests(), 0u);
}

/**
 * @brief B7 pin: bare EnqueueWork tasks enqueued around Shutdown always
 * execute — the hand-rolled caller barrier (the DispatchAndWait/ParallelFor
 * shape) must always release, before, during, and after shutdown.
 */
TEST_F(ShutdownContractTest, ShutdownExecutesBareBarrierTasks) {
    WorkStealingThreadPool pool(4);

    std::atomic<bool> stop{false};
    std::atomic<bool> barrierHung{false};
    std::atomic<int> roundsCompleted{0};
    std::atomic<int> roundsAfterShutdown{0};

    std::thread storm([&] {
        while (!stop.load(std::memory_order_acquire)) {
            constexpr uint32_t kBatch = 8;
            const bool startedAfterShutdown = pool.IsShuttingDown();
            std::atomic<uint32_t> remaining{kBatch};
            std::mutex mtx;
            std::condition_variable cv;
            for (uint32_t i = 0; i < kBatch; ++i) {
                pool.EnqueueWork([&remaining, &mtx, &cv] {
                    std::lock_guard<std::mutex> lock(mtx);
                    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        cv.notify_one();
                    }
                });
            }
            std::unique_lock<std::mutex> lock(mtx);
            const bool released = cv.wait_for(lock, std::chrono::seconds(10), [&remaining] {
                return remaining.load(std::memory_order_acquire) == 0;
            });
            if (!released) {
                barrierHung.store(true);
                return;
            }
            roundsCompleted.fetch_add(1);
            if (startedAfterShutdown) {
                roundsAfterShutdown.fetch_add(1);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    pool.Shutdown();
    // Let a few post-shutdown rounds prove the inline-execution path too.
    ASSERT_TRUE(WaitUntil([&roundsAfterShutdown, &barrierHung] {
                              return roundsAfterShutdown.load() >= 3 || barrierHung.load();
                          },
                          std::chrono::seconds(10)));
    stop.store(true, std::memory_order_release);
    storm.join();

    EXPECT_FALSE(barrierHung.load())
        << "a bare barrier task was dropped across Shutdown (B7 regression)";
    EXPECT_GT(roundsCompleted.load(), 0);
    std::printf("[ShutdownExecutesBareBarrierTasks] rounds=%d (post-shutdown=%d)\n",
                roundsCompleted.load(), roundsAfterShutdown.load());
}

/**
 * @brief F13b: ParallelFor / DispatchAndWait against a shut-down pool fall
 * back to sequential execution on the caller — correct results, no hang.
 */
TEST_F(ShutdownContractTest, ParallelForDuringShutdownFallsBackSequential) {
    WorkStealingThreadPool pool(4);
    pool.Shutdown();

    constexpr size_t kCount = 100000;
    std::vector<int> data(kCount, 0);
    JobSystem::ParallelFor(
        &pool, 0, kCount,
        [&data](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                data[i] = static_cast<int>(i);
            }
        },
        1024);
    for (size_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(data[i], static_cast<int>(i)) << "ParallelFor fallback skipped index " << i;
    }

    std::atomic<int> dispatched{0};
    std::function<void()> tasks[4];
    for (auto& task : tasks) {
        task = [&dispatched] { dispatched.fetch_add(1); };
    }
    JobSystem::DispatchAndWait(&pool, tasks, 4);
    EXPECT_EQ(dispatched.load(), 4);

    std::vector<int> toSort(20000);
    std::mt19937 rng(99);
    for (auto& v : toSort) {
        v = static_cast<int>(rng());
    }
    JobSystem::ParallelSort(&pool, toSort.begin(), toSort.end(), 1000);
    EXPECT_TRUE(std::is_sorted(toSort.begin(), toSort.end()));
}

/**
 * @brief Shutdown-under-load stress: create/storm/shutdown cycles with a
 * racing ParallelFor caller, racing lambda submits + cancels, and graph
 * chains. No hang, no crash; Debug tripwires must stay silent.
 */
TEST_F(ShutdownContractTest, ShutdownWhileStormingStress) {
    constexpr int kCycles = 50;
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> stormUs(0, 2000);

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        auto pool = std::make_unique<WorkStealingThreadPool>(4);
        std::atomic<bool> stop{false};

        // Racing fork-join caller: must never hang, even when Shutdown lands
        // mid-ParallelFor (chunks execute via workers, the post-join drain,
        // or the enqueuer's self-drain).
        std::thread forkJoin([&] {
            std::vector<int> buffer(20000, 0);
            while (!stop.load(std::memory_order_acquire)) {
                JobSystem::ParallelFor(
                    pool.get(), 0, buffer.size(),
                    [&buffer](size_t begin, size_t end) {
                        for (size_t i = begin; i < end; ++i) {
                            ++buffer[i];
                        }
                    },
                    1000);
            }
        });

        // Racing lambda submit + cancel churn (the TileStreaming regime).
        std::thread submitter([&] {
            while (!stop.load(std::memory_order_acquire)) {
                TaskHandle a = pool->Submit([] { BusyWaitUs(5); });
                TaskHandle b = pool->Submit([] { BusyWaitUs(5); });
                b.Cancel();
                (void)a;
            }
        });

        // Graph chains on this thread until the storm window closes.
        const auto stormDeadline =
            SteadyClock::now() + std::chrono::microseconds(stormUs(rng));
        while (SteadyClock::now() < stormDeadline) {
            auto dep = MakeUnique<ScriptedTask>(pool.get(), [] {});
            auto dependent = MakeUnique<ScriptedTask>(pool.get(), [] {});
            dependent->AddDependency(dep->GetTaskId());
            pool->Submit(std::move(dep));
            pool->Submit(std::move(dependent));
        }

        pool->Shutdown();
        stop.store(true, std::memory_order_release);
        forkJoin.join();
        submitter.join();
        pool.reset();
    }
    SUCCEED() << kCycles << " create/storm/shutdown cycles completed";
}

/**
 * @brief F13e pin: every VALID handle obtained from Submit across a racing
 * Shutdown() is waitable — Wait() returns instead of stranding. A Submit can
 * pass the gate just before the flag flips and the whole shutdown sequence
 * (drain, graph retire, registry sweep) can complete before its publish
 * lands: the fresh TaskData then postdates the sweep's snapshot and the
 * envelope sits in a queue nobody drains. The submitter's post-publish
 * re-check must cancel its own task. Exercises all three gated submit arms
 * (void lambda, non-void lambda, graph dep/dependent pair — the last also
 * covers the graph-PARKED payload no drain can reach).
 * ShutdownWhileStormingStress never Wait()s on its handles, which is exactly
 * why this hole was invisible to it.
 */
TEST_F(ShutdownContractTest, SubmitRacingShutdownNeverStrandsWaiter) {
    constexpr int kCycles = 40;
    constexpr int kSubmitters = 4;
    std::mt19937 rng(0xF13E);
    std::uniform_int_distribution<int> stormUs(0, 1500);

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        auto pool = std::make_unique<WorkStealingThreadPool>(2);
        std::atomic<bool> stop{false};
        std::atomic<int> submittersDone{0};

        std::vector<std::thread> submitters;
        submitters.reserve(kSubmitters);
        for (int t = 0; t < kSubmitters; ++t) {
            submitters.emplace_back([&] {
                while (!stop.load(std::memory_order_acquire)) {
                    TaskHandle voidHandle = pool->Submit([] {});
                    TaskHandle valueHandle = pool->Submit([] { return 42; });

                    auto dep = MakeUnique<ScriptedTask>(pool.get(), [] {});
                    auto dependent = MakeUnique<ScriptedTask>(pool.get(), [] {});
                    dependent->AddDependency(dep->GetTaskId());
                    TaskHandle depHandle = pool->Submit(std::move(dep));
                    TaskHandle dependentHandle = pool->Submit(std::move(dependent));

                    for (TaskHandle* handle :
                         {&voidHandle, &valueHandle, &depHandle, &dependentHandle}) {
                        if (handle->IsValid()) {
                            handle->Wait(); // the pre-F13e strand point
                        }
                    }
                }
                submittersDone.fetch_add(1, std::memory_order_acq_rel);
            });
        }

        BusyWaitUs(stormUs(rng));
        pool->Shutdown();
        stop.store(true, std::memory_order_release);

        // Deadline before the joins: a stranded Wait() must FAIL the test
        // with a message, not hang it silently. (The joins below still hang
        // on a genuine strand — the harness timeout reaps the process after
        // the failure text is out, same convention as ShutdownWakesAllWaiters.)
        EXPECT_TRUE(WaitUntil(
            [&submittersDone] { return submittersDone.load() == kSubmitters; },
            std::chrono::seconds(20)))
            << "a submitter stranded in Wait() across Shutdown (F13e regression), cycle "
            << cycle;
        for (auto& t : submitters) {
            t.join();
        }
        pool.reset();
    }
}

/**
 * @brief Two-phase drain pin (F13d): a bare task that Wait()s on a handle
 * task sitting in another queue must not wedge Shutdown's single-threaded
 * drain. The drain cancels every handle/graph envelope BEFORE executing any
 * bare task, so the Wait() observes Cancelled and returns. Pre-fix, the
 * drain executed bare tasks inline as it dequeued them while the handle
 * envelope was still queued — and with the workers already joined, nothing
 * could ever complete it: Shutdown hung forever.
 */
TEST_F(ShutdownContractTest, DrainedBareTaskWaitingOnQueuedHandleTaskReturns) {
    WorkStealingThreadPool pool(2);

    // Pin both workers until shutdown so both victim tasks stay queued and
    // deterministically reach the post-join drain.
    std::atomic<size_t> blockersRunning{0};
    for (int i = 0; i < 2; ++i) {
        pool.EnqueueWork([&pool, &blockersRunning] {
            blockersRunning.fetch_add(1, std::memory_order_acq_rel);
            while (!pool.IsShuttingDown()) {
                std::this_thread::yield();
            }
        });
    }
    ASSERT_TRUE(WaitUntil([&blockersRunning] { return blockersRunning.load() >= 2; },
                          std::chrono::seconds(10)));

    // Handle task, published to the external/global queue — which the drain
    // reaches AFTER the lock-free queue the bare task lands in (the exact
    // pre-fix wedge order).
    std::atomic<bool> victimRan{false};
    auto victim = MakeUnique<ScriptedTask>(&pool, [&victimRan] { victimRan.store(true); });
    TaskHandle victimHandle = pool.Submit(std::move(victim));

    // Bare task that joins the handle task — the caller-barrier-body shape.
    std::atomic<bool> bareReturned{false};
    std::atomic<bool> observedCancelled{false};
    pool.EnqueueWork([victimCopy = victimHandle, &bareReturned, &observedCancelled]() mutable {
        victimCopy.Wait(); // pre-fix: wedges the drain forever
        observedCancelled.store(victimCopy.HasFailed() && !victimCopy.IsCompleted());
        bareReturned.store(true, std::memory_order_release);
    });

    pool.Shutdown(); // runs the drain on this thread

    EXPECT_TRUE(bareReturned.load(std::memory_order_acquire))
        << "the drained bare task never executed (B7 regression)";
    EXPECT_TRUE(observedCancelled.load())
        << "the queued handle task was not cancelled before the bare task ran";
    EXPECT_FALSE(victimRan.load());
    EXPECT_TRUE(victimHandle.IsDone());
    EXPECT_TRUE(victimHandle.HasFailed());
}

} // namespace GameEngine::Tests
