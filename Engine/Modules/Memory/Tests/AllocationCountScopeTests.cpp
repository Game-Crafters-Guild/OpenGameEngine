// The two windows of the allocation counter: what each one sees and what it must not.

#include "Memory/AllocationCountScope.h"

#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>

namespace
{
using GameEngine::Memory::AllocationCountScope;
using GameEngine::Memory::CountWindow;

constexpr std::size_t kProbeBytes = 64;

/// One counted allocation. A direct call to the replaceable function is never elided, unlike a
/// new-expression whose result the optimizer can prove unused.
void AllocateOnce()
{
    ::operator delete(::operator new(kProbeBytes));
}

/// Forks before the measurement so the pool's first-use costs (the counter's tagged queue, the
/// workers' stubs) are outside the window.
constexpr int kWarmupForks = 64;
} // namespace

// Thread B allocates while thread A's ThisThread window is open: A's count moves only for A's
// own allocations. B is started before the window opens, because starting a thread allocates on
// the starting thread.
TEST(AllocationCountScopeTests, CountsOnThreadAAreUntouchedByThreadB)
{
    constexpr int kOtherThreadAllocations = 100;
    std::atomic<bool> go{false};
    std::thread threadB(
        [&go]
        {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kOtherThreadAllocations; ++i)
                AllocateOnce();
        });
    std::uint64_t afterOtherThread = 0;
    std::uint64_t afterOwnAllocation = 0;
    {
        const AllocationCountScope threadA(CountWindow::ThisThread);
        go.store(true, std::memory_order_release);
        threadB.join();
        afterOtherThread = threadA.Count();
        AllocateOnce();
        afterOwnAllocation = threadA.Count();
    }

    EXPECT_EQ(afterOtherThread, 0u) << "a ThisThread window counted another thread's allocations";
    EXPECT_EQ(afterOwnAllocation, 1u) << "a ThisThread window missed its own thread's allocation";
}

// A Run unit on a worker allocates once while the forking thread waits without running it: the
// forking thread's ThisThread window reads 0 and the Process window reads 1. A Process window
// that counted only its constructing thread would read 0, and every pin of a forking path
// (Query::Parallel's units, a wave's system bodies) would pass vacuously.
TEST(AllocationCountScopeTests, AnAllocationInAWorkerUnitIsCountedByTheProcessWindow)
{
    JobSystem::WorkStealingThreadPool pool(1);
    for (int i = 0; i < kWarmupForks; ++i)
    {
        JobSystem::JobCounter counter;
        pool.Run([] {}, counter);
        pool.Wait(counter);
    }

    const std::thread::id forkingThread = std::this_thread::get_id();
    std::atomic<bool> ran{false};
    std::thread::id unitThread;
    std::uint64_t forkingThreadCount = 0;
    std::uint64_t processCount = 0;
    {
        const AllocationCountScope process(CountWindow::Process);
        const AllocationCountScope thisThread(CountWindow::ThisThread);
        JobSystem::JobCounter counter;
        pool.Run(
            [&ran, &unitThread]
            {
                unitThread = std::this_thread::get_id();
                AllocateOnce();
                ran.store(true, std::memory_order_release);
            },
            counter);
        // Not Wait yet: a waiting thread may run the unit itself, and the unit must run on the
        // worker for the windows to differ.
        while (!ran.load(std::memory_order_acquire))
            std::this_thread::yield();
        pool.Wait(counter);
        forkingThreadCount = thisThread.Count();
        processCount = process.Count();
    }

    ASSERT_NE(unitThread, forkingThread) << "the unit ran on the forking thread; the test measured nothing";
    EXPECT_EQ(forkingThreadCount, 0u) << "the fork itself allocated on the forking thread after warm-up";
    EXPECT_EQ(processCount, 1u) << "the Process window did not count exactly the worker unit's one allocation";
}

// Two Process windows that overlap each read the allocations made while they were open, not
// each other's starting point.
TEST(AllocationCountScopeTests, OverlappingProcessWindowsEachReadTheirOwnDelta)
{
    const AllocationCountScope outer(CountWindow::Process);
    AllocateOnce();
    std::uint64_t innerCount = 0;
    {
        const AllocationCountScope inner(CountWindow::Process);
        AllocateOnce();
        AllocateOnce();
        innerCount = inner.Count();
    }
    AllocateOnce();

    EXPECT_EQ(innerCount, 2u);
    EXPECT_EQ(outer.Count(), 4u);
}

// Every global operator new form reaches the counter, the aligned and nothrow forms included.
TEST(AllocationCountScopeTests, EveryOperatorNewFormIsCounted)
{
    constexpr std::align_val_t kOverAligned{128};
    const AllocationCountScope scope(CountWindow::ThisThread);
    ::operator delete(::operator new(kProbeBytes));
    ::operator delete[](::operator new[](kProbeBytes));
    ::operator delete(::operator new(kProbeBytes, std::nothrow));
    ::operator delete[](::operator new[](kProbeBytes, std::nothrow));
    ::operator delete(::operator new(kProbeBytes, kOverAligned), kOverAligned);
    ::operator delete[](::operator new[](kProbeBytes, kOverAligned), kOverAligned);
    ::operator delete(::operator new(kProbeBytes, kOverAligned, std::nothrow), kOverAligned);
    ::operator delete[](::operator new[](kProbeBytes, kOverAligned, std::nothrow), kOverAligned);

    EXPECT_EQ(scope.Count(), 8u);
}
