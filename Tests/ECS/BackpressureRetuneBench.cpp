// ECS backpressure re-tune bench (JobSystem overhaul follow-up).
//
// The QueryPolicy backpressure constants (QueuePressureFactor=8,
// PendingPressureFactor=16, PressuredMinBatchFloor=4096) were tuned when
// GetApproximateQueueSize / GetPendingTasksApprox LIED — they included
// phantom counts (pre-slice-1) and the Background backlog (pre-#327). Both
// signals are honest now (Normal-lane, m_QueuedTasks-derived), so this bench
// measures whether backpressure engagement helps or hurts Query::Parallel
// wall time against REAL Normal-lane contention regimes, and whether the
// 1000 -> 4096 coarsening floor is the right response.
//
// Contention rig: two dedicated flood-producer threads (the asset-scan /
// streaming shape) top up the Normal lane with 20us tasks whenever the
// queued count drops below the regime target. Regimes (8-worker pool; the
// engagement thresholds are queued > 64 = workers x QueuePressureFactor,
// pending > 128 = workers x PendingPressureFactor):
//   none      target=0     (quiet — false-engagement sanity + basis)
//   light     target=32    (below both thresholds — must NOT engage)
//   engaged   target=200   (above — the marginal-engagement question)
//   saturated target=4000  (deep sustained flood)
//
// Call shapes (both real):
//   external  Query::Parallel from a persistent non-worker caller thread —
//             the F17 external waiter PARKS on the counter; wave stubs ride
//             the caller's moodycamel implicit-producer sub-queue.
//   worker    Query::Parallel from inside a pool job (the Scheduler::
//             MakeJobParallel production shape) — the F17 participating
//             waiter executes same-counter envelopes from the counter's
//             tagged queue directly.
//
// STARVATION PROBE: every measured call runs under a deadline. The global
// MPMC's tokenless try_dequeue scans only the first THREE non-empty
// producer sub-queues (newest-first) and dequeues from the biggest,
// falling through to a full scan only when that dequeue fails
// (concurrentqueue.h try_dequeue) — so a low-rate submitter's small
// sub-queue can be starved for as long as deeper flood sub-queues stay
// non-empty. When a call exceeds the deadline the bench STOPS the flood
// (which un-wedges the wave), marks the whole regime/shape block STARVED,
// and moves on: no ECS-side batch constant can influence a wave the
// scheduler never dequeues.
//
// Config sweep is interleaved round-robin (rep-major) per the measurement
// law: box drift lands on every config equally; ambient whole-box CPU is
// logged per cell block. Full matrix runs only with
// GE_BACKPRESSURE_BENCH_FULL=1 (~2-4 min); the default in-suite run is a
// bounded smoke that pins correctness (counts exact under flood) without
// gating on wall time.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "TestComponents.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace GameEngine::ECS;
using namespace GameEngine::ECS::test;

namespace {

using SteadyClock = std::chrono::steady_clock;

void BusyWaitUs(int64_t us)
{
    const auto deadline = SteadyClock::now() + std::chrono::microseconds(us);
    while (SteadyClock::now() < deadline)
    {
#ifdef _WIN32
        YieldProcessor();
#else
        std::this_thread::yield();
#endif
    }
}

double AmbientBusyPercentSince(uint64_t& idlePrev, uint64_t& kernelPrev, uint64_t& userPrev)
{
#ifdef _WIN32
    FILETIME idleFt{}, kernelFt{}, userFt{};
    GetSystemTimes(&idleFt, &kernelFt, &userFt);
    auto toU64 = [](const FILETIME& ft) {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<uint64_t>(u.QuadPart);
    };
    const uint64_t idle = toU64(idleFt), kernel = toU64(kernelFt), user = toU64(userFt);
    const double di = static_cast<double>(idle - idlePrev);
    const double dk = static_cast<double>(kernel - kernelPrev);
    const double du = static_cast<double>(user - userPrev);
    idlePrev = idle;
    kernelPrev = kernel;
    userPrev = user;
    const double total = dk + du;
    return total > 0.0 ? 100.0 * (total - di) / total : 0.0;
#else
    (void)idlePrev;
    (void)kernelPrev;
    (void)userPrev;
    return -1.0;
#endif
}

// Two producer threads hold the Normal lane at ~target queued 20us tasks —
// the streaming/scan flood shape (rate-limited top-up, no worker
// re-enqueue).
class NormalLaneFlood
{
  public:
    NormalLaneFlood(JobSystem::WorkStealingThreadPool& pool, size_t target)
        : m_Pool(pool), m_Target(target)
    {
        constexpr int kProducers = 2;
        for (int t = 0; t < kProducers; ++t)
        {
            m_Threads.emplace_back([this] {
                while (!m_Stop.load(std::memory_order_acquire))
                {
                    const size_t queued = m_Pool.GetApproximateQueueSize();
                    if (queued < m_Target)
                    {
                        const size_t burst = std::min<size_t>(64, m_Target - queued);
                        for (size_t i = 0; i < burst; ++i)
                        {
                            m_Pool.EnqueueWork([] { BusyWaitUs(20); });
                        }
                    }
                    else
                    {
                        std::this_thread::yield();
                    }
                }
            });
        }
    }

    void Stop()
    {
        m_Stop.store(true, std::memory_order_release);
    }

    ~NormalLaneFlood()
    {
        Stop();
        for (auto& t : m_Threads)
        {
            t.join();
        }
        // Best-effort drain so the next block starts quiet.
        const auto deadline = SteadyClock::now() + std::chrono::seconds(30);
        while (m_Pool.GetPendingTasksApprox() != 0 && SteadyClock::now() < deadline)
        {
            std::this_thread::yield();
        }
    }

  private:
    JobSystem::WorkStealingThreadPool& m_Pool;
    const size_t m_Target;
    std::atomic<bool> m_Stop{false};
    std::vector<std::thread> m_Threads;
};

void Populate(World& w, int n)
{
    for (int i = 0; i < n; ++i)
    {
        auto e = w.Create();
        e.Set(Position{static_cast<float>(i), 0.0f, 0.0f});
        e.Set(Velocity{1.0f, 2.0f, 3.0f});
    }
    w.ProcessCommands();
}

struct PolicyConfig
{
    const char* Name;
    QueryPolicy Policy;
};

std::vector<PolicyConfig> MakeConfigs()
{
    std::vector<PolicyConfig> configs;
    {
        QueryPolicy p; // defaults
        p.EnableBackpressure = false;
        configs.push_back({"off", p});
    }
    {
        QueryPolicy p;
        p.PressuredMinBatchFloor = 2048;
        configs.push_back({"floor2048", p});
    }
    {
        QueryPolicy p; // the pre-retune floor (was the shipping default)
        p.PressuredMinBatchFloor = 4096;
        configs.push_back({"floor4096", p});
    }
    {
        QueryPolicy p;
        p.PressuredMinBatchFloor = 8192;
        configs.push_back({"floor8192", p});
    }
    {
        QueryPolicy p;
        p.PressuredMinBatchFloor = 32768;
        configs.push_back({"floor32768", p});
    }
    return configs;
}

double Median(std::vector<double>& v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double P99(std::vector<double>& v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(0.99 * static_cast<double>(v.size() - 1) + 0.5)];
}

// One Parallel call; returns wall us and validates the count.
double OneParallelCall(World& w, JobSystem::WorkStealingThreadPool& js, int entityCount,
                       bool workerShape)
{
    std::atomic<size_t> cnt{0};
    const auto t0 = SteadyClock::now();
    if (workerShape)
    {
        JobSystem::JobCounter outer;
        js.Run(
            [&] {
                w.Query<Write<Position>, Read<Velocity>>().Parallel(
                    [&cnt](EntityHandle, Position& p, const Velocity& v) {
                        p.x += v.x * 0.016f;
                        cnt.fetch_add(1, std::memory_order_relaxed);
                    },
                    0); // 0 -> policy default (1000) or the pressured floor
            },
            outer);
        js.Wait(outer);
    }
    else
    {
        w.Query<Write<Position>, Read<Velocity>>().Parallel(
            [&cnt](EntityHandle, Position& p, const Velocity& v) {
                p.x += v.x * 0.016f;
                cnt.fetch_add(1, std::memory_order_relaxed);
            },
            0);
    }
    const double us = std::chrono::duration<double, std::micro>(SteadyClock::now() - t0).count();
    EXPECT_EQ(cnt.load(std::memory_order_relaxed), static_cast<size_t>(entityCount));
    return us;
}

// Persistent caller thread: all measured calls run here, so the wave's
// stubs ride ONE stable implicit-producer sub-queue (the production main-
// thread shape) instead of a fresh sub-queue per call.
class CallerThread
{
  public:
    CallerThread()
    {
        m_Thread = std::thread([this] {
            uint64_t seen = 0;
            for (;;)
            {
                while (m_Generation.load(std::memory_order_acquire) == seen)
                {
                    if (m_Quit.load(std::memory_order_relaxed))
                    {
                        return;
                    }
                    std::this_thread::yield();
                }
                seen = m_Generation.load(std::memory_order_acquire);
                m_ResultUs = m_Call();
                m_Done.store(true, std::memory_order_release);
            }
        });
    }

    ~CallerThread()
    {
        m_Quit.store(true, std::memory_order_relaxed);
        m_Thread.join();
    }

    // Runs `call` on the caller thread; returns {us, starved}. On deadline
    // expiry, `unwedge` is invoked (stops the flood) and the call is then
    // allowed to finish — a starved wave completes once the flood ebbs.
    struct Result
    {
        double Us;
        bool Starved;
    };
    Result Run(std::function<double()> call, const std::function<void()>& unwedge,
               std::chrono::seconds deadline)
    {
        m_Call = std::move(call);
        m_Done.store(false, std::memory_order_relaxed);
        m_Generation.fetch_add(1, std::memory_order_release);

        const auto hardDeadline = SteadyClock::now() + deadline;
        while (!m_Done.load(std::memory_order_acquire))
        {
            if (SteadyClock::now() >= hardDeadline)
            {
                unwedge();
                const auto drainDeadline = SteadyClock::now() + std::chrono::seconds(60);
                while (!m_Done.load(std::memory_order_acquire))
                {
                    if (SteadyClock::now() >= drainDeadline)
                    {
                        std::fprintf(stderr, "[bpbench] starved wave failed to complete even "
                                             "after the flood stopped — aborting\n");
                        std::fflush(stderr);
                        std::abort();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return {m_ResultUs, true};
            }
            std::this_thread::yield();
        }
        return {m_ResultUs, false};
    }

  private:
    std::thread m_Thread;
    std::function<double()> m_Call;
    std::atomic<uint64_t> m_Generation{0};
    std::atomic<bool> m_Done{false};
    std::atomic<bool> m_Quit{false};
    double m_ResultUs = 0.0;
};

void RunMatrix(bool fullMatrix)
{
    constexpr size_t kWorkers = 8;
    const auto kCallDeadline = std::chrono::seconds(3);

    Logger::Log::SetLogLevel(Logger::LogLevel::Error);

    struct Regime
    {
        const char* Name;
        size_t Target;       // flood-held Normal backlog (0 = no flood)
        int StormThreads;    // concurrent UNMEASURED Parallel-wave threads
    };
    std::vector<Regime> regimes;
    std::vector<int> sizes;
    std::vector<bool> shapes; // false = external, true = worker
    int reps, iters;
    if (fullMatrix)
    {
        // wavestorm: pressure from OTHER ECS waves (the self-pressure shape
        // the constants can actually reach — storm waves drain fully between
        // submissions, so sub-queues ebb and nothing starves).
        regimes = {{"none", 0, 0},
                   {"light", 32, 0},
                   {"engaged", 200, 0},
                   {"saturated", 4000, 0},
                   {"wavestorm", 0, 3}};
        sizes = {10'000, 100'000};
        shapes = {false, true};
        reps = 5;
        iters = 6;
    }
    else
    {
        // Bounded smoke: correctness + starvation-probe liveness, small matrix.
        regimes = {{"none", 0, 0}, {"engaged", 200, 0}, {"wavestorm", 0, 2}};
        sizes = {10'000};
        shapes = {false};
        reps = 2;
        iters = 3;
    }

    const auto configs = MakeConfigs();

    for (int entityCount : sizes)
    {
        JobSystem::WorkStealingThreadPool js(kWorkers);
        World w(&js);
        Populate(w, entityCount);
        CallerThread caller;

        for (bool workerShape : shapes)
        {
            for (const Regime& regime : regimes)
            {
                std::unique_ptr<NormalLaneFlood> flood;
                if (regime.Target > 0)
                {
                    flood = std::make_unique<NormalLaneFlood>(js, regime.Target);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                auto unwedge = [&flood] {
                    if (flood)
                    {
                        flood->Stop();
                    }
                };

                // Wave-storm threads: unmeasured competing Parallel waves on
                // the same World/pool (they see the same per-World policy the
                // measured calls sweep — every wave in a process does).
                std::atomic<bool> stormStop{false};
                std::vector<std::thread> storm;
                for (int t = 0; t < regime.StormThreads; ++t)
                {
                    storm.emplace_back([&] {
                        while (!stormStop.load(std::memory_order_acquire))
                        {
                            OneParallelCall(w, js, entityCount, /*workerShape=*/false);
                        }
                    });
                }

                struct CellStats
                {
                    std::vector<double> SamplesUs;
                    size_t EngagedCalls = 0;
                    size_t Calls = 0;
                    size_t QueueSum = 0;
                };
                std::vector<CellStats> stats(configs.size());
                bool blockStarved = false;
                double starvedCallUs = 0.0;

                uint64_t idlePrev = 0, kernelPrev = 0, userPrev = 0;
                AmbientBusyPercentSince(idlePrev, kernelPrev, userPrev); // prime

                // Interleaved config sweep: rep-major round-robin so box
                // drift lands on every config equally.
                for (int rep = 0; rep < reps && !blockStarved; ++rep)
                {
                    for (size_t c = 0; c < configs.size() && !blockStarved; ++c)
                    {
                        w.SetQueryPolicy(configs[c].Policy);
                        for (int i = 0; i < iters + 1; ++i) // first is warmup
                        {
                            const size_t q = js.GetApproximateQueueSize();
                            const size_t pending = js.GetPendingTasksApprox();
                            const QueryPolicy& p = configs[c].Policy;
                            const bool engaged =
                                p.EnableBackpressure &&
                                (q > kWorkers * p.QueuePressureFactor ||
                                 pending > kWorkers * p.PendingPressureFactor);

                            const CallerThread::Result r = caller.Run(
                                [&] {
                                    return OneParallelCall(w, js, entityCount, workerShape);
                                },
                                unwedge, kCallDeadline);
                            if (r.Starved)
                            {
                                blockStarved = true;
                                starvedCallUs = r.Us;
                                break;
                            }
                            if (i == 0)
                            {
                                continue; // warmup not recorded
                            }
                            stats[c].SamplesUs.push_back(r.Us);
                            stats[c].EngagedCalls += engaged ? 1 : 0;
                            stats[c].QueueSum += q;
                            stats[c].Calls += 1;
                        }
                    }
                }

                stormStop.store(true, std::memory_order_release);
                for (auto& t : storm)
                {
                    t.join();
                }

                const double ambient =
                    AmbientBusyPercentSince(idlePrev, kernelPrev, userPrev);

                if (blockStarved)
                {
                    std::printf("[bpbench] size=%d shape=%s regime=%-9s STARVED: wave held "
                                ">%llds by the flood (completed %.1fms after flood stop) — "
                                "no batch constant is reachable in this regime\n",
                                entityCount, workerShape ? "worker" : "external", regime.Name,
                                static_cast<long long>(kCallDeadline.count()),
                                starvedCallUs / 1000.0);
                }
                else
                {
                    for (size_t c = 0; c < configs.size(); ++c)
                    {
                        CellStats& s = stats[c];
                        std::printf("[bpbench] size=%d shape=%s regime=%-9s config=%-10s "
                                    "median=%9.1fus p99=%9.1fus engaged=%zu/%zu avgQ=%zu "
                                    "ambient=%.1f%%\n",
                                    entityCount, workerShape ? "worker" : "external",
                                    regime.Name, configs[c].Name, Median(s.SamplesUs),
                                    P99(s.SamplesUs), s.EngagedCalls, s.Calls,
                                    s.Calls ? s.QueueSum / s.Calls : 0, ambient);
                    }
                }
                std::fflush(stdout);
            }
        }
        js.Shutdown();
    }

    Logger::Log::SetLogLevel(Logger::LogLevel::Debug);
}

} // namespace

// Default in-suite run: bounded smoke (correctness under flood; the full
// matrix is opt-in via GE_BACKPRESSURE_BENCH_FULL=1 — it is a measurement
// tool, not a wall-time gate; see the file header).
TEST(BackpressureRetuneBench, BENCHMARK_ParallelUnderNormalLaneContention)
{
    const char* full = std::getenv("GE_BACKPRESSURE_BENCH_FULL");
    RunMatrix(full != nullptr && full[0] == '1');
}
