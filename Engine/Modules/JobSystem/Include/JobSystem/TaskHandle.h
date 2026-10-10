#pragma once

#include "JobSystem/TaskTypes.h"
#include "JobSystem/Types.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <any>
#include <string>
#include <typeinfo>
#include <condition_variable>
#include <exception>

namespace JobSystem {

// Forward declarations
class WorkStealingThreadPool;

#if GE_DEBUG_INSTRUMENTATION
namespace Detail {
/**
 * @brief Debug-only test hook for the completion notify-fence gate: process-
 * wide counts of completion-side CompletionMutex acquisitions (Fences) and
 * gate skips (Skips) across every pool. Relaxed counters, compiled out of
 * release builds so the Release fast path pays nothing for the pin.
 */
struct CompletionFenceStats
{
    std::uint64_t Fences;
    std::uint64_t Skips;
};
CompletionFenceStats GetCompletionFenceStatsForTests();
} // namespace Detail
#endif

namespace Detail {
class HandleTask;
template <typename F>
class SubmitTask;
template <typename F>
class ChannelSubmitTask;
} // namespace Detail

/**
 * @brief Handle for managing task execution, dependencies, and results
 * 
 * TaskHandle provides a clean interface for:
 * - Declaring task dependencies
 * - Non-blocking result access
 * - Completion/failure callbacks
 * - Task status monitoring
 * 
 * This is the primary interface for dependency-aware task management.
 */
class TaskHandle {
public:
    /**
     * @brief Default constructor creates an invalid handle
     */
    TaskHandle();
    
    /**
     * @brief Constructor for valid task handle
     * @param taskId Unique identifier for the task
     * @param jobSystem Pointer to the managing job system
     */
    TaskHandle(TaskId taskId, WorkStealingThreadPool* jobSystem);
    
    /**
     * @brief Copy constructor with reference counting
     */
    TaskHandle(const TaskHandle& other);
    
    /**
     * @brief Move constructor
     */
    TaskHandle(TaskHandle&& other) noexcept;
    
    /**
     * @brief Copy assignment operator
     */
    TaskHandle& operator=(const TaskHandle& other);
    
    /**
     * @brief Move assignment operator
     */
    TaskHandle& operator=(TaskHandle&& other) noexcept;
    
    /**
     * @brief Destructor with automatic cleanup
     */
    ~TaskHandle();
    
    // ========================================
    // CORE INTERFACE
    // ========================================
    
    /**
     * @brief Get the unique task identifier
     * @return TaskId for this task
     */
    TaskId GetId() const { return m_TaskId; }

    /**
     * @brief Check if this handle is valid
     * @return True if handle references a valid task
     */
    bool IsValid() const { return m_TaskId != 0 && m_JobSystem != nullptr; }
    
    /**
     * @brief Check if task has completed successfully
     * @return True if task completed without errors
     */
    bool IsCompleted() const;

    /**
     * @brief Check if task is done (completed, failed, or cancelled)
     * @return True if task is no longer running
     */
    bool IsDone() const;

    /**
     * @brief Check if task is currently running
     * @return True if task is being executed
     */
    bool IsRunning() const;

    /**
     * @brief Check if task has failed
     * @return True if task failed or was cancelled
     */
    bool HasFailed() const;

    /**
     * @brief Get error message if task failed
     * @return Error message string, empty if not failed
     */
    String GetErrorMessage() const;

    // ========================================
    // RESULT ACCESS
    // ========================================
    
    /**
     * @brief Try to get task result without blocking
     * @param result Reference to store the result if available
     * @return True if result was available and stored
     * @note Returns false while the task is not yet Completed AND when the
     *       stored payload is not a T. The second case is a caller bug that
     *       looks exactly like "never completes" from the polling side, so
     *       Debug builds assert on it (task Completed, a result exists, T
     *       doesn't match); Release stays silent-false.
     */
    template<typename T>
    bool TryGetResult(T& result) const;

    // ========================================
    // WAITING AND SYNCHRONIZATION
    // ========================================

    /**
     * @brief Wait for task completion
     * @note This method blocks the calling thread until task completion.
     *       Debug builds assert when the calling thread is a worker of the
     *       handle's pool, whether or not the task has finished: a parked
     *       worker can hold the slot its producer is queued for. A job body
     *       registers OnComplete or submits its continuation with
     *       Submit(fn, deps) instead.
     */
    void Wait();

    // ========================================
    // CALLBACK MANAGEMENT
    // ========================================
    
    /**
     * @brief Register callback for task completion
     * @param callback Function to call when task completes successfully
     */
    void OnComplete(Function<void()> callback);

    /**
     * @brief Register callback for task completion with result access
     * @param callback Function to call with task handle when completed
     */
    void OnComplete(Function<void(const TaskHandle&)> callback);

    /**
     * @brief Register callback for task failure
     * @param callback Function to call when task fails
     */
    void OnFailure(Function<void(const String&)> callback);

    /**
     * @brief Request cancellation of this task.
     *
     * Single-fire arbitration against the executing worker (F12): exactly one
     * of {execute, cancel} happens. Returns true if this call won — the task
     * body never runs and Cancelled callbacks fire exactly once, on this
     * thread. Returns false if the task is already running or terminal; a
     * running task completes normally. See
     * WorkStealingThreadPool::CancelTask for the full contract.
     */
    bool Cancel();

private:
    // ========================================
    // STATE MUTATORS (pool/graph internals ONLY)
    // ========================================
    //
    // Deliberately NOT public. Force-completing a handle from outside the
    // pool races the executing worker's F12 arbitration CAS (Pending ->
    // Running vs Pending -> Cancelled): the enqueued envelope then finds its
    // status already terminal, which is a contract violation (Debug assert)
    // and leaks the graph node in Release. Legitimate writers are the pool's
    // completion/cancel paths (friend WorkStealingThreadPool) and a running
    // Task publishing its own payload (friend Task — the protected
    // Task::PublishResult / PublishException channel, safe because the
    // executing task already won the arbitration). Callers whose value
    // already exists use CreateCompletedHandle(payload) instead.

    /**
     * @brief Store an exception for caller inspection. Does not change status.
     */
    void SetException(std::exception_ptr exception);

    /**
     * @brief Store the result payload for TryGetResult. Does not change status.
     */
    template<typename T>
    void SetResult(const T& result);

    /**
     * @brief Mark task as completed (called by job system)
     */
    void MarkCompleted();

    /**
     * @brief Mark task as failed (called by job system)
     * @param errorMessage Description of the failure
     */
    void MarkFailed(const String& errorMessage);

    /**
     * @brief Mark task as cancelled (called by job system)
     */
    void MarkCancelled();
    /**
     * @brief Internal data structure for task state
     */
    struct TaskData {
        std::atomic<TaskStatus> Status{TaskStatus::Pending};
        std::any Result;
        String ErrorMessage;
        std::exception_ptr Exception{nullptr};

        // Synchronization for blocking operations
        mutable std::mutex CompletionMutex;
        mutable std::condition_variable CompletionCondition;

        // Waiter-count gate for the completion notify fence (the slice-6
        // follow-up diet item). Registered Wait() callers, counted OUTSIDE
        // the mutex: completion skips the CompletionMutex + notify_all
        // entirely when this reads zero — the overwhelmingly common case
        // (pollers, discarded handles, callback consumers). Same seq_cst
        // Dekker discipline as HasCallbacks/GraphLinked below.
        //
        // Protocol ops (S = the seq_cst total order; program order gives
        // W1 < W2 and C1 < C2 in S):
        //   waiter    W1: Waiters.fetch_add(1, seq_cst)   — register FIRST
        //             W2: Status load (seq_cst, via IsDone) — re-check
        //                 terminal AFTER registering; skip the park if done
        //   completer C1: terminal Status store/exchange/CAS (seq_cst) —
        //                 MarkCompleted/MarkFailed's exchange, the F12
        //                 cancel CAS, or CompleteGraphTask's pre-store
        //             C2: Waiters.load(seq_cst) — notify only if non-zero
        //
        // Interleaving table — both orders of the racing pair, no lost
        // wakeup:
        //   C2 < W1 in S (completer read the count first, may skip):
        //     then C1 < C2 < W1 < W2, so W2 reads Status no older than
        //     C1's terminal value — the waiter observes terminal at its
        //     re-check and never parks. The skip was safe.
        //   W1 < C2 in S (waiter registered first):
        //     C2 reads >= 1 (a parked waiter's decrement cannot precede
        //     C2: it only runs after the waiter observes terminal, which
        //     requires C1, and if it observed terminal it no longer needs
        //     the notify) — the completer takes CompletionMutex and
        //     notify_alls. The park itself is the classic monitor shape:
        //     the waiter (re)evaluates IsDone under CompletionMutex, so it
        //     either sees terminal via the mutex ordering and never
        //     blocks, or it is already atomically blocked inside wait()
        //     when the notify lands.
        //
        // WHY seq_cst: each side is a store followed by a load of a
        // DIFFERENT atomic (W1 store -> W2 load; C1 store -> C2 load). The
        // failure mode is StoreLoad reordering on BOTH sides at once —
        // waiter reads stale non-terminal status AND completer reads stale
        // zero waiter count -> park + skip = permanent hang
        // (TaskHandle::Wait has no backstop timeout). acquire/release does
        // not order a store before a later load to a different variable;
        // on x86 the RMW/exchange sides are full fences anyway (zero
        // incremental cost), and the reasoning is required for ARM64.
        //
        // Release-edge note (the slice-2 terminal pre-store lesson): C1 —
        // the terminal Status transition — is ALSO the release edge for
        // the outcome payload (Result/ErrorMessage/Exception are written
        // before it); W2 and the in-park predicate load are the acquire
        // side. C2 carries no payload — it only gates the notify — and the
        // waiter's decrement is not a release edge for anything.
        //
        // Multi-waiter: the count is a sum; each parked-and-still-needing-
        // notify waiter's +1 is visible at C2 (its -1 cannot have run, per
        // the W1 < C2 case), so one registered waiter among many pollers
        // still forces the fence, and notify_all reaches every parked
        // waiter.
        //
        // Recycle: reset (with a Debug assert of 0) in RecycleTaskData —
        // a live waiter holds a SharedPtr ref for the whole Wait(), so the
        // registry drain's use_count()==1 exclusivity proof already
        // guarantees no waiter exists at recycle time.
        std::atomic<std::uint32_t> Waiters{0};

        // Callback storage. CallbackMutex guards the three vectors and the
        // registrar's terminal check only; no callback runs under it, so a
        // callback may register another callback on the same handle.
        Vector<Function<void()>> OnCompleteCallbacks;
        Vector<Function<void(const TaskHandle&)>> OnCompleteWithHandleCallbacks;
        Vector<Function<void(const String&)>> OnFailureCallbacks;

        mutable std::mutex CallbackMutex;

        // Zero-callback fast path (slice 6): stored (seq_cst, under
        // CallbackMutex) the moment ANY callback registers; the Mark*
        // completion paths skip the whole callback pass (CallbackMutex +
        // three vector walks) when it is still false — the overwhelmingly
        // common case. seq_cst Dekker with the status exchange: a registrar
        // stores this flag BEFORE loading Status, a completer exchanges
        // Status BEFORE loading this flag. If the registrar saw a
        // non-terminal status, its store precedes the completer's exchange
        // in the seq_cst order, so the completer's load sees true and runs
        // the pass (the pass takes the vectors out under CallbackMutex, so
        // it serializes against the in-flight registration); otherwise the
        // registrar saw a terminal status and ran the callback itself, after
        // releasing the lock. Either way no callback is lost, and a false
        // read is only ever a task with no registrations.
        std::atomic<bool> HasCallbacks{false};

        // Set (seq_cst) by Submit(F&&, deps) when this NON-graph task becomes
        // a dependency of a graph task: the graph then holds a payload-less
        // mirror node for this id, and the fast path's completion must bridge
        // the terminal flip into the graph (CompleteLinkedDependency). seq_cst
        // Dekker with the completer's status exchange, same shape as
        // HasCallbacks: the linker stores this flag BEFORE re-loading Status;
        // the completer exchanges Status BEFORE loading this flag — at least
        // one side observes the other, so no completed dependency can leave
        // its mirror node dangling. Never set for graph-managed tasks (their
        // completion reaches the graph natively). One relaxed-cost load on
        // the plain-Submit completion path; no lock, no allocation.
        std::atomic<bool> GraphLinked{false};

        // True iff this id is a GRAPH task (set in Submit(UniquePtr<Task>)
        // before the handle escapes, hence relaxed suffices). This bit — NOT
        // graph-node presence — is what LinkSubmitDependency dispatches on:
        // a cascade-retired graph task has NO node but stays Pending on its
        // TaskData until ProcessGraphActions flips it OUTSIDE the graph
        // mutex, so probing the graph in that window would misread a dying
        // graph task as a live non-graph lambda and resurrect a mirror node
        // that nothing ever completes (stranding the dependent forever).
        std::atomic<bool> GraphManaged{false};

        TaskData() = default;
        ~TaskData() = default;

        /**
         * @brief Check if task is in a completed state (completed, failed, or cancelled)
         */
        bool IsDone() const {
            TaskStatus currentStatus = Status.load();
            return currentStatus == TaskStatus::Completed ||
                   currentStatus == TaskStatus::Failed ||
                   currentStatus == TaskStatus::Cancelled;
        }
    };
    
    TaskId m_TaskId{0};
    WorkStealingThreadPool* m_JobSystem{nullptr};
    SharedPtr<TaskData> m_Data;

    /**
     * @brief Adopting constructor (pool-internal, slice 6): binds to an
     * existing TaskData without touching the pool's registry. Used on the
     * Submit(F&&) fast path — ONE GetOrCreateTaskData per task; the returned
     * handle, the envelope, and the execute side adopt that SharedPtr.
     */
    TaskHandle(TaskId taskId, WorkStealingThreadPool* jobSystem, SharedPtr<TaskData> data);

    /**
     * @brief Get or create task data
     */
    SharedPtr<TaskData> GetTaskData() const;

    /**
     * @brief Fire the Cancelled event set (waiter notify + failure callbacks)
     * directly on a TaskData object whose status is already Cancelled.
     *
     * Used by the pool's cancel paths (F12): the arbitration winner must fire
     * on the exact object it CAS'd — resolving a fresh handle by id can
     * re-materialize a different TaskData if the losing worker's deferred
     * registry cleanup already ran, notifying nobody.
     *
     * Callbacks run inline on the cancelling thread — during Shutdown's
     * registry sweep that is the shutting-down thread itself, so task
     * callbacks must not block on other task handles.
     */
    static void FireCancelledOn(const SharedPtr<TaskData>& data);

    /**
     * @brief Gated completion notify (ops C2 + the fence). Call ONLY after
     * the terminal Status transition (op C1) is globally visible — the C1
     * store is what licenses skipping the mutex when Waiters reads zero
     * (full interleaving table at TaskData::Waiters). Skips the
     * CompletionMutex + notify_all entirely when no waiter is registered.
     */
    static void NotifyWaitersAfterTerminal(const SharedPtr<TaskData>& data);

    /**
     * @brief Execute callbacks for status change
     */
    void ExecuteCallbacks(TaskStatus newStatus);
    
    friend class WorkStealingThreadPool;
    // For the protected PublishResult/PublishException channel — a running
    // task is its own payload's single writer (see the mutator block above).
    friend class Task;
    // The Submit(F&&) and JobChannel::Submit envelopes carry the task's
    // TaskData (TaskEnvelopes.h).
    friend class Detail::HandleTask;
    template <typename F>
    friend class Detail::SubmitTask;
    template <typename F>
    friend class Detail::ChannelSubmitTask;
};

// ========================================
// TEMPLATE IMPLEMENTATIONS
// ========================================

template<typename T>
bool TaskHandle::TryGetResult(T& result) const {
    if (!IsValid()) {
        return false;
    }

    auto data = GetTaskData();
    if (!data) {
        return false;
    }

    TaskStatus status = data->Status.load();
    if (status != TaskStatus::Completed) {
        return false;
    }

    try {
        result = std::any_cast<T>(data->Result);
        return true;
    } catch (const std::bad_any_cast&) {
        // Completed with NO stored payload (a void task, or a graph task that
        // never published a result) is a legitimate false. Completed with a
        // payload of a DIFFERENT type is a caller bug — the wrong-T poll
        // returns false forever and masquerades as "never completes".
        assert(!data->Result.has_value() &&
               "TaskHandle::TryGetResult<T>: type mismatch — the task completed with a stored "
               "result whose type is not T; this poll will never succeed");
        return false;
    }
}

template<typename T>
void TaskHandle::SetResult(const T& result) {
    if (!IsValid()) return;

    auto data = GetTaskData();
    if (data) {
        data->Result = result;
    }
}

} // namespace JobSystem
