// ParallelFor: the fork core. One run is a RunBlock in a 128-byte slab, shared
// by the caller and its helpers through an intrusive reference count; units
// are claimed one at a time from one atomic word, and the caller joins on the
// count of units in progress only.
//
// The claim word: the next unit to claim in the low bits, kClosed in the high
// bit. A claim is one seq_cst fetch_add(1), which takes a unit and observes
// the closure in the same read-modify-write; closing (the caller, at the end
// of its loop) and stopping (a false body, a throw, Stop) are a seq_cst
// fetch_or(kClosed) on the same word, so no unit starts after a stop in the
// word's modification order.
//
// A fork publishes only the helpers that fit the pool's bound on helpers
// published and not yet started in their lane, at most
// kMaxUnstartedClaimHelpersPerWorker per worker; the caller claims what they
// would have. A helper's first act is to give its count back to the pool,
// which outlives it; nothing after that depends on the count.
//
// A helper, in order (every exit after step 1 goes through Leave):
//  (0) load the claim word: closed or past the end -> drop the reference and
//      end, having counted nothing and touched nothing of the caller's;
//  (1) count itself in progress;
//  (2) re-load the claim word: closed -> Leave;
//  (3) Admission->TryAcquire, if the run has one: refused -> Leave;
//  (4) claim: closed or past the end -> release the admission, Leave;
//  (5) run the unit;
//  (6) release the admission;
//  (7) with an admission: Leave, keeping the reference, and re-publish at the
//      lane's tail with it while units are left, the run is open and the
//      shutdown gate is not set; without one: back to (4), holding the count,
//      so a helper leaves exactly once, through (4).
// Leave: decrement in-progress; if that reached zero and the claim word shows
// closed, notify the caller. Two Dekker pairs make the join exact: the
// helper's increment then claim-word load against the caller's close then
// in-progress load (either the helper backs out at (2) or the caller sees it
// in flight), and the helper's decrement then claim-word load against the
// same close then load (either the helper sees closed and notifies, or the
// caller sees zero and never waits). The caller's body, Stop and Admission
// are touched only between an open observation at (2) and the Leave, and the
// caller returns only once the count is zero after its close: a helper
// running after the caller returned touches only the block, which its own
// reference keeps alive.

#include "JobSystem/WorkStealingThreadPool.h"

#include "Platform/Thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <new>
#include <thread>
#include <utility>
#include <version>

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

static_assert(__cpp_lib_atomic_wait >= 201907L, "ParallelFor parks on std::atomic::wait");

namespace JobSystem
{

namespace
{

constexpr size_t kClosed = size_t{1} << (sizeof(size_t) * 8 - 1);

#if GE_DEBUG_INSTRUMENTATION
std::atomic<void (*)(WorkStealingThreadPool::ClaimHelperPoint)> g_ClaimHelperHook{nullptr};
std::atomic<uint64> g_ClaimCallerParks{0};

void CallClaimHelperHook(WorkStealingThreadPool::ClaimHelperPoint point)
{
    if (auto hook = g_ClaimHelperHook.load(std::memory_order_acquire))
        hook(point);
}
#endif

// One ParallelFor call. Lives in a slab; freed by whoever drops the last
// reference (the caller after its join, or a helper that ran late).
struct RunBlock
{
    std::atomic<size_t> Claim{0};
    std::atomic<uint32> InProgress{0};
    std::atomic<uint32> References{0};
    std::atomic<bool> Stopped{false};
    std::atomic<bool> HasException{false};
    std::exception_ptr Exception;
    size_t UnitCount = 0;
    bool (*Invoke)(void* body, size_t unit) = nullptr;
    void* Body = nullptr;
    const ParallelForOptions::UnitAdmission* Admission = nullptr;
    WorkStealingThreadPool* Pool = nullptr;
    JobPriority HelperClass = JobPriority::Normal;
};
static_assert(sizeof(RunBlock) <= kTaskSlabSize, "a ParallelFor run block fits one task slab");
static_assert(alignof(RunBlock) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__, "a task slab is default-aligned");

RunBlock* CreateRunBlock()
{
    return new (Detail::AcquireTaskSlab()) RunBlock();
}

void DropReference(RunBlock* block)
{
    if (block->References.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        block->~RunBlock();
        Detail::ReleaseTaskSlab(block);
    }
}

bool IsOpenWithUnitsLeft(size_t claimWord, size_t unitCount)
{
    return (claimWord & kClosed) == 0 && claimWord < unitCount;
}

// Stops the run: no unit starts after this in the claim word's order.
void StopRun(RunBlock& block)
{
    block.Stopped.store(true, std::memory_order_relaxed);
    block.Claim.fetch_or(kClosed, std::memory_order_seq_cst);
}

// Keeps the first exception of the run and stops it. Called inside a catch.
void RecordExceptionAndStop(RunBlock& block)
{
    if (!block.HasException.exchange(true, std::memory_order_acq_rel))
        block.Exception = std::current_exception();
    StopRun(block);
}

// Runs one claimed unit: a false return or a throw stops the run.
void RunUnit(RunBlock& block, size_t unit)
{
    try
    {
        if (!block.Invoke(block.Body, unit))
            StopRun(block);
    }
    catch (...)
    {
        RecordExceptionAndStop(block);
    }
}

void Leave(RunBlock& block)
{
    if (block.InProgress.fetch_sub(1, std::memory_order_seq_cst) == 1 &&
        (block.Claim.load(std::memory_order_seq_cst) & kClosed) != 0)
    {
        block.InProgress.notify_all();
    }
}

void ReleaseAdmission(RunBlock& block)
{
    try
    {
        block.Admission->Release();
    }
    catch (...)
    {
        RecordExceptionAndStop(block);
    }
}

// Steps (3) to (6) for a helper with an admission. Returns whether it ran a
// unit (step 7 re-publishes only then).
bool RunAdmittedUnit(RunBlock& block)
{
    bool admitted = false;
    try
    {
        admitted = block.Admission->TryAcquire();
    }
    catch (...)
    {
        RecordExceptionAndStop(block);
        return false;
    }
    if (!admitted)
        return false;

    const size_t unit = block.Claim.fetch_add(1, std::memory_order_seq_cst);
    if (!IsOpenWithUnitsLeft(unit, block.UnitCount))
    {
        ReleaseAdmission(block);
        return false;
    }
    RunUnit(block, unit);
    ReleaseAdmission(block);
    return true;
}

// Reserves up to `wanted` of the pool's unstarted-helper budget and returns
// the count granted; the rest is given back at once. The add comes first, so
// concurrent forks never grant more than `budget` between them.
size_t ReserveHelpers(std::atomic<uint32>& unstarted, size_t wanted, size_t budget)
{
    const size_t before = unstarted.fetch_add(static_cast<uint32>(wanted), std::memory_order_relaxed);
    const size_t granted = before >= budget ? 0 : std::min(wanted, budget - before);
    if (granted < wanted)
        unstarted.fetch_sub(static_cast<uint32>(wanted - granted), std::memory_order_relaxed);
    return granted;
}

// The helper task: one reference to the block, and one count of the pool's
// unstarted helpers, reserved for it before it was published and given back
// when it starts. Move-only; an envelope destroyed without running gives its
// count back and drops its reference.
class ClaimHelper
{
  public:
    ClaimHelper() = default;
    ClaimHelper(RunBlock* block, std::atomic<uint32>* unstarted) : m_Block(block), m_Unstarted(unstarted) {}
    ClaimHelper(ClaimHelper&& other) noexcept
        : m_Block(std::exchange(other.m_Block, nullptr)), m_Unstarted(std::exchange(other.m_Unstarted, nullptr))
    {
    }
    ClaimHelper& operator=(ClaimHelper&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_Block = std::exchange(other.m_Block, nullptr);
            m_Unstarted = std::exchange(other.m_Unstarted, nullptr);
        }
        return *this;
    }
    ClaimHelper(const ClaimHelper&) = delete;
    ClaimHelper& operator=(const ClaimHelper&) = delete;
    ~ClaimHelper() { Reset(); }

    void operator()()
    {
        std::atomic<uint32>* const unstarted = m_Unstarted;
        GiveBackUnstartedCount();
        RunBlock& block = *m_Block;
        // (0)
        if (!IsOpenWithUnitsLeft(block.Claim.load(std::memory_order_acquire), block.UnitCount))
        {
            Reset();
            return;
        }
#if GE_DEBUG_INSTRUMENTATION
        CallClaimHelperHook(WorkStealingThreadPool::ClaimHelperPoint::PassedOpenCheck);
#endif
        // (1)
        block.InProgress.fetch_add(1, std::memory_order_seq_cst);
#if GE_DEBUG_INSTRUMENTATION
        CallClaimHelperHook(WorkStealingThreadPool::ClaimHelperPoint::CountedInProgress);
#endif
        // (2)
        if ((block.Claim.load(std::memory_order_seq_cst) & kClosed) != 0)
        {
            Leave(block);
            Reset();
            return;
        }

        if (block.Admission == nullptr)
        {
            // (4) to (5), until nothing is left or the run is closed.
            for (;;)
            {
                const size_t unit = block.Claim.fetch_add(1, std::memory_order_seq_cst);
                if (!IsOpenWithUnitsLeft(unit, block.UnitCount))
                    break;
                RunUnit(block, unit);
            }
            Leave(block);
            Reset();
            return;
        }

        // (3) to (7).
        const bool ranUnit = RunAdmittedUnit(block);
        Leave(block);
        WorkStealingThreadPool& pool = *block.Pool;
        if (ranUnit && IsOpenWithUnitsLeft(block.Claim.load(std::memory_order_seq_cst), block.UnitCount) &&
            !pool.IsShuttingDown())
        {
            // The count this helper gave back at its start, taken again for
            // the copy it publishes: one unstarted helper for one.
            unstarted->fetch_add(1, std::memory_order_relaxed);
            pool.EnqueueWork(ClaimHelper(std::exchange(m_Block, nullptr), unstarted), block.HelperClass);
            return;
        }
        Reset();
    }

  private:
    void GiveBackUnstartedCount()
    {
        if (m_Unstarted)
            std::exchange(m_Unstarted, nullptr)->fetch_sub(1, std::memory_order_relaxed);
    }

    void Reset()
    {
        GiveBackUnstartedCount();
        if (m_Block)
            DropReference(std::exchange(m_Block, nullptr));
    }

    RunBlock* m_Block = nullptr;
    std::atomic<uint32>* m_Unstarted = nullptr;
};

// The caller's join: wait until no unit is in progress. Called after the
// caller closed the run.
void WaitForUnitsInProgress(RunBlock& block)
{
    uint32 inProgress = block.InProgress.load(std::memory_order_seq_cst);
    if (inProgress == 0)
        return;
#if GE_DEBUG_INSTRUMENTATION
    g_ClaimCallerParks.fetch_add(1, std::memory_order_relaxed);
#endif
    if (GameEngine::Platform::CanBlockCurrentThread())
    {
        do
        {
            block.InProgress.wait(inProgress, std::memory_order_seq_cst);
            inProgress = block.InProgress.load(std::memory_order_seq_cst);
        } while (inProgress != 0);
        return;
    }

    // A thread that must service worker requests (the browser main thread):
    // a unit in progress may need it, through a proxied call, to finish. The
    // wait below services those calls while it waits; a stalled wait is
    // reported every 10 s, since it stops frames and input.
    const auto start = std::chrono::steady_clock::now();
    auto nextReport = start + std::chrono::seconds(10);
    while ((inProgress = block.InProgress.load(std::memory_order_seq_cst)) != 0)
    {
#if defined(__EMSCRIPTEN__)
        // On the main browser thread a futex wait processes the queued
        // proxied calls while it waits; the 1 ms bound keeps the loop
        // independent of how the notify is delivered.
        emscripten_futex_wait(&block.InProgress, inProgress, 1.0);
#else
        std::this_thread::yield();
#endif
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextReport)
        {
            nextReport = now + std::chrono::seconds(10);
            std::fprintf(stderr,
                         "JobSystem: host-thread ParallelFor join has lasted %llds with %u unit(s) in progress on "
                         "other threads.\n",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - start).count()),
                         static_cast<unsigned>(inProgress));
            std::fflush(stderr);
        }
    }
}

// The caller's claims, Stop polled before each.
void RunCallerUnits(RunBlock& block, const std::function<bool()>* stop)
{
    for (;;)
    {
        if (stop != nullptr && *stop)
        {
            bool stopRequested = false;
            try
            {
                stopRequested = (*stop)();
            }
            catch (...)
            {
                RecordExceptionAndStop(block);
                return;
            }
            if (stopRequested)
            {
                StopRun(block);
                return;
            }
        }
        const size_t unit = block.Claim.fetch_add(1, std::memory_order_seq_cst);
        if (!IsOpenWithUnitsLeft(unit, block.UnitCount))
            return;
        RunUnit(block, unit);
    }
}

// Every exit of a run with helpers closes it and waits for the units in
// progress before the caller's frame unwinds: helpers write into it.
class CloseAndJoinOnExit
{
  public:
    explicit CloseAndJoinOnExit(RunBlock& block) : m_Block(block) {}
    ~CloseAndJoinOnExit()
    {
        m_Block.Claim.fetch_or(kClosed, std::memory_order_seq_cst);
        WaitForUnitsInProgress(m_Block);
    }

    CloseAndJoinOnExit(const CloseAndJoinOnExit&) = delete;
    CloseAndJoinOnExit& operator=(const CloseAndJoinOnExit&) = delete;

  private:
    RunBlock& m_Block;
};

// The caller's reference to its block, dropped on every exit, a throw from
// the publish included. Declared before the join, so it is dropped after it.
class CallerReference
{
  public:
    explicit CallerReference(RunBlock* block) : m_Block(block) {}
    ~CallerReference() { DropReference(m_Block); }

    CallerReference(const CallerReference&) = delete;
    CallerReference& operator=(const CallerReference&) = delete;

  private:
    RunBlock* m_Block;
};

// A run with no helper: every unit on the caller, no block. The first throw
// leaves at once: no other thread has a unit in flight.
bool RunEveryUnitOnTheCaller(bool (*invoke)(void* body, size_t unit), void* body, size_t unitCount,
                             const std::function<bool()>* stop)
{
    for (size_t unit = 0; unit < unitCount; ++unit)
    {
        if (stop != nullptr && *stop && (*stop)())
            return false;
        if (!invoke(body, unit))
            return false;
    }
    return true;
}

} // namespace

#if GE_DEBUG_INSTRUMENTATION
void WorkStealingThreadPool::SetClaimHelperHookForTests(void (*hook)(ClaimHelperPoint point))
{
    g_ClaimHelperHook.store(hook, std::memory_order_release);
}

uint64 WorkStealingThreadPool::GetClaimCallerParksForTests()
{
    return g_ClaimCallerParks.load(std::memory_order_relaxed);
}
#endif

bool WorkStealingThreadPool::RunParallelFor(WorkStealingThreadPool* pool, bool (*invoke)(void* body, size_t unit),
                                            void* body, size_t unitCount, const ParallelForOptions& options)
{
    if (unitCount == 0)
        return true;
    if (pool == nullptr)
        return RunEveryUnitOnTheCaller(invoke, body, unitCount, options.Stop);

    const size_t asked = options.Helpers == ParallelForOptions::kAllWorkers ? pool->GetWorkerCount() : options.Helpers;
    const size_t wanted =
        (pool->m_InlineMode || pool->IsShuttingDown() || unitCount <= 1)
            ? 0
            : std::min({asked, unitCount - 1, kStackBatch});
    const JobPriority helperClass =
        (options.HelperPriority == JobPriority::Background || CallerContext().Class == JobPriority::Background)
            ? JobPriority::Background
            : JobPriority::Normal;
    std::atomic<uint32>& unstarted = pool->m_UnstartedClaimHelpers[static_cast<size_t>(helperClass)];
    const size_t helpers =
        wanted == 0 ? 0
                    : ReserveHelpers(unstarted, wanted, kMaxUnstartedClaimHelpersPerWorker * pool->GetWorkerCount());
    if (helpers == 0)
        return RunEveryUnitOnTheCaller(invoke, body, unitCount, options.Stop);

    RunBlock* block = CreateRunBlock();
    block->UnitCount = unitCount;
    block->Invoke = invoke;
    block->Body = body;
    block->Admission = options.Admission;
    block->Pool = pool;
    block->HelperClass = helperClass;
    // The caller's reference and one per helper, counted before any helper
    // can run and drop its own.
    block->References.store(static_cast<uint32>(helpers + 1), std::memory_order_relaxed);
    const CallerReference reference(block);

    {
        const CloseAndJoinOnExit join(*block);
        {
            ClaimHelper staging[kStackBatch];
            for (size_t i = 0; i < helpers; ++i)
                staging[i] = ClaimHelper(block, &unstarted);
            pool->EnqueueWorkBatch(staging, helpers, block->HelperClass);
        }
        RunCallerUnits(*block, options.Stop);
    }

    const bool stopped = block->Stopped.load(std::memory_order_relaxed);
    std::exception_ptr thrown = block->HasException.load(std::memory_order_acquire) ? block->Exception : nullptr;
    if (thrown)
        std::rethrow_exception(thrown);
    return !stopped;
}

} // namespace JobSystem
