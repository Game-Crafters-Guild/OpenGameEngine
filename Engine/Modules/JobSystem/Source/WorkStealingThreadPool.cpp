#include "JobSystem/WorkStealingThreadPool.h"
#include "BlockingThreads.h"
#include "JobChannelState.h"
#include "JobSystem/TaskDependencyGraph.h"
#include "Platform/Thread.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace JobSystem {

namespace
{
    // Adaptive spin-before-sleep defaults (overridable via env / test hook).
    // A spin window of 0 disables the spin path entirely.
    constexpr uint32 kDefaultSpinWindowUs = 100;
    constexpr uint32 kDefaultMaxSpinners = 2;

    // Spin-loop shape: the steal ring is polled only every Nth iteration (it
    // touches other workers' queue cache lines); the pause ramp caps at
    // kSpinPauseMax back-to-back pause instructions; a scheduler yield is
    // inserted periodically so a spinner never monopolizes a hyperthread pair.
    constexpr uint32 kSpinStealPeriod = 8;
    constexpr uint32 kSpinPauseMax = 32;
    constexpr uint32 kSpinYieldPeriod = 64;

    // Inline-mode stack bound: nested execute-at-publish frames allowed on a
    // thread before further tasks are deferred to the trampoline queue. Deep
    // enough for short chains; joins explicitly drain deferred work so reaching
    // the bound cannot strand a child. The bound fits the 1 MiB wasm stack.
    constexpr int kMaxInlineExecutionDepth = 64;

    // The occupancy names of the two compute classes. Static literals, so a
    // reader compares by pointer.
    constexpr const char kNormalJobName[] = "Normal";
    constexpr const char kBackgroundJobName[] = "Background";

    struct DeferredInlineTask
    {
        WorkStealingThreadPool* Pool;
        UniquePtr<TaskBase> Task;
        // The class given at publish (or the channel), which the deferred
        // execution runs the body as.
        const char* Channel;
        JobPriority Class;
    };

    struct InlineTrampoline
    {
        int Depth = 0;
        std::deque<DeferredInlineTask> Deferred;
    };

    InlineTrampoline& GetInlineTrampoline()
    {
        // The stack budget belongs to the thread. Each pool still owns its
        // queued envelopes and must consume them before it is destroyed.
        thread_local InlineTrampoline trampoline;
        return trampoline;
    }

    uint32 ReadEnvU32(const char* name, uint32 defaultValue)
    {
        if (const char* env = std::getenv(name))
        {
            char* end = nullptr;
            const unsigned long v = std::strtoul(env, &end, 10);
            if (end != env)
            {
                return static_cast<uint32>(v);
            }
        }
        return defaultValue;
    }

    inline void CpuRelax()
    {
#if defined(_MSC_VER)
#if defined(_M_ARM64) || defined(_M_ARM)
        __yield();
#else
        _mm_pause();
#endif
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        asm volatile("yield");
#else
        std::this_thread::yield();
#endif
    }
} // namespace

// Thread-local storage for worker ID and fast access.
// s_CurrentPool is never cleared on thread exit: compute workers and blocking
// threads terminate at their join and are never recycled across pools.
thread_local size_t WorkStealingThreadPool::s_CurrentWorkerId = SIZE_MAX;
thread_local WorkStealingThreadPool::Worker* WorkStealingThreadPool::s_CurrentWorker = nullptr;
thread_local WorkStealingThreadPool* WorkStealingThreadPool::s_CurrentPool = nullptr;
thread_local uint32* WorkStealingThreadPool::s_PublishSink = nullptr;
thread_local WorkStealingThreadPool::ThreadJobState* WorkStealingThreadPool::s_JobState = nullptr;
thread_local WorkStealingThreadPool::ThreadJobState WorkStealingThreadPool::s_OwnJobState;

namespace
{
    // What the occupancy slot and GetCurrentJobForTests name a body by.
    template <typename Context>
    const char* JobName(const Context& context)
    {
        if (context.Channel != nullptr)
        {
            return context.Channel;
        }
        return context.Class == JobPriority::Background ? kBackgroundJobName : kNormalJobName;
    }
} // namespace

namespace
{
    template <typename Slot>
    JobSystemStatistics::Occupancy ReadOccupancy(const Slot& slot, JobSystemStatistics::ThreadKind kind,
                                                 size_t index)
    {
        JobSystemStatistics::Occupancy entry;
        entry.Kind = kind;
        entry.Index = static_cast<uint32>(index);
        entry.Running = slot.Running.load(std::memory_order_relaxed);
        // A Normal job's slot carries no start time of its own.
        const bool timed = entry.Running != nullptr && entry.Running != kNormalJobName;
        entry.SinceNs = timed ? slot.SinceNs.load(std::memory_order_relaxed) : 0;
        return entry;
    }
} // namespace

uint64 WorkStealingThreadPool::SteadyClockNs() {
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

WorkStealingThreadPool::JobBodyScope::JobBodyScope(JobContext context)
    : m_State(CurrentJobState()), m_Enclosing(m_State) {
    // Background and channel jobs are the ones that run long enough for a
    // start time to matter; a Normal job (a 1 us ECS task) never reads the
    // clock.
    const bool timed = context.Channel != nullptr || context.Class == JobPriority::Background;
    m_State.Context = context;
    m_State.SinceNs = timed ? SteadyClockNs() : 0;
    ++m_State.Depth;
    if (OccupancySlot* slot = m_State.Slot) {
        if (timed) {
            slot->SinceNs.store(m_State.SinceNs, std::memory_order_relaxed);
        }
        slot->Running.store(JobName(context), std::memory_order_relaxed);
    }
}

WorkStealingThreadPool::JobBodyScope::~JobBodyScope() {
    m_State.Context = m_Enclosing.Context;
    m_State.SinceNs = m_Enclosing.SinceNs;
    m_State.Depth = m_Enclosing.Depth;
    if (OccupancySlot* slot = m_State.Slot) {
        if (m_Enclosing.Depth == 0) {
            slot->Running.store(nullptr, std::memory_order_relaxed);
            return;
        }
        if (m_Enclosing.SinceNs != 0) {
            slot->SinceNs.store(m_Enclosing.SinceNs, std::memory_order_relaxed);
        }
        slot->Running.store(JobName(m_Enclosing.Context), std::memory_order_relaxed);
    }
}

WorkStealingThreadPool::JobContext WorkStealingThreadPool::CallerContext() {
    const ThreadJobState& state = CurrentJobState();
    return state.Depth > 0 ? state.Context : ComputeContext(JobPriority::Normal);
}

const char* WorkStealingThreadPool::GetCurrentJobForTests() {
    const ThreadJobState& state = CurrentJobState();
    return state.Depth > 0 ? JobName(state.Context) : nullptr;
}

WorkStealingThreadPool::ThreadJobState& WorkStealingThreadPool::CurrentJobState() {
    ThreadJobState* state = s_JobState;
    return state != nullptr ? *state : s_OwnJobState;
}

// Thread-local deferred cleanup buffer. CleanupTaskData() pushes entries
// here without locking; the buffer is drained the next time the same thread
// hits the owning pool's registry mutex (in GetOrCreateTaskData) or when the
// owning pool's share overflows the flush threshold. Reduces mutex-op count
// per task ~2× by batching erases with the unavoidable Submit-side insert.
// The TaskData shared_ptr stays alive in the registry until drain; this is
// fine because TaskIds are monotonic and never revisited, so no one looks up
// a "stale" entry.
//
// Entries are TAGGED with their owning pool and drained ONLY by it. One
// thread legitimately produces cleanups for several pools (sequential test
// pools; cross-pool submits — CrossPoolSubmitTest), and the untagged drain
// used to clear the WHOLE buffer, silently dropping foreign pools' ids and
// leaking their registry entries until pool destruction. The pool pointer is
// an identity token ONLY — never dereferenced — so residue for a destroyed
// pool is inert: Shutdown() purges the shutting-down thread's own entries,
// ids are never reused across pools (m_NextTaskId is process-global), and an
// address-aliased later pool can therefore never match a stale id's entry in
// its registry.
struct PendingCleanupEntry
{
    const WorkStealingThreadPool* Pool;
    TaskId Id;
};
thread_local std::vector<PendingCleanupEntry> t_pendingCleanup;

// Static task ID counter
std::atomic<TaskId> WorkStealingThreadPool::m_NextTaskId{1};

WorkStealingThreadPool::WorkStealingThreadPool(size_t numThreads, size_t blockingThreadBudget) {
    if (numThreads == 0) {
        // Inline mode: no workers, no wake protocol; submissions execute on
        // the submitting thread at the publish points.
        m_InlineMode = true;
        m_DependencyGraph = MakeUnique<TaskDependencyGraph>();
        Logger::Log::Info("JobSystem: WorkStealingThreadPool in inline mode (0 workers)");
        return;
    }

    // Env knobs are read once, before any worker starts. Tests override via
    // SetSpinConfigForTest instead of mutating process env.
    m_SpinWindowUs.store(ReadEnvU32("GE_JOB_SPIN_US", kDefaultSpinWindowUs), std::memory_order_relaxed);
    m_MaxSpinners.store(ReadEnvU32("GE_JOB_MAX_SPINNERS", kDefaultMaxSpinners), std::memory_order_relaxed);

    m_DependencyGraph = MakeUnique<TaskDependencyGraph>();
    m_BlockingThreads = MakeUnique<Detail::BlockingThreads>(*this, blockingThreadBudget);

    // The slabs ParallelFor can hold at once on this pool, provisioned before
    // the first fork so that no fork takes a heap allocation: the helper
    // envelopes (kMaxUnstartedClaimHelpersPerWorker unstarted in each of the
    // two lanes and one running, per worker), a run block for each of them,
    // and the forking caller's block. Each further caller forking at the same
    // time adds one block.
    const size_t claimHelperEnvelopes = (2 * kMaxUnstartedClaimHelpersPerWorker + 1) * numThreads;
    Detail::ProvisionTaskSlabs(2 * claimHelperEnvelopes + 1);
    // Every record exists before any thread that writes one: the workers
    // start below, blocking threads on a channel's first dispatch.
    m_BlockingRecordCount = blockingThreadBudget;
    m_ThreadRecords.reset(new GameEngine::FalseSharingPadded<ThreadRecord>[numThreads + blockingThreadBudget]);
    for (size_t i = 0; i < numThreads + blockingThreadBudget; ++i) {
        ThreadRecord& record = m_ThreadRecords[i].Value;
        record.State.Slot = &record.Occupancy;
    }

    // Initialize workers using unique_ptr to avoid copy constructor issues.
    // Workers take *this for their global-lane ConsumerTokens; both lanes
    // are fully constructed before this line.
    workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back(MakeUnique<Worker>(*this));
    }

    // Start worker threads with dependency-aware loop
    for (size_t i = 0; i < numThreads; ++i) {
        workers[i]->Thread = std::thread(&WorkStealingThreadPool::DependencyAwareWorkerLoop, this, i);
    }

    Logger::Log::Info("JobSystem: WorkStealingThreadPool initialized with {} worker threads", numThreads);
}

WorkStealingThreadPool::~WorkStealingThreadPool() {
#ifndef NDEBUG
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        assert(m_Channels.empty() &&
               "JobSystem: a JobChannel outlives its pool; its owning service must destroy it first");
    }
#endif
    Shutdown();
}

TaskHandle WorkStealingThreadPool::Submit(UniquePtr<Task> task) {
    if (shutdown_requested.load()) {
        return TaskHandle(); // Return invalid handle
    }

    // Use the task's existing ID to maintain dependency relationships
    TaskId taskId = task->GetTaskId();

    // The TaskData must exist before the task is published: completion on a
    // worker bridges status into it via ProcessGraphActions.
    TaskHandle handle(taskId, this);

    // Brand the id graph-managed BEFORE the handle can escape (it escapes at
    // return): LinkSubmitDependency dispatches on this bit, not on graph-node
    // presence — see TaskData::GraphManaged for the cascade-retire window
    // this closes.
    if (auto data = handle.GetTaskData()) {
        data->GraphManaged.store(true, std::memory_order_relaxed);
    }

    // Slice 6: every Task subclass is graph-managed by construction —
    // dependency-exempt envelope types derive from TaskBase and never reach
    // this API (Submit(F&&) / EnqueueWork own those paths).

    // F9 submit-side critical section: node creation, edge registration
    // (absent dependency = already terminal, F8), readiness check, and
    // parking are ONE atomic step under the graph mutex. If the last
    // dependency completes concurrently, exactly one side observes the task
    // as ready — either Register returns it here, or the completer's
    // MarkCompleted extracts the parked payload (closes the B8
    // submit-vs-complete TOCTOU without any rescan).
    if (UniquePtr<Task> ready = m_DependencyGraph->Register(std::move(task))) {
        EnqueueTask(std::move(ready));
    }

    // F13e: same race as above, with one more shape — the payload can be
    // PARKED in the graph (never queued, invisible to any drain) after
    // Shutdown()'s CancelAllPending already ran. AbandonSubmitDuringShutdown's
    // direct arbitration covers it: CancelPending retires the parked payload.
    if (IsShuttingDown()) {
        AbandonSubmitDuringShutdown(taskId);
    }

    return handle;
}

bool WorkStealingThreadPool::CancelTask(TaskId taskId) {
    // Lookup only — never create. An absent registry entry means the id was
    // never submitted here or the task is terminal with its deferred cleanup
    // already drained; both refuse cancellation.
    auto data = TryGetTaskData(taskId);
    if (!data) {
        return false;
    }

    return CancelPendingArbitrated(taskId, data);
}

bool WorkStealingThreadPool::CancelPendingArbitrated(TaskId taskId,
                                                     const SharedPtr<TaskHandle::TaskData>& data) {
    // F12: the TaskData status atomic is the SINGLE cancel/execute arbiter.
    // The executing worker CASes Pending -> Running at dequeue; we CAS
    // Pending -> Cancelled here. Exactly one side wins; the winner owns the
    // task's terminal events.
    TaskStatus expected = TaskStatus::Pending;
    if (!data->Status.compare_exchange_strong(expected, TaskStatus::Cancelled,
                                              std::memory_order_seq_cst)) {
        return false; // running or already terminal — the other side fires
    }

    // We won: fire the Cancelled event set on THIS thread, outside every lock
    // (F10). For graph-tracked tasks the graph critical section retires the
    // parked payload (if any), cascades pending dependents, and erases the
    // nodes; lambda-path tasks are never graph-tracked and CancelPending is a
    // no-op for them.
    TaskGraphActions actions;
    m_DependencyGraph->CancelPending(taskId, actions);

    // The winner's own fire goes to the EXACT TaskData it arbitrated on —
    // never a by-id handle lookup. The losing worker can dequeue-drop the
    // envelope and queue its registry cleanup concurrently; once another
    // thread drains that cleanup, a by-id lookup re-materializes a fresh
    // Pending TaskData and the notify/callbacks would fire on the wrong
    // object, stranding real waiters. Cascaded dependents (below) are safe to
    // resolve by id: their envelopes sit in actions.RetiredTasks until after
    // the events fire, so their registry entries cannot be cleaned yet.
    std::erase_if(actions.Events, [taskId](const TaskGraphActions::StatusEvent& event) {
        return event.Id == taskId;
    });
    TaskHandle::FireCancelledOn(data);
    ProcessGraphActions(actions, taskId, String{}, nullptr);
    return true;
}

bool WorkStealingThreadPool::LinkSubmitDependency(const TaskHandle& dependency) {
    if (!dependency.IsValid()) {
        return false; // matches Task::AddDependency(const TaskHandle&): invalid = no edge
    }
    // A foreign pool's id can never complete in THIS graph — an edge to it
    // would park the dependent forever. Contract violation; skip the edge.
    assert(dependency.m_JobSystem == this &&
           "Submit dependency handle belongs to another pool");
    if (dependency.m_JobSystem != this) {
        return false;
    }
    const SharedPtr<TaskHandle::TaskData>& data = dependency.m_Data;
    if (!data || data->IsDone()) {
        // F8: terminal dependencies add no edge — the dependent runs and
        // observes the outcome through its handle, exactly like an absent
        // dependency at Register. Skipping (rather than registering an id
        // with no tombstone) also keeps the F7 diagnostic quiet for
        // already-terminal lambda handles, which the graph never saw.
        return false;
    }

    const TaskId depId = dependency.GetId();
    // GRAPH task (branded at Submit(UniquePtr<Task>)): add a plain edge and
    // never touch the mirror machinery — its completion reaches the graph
    // natively (CompleteGraphTask), and a bridged completion would steal the
    // executing worker's own MarkCompleted, muting its handle events. The
    // dependent's Register decides node-present (edge) vs node-absent
    // (terminal, F8) atomically under the graph mutex — the ordinary B8
    // shape. Dispatching on the BIT rather than probing the graph matters:
    // a cascade-retired graph task (failure/cancel cascade, shutdown
    // CancelAllPending) has NO node but stays Pending on its TaskData until
    // ProcessGraphActions flips it OUTSIDE the graph mutex — probing in
    // that window would create a mirror for a task whose completion path is
    // already spent, and the parked dependent would strand forever (nothing
    // ever completes a graph task's mirror). In the window, Register's
    // absent=terminal reading runs the dependent (identical to the subclass
    // AddDependency path's pre-existing semantics for this exact window).
    if (data->GraphManaged.load(std::memory_order_relaxed)) {
        return true;
    }

    // NON-graph task (plain Submit(F&&) lambda): bridge it with a mirror
    // node. EnsureTracked returning false means an earlier Submit(fn, deps)
    // already mirrors this id — the creator arms the flag and resolves the
    // Dekker recheck; this caller only needs the edge.
    if (!m_DependencyGraph->EnsureTracked(depId)) {
        return true;
    }

    // A mirror was created. Bridge protocol (seq_cst Dekker, the
    // TaskData::HasCallbacks shape):
    //   linker:    store GraphLinked   -> load Status
    //   completer: exchange Status     -> load GraphLinked
    // At least one side observes the other: either the completer sees the
    // flag and bridges the mirror terminal, or we see the terminal status
    // and resolve it here. Both may fire — MarkCompleted/CancelPending on an
    // already-erased node returns empty actions, so double-bridging is
    // benign (the graph mutex serializes the two).
    data->GraphLinked.store(true, std::memory_order_seq_cst);

    switch (data->Status.load(std::memory_order_seq_cst)) {
    case TaskStatus::Completed:
        CompleteLinkedDependency(depId, true, String{}, nullptr);
        return false;
    case TaskStatus::Failed:
        // The fast path publishes Exception (SetException) and ErrorMessage
        // (inside MarkFailed) BEFORE MarkFailed's status exchange, and our
        // seq_cst load reads from that exchange — both fields are safely
        // readable here. (Only lambda deps reach this arm; graph-managed
        // ids returned above.)
        CompleteLinkedDependency(depId, false, data->ErrorMessage, data->Exception);
        return false;
    case TaskStatus::Cancelled: {
        // The winning canceller fired the dependency's own events already;
        // if its graph CancelPending ran before our mirror existed, the
        // mirror is dangling — retire it with the graph's cancel cascade,
        // suppressing the source's events (single-fire is the canceller's).
        TaskGraphActions actions;
        m_DependencyGraph->CancelPending(depId, actions);
        std::erase_if(actions.Events, [depId](const TaskGraphActions::StatusEvent& event) {
            return event.Id == depId;
        });
        ProcessGraphActions(actions, depId, String{}, nullptr);
        return false;
    }
    default:
        return true; // live (Pending/Running): the edge orders the dependent behind it
    }
}

void WorkStealingThreadPool::CompleteLinkedDependency(TaskId taskId, bool success,
                                                      const String& failureMessage,
                                                      std::exception_ptr failureException) {
    // No status pre-store (CompleteGraphTask's F9 preamble): the source's
    // TaskData is already terminal — that is exactly what licensed this call.
    TaskGraphActions actions = m_DependencyGraph->MarkCompleted(taskId, success);
    // The source's own terminal events already fired on its fast path
    // (MarkCompleted/MarkFailed) — only the graph-side effects (dependent
    // releases, failure cascades) are ours. Without the strip, ProcessGraph-
    // Actions would re-run the source's user callbacks.
    std::erase_if(actions.Events, [taskId](const TaskGraphActions::StatusEvent& event) {
        return event.Id == taskId;
    });
    ProcessGraphActions(actions, taskId, failureMessage, failureException);
}

void WorkStealingThreadPool::AbandonSubmitDuringShutdown(TaskId taskId) {
    // F13e: this submitter passed the gate, published, and then observed
    // shutdown — Shutdown()'s drain/graph-retire/registry-sweep may all have
    // completed before the publish landed, so nothing else will ever consume
    // the envelope or flip the TaskData. Self-drain first (consumes and
    // cancels the queued envelope, ours included, plus any other stranded
    // work), then arbitrate our own task directly: the drain only reaches
    // QUEUED envelopes, so a payload parked in the graph needs the explicit
    // CancelPendingArbitrated (whose CancelPending retires it). If a racing
    // drain or the registry sweep already fired our task, the CAS inside
    // loses and this is a no-op — single-fire is preserved.
    DrainQueuesOnThisThread();
    if (auto data = TryGetTaskData(taskId)) {
        CancelPendingArbitrated(taskId, data);
    }
}

SharedPtr<TaskHandle::TaskData> WorkStealingThreadPool::GetOrCreateTaskData(TaskId taskId) {
    SharedPtr<TaskHandle::TaskData> result;
    SharedPtr<TaskHandle::TaskData> recycle[kPendingCleanupBatch];
    std::vector<SharedPtr<TaskHandle::TaskData>> deferDrop; // rarely allocates
    size_t recycled = 0;
    {
        std::lock_guard<std::mutex> lock(m_TaskDataMutex);

        // Drain deferred cleanups under the same lock as the insert. This is
        // amortized free for callers that hit the lock anyway; the per-task
        // CleanupTaskData() fast path stays lock-free.
        recycled = DrainPendingCleanupLocked(recycle, deferDrop);

        auto it = m_TaskDataRegistry.find(taskId);
        if (it != m_TaskDataRegistry.end()) {
            result = it->second;
        } else {
            // Freelist entries are pristine (reset in RecycleTaskData).
            result = AcquireTaskData();
            m_TaskDataRegistry[taskId] = result;
        }
    }
    // Reset + pool the recycled entries OUTSIDE the registry lock (their
    // stale results/callbacks can run user destructors). deferDrop's shared
    // entries drop their possibly-last references at scope exit, also
    // outside the lock.
    CommitRecycledTaskData(recycle, recycled);
    return result;
}

size_t WorkStealingThreadPool::DrainPendingCleanupLocked(
    SharedPtr<TaskHandle::TaskData> (&recycle)[kPendingCleanupBatch],
    std::vector<SharedPtr<TaskHandle::TaskData>>& deferDrop) {
    size_t recycled = 0;
    // Consume only THIS pool's entries; foreign pools' entries stay parked
    // for their own drains (pointer equality only — see the buffer comment).
    // erase_if keeps the vector's capacity, like the pre-tag clear() did
    // (allocation diet). No user code runs inside the predicate (every
    // erased entry's ref is moved out first), so the buffer cannot be
    // mutated reentrantly while erase_if iterates.
    std::erase_if(t_pendingCleanup, [&](const PendingCleanupEntry& entry) {
        if (entry.Pool != this) {
            return false;
        }
        auto it = m_TaskDataRegistry.find(entry.Id);
        if (it == m_TaskDataRegistry.end()) {
            return true; // duplicate cleanup — a prior drain already erased it
        }
        // use_count() == 1 means the registry holds the LAST reference: every
        // new reference is minted under m_TaskDataMutex (GetOrCreateTaskData,
        // TryGetTaskData, the shutdown sweep's snapshot) or copied from an
        // already-existing handle/envelope — at count 1 neither source
        // exists, so the count cannot rise concurrently and the object is
        // exclusively ours to recycle (slice 6: restores the TaskData
        // Release() optimisation with an exclusivity proof).
        if (recycled < kPendingCleanupBatch && it->second.use_count() == 1) {
            recycle[recycled++] = std::move(it->second);
        } else {
            // The registry's reference may become the LAST one between the
            // use_count() read above and the erase — handles drop their
            // references WITHOUT this mutex, so "a count>1 erase only drops
            // a reference" is not guaranteed. Destroying a TaskData runs
            // user destructors (stale std::any results, callback
            // std::functions), which must never happen under
            // m_TaskDataMutex: a destructor that re-enters the pool would
            // self-deadlock. Move the ref out and drop it after unlock
            // (audit fix).
            deferDrop.push_back(std::move(it->second));
        }
        m_TaskDataRegistry.erase(it);
        return true;
    });
    // use_count() is a RELAXED load (this is why shared_ptr::unique() was
    // deprecated): observing the dying owner's 2->1 decrement above gives no
    // happens-before with that owner's earlier writes to TaskData fields
    // (MarkFailed's ErrorMessage buffer, the std::any Result). RecycleTaskData
    // mutates those same fields, so pair with the owner's release-decrement
    // here. Free on x86 (the lock-prefixed decrement is already ordered);
    // required on ARM64 — the macOS Editor ships.
    if (recycled > 0) {
        std::atomic_thread_fence(std::memory_order_acquire);
    }
    return recycled;
}

void WorkStealingThreadPool::CommitRecycledTaskData(
    SharedPtr<TaskHandle::TaskData> (&recycle)[kPendingCleanupBatch], size_t count) {
    for (size_t i = 0; i < count; ++i) {
        RecycleTaskData(std::move(recycle[i]));
    }
}

moodycamel::ConcurrentQueue<SharedPtr<TaskHandle::TaskData>>&
WorkStealingThreadPool::TaskDataFreelist() {
    // IMMORTAL for the same reason as the slab freelist (TaskSlabPool.cpp):
    // recycles can run during static destruction; a destructed freelist would
    // UAF. The constructed capacity is the retention cap — try_enqueue never
    // allocates new blocks, so at most kTaskDataFreelistCapacity pristine
    // TaskData (~500B each) are ever pinned.
    static constexpr size_t kTaskDataFreelistCapacity = 256;
    static auto* const s_Freelist =
        new moodycamel::ConcurrentQueue<SharedPtr<TaskHandle::TaskData>>(
            kTaskDataFreelistCapacity);
    return *s_Freelist;
}

SharedPtr<TaskHandle::TaskData> WorkStealingThreadPool::AcquireTaskData() {
    SharedPtr<TaskHandle::TaskData> data;
    if (TaskDataFreelist().try_dequeue(data)) {
        return data;
    }
    return JobSystem::MakeShared<TaskHandle::TaskData>();
}

void WorkStealingThreadPool::RecycleTaskData(SharedPtr<TaskHandle::TaskData> data) {
    if (!data) {
        return;
    }
    // Reset to pristine BEFORE pooling. Runs outside the registry mutex
    // (CommitRecycledTaskData) because destroying the stale result/callback
    // set can run user destructors.
    data->Status.store(TaskStatus::Pending, std::memory_order_relaxed);
    // No waiter can exist here: a live Wait() holds its own SharedPtr ref for
    // the whole wait, which the drain's use_count()==1 exclusivity proof
    // excludes. The store is belt-and-braces for the pooled reuse.
    assert(data->Waiters.load(std::memory_order_relaxed) == 0 &&
           "JobSystem: recycling a TaskData with a registered waiter");
    data->Waiters.store(0, std::memory_order_relaxed);
    data->Result.reset();
    data->Exception = nullptr;
    data->ErrorMessage.clear();
    data->OnCompleteCallbacks.clear();
    data->OnCompleteWithHandleCallbacks.clear();
    data->OnFailureCallbacks.clear();
    data->HasCallbacks.store(false, std::memory_order_relaxed);
    data->GraphLinked.store(false, std::memory_order_relaxed);
    data->GraphManaged.store(false, std::memory_order_relaxed);

    // Bounded, non-allocating: over-cap entries simply die here (the
    // last-owner destructor reclaims them).
    TaskDataFreelist().try_enqueue(std::move(data));
}

SharedPtr<TaskHandle::TaskData> WorkStealingThreadPool::TryGetTaskData(TaskId taskId) const {
    std::lock_guard<std::mutex> lock(m_TaskDataMutex);
    auto it = m_TaskDataRegistry.find(taskId);
    return it != m_TaskDataRegistry.end() ? it->second : nullptr;
}

TaskId WorkStealingThreadPool::GenerateTaskId() {
    return m_NextTaskId.fetch_add(1, std::memory_order_relaxed);
}

size_t WorkStealingThreadPool::GetGraphTaskCountForTests() const {
    return m_DependencyGraph->GetTrackedTaskCountForTests();
}

void WorkStealingThreadPool::SetSpinConfigForTest(uint32 spinWindowUs, uint32 maxSpinners) {
    m_SpinWindowUs.store(spinWindowUs, std::memory_order_relaxed);
    m_MaxSpinners.store(maxSpinners, std::memory_order_relaxed);
}

uint32 WorkStealingThreadPool::GetSpinningCountForTests() const {
    return m_NumSpinning.load(std::memory_order_seq_cst);
}

uint32 WorkStealingThreadPool::GetSleepingCountForTests() const {
    return m_NumSleeping.load(std::memory_order_seq_cst);
}

uint64 WorkStealingThreadPool::GetBatchNotifyCountForTests() const {
    return m_BatchNotifiesForTests.load(std::memory_order_relaxed);
}

size_t WorkStealingThreadPool::GetTaskDataRegistrySizeForTests() const {
    std::lock_guard<std::mutex> lock(m_TaskDataMutex);
    return m_TaskDataRegistry.size();
}

uint32 WorkStealingThreadPool::GetBackgroundQueuedForTests() const {
    return m_BackgroundQueued.load(std::memory_order_relaxed);
}

void WorkStealingThreadPool::SetSleepBackstopForTest(uint32 backstopUs) {
    m_SleepBackstopUs.store(backstopUs, std::memory_order_relaxed);
}

JobSystemStatistics WorkStealingThreadPool::GetStatistics() const {
    JobSystemStatistics stats;
    stats.ComputeWorkers = static_cast<uint32>(workers.size());
    stats.BlockingThreads = m_BlockingThreads ? m_BlockingThreads->ThreadCount() : 0;

    stats.Threads.resize(workers.size() + m_BlockingRecordCount);
    stats.Threads.resize(SnapshotOccupancy(stats.Threads));
    for (const JobSystemStatistics::Occupancy& thread : stats.Threads) {
        if (thread.Kind != JobSystemStatistics::ThreadKind::Worker || thread.Running == nullptr) {
            continue;
        }
        ++(thread.Running == kBackgroundJobName ? stats.Background : stats.Normal).Running;
    }
    // m_QueuedTasks counts both lanes; the Background occupancy counter can
    // run ahead of it during a producer's publish window, hence the floor.
    const uint32 queued = m_QueuedTasks.load(std::memory_order_relaxed);
    const uint32 background = m_BackgroundQueued.load(std::memory_order_relaxed);
    stats.Normal.Queued = queued > background ? queued - background : 0;
    stats.Background.Queued = background;

    // Strong references under the registry mutex, each channel read under
    // its own mutex outside it: a channel's mutex is a leaf.
    Vector<SharedPtr<Detail::JobChannelState>> channels;
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        channels.reserve(m_Channels.size());
        for (const WeakPtr<Detail::JobChannelState>& channel : m_Channels) {
            if (SharedPtr<Detail::JobChannelState> live = channel.lock()) {
                channels.push_back(std::move(live));
            }
        }
    }
    stats.Channels.reserve(channels.size());
    for (const SharedPtr<Detail::JobChannelState>& channel : channels) {
        stats.Channels.push_back(channel->Statistics());
    }

    stats.GlobalPushes = m_Census.GlobalPushes.load(std::memory_order_relaxed);
    stats.LocalPushes = m_Census.LocalPushes.load(std::memory_order_relaxed);
    stats.BackgroundPushes = m_Census.BackgroundPushes.load(std::memory_order_relaxed);
    stats.GlobalPops = m_Census.GlobalPops.load(std::memory_order_relaxed);
    stats.LocalPops = m_Census.LocalPops.load(std::memory_order_relaxed);
    stats.StealPops = m_Census.StealPops.load(std::memory_order_relaxed);
    stats.BackgroundPops = m_Census.BackgroundPops.load(std::memory_order_relaxed);
    stats.StealMisses = m_Census.StealMisses.load(std::memory_order_relaxed);
    stats.EmptyLoopsWithBacklog = m_Census.EmptyLoopsWithBacklog.load(std::memory_order_relaxed);
    stats.BackstopTimeouts = m_Census.BackstopTimeouts.load(std::memory_order_relaxed);
    stats.TasksExecuted = stats.GlobalPops + stats.LocalPops + stats.StealPops + stats.BackgroundPops +
                          (m_BlockingThreads ? m_BlockingThreads->JobsRun() : 0);
    return stats;
}

size_t WorkStealingThreadPool::SnapshotOccupancy(std::span<JobSystemStatistics::Occupancy> out) const {
    if (!m_ThreadRecords) {
        return 0;
    }
    size_t written = 0;
    for (size_t i = 0; i < workers.size() && written < out.size(); ++i) {
        out[written++] = ReadOccupancy(m_ThreadRecords[i].Value.Occupancy, JobSystemStatistics::ThreadKind::Worker, i);
    }
    for (size_t i = 0; i < m_BlockingRecordCount && written < out.size(); ++i) {
        const OccupancySlot& slot = m_ThreadRecords[workers.size() + i].Value.Occupancy;
        if (slot.Live.load(std::memory_order_relaxed)) {
            out[written++] = ReadOccupancy(slot, JobSystemStatistics::ThreadKind::Blocking, i);
        }
    }
    return written;
}

void WorkStealingThreadPool::BindBlockingThread(size_t index) {
    s_CurrentPool = this;
    if (index < m_BlockingRecordCount) {
        ThreadRecord& record = m_ThreadRecords[workers.size() + index].Value;
        s_JobState = &record.State;
        record.Occupancy.Live.store(true, std::memory_order_relaxed);
    }
}

uint64 WorkStealingThreadPool::GetPublishedJobCount() const {
    return m_Census.GlobalPushes.load(std::memory_order_relaxed) +
           m_Census.LocalPushes.load(std::memory_order_relaxed) +
           m_Census.BackgroundPushes.load(std::memory_order_relaxed);
}

WorkStealingThreadPool::PublishCountScope::PublishCountScope(uint32& sink)
    : m_Enclosing(s_PublishSink) {
    s_PublishSink = &sink;
}

WorkStealingThreadPool::PublishCountScope::~PublishCountScope() {
    s_PublishSink = m_Enclosing;
}

void WorkStealingThreadPool::CountPublishedOnThisThread(size_t count) {
    if (uint32* sink = s_PublishSink) {
        *sink += static_cast<uint32>(count);
    }
}

size_t WorkStealingThreadPool::GetCurrentWorkerId() const {
    return s_CurrentWorkerId;
}

bool WorkStealingThreadPool::IsComputeWorkerOf(const WorkStealingThreadPool* pool) {
    return pool != nullptr && s_CurrentPool == pool && s_CurrentWorkerId != SIZE_MAX;
}

void WorkStealingThreadPool::Wait(JobCounter& counter) {
    if (m_InlineMode)
        DrainInlineTasksOnThisThread();

    // Every zero observation that lets this function RETURN is followed by an
    // empty lock/unlock of the counter mutex (or happens under it): zero is
    // only ever published from inside a Decrement critical section, so the
    // lock orders the last notifier's completion before the caller can
    // destroy the (typically stack-owned) counter — the verbatim-binding
    // stack-lifetime rationale in JobCounter.h.
    auto synchronizedZero = [&counter] {
        if (!counter.IsZero()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(counter.m_Mutex);
        return true;
    };

    const bool canBlock = GameEngine::Platform::CanBlockCurrentThread();
    const bool participant = (s_CurrentPool == this && s_CurrentWorkerId != SIZE_MAX) || !canBlock;
    if (!participant) {
        // Preserve the native external-wait contract: jobs run on workers.
        if (synchronizedZero()) {
            return;
        }
        std::unique_lock<std::mutex> lock(counter.m_Mutex);
        ++counter.m_Waiters;
        counter.m_Cv.wait(lock, [&counter] {
            return counter.m_Count.load(std::memory_order_acquire) == 0;
        });
        --counter.m_Waiters;
        return;
    }

    // Workers and host threads that cannot park execute ONLY tasks tagged
    // with this counter. The pool queues and wake protocol stay untouched.
    JobCounter::TaggedJobsRef jobs;
    {
        std::lock_guard<std::mutex> lock(counter.m_Mutex);
        jobs = counter.m_Jobs;
    }
    for (;;) {
        if (synchronizedZero()) {
            return;
        }
        if (jobs) {
            // A tagged job run here is a body of the waiter's own current
            // context: the class of the body that waits, Normal outside one.
            JobBodyScope body(CallerContext());
            if (JobCounter::RunOneTagged(*jobs)) {
                continue;
            }
        }

        // Nothing findable (RunOneTagged returned false — tagged occupancy
        // provably zero at that instant): the remaining same-counter tasks
        // are in-flight on other threads, or a Run() is mid-publish. Park on
        // the COUNTER's condvar. The predicate re-reads m_Jobs under the
        // mutex (the queue can be created by a concurrent first Run()) and
        // tests the OCCUPANCY count only — the actual pop happens outside
        // the lock via RunOneTagged's retry loop on the next iteration, so
        // moodycamel's spurious dequeue failures can neither strand the park
        // nor spin under the mutex. Paired with Run()'s publish critical
        // section — occupancy increment, tagged enqueue, and notify all under
        // this same mutex — this closes the publish-vs-park window (§7
        // Hazard-B shape): either the predicate sees the fresh occupancy, or
        // the waiter is already atomically blocked when the publish notify
        // lands.
        {
            std::unique_lock<std::mutex> lock(counter.m_Mutex);
            ++counter.m_Waiters;
            const auto parkPredicate = [&counter, &jobs] {
                if (counter.m_Count.load(std::memory_order_acquire) == 0) {
                    return true;
                }
                jobs = counter.m_Jobs;
                return jobs && jobs->JobCount.load(std::memory_order_acquire) > 0;
            };
            if (canBlock) {
                // Keep the native worker wait free of timeout bookkeeping.
                counter.m_Cv.wait(lock, parkPredicate);
            } else {
                // A stalled host-thread wait stops frames and input. Report
                // progress directly even when the logger's worker is busy.
                int parkedIntervals = 0;
                while (!counter.m_Cv.wait_for(lock, std::chrono::seconds(10), parkPredicate)) {
                    ++parkedIntervals;
                    std::fprintf(stderr,
                                 "JobSystem: host-thread wait has lasted %ds with %d task(s) "
                                 "outstanding and none available for this waiter.\n",
                                 parkedIntervals * 10,
                                 static_cast<int>(counter.m_Count.load(std::memory_order_relaxed)));
                    std::fflush(stderr);
                }
            }
            --counter.m_Waiters;
        }
    }
}

void WorkStealingThreadPool::Shutdown() {
    if (shutdown_requested.exchange(true)) {
        return; // Already shutdown
    }

    // Shutdown order (binding: each step depends on the previous):
    //   1. GATE: the flag above refuses Submit and JobChannel::Submit, makes
    //      EnqueueWork self-drain, sends ParallelFor down
    //      its caller-only path, and stops every blocking thread from
    //      taking further work (each finishes the job it holds).
    //   2. JOIN COMPUTE WORKERS: they stop taking new work (loop-top check),
    //      finish their in-flight task, and exit.
    //   3. RELEASE: one drain over the pool queues, every registered
    //      channel's FIFO and the blocking FIFO together (handle and graph
    //      tasks are Cancelled, then bare tasks EXECUTE: they release caller
    //      barriers, dropping them is a hang), then the graph retire
    //      (dependency-parked payloads, which no drain can see, are retired
    //      with Cancelled events) and the registry sweep (any remaining
    //      non-terminal TaskData is marked Cancelled and its waiters
    //      notified; GetOrCreateTaskData is the sole TaskData factory, so this
    //      reaches every TaskHandle::Wait whose TaskData existed at the
    //      sweep's snapshot). A blocking job may be parked on a compute handle
    //      or on another channel's handle; after this step every such handle
    //      is terminal or running on a blocking thread.
    //   4. JOIN BLOCKING THREADS: possible only now; joining them in step 2
    //      would wait forever on a blocking job parked on a handle that only
    //      step 3 releases.
    //   5. RELEASE AGAIN: a blocking job that finished in step 4 may have
    //      published pool work (and self-drained it, F13c/F13e) or released
    //      graph dependents.
    // A Submit racing the ENTIRE sequence can insert its TaskData after the
    // last sweep's snapshot; that case is closed on the submitter's side by
    // the post-publish re-check (self-drain + direct cancel), and a channel
    // reads the gate under the mutex of the FIFO it appends to, so its job
    // either precedes a drain or is handed back.

    // Notify all workers to stop. The empty lock/unlock pairs with the
    // sleeper's increment+predicate evaluation under the same mutex (§7
    // Hazard B): either a sleeper's predicate observes the flag, or it is
    // already atomically blocked inside wait_for and receives the notify.
    // An unlocked notify_all here can race a worker between its predicate
    // and its block, stranding it until the backstop timeout.
    {
        std::lock_guard<std::mutex> lk(globalMutex);
    }
    globalCondition.notify_all();
    if (m_BlockingThreads) {
        m_BlockingThreads->WakeForShutdown();
    }

    // Step 2: wait for all compute workers to finish.
    for (auto& worker : workers) {
        if (worker->Thread.joinable()) {
            worker->Thread.join();
        }
    }

    // Step 3.
    ReleaseQueuedAndParkedWork();

    // Steps 4 and 5.
    if (m_BlockingThreads) {
        m_BlockingThreads->Join();
        ReleaseQueuedAndParkedWork();
    }

#if GE_DEBUG_INSTRUMENTATION
    // F4: spinner tokens are RAII-guarded; with every worker joined, none may
    // remain armed.
    assert(m_NumSpinning.load(std::memory_order_seq_cst) == 0 &&
           "JobSystem: spinner token leaked past worker join");

    // F20 conservation tripwire: with the post-join drain done, every queue
    // must be empty and both counters zero.
    //
    // Racers that can still mutate the counters mid-observation: a publisher
    // between its counter increments (F5 puts them BEFORE the push) and its
    // push, or a racing self-drain between a physical dequeue and its
    // AccountDequeuedTask. Three disarms cover them:
    //  - m_CounterRacersInFlight sampled FIRST and LAST catches a racer that
    //    is INSIDE its bracket at either sample. It does NOT span the whole
    //    mutation window: a publisher's bracket ends at its push while the
    //    counters stay elevated until some drain consumes the task, and a
    //    racer can enter and exit entirely between the two samples.
    //  - m_CounterBracketEpoch (bumped on every bracket ENTRY) compared
    //    across the full read sequence catches exactly that fully-contained
    //    racer — e.g. a publish plus its complete self-drain landing between
    //    the counter snapshot and the second queue check.
    //  - Queue emptiness snapshotted on both sides of the counter reads
    //    catches a completed racy publish that nothing consumed.
    // A racy envelope left in a queue is freed by the queue destructors; its
    // waiters were woken by the registry sweep above (or by the racing
    // submitter's own F13e cancel).
    auto allQueuesEmpty = [this] {
        bool empty = m_GlobalQueue.size_approx() == 0 && m_BackgroundQueue.size_approx() == 0;
        for (const auto& worker : workers) {
            empty = empty && worker->LocalQueue.size_approx() == 0;
        }
        return empty;
    };
    const uint64 epochBefore = m_CounterBracketEpoch.load(std::memory_order_seq_cst);
    const uint32 racersBefore = m_CounterRacersInFlight.load(std::memory_order_seq_cst);
    const bool queuesEmptyBefore = allQueuesEmpty();
    [[maybe_unused]] const uint32 queuedSnapshot = m_QueuedTasks.load(std::memory_order_seq_cst);
    [[maybe_unused]] const uint32 backgroundSnapshot =
        m_BackgroundQueued.load(std::memory_order_seq_cst);
    [[maybe_unused]] const size_t pendingSnapshot = pendingTasks.load(std::memory_order_seq_cst);
    const bool queuesEmptyAfter = allQueuesEmpty();
    const uint32 racersAfter = m_CounterRacersInFlight.load(std::memory_order_seq_cst);
    const uint64 epochAfter = m_CounterBracketEpoch.load(std::memory_order_seq_cst);
    if (racersBefore == 0 && racersAfter == 0 && epochBefore == epochAfter &&
        queuesEmptyBefore && queuesEmptyAfter) {
        assert(queuedSnapshot == 0 &&
               "JobSystem: m_QueuedTasks out of conservation (queues empty, no tasks in flight)");
        assert(backgroundSnapshot == 0 &&
               "JobSystem: m_BackgroundQueued out of conservation (queues empty, no tasks in flight)");
        assert(pendingSnapshot == 0 &&
               "JobSystem: pendingTasks out of conservation (queues empty, no tasks in flight)");
    }
#endif

    // This thread's deferred-cleanup residue for THIS pool is meaningless now
    // (the registry dies with the pool) — purge it so sequential pools on one
    // thread never accumulate dead-pool entries. Residue held by OTHER
    // threads is inert by construction (pointer used for equality only, ids
    // never reused — see t_pendingCleanup).
    std::erase_if(t_pendingCleanup,
                  [this](const PendingCleanupEntry& entry) { return entry.Pool == this; });
}

void WorkStealingThreadPool::ReleaseQueuedAndParkedWork() {
    DrainQueuesOnThisThread();

    // Retire dependency-parked payloads with Cancelled events.
    {
        TaskGraphActions actions;
        m_DependencyGraph->CancelAllPending(actions);
        ProcessGraphActions(actions, /*sourceTaskId=*/0, String{}, nullptr);
    }

    // Registry sweep. Snapshot under the lock, fire outside it: the fire
    // takes the TaskData's own mutexes and runs user callbacks INLINE on this
    // (shutting-down) thread. Contract: task callbacks must not block on
    // other task handles; a callback that Wait()s here has no workers left to
    // complete its target.
    Vector<SharedPtr<TaskHandle::TaskData>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_TaskDataMutex);
        snapshot.reserve(m_TaskDataRegistry.size());
        for (const auto& entry : m_TaskDataRegistry) {
            snapshot.push_back(entry.second);
        }
    }
    for (auto& data : snapshot) {
        TaskStatus expected = TaskStatus::Pending;
        if (data && data->Status.compare_exchange_strong(expected, TaskStatus::Cancelled,
                                                         std::memory_order_seq_cst)) {
            // Fire on the snapshotted object, not a by-id lookup: a racing
            // self-drain can consume a stranded envelope and clean its
            // registry entry between our snapshot and this fire.
            TaskHandle::FireCancelledOn(data);
        }
    }
}

void WorkStealingThreadPool::DrainQueuesOnThisThread() {
#if GE_DEBUG_INSTRUMENTATION
    // A drain is a counter mutator that can be mid-flight (between a physical
    // dequeue and its AccountDequeuedTask) while another thread's Shutdown
    // tripwire samples — bracket it like the publishers (see
    // m_CounterRacersInFlight / m_CounterBracketEpoch; in-flight first, then
    // epoch).
    m_CounterRacersInFlight.fetch_add(1, std::memory_order_seq_cst);
    m_CounterBracketEpoch.fetch_add(1, std::memory_order_seq_cst);
    struct RacerGuard {
        std::atomic<uint32>& Counter;
        ~RacerGuard() { Counter.fetch_sub(1, std::memory_order_seq_cst); }
    } racerGuard{m_CounterRacersInFlight};
#endif

    // Two-phase per pass: dequeue EVERYTHING first, then cancel every
    // handle/graph envelope, and only then execute the bare tasks. The drain
    // runs bare tasks on the shutting-down thread, which is not a worker, so
    // such a task may Wait() on a handle task queued behind it (on a worker
    // that wait is a Debug assert, see TaskHandle::Wait). No concurrent
    // worker exists to complete that handle task, so executing bare tasks
    // inline as they are dequeued would wedge the drain forever. Cancelling
    // every handle/graph envelope first makes any such Wait() observe
    // Cancelled and return.
    //
    // Counter bookkeeping matches a worker dequeue exactly (F20):
    // m_QueuedTasks releases at the physical dequeue via AccountDequeuedTask,
    // pendingTasks after each task is disposed of (executed or
    // cancelled+freed) — the conservation tripwire must not be able to tell
    // drained tasks from executed ones, and each envelope is consumed once.
    //
    // Re-entrancy: a drained bare task that calls EnqueueWork publishes and
    // then self-drains recursively via the F13c re-check (this thread, fresh
    // pass) — the nested drain applies the same two phases to whatever it can
    // reach, and anything it misses is picked up by this loop's next pass.
    //
    // Channel jobs: the same pass empties every registered channel's FIFO and
    // the blocking threads' FIFO, each under its own mutex, beside the pool
    // queues, because a job of one FIFO may wait on a handle queued in
    // another; emptying them one after another would let phase 3 run a job
    // whose handle dependency is still queued. Those envelopes never touched
    // the pool's counters, so they skip AccountDequeuedTask and the
    // pendingTasks decrement (DrainedTask::PoolCounted). A blocking-FIFO
    // envelope holds its channel slot; destroying it releases the slot, and
    // with the gate set the release promotes nothing.

    // Loop until a full pass over every queue finds nothing AND the size
    // signals agree empty: a racing EnqueueWork (the F13c window) or Submit
    // (F13e) can publish between passes — that caller self-drains too, but
    // this side must not exit while it can still observe work.
    for (;;) {
        // Phase 1: dequeue everything reachable into a local list. Every
        // dequeue in this drain is TOKENLESS: all of a Worker's
        // ConsumerTokens (local queue AND the two global lanes) are
        // thread-affine to that worker's thread and must never be used from
        // this one (same rule as the steal path). The tokenless heuristic's
        // #360 unfairness is irrelevant here — the drain loops until every
        // queue is empty.
        Vector<DrainedTask> drained;
        if (m_InlineMode) {
            auto& deferred = GetInlineTrampoline().Deferred;
            for (auto it = deferred.begin(); it != deferred.end();) {
                if (it->Pool == this) {
                    drained.push_back({std::move(it->Task), /*PoolCounted=*/false,
                                       JobContext{it->Channel, it->Class}});
                    it = deferred.erase(it);
                } else {
                    ++it;
                }
            }
        }
        TakeChannelJobs(drained);
        for (;;) {
            bool found = false;
            UniquePtr<TaskBase> task;
            while (m_GlobalQueue.try_dequeue(task)) {
                // Census pops count PHYSICAL dequeues from a lane, whichever
                // path consumes them — the push/pop conservation identity
                // must hold across a drain too (a participating Wait can
                // leave its no-op stub for the shutdown drain to consume).
                m_Census.GlobalPops.fetch_add(1, std::memory_order_relaxed);
                AccountDequeuedTask();
                drained.push_back({std::move(task), /*PoolCounted=*/true, ComputeContext(JobPriority::Normal)});
                found = true;
            }
            for (auto& worker : workers) {
                while (worker->LocalQueue.try_dequeue(task)) {
                    m_Census.LocalPops.fetch_add(1, std::memory_order_relaxed);
                    AccountDequeuedTask();
                    drained.push_back({std::move(task), /*PoolCounted=*/true, ComputeContext(JobPriority::Normal)});
                    found = true;
                }
            }
            // The background lane is drained ungated (the drain must reach
            // everything; the occupancy gate is a hot-scan optimization).
            // Disposal is priority-blind: phase 2/3 below dispatch on
            // envelope KIND — bare background tasks EXECUTE, background
            // handle tasks are Cancelled (see the class comment's shutdown
            // paragraph for the decision). The lane is recorded beside the
            // envelope, because phase 3 executes one mixed list and a bare
            // Background task still runs as Background there.
            while (m_BackgroundQueue.try_dequeue(task)) {
                m_BackgroundQueued.fetch_sub(1, std::memory_order_relaxed);
                m_Census.BackgroundPops.fetch_add(1, std::memory_order_relaxed);
                AccountDequeuedTask();
                drained.push_back({std::move(task), /*PoolCounted=*/true, ComputeContext(JobPriority::Background)});
                found = true;
            }

            if (!found) {
                bool empty = m_GlobalQueue.size_approx() == 0 &&
                             m_BackgroundQueue.size_approx() == 0;
                for (const auto& worker : workers) {
                    empty = empty && worker->LocalQueue.size_approx() == 0;
                }
                if (empty) {
                    break;
                }
            }
        }
        if (drained.empty()) {
            break;
        }

        // Phase 2: cancel handle/graph tasks, never execute them. The
        // arbitration CAS keeps this single-fire against a concurrent
        // CancelTask; a task the drain dequeues can no longer be claimed by
        // anything else, so losing the CAS just means the events already
        // fired.
        for (DrainedTask& entry : drained) {
            const bool bare =
                !entry.Task->NeedsTaskData() && !entry.Task->RequiresDependencyManagement();
            if (bare) {
                continue;
            }
            CancelUnrunEnvelope(std::move(entry.Task));
            if (entry.PoolCounted)
                pendingTasks.fetch_sub(1, std::memory_order_relaxed);
        }

        // Phase 3: execute the bare tasks. They are caller-barrier releasers
        // (ParallelFor helpers reference a blocked caller's
        // stack): dropping one hangs that caller forever (B7), and any
        // handle task they Wait() on is already terminal from phase 2.
        for (DrainedTask& entry : drained) {
            if (entry.Task) {
                ExecuteTaskOptimized(std::move(entry.Task), entry.Context);
                if (entry.PoolCounted)
                    pendingTasks.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }
}

void WorkStealingThreadPool::TakeChannelJobs(Vector<DrainedTask>& drained) {
    // Strong references under the registry mutex, the FIFOs outside it: each
    // channel's mutex is a leaf, never taken under another lock.
    Vector<SharedPtr<Detail::JobChannelState>> channels;
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        channels.reserve(m_Channels.size());
        for (const WeakPtr<Detail::JobChannelState>& channel : m_Channels) {
            if (SharedPtr<Detail::JobChannelState> live = channel.lock()) {
                channels.push_back(std::move(live));
            }
        }
    }
    Vector<Detail::ChannelJob> jobs;
    for (const SharedPtr<Detail::JobChannelState>& channel : channels) {
        channel->TakeQueued(jobs);
    }
    if (m_BlockingThreads) {
        m_BlockingThreads->TakeQueued(jobs);
    }
    for (Detail::ChannelJob& job : jobs) {
        drained.push_back({std::move(job.Envelope), /*PoolCounted=*/false, ChannelContext(job.Channel)});
    }
}

void WorkStealingThreadPool::CancelUnrunEnvelope(UniquePtr<TaskBase> task) {
    const TaskId taskId = task->GetTaskId();
    // Fast-path envelopes carry their TaskData inline; use it directly (one
    // fewer registry round-trip, and still correct if the registry entry is
    // already gone).
    SharedPtr<TaskHandle::TaskData> data =
        task->HasInlineTaskData() ? static_cast<Detail::HandleTask*>(task.get())->GetTaskData()
                                  : TryGetTaskData(taskId);
    if (data) {
        CancelPendingArbitrated(taskId, data);
    }
    task.reset();            // envelope + closure freed here (F14)
    CleanupTaskData(taskId); // ...so the registry entry releases here
}

void WorkStealingThreadPool::DisposeEnvelopeAfterGate(UniquePtr<TaskBase> task, JobContext context) {
    if (task->NeedsTaskData() || task->RequiresDependencyManagement()) {
        CancelUnrunEnvelope(std::move(task));
        return;
    }
    ExecuteTaskOptimized(std::move(task), context);
}

void WorkStealingThreadPool::DispatchToBlockingThreads(UniquePtr<TaskBase> task, const char* channel) {
    if (m_InlineMode) {
        ExecuteInlineBounded(std::move(task), ChannelContext(channel));
        return;
    }
    if (UniquePtr<TaskBase> refused =
            m_BlockingThreads->Push({std::move(task), channel}, Detail::BlockingThreads::Spawn::IfNeeded)) {
        DisposeEnvelopeAfterGate(std::move(refused), ChannelContext(channel));
    }
}

void WorkStealingThreadPool::HandOnToBlockingThreads(UniquePtr<TaskBase> task, const char* channel) {
    // An inline pool never queues a channel job, so it never hands one on.
    assert(!m_InlineMode && "JobChannel: an inline pool queued a channel job");
    if (UniquePtr<TaskBase> refused =
            m_BlockingThreads->Push({std::move(task), channel}, Detail::BlockingThreads::Spawn::Never)) {
        DisposeEnvelopeAfterGate(std::move(refused), ChannelContext(channel));
    }
}

void WorkStealingThreadPool::RegisterChannel(const SharedPtr<Detail::JobChannelState>& channel,
                                             uint32 maxRunning) {
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        m_Channels.push_back(channel);
    }
    if (m_BlockingThreads) {
        m_BlockingThreads->AddDemand(static_cast<int64>(maxRunning));
    }
}

void WorkStealingThreadPool::UnregisterChannel(const Detail::JobChannelState* channel, uint32 maxRunning) {
    {
        std::lock_guard<std::mutex> lock(m_ChannelsMutex);
        std::erase_if(m_Channels, [channel](const WeakPtr<Detail::JobChannelState>& registered) {
            const SharedPtr<Detail::JobChannelState> live = registered.lock();
            return live == nullptr || live.get() == channel;
        });
    }
    if (m_BlockingThreads) {
        m_BlockingThreads->AddDemand(-static_cast<int64>(maxRunning));
    }
}

void WorkStealingThreadPool::FlushPendingCleanupOnThisThread() {
    const bool ownEntries = std::any_of(t_pendingCleanup.begin(), t_pendingCleanup.end(),
                                        [this](const PendingCleanupEntry& entry) { return entry.Pool == this; });
    if (!ownEntries) {
        return;
    }
    SharedPtr<TaskHandle::TaskData> recycle[kPendingCleanupBatch];
    std::vector<SharedPtr<TaskHandle::TaskData>> deferDrop;
    size_t recycled = 0;
    {
        std::lock_guard<std::mutex> lock(m_TaskDataMutex);
        recycled = DrainPendingCleanupLocked(recycle, deferDrop);
    }
    // deferDrop's possibly-last references drop at scope exit, outside the
    // registry lock.
    CommitRecycledTaskData(recycle, recycled);
}

void WorkStealingThreadPool::DependencyAwareWorkerLoop(size_t workerId) {
    // Set thread-local worker ID and pointer for fast task submission
    s_CurrentWorkerId = workerId;
    s_CurrentWorker = workers[workerId].get();
    s_CurrentPool = this;
    s_JobState = &m_ThreadRecords[workerId].Value.State;

    char threadName[32];
    snprintf(threadName, sizeof(threadName), "Job Worker #%zu", workerId);
    GameEngine::Platform::SetCurrentThreadName(threadName);

    // Gates the spin window on recent productivity: after a sleep cycle that
    // expired on the backstop timeout (predicate still false), the worker goes
    // straight back to sleep instead of re-arming a full spin window. Without
    // this, every backstop expiry at complete idle would burn kSpinWindowUs
    // per armed spinner per timeout period. The gate never affects the
    // producer-visible protocol: a non-spinning worker is simply not counted
    // in m_NumSpinning, which producers already handle via P3/P4.
    bool spinAllowed = true;

    while (true) {
        // F13: exit as soon as shutdown is requested instead of draining the
        // queues dry. Leftover queued work belongs to Shutdown()'s post-join
        // drain — bare tasks execute there, handle/graph tasks are Cancelled.
        // Draining here would execute handle tasks the shutdown contract
        // promises to cancel.
        if (shutdown_requested.load()) {
            break;
        }

        UniquePtr<TaskBase> task;

        // 1. local queue (best cache locality), 2. global queue, 3. steal
        // ring, 4. background lane LAST (gated: one relaxed load when empty).
        // The lane a task came from is its class: the first three are Normal.
        JobPriority lane = JobPriority::Normal;
        bool foundTask = TryGetLocalTask(workerId, task) || TryGetGlobalTask(workerId, task) ||
                         TryStealTask(workerId, task);
        if (!foundTask && TryGetBackgroundTask(workerId, task)) {
            foundTask = true;
            lane = JobPriority::Background;
        }

        if (!foundTask && spinAllowed) {
            foundTask = TrySpinPoll(workerId, task, lane);
        }

        if (foundTask) {
            // Common bookkeeping point for EVERY consumed task (normal dequeue
            // and spin-poll finds): m_QueuedTasks release + wake propagation
            // happen here, and the pendingTasks decrement stays tied to
            // post-execution (F20 — ECS backpressure semantics).
            AccountDequeuedTask();
            ExecuteTaskOptimized(std::move(task), ComputeContext(lane));
            pendingTasks.fetch_sub(1, std::memory_order_relaxed);
            spinAllowed = true;
            continue;
        }

        // Census: the queued counter says work exists somewhere, but this
        // worker's full find cycle (own queue, global lanes, steal half-ring)
        // could not reach it — the sleep predicate below is true, so the
        // worker immediately re-loops instead of sleeping. Sustained counts
        // here are the pred-true busy-poll the steal half-scan can cause.
        if (m_QueuedTasks.load(std::memory_order_relaxed) > 0) {
            m_Census.EmptyLoopsWithBacklog.fetch_add(1, std::memory_order_relaxed);
        }

        // No tasks available, wait for new work.
        std::unique_lock<std::mutex> lock(globalMutex);
        // Sleeper count is maintained UNDER the mutex, incremented BEFORE the
        // predicate evaluation (§7 Hazard B): a producer that notifies takes
        // this mutex first, so either our predicate sees its published task,
        // or we are atomically blocked inside wait_for when its notify lands.
        m_NumSleeping.fetch_add(1, std::memory_order_seq_cst);
        const bool wokenWithWork = globalCondition.wait_for(
            lock,
            std::chrono::microseconds(m_SleepBackstopUs.load(std::memory_order_relaxed)),
            [this] {
                return shutdown_requested.load() ||
                       m_QueuedTasks.load(std::memory_order_seq_cst) > 0;
            });
        m_NumSleeping.fetch_sub(1, std::memory_order_relaxed);
        if (!wokenWithWork) {
            m_Census.BackstopTimeouts.fetch_add(1, std::memory_order_relaxed);
        }
        spinAllowed = wokenWithWork;
    }
}

void WorkStealingThreadPool::DrainInlineTasksOnThisThread() {
    auto& trampoline = GetInlineTrampoline();
    for (;;) {
        const auto pending = std::find_if(trampoline.Deferred.begin(), trampoline.Deferred.end(),
            [this](const DeferredInlineTask& task) { return task.Pool == this; });
        if (pending == trampoline.Deferred.end())
            return;
        auto task = std::move(pending->Task);
        const JobContext context{pending->Channel, pending->Class};
        trampoline.Deferred.erase(pending);
        // A synchronous join must make progress even at the recursion bound.
        // Children published by this task stay in the same bounded queue.
        ++trampoline.Depth;
        ExecuteTaskOptimized(std::move(task), context);
        --trampoline.Depth;
    }
}

void WorkStealingThreadPool::ExecuteInlineBounded(UniquePtr<TaskBase> task, JobContext context) {
    // Per-thread, not per-pool: the resource being bounded is this thread's
    // stack, and nested inline pools share it. Entries carry their owning
    // pool so the drain executes each task against the pool it was published
    // to.
    auto& trampoline = GetInlineTrampoline();

    if (trampoline.Depth >= kMaxInlineExecutionDepth) {
        trampoline.Deferred.push_back({this, std::move(task), context.Channel, context.Class});
        return;
    }

    ++trampoline.Depth;
    ExecuteTaskOptimized(std::move(task), context);
    if (trampoline.Depth == 1) {
        // Outermost frame: drain iteratively. A drained task's own enqueues
        // re-enter above at depth 1, recurse up to the bound, and overflow
        // back into this queue — the loop terminates when the tree is done.
        while (!trampoline.Deferred.empty()) {
            DeferredInlineTask deferred = std::move(trampoline.Deferred.front());
            trampoline.Deferred.pop_front();
            deferred.Pool->ExecuteTaskOptimized(std::move(deferred.Task),
                                                JobContext{deferred.Channel, deferred.Class});
        }
    }
    --trampoline.Depth;
}

void WorkStealingThreadPool::EnqueueTask(UniquePtr<TaskBase> task) {
    if (m_InlineMode) {
        // Inline mode: execute at publish, bypassing queues, counters, and
        // wakes entirely. Graph-published dependents re-enter here;
        // ExecuteInlineBounded caps the recursion depth and carries the
        // cancel arbitration and completion paths unchanged. Graph tasks
        // are Normal.
        ExecuteInlineBounded(std::move(task), ComputeContext(JobPriority::Normal));
        return;
    }
    // Producer publish sequence (§7): counters BEFORE the push (F5 — a fast
    // consumer decrements at dequeue-success; push-then-increment would let it
    // transiently underflow the unsigned counter), then the wake decision.
#if GE_DEBUG_INSTRUMENTATION
    // Bracket enter: in-flight FIRST, then epoch (see m_CounterBracketEpoch —
    // this order is what makes the tripwire's two disarms gapless).
    m_CounterRacersInFlight.fetch_add(1, std::memory_order_seq_cst);
    m_CounterBracketEpoch.fetch_add(1, std::memory_order_seq_cst);
#endif
    pendingTasks.fetch_add(1, std::memory_order_relaxed);
    m_QueuedTasks.fetch_add(1, std::memory_order_seq_cst); // (P1)
    PushTask(std::move(task));
#if GE_DEBUG_INSTRUMENTATION
    m_CounterRacersInFlight.fetch_sub(1, std::memory_order_seq_cst);
#endif
    NotifyAfterPublish(); // (P2..P4)
}

void WorkStealingThreadPool::PushTask(UniquePtr<TaskBase> task) {
    // Use the local worker queue only when the calling thread is a worker of
    // THIS pool; a worker of another pool must take the external path or the
    // task would land in a queue this pool never drains.
    if (s_CurrentWorker != nullptr && s_CurrentPool == this) {
        if (!s_CurrentWorker->LocalQueue.enqueue(s_CurrentWorker->ProducerToken,
                                                 std::move(task))) {
            // Allocating enqueue fails only on genuine OOM; a dropped graph
            // task strands its dependents forever — fail loud (slice-4 OOM
            // discipline).
            std::fprintf(stderr, "WorkStealingThreadPool: local queue allocation failed under "
                                 "OOM; aborting rather than dropping queued work.\n");
            std::fflush(stderr);
            std::abort();
        }
        m_Census.LocalPushes.fetch_add(1, std::memory_order_relaxed);
        CountPublishedOnThisThread(1);
    } else {
        // External submissions may come from many threads (UI, asset batcher,
        // thumbnail preload, file watchers). ProducerToken is thread-affine, so
        // use the tokenless MPMC enqueue path instead of sharing one token.
        // Graph tasks are always Normal.
        PushGlobalQueue(std::move(task), JobPriority::Normal);
    }
}

void WorkStealingThreadPool::NotifyAfterPublish() {
    // (P2, F2-amended): skip the kernel notify ONLY when the armed spinners
    // cover the whole queue depth, evaluated AFTER our own increment — one
    // spinner can only cover one task, so N producers racing one spinner must
    // still send N-1 notifies. Both loads are seq_cst: the failure mode is
    // StoreLoad reordering of our (P1) increment below this load against a
    // retiring spinner's (Y)/(Z) pair — acquire/release does not order a store
    // before a later load to a different variable (§7 Hazard A).
    const uint32 spinning = m_NumSpinning.load(std::memory_order_seq_cst); // (P2)
    const uint32 queued = m_QueuedTasks.load(std::memory_order_seq_cst);
    if (spinning >= queued) {
        return;
    }

    // (P3): nobody is sleeping — every worker is busy, spinning (insufficiently,
    // handled above), or between polls; the next natural dequeue picks it up.
    if (m_NumSleeping.load(std::memory_order_seq_cst) == 0) {
        return;
    }

    // (P4): the empty lock/unlock pairs with the sleeper's increment+predicate
    // under the same mutex (§7 Hazard B) — this closes the classic lost-wakeup
    // window that the 1ms backstop used to paper over.
    {
        std::lock_guard<std::mutex> lk(globalMutex);
    }
    globalCondition.notify_one();
}

void WorkStealingThreadPool::PublishAndWakeBatch(size_t publishedCount) {
    // (P2 batch form, F2-amended): an armed spinner covers AT MOST ONE queued
    // task, so only the queued remainder above the spinner count needs kernel
    // wakes — subtracting the counts is the batch generalization of the
    // single-task "spinning >= queued" skip. Both loads are seq_cst for the
    // same §7 Hazard-A StoreLoad reasoning as NotifyAfterPublish: our (P1)
    // increments must not sink below these loads against a retiring
    // spinner's (Y)/(Z) pair — a spinner we counted either re-checks after
    // retire and sees our tasks, or retired first and is not counted here.
    const uint32 spinning = m_NumSpinning.load(std::memory_order_seq_cst); // (P2)
    const uint32 queued = m_QueuedTasks.load(std::memory_order_seq_cst);
    const uint32 uncovered = queued > spinning ? queued - spinning : 0;
    const uint32 need =
        static_cast<uint32>(std::min<size_t>(publishedCount, uncovered));
    if (need == 0) {
        return; // armed spinners cover the whole backlog (or it drained already)
    }

    // (P3 batch form): nobody sleeping — every worker is busy, spinning
    // (covered above), or between polls; the next natural dequeue picks the
    // batch up. Same acceptable staleness as the single-task P3: a worker
    // concurrently entering the sleep path evaluates its predicate under
    // globalMutex AFTER our seq_cst (P1) increments and sees the backlog.
    const uint32 sleepers = m_NumSleeping.load(std::memory_order_seq_cst); // (P3)
    if (sleepers == 0) {
        return;
    }

    // (P4 batch form): bounded caller-side fan-out. At most kMaxWakePerBatch
    // sleepers are notified; the worker-side F3 propagation chain
    // (AccountDequeuedTask) extends the wake front by one on every successful
    // dequeue while backlog and sleepers remain, so a wide batch still
    // reaches full worker width. notify_all is rejected (spec: thundering
    // herd × steal-ring cache thrash).
    //
    // The lock acquisition pairs with the sleeper's increment+predicate
    // sequence under the same mutex (§7 Hazard B), exactly like the
    // single-task (P4): every sleeper either observes m_QueuedTasks > 0 in
    // its predicate or is already atomically blocked when the notifies land.
    // The notifies are issued while the mutex is HELD: releasing first lets
    // the woken workers' wait_for exits (which must reacquire globalMutex)
    // interleave with the remaining notifies and with this caller's next
    // batch publish — measured ~30% worse caller-side in the 13-run storm
    // bench. Holding the lock keeps the wake batch atomic: all notified
    // workers queue behind one release edge.
    const uint32 wakes = std::min({need, sleepers, kMaxWakePerBatch});
    {
        std::lock_guard<std::mutex> lk(globalMutex);
        for (uint32 i = 0; i < wakes; ++i) {
            globalCondition.notify_one();
        }
    }
    m_BatchNotifiesForTests.fetch_add(wakes, std::memory_order_relaxed);
}

void WorkStealingThreadPool::AccountDequeuedTask() {
    m_QueuedTasks.fetch_sub(1, std::memory_order_seq_cst);

    // Wake propagation (F3): if a backlog remains and sleepers exist, pass one
    // wake along. The chain terminates by sleeper exhaustion or queue drain;
    // it makes bursts self-healing when producer notifies coalesced.
    if (m_QueuedTasks.load(std::memory_order_seq_cst) > 0 &&
        m_NumSleeping.load(std::memory_order_seq_cst) > 0) {
        {
            std::lock_guard<std::mutex> lk(globalMutex);
        }
        globalCondition.notify_one();
    }
}

bool WorkStealingThreadPool::TrySpinPoll(size_t workerId, UniquePtr<TaskBase>& task, JobPriority& lane) {
    const uint32 spinWindowUs = m_SpinWindowUs.load(std::memory_order_relaxed);
    if (spinWindowUs == 0) {
        return false; // Spin path disabled: pre-change sleep behavior.
    }

    // Bounded token acquisition: at most m_MaxSpinners workers poll at once.
    {
        const uint32 maxSpinners = m_MaxSpinners.load(std::memory_order_relaxed);
        uint32 current = m_NumSpinning.load(std::memory_order_seq_cst);
        do {
            if (current >= maxSpinners) {
                return false;
            }
        } while (!m_NumSpinning.compare_exchange_weak(current, current + 1,
                                                      std::memory_order_seq_cst));
    }

    // F4: the token is RAII-guarded — released exactly once on every exit path
    // (deadline expiry, task found, shutdown, exception). Retire() is §7 op (Y).
    struct SpinToken {
        std::atomic<uint32>& Counter;
        bool Armed = true;
        void Retire() {
            if (Armed) {
                Counter.fetch_sub(1, std::memory_order_seq_cst); // (Y)
                Armed = false;
            }
        }
        ~SpinToken() { Retire(); }
    } token{m_NumSpinning};

    bool found = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::microseconds(spinWindowUs);
    uint32 pauseCount = 1;

    for (uint32 iteration = 1;; ++iteration) {
        if (shutdown_requested.load()) {
            break;
        }

        // Our own local queue is provably empty here: only this thread feeds
        // it, and it is not executing tasks while spinning. Poll the global
        // queue every iteration and the steal ring every kSpinStealPeriod-th;
        // the background lane keeps its scan position (LAST, after the steal
        // attempt) on the same cadence — polling it every iteration would let
        // a background task beat a steal-visible Normal task 7 laps out of 8.
        if (TryGetGlobalTask(workerId, task) ||
            ((iteration % kSpinStealPeriod) == 0 && TryStealTask(workerId, task))) {
            lane = JobPriority::Normal;
            found = true;
            break;
        }
        if ((iteration % kSpinStealPeriod) == 0 && TryGetBackgroundTask(workerId, task)) {
            lane = JobPriority::Background;
            found = true;
            break;
        }

        // Wall-clock deadline: pause-ramp iterations have variable cost, so
        // the window is bounded by steady_clock, not iteration count.
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }

        for (uint32 p = 0; p < pauseCount; ++p) {
            CpuRelax();
        }
        if (pauseCount < kSpinPauseMax) {
            pauseCount *= 2;
        }
        if ((iteration % kSpinYieldPeriod) == 0) {
            std::this_thread::yield();
        }
    }

    // (Y) Retire BEFORE executing anything found. The caller decrements
    // m_QueuedTasks at the common bookkeeping point only after this returns,
    // i.e. after (Y): a producer that still observes our armed token also
    // still observes the queued count covering the task we physically hold,
    // so its F2 comparison cannot double-count us against a second task.
    token.Retire();

    if (!found && m_QueuedTasks.load(std::memory_order_seq_cst) > 0) { // (Z)
        // A producer may have skipped its notify on our account (P2) between
        // our last poll and (Y). Dequeue-or-notify (F5): take the task if we
        // can still see it, otherwise hand the wake to a sleeper — never drop.
        // m_QueuedTasks counts both classes, so the recheck's dequeue attempt
        // covers the background lane too (last, as everywhere).
        found = TryGetGlobalTask(workerId, task) || TryStealTask(workerId, task);
        lane = JobPriority::Normal;
        if (!found && TryGetBackgroundTask(workerId, task)) {
            found = true;
            lane = JobPriority::Background;
        }
        if (!found) {
            {
                std::lock_guard<std::mutex> lk(globalMutex);
            }
            globalCondition.notify_one();
        }
    }

    return found;
}

bool WorkStealingThreadPool::TryGetLocalTask(size_t workerId, UniquePtr<TaskBase>& task) {
    auto& worker = *workers[workerId];
    if (worker.LocalQueue.try_dequeue(worker.LocalConsumerToken, task)) {
        m_Census.LocalPops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool WorkStealingThreadPool::TryGetGlobalTask(size_t workerId, UniquePtr<TaskBase>& task) {
    // Token dequeue (#360): fair rotation across producer sub-queues instead
    // of the tokenless biggest-of-first-3-non-empty heuristic, which starves
    // low-rate producers for the whole lifetime of a sustained flood (full
    // mechanism at Worker::GlobalConsumerToken). Failure semantics match the
    // tokenless path — false means every sub-queue appeared empty at check
    // time — so callers' m_QueuedTasks relap tolerance is unchanged.
    if (m_GlobalQueue.try_dequeue(workers[workerId]->GlobalConsumerToken, task)) {
        m_Census.GlobalPops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool WorkStealingThreadPool::TryGetBackgroundTask(size_t workerId, UniquePtr<TaskBase>& task) {
    // The occupancy gate: an empty background lane costs exactly this one
    // relaxed load per scan — the queue itself is never touched (the slice-7
    // lesson: an ungated second global queue taxed every consumer scan).
    // A stale zero here is benign: the producer's m_QueuedTasks increment
    // keeps the sleep predicate true, so the worker relaps and re-reads.
    if (m_BackgroundQueued.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    // Token dequeue for the same #360 fairness reason as TryGetGlobalTask:
    // the hole is producer-count-driven, so a background flood (streaming is
    // this lane's steady-state consumer) could starve another background
    // producer identically.
    if (m_BackgroundQueue.try_dequeue(workers[workerId]->BackgroundConsumerToken, task)) {
        // Decrement AFTER dequeue-success only — paired with the producers'
        // increment-before-push, the counter can never underflow and always
        // over-counts physical occupancy (the safe direction: a transient
        // false-nonzero costs one failed try_dequeue, never a missed task).
        m_BackgroundQueued.fetch_sub(1, std::memory_order_relaxed);
        m_Census.BackgroundPops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool WorkStealingThreadPool::TryStealTask(size_t thiefId, UniquePtr<TaskBase>& task) {
    // Ring-based work stealing for better cache locality
    // Try adjacent workers first, then expand outward
    const size_t numWorkers = workers.size();

    if (numWorkers <= 1) {
        return false; // No other workers to steal from
    }

    // Try workers in ring order: thiefId+1, thiefId+2, etc.
    // This improves cache locality by preferring nearby cores
    for (size_t offset = 1; offset < numWorkers; ++offset) {
        size_t victimId = (thiefId + offset) % numWorkers;

        auto& victim = *workers[victimId];

        // IMPORTANT:
        // `moodycamel::ConsumerToken` is thread-affine. The victim's consumer token must only be
        // used by the victim thread; using it from the thief thread is undefined behavior and
        // can crash intermittently (especially under contention).
        //
        // For work stealing we either need per-thief tokens per victim queue, or simply use the
        // tokenless dequeue API (slightly slower but correct).
        if (victim.LocalQueue.try_dequeue(task)) {
            m_Census.StealPops.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        // Early exit optimization: if we've tried half the workers
        // and found nothing, the remaining workers are likely empty too
        if (offset >= numWorkers / 2) {
            break;
        }
    }

    m_Census.StealMisses.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void WorkStealingThreadPool::ExecuteTaskOptimized(UniquePtr<TaskBase> task, JobContext context) {
    TaskId taskId = task->GetTaskId();

    // Capability bits (slice 6: construction-time flags on TaskBase — these
    // were two virtual calls per execution).
    const bool requiresDependencyManagement = task->RequiresDependencyManagement();
    const bool needsTaskData = task->NeedsTaskData();

    // Ultra-lightweight path: no TaskHandle, no TaskData, no dependency graph.
    // Used by EnqueueWork / EnqueueWorkBatch (and the fork-join algorithms
    // built on them) for zero-bookkeeping dispatch.
    if (!needsTaskData && !requiresDependencyManagement) {
        try {
            JobBodyScope body(context);
            task->Execute();
        } catch (const std::exception& e) {
            Logger::Log::Error("Bare task execution failed: {}", e.what());
        } catch (...) {
            Logger::Log::Error("Bare task {} threw a non-standard exception", taskId);
        }
        return;
    }

    // Releases the registry's strong reference to this task's TaskData on
    // every exit path below. The TaskHandle held by the submitter keeps its
    // own shared_ptr to the same TaskData (via GetTaskData()), so the
    // underlying object stays alive for any in-flight Wait()/GetResult()
    // callers — only the registry entry goes away. Without this, every
    // Submit() leaks a TaskData entry forever (TaskIds are monotonic; no
    // entry is ever overwritten), which manifested as ~3 MB/sec CPU heap
    // growth during pure idle (TaskData is the top growing type in VS
    // Diagnostic Tools).
    struct TaskDataReleaser
    {
        WorkStealingThreadPool* pool;
        TaskId                  id;
        bool                    needsCleanup;
        ~TaskDataReleaser()
        {
            if (needsCleanup)
                pool->CleanupTaskData(id);
        }
    } releaser{this, taskId, needsTaskData};

    // The Submit(F&&) fast path carries its TaskData in the envelope
    // (HandleTask); adopt it instead of re-entering the registry, which would
    // take m_TaskDataMutex a second time. Graph tasks keep the registry
    // lookup; their completion is bridged by id from
    // CompleteGraphTask/ProcessGraphActions anyway.
    TaskHandle handle =
        task->HasInlineTaskData()
            ? TaskHandle(taskId, this, static_cast<Detail::HandleTask*>(task.get())->GetTaskData())
            : TaskHandle(taskId, this);

    // F12 cancel/execute arbitration, BEFORE any side effect of execution:
    // CAS(Pending -> Running) against Cancel()'s CAS(Pending -> Cancelled) on
    // the same status atomic. Exactly one side wins.
    if (auto data = handle.GetTaskData()) {
        TaskStatus expected = TaskStatus::Pending;
        if (!data->Status.compare_exchange_strong(expected, TaskStatus::Running,
                                                  std::memory_order_seq_cst)) {
            // The canceller won: it fired the Cancelled callbacks and waiter
            // wakeups on its own thread, and (for graph tasks) already
            // retired the graph node. This side fires NOTHING and drops the
            // envelope — the UniquePtr destructor frees the closure here
            // (F14). Counters conserve: m_QueuedTasks was released at the
            // dequeue-success point and the worker loop decrements
            // pendingTasks after this returns (F20).
            assert(expected == TaskStatus::Cancelled &&
                   "JobSystem: dequeued task in non-Pending, non-Cancelled state");
            return;
        }
    }

    if (requiresDependencyManagement) {
        // Dependency-aware path: flip the graph node Pending → Running. With
        // the execution CAS won above, no cancel/cascade can have erased the
        // node (cancel requires winning the same CAS; cascades only reach
        // parked payloads, and this one was dequeued) — a false return is
        // unreachable. Assert loudly rather than silently executing without a
        // graph-terminal (which would strand this task's dependents parked
        // forever); Release degrades to exactly that silent narrowing.
        const bool isInDependencyGraph = m_DependencyGraph->MarkRunning(taskId);
        assert(isInDependencyGraph &&
               "unreachable: execution CAS won but the graph node is gone — a cancel/cascade "
               "erased a node whose payload was already dequeued (F12 arbitration breach)");

        try {
            {
                // The body only: the graph release below runs outside it.
                JobBodyScope body(context);
                task->Execute();
            }

            if (isInDependencyGraph) {
                CompleteGraphTask(taskId, true, String{}, nullptr);
            }
        } catch (const std::exception& e) {
            Logger::Log::Error("Task execution failed: {}", e.what());

            if (handle.IsValid()) {
                handle.SetException(std::current_exception());

                auto data = handle.GetTaskData();
                if (data) {
                    data->ErrorMessage = e.what();
                }
            }

            if (isInDependencyGraph) {
                CompleteGraphTask(taskId, false, e.what(), std::current_exception());
            }
        } catch (...) {
            const String msg = "Task " + std::to_string(taskId) + " threw a non-standard exception";
            Logger::Log::Error("{}", msg);

            if (handle.IsValid()) {
                handle.SetException(std::current_exception());

                auto data = handle.GetTaskData();
                if (data) {
                    data->ErrorMessage = msg;
                }
            }

            if (isInDependencyGraph) {
                CompleteGraphTask(taskId, false, msg, std::current_exception());
            }
        }
    } else {
        // Ultra-fast path for simple tasks: no dependency graph interaction,
        // minimal overhead. The outcome locals cost nothing on success (SSO
        // empty string, null exception_ptr) — they exist for the graph-link
        // bridge below.
        bool success = true;
        String failureMessage;
        std::exception_ptr failureException;
        try {
            {
                // The body only: completion callbacks run outside it.
                JobBodyScope body(context);
                task->Execute();
            }
            if (handle.IsValid()) {
                handle.MarkCompleted();
            }
        } catch (const std::exception& e) {
            success = false;
            failureMessage = e.what();
            failureException = std::current_exception();
            Logger::Log::Error("Simple task execution failed: {}", failureMessage);
            if (handle.IsValid()) {
                // Exception BEFORE the terminal flip: MarkFailed's status
                // exchange is the release edge cross-thread observers read
                // from (waiters, LinkSubmitDependency's terminal recheck) —
                // writing the payload after it races those readers.
                handle.SetException(failureException);
                handle.MarkFailed(failureMessage);
            }
        } catch (...) {
            success = false;
            failureMessage = "Task " + std::to_string(taskId) + " threw a non-standard exception";
            failureException = std::current_exception();
            Logger::Log::Error("{}", failureMessage);
            if (handle.IsValid()) {
                // Exception before the terminal flip — see above.
                handle.SetException(failureException);
                handle.MarkFailed(failureMessage);
            }
        }

        // Graph-link bridge (Submit(F&&, deps)): if a dependent linked this
        // task as a dependency, its mirror node is waiting on this terminal —
        // release/cascade it now. ONE seq_cst load on the fast path, no lock,
        // no allocation (the plain-Submit alloc/lock pins hold). Sequenced
        // AFTER Mark*'s status exchange: the Dekker pair with
        // LinkSubmitDependency's flag-store-then-status-load, so a linker
        // that missed this completion sees the terminal status instead.
        if (auto data = handle.GetTaskData();
            data && data->GraphLinked.load(std::memory_order_seq_cst)) {
            CompleteLinkedDependency(taskId, success, failureMessage, failureException);
        }

        // Envelope death point, BEFORE the releaser queues the registry
        // cleanup at scope exit: the deferred drain then usually observes the
        // registry as the TaskData's last reference and can recycle it
        // (slice 6). Same ordering vs MarkCompleted as before — the closure
        // is destroyed after the completion events fire.
        task.reset();
    }
}

void WorkStealingThreadPool::CompleteGraphTask(TaskId taskId, bool success, const String& failureMessage,
                                               std::exception_ptr failureException) {
    // Pre-store the source's terminal status BEFORE the graph flip. The graph
    // mutex inside MarkCompleted is then the release edge that makes
    // graph-observed terminality imply handle-observed terminality: a
    // dependent released by ANOTHER thread (absent-dep at Register, or
    // cross-thread fan-in extraction) can start executing before this thread
    // reaches ProcessGraphActions, and its guarded TryGetResult on this task
    // must already see Completed. The MarkCompleted/MarkFailed below is an
    // idempotent re-exchange that fires the notify + callbacks exactly once
    // (terminal events are emitted once per id — the node is erased in the
    // same critical section that emits them). The source's ErrorMessage /
    // Exception were recorded by the worker's catch block before this call.
    // Cascade-FAILED dependents need no pre-store: an early TryGetResult
    // observing "not completed" on a genuinely failed dependency is the
    // correct outcome for the guarded callers.
    {
        TaskHandle sourceHandle(taskId, this);
        if (auto data = sourceHandle.GetTaskData()) {
            data->Status.store(success ? TaskStatus::Completed : TaskStatus::Failed);
        }
    }

    // ONE critical section inside MarkCompleted (F9): terminal flip,
    // dependents' remaining-dependency decrement, extraction of newly-ready
    // parked payloads, failure propagation, and erase of every node made
    // terminal (B4). Everything user-visible happens after unlock, below.
    TaskGraphActions actions = m_DependencyGraph->MarkCompleted(taskId, success);
    ProcessGraphActions(actions, taskId, failureMessage, failureException);
}

void WorkStealingThreadPool::ProcessGraphActions(TaskGraphActions& actions, TaskId sourceTaskId,
                                                 const String& failureMessage,
                                                 std::exception_ptr failureException) {
    // F10: runs with NO graph lock held. TaskHandle flips fire user callbacks
    // (which may Submit or Cancel and re-enter the graph), and EnqueueTask
    // takes the pool's globalMutex for wake notifies.
    //
    // NOTE: the guarded-TryGetResult contract (a released dependent observes
    // its dependency's TaskData terminal) is carried by CompleteGraphTask's
    // status PRE-STORE before the graph flip — not by the events-before-
    // enqueue order here, which only covers dependents released in THIS
    // actions set. Dependents released by another thread (absent-dep
    // Register, cross-thread fan-in) never see this loop's ordering.
    // Single-fire discipline (F12): terminal events for the SOURCE id are
    // owned by the caller — the executing worker (which won Pending→Running
    // and is the only possible producer of its Completed/Failed), or a
    // canceller that won Pending→Cancelled before calling in. Cascaded
    // dependents' events, however, can race a concurrent CancelTask on the
    // same dependent, so their fire is guarded by winning the dependent's own
    // Pending→{Failed,Cancelled} transition; the loser fires nothing.
    for (const auto& event : actions.Events) {
        TaskHandle handle(event.Id, this);
        if (!handle.IsValid()) {
            continue;
        }
        const bool isSource = (event.Id == sourceTaskId);

        switch (event.NewStatus) {
        case TaskStatus::Completed:
            // Only ever emitted for the source id (executor-exclusive).
            handle.MarkCompleted();
            break;
        case TaskStatus::Failed: {
            auto data = handle.GetTaskData();
            if (!isSource) {
                // Claim the dependent exclusively BEFORE seeding its error
                // info: CAS(Pending -> Running) loses only to a canceller
                // that already won this dependent's arbitration and fired.
                // Claiming with Running (not Failed) keeps the ordering
                // waiters rely on — a handle never reads terminal without
                // its ErrorMessage/Exception already seeded; MarkFailed
                // below performs the terminal flip + notify + callbacks.
                TaskStatus expected = TaskStatus::Pending;
                if (!data || !data->Status.compare_exchange_strong(
                                 expected, TaskStatus::Running, std::memory_order_seq_cst)) {
                    break; // a concurrent canceller won this dependent's fire
                }
                // Dependency-failure propagation: seed the dependent's
                // error info before its failure callbacks observe it. The
                // source task's own info was recorded by the worker in
                // ExecuteTaskOptimized's catch block.
                if (data->ErrorMessage.empty()) {
                    data->ErrorMessage = "Dependency task " + std::to_string(sourceTaskId) +
                                         " failed: " + failureMessage;
                }
                if (!data->Exception) {
                    data->Exception = failureException;
                }
            }
            handle.MarkFailed(data ? data->ErrorMessage : String{});
            break;
        }
        case TaskStatus::Cancelled: {
            // A cascaded dependent, or a cancel ORIGIN routed from Shutdown
            // step 4 (CancelAllPending emits origin events with sourceTaskId
            // 0, and nothing strips them). A user-facing Cancel() origin
            // never reaches here — CancelPendingArbitrated fires it directly
            // on its arbitrated TaskData and strips its event before this
            // loop. The CAS below keeps every path single-fire.
            auto data = handle.GetTaskData();
            TaskStatus expected = TaskStatus::Pending;
            if (!data || !data->Status.compare_exchange_strong(
                             expected, TaskStatus::Cancelled, std::memory_order_seq_cst)) {
                break; // a concurrent canceller won this dependent's fire
            }
            handle.MarkCancelled();
            break;
        }
        default:
            break;
        }

        // Registry cleanup does NOT happen per-event here. The TaskData
        // registry entry must outlive the task's ENVELOPE: a Cancelled task's
        // payload can still be sitting in a pool queue, and erasing its entry
        // lets the dequeuing worker's GetOrCreateTaskData re-materialize a
        // FRESH Pending TaskData — which then wins the F12 execution CAS and
        // resurrects the cancelled task. Cleanup is tied to envelope death
        // instead: the executing/dropping worker's TaskDataReleaser, the
        // retired-payload loop below, or the shutdown drain.
    }

    for (auto& ready : actions.ReadyTasks) {
        EnqueueTask(std::move(ready));
    }

    // Parked payloads of failed/cancelled dependents die here, outside every
    // lock — their destructors run user code (captured closures). This is
    // their envelopes' death point, so their registry entries release here
    // too (callers' TaskHandle copies keep their own shared_ptr).
    for (const auto& retired : actions.RetiredTasks) {
        if (retired && retired->NeedsTaskData()) {
            CleanupTaskData(retired->GetTaskId());
        }
    }
    actions.RetiredTasks.clear();
}

void WorkStealingThreadPool::CleanupTaskData(TaskId taskId) {
    // INVARIANT (F12): only call this once the task's ENVELOPE is dead —
    // executed, dropped by the losing worker, retired from the graph, or
    // consumed by the shutdown drain. The registry entry is the cancel/execute
    // arbitration state; erasing it while the envelope is still queued lets
    // the dequeuing worker's GetOrCreateTaskData re-materialize a fresh
    // Pending TaskData and resurrect a cancelled task.
    //
    // Defer the registry erase to the thread-local pending list. The next
    // GetOrCreateTaskData() call from this thread (or a forced flush) will
    // drain the batch under the existing Submit-side lock. This collapses
    // ~2 mutex ops per task to ~1, which is meaningful in DebugFast where
    // each mutex op carries CRT validation overhead.
    //
    // The TaskData shared_ptr ref held by the registry stays alive until
    // drain — fine, because callers' TaskHandle copies keep their own ref
    // (via GetTaskData()) until they go out of scope. Entries whose registry
    // reference is the LAST one are recycled into the thread-local TaskData
    // pool at drain time (slice 6 — see DrainPendingCleanupLocked for the
    // exclusivity proof).
    t_pendingCleanup.push_back({this, taskId});

    // Bounded flush: if THIS pool's share of the pending list grows past the
    // batch threshold, force a drain to keep the registry's high-water mark
    // sane even on a thread that rarely calls GetOrCreateTaskData. The share
    // is counted only once the total crosses the threshold — in the
    // single-pool common case that is exactly the old size check (own ==
    // total), and the O(size) count is amortized across the batch. Gating on
    // the OWN count (not the total) keeps foreign residue — bounded per pool
    // by this same threshold — from turning every push into a fruitless
    // mutexed drain.
    if (t_pendingCleanup.size() >= kPendingCleanupBatch)
    {
        size_t own = 0;
        for (const PendingCleanupEntry& entry : t_pendingCleanup)
        {
            own += (entry.Pool == this) ? 1 : 0;
        }
        if (own < kPendingCleanupBatch)
        {
            return;
        }
        SharedPtr<TaskHandle::TaskData> recycle[kPendingCleanupBatch];
        std::vector<SharedPtr<TaskHandle::TaskData>> deferDrop;
        size_t recycled = 0;
        {
            std::lock_guard<std::mutex> lock(m_TaskDataMutex);
            recycled = DrainPendingCleanupLocked(recycle, deferDrop);
        }
        // deferDrop's possibly-last references drop at scope exit, outside
        // the registry lock.
        CommitRecycledTaskData(recycle, recycled);
    }
}

void WorkStealingThreadPool::EnqueueTaskGlobal(UniquePtr<TaskBase> task, JobPriority priority) {
    if (m_InlineMode) {
        // Inline mode: execute at publish (see EnqueueTask). Priority orders
        // nothing when execution is synchronous; the body still runs as its
        // class.
        ExecuteInlineBounded(std::move(task), ComputeContext(priority));
        return;
    }
    // Same publish sequence as EnqueueTask (§7): counters BEFORE the push (F5),
    // wake decision after. Always a global lane — never a local queue.
    // m_QueuedTasks counts BOTH classes, so the wake protocol below is
    // priority-blind; the background occupancy gate follows the same
    // before-push direction.
#if GE_DEBUG_INSTRUMENTATION
    // Bracket enter: in-flight FIRST, then epoch (see m_CounterBracketEpoch).
    m_CounterRacersInFlight.fetch_add(1, std::memory_order_seq_cst);
    m_CounterBracketEpoch.fetch_add(1, std::memory_order_seq_cst);
#endif
    pendingTasks.fetch_add(1, std::memory_order_relaxed);
    m_QueuedTasks.fetch_add(1, std::memory_order_seq_cst); // (P1)
    if (priority == JobPriority::Background) {
        m_BackgroundQueued.fetch_add(1, std::memory_order_relaxed);
    }
    PushGlobalQueue(std::move(task), priority);
#if GE_DEBUG_INSTRUMENTATION
    m_CounterRacersInFlight.fetch_sub(1, std::memory_order_seq_cst);
#endif
    NotifyAfterPublish(); // (P2..P4)
}

moodycamel::ConcurrentQueue<UniquePtr<TaskBase>>& WorkStealingThreadPool::PriorityLane(JobPriority priority) {
    return priority == JobPriority::Background ? m_BackgroundQueue : m_GlobalQueue;
}

void WorkStealingThreadPool::PushGlobalQueue(UniquePtr<TaskBase> task, JobPriority priority) {
    // Allocating tokenless enqueue: fresh blocks are allocated on demand and
    // recycled after warmup, so this fails only on genuine OOM. Until slice 7
    // this lane was a try_enqueue (never-allocate) queue spilling into a
    // second allocating queue — the census measured the spill at 0 across
    // ~450k pushes, so the two-queue dance bought nothing and cost every
    // consumer a second empty poll per scan. (The background lane does not
    // regress that: every poll of it is gated on m_BackgroundQueued.)
    if (!PriorityLane(priority).enqueue(std::move(task))) {
        // A silently dropped task can be a caller-barrier release (B7) or a
        // graph dependency — dropping is a permanent hang, strictly worse
        // than terminating an already-OOM'd process (slice-4 discipline).
        std::fprintf(stderr, "WorkStealingThreadPool: global queue allocation failed under "
                             "OOM; aborting rather than dropping queued work.\n");
        std::fflush(stderr);
        std::abort();
    }
    auto& pushCounter = priority == JobPriority::Background ? m_Census.BackgroundPushes
                                                            : m_Census.GlobalPushes;
    pushCounter.fetch_add(1, std::memory_order_relaxed);
    CountPublishedOnThisThread(1);
}

void WorkStealingThreadPool::EnqueueBulkGlobal(UniquePtr<TaskBase>* tasks, size_t count,
                                               JobPriority priority) {
    // ALLOCATING enqueue_bulk (see the declaration comment for why
    // try_enqueue_bulk is not used). The caller already published the
    // counters for the whole run (F5), the background occupancy included.
    const bool ok = PriorityLane(priority).enqueue_bulk(std::make_move_iterator(tasks), count);
    if (!ok) {
        // OOM-only path (audit F3): the counters are already published and
        // barrier chunks may be in this run — dropping it would strand
        // callers and poison the counters forever. A failed enqueue_bulk
        // returns BEFORE constructing any element (audit-verified), so the
        // source range is un-moved and a per-task retry is safe; if even
        // single-element allocation fails, fail LOUD.
        assert(false && "enqueue_bulk failed — falling back to per-task publish");
        for (size_t i = 0; i < count; ++i) {
            PushGlobalQueue(std::move(tasks[i]), priority);
        }
        return;
    }
    auto& pushCounter = priority == JobPriority::Background ? m_Census.BackgroundPushes
                                                            : m_Census.GlobalPushes;
    pushCounter.fetch_add(count, std::memory_order_relaxed);
    CountPublishedOnThisThread(count);
}

} // namespace JobSystem
