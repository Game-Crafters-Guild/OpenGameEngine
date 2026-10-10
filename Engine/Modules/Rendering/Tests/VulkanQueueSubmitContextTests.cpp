// Regression tests for the per-physical-queue submit chokepoint, and for the accounting that
// reads the values it allocates.
//
// The defect these pin is invisible to the Vulkan validation layers: validation trusts the
// timeline value the engine itself signals, so two submits claiming the same value read as
// one well-formed submit each. The failure only surfaces later, as a command buffer reset
// while the GPU is still executing it. The invariant is therefore asserted structurally, on
// the values the chokepoint allocates, rather than on any observable GPU behaviour.
//
// Assertions run on the main thread after every worker has joined (a fatal gtest assertion
// from a worker is undefined), so a failure here is a genuinely lost or duplicated value and
// never a benign mid-flight read.

#include "Source/Vulkan/VulkanQueueRetireTracking.h"
#include "Source/Vulkan/VulkanQueueSubmitContext.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

constexpr int kThreads = 8;
constexpr int kSubmitsPerThread = 256;
constexpr uint64_t kTotalSubmits = static_cast<uint64_t>(kThreads) * kSubmitsPerThread;

// Handles the context only stores and compares against null, never dereferences.
VkQueue FakeQueue()
{
    return reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0xC0FFEE));
}

VkSemaphore FakeTimelineSemaphore()
{
    return reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0x5E4A));
}

// QueueSubmitContext owns a mutex, so it is neither copyable nor movable: bind in place.
void BindTestQueue(QueueSubmitContext& ctx, uint64_t timelineId = 7)
{
    ctx.Bind(FakeQueue(), SemaphoreHandle(timelineId), FakeTimelineSemaphore());
}

// Releases every worker at once, so the threads genuinely overlap instead of running to
// completion one after another (which would let a broken allocator pass vacuously).
struct StartGate
{
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    void ArriveAndWait()
    {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

    void ReleaseWhenAllReady(int expected)
    {
        while (ready.load(std::memory_order_acquire) < expected)
            std::this_thread::yield();
        go.store(true, std::memory_order_release);
    }
};

// The structural oracle. A queue's signalled values must be exactly 1..N: no repeats, because
// two submits sharing a value means the first to complete releases the second's command
// buffers while they still execute; no gaps, because a value that is consumed but never
// signalled is one every later wait on that timeline blocks on forever.
::testing::AssertionResult ValuesAreExactlyOneThroughN(std::vector<uint64_t> values, uint64_t n)
{
    if (values.size() != n)
        return ::testing::AssertionFailure() << "expected " << n << " values, got " << values.size();

    std::sort(values.begin(), values.end());

    for (uint64_t i = 0; i < n; ++i)
    {
        const size_t at = static_cast<size_t>(i);
        if (values[at] == i + 1)
            continue;

        const bool duplicate = i > 0 && values[at] == values[at - 1];
        return ::testing::AssertionFailure()
               << (duplicate ? "duplicate value " : "gap before value ") << values[at] << " (expected "
               << (i + 1) << " at sorted position " << i << ")";
    }

    return ::testing::AssertionSuccess();
}

} // namespace

// Instrument validation: the oracle above must actually reject the allocation shape this
// chokepoint replaced. The pre-fix graphics path read the counter, submitted, then published
// (VulkanDevice.cpp before this change: read at :6997, vkQueueSubmit at :7009, publish at
// :7060) with no lock held. Every worker here reads before any worker publishes, which makes
// the duplicate that races into existence in the real path deterministic — so this pins the
// oracle's discriminating power without depending on a race firing.
//
// The counter is atomic with separate relaxed load and store: the defect being modelled is the
// non-atomicity of the read-modify-write *sequence*, not of its individual accesses, and a
// plain uint64_t here would be a data race in the test itself.
TEST(VulkanQueueSubmitContext, OracleRejectsUnlockedReadModifyWrite)
{
    std::atomic<uint64_t> lastSignalled{0};
    std::mutex collectMutex;
    std::vector<uint64_t> signalled;

    StartGate readGate;
    StartGate publishGate;
    std::vector<std::thread> workers;

    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&] {
            readGate.ArriveAndWait();
            const uint64_t value = lastSignalled.load(std::memory_order_relaxed) + 1; // read

            // Stand-in for vkQueueSubmit: every thread has now read, none has published.
            publishGate.ArriveAndWait();

            lastSignalled.store(value, std::memory_order_relaxed); // publish
            {
                std::lock_guard<std::mutex> lock(collectMutex);
                signalled.push_back(value);
            }
        });
    }

    readGate.ReleaseWhenAllReady(kThreads);
    publishGate.ReleaseWhenAllReady(kThreads);
    for (std::thread& worker : workers)
        worker.join();

    EXPECT_FALSE(ValuesAreExactlyOneThroughN(signalled, static_cast<uint64_t>(kThreads)))
        << "the oracle accepted an unlocked read-modify-write, so it cannot detect the defect";

    // Name the mechanism rather than leaving a negated assertion: every thread claimed value 1.
    EXPECT_EQ(std::count(signalled.begin(), signalled.end(), 1ull), static_cast<ptrdiff_t>(kThreads));
}

// The invariant itself: concurrent submits allocate unique, gap-free values.
TEST(VulkanQueueSubmitContext, ConcurrentSubmitsAllocateUniqueContiguousValues)
{
    QueueSubmitContext ctx;
    BindTestQueue(ctx);

    std::mutex collectMutex;
    std::vector<uint64_t> signalled;
    signalled.reserve(static_cast<size_t>(kTotalSubmits));

    StartGate gate;
    std::vector<std::thread> workers;

    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&] {
            gate.ArriveAndWait();
            for (int i = 0; i < kSubmitsPerThread; ++i)
            {
                const QueueSubmitContext::SubmitResult result =
                    ctx.SubmitAndSignal([](uint64_t) { return true; });
                std::lock_guard<std::mutex> lock(collectMutex);
                signalled.push_back(result.SignalledValue);
            }
        });
    }

    gate.ReleaseWhenAllReady(kThreads);
    for (std::thread& worker : workers)
        worker.join();

    EXPECT_TRUE(ValuesAreExactlyOneThroughN(signalled, kTotalSubmits));
    EXPECT_EQ(ctx.LastSignalled(), kTotalSubmits);
}

// Allocation order must equal submission order. This is the property a probe that only
// compares values would miss: values could be unique and still reach vkQueueSubmit out of
// order, which signals a timeline non-monotonically and mistags every retire.
//
// The callback runs under the queue's lock, so the order values are appended here IS the order
// the submits were issued in.
TEST(VulkanQueueSubmitContext, SubmissionOrderMatchesAllocationOrder)
{
    QueueSubmitContext ctx;
    BindTestQueue(ctx);

    std::vector<uint64_t> submitOrder;
    submitOrder.reserve(static_cast<size_t>(kTotalSubmits));
    std::atomic<int> concurrentSubmits{0};
    std::atomic<int> maxConcurrentSubmits{0};

    auto noteEntry = [&] {
        const int inFlight = concurrentSubmits.fetch_add(1, std::memory_order_acq_rel) + 1;
        int observed = maxConcurrentSubmits.load(std::memory_order_relaxed);
        while (inFlight > observed &&
               !maxConcurrentSubmits.compare_exchange_weak(observed, inFlight, std::memory_order_relaxed))
        {
        }
    };

    StartGate gate;
    std::vector<std::thread> workers;

    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&] {
            gate.ArriveAndWait();
            for (int i = 0; i < kSubmitsPerThread; ++i)
            {
                ctx.SubmitAndSignal([&](uint64_t signalValue) {
                    noteEntry();
                    // Unsynchronised on purpose: the queue lock is what makes this safe, and
                    // that is exactly what is under test.
                    submitOrder.push_back(signalValue);
                    concurrentSubmits.fetch_sub(1, std::memory_order_acq_rel);
                    return true;
                });
            }
        });
    }

    gate.ReleaseWhenAllReady(kThreads);
    for (std::thread& worker : workers)
        worker.join();

    ASSERT_EQ(submitOrder.size(), static_cast<size_t>(kTotalSubmits));
    EXPECT_EQ(maxConcurrentSubmits.load(), 1)
        << "two submits entered the queue at once; VkQueue is externally synchronised";

    const auto outOfOrder = std::adjacent_find(submitOrder.begin(), submitOrder.end(),
                                               [](uint64_t a, uint64_t b) { return b <= a; });
    EXPECT_EQ(outOfOrder, submitOrder.end()) << "values reached vkQueueSubmit out of allocation order";
    EXPECT_EQ(submitOrder.front(), 1ull);
    EXPECT_EQ(submitOrder.back(), kTotalSubmits);
}

// A command buffer must never be retired against a value below its own submit's. Tagging with
// the value the submit RETURNED makes that true by construction; tagging with a re-read of the
// counter does not, which is what the returned value exists to prevent.
TEST(VulkanQueueSubmitContext, RetireTagsMatchTheirOwnSubmit)
{
    QueueSubmitContext ctx;
    BindTestQueue(ctx);

    struct RetireTag
    {
        uint64_t submitValue = 0; // value the submit reported signalling
        uint64_t tag = 0;         // value the command buffer was routed against
    };

    std::mutex collectMutex;
    std::vector<RetireTag> tags;
    tags.reserve(static_cast<size_t>(kTotalSubmits));

    StartGate gate;
    std::vector<std::thread> workers;

    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back([&] {
            gate.ArriveAndWait();
            for (int i = 0; i < kSubmitsPerThread; ++i)
            {
                uint64_t routedAgainst = 0;
                const QueueSubmitContext::SubmitResult result =
                    ctx.SubmitAndSignal([&](uint64_t signalValue) {
                        routedAgainst = signalValue; // what the retire list records
                        return true;
                    });
                std::lock_guard<std::mutex> lock(collectMutex);
                tags.push_back({result.SignalledValue, routedAgainst});
            }
        });
    }

    gate.ReleaseWhenAllReady(kThreads);
    for (std::thread& worker : workers)
        worker.join();

    ASSERT_EQ(tags.size(), static_cast<size_t>(kTotalSubmits));
    std::vector<uint64_t> routed;
    routed.reserve(tags.size());
    for (const RetireTag& tag : tags)
    {
        ASSERT_NE(tag.submitValue, 0ull);
        ASSERT_EQ(tag.tag, tag.submitValue) << "command buffer routed against another submit's value";
        routed.push_back(tag.tag);
    }
    EXPECT_TRUE(ValuesAreExactlyOneThroughN(std::move(routed), kTotalSubmits));
}

// A failed submit signals nothing, so it must publish nothing: the value it would have used
// stays available, and the caller is told it may retire nothing against it.
TEST(VulkanQueueSubmitContext, FailedSubmitPublishesNothingAndReleasesItsValue)
{
    QueueSubmitContext ctx;
    BindTestQueue(ctx);

    EXPECT_EQ(ctx.SubmitAndSignal([](uint64_t) { return true; }).SignalledValue, 1ull);

    uint64_t offeredToFailure = 0;
    const QueueSubmitContext::SubmitResult failed = ctx.SubmitAndSignal([&](uint64_t signalValue) {
        offeredToFailure = signalValue;
        return false;
    });

    EXPECT_EQ(offeredToFailure, 2ull);
    EXPECT_FALSE(failed.Submitted);
    EXPECT_EQ(failed.SignalledValue, 0ull) << "a failed submit must not hand out a retire tag";
    EXPECT_EQ(ctx.LastSignalled(), 1ull) << "a failed submit must not advance the timeline counter";

    // The value the failure did not consume is the next one handed out; leaking it would leave
    // a hole the GPU never signals.
    const QueueSubmitContext::SubmitResult next = ctx.SubmitAndSignal([](uint64_t) { return true; });
    EXPECT_TRUE(next.Submitted);
    EXPECT_EQ(next.SignalledValue, 2ull);
    EXPECT_EQ(ctx.LastSignalled(), 2ull);
}

// A queue with no timeline semaphore still serialises, but allocates no values. Reporting a
// value here would tag work against a timeline that will never advance.
TEST(VulkanQueueSubmitContext, QueueWithoutTimelineSignalsNothingButStillSubmits)
{
    QueueSubmitContext ctx;
    ctx.Bind(FakeQueue(), SemaphoreHandle(0ull), VK_NULL_HANDLE);

    uint64_t offered = ~0ull;
    const QueueSubmitContext::SubmitResult result = ctx.SubmitAndSignal([&](uint64_t signalValue) {
        offered = signalValue;
        return true;
    });

    EXPECT_FALSE(ctx.HasTimeline());
    EXPECT_EQ(offered, 0ull) << "the submit must be told to signal no timeline value";
    EXPECT_TRUE(result.Submitted);
    EXPECT_EQ(result.SignalledValue, 0ull);
    EXPECT_EQ(ctx.LastSignalled(), 0ull);
}

// Fence-only submits and presentation take the same queue lock as signalling submits: VkQueue
// is externally synchronised for every call that takes it, not only for vkQueueSubmit. They
// must not consume a timeline value while doing so.
TEST(VulkanQueueSubmitContext, UnsignalledOperationsExcludeSubmitsWithoutConsumingValues)
{
    QueueSubmitContext ctx;
    BindTestQueue(ctx);

    std::atomic<int> inCriticalSection{0};
    std::atomic<int> maxInCriticalSection{0};
    std::atomic<int> unsignalledOps{0};

    auto enter = [&] {
        const int now = inCriticalSection.fetch_add(1, std::memory_order_acq_rel) + 1;
        int observed = maxInCriticalSection.load(std::memory_order_relaxed);
        while (now > observed &&
               !maxInCriticalSection.compare_exchange_weak(observed, now, std::memory_order_relaxed))
        {
        }
    };
    auto leave = [&] { inCriticalSection.fetch_sub(1, std::memory_order_acq_rel); };

    StartGate gate;
    std::vector<std::thread> workers;

    for (int t = 0; t < kThreads; ++t)
    {
        const bool presents = (t % 2) == 0;
        workers.emplace_back([&, presents] {
            gate.ArriveAndWait();
            for (int i = 0; i < kSubmitsPerThread; ++i)
            {
                if (presents)
                {
                    ctx.WithQueueLocked([&] {
                        enter();
                        unsignalledOps.fetch_add(1, std::memory_order_relaxed);
                        leave();
                    });
                }
                else
                {
                    ctx.SubmitAndSignal([&](uint64_t) {
                        enter();
                        leave();
                        return true;
                    });
                }
            }
        });
    }

    gate.ReleaseWhenAllReady(kThreads);
    for (std::thread& worker : workers)
        worker.join();

    const uint64_t signallingSubmits = static_cast<uint64_t>(kThreads / 2) * kSubmitsPerThread;
    EXPECT_EQ(maxInCriticalSection.load(), 1)
        << "presentation and submission overlapped on one externally-synchronised VkQueue";
    EXPECT_EQ(unsignalledOps.load(), (kThreads / 2) * kSubmitsPerThread);
    EXPECT_EQ(ctx.LastSignalled(), signallingSubmits)
        << "unsignalled operations must not consume timeline values";
}

// A rebuild creates fresh semaphores starting at 0, so the counter must restart with them.
// Carrying the old counter forward would tag work against values the new timeline reaches
// immediately, freeing command buffers the GPU has not started.
TEST(VulkanQueueSubmitContext, RebindResetsTheCounter)
{
    QueueSubmitContext ctx;
    BindTestQueue(ctx);
    EXPECT_EQ(ctx.SubmitAndSignal([](uint64_t) { return true; }).SignalledValue, 1ull);
    EXPECT_EQ(ctx.SubmitAndSignal([](uint64_t) { return true; }).SignalledValue, 2ull);

    ctx.Unbind();
    EXPECT_EQ(ctx.LastSignalled(), 0ull);
    EXPECT_FALSE(ctx.HasTimeline());

    BindTestQueue(ctx, 9);
    EXPECT_EQ(ctx.LastSignalled(), 0ull);
    EXPECT_EQ(ctx.SubmitAndSignal([](uint64_t) { return true; }).SignalledValue, 1ull);
}

// ---------------------------------------------------------------------------------------------
// Role space vs context space.
//
// Allocation lives in per-physical-queue contexts, so every consumer of an allocated value must
// read it from the same place. The counters named after the ROLES (graphics/compute/transfer
// timelines) are not those places: a role with no dedicated family resolves onto another role's
// queue, and the timeline named after it is then signalled by nothing at all. Pairing a role's
// timeline against a context's counter yields "completed 0, submitted N" forever, which is not a
// stale reading — it is a predicate that can never come true again.
// ---------------------------------------------------------------------------------------------

namespace
{

// The three device-created timelines. Which of them a role's work actually signals depends on
// the family selection, which is the whole point.
constexpr uint64_t kGraphicsTimelineId = 1;
constexpr uint64_t kComputeTimelineId = 2;
constexpr uint64_t kTransferTimelineId = 3;

constexpr size_t kGraphicsRole = 0;
constexpr size_t kComputeRole = 1;
constexpr size_t kTransferRole = 2;

using ContextStorage = std::array<QueueSubmitContext, kQueueRoleCount>;
using ContextsByRole = std::array<const QueueSubmitContext*, kQueueRoleCount>;

void BindContext(QueueSubmitContext& ctx, uintptr_t queue, uint64_t timelineId)
{
    ctx.Bind(reinterpret_cast<VkQueue>(queue), SemaphoreHandle(timelineId), FakeTimelineSemaphore());
}

void SubmitTimes(QueueSubmitContext& ctx, int times)
{
    for (int i = 0; i < times; ++i)
        ctx.SubmitAndSignal([](uint64_t) { return true; });
}

// What VulkanDevice::NextSubmitTagsLocked forecasts for a destruction requested now.
QueueRetireTags NextSubmitTags(const ContextsByRole& byRole)
{
    return {byRole[kGraphicsRole]->LastSignalled() + 1, byRole[kComputeRole]->LastSignalled() + 1,
            byRole[kTransferRole]->LastSignalled() + 1};
}

// The defective pairing, spelled out: completion read from the timeline named after the ROLE,
// submission read from the context the role resolved to. Hand-built rather than produced by any
// shipping function, so the tests below assert what the retire predicate does with it without
// keeping the defect reachable from the engine.
QueueTimelineProgress RoleSpaceCompletionPairing(const ContextsByRole& byRole,
                                                 const QueueTimelineCompletions& completions)
{
    QueueTimelineProgress progress{};
    completions.TryGet(SemaphoreHandle(kGraphicsTimelineId), progress.graphicsCompleted);
    completions.TryGet(SemaphoreHandle(kComputeTimelineId), progress.computeCompleted);
    completions.TryGet(SemaphoreHandle(kTransferTimelineId), progress.transferCompleted);
    progress.graphicsSubmitted = byRole[kGraphicsRole]->LastSignalled();
    progress.computeSubmitted = byRole[kComputeRole]->LastSignalled();
    progress.transferSubmitted = byRole[kTransferRole]->LastSignalled();
    return progress;
}

} // namespace

// Instrument validation, in the shape of the first test in this file: the retire predicate must
// actually reject the pairing this change replaced, or the tests that follow prove nothing.
//
// The device here found no dedicated compute or transfer family — an Intel iGPU, MoltenVK, most
// mobile parts — so all three roles run on the graphics queue and signal the graphics timeline.
// Nothing ever signals the compute or transfer timelines, so their completed values stay 0 while
// the submitted counts climb with the graphics queue's.
TEST(VulkanQueueRetireTracking, RoleSpaceCompletionNeverRetiresOnceAnAliasedQueueHasSubmitted)
{
    ContextStorage storage;
    BindContext(storage[0], 0xC0FFEE, kGraphicsTimelineId);
    const ContextsByRole byRole = {&storage[0], &storage[0], &storage[0]};

    SubmitTimes(storage[0], 5);

    QueueTimelineCompletions completions;
    completions.Add(SemaphoreHandle(kGraphicsTimelineId), 5); // GPU has drained everything

    const QueueRetireTags tags = NextSubmitTags(byRole);
    const QueueTimelineProgress roleSpace = RoleSpaceCompletionPairing(byRole, completions);

    // Named rather than left as a bare negation: the compute and transfer clauses compare a
    // counter nothing signals against a count that only grows.
    EXPECT_EQ(roleSpace.computeCompleted, 0ull);
    EXPECT_EQ(roleSpace.computeSubmitted, 5ull);
    EXPECT_FALSE(roleSpace.Retires(tags))
        << "the retire predicate accepted the role/context pairing, so it cannot detect the leak";
}

// The invariant: sampling through the contexts retires the same entry, because both halves of
// each clause come from the queue the work actually ran on.
TEST(VulkanQueueRetireTracking, AliasedRolesRetireAgainstTheQueueTheyRunOn)
{
    ContextStorage storage;
    BindContext(storage[0], 0xC0FFEE, kGraphicsTimelineId);
    const ContextsByRole byRole = {&storage[0], &storage[0], &storage[0]};

    SubmitTimes(storage[0], 5);

    QueueTimelineCompletions completions;
    completions.Add(SemaphoreHandle(kGraphicsTimelineId), 5);

    const QueueRetireTags tags = NextSubmitTags(byRole);
    const QueueTimelineProgress progress = SampleQueueProgress(byRole, completions);

    EXPECT_EQ(progress.computeCompleted, 5ull) << "the compute clause must read the queue compute runs on";
    EXPECT_TRUE(progress.Retires(tags))
        << "deferred destroys never retire on a device with no dedicated compute/transfer family";
}

// The mirror case, which needs no missing family at all: one COMPUTE|TRANSFER family serves both
// roles, so they share a context — bound with the compute timeline, because compute binds first.
// The transfer timeline is then the one nothing signals.
TEST(VulkanQueueRetireTracking, SharedComputeTransferFamilyRetiresAgainstTheSharedTimeline)
{
    ContextStorage storage;
    BindContext(storage[0], 0xC0FFEE, kGraphicsTimelineId);
    BindContext(storage[1], 0xBEEF, kComputeTimelineId);
    const ContextsByRole byRole = {&storage[0], &storage[1], &storage[1]};

    SubmitTimes(storage[0], 3);
    SubmitTimes(storage[1], 4);

    QueueTimelineCompletions completions;
    completions.Add(SemaphoreHandle(kGraphicsTimelineId), 3);
    completions.Add(SemaphoreHandle(kComputeTimelineId), 4);

    const QueueRetireTags tags = NextSubmitTags(byRole);

    EXPECT_FALSE(RoleSpaceCompletionPairing(byRole, completions).Retires(tags))
        << "instrument check: the transfer clause must be the one that fails";
    EXPECT_TRUE(SampleQueueProgress(byRole, completions).Retires(tags));
}

// ...and the fix must not simply make the predicate true. A dedicated queue that genuinely lags
// still holds its entries back: no aliasing, real work outstanding, nothing retires.
TEST(VulkanQueueRetireTracking, LaggingDedicatedQueueStillHoldsEntriesBack)
{
    ContextStorage storage;
    BindContext(storage[0], 0xC0FFEE, kGraphicsTimelineId);
    BindContext(storage[1], 0xBEEF, kComputeTimelineId);
    BindContext(storage[2], 0xFEED, kTransferTimelineId);
    const ContextsByRole byRole = {&storage[0], &storage[1], &storage[2]};

    SubmitTimes(storage[0], 4);
    SubmitTimes(storage[1], 3);

    QueueTimelineCompletions completions;
    completions.Add(SemaphoreHandle(kGraphicsTimelineId), 4); // drained
    completions.Add(SemaphoreHandle(kComputeTimelineId), 1);  // two submits still executing
    completions.Add(SemaphoreHandle(kTransferTimelineId), 0); // idle

    const QueueTimelineProgress progress = SampleQueueProgress(byRole, completions);

    // Tagged against compute's second submit, which the GPU has not reached.
    const QueueRetireTags tags = {5, 2, 1};
    EXPECT_FALSE(progress.Retires(tags)) << "an entry was freed while the compute queue was still running it";

    // The idle transfer role retires vacuously: completed 0 >= submitted 0.
    EXPECT_TRUE(progress.Retires({5, 1, 1}));
}

// Staging buffers and command buffers belong to ONE submit on one queue, so they carry that
// submit's (timeline, value) rather than per-queue tags. Resolving completion by the recorded
// handle is what makes both aliasing shapes safe at once; a role-derived bool cannot, because
// the same "transfer" answer has to mean three different timelines.
TEST(VulkanQueueRetireTracking, DeferredWorkResolvesCompletionByItsRecordedTimeline)
{
    QueueTimelineCompletions completions;
    completions.Add(SemaphoreHandle(kGraphicsTimelineId), 10); // graphics runs well ahead
    completions.Add(SemaphoreHandle(kComputeTimelineId), 4);   // the shared compute+transfer queue

    // A copy submitted through the transfer ROLE onto the shared queue, still executing.
    constexpr uint64_t copyValue = 5;
    uint64_t completed = 0;
    ASSERT_TRUE(completions.TryGet(SemaphoreHandle(kComputeTimelineId), completed));
    EXPECT_EQ(completed, 4ull);
    EXPECT_FALSE(copyValue <= completed) << "staging freed while the copy was still executing";

    // Instrument check on both halves of the bool the handle replaced. "Not the transfer
    // timeline" sent the entry to the graphics counter, which is ahead — an early free; "the
    // transfer timeline" sent it to a counter nothing signals — a permanent leak.
    uint64_t graphicsCompleted = 0;
    ASSERT_TRUE(completions.TryGet(SemaphoreHandle(kGraphicsTimelineId), graphicsCompleted));
    EXPECT_TRUE(copyValue <= graphicsCompleted) << "instrument check: the graphics counter must be ahead";
    uint64_t transferCompleted = 0;
    EXPECT_FALSE(completions.TryGet(SemaphoreHandle(kTransferTimelineId), transferCompleted))
        << "instrument check: nothing signals the transfer timeline in this configuration";

    // Once the shared queue reaches it, the same entry retires.
    completions = {};
    completions.Add(SemaphoreHandle(kComputeTimelineId), 5);
    ASSERT_TRUE(completions.TryGet(SemaphoreHandle(kComputeTimelineId), completed));
    EXPECT_TRUE(copyValue <= completed);
}

// Unbound contexts carry the null handle and must contribute nothing: recording them would let
// an entry resolve against slot 0 of a table that means "no queue".
TEST(VulkanQueueRetireTracking, CompletionsIgnoreUnboundContextsAndUnknownTimelines)
{
    QueueTimelineCompletions completions;
    completions.Add(SemaphoreHandle(), 99);
    completions.Add(SemaphoreHandle(kGraphicsTimelineId), 7);

    uint64_t completed = ~0ull;
    EXPECT_FALSE(completions.TryGet(SemaphoreHandle(), completed));
    EXPECT_FALSE(completions.TryGet(SemaphoreHandle(kTransferTimelineId), completed));
    EXPECT_TRUE(completions.TryGet(SemaphoreHandle(kGraphicsTimelineId), completed));
    EXPECT_EQ(completed, 7ull);

    // Roles pointing at an unbound context read zeros rather than another queue's progress.
    ContextStorage storage;
    const ContextsByRole byRole = {&storage[0], &storage[1], &storage[2]};
    const QueueTimelineProgress progress = SampleQueueProgress(byRole, completions);
    EXPECT_EQ(progress.graphicsCompleted, 0ull);
    EXPECT_EQ(progress.graphicsSubmitted, 0ull);
    EXPECT_TRUE(progress.Retires({0, 0, 0}));
}
