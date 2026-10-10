// Slice-7 topology decision benches.
//
// Slice 7 is MEASURE-FIRST: per-worker deques (enkiTS-shaped) are
// built ONLY if the post-slice-1/4/5/6 pool still shows global-queue
// contention; otherwise the slice is naming/docs/cleanup. These benches are
// the decision input:
//
//  1. BENCHMARK_MpmcContentionProfile — N submitters x M workers saturating
//     with ~1-10us tasks: does the single global MPMC scale, and does the
//     per-publish caller cost degrade with producer count?
//  2. BENCHMARK_FeedCensus_* — what fraction of production traffic actually
//     lands in the worker-local queues vs the global lanes, per workload
//     shape (ECS parallel wave, graph chains, JobCounter waves)?
//  3. BENCHMARK_StealReality_LocalHeavyFanout — when local queues DO fill,
//     do steals succeed, and how much pred-true busy-poll does the steal
//     half-ring visibility gap cost (EmptyLoopsWithBacklog, backstop rate)?
//
// These are census probes, not gates: assertions pin only sanity (all tasks
// executed, census conservation). The decision numbers go to stdout.

#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

#include "TestPlatform.h"

namespace GameEngine::Tests {

namespace {

using JobSystem::WorkStealingThreadPool;
using SteadyClock = std::chrono::steady_clock;
using Census = JobSystem::JobSystemStatistics;

double ElapsedUs(SteadyClock::time_point t0, SteadyClock::time_point t1)
{
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

// Precise microsecond-scale task body; sleep_for cannot hit us gaps on
// Windows.
void BusyWaitUs(int us)
{
    if (us <= 0)
        return;
    const auto deadline = SteadyClock::now() + std::chrono::microseconds(us);
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

bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout)
{
    const auto deadline = SteadyClock::now() + timeout;
    while (!pred())
    {
        if (SteadyClock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

Census Delta(const Census& after, const Census& before)
{
    Census d;
    d.GlobalPushes = after.GlobalPushes - before.GlobalPushes;
    d.LocalPushes = after.LocalPushes - before.LocalPushes;
    d.GlobalPops = after.GlobalPops - before.GlobalPops;
    d.LocalPops = after.LocalPops - before.LocalPops;
    d.StealPops = after.StealPops - before.StealPops;
    d.StealMisses = after.StealMisses - before.StealMisses;
    d.EmptyLoopsWithBacklog = after.EmptyLoopsWithBacklog - before.EmptyLoopsWithBacklog;
    d.BackstopTimeouts = after.BackstopTimeouts - before.BackstopTimeouts;
    return d;
}

void PrintCensus(const char* label, const Census& d, double wallMs)
{
    const uint64_t pushes = d.GlobalPushes + d.LocalPushes;
    std::printf("[census] %s (wall %.2fms)\n", label, wallMs);
    std::printf("[census]   pushes: global=%llu (%.1f%%) local=%llu (%.1f%%)\n",
                static_cast<unsigned long long>(d.GlobalPushes),
                pushes ? 100.0 * d.GlobalPushes / pushes : 0.0,
                static_cast<unsigned long long>(d.LocalPushes),
                pushes ? 100.0 * d.LocalPushes / pushes : 0.0);
    std::printf("[census]   pops:   global=%llu local=%llu steal=%llu stealMisses=%llu\n",
                static_cast<unsigned long long>(d.GlobalPops),
                static_cast<unsigned long long>(d.LocalPops),
                static_cast<unsigned long long>(d.StealPops),
                static_cast<unsigned long long>(d.StealMisses));
    std::printf("[census]   idle:   emptyLoopsWithBacklog=%llu backstopTimeouts=%llu "
                "(%.0f timeouts/s)\n",
                static_cast<unsigned long long>(d.EmptyLoopsWithBacklog),
                static_cast<unsigned long long>(d.BackstopTimeouts),
                wallMs > 0.0 ? d.BackstopTimeouts * 1000.0 / wallMs : 0.0);
}

// Push/pop conservation: after every task has executed and before shutdown,
// each queue's pushes must equal its pops (steals count against the local
// queues). Pins the instrumentation itself.
void ExpectCensusConserved(const Census& d)
{
    EXPECT_EQ(d.GlobalPushes, d.GlobalPops);
    EXPECT_EQ(d.LocalPushes, d.LocalPops + d.StealPops);
}

void LogTimerResolution()
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
                std::printf("[timer] NtQueryTimerResolution (100ns units -> ms): "
                            "min=%.3f max=%.3f CURRENT=%.3f\n",
                            minRes / 10000.0, maxRes / 10000.0, curRes / 10000.0);
            }
        }
    }
#else
    std::printf("[timer] host timer-resolution diagnostics unavailable on this platform\n");
#endif
}

/** Pool-level graph task whose body is supplied by the bench. */
class ScriptedTask : public JobSystem::Task {
public:
    ScriptedTask(WorkStealingThreadPool* pool, std::function<void()> body)
        : Task(pool), m_Body(std::move(body)) {}

    void Execute() override
    {
        if (m_Body)
        {
            m_Body();
        }
    }

private:
    std::function<void()> m_Body;
};

// Warm the slab pool / queue blocks so census runs measure steady state.
void Warmup(WorkStealingThreadPool& pool)
{
    std::atomic<uint32_t> executed{0};
    constexpr uint32_t kWarmupTasks = 2048;
    for (uint32_t i = 0; i < kWarmupTasks; ++i)
    {
        pool.EnqueueWork([&executed] { executed.fetch_add(1, std::memory_order_relaxed); });
    }
    ASSERT_TRUE(WaitUntil([&] { return executed.load(std::memory_order_relaxed) == kWarmupTasks; },
                          std::chrono::milliseconds(5000)));
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Global-queue contention profile.
//
// N submitter threads publish `kTotalTasks / N` fire-and-forget tasks each
// (EnqueueWork -> the single global MPMC lane), with a ~1us busy body so the
// workers are genuinely saturated. Reported per cell:
//   - caller-side per-publish latency (median / p99 across every call), and
//   - end-to-end throughput (first publish -> last task executed).
// If per-publish cost stays flat as producers rise, and throughput rises
// until it hits worker capacity, the single MPMC is NOT contended in any way
// per-worker deques would fix.
// ---------------------------------------------------------------------------
TEST(TopologyBench, BENCHMARK_MpmcContentionProfile)
{
    LogTimerResolution();

    const uint32_t hwc = std::max(2u, std::thread::hardware_concurrency());
    std::printf("[mpmc] hardware_concurrency=%u\n", hwc);

    struct Cell
    {
        uint32_t Workers;
        uint32_t Producers;
        uint32_t BodyUs;
        uint32_t TotalTasks;
    };
    std::vector<Cell> cells;
    for (uint32_t workers : {8u, 16u, hwc})
    {
        for (uint32_t producers : {1u, 4u, 8u})
        {
            cells.push_back({workers, producers, 1, 240000});
        }
    }
    // Consumer-bound row: 10us bodies keep every worker busy regardless of
    // producer count; publish cost under a persistent deep backlog.
    cells.push_back({hwc, 1, 10, 96000});
    cells.push_back({hwc, 8, 10, 96000});

    for (const Cell& cell : cells)
    {
        WorkStealingThreadPool pool(cell.Workers);
        Warmup(pool);
        const Census before = pool.GetStatistics();

        std::atomic<uint32_t> executed{0};
        const uint32_t bodyUs = cell.BodyUs;
        const uint32_t perProducer = cell.TotalTasks / cell.Producers;
        const uint32_t total = perProducer * cell.Producers;

        std::vector<std::vector<double>> latencies(cell.Producers);
        std::atomic<bool> start{false};
        std::vector<std::thread> producers;
        producers.reserve(cell.Producers);
        for (uint32_t p = 0; p < cell.Producers; ++p)
        {
            latencies[p].reserve(perProducer);
            producers.emplace_back([&, p] {
                while (!start.load(std::memory_order_acquire))
                {
                    YieldProcessor();
                }
                auto& lat = latencies[p];
                for (uint32_t i = 0; i < perProducer; ++i)
                {
                    const auto t0 = SteadyClock::now();
                    pool.EnqueueWork([&executed, bodyUs] {
                        BusyWaitUs(static_cast<int>(bodyUs));
                        executed.fetch_add(1, std::memory_order_relaxed);
                    });
                    lat.push_back(ElapsedUs(t0, SteadyClock::now()));
                }
            });
        }

        const auto t0 = SteadyClock::now();
        start.store(true, std::memory_order_release);
        for (auto& t : producers)
        {
            t.join();
        }
        const auto tPublished = SteadyClock::now();
        ASSERT_TRUE(WaitUntil([&] { return executed.load(std::memory_order_relaxed) == total; },
                              std::chrono::milliseconds(30000)));
        const auto t1 = SteadyClock::now();

        std::vector<double> all;
        all.reserve(total);
        for (auto& lat : latencies)
        {
            all.insert(all.end(), lat.begin(), lat.end());
        }
        std::sort(all.begin(), all.end());

        const double publishWallUs = ElapsedUs(t0, tPublished);
        const double totalWallUs = ElapsedUs(t0, t1);
        std::printf("[mpmc] workers=%2u producers=%u body=%2uus tasks=%u | "
                    "publish/op median=%.3fus p99=%.3fus | aggregate publish %.2fM/s | "
                    "end-to-end %.2fM tasks/s\n",
                    cell.Workers, cell.Producers, cell.BodyUs, total, Percentile(all, 0.5),
                    Percentile(all, 0.99), total / publishWallUs, total / totalWallUs);

        // Idle-machinery attribution for the cell: when pool width exceeds
        // the offered load, the throughput loss should show up here (steal
        // scans, pred-true relaps, backstop wakes) — NOT as queue growth.
        const Census d = Delta(pool.GetStatistics(), before);
        std::printf("[mpmc]   idle-side: stealMisses=%llu emptyLoopsWithBacklog=%llu "
                    "backstopTimeouts=%llu\n",
                    static_cast<unsigned long long>(d.StealMisses),
                    static_cast<unsigned long long>(d.EmptyLoopsWithBacklog),
                    static_cast<unsigned long long>(d.BackstopTimeouts));
        ExpectCensusConserved(d);
    }
}

// ---------------------------------------------------------------------------
// 2a. Feed census: ECS parallel storm.
//
// The exact Query::Parallel / BatchEach shape post-slice-5: per wave, one
// JobCounter, Run(batch, counter) per batch, participating Wait. Everything
// ECS publishes should ride the global MPMC lane (Run stubs via EnqueueWork);
// the local queues should see ~zero production traffic.
// ---------------------------------------------------------------------------
TEST(TopologyBench, BENCHMARK_FeedCensus_EcsParallelStorm)
{
    WorkStealingThreadPool pool(std::max(2u, std::thread::hardware_concurrency()));
    Warmup(pool);

    constexpr uint32_t kWaves = 2000;
    constexpr uint32_t kBatchesPerWave = 64;
    std::atomic<uint64_t> executed{0};

    const Census before = pool.GetStatistics();
    const auto t0 = SteadyClock::now();
    for (uint32_t wave = 0; wave < kWaves; ++wave)
    {
        JobSystem::JobCounter counter;
        for (uint32_t b = 0; b < kBatchesPerWave; ++b)
        {
            pool.Run(
                [&executed] {
                    BusyWaitUs(1);
                    executed.fetch_add(1, std::memory_order_relaxed);
                },
                counter);
        }
        pool.Wait(counter);
    }
    const double wallMs = ElapsedUs(t0, SteadyClock::now()) / 1000.0;

    EXPECT_EQ(executed.load(), uint64_t{kWaves} * kBatchesPerWave);
    const Census d = Delta(pool.GetStatistics(), before);
    PrintCensus("ECS parallel storm (2000 waves x 64 batches, 1us bodies)", d, wallMs);
    ExpectCensusConserved(d);
}

// ---------------------------------------------------------------------------
// 2b. Feed census: graph chain storm.
//
// 10k dependent pairs. Parents are submitted from this (external) thread and
// publish into the global queue; each child parks in the graph (the 5us parent
// body guarantees the parent is still live at child registration) and is
// extracted by the WORKER that completes its parent — the extraction lands
// in that worker's LOCAL queue via PushTask's s_CurrentWorker route. This is
// the workload that actually exercises the local queues in production
// (asset-load chains, hot-reload pipelines).
// ---------------------------------------------------------------------------
TEST(TopologyBench, BENCHMARK_FeedCensus_GraphChainStorm)
{
    using JobSystem::MakeUnique;

    WorkStealingThreadPool pool(std::max(2u, std::thread::hardware_concurrency()));
    Warmup(pool);

    constexpr uint32_t kPairs = 10000;
    std::atomic<uint32_t> parentsRan{0};
    std::atomic<uint32_t> childrenRan{0};

    const Census before = pool.GetStatistics();
    const auto t0 = SteadyClock::now();
    for (uint32_t i = 0; i < kPairs; ++i)
    {
        auto parent = MakeUnique<ScriptedTask>(&pool, [&parentsRan] {
            BusyWaitUs(5);
            parentsRan.fetch_add(1, std::memory_order_relaxed);
        });
        auto child = MakeUnique<ScriptedTask>(&pool, [&childrenRan] {
            BusyWaitUs(1);
            childrenRan.fetch_add(1, std::memory_order_relaxed);
        });
        child->AddDependency(parent->GetTaskId());
        pool.Submit(std::move(parent));
        pool.Submit(std::move(child));
    }
    ASSERT_TRUE(WaitUntil([&] { return childrenRan.load(std::memory_order_relaxed) == kPairs; },
                          std::chrono::milliseconds(30000)));
    const double wallMs = ElapsedUs(t0, SteadyClock::now()) / 1000.0;

    EXPECT_EQ(parentsRan.load(), kPairs);
    const Census d = Delta(pool.GetStatistics(), before);
    PrintCensus("graph chain storm (10k dependent pairs)", d, wallMs);
    ExpectCensusConserved(d);
    std::printf("[census]   graph: children extracted on workers -> local = %.1f%% of pairs\n",
                100.0 * d.LocalPushes / kPairs);
}

// ---------------------------------------------------------------------------
// 2c. Feed census: JobCounter wave storm (the warm fork-join shape:
// Run x8 + Wait, repeated). All stub traffic -> global MPMC lane.
// ---------------------------------------------------------------------------
TEST(TopologyBench, BENCHMARK_FeedCensus_JobCounterWaveStorm)
{
    WorkStealingThreadPool pool(std::max(2u, std::thread::hardware_concurrency()));
    Warmup(pool);

    constexpr uint32_t kWaves = 10000;
    constexpr uint32_t kTasksPerWave = 8;
    std::atomic<uint64_t> executed{0};

    const Census before = pool.GetStatistics();
    const auto t0 = SteadyClock::now();
    for (uint32_t wave = 0; wave < kWaves; ++wave)
    {
        JobSystem::JobCounter counter;
        for (uint32_t i = 0; i < kTasksPerWave; ++i)
        {
            pool.Run([&executed] { executed.fetch_add(1, std::memory_order_relaxed); }, counter);
        }
        pool.Wait(counter);
    }
    const double wallMs = ElapsedUs(t0, SteadyClock::now()) / 1000.0;

    EXPECT_EQ(executed.load(), uint64_t{kWaves} * kTasksPerWave);
    const Census d = Delta(pool.GetStatistics(), before);
    PrintCensus("JobCounter wave storm (10k waves x Run(8)+Wait)", d, wallMs);
    ExpectCensusConserved(d);
}

// ---------------------------------------------------------------------------
// 3. Steal-path reality under a deliberately local-queue-heavy load.
//
// Per wave: 8 parents (each the sole dependency of 32 children) all depend on
// ONE gate task whose body BLOCKS on an atomic released by this thread only
// after the entire wave is submitted. Dependencies cannot express a held
// gate — TaskDependencyGraph::Register treats an ABSENT dependency as already
// terminal (F8) — so the gate must be submitted FIRST (present + non-terminal
// in the graph while the wave registers against it) and released LAST. That
// parks the whole wave in the graph by construction: the gate's worker
// extracts all 8 parents into its OWN local queue; each parent's completing
// worker extracts 32 children into ITS local queue (20us bodies). At most 8
// workers hold the wave's 256 children; the rest can reach them only through
// the steal half-ring. Reported:
//   - LocalPops vs StealPops: does local work spread, or do the owners drain
//     their queues alone?
//   - drain time (gate release -> wave done) vs the perfectly-balanced
//     ideal: the load-balance / starvation check.
//   - EmptyLoopsWithBacklog + backstop rate: the pred-true busy-poll cost of
//     tasks that are queued but invisible to a half-ring scan.
// The census tripwire at the bottom pins the regime itself: the original run
// of this bench submitted the gate LAST, F8 made the parents ready-at-submit
// (external publish -> global lane), and only ~17% of the wave ever parked —
// it measured the wrong regime (spec §8.3 erratum).
// ---------------------------------------------------------------------------
TEST(TopologyBench, BENCHMARK_StealReality_LocalHeavyFanout)
{
    using JobSystem::MakeUnique;

    LogTimerResolution();

    const uint32_t hwc = std::max(2u, std::thread::hardware_concurrency());
    WorkStealingThreadPool pool(hwc);
    Warmup(pool);

    constexpr uint32_t kWaves = 100;
    constexpr uint32_t kParentsPerWave = 8;
    constexpr uint32_t kChildrenPerParent = 32;
    constexpr uint32_t kChildBodyUs = 20;
    constexpr uint32_t kChildrenPerWave = kParentsPerWave * kChildrenPerParent;

    std::atomic<uint32_t> childrenRan{0};

    const Census before = pool.GetStatistics();
    double drainMs = 0.0; // sum of (gate release -> wave drained) windows
    const auto t0 = SteadyClock::now();
    for (uint32_t wave = 0; wave < kWaves; ++wave)
    {
        childrenRan.store(0, std::memory_order_relaxed);

        // Held gate: submitted FIRST (see the header comment — F8 means a
        // gate submitted after its dependents never gates anything), released
        // by this thread after the wave is fully submitted. The gate pins one
        // worker in a yield loop for the submission phase; that is the price
        // of a genuinely parked wave.
        std::atomic<bool> release{false};
        auto gate = MakeUnique<ScriptedTask>(&pool, [&release] {
            while (!release.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        });
        const JobSystem::TaskId gateId = gate->GetTaskId();
        pool.Submit(std::move(gate));

        for (uint32_t p = 0; p < kParentsPerWave; ++p)
        {
            auto parent = MakeUnique<ScriptedTask>(&pool, [] { BusyWaitUs(20); });
            const JobSystem::TaskId parentId = parent->GetTaskId();
            parent->AddDependency(gateId);
            pool.Submit(std::move(parent)); // parks until the gate completes
            for (uint32_t c = 0; c < kChildrenPerParent; ++c)
            {
                auto child = MakeUnique<ScriptedTask>(&pool, [&childrenRan] {
                    BusyWaitUs(static_cast<int>(kChildBodyUs));
                    childrenRan.fetch_add(1, std::memory_order_relaxed);
                });
                child->AddDependency(parentId);
                pool.Submit(std::move(child)); // parks until its parent completes
            }
        }

        const auto tRelease = SteadyClock::now();
        release.store(true, std::memory_order_release); // the whole wave is parked; open the gate
        ASSERT_TRUE(WaitUntil(
            [&] { return childrenRan.load(std::memory_order_relaxed) == kChildrenPerWave; },
            std::chrono::milliseconds(10000)));
        drainMs += ElapsedUs(tRelease, SteadyClock::now()) / 1000.0;
    }
    const double wallMs = ElapsedUs(t0, SteadyClock::now()) / 1000.0;

    const Census d = Delta(pool.GetStatistics(), before);
    PrintCensus("steal reality (100 waves x 8 parents x 32 local children, 20us bodies, held gate)",
                d, wallMs);
    ExpectCensusConserved(d);

    // Regime tripwire: with the gate held across submission, every parent and
    // child parks in the graph and is later extracted by a WORKER — exactly
    // kParentsPerWave + kChildrenPerWave local pushes per wave (the gates
    // themselves are the only external publishes). If the local-push census
    // strays from that constructed expectation, this bench is no longer
    // measuring the local-heavy regime and its numbers are invalid — fail
    // loud instead of silently re-measuring the F8 failure shape.
    const uint64_t expectedLocal = uint64_t{kWaves} * (kParentsPerWave + kChildrenPerWave);
    EXPECT_GE(d.LocalPushes, expectedLocal - expectedLocal / 10);
    EXPECT_LE(d.LocalPushes, expectedLocal + expectedLocal / 10);

    const double totalChildWorkMs = double{kWaves} * kChildrenPerWave * kChildBodyUs / 1000.0;
    const double idealBalancedMs = totalChildWorkMs / hwc;
    const double ownersOnlyMs = totalChildWorkMs / kParentsPerWave;
    std::printf("[steal] child work total=%.1fms | drain=%.1fms (wall %.1fms incl. submission) | "
                "ideal(%u workers)=%.1fms | owners-only(%u workers)=%.1fms\n",
                totalChildWorkMs, drainMs, wallMs, hwc, idealBalancedMs, kParentsPerWave,
                ownersOnlyMs);
    std::printf("[steal] local tasks (parents+children): stolen=%.1f%% owner-consumed=%.1f%%\n",
                100.0 * d.StealPops / (d.LocalPops + d.StealPops),
                100.0 * d.LocalPops / (d.LocalPops + d.StealPops));
}

} // namespace GameEngine::Tests
