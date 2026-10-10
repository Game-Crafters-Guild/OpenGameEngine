// Stress + invariant tests for CompileConcurrencyGate: the admission gate must
// never let more than `cap` compile tasks run concurrently, must drain every
// submitted task (Background lane before Normal), must never run work inline on
// the submitting thread, and must not leak/strand work when a dispatch throws.

#include "Engine/Rendering/CompileConcurrencyGate.h"

#include "JobSystem/TaskTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using GameEngine::Engine::Renderer::CompileConcurrencyGate;
using GameEngine::Engine::Renderer::ComputeShaderCompileConcurrencyCap;
using JobSystem::JobPriority;

namespace
{
// A shared_ptr whose deleter bumps `counter` once, mirroring the production
// prewarm-counter guard: captured into a submitted closure, it fires exactly
// once when the last copy of that closure is destroyed — run, gate-dropped, or
// pool-dropped. Asserting `counter == submitted` proves no accounting leak.
std::shared_ptr<void> MakeDestroyGuard(std::atomic<int>& counter)
{
    return std::shared_ptr<void>(nullptr, [&counter](void*) { counter.fetch_add(1); });
}

// Real worker pool with more workers than any gate cap under test, so the gate
// (not the pool) is the binding constraint. Pump-spawned tasks re-enter Dispatch
// and drain like any other.
class PoolExecutor
{
  public:
    explicit PoolExecutor(int workerCount)
    {
        for (int i = 0; i < workerCount; ++i)
            m_Workers.emplace_back([this] { WorkerLoop(); });
    }

    ~PoolExecutor()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Stop = true;
        }
        m_Cv.notify_all();
        for (auto& t : m_Workers)
            if (t.joinable())
                t.join();
    }

    void Dispatch(std::function<void()> work, JobPriority)
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Queue.push_back(std::move(work));
        }
        m_Cv.notify_one();
    }

    bool WaitForCompleted(int expected, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_DoneCv.wait_until(lock, deadline,
                                   [this, expected] { return m_Completed >= expected; });
    }

  private:
    void WorkerLoop()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Cv.wait(lock, [this] { return m_Stop || !m_Queue.empty(); });
                if (m_Stop && m_Queue.empty())
                    return;
                job = std::move(m_Queue.front());
                m_Queue.pop_front();
            }
            job();
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                ++m_Completed;
            }
            m_DoneCv.notify_all();
        }
    }

    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::condition_variable m_DoneCv;
    std::deque<std::function<void()>> m_Queue;
    std::vector<std::thread> m_Workers;
    int m_Completed = 0;
    bool m_Stop = false;
};

// Single-threaded, deterministic executor: Dispatch enqueues, RunAll runs FIFO
// on the test thread. Running a task may enqueue more (via the gate's pump), so
// RunAll keeps going until the queue is empty. Optionally throws on the Nth
// Dispatch to model an allocation failure at a chosen point.
class ManualExecutor
{
  public:
    void ThrowOnDispatch(int nthCall) { m_ThrowOnCall = nthCall; }

    void Dispatch(std::function<void()> work, JobPriority)
    {
        if (++m_DispatchCalls == m_ThrowOnCall)
            throw std::runtime_error("dispatch failed");
        m_Queue.push_back(std::move(work));
    }

    void RunAll()
    {
        while (!m_Queue.empty())
        {
            auto job = std::move(m_Queue.front());
            m_Queue.pop_front();
            job();
        }
    }

  private:
    std::deque<std::function<void()>> m_Queue;
    int m_DispatchCalls = 0;
    int m_ThrowOnCall = -1;
};

constexpr auto kDrainTimeout = std::chrono::seconds(60);
} // namespace

TEST(CompileConcurrencyGate, DispatchIsNeverInline)
{
    PoolExecutor executor(4);
    CompileConcurrencyGate gate(1, [&executor](std::function<void()> work, JobPriority priority)
                                { executor.Dispatch(std::move(work), priority); });

    const std::thread::id submittingThread = std::this_thread::get_id();
    std::atomic<bool> ranInline{false};

    gate.Submit(
        [&]()
        {
            if (std::this_thread::get_id() == submittingThread)
                ranInline.store(true);
        },
        JobPriority::Normal);

    ASSERT_TRUE(executor.WaitForCompleted(1, kDrainTimeout));
    EXPECT_FALSE(ranInline.load()) << "work must never run on the submitting thread";
}

// A dispatch that throws while PUMPING (not on the initial admit) must drop the
// promoted job — its closure destroyed (guard fires, no counter leak) — release
// the slot, and continue draining the rest. cap=1 is the sharp case.
TEST(CompileConcurrencyGate, ThrowingPumpDropsJobWithoutLeakOrStrand)
{
    ManualExecutor executor;
    executor.ThrowOnDispatch(2); // admit succeeds; the first pump dispatch throws
    CompileConcurrencyGate gate(1, [&executor](std::function<void()> work, JobPriority priority)
                                { executor.Dispatch(std::move(work), priority); });

    std::atomic<int> destroyed{0};
    std::atomic<int> ran{0};
    for (int i = 0; i < 3; ++i)
        gate.Submit([&ran, guard = MakeDestroyGuard(destroyed)]() { ran.fetch_add(1); },
                    JobPriority::Normal);

    executor.RunAll();

    EXPECT_EQ(destroyed.load(), 3) << "every closure must be destroyed exactly once (no leak)";
    EXPECT_EQ(ran.load(), 2) << "one job was dropped by the throwing pump dispatch";
    EXPECT_EQ(gate.Running(), 0u) << "the dropped job's slot must be released";
    EXPECT_EQ(gate.PendingCount(), 0u) << "no job left stranded";
}

// R1-F2: a job pushed to a lane in the window between Submit's unlock and its
// dispatch throwing must not be stranded. Reproduced deterministically by having
// the throwing dispatch re-enter Submit (simulating a concurrent submitter) just
// before it throws; the admit-failure path must pump that stranded job.
TEST(CompileConcurrencyGate, AdmitFailurePumpsConcurrentlyStrandedJob)
{
    struct RaceExecutor
    {
        CompileConcurrencyGate* Gate = nullptr;
        std::function<void()> ExtraJob;
        bool Fired = false;
        std::deque<std::function<void()>> Queue;

        void Dispatch(std::function<void()> work, JobPriority)
        {
            if (!Fired)
            {
                Fired = true;
                // Land a second submit in the unlock->throw window: cap is full,
                // so it goes to a pending lane, then this admit dispatch throws.
                Gate->Submit(std::move(ExtraJob), JobPriority::Normal);
                throw std::runtime_error("dispatch failed mid-window");
            }
            Queue.push_back(std::move(work));
        }
        void RunAll()
        {
            while (!Queue.empty())
            {
                auto job = std::move(Queue.front());
                Queue.pop_front();
                job();
            }
        }
    } executor;

    CompileConcurrencyGate gate(1, [&executor](std::function<void()> work, JobPriority priority)
                                { executor.Dispatch(std::move(work), priority); });
    executor.Gate = &gate;

    std::atomic<int> destroyed{0};
    std::atomic<int> ran{0};
    executor.ExtraJob = [&ran, guard = MakeDestroyGuard(destroyed)]() { ran.fetch_add(1); };

    bool threw = false;
    try
    {
        gate.Submit([&ran, guard = MakeDestroyGuard(destroyed)]() { ran.fetch_add(1); },
                    JobPriority::Normal);
    }
    catch (const std::exception&)
    {
        threw = true; // the allocation-failure exception propagates to the submitter
    }
    ASSERT_TRUE(threw);

    executor.RunAll();

    EXPECT_EQ(gate.PendingCount(), 0u) << "the concurrently stranded job was not pumped";
    EXPECT_EQ(gate.Running(), 0u);
    EXPECT_EQ(ran.load(), 1) << "the stranded job ran; the throwing one did not";
    EXPECT_EQ(destroyed.load(), 2) << "both closures destroyed (no leak)";
}

// Destroying the gate while jobs are still pending (executor paused, never runs
// them) must not crash or hang. Held wrappers capture the gate but are never
// invoked, so they touch no freed state.
TEST(CompileConcurrencyGate, DestroyWithPendingDoesNotCrash)
{
    struct PausedExecutor
    {
        std::vector<std::function<void()>> Held;
        void Dispatch(std::function<void()> work, JobPriority) { Held.push_back(std::move(work)); }
    };

    std::atomic<int> destroyed{0};
    {
        PausedExecutor executor;
        {
            CompileConcurrencyGate gate(
                2, [&executor](std::function<void()> work, JobPriority priority)
                { executor.Dispatch(std::move(work), priority); });

            for (int i = 0; i < 10; ++i)
                gate.Submit([guard = MakeDestroyGuard(destroyed)]() {}, JobPriority::Normal);

            EXPECT_EQ(gate.Running(), 2u);
            EXPECT_EQ(gate.PendingCount(), 8u);
        } // gate destroyed here with 8 pending closures -> 8 guards fire.
    } // executor destroyed -> 2 held (un-invoked) wrappers destroyed -> 2 guards fire.

    EXPECT_EQ(destroyed.load(), 10) << "every closure destroyed exactly once, none leaked/crashed";
}

// Background-lane jobs (user-visible variant misses) drain before Normal-lane
// prewarm even when queued after it.
TEST(CompileConcurrencyGate, BackgroundLaneDrainsBeforeNormal)
{
    ManualExecutor executor;
    CompileConcurrencyGate gate(1, [&executor](std::function<void()> work, JobPriority priority)
                                { executor.Dispatch(std::move(work), priority); });

    std::vector<std::string> order;
    gate.Submit([&order]() { order.push_back("A_normal"); }, JobPriority::Normal);        // admitted
    gate.Submit([&order]() { order.push_back("B_normal"); }, JobPriority::Normal);        // pending
    gate.Submit([&order]() { order.push_back("C_normal"); }, JobPriority::Normal);        // pending
    gate.Submit([&order]() { order.push_back("X_background"); }, JobPriority::Background); // pending

    executor.RunAll();

    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order[0], "A_normal");     // already running when the rest queued
    EXPECT_EQ(order[1], "X_background"); // Background jumps ahead of the Normal backlog
    EXPECT_EQ(order[2], "B_normal");
    EXPECT_EQ(order[3], "C_normal");
    EXPECT_EQ(gate.Running(), 0u);
    EXPECT_EQ(gate.PendingCount(), 0u);
}

// The real main-thread + BeginFrame shape: many concurrent submitters, mixed
// lanes. Concurrency must never exceed cap and everything must drain, the
// Background lane included.
TEST(CompileConcurrencyGate, ConcurrentMultiSubmitterStress)
{
    constexpr uint32_t kCap = 6;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    constexpr int kTotal = kThreads * kPerThread;

    PoolExecutor executor(16);
    CompileConcurrencyGate gate(kCap, [&executor](std::function<void()> work, JobPriority priority)
                                { executor.Dispatch(std::move(work), priority); });

    std::atomic<int> concurrent{0};
    std::atomic<int> observedPeak{0};
    std::atomic<int> completed{0};

    std::vector<std::thread> submitters;
    for (int t = 0; t < kThreads; ++t)
    {
        submitters.emplace_back(
            [&]()
            {
                for (int j = 0; j < kPerThread; ++j)
                {
                    const JobPriority prio =
                        (j % 3 == 0) ? JobPriority::Background : JobPriority::Normal;
                    gate.Submit(
                        [&concurrent, &observedPeak, &completed]()
                        {
                            const int now = concurrent.fetch_add(1, std::memory_order_acq_rel) + 1;
                            int peak = observedPeak.load(std::memory_order_relaxed);
                            while (now > peak
                                   && !observedPeak.compare_exchange_weak(
                                       peak, now, std::memory_order_relaxed))
                            {
                            }
                            std::this_thread::sleep_for(std::chrono::microseconds(20));
                            concurrent.fetch_sub(1, std::memory_order_acq_rel);
                            completed.fetch_add(1, std::memory_order_acq_rel);
                        },
                        prio);
                }
            });
    }
    for (auto& s : submitters)
        s.join();

    ASSERT_TRUE(executor.WaitForCompleted(kTotal, kDrainTimeout)) << "gate lost work";
    EXPECT_EQ(completed.load(), kTotal) << "every submitted task must run exactly once";
    EXPECT_LE(observedPeak.load(), static_cast<int>(kCap)) << "concurrency exceeded cap";
    EXPECT_LE(gate.HighWater(), kCap) << "gate's own high-water exceeded cap";
    EXPECT_EQ(gate.Running(), 0u) << "gate leaked a running slot";
    EXPECT_EQ(gate.PendingCount(), 0u) << "gate leaked a pending job";
}

TEST(CompileConcurrencyGate, ComputedCapIsSane)
{
    const uint32_t cap = ComputeShaderCompileConcurrencyCap();
    EXPECT_GE(cap, 2u) << "cap must leave at least a minimum of parallelism";
    if (std::getenv("GE_SHADER_COMPILE_CAP") == nullptr)
        EXPECT_LE(cap, std::max(2u, std::thread::hardware_concurrency()))
            << "derived cap must not exceed the logical core count";
}
