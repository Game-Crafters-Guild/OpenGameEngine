// The pool's statistics, its occupancy snapshot and the current-job context
// (jobsystem design slice S5).
//
// Layers:
//  - JobSystemStatisticsTest: GetStatistics() counts a known workload
//    exactly (pushes, pops, TasksExecuted, a channel's row with its queue
//    wait) and reports what each lane holds and runs.
//  - OccupancySnapshotTest: SnapshotOccupancy() names what every pool thread
//    runs while jobs are parked on it, with a start time for Background and
//    channel jobs; a body that ran nested jobs reads as itself again, with its
//    own start time; an inline pool has no slots.
//  - CurrentJobContextTest: a job body runs as the class of the lane it was
//    dequeued from (local, global, steal and Background pops, the spin poll's
//    Background find), as its channel on a blocking thread, including after a
//    destroyed channel handed it on, as the class given at publish on an
//    inline pool, including past the trampoline's depth bound, as its recorded
//    lane or channel in the shutdown drain, and a participating Wait runs a
//    tagged job as the waiter's own class. Completion callbacks run outside
//    any body.

#include "JobSystem/JobChannel.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/JobSystemStatistics.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace JobSystem;
using namespace std::chrono_literals;

namespace GameEngine::Tests {

namespace {

using SteadyClock = std::chrono::steady_clock;
using Occupancy = JobSystemStatistics::Occupancy;
using ThreadKind = JobSystemStatistics::ThreadKind;

constexpr auto kDeadline = 10s;
constexpr const char* kChannelName = "Statistics test channel";

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

uint64 NowNs() {
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count());
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

// What a job body saw as its current job, as a string ("" outside a body).
std::string CurrentJob() {
    const char* job = WorkStealingThreadPool::GetCurrentJobForTests();
    return job != nullptr ? job : "";
}

// A thread-safe record of what each body saw.
class Seen {
public:
    void Record() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Jobs.push_back(CurrentJob());
    }
    std::vector<std::string> Jobs() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Jobs;
    }
    size_t Count() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Jobs.size();
    }

private:
    std::mutex m_Mutex;
    std::vector<std::string> m_Jobs;
};

std::vector<Occupancy> Snapshot(const WorkStealingThreadPool& pool) {
    std::vector<Occupancy> threads(pool.GetWorkerCount() + kMaxBlockingThreadBudget);
    threads.resize(pool.SnapshotOccupancy(threads));
    return threads;
}

size_t CountRunning(const std::vector<Occupancy>& threads, ThreadKind kind, const char* running) {
    size_t count = 0;
    for (const Occupancy& thread : threads) {
        if (thread.Kind == kind && thread.Running != nullptr && std::strcmp(thread.Running, running) == 0) {
            ++count;
        }
    }
    return count;
}

const Occupancy* FindRunning(const std::vector<Occupancy>& threads, ThreadKind kind, const char* running) {
    for (const Occupancy& thread : threads) {
        if (thread.Kind == kind && thread.Running != nullptr && std::strcmp(thread.Running, running) == 0) {
            return &thread;
        }
    }
    return nullptr;
}

bool AllThreadsIdle(const WorkStealingThreadPool& pool) {
    for (const Occupancy& thread : Snapshot(pool)) {
        if (thread.Running != nullptr) {
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// GetStatistics
// ---------------------------------------------------------------------------

// A fresh pool runs 1,000 Normal and 300 Background fire-and-forget jobs, 50
// Normal and 20 Background handle jobs, and three jobs on a cap-1 channel
// whose first job is held while the other two wait behind it. Every count is
// exact; the channel's row reports the two queued jobs while they wait and
// at least the time they were held back afterwards.
TEST(JobSystemStatisticsTest, CountsAKnownWorkload) {
    constexpr uint64 kNormalBare = 1000;
    constexpr uint64 kBackgroundBare = 300;
    constexpr uint64 kNormalHandles = 50;
    constexpr uint64 kBackgroundHandles = 20;
    constexpr auto kHold = 60ms;

    WorkStealingThreadPool pool(4);
    JobChannel channel(pool, {.Name = kChannelName, .MaxRunning = 1});

    std::atomic<uint64> ran{0};
    for (uint64 i = 0; i < kNormalBare; ++i) {
        pool.EnqueueWork([&ran] { ran.fetch_add(1); });
    }
    for (uint64 i = 0; i < kBackgroundBare; ++i) {
        pool.EnqueueWork([&ran] { ran.fetch_add(1); }, JobPriority::Background);
    }
    std::vector<TaskHandle> handles;
    for (uint64 i = 0; i < kNormalHandles; ++i) {
        handles.push_back(pool.Submit([&ran] { ran.fetch_add(1); }));
    }
    for (uint64 i = 0; i < kBackgroundHandles; ++i) {
        handles.push_back(pool.Submit([&ran] { ran.fetch_add(1); }, JobPriority::Background));
    }

    Gate gate;
    GateOpener opener{gate};
    std::atomic<bool> firstStarted{false};
    std::vector<TaskHandle> channelJobs;
    channelJobs.push_back(channel.Submit([&] {
        firstStarted.store(true);
        gate.Wait();
    }));
    ASSERT_TRUE(WaitUntil([&] { return firstStarted.load(); }));
    channelJobs.push_back(channel.Submit([] {}));
    channelJobs.push_back(channel.Submit([] {}));

    const JobSystemStatistics held = pool.GetStatistics();
    ASSERT_EQ(held.Channels.size(), 1u);
    EXPECT_STREQ(held.Channels[0].Name, kChannelName);
    EXPECT_EQ(held.Channels[0].Cap, 1u);
    EXPECT_EQ(held.Channels[0].Running, 1u);
    EXPECT_EQ(held.Channels[0].Queued, 2u);
    EXPECT_EQ(held.Channels[0].Jobs, 1u);
    EXPECT_EQ(held.Channels[0].QueueWaitNs, 0u);

    std::this_thread::sleep_for(kHold);
    gate.Open();
    for (TaskHandle& handle : handles) {
        handle.Wait();
    }
    for (TaskHandle& job : channelJobs) {
        job.Wait();
    }
    constexpr uint64 kComputeJobs = kNormalBare + kBackgroundBare + kNormalHandles + kBackgroundHandles;
    ASSERT_TRUE(WaitUntil([&] { return ran.load() == kComputeJobs && AllThreadsIdle(pool); }));
    // The channel's last slot is released when its envelope dies, after the
    // handle completed.
    ASSERT_TRUE(WaitUntil([&] { return pool.GetStatistics().Channels[0].Running == 0; }));

    const JobSystemStatistics stats = pool.GetStatistics();
    EXPECT_EQ(stats.ComputeWorkers, 4u);
    EXPECT_EQ(stats.BlockingThreads, 1u);
    EXPECT_EQ(stats.GlobalPushes, kNormalBare + kNormalHandles);
    EXPECT_EQ(stats.GlobalPops, kNormalBare + kNormalHandles);
    EXPECT_EQ(stats.BackgroundPushes, kBackgroundBare + kBackgroundHandles);
    EXPECT_EQ(stats.BackgroundPops, kBackgroundBare + kBackgroundHandles);
    EXPECT_EQ(stats.LocalPushes, 0u);
    EXPECT_EQ(stats.LocalPops, 0u);
    EXPECT_EQ(stats.StealPops, 0u);
    EXPECT_EQ(stats.TasksExecuted, kComputeJobs + 3);
    EXPECT_EQ(stats.Normal.Queued, 0u);
    EXPECT_EQ(stats.Normal.Running, 0u);
    EXPECT_EQ(stats.Background.Queued, 0u);
    EXPECT_EQ(stats.Background.Running, 0u);
    ASSERT_EQ(stats.Threads.size(), 5u);
    EXPECT_EQ(stats.Threads[4].Kind, ThreadKind::Blocking);
    EXPECT_EQ(stats.Threads[4].Index, 0u);

    ASSERT_EQ(stats.Channels.size(), 1u);
    const JobSystemStatistics::Channel& row = stats.Channels[0];
    EXPECT_EQ(row.Queued, 0u);
    EXPECT_EQ(row.Running, 0u);
    EXPECT_EQ(row.Jobs, 3u);
    // Both queued jobs waited at least as long as the first was held.
    EXPECT_GE(row.QueueWaitNs, 2 * static_cast<uint64>(std::chrono::nanoseconds(kHold).count()));
}

// Two workers each hold a job, one Normal and one Background, while five
// Normal and three Background jobs wait behind them: each lane reports its
// own queued and running jobs.
TEST(JobSystemStatisticsTest, EachLaneReportsItsQueuedAndRunningJobs) {
    WorkStealingThreadPool pool(2);
    Gate gate;
    GateOpener opener{gate};
    std::atomic<int> parked{0};
    pool.EnqueueWork([&] {
        parked.fetch_add(1);
        gate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return parked.load() == 1; }));
    pool.EnqueueWork(
        [&] {
            parked.fetch_add(1);
            gate.Wait();
        },
        JobPriority::Background);
    ASSERT_TRUE(WaitUntil([&] { return parked.load() == 2; }));

    std::atomic<int> ran{0};
    for (int i = 0; i < 5; ++i) {
        pool.EnqueueWork([&ran] { ran.fetch_add(1); });
    }
    for (int i = 0; i < 3; ++i) {
        pool.EnqueueWork([&ran] { ran.fetch_add(1); }, JobPriority::Background);
    }

    const JobSystemStatistics stats = pool.GetStatistics();
    EXPECT_EQ(stats.Normal.Running, 1u);
    EXPECT_EQ(stats.Background.Running, 1u);
    EXPECT_EQ(stats.Normal.Queued, 5u);
    EXPECT_EQ(stats.Background.Queued, 3u);

    gate.Open();
    ASSERT_TRUE(WaitUntil([&] { return ran.load() == 8; }));
}

// ---------------------------------------------------------------------------
// SnapshotOccupancy
// ---------------------------------------------------------------------------

// A Background job and a Normal job parked on the two workers and a channel
// job parked on a blocking thread: the snapshot names all three, gives the
// Background and channel jobs the time they started and the Normal job none,
// and every thread reads idle once the jobs end.
TEST(OccupancySnapshotTest, NamesWhatEveryThreadRunsWhileJobsAreParked) {
    WorkStealingThreadPool pool(2);
    JobChannel channel(pool, {.Name = kChannelName, .MaxRunning = 1});
    Gate gate;
    GateOpener opener{gate};
    std::atomic<int> parked{0};
    const auto park = [&] {
        parked.fetch_add(1);
        gate.Wait();
    };

    const uint64 before = NowNs();
    pool.EnqueueWork(park, JobPriority::Background);
    ASSERT_TRUE(WaitUntil([&] { return parked.load() == 1; }));
    pool.EnqueueWork(park);
    TaskHandle channelJob = channel.Submit(park);
    ASSERT_TRUE(WaitUntil([&] { return parked.load() == 3; }));
    const uint64 after = NowNs();

    const std::vector<Occupancy> threads = Snapshot(pool);
    ASSERT_EQ(threads.size(), 3u) << "two workers and the one blocking thread created";
    EXPECT_EQ(threads[0].Kind, ThreadKind::Worker);
    EXPECT_EQ(threads[0].Index, 0u);
    EXPECT_EQ(threads[1].Kind, ThreadKind::Worker);
    EXPECT_EQ(threads[1].Index, 1u);

    const Occupancy* background = FindRunning(threads, ThreadKind::Worker, "Background");
    ASSERT_NE(background, nullptr);
    EXPECT_GE(background->SinceNs, before);
    EXPECT_LE(background->SinceNs, after);

    const Occupancy* normal = FindRunning(threads, ThreadKind::Worker, "Normal");
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->SinceNs, 0u) << "a Normal job reads no clock";

    const Occupancy* blocking = FindRunning(threads, ThreadKind::Blocking, kChannelName);
    ASSERT_NE(blocking, nullptr);
    EXPECT_EQ(blocking->Index, 0u);
    EXPECT_EQ(blocking->Running, kChannelName) << "the channel's own literal";
    EXPECT_GE(blocking->SinceNs, before);
    EXPECT_LE(blocking->SinceNs, after);

    // A shorter span gets the first entries and the count written.
    Occupancy first[1];
    EXPECT_EQ(pool.SnapshotOccupancy(first), 1u);
    EXPECT_EQ(first[0].Kind, ThreadKind::Worker);
    EXPECT_EQ(first[0].Index, 0u);

    gate.Open();
    channelJob.Wait();
    EXPECT_TRUE(WaitUntil([&] { return AllThreadsIdle(pool); }));
}

// GetStatistics reports the same slots: its Threads are the snapshot.
TEST(OccupancySnapshotTest, StatisticsCarryTheSnapshot) {
    WorkStealingThreadPool pool(3);
    Gate gate;
    GateOpener opener{gate};
    std::atomic<bool> parked{false};
    pool.EnqueueWork(
        [&] {
            parked.store(true);
            gate.Wait();
        },
        JobPriority::Background);
    ASSERT_TRUE(WaitUntil([&] { return parked.load(); }));

    const JobSystemStatistics stats = pool.GetStatistics();
    ASSERT_EQ(stats.Threads.size(), 3u) << "no blocking thread exists before a channel dispatches";
    EXPECT_EQ(CountRunning(stats.Threads, ThreadKind::Worker, "Background"), 1u);
    EXPECT_EQ(CountRunning(stats.Threads, ThreadKind::Worker, "Normal"), 0u);
    EXPECT_EQ(stats.BlockingThreads, 0u);
}

// A Background body on the only worker forks four jobs and waits on them, so
// the worker runs each one nested inside the body; then the body parks. The
// slot names the body again, with the start time it had before the fork, not
// idle and not the last nested job's start.
TEST(OccupancySnapshotTest, ABodyThatRanNestedJobsReadsAsItselfWithItsOwnStartTime) {
    WorkStealingThreadPool pool(1);
    Gate gate;
    GateOpener opener{gate};
    std::atomic<uint64> outerSinceNs{0};
    std::atomic<int> nestedRan{0};
    std::atomic<bool> parked{false};
    pool.EnqueueWork(
        [&] {
            const std::vector<Occupancy> before = Snapshot(pool);
            if (const Occupancy* self = FindRunning(before, ThreadKind::Worker, "Background")) {
                outerSinceNs.store(self->SinceNs);
            }
            JobCounter counter;
            for (int i = 0; i < 4; ++i) {
                pool.Run(
                    [&] {
                        // Each nested job starts measurably after the body.
                        std::this_thread::sleep_for(1ms);
                        nestedRan.fetch_add(1);
                    },
                    counter);
            }
            pool.Wait(counter);
            parked.store(true);
            gate.Wait();
        },
        JobPriority::Background);
    ASSERT_TRUE(WaitUntil([&] { return parked.load(); }));
    ASSERT_EQ(nestedRan.load(), 4);
    ASSERT_NE(outerSinceNs.load(), 0u) << "the body did not find its own slot";

    const std::vector<Occupancy> threads = Snapshot(pool);
    const Occupancy* body = FindRunning(threads, ThreadKind::Worker, "Background");
    ASSERT_NE(body, nullptr) << "the worker reads idle while its body is still running";
    EXPECT_EQ(body->SinceNs, outerSinceNs.load()) << "the slot kept a nested job's start time";
}

// An inline pool has no threads and so no slots.
TEST(OccupancySnapshotTest, AnInlinePoolHasNoSlots) {
    WorkStealingThreadPool pool(0);
    Occupancy threads[4];
    EXPECT_EQ(pool.SnapshotOccupancy(threads), 0u);
    const JobSystemStatistics stats = pool.GetStatistics();
    EXPECT_TRUE(stats.Threads.empty());
    EXPECT_EQ(stats.ComputeWorkers, 0u);
}

// ---------------------------------------------------------------------------
// The current-job context
// ---------------------------------------------------------------------------

// Global-lane pops, handle and bare, run as Normal; Background-lane pops run
// as Background. Outside any body the context is empty. The spin path is off,
// so every job is found by the worker loop's own scan (the spinner's finds
// have tests of their own below).
TEST(CurrentJobContextTest, LanePopsRunAsTheirLanesClass) {
    WorkStealingThreadPool pool(2);
    pool.SetSpinConfigForTest(0, 0);
    EXPECT_EQ(CurrentJob(), "");

    Seen normal;
    Seen background;
    pool.EnqueueWork([&] { normal.Record(); });
    pool.Submit([&] { normal.Record(); }).Wait();
    pool.EnqueueWork([&] { background.Record(); }, JobPriority::Background);
    pool.Submit([&] { background.Record(); }, JobPriority::Background).Wait();
    ASSERT_TRUE(WaitUntil([&] { return normal.Count() == 2 && background.Count() == 2; }));

    EXPECT_EQ(normal.Jobs(), (std::vector<std::string>{"Normal", "Normal"}));
    EXPECT_EQ(background.Jobs(), (std::vector<std::string>{"Background", "Background"}));
}

// A graph task published on the only worker lands in its local queue and is
// popped from there as Normal.
TEST(CurrentJobContextTest, ALocalQueuePopRunsAsNormal) {
    WorkStealingThreadPool pool(1);
    const uint64 localPopsBefore = pool.GetStatistics().LocalPops;
    Seen seen;
    pool.EnqueueWork(
        [&] {
            (void)pool.Submit([&] { seen.Record(); }, std::span<const TaskHandle>{});
        },
        JobPriority::Background);
    ASSERT_TRUE(WaitUntil([&] { return seen.Count() == 1; }));
    EXPECT_EQ(seen.Jobs()[0], "Normal");
    EXPECT_EQ(pool.GetStatistics().LocalPops - localPopsBefore, 1u);
}

// A graph task published on a worker that then stays busy is stolen by the
// other worker, and runs there as Normal.
TEST(CurrentJobContextTest, AStolenJobRunsAsNormal) {
    WorkStealingThreadPool pool(2);
    const uint64 stealPopsBefore = pool.GetStatistics().StealPops;
    Seen seen;
    std::atomic<bool> publisherDone{false};
    pool.EnqueueWork(
        [&] {
            (void)pool.Submit([&] { seen.Record(); }, std::span<const TaskHandle>{});
            // Busy until the other worker has run it, so this worker never
            // pops its own local queue.
            const auto deadline = SteadyClock::now() + kDeadline;
            while (seen.Count() == 0 && SteadyClock::now() < deadline) {
                std::this_thread::yield();
            }
            publisherDone.store(true);
        },
        JobPriority::Background);
    ASSERT_TRUE(WaitUntil([&] { return publisherDone.load(); }));
    ASSERT_EQ(seen.Count(), 1u);
    EXPECT_EQ(seen.Jobs()[0], "Normal");
    EXPECT_EQ(pool.GetStatistics().StealPops - stealPopsBefore, 1u);
}

// A Background job published while the only worker spins (and none sleeps)
// is found by the spin poll, which reports the lane it came from: the job
// runs as Background.
TEST(CurrentJobContextTest, ABackgroundJobFoundByTheSpinnerRunsAsBackground) {
    WorkStealingThreadPool pool(1);
    pool.SetSpinConfigForTest(5'000'000, 1);
    // Wake the worker; after this job it finds nothing and arms its spin.
    pool.EnqueueWork([] {});
    ASSERT_TRUE(WaitUntil(
        [&] { return pool.GetSpinningCountForTests() == 1 && pool.GetSleepingCountForTests() == 0; }));

    Seen seen;
    pool.EnqueueWork([&] { seen.Record(); }, JobPriority::Background);
    ASSERT_TRUE(WaitUntil([&] { return seen.Count() == 1; }));
    EXPECT_EQ(seen.Jobs()[0], "Background");
    pool.SetSpinConfigForTest(0, 0);
}

// The spinner's retiring recheck dequeues a Background job its last lap did
// not reach (the lap polls the Background lane only every eighth iteration):
// with a 1 us window the spin ends before that lap, so a Background job
// published just after the previous one ran is found there or by the lap,
// and it runs as Background either way. Each job is published once the one
// before it has run, so the worker is spinning, not sleeping, when it lands.
TEST(CurrentJobContextTest, BackgroundJobsFoundAsTheSpinnerRetiresRunAsBackground) {
    constexpr int kJobs = 100000;
    WorkStealingThreadPool pool(1);
    pool.SetSpinConfigForTest(1, 1);
    std::atomic<int> ran{0};
    std::atomic<int> notBackground{0};
    for (int i = 0; i < kJobs; ++i) {
        pool.EnqueueWork(
            [&] {
                if (CurrentJob() != "Background") {
                    notBackground.fetch_add(1);
                }
                ran.fetch_add(1, std::memory_order_release);
            },
            JobPriority::Background);
        const auto deadline = SteadyClock::now() + kDeadline;
        while (ran.load(std::memory_order_acquire) <= i && SteadyClock::now() < deadline) {
        }
    }
    ASSERT_EQ(ran.load(), kJobs);
    EXPECT_EQ(notBackground.load(), 0);
    pool.SetSpinConfigForTest(0, 0);
}

// Channel jobs run as their channel on a blocking thread, Submit and Enqueue
// alike, and an Enqueue the shutdown gate refuses runs as its channel on the
// caller.
TEST(CurrentJobContextTest, AChannelJobRunsAsItsChannel) {
    auto pool = std::make_unique<WorkStealingThreadPool>(1);
    auto channel = std::make_unique<JobChannel>(*pool, JobChannelDesc{.Name = kChannelName, .MaxRunning = 2});
    Seen seen;
    channel->Submit([&] { seen.Record(); }).Wait();
    channel->Enqueue([&] { seen.Record(); });
    ASSERT_TRUE(WaitUntil([&] { return seen.Count() == 2; }));

    pool->Shutdown();
    channel->Enqueue([&] { seen.Record(); });
    EXPECT_EQ(seen.Jobs(), (std::vector<std::string>{kChannelName, kChannelName, kChannelName}));
    EXPECT_EQ(CurrentJob(), "") << "the caller's context is restored after the refused job";
    channel.reset();
}

// An inline pool runs each body as the class given at publish, or as its
// channel; a body published from inside another body restores the outer
// body's context when it returns.
TEST(CurrentJobContextTest, AnInlinePoolRunsEachJobAsTheClassGivenAtPublish) {
    WorkStealingThreadPool pool(0);
    JobChannel channel(pool, {.Name = kChannelName, .MaxRunning = 1});
    Seen seen;
    pool.EnqueueWork([&] { seen.Record(); });
    pool.EnqueueWork([&] { seen.Record(); }, JobPriority::Background);
    auto batch = [&] { seen.Record(); };
    pool.EnqueueWorkBatch(&batch, 1, JobPriority::Background);
    (void)pool.Submit([&] { seen.Record(); }, std::span<const TaskHandle>{});
    channel.Enqueue([&] { seen.Record(); });
    pool.EnqueueWork(
        [&] {
            seen.Record();
            pool.EnqueueWork([&] { seen.Record(); });
            seen.Record();
        },
        JobPriority::Background);

    EXPECT_EQ(seen.Jobs(), (std::vector<std::string>{"Normal", "Background", "Background", "Normal", kChannelName,
                                                     "Background", "Normal", "Background"}));
    EXPECT_EQ(CurrentJob(), "");
}

namespace {

// Publishes a Background job that publishes the next one, levels deep; the
// innermost records its context.
void PublishNestedBackground(WorkStealingThreadPool& pool, int levels, Seen& innermost) {
    if (levels == 1) {
        pool.EnqueueWork([&innermost] { innermost.Record(); }, JobPriority::Background);
        return;
    }
    pool.EnqueueWork([&pool, levels, &innermost] { PublishNestedBackground(pool, levels - 1, innermost); },
                     JobPriority::Background);
}

} // namespace

// Published 65 levels deep on an inline pool, a Background job passes the
// trampoline's depth bound, waits in its deferred queue and runs when the
// outermost body returns, still as Background.
TEST(CurrentJobContextTest, AnInlineJobDeferredAtTheDepthBoundRunsAsTheClassGivenAtPublish) {
    constexpr int kLevels = 65; // one past the trampoline's bound of 64 nested executions
    WorkStealingThreadPool pool(0);
    Seen innermost;
    PublishNestedBackground(pool, kLevels, innermost);
    EXPECT_EQ(innermost.Jobs(), (std::vector<std::string>{"Background"}));
    EXPECT_EQ(CurrentJob(), "");
}

// A cap-1 channel's running job holds the only blocking thread while an
// Enqueue job waits behind it. Destroying the channel hands the waiting job
// on to the blocking threads, where it runs as the destroyed channel.
TEST(CurrentJobContextTest, AJobADestroyedChannelHandedOnRunsAsItsChannel) {
    WorkStealingThreadPool pool(1);
    auto channel = std::make_unique<JobChannel>(pool, JobChannelDesc{.Name = kChannelName, .MaxRunning = 1});
    Gate gate;
    GateOpener opener{gate};
    std::atomic<bool> headRunning{false};
    TaskHandle head = channel->Submit([&] {
        headRunning.store(true);
        gate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return headRunning.load(); }));
    Seen handedOn;
    channel->Enqueue([&] { handedOn.Record(); });

    channel.reset();
    gate.Open();
    head.Wait();
    ASSERT_TRUE(WaitUntil([&] { return handedOn.Count() == 1; }));
    EXPECT_EQ(handedOn.Jobs(), (std::vector<std::string>{kChannelName}));
}

// Jobs still queued when the pool shuts down run in the drain on the thread
// that called Shutdown(), each as the lane it was queued in or as its channel.
TEST(CurrentJobContextTest, TheShutdownDrainRunsEachJobAsItsLaneOrChannel) {
    auto pool = std::make_unique<WorkStealingThreadPool>(1);
    auto channel = std::make_unique<JobChannel>(*pool, JobChannelDesc{.Name = kChannelName, .MaxRunning = 1});
    Gate gate;
    GateOpener opener{gate};
    std::atomic<int> parked{0};
    pool->EnqueueWork([&] {
        parked.fetch_add(1);
        gate.Wait();
    });
    channel->Enqueue([&] {
        parked.fetch_add(1);
        gate.Wait();
    });
    ASSERT_TRUE(WaitUntil([&] { return parked.load() == 2; }));

    Seen background;
    Seen normal;
    Seen channelJob;
    pool->EnqueueWork([&] { background.Record(); }, JobPriority::Background);
    pool->EnqueueWork([&] { normal.Record(); });
    channel->Enqueue([&] { channelJob.Record(); });

    WorkStealingThreadPool* shuttingDown = pool.get();
    auto shutdown = std::async(std::launch::async, [shuttingDown] { shuttingDown->Shutdown(); });
    ASSERT_TRUE(WaitUntil([&] { return shuttingDown->IsShuttingDown(); }));
    gate.Open();
    ASSERT_EQ(shutdown.wait_for(kDeadline), std::future_status::ready);

    EXPECT_EQ(background.Jobs(), (std::vector<std::string>{"Background"}));
    EXPECT_EQ(normal.Jobs(), (std::vector<std::string>{"Normal"}));
    EXPECT_EQ(channelJob.Jobs(), (std::vector<std::string>{kChannelName}));
    channel.reset();
}

// A Background body on the only worker forks and waits: the worker
// participates and runs every tagged job itself, as its own class.
TEST(CurrentJobContextTest, AParticipatingWaitRunsTaggedJobsAsTheWaitersClass) {
    WorkStealingThreadPool pool(1);
    Seen seen;
    std::atomic<bool> joined{false};
    pool.EnqueueWork(
        [&] {
            JobCounter counter;
            for (int i = 0; i < 4; ++i) {
                pool.Run([&] { seen.Record(); }, counter);
            }
            pool.Wait(counter);
            joined.store(true);
        },
        JobPriority::Background);
    ASSERT_TRUE(WaitUntil([&] { return joined.load(); }));
    EXPECT_EQ(seen.Jobs(), (std::vector<std::string>(4, "Background")));
}

// A completion callback is not a body: it runs after the context of the job
// that completed has ended, on the worker and on a blocking thread.
TEST(CurrentJobContextTest, CompletionCallbacksRunOutsideTheBody) {
    WorkStealingThreadPool pool(1);
    JobChannel channel(pool, {.Name = kChannelName, .MaxRunning = 1});
    Seen bodies;
    Seen callbacks;
    for (int i = 0; i < 2; ++i) {
        Gate gate;
        GateOpener opener{gate};
        TaskHandle handle = i == 0 ? pool.Submit([&] {
            gate.Wait();
            bodies.Record();
        })
                                   : channel.Submit([&] {
                                         gate.Wait();
                                         bodies.Record();
                                     });
        handle.OnComplete([&] { callbacks.Record(); });
        gate.Open();
        handle.Wait();
        ASSERT_TRUE(WaitUntil([&] { return callbacks.Count() == static_cast<size_t>(i + 1); }));
    }
    EXPECT_EQ(bodies.Jobs(), (std::vector<std::string>{"Normal", kChannelName}));
    EXPECT_EQ(callbacks.Jobs(), (std::vector<std::string>{"", ""}));
}

} // namespace GameEngine::Tests
