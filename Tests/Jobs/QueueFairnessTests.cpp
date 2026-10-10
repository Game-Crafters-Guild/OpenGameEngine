// Global-lane consumer fairness pins (issue #360).
//
// moodycamel's TOKENLESS try_dequeue scores only the first THREE non-empty
// producer sub-queues (newest-first), dequeues from the BIGGEST, and
// full-scans only when that dequeue fails. Under a sustained flood from a
// few producers, the flood's deep sub-queues stay non-empty, their dequeues
// keep succeeding, and a low-rate producer's small sub-queue is never tried:
// its tasks sit for the flood's whole lifetime (measured on the pre-fix
// pool: every sustained-flood regime of BackpressureRetuneBench starved
// Query::Parallel waves outright — waves completed exactly at flood drain).
//
// The fix is per-worker moodycamel::ConsumerTokens on both global lanes
// (Worker::GlobalConsumerToken / BackgroundConsumerToken): token consumers
// stick to one producer sub-queue, force a global rotation every 256
// consumed items, and walk the ENTIRE producer ring when their current
// sub-queue is empty — every producer's sub-queue is reached within a
// bounded number of rotations regardless of flood depth.
//
// These tests pin that bound. The victim latency expectation under a
// 2000-deep 20us flood is single-digit-to-tens-of-ms (rotation quota x ring
// size x body length); the asserted bound is >100x that, yet decisively
// below the starved fingerprint (the pre-fix pool holds the victim for the
// flood's WHOLE lifetime — here that would be the watchdog's un-wedge,
// >= kFloodWatchdog). Generous on purpose: a loaded CI box must not flake
// this, only a regressed consumer can.

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

using namespace JobSystem;

namespace GameEngine::Tests {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr size_t kWorkers = 8;
constexpr int kFloodBodyUs = 20;
constexpr size_t kFloodTarget = 2000; // deep sustained backlog (the
                                      // BackpressureRetuneBench "saturated"
                                      // shape at half depth — starved the
                                      // pre-fix pool in every run)
// Un-wedge rescue so a REGRESSED pool fails the elapsed assert instead of
// hanging the suite: the flood hard-stops after this long no matter what.
constexpr auto kFloodWatchdog = std::chrono::seconds(20);
// Absolute floor of the per-wave bound; the quiet-basis multiple below can
// only raise it. See the file header for why this is generous, not tight.
constexpr double kBoundFloorMs = 1000.0;
constexpr double kQuietMultiple = 500.0;

double ElapsedMs(SteadyClock::time_point t0, SteadyClock::time_point t1)
{
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void BusyWaitUs(int us)
{
    const auto deadline = SteadyClock::now() + std::chrono::microseconds(us);
    while (SteadyClock::now() < deadline)
    {
        YieldProcessor();
    }
}

double Median(std::vector<double>& v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

// Sustained flood from a few dedicated producer threads (the streaming /
// asset-scan shape): top up the lane whenever the occupancy signal drops
// below target, hard-stopped by the watchdog. Two producers = two deep
// moodycamel sub-queues, exactly the #360 regime.
class SustainedFlood
{
  public:
    SustainedFlood(WorkStealingThreadPool& pool, JobPriority priority,
                   std::function<size_t()> occupancy)
        : m_Pool(pool)
    {
        constexpr int kProducers = 2;
        for (int t = 0; t < kProducers; ++t)
        {
            m_Threads.emplace_back([this, priority, occupancy] {
                const auto hardStop = SteadyClock::now() + kFloodWatchdog;
                while (!m_Stop.load(std::memory_order_acquire))
                {
                    if (SteadyClock::now() >= hardStop)
                    {
                        m_WatchdogFired.store(true, std::memory_order_release);
                        break;
                    }
                    if (occupancy() < kFloodTarget)
                    {
                        for (size_t i = 0; i < 64; ++i)
                        {
                            m_Pool.EnqueueWork([] { BusyWaitUs(kFloodBodyUs); }, priority);
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
        for (auto& t : m_Threads)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
    }

    bool WatchdogFired() const { return m_WatchdogFired.load(std::memory_order_acquire); }

    ~SustainedFlood() { Stop(); }

  private:
    WorkStealingThreadPool& m_Pool;
    std::atomic<bool> m_Stop{false};
    std::atomic<bool> m_WatchdogFired{false};
    std::vector<std::thread> m_Threads;
};

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

} // namespace

// #360 pin, Normal lane: an external caller's fork-join wave (the ECS
// Query::Parallel shape — JobCounter stubs from ONE low-rate sub-queue,
// caller parked on the counter) must complete DURING a sustained deep flood
// from other producers, in bounded time — not at flood drain.
TEST(QueueFairnessTest, NormalWaveCompletesDuringSustainedFlood)
{
    WorkStealingThreadPool pool(kWorkers);

    constexpr int kWaveTasks = 16;
    auto runWaveMs = [&pool]() -> double {
        const auto t0 = SteadyClock::now();
        JobCounter counter;
        for (int i = 0; i < kWaveTasks; ++i)
        {
            pool.Run([] { BusyWaitUs(kFloodBodyUs); }, counter);
        }
        pool.Wait(counter);
        return ElapsedMs(t0, SteadyClock::now());
    };

    // Quiet-pool basis (also warms slabs/queue blocks).
    std::vector<double> quiet;
    for (int i = 0; i < 9; ++i)
    {
        quiet.push_back(runWaveMs());
    }
    const double quietMedianMs = Median(quiet);
    const double boundMs = std::max(kBoundFloorMs, kQuietMultiple * quietMedianMs);

    SustainedFlood flood(pool, JobPriority::Normal,
                         [&pool] { return pool.GetApproximateQueueSize(); });
    ASSERT_TRUE(WaitUntil([&pool] { return pool.GetApproximateQueueSize() >= kFloodTarget * 3 / 4; },
                          std::chrono::milliseconds(5000)))
        << "flood never established — rig problem, not a fairness result";

    // Preconditions frame the measurement, they are not the pin: a wave
    // counts only if the flood was deep before it AND still deep after it —
    // otherwise it merely rode a drain gap and proves nothing. On a loaded
    // box (parallel -j12 build) the two flood producers can be descheduled
    // long enough for the workers to eat the backlog, so a shallow flood
    // VOIDS the measurement instead of failing the test. A regressed pool
    // still fails decisively: its first wave blocks until the watchdog kills
    // the flood, and the watchdog asserts below are hard failures.
    constexpr int kFloodWavesTarget = 5;
    constexpr int kFloodWaveAttempts = 15;
    int measured = 0;
    for (int attempt = 0; attempt < kFloodWaveAttempts && measured < kFloodWavesTarget; ++attempt)
    {
        ASSERT_FALSE(flood.WatchdogFired())
            << "a wave outlived the flood watchdog — the starved fingerprint";
        if (!WaitUntil([&pool] { return pool.GetApproximateQueueSize() >= kFloodTarget / 2; },
                       std::chrono::milliseconds(2000)))
        {
            continue; // flood momentarily shallow (loaded box) — not measurable
        }
        const double ms = runWaveMs();
        ASSERT_FALSE(flood.WatchdogFired())
            << "a wave outlived the flood watchdog — the starved fingerprint";
        if (pool.GetApproximateQueueSize() < kFloodTarget / 4)
        {
            continue; // flood drained during the wave — measurement void
        }
        EXPECT_LT(ms, boundMs) << "wave " << measured
                               << " starved under sustained flood (#360): quiet median "
                               << quietMedianMs << "ms, bound " << boundMs << "ms";
        std::printf("[fairness] normal wave %d under flood: %.2fms (quiet median %.2fms, "
                    "bound %.0fms)\n",
                    measured, ms, quietMedianMs, boundMs);
        ++measured;
    }
    ASSERT_GE(measured, 1) << "no wave could be measured with the flood deep — rig/load "
                              "problem, not a fairness result";

    flood.Stop();
    pool.Shutdown();
}

// #360 pin, Background lane: the hole is producer-count-driven, not
// priority-driven — a deep background flood (streaming is this lane's
// steady-state consumer) must not starve another producer's occasional
// background task. Normal lane stays empty so every worker scan falls
// through to the background arm.
TEST(QueueFairnessTest, BackgroundTaskCompletesDuringBackgroundFlood)
{
    WorkStealingThreadPool pool(kWorkers);

    auto runProbeMs = [&pool]() -> double {
        const auto t0 = SteadyClock::now();
        TaskHandle handle =
            pool.Submit([] { BusyWaitUs(kFloodBodyUs); }, JobPriority::Background);
        handle.Wait();
        return ElapsedMs(t0, SteadyClock::now());
    };

    std::vector<double> quiet;
    for (int i = 0; i < 9; ++i)
    {
        quiet.push_back(runProbeMs());
    }
    const double quietMedianMs = Median(quiet);
    const double boundMs = std::max(kBoundFloorMs, kQuietMultiple * quietMedianMs);

    SustainedFlood flood(pool, JobPriority::Background,
                         [&pool] { return pool.GetBackgroundQueuedForTests(); });
    ASSERT_TRUE(WaitUntil(
                    [&pool] { return pool.GetBackgroundQueuedForTests() >= kFloodTarget * 3 / 4; },
                    std::chrono::milliseconds(5000)))
        << "background flood never established — rig problem, not a fairness result";

    // Same void-not-fail precondition discipline as the Normal-lane test
    // (see there): shallow-flood probes are unmeasurable on a loaded box,
    // never a failure; the watchdog asserts keep the regressed-pool teeth.
    constexpr int kProbesTarget = 5;
    constexpr int kProbeAttempts = 15;
    int measured = 0;
    for (int attempt = 0; attempt < kProbeAttempts && measured < kProbesTarget; ++attempt)
    {
        ASSERT_FALSE(flood.WatchdogFired())
            << "a probe outlived the flood watchdog — the starved fingerprint";
        if (!WaitUntil(
                [&pool] { return pool.GetBackgroundQueuedForTests() >= kFloodTarget / 2; },
                std::chrono::milliseconds(2000)))
        {
            continue; // flood momentarily shallow (loaded box) — not measurable
        }
        const double ms = runProbeMs();
        ASSERT_FALSE(flood.WatchdogFired())
            << "a probe outlived the flood watchdog — the starved fingerprint";
        if (pool.GetBackgroundQueuedForTests() < kFloodTarget / 4)
        {
            continue; // flood drained during the probe — measurement void
        }
        EXPECT_LT(ms, boundMs) << "probe " << measured
                               << " starved under background flood (#360): quiet median "
                               << quietMedianMs << "ms, bound " << boundMs << "ms";
        std::printf("[fairness] background probe %d under flood: %.2fms (quiet median %.2fms, "
                    "bound %.0fms)\n",
                    measured, ms, quietMedianMs, boundMs);
        ++measured;
    }
    ASSERT_GE(measured, 1) << "no probe could be measured with the flood deep — rig/load "
                              "problem, not a fairness result";

    flood.Stop();
    pool.Shutdown();
}

// #360 conservation pin: switching the workers' consumption path from the
// tokenless heuristic to per-worker ConsumerTokens must not change WHAT is
// consumed, only the ORDER — every published task still executes exactly
// once. Finite flood (so the test is a strict count, not a latency bound):
// four Normal-lane producer threads, a background producer, and JobCounter
// waves from this thread all interleave; every task owns a distinct slot and
// flags double-execution via exchange.
TEST(QueueFairnessTest, FloodPlusWavesConserveEveryTaskExactlyOnce)
{
    WorkStealingThreadPool pool(kWorkers);

    constexpr size_t kFloodProducers = 4;
    constexpr size_t kFloodPerProducer = 25000;
    constexpr size_t kFloodTasks = kFloodProducers * kFloodPerProducer;
    constexpr size_t kWaveCount = 32;
    constexpr size_t kWaveWidth = 16;
    constexpr size_t kWaveTasks = kWaveCount * kWaveWidth;
    constexpr size_t kBackgroundTasks = 4096;
    constexpr size_t kTotal = kFloodTasks + kWaveTasks + kBackgroundTasks;

    std::vector<std::atomic<uint8_t>> executed(kTotal);
    std::atomic<size_t> doubleRuns{0};
    std::atomic<size_t> ran{0};
    auto body = [&executed, &doubleRuns, &ran](size_t slot) {
        if (executed[slot].exchange(1, std::memory_order_acq_rel) != 0)
        {
            doubleRuns.fetch_add(1, std::memory_order_relaxed);
        }
        ran.fetch_add(1, std::memory_order_release);
    };

    std::vector<std::thread> producers;
    for (size_t p = 0; p < kFloodProducers; ++p)
    {
        producers.emplace_back([&pool, &body, p] {
            const size_t base = p * kFloodPerProducer;
            for (size_t i = 0; i < kFloodPerProducer; ++i)
            {
                pool.EnqueueWork([&body, slot = base + i] { body(slot); });
            }
        });
    }
    std::thread backgroundProducer([&pool, &body] {
        const size_t base = kFloodTasks + kWaveTasks;
        for (size_t i = 0; i < kBackgroundTasks; ++i)
        {
            pool.EnqueueWork([&body, slot = base + i] { body(slot); },
                             JobPriority::Background);
        }
    });

    // Waves from this thread while the producers publish: the caller parks
    // per wave (F17 external waiter), so each Wait's return proves the wave's
    // stubs were consumed out of a lane the flood was actively deepening.
    for (size_t w = 0; w < kWaveCount; ++w)
    {
        JobCounter counter;
        const size_t base = kFloodTasks + w * kWaveWidth;
        for (size_t i = 0; i < kWaveWidth; ++i)
        {
            pool.Run([&body, slot = base + i] { body(slot); }, counter);
        }
        pool.Wait(counter);
    }

    for (auto& t : producers)
    {
        t.join();
    }
    backgroundProducer.join();

    ASSERT_TRUE(WaitUntil([&ran] { return ran.load(std::memory_order_acquire) >= kTotal; },
                          std::chrono::milliseconds(60000)))
        << "conservation: only " << ran.load() << "/" << kTotal << " tasks ran";

    EXPECT_EQ(ran.load(std::memory_order_acquire), kTotal);
    EXPECT_EQ(doubleRuns.load(std::memory_order_relaxed), size_t{0});
    size_t setCount = 0;
    for (auto& flag : executed)
    {
        setCount += flag.load(std::memory_order_relaxed);
    }
    EXPECT_EQ(setCount, kTotal);

    pool.Shutdown();
}

} // namespace GameEngine::Tests
