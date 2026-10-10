#pragma once

#include "JobSystem/Types.h"
#include "JobSystem/TaskTypes.h"
#include <array>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace JobSystem {

/**
 * @brief Side effects of a graph mutation, returned to the caller.
 *
 * Every mutating graph operation is a single critical section whose outward
 * effects — TaskHandle status flips (which fire user callbacks) and payloads
 * to enqueue into the pool — are collected here and processed by the caller
 * AFTER the graph mutex is released (overhaul spec F10).
 */
struct TaskGraphActions {
    struct StatusEvent {
        TaskId Id;
        TaskStatus NewStatus;
    };

    /** Status flips to mirror into TaskHandle state, in propagation order. */
    Vector<StatusEvent> Events;

    /** Payloads whose last dependency just completed; enqueue after unlock. */
    Vector<UniquePtr<Task>> ReadyTasks;

    /**
     * Parked payloads of failed/cancelled dependents. They never run; their
     * destructors (user closures) must execute outside the graph mutex.
     */
    Vector<UniquePtr<Task>> RetiredTasks;
};

/**
 * @brief Dependency bookkeeping for graph-managed tasks.
 *
 * Design (overhaul spec §3 slice 2, F7–F11): the pending payload is parked IN
 * the graph node, and both sides of the dependency handshake are one critical
 * section each:
 *  - Register: node creation + edge registration (absent dependency ⇒ already
 *    terminal, F8) + readiness check + park. If the task is ready it is
 *    returned to the caller for enqueue after unlock.
 *  - MarkCompleted: terminal flip + dependents' remaining-dependency
 *    decrement + extraction of newly-ready payloads + erase of every node
 *    made terminal.
 * Terminal nodes are erased in the same critical section that makes them
 * terminal, so the graph never grows beyond in-flight tasks (B4) and
 * completion never rescans the task table. A dependency that completes
 * concurrently with its dependent's Register is observed by exactly one side
 * (B8): either Register parks against the still-present node, or the node is
 * already erased and the dependency counts as terminal.
 */
class TaskDependencyGraph {
public:
    TaskDependencyGraph() = default;
    ~TaskDependencyGraph();

    /**
     * @brief Submit-side critical section (F9).
     *
     * Creates the node, registers edges to present dependencies, and either
     * returns the payload (ready — enqueue after unlock) or parks it in the
     * node until the last dependency completes.
     *
     * Absent dependencies are treated as already terminal (F8): dependents of
     * completed, failed, or cancelled tasks run and observe results/failures
     * through their handles. Debug builds assert that an absent dependency id
     * is recently terminal (F7) — a never-submitted id is a submit-order
     * violation that would otherwise silently run the dependent early.
     */
    UniquePtr<Task> Register(UniquePtr<Task> task);

    /**
     * @brief Completion-side critical section (F9).
     *
     * Flips the task terminal (Completed or Failed), decrements each
     * dependent's remaining-dependency count and extracts newly-ready
     * payloads (success), or propagates the failure to pending dependents
     * (failure), then erases every node made terminal.
     */
    TaskGraphActions MarkCompleted(TaskId taskId, bool success);

    /**
     * @brief Cancel a Pending task (parked or already queued).
     *
     * Retires the parked payload (if any), propagates cancellation to pending
     * dependents, and erases every node made terminal.
     *
     * @return True if the task was Pending and is now cancelled. Running,
     *         terminal (erased), and untracked tasks return false.
     */
    bool CancelPending(TaskId taskId, TaskGraphActions& actions);

    /**
     * @brief Cancel every Pending task (shutdown, F13 step 4 / B6).
     *
     * Retires all parked payloads and emits Cancelled events for every node
     * cancelled, erasing them. Called after the pool's workers are joined, so
     * no node can be Running.
     */
    void CancelAllPending(TaskGraphActions& actions);

    /**
     * @brief Flip a task Pending → Running.
     * @return True if the task is tracked (doubles as the membership check on
     *         the execute path); false for cancelled-while-queued or
     *         non-graph tasks.
     */
    bool MarkRunning(TaskId taskId);

    /**
     * @brief Ensure `taskId` has a graph node, creating a payload-less
     * MIRROR node (Pending, no dependencies, no parked payload) if absent.
     *
     * Mirrors are how Submit(F&&, deps) makes a live NON-graph task (a plain
     * Submit(F&&) lambda) usable as a dependency: dependents register edges
     * against the mirror, and the pool bridges the real task's terminal flip
     * into MarkCompleted / CancelPending on this id (the TaskData::GraphLinked
     * protocol). A mirror is an ordinary node to every other operation —
     * cascades, cancellation, shutdown retire, and the eager terminal erase
     * all apply unchanged.
     *
     * Callers must pre-filter GRAPH-managed ids (TaskData::GraphManaged) and
     * never route them here: a cascade-retired graph task is node-absent
     * while its TaskData is still Pending (ProcessGraphActions flips it
     * outside the graph mutex), and creating a mirror for it strands
     * dependents — nothing ever completes a graph task's mirror.
     *
     * @return True if a mirror was created (the caller arms the bridge and
     *         runs the terminal recheck); false if the id was already
     *         mirrored by an earlier Submit(fn, deps) — the caller adds a
     *         plain edge; the mirror's creator owns the bridge arming.
     */
    bool EnsureTracked(TaskId taskId);

    /**
     * @brief Check whether a task is currently tracked (test visibility).
     */
    bool TaskExists(TaskId taskId) const;

    /**
     * @brief Test hook: number of tracked nodes.
     *
     * Terminal nodes are erased eagerly, so this returns to zero once every
     * graph-managed task has reached a terminal state (the B4 leak tripwire).
     */
    size_t GetTrackedTaskCountForTests() const;

private:
    struct TaskNode {
        std::unordered_set<TaskId> Dependencies;
        std::unordered_set<TaskId> Dependents;
        size_t RemainingDependencies = 0;
        TaskStatus Status = TaskStatus::Pending;
        UniquePtr<Task> Parked; // non-null only while dependencies remain
    };

    /**
     * @brief Flip pending dependents of a terminal task to failureStatus.
     *
     * Iterative cascade: each flipped dependent's parked payload is retired,
     * its own dependents are visited, and its node is erased.
     */
    void PropagateFailureLocked(const TaskNode& fromNode, TaskStatus failureStatus, TaskGraphActions& actions);

#if GE_DEBUG_INSTRUMENTATION
    /** F7: absent dependencies must be recently terminal, not never-submitted. */
    bool IsRecentlyTerminalLocked(TaskId taskId) const;
    void RecordTombstoneLocked(TaskId taskId);
    /** B11: bounded DFS asserting an edge cannot close a dependency cycle. */
    void AssertNoCycleLocked(TaskId taskId, TaskId dependencyId) const;

    // 8192 sized against the flagship 10k-asset-scan workload: a submitter
    // preempted for a full scheduler quantum (~15ms) between submitting a
    // dependency and its dependent must not see the dependency's tombstone
    // evicted by other workers' retirements in the gap (audit finding on the
    // original 256 — a false abort on legitimate submit order). Debug-only
    // memory: 64KB.
    static constexpr size_t kTombstoneRingSize = 8192;
    std::array<TaskId, kTombstoneRingSize> m_Tombstones{};
    size_t m_TombstoneNext = 0;
#endif

    // LOCK-ORDER RULES (binding, overhaul spec F10):
    //  - NEVER enqueue into the thread pool while holding m_Mutex.
    //    EnqueueTask acquires the pool's globalMutex for wake notifies
    //    (slice 1); nesting that acquisition under the graph mutex is a
    //    lock-order hazard with any future reverse path.
    //  - NEVER fire TaskHandle/user callbacks while holding m_Mutex. A
    //    callback that Submits re-enters Register and self-deadlocks.
    // All mutations therefore RETURN their side effects (TaskGraphActions)
    // for the caller to process after unlock.
    mutable std::mutex m_Mutex;
    std::unordered_map<TaskId, TaskNode> m_Tasks;

    TaskDependencyGraph(const TaskDependencyGraph&) = delete;
    TaskDependencyGraph& operator=(const TaskDependencyGraph&) = delete;
};

} // namespace JobSystem
