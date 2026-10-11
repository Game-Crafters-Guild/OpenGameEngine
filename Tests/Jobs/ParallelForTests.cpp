// ParallelFor, the fork core, and its chunked overload: every unit runs
// once, a fork is legal on a worker, the caller never waits for a helper that
// has not started, a helper that runs after the caller returned touches
// nothing of the caller's, a false or a throw stops the run, a caller parked
// on a helper is woken by every way the helper can leave, a Background body's
// forks publish no Normal work, back-to-back forks on a busy pool keep the
// helpers they leave queued bounded in each lane and get their helper budget
// back as the helpers start, and forks take no heap allocation once the pool
// exists.

#include "HeldSlabFreelist.h"

#include "JobSystem/ParallelAlgorithms.h"
#include "JobSystem/TaskSlabPool.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace JobSystem;

namespace GameEngine::Tests
{

namespace
{

using SteadyClock = std::chrono::steady_clock;

bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = SteadyClock::now() + timeout;
    while (!pred())
    {
        if (SteadyClock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// Holds every worker of the pool inside one task each until Release().
class HeldWorkers
{
  public:
    explicit HeldWorkers(WorkStealingThreadPool& pool)
    {
        const size_t workers = pool.GetWorkerCount();
        for (size_t i = 0; i < workers; ++i)
        {
            pool.EnqueueWork([this] {
                m_Holding.fetch_add(1);
                while (!m_Released.load())
                    std::this_thread::yield();
            });
        }
        EXPECT_TRUE(WaitUntil([this, workers] { return m_Holding.load() == workers; }, std::chrono::seconds(10)));
    }
    void Release() { m_Released.store(true); }

  private:
    std::atomic<size_t> m_Holding{0};
    std::atomic<bool> m_Released{false};
};

// Runs a task on the pool and waits for it to finish, from behind whatever
// the test thread queued before it (one producer's tasks dequeue in order on
// a 1-worker pool).
void DrainBehind(WorkStealingThreadPool& pool)
{
    std::atomic<bool> done{false};
    pool.EnqueueWork([&done] { done.store(true); });
    EXPECT_TRUE(WaitUntil([&done] { return done.load(); }, std::chrono::seconds(10)));
}

TEST(ParallelFor, EveryUnitRunsOnceAcrossTheCallerAndHelpers)
{
    WorkStealingThreadPool pool(4);
    constexpr size_t kUnits = 1000;
    std::vector<std::atomic<uint32_t>> runs(kUnits);
    ParallelForOptions options;
    options.Helpers = pool.GetWorkerCount();
    EXPECT_TRUE(ParallelFor(&pool, kUnits, [&runs](size_t unit) { runs[unit].fetch_add(1); return true; }, options));
    for (size_t unit = 0; unit < kUnits; ++unit)
        EXPECT_EQ(runs[unit].load(), 1u) << "unit " << unit;
}

// The worker-call pin: a ParallelFor from a pool worker, on a pool whose only
// worker is that one, completes; the caller claims every chunk itself.
TEST(ParallelFor, AParallelForFromAWorkerCompletes)
{
    // Leaked if the fork never returns: the worker is then parked for good and
    // the pool's destructor would join it forever.
    auto* pool = new WorkStealingThreadPool(1);
    std::atomic<bool> done{false};
    std::atomic<size_t> covered{0};
    pool->EnqueueWork([pool, &done, &covered] {
        ParallelFor(pool, 4, [&covered](size_t begin, size_t end) { covered.fetch_add(end - begin); }, 1);
        done.store(true);
    });
    if (!WaitUntil([&done] { return done.load(); }, std::chrono::seconds(10)))
    {
        ADD_FAILURE() << "a ParallelFor from the pool's only worker never returned";
        return;
    }
    EXPECT_EQ(covered.load(), 4u);
    delete pool;
}

// A helper still queued when the caller has run every chunk is not waited
// for: ParallelFor returns while the pool's only worker is held, and the
// helper, run later, claims nothing.
TEST(ParallelFor, ARunClosedByTheCallerIsNotWaitedOnByQueuedHelpers)
{
    WorkStealingThreadPool pool(1);
    HeldWorkers held(pool);

    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<uint32_t> chunksOffCaller{0};
    std::atomic<size_t> covered{0};
    ParallelFor(&pool, 2,
                [&](size_t begin, size_t end) {
                    if (std::this_thread::get_id() != caller)
                        chunksOffCaller.fetch_add(1);
                    covered.fetch_add(end - begin);
                },
                1);
    EXPECT_EQ(covered.load(), 2u);

    held.Release();
    DrainBehind(pool);
    EXPECT_EQ(chunksOffCaller.load(), 0u) << "the queued helper claimed a chunk after the run had closed";
}

// The caller's part of a run, kept in a heap block the test frees as soon as
// ParallelFor returns: a helper that touched any of it afterwards is a
// use-after-free, which an ASan build reports however the frame was laid out.
struct CallerFrame
{
    std::atomic<uint32_t> UnitsOffCaller{0};
    std::atomic<uint32_t> AdmissionCalls{0};
    std::thread::id Caller = std::this_thread::get_id();
    std::function<bool()> Stop = [] { return false; };
    ParallelForOptions::UnitAdmission Admission;
    std::function<void()> InsideUnit;

    CallerFrame()
    {
        Admission.TryAcquire = [this] { AdmissionCalls.fetch_add(1); return true; };
        Admission.Release = [] {};
    }

    bool Unit(size_t)
    {
        if (std::this_thread::get_id() != Caller)
            UnitsOffCaller.fetch_add(1);
        if (InsideUnit)
            InsideUnit();
        return true;
    }
};

// One ParallelFor of `units` units with one helper, every caller-side piece
// (body, Stop, Admission) inside `frame`.
bool RunWithFrame(WorkStealingThreadPool& pool, CallerFrame& frame, size_t units)
{
    ParallelForOptions options;
    options.Helpers = 1;
    options.Stop = &frame.Stop;
    options.Admission = &frame.Admission;
    return ParallelFor(&pool, units, [&frame](size_t unit) { return frame.Unit(unit); }, options);
}

#if GE_DEBUG_INSTRUMENTATION
// A hook that holds the first helper reaching `HeldAt` until released.
struct HelperHold
{
    static inline std::atomic<int> HeldAt{-1};
    static inline std::atomic<bool> Held{false};
    static inline std::atomic<bool> Released{false};

    static void Hook(WorkStealingThreadPool::ClaimHelperPoint point)
    {
        if (static_cast<int>(point) != HeldAt.load())
            return;
        bool expected = false;
        if (!Held.compare_exchange_strong(expected, true))
            return;
        while (!Released.load())
            std::this_thread::yield();
    }

    explicit HelperHold(WorkStealingThreadPool::ClaimHelperPoint point)
    {
        Held.store(false);
        Released.store(false);
        HeldAt.store(static_cast<int>(point));
        WorkStealingThreadPool::SetClaimHelperHookForTests(&HelperHold::Hook);
    }
    ~HelperHold()
    {
        Released.store(true);
        WorkStealingThreadPool::SetClaimHelperHookForTests(nullptr);
        HeldAt.store(-1);
    }
    void Release() { Released.store(true); }
};
#endif

// A queued helper that runs after the caller returned touches nothing of the
// caller's: neither its body, nor Stop, nor Admission. Arm 1: the helper is
// still queued at the return and ends at its first load. Arm 2 (Debug): the
// helper has passed its first load on an open run and is held there while
// the caller finishes, returns and frees its frame; released, it counts
// itself in, sees the run closed and leaves.
TEST(ParallelFor, AQueuedHelperRunningAfterTheCallerReturnedTouchesNothingOfTheCaller)
{
    {
        WorkStealingThreadPool pool(1);
        HeldWorkers held(pool);
        auto frame = std::make_unique<CallerFrame>();
        EXPECT_TRUE(RunWithFrame(pool, *frame, 4));
        EXPECT_EQ(frame->UnitsOffCaller.load(), 0u);
        EXPECT_EQ(frame->AdmissionCalls.load(), 0u);
        frame.reset();
        held.Release();
        DrainBehind(pool);
    }

#if GE_DEBUG_INSTRUMENTATION
    {
        WorkStealingThreadPool pool(1);
        HelperHold hold(WorkStealingThreadPool::ClaimHelperPoint::PassedOpenCheck);
        auto frame = std::make_unique<CallerFrame>();
        frame->InsideUnit = [] {
            EXPECT_TRUE(WaitUntil([] { return HelperHold::Held.load(); }, std::chrono::seconds(10)));
        };
        EXPECT_TRUE(RunWithFrame(pool, *frame, 2));
        EXPECT_EQ(frame->UnitsOffCaller.load(), 0u);
        EXPECT_EQ(frame->AdmissionCalls.load(), 0u);
        frame.reset();
        hold.Release();
        DrainBehind(pool);
    }
#else
    std::printf("[ParallelFor] the held-helper arm needs the Debug-only helper hook; not run in this "
                "configuration\n");
#endif
}

// A false from the unit claimed last fails the run although no unit was left
// to skip: the result is "nothing stopped the run", not "every unit ran".
TEST(ParallelFor, AFalseFromTheLastUnitFailsTheRun)
{
    for (const size_t helpers : {size_t{0}, size_t{1}})
    {
        WorkStealingThreadPool pool(2);
        std::atomic<int> started{0};
        std::atomic<bool> firstFinished{false};
        const auto body = [&](size_t) {
            if (started.fetch_add(1) == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                firstFinished.store(true);
                return true;
            }
            EXPECT_TRUE(WaitUntil([&] { return firstFinished.load(); }, std::chrono::seconds(10)));
            return false;
        };
        ParallelForOptions options;
        options.Helpers = helpers;
        EXPECT_FALSE(ParallelFor(&pool, 2, body, options)) << "helpers " << helpers;
        EXPECT_EQ(started.load(), 2) << "helpers " << helpers;
    }
}

TEST(ParallelFor, StopEndsTheRunAndNoUnitStartsAfterIt)
{
    WorkStealingThreadPool pool(4);
    constexpr size_t kUnits = 400;
    std::atomic<uint32_t> ran{0};
    std::atomic<uint32_t> running{0};
    int polls = 0;
    const std::function<bool()> stop = [&polls] { return ++polls >= 3; };
    ParallelForOptions options;
    options.Helpers = pool.GetWorkerCount();
    options.Stop = &stop;
    EXPECT_FALSE(ParallelFor(&pool, kUnits, [&](size_t) {
            running.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            ran.fetch_add(1);
            running.fetch_sub(1);
            return true;
        }, options));
    EXPECT_EQ(running.load(), 0u) << "ParallelFor returned while a unit was still running";
    const uint32_t ranAtReturn = ran.load();
    EXPECT_LT(ranAtReturn, kUnits);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(ran.load(), ranAtReturn) << "a unit started after ParallelFor returned";
}

// The first throw, from any thread, reaches the caller after every unit in
// flight finished, and no unit starts after it. The helper's first unit throws
// once the caller and every other helper are inside a unit; those units return
// 50 ms after the throw, so a unit that starts after it is one the run let
// start after the stop (the thrower's own thread claiming on, or a returning
// unit's thread), not a claim that raced the throw.
TEST(ParallelFor, AHelperThrowIsRethrownOnTheCallerAfterTheUnitsInFlight)
{
    WorkStealingThreadPool pool(4);
    constexpr size_t kUnits = 200;
    const uint32_t parties = static_cast<uint32_t>(pool.GetWorkerCount()) + 1;
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> helperThrew{false};
    std::atomic<bool> throwerChosen{false};
    std::atomic<uint32_t> started{0};
    std::atomic<uint32_t> startedAfterThrow{0};
    std::atomic<uint32_t> running{0};
    std::string thrown;
    ParallelForOptions options;
    options.Helpers = pool.GetWorkerCount();
    try
    {
        ParallelFor(&pool, kUnits, [&](size_t) {
                started.fetch_add(1);
                if (helperThrew.load())
                {
                    startedAfterThrow.fetch_add(1);
                    return true;
                }
                running.fetch_add(1);
                bool expected = false;
                if (std::this_thread::get_id() != caller && throwerChosen.compare_exchange_strong(expected, true))
                {
                    EXPECT_TRUE(WaitUntil([&] { return started.load() >= parties; }, std::chrono::seconds(10)))
                        << "the caller and every helper did not each start a unit";
                    running.fetch_sub(1);
                    helperThrew.store(true);
                    throw std::runtime_error("unit threw on a helper");
                }
                WaitUntil([&] { return helperThrew.load(); }, std::chrono::seconds(10));
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                running.fetch_sub(1);
                return true;
            }, options);
    }
    catch (const std::runtime_error& e)
    {
        thrown = e.what();
    }
    EXPECT_EQ(thrown, "unit threw on a helper");
    EXPECT_EQ(running.load(), 0u);
    EXPECT_LT(started.load(), kUnits) << "the throw did not stop the run";
    EXPECT_EQ(startedAfterThrow.load(), 0u) << "a unit started after the helper's throw";
}

// Runs `fork` on a thread of its own and reports whether it returned within
// 10 s. A caller parked for good then fails the test instead of hanging the
// run: its thread is detached, and whatever `fork` refers to must be leaked
// with it.
bool ForkReturnsInTime(std::function<void()> fork)
{
    auto returned = std::make_shared<std::atomic<bool>>(false);
    std::thread thread([fork = std::move(fork), returned] {
        fork();
        returned->store(true);
    });
    if (!WaitUntil([&returned] { return returned->load(); }, std::chrono::seconds(10)))
    {
        thread.detach();
        return false;
    }
    thread.join();
    return true;
}

// The caller parks while a helper is in the middle of a chunk; the helper's
// exit through the claim loop (nothing left to claim) wakes it.
struct LoopingHelperFork
{
    WorkStealingThreadPool Pool{2};
    std::atomic<bool> HelperStarted{false};
    std::thread::id Caller;

    void Run()
    {
        Caller = std::this_thread::get_id();
        ParallelFor(&Pool, 2, [this](size_t, size_t) { Chunk(); }, 1);
    }

    // The caller's chunk returns once the helper's has started; the helper's
    // chunk outlasts it, so the caller closes the run and parks.
    void Chunk()
    {
        if (std::this_thread::get_id() == Caller)
        {
            EXPECT_TRUE(WaitUntil([this] { return HelperStarted.load(); }, std::chrono::seconds(10)));
            return;
        }
        HelperStarted.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
};

TEST(ParallelFor, ACallerParkedOnALoopingHelperIsWokenByItsExit)
{
    // Leaked if the caller is never woken: its thread still refers to it.
    auto* fork = new LoopingHelperFork;
#if GE_DEBUG_INSTRUMENTATION
    const uint64_t parksBefore = WorkStealingThreadPool::GetClaimCallerParksForTests();
#endif
    if (!ForkReturnsInTime([fork] { fork->Run(); }))
    {
        ADD_FAILURE() << "the caller parked on the helper's chunk was never woken";
        return;
    }
    EXPECT_TRUE(fork->HelperStarted.load());
#if GE_DEBUG_INSTRUMENTATION
    EXPECT_EQ(WorkStealingThreadPool::GetClaimCallerParksForTests() - parksBefore, 1u)
        << "the caller did not park on the helper's chunk";
#endif
    delete fork;
}

#if GE_DEBUG_INSTRUMENTATION
// The caller parks on a helper that counted itself in progress but has not
// yet re-read the run; the helper then sees the run closed and backs out,
// and that back-out wakes the caller.
struct BackingOutHelperFork
{
    WorkStealingThreadPool Pool{1};
    std::atomic<uint32_t> Ran{0};
    bool Result = false;

    void Run()
    {
        ParallelForOptions options;
        options.Helpers = 1;
        Result = ParallelFor(&Pool, 2, [this](size_t) { return Unit(); }, options);
    }

    // Holds until the helper is held between counting itself in and re-reading
    // the run, so it cannot end at its first load instead.
    bool Unit()
    {
        EXPECT_TRUE(WaitUntil([] { return HelperHold::Held.load(); }, std::chrono::seconds(10)));
        Ran.fetch_add(1);
        return true;
    }
};

TEST(ParallelFor, ACallerParkedOnAHelperThatBacksOutIsWoken)
{
    HelperHold hold(WorkStealingThreadPool::ClaimHelperPoint::CountedInProgress);
    const uint64_t parksBefore = WorkStealingThreadPool::GetClaimCallerParksForTests();
    std::thread releaser([&hold, parksBefore] {
        EXPECT_TRUE(WaitUntil(
            [parksBefore] { return WorkStealingThreadPool::GetClaimCallerParksForTests() != parksBefore; },
            std::chrono::seconds(10)));
        // std::atomic::wait polls the word for a while before it blocks; the
        // release waits until the caller is past that, so only a notify can
        // wake it.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        hold.Release();
    });

    // Leaked if the caller is never woken: its thread still refers to it.
    auto* fork = new BackingOutHelperFork;
    const bool returned = ForkReturnsInTime([fork] { fork->Run(); });
    releaser.join();
    if (!returned)
    {
        ADD_FAILURE() << "the caller parked on the backing-out helper was never woken";
        return;
    }
    EXPECT_TRUE(fork->Result);
    EXPECT_EQ(fork->Ran.load(), 2u);
    EXPECT_EQ(WorkStealingThreadPool::GetClaimCallerParksForTests() - parksBefore, 1u);
    delete fork;
}
#else
TEST(ParallelFor, ACallerParkedOnAHelperThatBacksOutIsWoken)
{
    GTEST_SKIP() << "needs the Debug-only helper hook (GE_DEBUG_INSTRUMENTATION)";
}
#endif

#if GE_DEBUG_INSTRUMENTATION
// A helper admitted after the caller claimed the last unit finds nothing left
// at its claim and releases the admission it took: every TryAcquire that
// returned true is matched by a Release, so a lost race leaks no slot of the
// admission's gate. The helper is held after counting itself in while the
// caller runs unit 0; the caller claims the last unit, releases the helper
// from inside it and stays there until the helper was admitted, so the
// helper's claim is the one past the end.
struct PastTheEndAdmissionFork
{
    WorkStealingThreadPool Pool{1};
    std::atomic<uint32_t> Acquires{0};
    std::atomic<uint32_t> Releases{0};
    std::atomic<uint32_t> UnitsOffCaller{0};
    std::thread::id Caller = std::this_thread::get_id();
    ParallelForOptions::UnitAdmission Admission;

    PastTheEndAdmissionFork()
    {
        Admission.TryAcquire = [this] { Acquires.fetch_add(1); return true; };
        Admission.Release = [this] { Releases.fetch_add(1); };
    }

    bool Run()
    {
        ParallelForOptions options;
        options.Helpers = 1;
        options.Admission = &Admission;
        return ParallelFor(&Pool, 2, [this](size_t unit) { return Unit(unit); }, options);
    }

    bool Unit(size_t unit)
    {
        if (std::this_thread::get_id() != Caller)
        {
            UnitsOffCaller.fetch_add(1);
            return true;
        }
        if (unit == 0)
        {
            EXPECT_TRUE(WaitUntil([] { return HelperHold::Held.load(); }, std::chrono::seconds(10)));
            return true;
        }
        HelperHold::Released.store(true);
        EXPECT_TRUE(WaitUntil([this] { return Acquires.load() == 1; }, std::chrono::seconds(10)))
            << "the released helper was never admitted";
        return true;
    }
};

TEST(ParallelFor, AHelperAdmittedPastTheEndReleasesItsAdmission)
{
    HelperHold hold(WorkStealingThreadPool::ClaimHelperPoint::CountedInProgress);
    PastTheEndAdmissionFork fork;
    EXPECT_TRUE(fork.Run());
    DrainBehind(fork.Pool);
    EXPECT_EQ(fork.UnitsOffCaller.load(), 0u) << "the helper claimed a unit; its claim was not the one past the end";
    EXPECT_EQ(fork.Acquires.load(), 1u);
    EXPECT_EQ(fork.Releases.load(), fork.Acquires.load()) << "an admission taken for a claim past the end was kept";
}
#else
TEST(ParallelFor, AHelperAdmittedPastTheEndReleasesItsAdmission)
{
    GTEST_SKIP() << "needs the Debug-only helper hook (GE_DEBUG_INSTRUMENTATION)";
}
#endif

// A Background body's forks, Run + Wait and ParallelFor alike, publish into
// the Background lane only: the census's cumulative push counters (never a
// sampled queue depth, which a stub published and dequeued between two
// samples would pass) show no Normal push beyond none. Repeated on an idle
// pool with spinning enabled, so some bodies are found by a spinner.
void ForkFromBackgroundBodies(WorkStealingThreadPool& pool, int rounds)
{
    const auto before = pool.GetStatistics();
    std::atomic<int> finished{0};
    for (int round = 0; round < rounds; ++round)
    {
        // Let the workers go idle (and arm their spin windows) between rounds.
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        pool.EnqueueWork(
            [&pool, &finished] {
                JobCounter counter;
                std::atomic<int> sink{0};
                for (int i = 0; i < 4; ++i)
                    pool.Run([&sink] { sink.fetch_add(1); }, counter);
                pool.Wait(counter);
                ParallelFor(&pool, 64, [&sink](size_t, size_t) { sink.fetch_add(1); }, 1);
                finished.fetch_add(1);
            },
            JobPriority::Background);
    }
    ASSERT_TRUE(WaitUntil([&] { return finished.load() == rounds; }, std::chrono::seconds(20)));
    ASSERT_TRUE(WaitUntil([&] { return pool.GetPendingTasksApprox() == 0 && pool.GetBackgroundQueuedForTests() == 0; },
                          std::chrono::seconds(10)));
    const auto after = pool.GetStatistics();
    EXPECT_GT(after.BackgroundPushes - before.BackgroundPushes, static_cast<uint64_t>(rounds));
    EXPECT_EQ(after.GlobalPushes - before.GlobalPushes, 0u) << "a Background body's fork published Normal work";
    EXPECT_EQ(after.LocalPushes - before.LocalPushes, 0u);
}

TEST(ParallelFor, BackgroundBodyForkPublishesNoNormalWork)
{
    {
        WorkStealingThreadPool pool(4);
        pool.SetSpinConfigForTest(0, 0);
        ForkFromBackgroundBodies(pool, 20);
    }
    {
        WorkStealingThreadPool pool(4);
        pool.SetSpinConfigForTest(2000, 4);
        ForkFromBackgroundBodies(pool, 50);
    }
}

// A thread that is no worker forking back to back while every worker is busy:
// the caller runs every unit of each run and closes it, and the helpers of the
// closed runs wait in the queue. The pool keeps at most
// kMaxUnstartedClaimHelpersPerWorker unstarted helpers per worker in a lane,
// so the queue and the slabs it holds (each queued helper's envelope and at
// most one run block) stay within that bound however many forks are issued,
// and drain once the workers are free.
TEST(ParallelFor, BackToBackForksOnABusyPoolKeepTheQueuedHelpersBounded)
{
    WorkStealingThreadPool pool(4);
    HeldWorkers held(pool);
    const int64_t slabsBefore = JobSystem::Detail::GetTaskSlabStatsForTests().Live;
    const size_t bound = WorkStealingThreadPool::kMaxUnstartedClaimHelpersPerWorker * pool.GetWorkerCount();

    constexpr size_t kForks = 1000;
    std::atomic<size_t> covered{0};
    for (size_t fork = 0; fork < kForks; ++fork)
        ParallelFor(&pool, 8, [&covered](size_t begin, size_t end) { covered.fetch_add(end - begin); }, 1);
    EXPECT_EQ(covered.load(), kForks * 8);

    EXPECT_LE(pool.GetApproximateQueueSize(), bound);
    EXPECT_LE(JobSystem::Detail::GetTaskSlabStatsForTests().Live - slabsBefore, static_cast<int64_t>(2 * bound));

    held.Release();
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0; }, std::chrono::seconds(10)));
    EXPECT_TRUE(WaitUntil([slabsBefore] { return JobSystem::Detail::GetTaskSlabStatsForTests().Live <= slabsBefore; },
                          std::chrono::seconds(10)));
}

// Publishes one ParallelFor of 8 units with a helper per worker while every
// worker is held, and returns how many helpers it left queued: the pool's
// whole width when none of its helper budget is in use.
size_t HelpersPublishedByAForkOnAHeldPool(WorkStealingThreadPool& pool)
{
    HeldWorkers held(pool);
    ParallelForOptions options;
    options.Helpers = pool.GetWorkerCount();
    ParallelFor(&pool, 8, [](size_t) { return true; }, options);
    const size_t queued = pool.GetApproximateQueueSize();
    held.Release();
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0; }, std::chrono::seconds(10)));
    return queued;
}

// Every helper gives its count back when it starts, so the budget is whole
// again once the queue has drained: after the back-to-back storm, and after
// an admitted run whose helpers re-published themselves between units.
TEST(ParallelFor, TheHelperBudgetComesBackAsHelpersStart)
{
    WorkStealingThreadPool pool(4);
    {
        HeldWorkers held(pool);
        for (size_t fork = 0; fork < 100; ++fork)
            ParallelFor(&pool, 8, [](size_t, size_t) {}, 1);
        held.Release();
        EXPECT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0; }, std::chrono::seconds(10)));
    }
    EXPECT_EQ(HelpersPublishedByAForkOnAHeldPool(pool), pool.GetWorkerCount());

    // Units long enough that the helpers run some, each followed by a
    // re-publish while units are left.
    ParallelForOptions::UnitAdmission admission;
    admission.TryAcquire = [] { return true; };
    admission.Release = [] {};
    ParallelForOptions options;
    options.Helpers = pool.GetWorkerCount();
    options.Admission = &admission;
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<uint32_t> offCaller{0};
    EXPECT_TRUE(ParallelFor(&pool, 64, [&](size_t) {
            if (std::this_thread::get_id() != caller)
                offCaller.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            return true;
        }, options));
    EXPECT_GT(offCaller.load(), 0u) << "no helper ran a unit, so none re-published";
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0; }, std::chrono::seconds(10)));
    EXPECT_EQ(HelpersPublishedByAForkOnAHeldPool(pool), pool.GetWorkerCount());
}

// A helper gives its count back before it runs a unit, not when it ends: with
// every worker inside a long unit of one run, the whole budget is still open
// to the helpers of other forks, which queue behind the busy workers.
TEST(ParallelFor, AHelperInsideAUnitNoLongerHoldsItsCount)
{
    WorkStealingThreadPool pool(4);
    const size_t workers = pool.GetWorkerCount();
    const size_t bound = WorkStealingThreadPool::kMaxUnstartedClaimHelpersPerWorker * workers;

    std::atomic<size_t> entered{0};
    std::atomic<bool> gate{false};
    std::thread longRunCaller([&] {
        ParallelForOptions options;
        options.Helpers = workers;
        ParallelFor(&pool, 64, [&](size_t) {
                entered.fetch_add(1);
                while (!gate.load())
                    std::this_thread::yield();
                return true;
            }, options);
    });
    // The long run's caller and one helper per worker are each inside a unit.
    ASSERT_TRUE(WaitUntil([&] { return entered.load() == workers + 1; }, std::chrono::seconds(10)));

    ParallelForOptions options;
    options.Helpers = workers;
    for (size_t fork = 0; fork < bound / workers; ++fork)
        ParallelFor(&pool, 8, [](size_t) { return true; }, options);
    EXPECT_EQ(pool.GetApproximateQueueSize(), bound) << "the helpers running units still held part of the budget";

    gate.store(true);
    longRunCaller.join();
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0; }, std::chrono::seconds(10)));
}

// Each lane has its own budget: Background helpers of closed runs queued
// behind busy workers up to their lane's bound (cook bands behind a streaming
// flood) leave a Normal fork its helpers.
TEST(ParallelFor, QueuedBackgroundHelpersLeaveANormalForkItsHelpers)
{
    WorkStealingThreadPool pool(4);
    const size_t workers = pool.GetWorkerCount();
    const size_t bound = WorkStealingThreadPool::kMaxUnstartedClaimHelpersPerWorker * workers;
    HeldWorkers held(pool);

    ParallelForOptions background;
    background.Helpers = workers;
    background.HelperPriority = JobPriority::Background;
    for (size_t fork = 0; fork < 100; ++fork)
        ParallelFor(&pool, 8, [](size_t) { return true; }, background);
    EXPECT_EQ(pool.GetBackgroundQueuedForTests(), bound);

    ParallelForOptions normal;
    normal.Helpers = workers;
    ParallelFor(&pool, 8, [](size_t) { return true; }, normal);
    EXPECT_EQ(pool.GetApproximateQueueSize(), workers) << "the Background helpers took the Normal fork's budget";

    held.Release();
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0 && pool.GetBackgroundQueuedForTests() == 0; },
                          std::chrono::seconds(10)));
}

// A pool provisions the slabs its ParallelFor helpers can hold, so forks on it
// take no heap allocation however many are issued and however busy the pool
// is: 1,000 forks into each lane from a thread that is no worker, every worker
// held, the freelist emptied before the pool was built. Each fork asks for one
// helper, so every queued helper holds a run block of its own, and both lanes
// fill to their bound of 16: the 4 held workers' envelopes plus 2 lanes x 16
// queued helpers x (an envelope + a run block) make 68 slabs, under the pool's
// 73 (2 x (2 x 4 + 1) x 4 + 1) and over what provisioning one lane (41) or the
// envelopes without their run blocks (37) would give.
TEST(ParallelFor, DefaultOptionsPublishOneHelperPerWorker)
{
    WorkStealingThreadPool pool(4);
    HeldWorkers held(pool);

    std::atomic<size_t> covered{0};
    ParallelFor(&pool, 8, [&covered](size_t) { covered.fetch_add(1); });
    EXPECT_EQ(covered.load(), 8u) << "the caller runs every unit while the workers are held";
    EXPECT_EQ(pool.GetApproximateQueueSize(), pool.GetWorkerCount())
        << "default options publish one helper per worker";

    held.Release();
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0; }, std::chrono::seconds(10)));
}

TEST(ParallelFor, ForksTakeNoHeapAllocationOnceThePoolExists)
{
    const HeldSlabFreelist emptied;
    WorkStealingThreadPool pool(4);
    const uint64_t heapAllocsAfterConstruction = JobSystem::Detail::GetTaskSlabStatsForTests().HeapAllocs;
    HeldWorkers held(pool);
    const size_t bound = WorkStealingThreadPool::kMaxUnstartedClaimHelpersPerWorker * pool.GetWorkerCount();

    ParallelForOptions normal;
    normal.Helpers = 1;
    ParallelForOptions background;
    background.Helpers = 1;
    background.HelperPriority = JobPriority::Background;
    constexpr size_t kForks = 1000;
    std::atomic<size_t> covered{0};
    const auto cover = [&covered](size_t) { covered.fetch_add(1); };
    for (size_t fork = 0; fork < kForks; ++fork)
    {
        ParallelFor(&pool, 8, cover, normal);
        ParallelFor(&pool, 8, cover, background);
    }
    EXPECT_EQ(covered.load(), 2 * kForks * 8);
    EXPECT_EQ(pool.GetApproximateQueueSize(), bound) << "the Normal lane did not fill to its bound";
    EXPECT_EQ(pool.GetBackgroundQueuedForTests(), bound) << "the Background lane did not fill to its bound";
    EXPECT_EQ(JobSystem::Detail::GetTaskSlabStatsForTests().HeapAllocs - heapAllocsAfterConstruction, 0u);

    held.Release();
    EXPECT_TRUE(WaitUntil([&pool] { return pool.GetPendingTasksApprox() == 0 && pool.GetBackgroundQueuedForTests() == 0; },
                          std::chrono::seconds(10)));
}

// An admission that refuses ends the helper; the caller runs every unit.
TEST(ParallelFor, ARefusedAdmissionLeavesTheUnitsToTheCaller)
{
    WorkStealingThreadPool pool(4);
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<uint32_t> offCaller{0};
    ParallelForOptions::UnitAdmission admission;
    admission.TryAcquire = [] { return false; };
    admission.Release = [] {};
    ParallelForOptions options;
    options.Helpers = pool.GetWorkerCount();
    options.Admission = &admission;
    EXPECT_TRUE(ParallelFor(&pool, 64, [&](size_t) {
            if (std::this_thread::get_id() != caller)
                offCaller.fetch_add(1);
            return true;
        }, options));
    EXPECT_EQ(offCaller.load(), 0u);
}

} // namespace

} // namespace GameEngine::Tests
