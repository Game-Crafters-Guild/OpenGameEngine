// CompletionMutex waiter-count gate: stress + A/B benches
// (the slice-6 "CompletionMutex waiter-count Dekker" diet item).
//
// The stress tests in this file pin the ONE property the gate is allowed to
// bet on: TaskHandle::Wait from an external thread NEVER strands, no matter
// how the waiter's registration interleaves with the completer's terminal
// flip. TaskHandle::Wait has NO backstop timeout (unlike the worker sleep
// path), so a lost wakeup here is a permanent hang — every stress runs under
// a watchdog that aborts loudly instead of wedging the suite.
//
// The benches are the interleaved-A/B measurement surface: they are compiled
// IDENTICALLY into the baseline (origin/main product code) and gated
// binaries, and log ambient whole-system CPU per run so cross-binary pairs
// can be judged against box drift. Two shapes:
//  - BENCHMARK_SubmitCompleteLatency: single-task submit -> IsDone ping-pong.
//    Serializes the worker's completion tail (status exchange + notify fence
//    + cleanup) into the next iteration's latency.
//  - BENCHMARK_HandleCompletionThroughput: pool(1), two producers, bounded
//    in-flight window — the single worker is the bottleneck, so tasks/sec is
//    ~1/worker-per-task-cost, where the notify fence lives.
//  - BENCHMARK_WarmStormFork (WakeProtocolAndSpinTests.cpp) is the A/B
//    CONTROL row: the JobCounter fork-join path never touches TaskData's
//    CompletionMutex, so it must not move.

#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "TestPlatform.h"

namespace GameEngine::Tests {

namespace {

using SteadyClock = std::chrono::steady_clock;

double ElapsedNs(SteadyClock::time_point t0, SteadyClock::time_point t1)
{
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

void BusyWaitNs(int64_t ns)
{
    if (ns <= 0)
        return;
    const auto deadline = SteadyClock::now() + std::chrono::nanoseconds(ns);
    while (SteadyClock::now() < deadline)
    {
        YieldProcessor();
    }
}

double Percentile(const std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    const double idx = p * static_cast<double>(sorted.size() - 1);
    return sorted[static_cast<size_t>(idx + 0.5)];
}

// Ambient-load bracket (measurement law: every perf number ships with the
// whole-box CPU busy%% over its own window). Windows: GetSystemTimes, whose
// kernel time INCLUDES idle time; busy = (kernel - idle + user) /
// (kernel + user). Other hosts report 0% — "unmeasured", not "idle".
struct AmbientCpuSample
{
    uint64_t Idle = 0;
    uint64_t Kernel = 0;
    uint64_t User = 0;
};

AmbientCpuSample SampleSystemCpu()
{
    AmbientCpuSample s;
#if GE_TEST_HAS_WIN_TIMERS
    FILETIME idle{}, kernel{}, user{};
    GetSystemTimes(&idle, &kernel, &user);
    const auto pack = [](const FILETIME& ft) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    s.Idle = pack(idle);
    s.Kernel = pack(kernel);
    s.User = pack(user);
#endif
    return s;
}

double AmbientBusyPercent(const AmbientCpuSample& a, const AmbientCpuSample& b)
{
    const double idle = static_cast<double>(b.Idle - a.Idle);
    const double kernel = static_cast<double>(b.Kernel - a.Kernel);
    const double user = static_cast<double>(b.User - a.User);
    const double total = kernel + user;
    return total > 0.0 ? 100.0 * (total - idle) / total : 0.0;
}

void LogTimerPeriod()
{
#if GE_TEST_HAS_WIN_TIMERS
    using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
    {
        if (auto fn = reinterpret_cast<NtQueryTimerResolutionFn>(
                GetProcAddress(ntdll, "NtQueryTimerResolution")))
        {
            ULONG minRes = 0, maxRes = 0, curRes = 0;
            if (fn(&minRes, &maxRes, &curRes) == 0)
            {
                std::printf("[timer] current period %.3fms\n", curRes / 10000.0);
            }
        }
    }
#else
    std::printf("[timer] host timer-resolution diagnostics unavailable on this platform\n");
#endif
}

// A lost wakeup in TaskHandle::Wait wedges forever (no backstop). The
// watchdog turns "hung" into an unmissable abort with the stall location.
class StallWatchdog
{
  public:
    StallWatchdog(const std::atomic<uint64_t>& progress, int stallBudgetSeconds,
                  const char* label)
        : m_Progress(progress)
    {
        m_Thread = std::thread([this, stallBudgetSeconds, label] {
            uint64_t last = m_Progress.load(std::memory_order_relaxed);
            auto lastChange = SteadyClock::now();
            while (!m_Stop.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                const uint64_t now = m_Progress.load(std::memory_order_relaxed);
                if (now != last)
                {
                    last = now;
                    lastChange = SteadyClock::now();
                    continue;
                }
                if (SteadyClock::now() - lastChange >
                    std::chrono::seconds(stallBudgetSeconds))
                {
                    std::fprintf(stderr,
                                 "[%s] WATCHDOG: no progress for %ds at iteration %llu — "
                                 "lost wakeup in TaskHandle::Wait. Aborting.\n",
                                 label, stallBudgetSeconds,
                                 static_cast<unsigned long long>(now));
                    std::fflush(stderr);
                    std::abort();
                }
            }
        });
    }

    ~StallWatchdog()
    {
        m_Stop.store(true, std::memory_order_relaxed);
        m_Thread.join();
    }

  private:
    const std::atomic<uint64_t>& m_Progress;
    std::atomic<bool> m_Stop{false};
    std::thread m_Thread;
};

} // namespace

// ============================================================================
// Stress: external-thread Wait vs completion, every interleaving phase
// ============================================================================

// 150k iterations of Submit + immediate external Wait, with randomized task
// body length and randomized submit->Wait gap so the waiter's register/recheck
// lands before, during, and after the completer's terminal flip + gated
// notify (the LostWakeupStress phase-sweep pattern, adapted: there is no
// rescue to disable because TaskHandle::Wait has none — the watchdog IS the
// detector).
TEST(CompletionWaiterTest, ExternalWaitNeverStrands)
{
    LogTimerPeriod();
    constexpr uint64_t kIterations = 150'000;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);
    JobSystem::WorkStealingThreadPool pool(4);

    std::atomic<uint64_t> progress{0};
    StallWatchdog watchdog(progress, 30, "ExternalWaitNeverStrands");

    // Cheap xorshift so the RNG cost doesn't dominate the phase sweep.
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto nextRand = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    for (uint64_t i = 0; i < kIterations; ++i)
    {
        const int64_t bodyNs = static_cast<int64_t>(nextRand() % 3000);  // 0-3us
        const int64_t gapNs = static_cast<int64_t>(nextRand() % 3000);   // 0-3us

        auto handle = pool.Submit([bodyNs] { BusyWaitNs(bodyNs); });
        ASSERT_TRUE(handle.IsValid());
        BusyWaitNs(gapNs);
        handle.Wait(); // must never strand, in any phase
        ASSERT_TRUE(handle.IsDone());
        progress.fetch_add(1, std::memory_order_relaxed);
    }

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    pool.Shutdown();
    SUCCEED() << kIterations << " Wait-vs-complete races without a strand";
}

// Multiple concurrent waiters on ONE handle: the completer's single gated
// notify decision must reach all of them (notify_all under the mutex). Three
// persistent waiter threads + the main thread all Wait on the same task,
// 20k rounds, generation-handoff protocol.
TEST(CompletionWaiterTest, MultiWaiterAllWake)
{
    constexpr uint64_t kRounds = 20'000;
    constexpr int kHelperWaiters = 3;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);
    JobSystem::WorkStealingThreadPool pool(4);

    std::atomic<uint64_t> progress{0};
    StallWatchdog watchdog(progress, 30, "MultiWaiterAllWake");

    JobSystem::TaskHandle shared;
    std::atomic<uint64_t> generation{0};
    std::atomic<int> waitersDone{0};
    std::atomic<bool> stop{false};

    auto helper = [&] {
        uint64_t seen = 0;
        for (;;)
        {
            while (generation.load(std::memory_order_acquire) == seen)
            {
                if (stop.load(std::memory_order_relaxed))
                {
                    return;
                }
                YieldProcessor();
            }
            seen = generation.load(std::memory_order_acquire);
            // `shared` was written before the generation bump (release) and
            // is not rewritten until every helper reports done.
            JobSystem::TaskHandle mine = shared;
            mine.Wait();
            waitersDone.fetch_add(1, std::memory_order_release);
        }
    };

    std::vector<std::thread> helpers;
    helpers.reserve(kHelperWaiters);
    for (int t = 0; t < kHelperWaiters; ++t)
    {
        helpers.emplace_back(helper);
    }

    uint64_t rng = 0xC0FFEE123456789ull;
    auto nextRand = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    for (uint64_t round = 0; round < kRounds; ++round)
    {
        const int64_t bodyNs = static_cast<int64_t>(nextRand() % 2000);
        waitersDone.store(0, std::memory_order_relaxed);
        shared = pool.Submit([bodyNs] { BusyWaitNs(bodyNs); });
        ASSERT_TRUE(shared.IsValid());
        generation.fetch_add(1, std::memory_order_release);

        shared.Wait(); // main thread is the 4th waiter
        while (waitersDone.load(std::memory_order_acquire) < kHelperWaiters)
        {
            YieldProcessor();
        }
        progress.fetch_add(1, std::memory_order_relaxed);
    }

    stop.store(true, std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_release); // release spinning helpers
    for (auto& t : helpers)
    {
        t.join();
    }

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    pool.Shutdown();
    SUCCEED() << kRounds << " rounds x " << (kHelperWaiters + 1) << " concurrent waiters";
}

// Cancel vs execute arbitration with a live waiter: pool(1) with a short
// blocker keeps the victim Pending long enough that Cancel() genuinely races
// the worker's dequeue CAS. Whichever side wins the F12 arbitration, the
// parked waiter must be woken (MarkCompleted's gated notify on the worker
// path, FireCancelledOn's gated notify on the cancel path). 60k rounds mixes
// both winners.
TEST(CompletionWaiterTest, CancelVsCompleteBothWakeWaiter)
{
    constexpr uint64_t kRounds = 60'000;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);
    JobSystem::WorkStealingThreadPool pool(1);

    std::atomic<uint64_t> progress{0};
    StallWatchdog watchdog(progress, 30, "CancelVsCompleteBothWakeWaiter");

    JobSystem::TaskHandle shared;
    std::atomic<uint64_t> generation{0};
    std::atomic<int> waiterDone{0};
    std::atomic<bool> stop{false};

    std::thread waiter([&] {
        uint64_t seen = 0;
        for (;;)
        {
            while (generation.load(std::memory_order_acquire) == seen)
            {
                if (stop.load(std::memory_order_relaxed))
                {
                    return;
                }
                YieldProcessor();
            }
            seen = generation.load(std::memory_order_acquire);
            JobSystem::TaskHandle mine = shared;
            mine.Wait();
            waiterDone.store(1, std::memory_order_release);
        }
    });

    uint64_t rng = 0xDEADBEEFCAFEF00Dull;
    auto nextRand = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    uint64_t cancelWins = 0;
    uint64_t workerWins = 0;

    for (uint64_t round = 0; round < kRounds; ++round)
    {
        // Half the rounds run WITHOUT a blocker (worker snaps the victim up
        // immediately -> the execute side usually wins the arbitration); the
        // other half park the victim behind a short blocker while the
        // canceller races in (cancel usually wins). The gap sweep straddles
        // both regimes so the CAS race is genuinely exercised from each side.
        const bool useBlocker = (nextRand() & 1) != 0;
        const int64_t blockerNs = static_cast<int64_t>(nextRand() % 2000); // 0-2us
        const int64_t gapNs = static_cast<int64_t>(nextRand() % 8000);     // 0-8us

        if (useBlocker)
        {
            pool.EnqueueWork([blockerNs] { BusyWaitNs(blockerNs); });
        }
        waiterDone.store(0, std::memory_order_relaxed);
        shared = pool.Submit([] {});
        ASSERT_TRUE(shared.IsValid());
        generation.fetch_add(1, std::memory_order_release);

        BusyWaitNs(gapNs);
        if (shared.Cancel())
        {
            ++cancelWins;
        }
        else
        {
            ++workerWins;
        }

        while (waiterDone.load(std::memory_order_acquire) == 0)
        {
            YieldProcessor();
        }
        ASSERT_TRUE(shared.IsDone());
        progress.fetch_add(1, std::memory_order_relaxed);
    }

    stop.store(true, std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_release);
    waiter.join();

    std::printf("[CancelVsCompleteBothWakeWaiter] rounds=%llu cancelWins=%llu workerWins=%llu\n",
                static_cast<unsigned long long>(kRounds),
                static_cast<unsigned long long>(cancelWins),
                static_cast<unsigned long long>(workerWins));
    // The mix must genuinely exercise both completer paths.
    EXPECT_GT(cancelWins, kRounds / 100);
    EXPECT_GT(workerWins, kRounds / 100);

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    pool.Shutdown();
}

// Adversarial variant (PR #358 audit): the ExternalWaitNeverStrands sweep
// randomizes the SUBMIT->Wait gap, so most iterations land far from the
// terminal flip. This one gates the waiters on the task body's LAST
// instruction (an atomic the body flips before returning), so the
// register/recheck lands inside the few-hundred-ns window between Execute()
// returning and the completer's C1 exchange + C2 count load — the exact
// Dekker crossing. A 0-800ns stagger sweeps W1 across C1, C2, and the fence.
//
// Two waiters race every round: a second waiter's fast-exit DECREMENT (1->0)
// is in flight against the first waiter's registration and the completer's
// C2 load — the "count drops to zero while another waiter is between W1 and
// park" shape. Both waiters also pin the skip-path OUTCOME PAYLOAD: a waiter
// that returns from Wait without ever touching CompletionMutex (W2/fast-path
// exit) must still observe the task's Result through C1's release edge —
// TryGetResult must succeed with the right value on every round.
TEST(CompletionWaiterTest, WaitRegistrationAtCompletionWindow)
{
    constexpr uint64_t kIterations = 120'000;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);
    JobSystem::WorkStealingThreadPool pool(2);

    std::atomic<uint64_t> progress{0};
    StallWatchdog watchdog(progress, 30, "WaitRegistrationAtCompletionWindow");

    JobSystem::TaskHandle shared;
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> bodyDone{false};
    std::atomic<int> helperDone{0};
    std::atomic<uint64_t> payloadFailures{0};
    std::atomic<bool> stop{false};

    std::thread helper([&] {
        uint64_t seen = 0;
        for (;;)
        {
            while (generation.load(std::memory_order_acquire) == seen)
            {
                if (stop.load(std::memory_order_relaxed))
                {
                    return;
                }
                YieldProcessor();
            }
            if (stop.load(std::memory_order_relaxed))
            {
                return; // final release-bump: no fresh round behind it
            }
            seen = generation.load(std::memory_order_acquire);
            JobSystem::TaskHandle mine = shared;
            // Gate on the body's last instruction: register at the window.
            while (!bodyDone.load(std::memory_order_acquire))
            {
                YieldProcessor();
            }
            mine.Wait();
            uint64_t value = 0;
            if (!mine.TryGetResult(value) || value != (seen ^ 0x5DEECE66Dull))
            {
                payloadFailures.fetch_add(1, std::memory_order_relaxed);
            }
            helperDone.store(1, std::memory_order_release);
        }
    });

    uint64_t rng = 0xB5297A4D3F84D5B5ull;
    auto nextRand = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    for (uint64_t i = 0; i < kIterations; ++i)
    {
        const uint64_t expected = (i + 1) ^ 0x5DEECE66Dull; // generation is i+1
        bodyDone.store(false, std::memory_order_relaxed);
        helperDone.store(0, std::memory_order_relaxed);
        shared = pool.Submit([&bodyDone, expected]() -> uint64_t {
            bodyDone.store(true, std::memory_order_release);
            return expected;
        });
        ASSERT_TRUE(shared.IsValid());
        generation.fetch_add(1, std::memory_order_release);

        // Main thread: same gate, staggered 0-800ns to sweep the crossing.
        while (!bodyDone.load(std::memory_order_acquire))
        {
            YieldProcessor();
        }
        BusyWaitNs(static_cast<int64_t>(nextRand() % 800));
        shared.Wait(); // must never strand, in any phase
        ASSERT_TRUE(shared.IsDone());
        uint64_t value = 0;
        ASSERT_TRUE(shared.TryGetResult(value))
            << "skip-path waiter returned from Wait without an observable result";
        ASSERT_EQ(value, expected);

        while (helperDone.load(std::memory_order_acquire) == 0)
        {
            YieldProcessor();
        }
        progress.fetch_add(1, std::memory_order_relaxed);
    }

    stop.store(true, std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_release);
    helper.join();

    EXPECT_EQ(payloadFailures.load(std::memory_order_relaxed), 0u)
        << "a Wait exit observed terminal status without the outcome payload";

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    pool.Shutdown();
    SUCCEED() << kIterations << " completion-window-gated Wait races without a strand";
}

// ============================================================================
// A/B benches (identical source in baseline and gated binaries)
// ============================================================================

// Submit -> IsDone ping-pong: per-iteration latency serializes the worker's
// completion tail (terminal exchange + notify fence + envelope death +
// deferred cleanup) into the next submit's pickup.
TEST(JobSystemBench, BENCHMARK_SubmitCompleteLatency)
{
    constexpr size_t kWarmup = 20'000;
    constexpr size_t kIterations = 100'000;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);
    LogTimerPeriod();

    JobSystem::WorkStealingThreadPool pool(4);

    for (size_t i = 0; i < kWarmup; ++i)
    {
        auto h = pool.Submit([] {});
        while (!h.IsDone())
        {
            YieldProcessor();
        }
    }

    std::vector<double> samples;
    samples.reserve(kIterations);

    const AmbientCpuSample cpu0 = SampleSystemCpu();
    const auto benchStart = SteadyClock::now();
    for (size_t i = 0; i < kIterations; ++i)
    {
        const auto t0 = SteadyClock::now();
        auto h = pool.Submit([] {});
        while (!h.IsDone())
        {
            YieldProcessor();
        }
        samples.push_back(ElapsedNs(t0, SteadyClock::now()));
    }
    const auto benchEnd = SteadyClock::now();
    const AmbientCpuSample cpu1 = SampleSystemCpu();

    std::sort(samples.begin(), samples.end());
    const double wallMs = std::chrono::duration<double, std::milli>(benchEnd - benchStart).count();
    std::printf("[bench] SubmitCompleteLatency: median=%.0fns p99=%.0fns p99.9=%.0fns "
                "worst=%.0fns wall=%.1fms ambientCPU=%.1f%%\n",
                Percentile(samples, 0.5), Percentile(samples, 0.99),
                Percentile(samples, 0.999), samples.back(), wallMs,
                AmbientBusyPercent(cpu0, cpu1));

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    pool.Shutdown();
}

// Worker-bound handle-task throughput: pool(1), two producer threads keep a
// bounded in-flight window full, so the single worker's per-task cost (which
// contains the completion notify fence) is the throughput ceiling.
TEST(JobSystemBench, BENCHMARK_HandleCompletionThroughput)
{
    constexpr size_t kWarmup = 20'000;
    constexpr size_t kTasks = 200'000;
    constexpr size_t kProducers = 2;
    constexpr size_t kTasksPerProducer = kTasks / kProducers;
    constexpr size_t kInFlightWindow = 4096;

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    JobSystem::WorkStealingThreadPool pool(1);
    std::atomic<size_t> executed{0};

    for (size_t i = 0; i < kWarmup; ++i)
    {
        pool.Submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
    }
    {
        const auto deadline = SteadyClock::now() + std::chrono::seconds(60);
        while (executed.load(std::memory_order_relaxed) < kWarmup &&
               SteadyClock::now() < deadline)
        {
            std::this_thread::yield();
        }
        ASSERT_EQ(executed.load(std::memory_order_relaxed), kWarmup);
    }

    executed.store(0, std::memory_order_relaxed);
    std::atomic<size_t> submitted{0};

    const AmbientCpuSample cpu0 = SampleSystemCpu();
    const auto start = SteadyClock::now();
    {
        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (size_t p = 0; p < kProducers; ++p)
        {
            producers.emplace_back([&] {
                for (size_t i = 0; i < kTasksPerProducer; ++i)
                {
                    while (submitted.load(std::memory_order_relaxed) -
                               executed.load(std::memory_order_relaxed) >
                           kInFlightWindow)
                    {
                        std::this_thread::yield();
                    }
                    submitted.fetch_add(1, std::memory_order_relaxed);
                    pool.Submit(
                        [&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
                }
            });
        }
        for (auto& t : producers)
        {
            t.join();
        }
    }
    {
        const auto deadline = SteadyClock::now() + std::chrono::seconds(120);
        while (executed.load(std::memory_order_relaxed) < kTasks &&
               SteadyClock::now() < deadline)
        {
            std::this_thread::yield();
        }
    }
    const auto end = SteadyClock::now();
    const AmbientCpuSample cpu1 = SampleSystemCpu();

    ASSERT_EQ(executed.load(std::memory_order_relaxed), kTasks);
    const double seconds = std::chrono::duration<double>(end - start).count();
    std::printf("[bench] HandleCompletionThroughput: %.0f tasks/sec worker-bound "
                "(%.0fns/task) wall=%.1fms ambientCPU=%.1f%%\n",
                static_cast<double>(kTasks) / seconds,
                seconds * 1e9 / static_cast<double>(kTasks), seconds * 1000.0,
                AmbientBusyPercent(cpu0, cpu1));

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
    pool.Shutdown();
}

} // namespace GameEngine::Tests
