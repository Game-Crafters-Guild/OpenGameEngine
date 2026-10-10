// JobChannel and the pool's blocking threads (jobsystem design slice S6).
//
// Layers:
//  - JobChannelTest: channel jobs run on blocking threads and never on the
//    compute workers; the cap holds under many producers; queued jobs hold no
//    thread; cancellation releases exactly the slot a job took; handles are
//    graph dependencies; inline pools run at Submit; the pool's budget caps
//    the blocking threads; an idle blocking thread holds no registry entry.
//  - JobChannelEnqueueTest (slice S7): Enqueue jobs are never dropped. A
//    destroyed channel hands its queued Enqueue jobs on to the blocking
//    threads in order; after the pool's Shutdown() an Enqueue runs on its
//    caller; inline pools run at Enqueue.
//  - JobChannelShutdownTest: the shutdown order. Queued channel Submit jobs
//    are cancelled and queued Enqueue jobs run on the thread that called
//    Shutdown(), after every queued handle was cancelled; a job finishing
//    while Shutdown() runs promotes nothing; a blocking job parked on a queued
//    compute handle or on another channel's queued handle is released before
//    the blocking threads are joined; a Submit or an Enqueue racing Shutdown
//    between the channel's lock and the blocking threads' FIFO is handed back
//    to its caller (the Submit cancelled, the Enqueue run there once).

#include "JobSystem/JobChannel.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace JobSystem;
using namespace std::chrono_literals;

namespace GameEngine::Tests {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr auto kDeadline = 10s;
// Long enough for a job that should not start to have started if it could.
constexpr auto kNotStartedProbe = 200ms;

bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = kDeadline) {
    const auto deadline = SteadyClock::now() + timeout;
    while (!pred()) {
        if (SteadyClock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A cancelled handle reads as failed with no error message; a job that threw
// carries the exception's message.
bool IsCancelled(const TaskHandle& handle) {
    return handle.IsDone() && handle.HasFailed() && handle.GetErrorMessage().empty();
}

// A latch the test opens; jobs park on it.
class Gate {
public:
    void Open() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Open = true;
        m_Changed.notify_all();
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Changed.wait(lock, [this] { return m_Open; });
    }

private:
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    bool m_Open = false;
};

// Opens the gate on every exit path, so a failed assertion never leaves a
// parked job holding a thread the pool's destructor joins.
struct GateOpener {
    Gate& Target;
    ~GateOpener() { Target.Open(); }
};

uint64 TotalComputePops(const WorkStealingThreadPool& pool) {
    const JobSystemStatistics census = pool.GetStatistics();
    return census.GlobalPops + census.LocalPops + census.StealPops + census.BackgroundPops;
}

// Runs Shutdown() on its own thread, waits for the pool's gate, then calls
// `afterGate` (the test releases its parked jobs there). True when Shutdown()
// returned within the deadline; the channels and the pool are still alive
// then, for the test to inspect. On a hang the pool and its channels are
// leaked, with their threads still parked, so the test reports the hang
// instead of wedging the run in a destructor.
bool ShutdownReturnsAfterGate(std::unique_ptr<WorkStealingThreadPool>& pool,
                              std::vector<std::unique_ptr<JobChannel>>& channels,
                              const std::function<void()>& afterGate) {
    WorkStealingThreadPool* shuttingDown = pool.get();
    auto shutdown = std::async(std::launch::async, [shuttingDown] { shuttingDown->Shutdown(); });
    EXPECT_TRUE(WaitUntil([&] { return shuttingDown->IsShuttingDown(); }));
    afterGate();
    if (shutdown.wait_for(kDeadline) != std::future_status::ready) {
        for (std::unique_ptr<JobChannel>& channel : channels) {
            (void)channel.release();
        }
        (void)pool.release();
        // The future's destructor would wait for the hung Shutdown().
        new std::future<void>(std::move(shutdown));
        return false;
    }
    return true;
}

// Busy for about `duration`, so concurrent jobs overlap.
void Spin(std::chrono::microseconds duration) {
    const auto end = SteadyClock::now() + duration;
    while (SteadyClock::now() < end) {
    }
}

} // namespace

// Four parked channel jobs occupy four blocking threads at once on a pool with
// two compute workers, and a fork on the main thread still completes: the
// compute workers never ran a channel job.
TEST(JobChannelTest, BlockingJobsNeverOccupyCpuWorkers) {
    WorkStealingThreadPool pool(2);
    JobChannel channel(pool, {.Name = "Test blocking", .MaxRunning = 4});
    Gate gate;
    GateOpener opener{gate};

    const uint64 popsBefore = TotalComputePops(pool);
    std::atomic<int> running{0};
    std::vector<TaskHandle> handles;
    for (int i = 0; i < 4; ++i) {
        handles.push_back(channel.Submit([&] {
            running.fetch_add(1);
            gate.Wait();
        }));
    }
    ASSERT_TRUE(WaitUntil([&] { return running.load() == 4; }))
        << "four cap-4 channel jobs never ran at once";

    std::atomic<int> forked{0};
    JobCounter counter;
    const auto forkStart = SteadyClock::now();
    for (int i = 0; i < 8; ++i) {
        pool.Run([&forked] { forked.fetch_add(1); }, counter);
    }
    pool.Wait(counter);
    EXPECT_LT(SteadyClock::now() - forkStart, 1s) << "the fork waited behind the channel jobs";
    EXPECT_EQ(forked.load(), 8);

    gate.Open();
    for (TaskHandle& handle : handles) {
        handle.Wait();
        EXPECT_TRUE(handle.IsCompleted());
    }
    // Every compute pop since the channel jobs were submitted is a fork stub.
    EXPECT_LE(TotalComputePops(pool) - popsBefore, 8u) << "a compute worker dequeued a channel job";
}

TEST(JobChannelTest, CapIsNeverExceeded) {
    constexpr int kProducers = 8;
    constexpr int kJobsPerProducer = 1000;
    constexpr uint32 kCap = 3;
    WorkStealingThreadPool pool(2);
    JobChannel channel(pool, {.Name = "Test cap", .MaxRunning = kCap});
    // An idle wide channel raises the blocking-thread demand above the cap, so
    // the thread count cannot be what holds the channel to it.
    JobChannel wide(pool, {.Name = "Test wide", .MaxRunning = 8});

    std::atomic<int> running{0};
    std::atomic<int> highWater{0};
    std::vector<std::atomic<int>> runs(kProducers * kJobsPerProducer);
    std::vector<std::vector<TaskHandle>> handles(kProducers);
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int j = 0; j < kJobsPerProducer; ++j) {
                const int index = p * kJobsPerProducer + j;
                handles[p].push_back(channel.Submit([&, index] {
                    const int now = running.fetch_add(1) + 1;
                    int seen = highWater.load();
                    while (now > seen && !highWater.compare_exchange_weak(seen, now)) {
                    }
                    runs[index].fetch_add(1);
                    Spin(20us);
                    running.fetch_sub(1);
                }));
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    for (std::vector<TaskHandle>& producerHandles : handles) {
        for (TaskHandle& handle : producerHandles) {
            handle.Wait();
        }
    }
    EXPECT_LE(highWater.load(), static_cast<int>(kCap));
    for (size_t i = 0; i < runs.size(); ++i) {
        ASSERT_EQ(runs[i].load(), 1) << "job " << i;
    }
}

// Eleven jobs wait behind a parked job of a cap-1 channel while a second
// cap-1 channel's job runs to completion: the queued jobs took no thread, so
// every job of both channels ran on at most two blocking threads.
TEST(JobChannelTest, QueuedJobsHoldNoThread) {
    WorkStealingThreadPool pool(1);
    JobChannel parked(pool, {.Name = "Test parked", .MaxRunning = 1});
    JobChannel other(pool, {.Name = "Test other", .MaxRunning = 1});
    Gate gate;
    GateOpener opener{gate};

    std::mutex threadsMutex;
    std::set<std::thread::id> threads;
    auto recordThread = [&] {
        std::lock_guard<std::mutex> lock(threadsMutex);
        threads.insert(std::this_thread::get_id());
    };
    std::atomic<bool> headRunning{false};
    std::vector<TaskHandle> handles;
    handles.push_back(parked.Submit([&] {
        recordThread();
        headRunning.store(true);
        gate.Wait();
    }));
    ASSERT_TRUE(WaitUntil([&] { return headRunning.load(); }));
    for (int i = 0; i < 10; ++i) {
        handles.push_back(parked.Submit(recordThread));
    }

    TaskHandle otherJob = other.Submit(recordThread);
    otherJob.Wait();
    EXPECT_TRUE(otherJob.IsCompleted()) << "a second channel's job waited behind the first channel's queue";

    gate.Open();
    for (TaskHandle& handle : handles) {
        handle.Wait();
        EXPECT_TRUE(handle.IsCompleted());
    }
    EXPECT_LE(threads.size(), 2u) << "queued channel jobs took threads of their own";
    EXPECT_EQ(threads.count(std::this_thread::get_id()), 0u);
}

TEST(JobChannelTest, CancelledQueuedSubmitReleasesNothingItNeverTook) {
    WorkStealingThreadPool pool(1);
    JobChannel channel(pool, {.Name = "Test cancel queued", .MaxRunning = 1});
    Gate gate;
    GateOpener opener{gate};

    std::atomic<bool> headRunning{false};
    std::atomic<bool> cancelledRan{false};
    std::atomic<bool> nextRan{false};
    TaskHandle head = channel.Submit([&] {
        headRunning.store(true);
        gate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return headRunning.load(); }));
    TaskHandle cancelled = channel.Submit([&] { cancelledRan.store(true); });
    TaskHandle next = channel.Submit([&] { nextRan.store(true); });

    EXPECT_TRUE(cancelled.Cancel());
    EXPECT_TRUE(IsCancelled(cancelled));
    std::this_thread::sleep_for(kNotStartedProbe);
    EXPECT_FALSE(nextRan.load()) << "cancelling a queued job freed a slot it never took";

    gate.Open();
    next.Wait();
    EXPECT_TRUE(next.IsCompleted());
    EXPECT_FALSE(cancelledRan.load());

    // One slot, exactly: the second of two parked jobs waits for the first.
    Gate second;
    GateOpener secondOpener{second};
    std::atomic<int> running{0};
    std::atomic<int> highWater{0};
    auto parkedJob = [&] {
        const int now = running.fetch_add(1) + 1;
        highWater.store(std::max(highWater.load(), now));
        second.Wait();
        running.fetch_sub(1);
    };
    TaskHandle first = channel.Submit(parkedJob);
    TaskHandle last = channel.Submit(parkedJob);
    ASSERT_TRUE(WaitUntil([&] { return running.load() == 1; })) << "the channel lost its slot";
    std::this_thread::sleep_for(kNotStartedProbe);
    EXPECT_EQ(highWater.load(), 1);
    second.Open();
    first.Wait();
    last.Wait();
    EXPECT_TRUE(last.IsCompleted());
}

// A job that took a slot and waits in the blocking threads' FIFO is cancelled
// there; when a thread reaches it the dropped envelope frees its slot, so the
// channel still runs two jobs at once afterwards.
TEST(JobChannelTest, CancelledDispatchedSubmitReleasesItsSlot) {
    WorkStealingThreadPool pool(1, /*blockingThreadBudget=*/2);
    JobChannel holder(pool, {.Name = "Test holder", .MaxRunning = 1});
    JobChannel channel(pool, {.Name = "Test dispatched", .MaxRunning = 2});
    Gate holderGate;
    Gate headGate;
    GateOpener holderOpener{holderGate};
    GateOpener headOpener{headGate};

    std::atomic<int> running{0};
    TaskHandle hold = holder.Submit([&] {
        running.fetch_add(1);
        holderGate.Wait();
    });
    TaskHandle head = channel.Submit([&] {
        running.fetch_add(1);
        headGate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return running.load() == 2; })) << "both blocking threads should be busy";

    // Slot two: dispatched to the FIFO, where no thread is free to take it.
    std::atomic<bool> cancelledRan{false};
    TaskHandle dispatched = channel.Submit([&] { cancelledRan.store(true); });
    EXPECT_TRUE(dispatched.Cancel());
    std::atomic<bool> queuedRan{false};
    TaskHandle queued = channel.Submit([&] { queuedRan.store(true); });

    headGate.Open();
    holderGate.Open();
    queued.Wait();
    EXPECT_TRUE(queued.IsCompleted());
    EXPECT_FALSE(cancelledRan.load());
    head.Wait();
    hold.Wait();

    Gate pairGate;
    GateOpener pairOpener{pairGate};
    std::atomic<int> pairRunning{0};
    auto pairJob = [&] {
        pairRunning.fetch_add(1);
        pairGate.Wait();
    };
    TaskHandle a = channel.Submit(pairJob);
    TaskHandle b = channel.Submit(pairJob);
    EXPECT_TRUE(WaitUntil([&] { return pairRunning.load() == 2; }))
        << "the cancelled dispatched job kept its slot";
    pairGate.Open();
    a.Wait();
    b.Wait();
}

TEST(JobChannelTest, DestroyedChannelCancelsQueuedSubmit) {
    WorkStealingThreadPool pool(1);
    auto channel = std::make_unique<JobChannel>(pool, JobChannelDesc{.Name = "Test destroyed", .MaxRunning = 1});
    Gate gate;
    GateOpener opener{gate};

    std::atomic<bool> headRunning{false};
    std::atomic<bool> queuedRan{false};
    TaskHandle head = channel->Submit([&] {
        headRunning.store(true);
        gate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return headRunning.load(); }));
    TaskHandle queued = channel->Submit([&] { queuedRan.store(true); });

    channel.reset();
    EXPECT_TRUE(IsCancelled(queued));

    gate.Open();
    head.Wait();
    EXPECT_TRUE(head.IsCompleted()) << "a running job outlives its channel";
    EXPECT_FALSE(queuedRan.load());
}

TEST(JobChannelTest, SubmitHandleIsAGraphDependency) {
    WorkStealingThreadPool pool(2);
    JobChannel channel(pool, {.Name = "Test dependency", .MaxRunning = 1});
    Gate gate;
    GateOpener opener{gate};

    TaskHandle produced = channel.Submit([&] {
        gate.Wait();
        return 7;
    });
    std::atomic<int> seen{0};
    TaskHandle dependent = pool.Submit(
        [&] {
            int value = 0;
            if (produced.TryGetResult(value)) {
                seen.store(value);
            }
        },
        std::span<const TaskHandle>(&produced, 1));
    std::this_thread::sleep_for(kNotStartedProbe);
    EXPECT_FALSE(dependent.IsDone()) << "the dependent ran before the channel job";

    gate.Open();
    dependent.Wait();
    EXPECT_TRUE(dependent.IsCompleted());
    EXPECT_EQ(seen.load(), 7);

    TaskHandle failing = channel.Submit([]() -> int { throw std::runtime_error("channel job failed"); });
    std::atomic<bool> dependentRan{false};
    TaskHandle afterFailure =
        pool.Submit([&] { dependentRan.store(true); }, std::span<const TaskHandle>(&failing, 1));
    afterFailure.Wait();
    EXPECT_TRUE(failing.HasFailed());
    EXPECT_FALSE(afterFailure.IsCompleted());
    EXPECT_FALSE(dependentRan.load()) << "a failed channel job released its dependent";
}

TEST(JobChannelTest, InlinePoolRunsChannelJobsAtSubmit) {
    WorkStealingThreadPool pool(0);
    JobChannel channel(pool, {.Name = "Test inline", .MaxRunning = 1});
    std::thread::id ranOn;
    TaskHandle handle = channel.Submit([&] { ranOn = std::this_thread::get_id(); });
    EXPECT_TRUE(handle.IsCompleted()) << "an inline pool runs a channel job inside Submit";
    EXPECT_EQ(ranOn, std::this_thread::get_id());
}

TEST(JobChannelTest, BudgetCapsBlockingThreads) {
    WorkStealingThreadPool pool(2, /*blockingThreadBudget=*/1);
    JobChannel channel(pool, {.Name = "Test budget", .MaxRunning = 4});

    std::mutex threadsMutex;
    std::set<std::thread::id> threads;
    std::atomic<int> running{0};
    std::atomic<int> highWater{0};
    std::vector<TaskHandle> handles;
    for (int i = 0; i < 4; ++i) {
        handles.push_back(channel.Submit([&] {
            const int now = running.fetch_add(1) + 1;
            highWater.store(std::max(highWater.load(), now));
            {
                std::lock_guard<std::mutex> lock(threadsMutex);
                threads.insert(std::this_thread::get_id());
            }
            std::this_thread::sleep_for(20ms);
            running.fetch_sub(1);
        }));
    }
    for (TaskHandle& handle : handles) {
        handle.Wait();
        EXPECT_TRUE(handle.IsCompleted());
    }
    EXPECT_EQ(threads.size(), 1u) << "a budget of one created more than one blocking thread";
    EXPECT_EQ(highWater.load(), 1);
}

// A blocking thread may idle for an hour between jobs; its deferred registry
// cleanups must not keep the finished job's TaskData alive that long.
TEST(JobChannelTest, IdleBlockingThreadFlushesItsCleanupShare) {
    WorkStealingThreadPool pool(1);
    JobChannel channel(pool, {.Name = "Test cleanup", .MaxRunning = 1});
    const size_t baseline = pool.GetTaskDataRegistrySizeForTests();
    {
        TaskHandle handle = channel.Submit([] {});
        handle.Wait();
        EXPECT_TRUE(handle.IsCompleted());
    }
    EXPECT_TRUE(WaitUntil([&] { return pool.GetTaskDataRegistrySizeForTests() == baseline; }, 2s))
        << "the idle blocking thread still holds " << pool.GetTaskDataRegistrySizeForTests() - baseline
        << " registry entries";
}

// A cap-1 channel's running job holds the only blocking thread; three
// Enqueue jobs and a Submit job wait in its FIFO. Destroying the channel
// cancels the Submit job and hands the Enqueue jobs on: they run after the
// running job, in order, on that blocking thread. An idle second channel keeps
// the registered caps above the thread count, so a hand-on that created a
// thread would run them beside the running job.
TEST(JobChannelEnqueueTest, DestroyedChannelHandsEnqueueOnToItsTarget) {
    WorkStealingThreadPool pool(1);
    JobChannel idle(pool, {.Name = "Test idle demand", .MaxRunning = 2});
    auto channel = std::make_unique<JobChannel>(pool, JobChannelDesc{.Name = "Test hand on", .MaxRunning = 1});
    Gate gate;
    GateOpener opener{gate};

    std::atomic<bool> headRunning{false};
    std::thread::id headThread;
    TaskHandle head = channel->Submit([&] {
        headThread = std::this_thread::get_id();
        headRunning.store(true);
        gate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return headRunning.load(); }));

    std::mutex orderMutex;
    std::vector<int> order;
    std::vector<std::thread::id> ranOn;
    std::atomic<int> done{0};
    std::atomic<bool> submitRan{false};
    for (int i = 0; i < 3; ++i) {
        channel->Enqueue([&, i] {
            {
                std::lock_guard<std::mutex> lock(orderMutex);
                order.push_back(i);
                ranOn.push_back(std::this_thread::get_id());
            }
            done.fetch_add(1);
        });
        if (i == 1) {
            (void)channel->Submit([&] { submitRan.store(true); });
        }
    }
    const uint64 popsBefore = TotalComputePops(pool);

    channel.reset();
    std::this_thread::sleep_for(kNotStartedProbe);
    EXPECT_EQ(done.load(), 0) << "a handed-on Enqueue job ran beside the job its channel was still running";

    gate.Open();
    head.Wait();
    ASSERT_TRUE(WaitUntil([&] { return done.load() == 3; })) << "a destroyed channel dropped its queued Enqueue jobs";
    EXPECT_FALSE(submitRan.load()) << "a destroyed channel ran a queued Submit job";
    std::lock_guard<std::mutex> lock(orderMutex);
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
    for (const std::thread::id& thread : ranOn) {
        EXPECT_EQ(thread, headThread) << "a handed-on Enqueue job ran off the channel's blocking thread";
    }
    EXPECT_EQ(TotalComputePops(pool), popsBefore) << "a compute worker ran a handed-on Enqueue job";
}

// Enqueue jobs share the channel's FIFO and cap with Submit jobs and run on
// blocking threads, never on a compute worker.
TEST(JobChannelEnqueueTest, EnqueueJobsRunOnBlockingThreadsUnderTheCap) {
    constexpr uint32 kCap = 2;
    WorkStealingThreadPool pool(1);
    JobChannel channel(pool, {.Name = "Test enqueue cap", .MaxRunning = kCap});
    const uint64 popsBefore = TotalComputePops(pool);

    std::atomic<int> running{0};
    std::atomic<int> highWater{0};
    std::atomic<int> done{0};
    std::atomic<bool> ranOnCaller{false};
    const std::thread::id caller = std::this_thread::get_id();
    constexpr int kJobs = 64;
    for (int i = 0; i < kJobs; ++i) {
        channel.Enqueue([&] {
            if (std::this_thread::get_id() == caller) {
                ranOnCaller.store(true);
            }
            const int now = running.fetch_add(1) + 1;
            int seen = highWater.load();
            while (now > seen && !highWater.compare_exchange_weak(seen, now)) {
            }
            Spin(200us);
            running.fetch_sub(1);
            done.fetch_add(1);
        });
    }
    ASSERT_TRUE(WaitUntil([&] { return done.load() == kJobs; }));
    EXPECT_LE(highWater.load(), static_cast<int>(kCap));
    EXPECT_FALSE(ranOnCaller.load());
    EXPECT_EQ(TotalComputePops(pool), popsBefore) << "a compute worker ran an Enqueue job";
}

TEST(JobChannelEnqueueTest, EnqueueAfterShutdownRunsOnCaller) {
    WorkStealingThreadPool pool(1);
    JobChannel channel(pool, {.Name = "Test enqueue after shutdown", .MaxRunning = 1});
    pool.Shutdown();
    std::thread::id ranOn;
    int runs = 0;
    channel.Enqueue([&] {
        ranOn = std::this_thread::get_id();
        ++runs;
    });
    EXPECT_EQ(runs, 1) << "an Enqueue after Shutdown() did not run inside Enqueue";
    EXPECT_EQ(ranOn, std::this_thread::get_id());
}

TEST(JobChannelEnqueueTest, InlinePoolRunsChannelJobsAtEnqueue) {
    WorkStealingThreadPool pool(0);
    JobChannel channel(pool, {.Name = "Test inline enqueue", .MaxRunning = 1});
    std::thread::id ranOn;
    int runs = 0;
    channel.Enqueue([&] {
        ranOn = std::this_thread::get_id();
        ++runs;
    });
    EXPECT_EQ(runs, 1) << "an inline pool runs a channel job inside Enqueue";
    EXPECT_EQ(ranOn, std::this_thread::get_id());
}

TEST(JobChannelShutdownTest, ShutdownCancelsQueuedHandles) {
    auto pool = std::make_unique<WorkStealingThreadPool>(1);
    std::vector<std::unique_ptr<JobChannel>> channels;
    channels.push_back(std::make_unique<JobChannel>(*pool, JobChannelDesc{.Name = "Test queued", .MaxRunning = 1}));
    Gate gate;
    GateOpener opener{gate};

    std::atomic<bool> headRunning{false};
    std::atomic<int> queuedRan{0};
    TaskHandle head = channels[0]->Submit([&] {
        headRunning.store(true);
        gate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return headRunning.load(); }));
    std::vector<TaskHandle> queued;
    std::vector<std::weak_ptr<int>> closures;
    for (int i = 0; i < 3; ++i) {
        auto token = std::make_shared<int>(i);
        closures.push_back(token);
        queued.push_back(channels[0]->Submit([&queuedRan, token] { queuedRan.fetch_add(*token + 1); }));
    }

    ASSERT_TRUE(ShutdownReturnsAfterGate(pool, channels, [&] { gate.Open(); }));
    EXPECT_TRUE(head.IsCompleted()) << "a running channel job finishes at shutdown";
    for (size_t i = 0; i < queued.size(); ++i) {
        EXPECT_TRUE(IsCancelled(queued[i]));
        // The drain took the job out of the channel's FIFO, not the channel's
        // destructor.
        EXPECT_TRUE(closures[i].expired()) << "Shutdown() left queued job " << i << " in its channel";
    }
    EXPECT_EQ(queuedRan.load(), 0);
    channels.clear();
    pool.reset();
}

// The worker is parked; a channel job submits a compute task (queued behind
// the parked worker) and waits on its handle. Only the shutdown drain can
// release that wait, so the blocking threads are joined after it.
TEST(JobChannelShutdownTest, ShutdownReleasesABlockingJobWaitingOnAQueuedComputeHandle) {
    auto pool = std::make_unique<WorkStealingThreadPool>(1);
    std::vector<std::unique_ptr<JobChannel>> channels;
    channels.push_back(std::make_unique<JobChannel>(*pool, JobChannelDesc{.Name = "Test waiter", .MaxRunning = 1}));
    Gate workerGate;
    GateOpener opener{workerGate};

    std::atomic<bool> workerParked{false};
    TaskHandle parkedWorker = pool->Submit([&] {
        workerParked.store(true);
        workerGate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return workerParked.load(); }));

    WorkStealingThreadPool* computePool = pool.get();
    std::atomic<bool> waiting{false};
    auto computeHandle = std::make_shared<TaskHandle>();
    TaskHandle blockingJob = channels[0]->Submit([&, computePool, computeHandle] {
        *computeHandle = computePool->Submit([] {});
        waiting.store(true);
        computeHandle->Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return waiting.load(); }));

    ASSERT_TRUE(ShutdownReturnsAfterGate(pool, channels, [&] { workerGate.Open(); }))
        << "Shutdown() joined the blocking threads before releasing the job's compute handle";
    EXPECT_TRUE(IsCancelled(*computeHandle));
    EXPECT_TRUE(blockingJob.IsCompleted());
    channels.clear();
    pool.reset();
}

// The channel arm: a job of the first channel waits on a handle queued in the
// second channel, whose only slot a parked job holds.
TEST(JobChannelShutdownTest, ShutdownReleasesABlockingJobWaitingOnAQueuedChannelHandle) {
    auto pool = std::make_unique<WorkStealingThreadPool>(1);
    std::vector<std::unique_ptr<JobChannel>> channels;
    channels.push_back(std::make_unique<JobChannel>(*pool, JobChannelDesc{.Name = "Test first", .MaxRunning = 1}));
    channels.push_back(std::make_unique<JobChannel>(*pool, JobChannelDesc{.Name = "Test second", .MaxRunning = 1}));
    JobChannel& first = *channels[0];
    JobChannel& second = *channels[1];
    Gate gate;
    GateOpener opener{gate};

    std::atomic<bool> secondParked{false};
    TaskHandle parked = second.Submit([&] {
        secondParked.store(true);
        gate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return secondParked.load(); }));

    std::atomic<bool> waiting{false};
    auto queuedHandle = std::make_shared<TaskHandle>();
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> queuedClosure = token;
    TaskHandle waiter = first.Submit([&, queuedHandle, token]() mutable {
        *queuedHandle = second.Submit([token] { (void)*token; });
        token.reset();
        waiting.store(true);
        queuedHandle->Wait();
    });
    token.reset();
    ASSERT_TRUE(WaitUntil([&] { return waiting.load(); }));

    ASSERT_TRUE(ShutdownReturnsAfterGate(pool, channels, [&] { gate.Open(); }))
        << "Shutdown() joined the blocking threads before releasing the job's channel handle";
    EXPECT_TRUE(IsCancelled(*queuedHandle));
    EXPECT_TRUE(queuedClosure.expired()) << "Shutdown() left the queued job in the second channel's FIFO";
    EXPECT_TRUE(waiter.IsCompleted());
    EXPECT_TRUE(parked.IsCompleted());
    channels.clear();
    pool.reset();
}

// A Submit that took a slot is held between the channel's lock and the
// blocking threads' FIFO while Shutdown() runs to completion. The FIFO reads
// the gate under its mutex and hands the job back: the handle reads
// Cancelled, never Pending, and the job never runs.
TEST(JobChannelShutdownTest, ChannelPublishRacingShutdownIsDrainedOnTheCaller) {
    auto pool = std::make_unique<WorkStealingThreadPool>(1);
    JobChannel channel(*pool, {.Name = "Test racing", .MaxRunning = 1});

    WorkStealingThreadPool* racingPool = pool.get();
    std::atomic<bool> shutdownReturned{false};
    channel.SetBeforeDispatchHookForTests([racingPool, &shutdownReturned] {
        std::thread shutdown([racingPool] { racingPool->Shutdown(); });
        shutdown.join();
        shutdownReturned.store(true);
    });
    std::atomic<bool> ran{false};
    TaskHandle handle = channel.Submit([&] { ran.store(true); });
    channel.SetBeforeDispatchHookForTests({});

    ASSERT_TRUE(shutdownReturned.load());
    ASSERT_TRUE(handle.IsValid()) << "the Submit passed the gate before Shutdown() set it";
    EXPECT_TRUE(IsCancelled(handle)) << "the handed-back job was stranded";
    std::this_thread::sleep_for(kNotStartedProbe);
    EXPECT_FALSE(ran.load());
    EXPECT_FALSE(channel.Submit([] {}).IsValid()) << "a Submit after Shutdown() returns an invalid handle";
}

// The Enqueue arm: the job is handed back to its caller and runs there,
// exactly once.
TEST(JobChannelShutdownTest, ChannelEnqueueRacingShutdownIsDrainedOnTheCaller) {
    auto pool = std::make_unique<WorkStealingThreadPool>(1);
    JobChannel channel(*pool, {.Name = "Test racing enqueue", .MaxRunning = 1});

    WorkStealingThreadPool* racingPool = pool.get();
    std::atomic<bool> shutdownReturned{false};
    channel.SetBeforeDispatchHookForTests([racingPool, &shutdownReturned] {
        std::thread shutdown([racingPool] { racingPool->Shutdown(); });
        shutdown.join();
        shutdownReturned.store(true);
    });
    std::atomic<int> runs{0};
    std::thread::id ranOn;
    channel.Enqueue([&] {
        ranOn = std::this_thread::get_id();
        runs.fetch_add(1);
    });
    channel.SetBeforeDispatchHookForTests({});

    ASSERT_TRUE(shutdownReturned.load());
    EXPECT_EQ(runs.load(), 1) << "the handed-back Enqueue job did not run inside Enqueue";
    EXPECT_EQ(ranOn, std::this_thread::get_id());
    std::this_thread::sleep_for(kNotStartedProbe);
    EXPECT_EQ(runs.load(), 1) << "the handed-back Enqueue job ran twice";
}

// The compute worker is parked, so Shutdown() stops between its gate and its
// drain. The channel's running job finishes in that window: its release
// promotes nothing, and the drain alone empties the channel's FIFO, so the
// queued Enqueue jobs run on the thread that called Shutdown() and the queued
// Submit jobs read Cancelled.
TEST(JobChannelShutdownTest, ShutdownExecutesQueuedBareJobsAndCancelsHandles) {
    auto pool = std::make_unique<WorkStealingThreadPool>(1);
    std::vector<std::unique_ptr<JobChannel>> channels;
    channels.push_back(std::make_unique<JobChannel>(*pool, JobChannelDesc{.Name = "Test drained", .MaxRunning = 1}));
    JobChannel& channel = *channels[0];
    Gate workerGate;
    Gate headGate;
    GateOpener workerOpener{workerGate};
    GateOpener headOpener{headGate};

    std::atomic<bool> workerParked{false};
    TaskHandle parkedWorker = pool->Submit([&] {
        workerParked.store(true);
        workerGate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return workerParked.load(); }));

    std::atomic<bool> headRunning{false};
    TaskHandle head = channel.Submit([&] {
        headRunning.store(true);
        headGate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return headRunning.load(); }));

    std::mutex ranMutex;
    std::vector<std::thread::id> enqueueThreads;
    std::atomic<bool> submitRan{false};
    auto bare = [&] {
        std::lock_guard<std::mutex> lock(ranMutex);
        enqueueThreads.push_back(std::this_thread::get_id());
    };
    channel.Enqueue(bare);
    TaskHandle queuedSubmit = channel.Submit([&] { submitRan.store(true); });
    channel.Enqueue(bare);

    WorkStealingThreadPool* shuttingDown = pool.get();
    std::thread::id shutdownThread;
    auto shutdown = std::async(std::launch::async, [shuttingDown, &shutdownThread] {
        shutdownThread = std::this_thread::get_id();
        shuttingDown->Shutdown();
    });
    ASSERT_TRUE(WaitUntil([&] { return shuttingDown->IsShuttingDown(); }));
    headGate.Open();
    head.Wait();
    // The head's envelope is released on its blocking thread after the handle
    // completes; give that release time to run before the drain can.
    std::this_thread::sleep_for(kNotStartedProbe);
    {
        std::lock_guard<std::mutex> lock(ranMutex);
        EXPECT_TRUE(enqueueThreads.empty()) << "a release after the shutdown gate promoted a queued job";
    }
    workerGate.Open();
    if (shutdown.wait_for(kDeadline) != std::future_status::ready) {
        (void)channels[0].release();
        (void)pool.release();
        new std::future<void>(std::move(shutdown));
        FAIL() << "Shutdown() did not return";
    }

    EXPECT_TRUE(IsCancelled(queuedSubmit));
    EXPECT_FALSE(submitRan.load());
    std::lock_guard<std::mutex> lock(ranMutex);
    ASSERT_EQ(enqueueThreads.size(), 2u) << "the drain dropped a queued Enqueue job";
    for (const std::thread::id& thread : enqueueThreads) {
        EXPECT_EQ(thread, shutdownThread) << "a queued Enqueue job ran off the shutting-down thread";
    }
    channels.clear();
    pool.reset();
}

// A queued Enqueue on the first channel waits on a Submit handle queued on the
// second channel, which registered later. The drain cancels every queued
// handle, across every channel's FIFO, before it runs any queued Enqueue job,
// so the wait returns on the shutting-down thread.
TEST(JobChannelShutdownTest, ShutdownCancelsEveryQueuedHandleBeforeRunningAQueuedEnqueue) {
    auto pool = std::make_unique<WorkStealingThreadPool>(1);
    std::vector<std::unique_ptr<JobChannel>> channels;
    channels.push_back(std::make_unique<JobChannel>(*pool, JobChannelDesc{.Name = "Test first", .MaxRunning = 1}));
    channels.push_back(std::make_unique<JobChannel>(*pool, JobChannelDesc{.Name = "Test second", .MaxRunning = 1}));
    JobChannel& first = *channels[0];
    JobChannel& second = *channels[1];
    Gate firstGate;
    Gate secondGate;
    GateOpener firstOpener{firstGate};
    GateOpener secondOpener{secondGate};

    std::atomic<int> parkedCount{0};
    TaskHandle firstParked = first.Submit([&] {
        parkedCount.fetch_add(1);
        firstGate.Wait();
    });
    TaskHandle secondParked = second.Submit([&] {
        parkedCount.fetch_add(1);
        secondGate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return parkedCount.load() == 2; }));

    std::atomic<bool> queuedRan{false};
    TaskHandle queuedOnSecond = second.Submit([&] { queuedRan.store(true); });
    std::atomic<bool> waitReturned{false};
    first.Enqueue([&queuedOnSecond, &waitReturned] {
        queuedOnSecond.Wait();
        waitReturned.store(true);
    });

    ASSERT_TRUE(ShutdownReturnsAfterGate(pool, channels, [&] {
        firstGate.Open();
        secondGate.Open();
    })) << "the drain ran a queued Enqueue job before it cancelled the handle that job waits on";
    EXPECT_TRUE(IsCancelled(queuedOnSecond));
    EXPECT_TRUE(waitReturned.load());
    EXPECT_FALSE(queuedRan.load());
    channels.clear();
    pool.reset();
}

} // namespace GameEngine::Tests
