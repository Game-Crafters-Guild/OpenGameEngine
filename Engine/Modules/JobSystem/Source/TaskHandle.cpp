#include "JobSystem/TaskHandle.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <utility>

namespace JobSystem {

#if GE_DEBUG_INSTRUMENTATION
namespace Detail {
namespace {
// Debug-only fence census for the waiter-count gate (see the test hook in
// TaskHandle.h). Process-wide across pools; relaxed — tests read deltas
// around sequentially-executed phases.
std::atomic<std::uint64_t> g_CompletionFences{0};
std::atomic<std::uint64_t> g_CompletionFenceSkips{0};
} // namespace

CompletionFenceStats GetCompletionFenceStatsForTests()
{
    return {g_CompletionFences.load(std::memory_order_relaxed),
            g_CompletionFenceSkips.load(std::memory_order_relaxed)};
}
} // namespace Detail
#endif

// ========================================
// CONSTRUCTORS AND DESTRUCTORS
// ========================================

TaskHandle::TaskHandle()
    : m_TaskId(0), m_JobSystem(nullptr) {
}

TaskHandle::TaskHandle(TaskId taskId, WorkStealingThreadPool* jobSystem)
    : m_TaskId(taskId), m_JobSystem(jobSystem) {
    // Get shared TaskData from job system to ensure all handles for same task share data
    if (jobSystem && taskId != 0) {
        m_Data = jobSystem->GetOrCreateTaskData(taskId);
    }
}

TaskHandle::TaskHandle(TaskId taskId, WorkStealingThreadPool* jobSystem, SharedPtr<TaskData> data)
    : m_TaskId(taskId), m_JobSystem(jobSystem), m_Data(std::move(data)) {
}

TaskHandle::TaskHandle(const TaskHandle& other)
    : m_TaskId(other.m_TaskId), m_JobSystem(other.m_JobSystem), m_Data(other.m_Data) {
}

TaskHandle::TaskHandle(TaskHandle&& other) noexcept
    : m_TaskId(other.m_TaskId), m_JobSystem(other.m_JobSystem), m_Data(std::move(other.m_Data)) {
    other.m_TaskId = 0;
    other.m_JobSystem = nullptr;
}

TaskHandle& TaskHandle::operator=(const TaskHandle& other) {
    if (this != &other) {
        m_TaskId = other.m_TaskId;
        m_JobSystem = other.m_JobSystem;
        m_Data = other.m_Data;
    }
    return *this;
}

TaskHandle& TaskHandle::operator=(TaskHandle&& other) noexcept {
    if (this != &other) {
        m_TaskId = other.m_TaskId;
        m_JobSystem = other.m_JobSystem;
        m_Data = std::move(other.m_Data);

        other.m_TaskId = 0;
        other.m_JobSystem = nullptr;
    }
    return *this;
}

TaskHandle::~TaskHandle() {
    // Automatic cleanup handled by shared_ptr
}

// ========================================
// STATUS QUERIES
// ========================================

bool TaskHandle::IsCompleted() const {
    if (!IsValid()) return false;

    auto data = GetTaskData();
    if (data) {
        TaskStatus status = data->Status.load();
        return status == TaskStatus::Completed;
    }

    return false;
}

bool TaskHandle::IsDone() const {
    if (!IsValid()) return true; // Invalid handles are considered done

    auto data = GetTaskData();
    if (data) {
        return data->IsDone();
    }

    return true;
}

bool TaskHandle::IsRunning() const {
    if (!IsValid()) return false;

    auto data = GetTaskData();
    if (data) {
        TaskStatus status = data->Status.load();
        return status == TaskStatus::Running;
    }

    return false;
}

bool TaskHandle::HasFailed() const {
    if (!IsValid()) return true; // Invalid handles are considered failed

    auto data = GetTaskData();
    if (data) {
        TaskStatus status = data->Status.load();
        return status == TaskStatus::Failed || status == TaskStatus::Cancelled;
    }

    return true;
}

String TaskHandle::GetErrorMessage() const {
    auto data = GetTaskData();
    if (data && HasFailed()) {
        return data->ErrorMessage;
    }
    return "";
}

// ========================================
// CALLBACK MANAGEMENT
// ========================================

void TaskHandle::OnComplete(Function<void()> callback) {
    auto data = GetTaskData();
    if (!data) return;

    {
        std::lock_guard<std::mutex> lock(data->CallbackMutex);

        // Publish "callbacks exist" BEFORE the terminal check (seq_cst Dekker
        // with the Mark* status exchange — see TaskData::HasCallbacks).
        data->HasCallbacks.store(true, std::memory_order_seq_cst);

        if (data->Status.load(std::memory_order_seq_cst) != TaskStatus::Completed) {
            data->OnCompleteCallbacks.push_back(std::move(callback));
            return;
        }
    }

    // Already completed: run it here, outside the lock, so it may register
    // further callbacks on this handle.
    try {
        callback();
    } catch (const std::exception& e) {
        Logger::Log::Warning("TaskHandle callback execution failed: {}", e.what());
    }
}

void TaskHandle::OnComplete(Function<void(const TaskHandle&)> callback) {
    auto data = GetTaskData();
    if (!data) return;

    {
        std::lock_guard<std::mutex> lock(data->CallbackMutex);

        // Dekker flag first — see the overload above.
        data->HasCallbacks.store(true, std::memory_order_seq_cst);

        if (data->Status.load(std::memory_order_seq_cst) != TaskStatus::Completed) {
            data->OnCompleteWithHandleCallbacks.push_back(std::move(callback));
            return;
        }
    }

    // Already completed: run it outside the lock — see the overload above.
    try {
        callback(*this);
    } catch (const std::exception& e) {
        Logger::Log::Warning("TaskHandle callback execution failed: {}", e.what());
    }
}

void TaskHandle::OnFailure(Function<void(const String&)> callback) {
    auto data = GetTaskData();
    if (!data) return;

    {
        std::lock_guard<std::mutex> lock(data->CallbackMutex);

        // Dekker flag first — see OnComplete.
        data->HasCallbacks.store(true, std::memory_order_seq_cst);

        TaskStatus status = data->Status.load(std::memory_order_seq_cst);
        if (status != TaskStatus::Failed && status != TaskStatus::Cancelled) {
            data->OnFailureCallbacks.push_back(std::move(callback));
            return;
        }
    }

    // Already failed or cancelled: run it outside the lock — see OnComplete.
    try {
        callback(data->ErrorMessage);
    } catch (const std::exception& e) {
        Logger::Log::Warning("TaskHandle failure callback execution failed: {}", e.what());
    }
}

// ========================================
// INTERNAL INTERFACE
// ========================================

bool TaskHandle::Cancel() {
    if (!IsValid() || !m_JobSystem) {
        return false;
    }

    return m_JobSystem->CancelTask(m_TaskId);
}


// Ops C2 + fence of the waiter-count Dekker (full interleaving table at
// TaskData::Waiters). Precondition: the terminal Status transition (C1) is
// already performed by the caller with seq_cst — that store is both the
// Dekker first-op AND the release edge for the outcome payload; this
// function adds no ordering of its own beyond the C2 load.
void TaskHandle::NotifyWaitersAfterTerminal(const SharedPtr<TaskData>& data) {
    if (data->Waiters.load(std::memory_order_seq_cst) == 0) { // (C2)
        // No waiter is registered. A concurrent Wait that has not yet
        // executed its W1 increment re-checks terminal state AFTER
        // registering (W2) and, because C1 < C2 < W1 < W2 in the seq_cst
        // order, observes the terminal status and never parks — skipping
        // the fence is safe (case "C2 < W1" of the table).
#if GE_DEBUG_INSTRUMENTATION
        Detail::g_CompletionFenceSkips.fetch_add(1, std::memory_order_relaxed);
#endif
        return;
    }
#if GE_DEBUG_INSTRUMENTATION
    Detail::g_CompletionFences.fetch_add(1, std::memory_order_relaxed);
#endif
    // A waiter is (or may be) parked: the pre-gate notify fence, verbatim.
    // The empty-to-notify mutex pairs with the waiter's predicate
    // evaluation under the same mutex (the classic monitor argument): the
    // waiter either sees the terminal status in its predicate and never
    // blocks, or is atomically blocked when this notify lands.
    std::lock_guard<std::mutex> lock(data->CompletionMutex);
    data->CompletionCondition.notify_all();
}

void TaskHandle::MarkCompleted() {
    auto data = GetTaskData();
    if (!data) return;

    data->Status.exchange(TaskStatus::Completed); // (C1)

    // Gated waiter notify — see NotifyWaitersAfterTerminal.
    NotifyWaitersAfterTerminal(data);

    // Zero-callback fast path (slice 6): the exchange above precedes this
    // load in the seq_cst order — see TaskData::HasCallbacks for the Dekker
    // pairing with the registrars.
    if (data->HasCallbacks.load(std::memory_order_seq_cst)) {
        ExecuteCallbacks(TaskStatus::Completed);
    }
}

void TaskHandle::MarkFailed(const String& errorMessage) {
    auto data = GetTaskData();
    if (!data) return;

    // ErrorMessage BEFORE the exchange: the terminal flip (C1) is the
    // release edge waiters/pollers acquire the payload through.
    data->ErrorMessage = errorMessage;
    data->Status.exchange(TaskStatus::Failed); // (C1)

    // Gated waiter notify — see NotifyWaitersAfterTerminal.
    NotifyWaitersAfterTerminal(data);

    // Zero-callback fast path — see MarkCompleted.
    if (data->HasCallbacks.load(std::memory_order_seq_cst)) {
        ExecuteCallbacks(TaskStatus::Failed);
    }
}

void TaskHandle::MarkCancelled() {
    auto data = GetTaskData();
    if (!data) return;

    data->Status.exchange(TaskStatus::Cancelled);
    FireCancelledOn(data);
}

void TaskHandle::FireCancelledOn(const SharedPtr<TaskData>& data) {
    // Gated waiter notify. Every caller performed the terminal transition
    // (C1) on this exact TaskData before calling in: MarkCancelled's
    // exchange, the F12 CAS in CancelPendingArbitrated / ProcessGraphActions
    // / the shutdown registry sweep — all seq_cst.
    NotifyWaitersAfterTerminal(data);

    // Zero-callback fast path (slice 6): the caller's terminal CAS/exchange
    // precedes this load — see TaskData::HasCallbacks.
    if (!data->HasCallbacks.load(std::memory_order_seq_cst)) {
        return;
    }

    // Cancelled fires the failure callbacks (mirrors ExecuteCallbacks'
    // Cancelled arm; completion callbacks never fire for cancelled tasks).
    // Taken out under the lock and run after it, as in ExecuteCallbacks.
    Vector<Function<void(const String&)>> onFailure;
    {
        std::lock_guard<std::mutex> lock(data->CallbackMutex);
        onFailure = std::exchange(data->OnFailureCallbacks, {});
    }
    for (const auto& callback : onFailure) {
        try {
            callback(data->ErrorMessage);
        } catch (const std::exception& e) {
            Logger::Log::Warning("TaskHandle failure callback failed: {}", e.what());
        }
    }
}

// ========================================
// PRIVATE HELPERS
// ========================================

SharedPtr<TaskHandle::TaskData> TaskHandle::GetTaskData() const {
    // TaskData comes from the pool's shared registry at construction.
    return m_Data;
}

void TaskHandle::ExecuteCallbacks(TaskStatus newStatus) {
    auto data = GetTaskData();
    if (!data) return;

    // Take the vectors out under the lock and run them after it: a callback
    // may register another callback on this handle, which takes the lock. A
    // registration that finds the lock free after this point sees the
    // terminal status and runs its callback itself, so each callback runs
    // exactly once.
    Vector<Function<void()>> onComplete;
    Vector<Function<void(const TaskHandle&)>> onCompleteWithHandle;
    Vector<Function<void(const String&)>> onFailure;
    {
        std::lock_guard<std::mutex> lock(data->CallbackMutex);
        if (newStatus == TaskStatus::Completed) {
            onComplete = std::exchange(data->OnCompleteCallbacks, {});
            onCompleteWithHandle = std::exchange(data->OnCompleteWithHandleCallbacks, {});
        } else if (newStatus == TaskStatus::Failed || newStatus == TaskStatus::Cancelled) {
            onFailure = std::exchange(data->OnFailureCallbacks, {});
        }
    }

    // Execute completion callbacks
    if (newStatus == TaskStatus::Completed) {
        for (const auto& callback : onComplete) {
            try {
                callback();
            } catch (const std::exception& e) {
                Logger::Log::Warning("TaskHandle completion callback failed: {}", e.what());
            }
        }

        for (const auto& callback : onCompleteWithHandle) {
            try {
                callback(*this);
            } catch (const std::exception& e) {
                Logger::Log::Warning("TaskHandle completion with handle callback failed: {}", e.what());
            }
        }
    }

    // Execute failure callbacks
    if (newStatus == TaskStatus::Failed || newStatus == TaskStatus::Cancelled) {
        for (const auto& callback : onFailure) {
            try {
                callback(data->ErrorMessage);
            } catch (const std::exception& e) {
                Logger::Log::Warning("TaskHandle failure callback failed: {}", e.what());
            }
        }
    }
}

// ========================================
// BLOCKING OPERATIONS
// ========================================

void TaskHandle::Wait() {
    if (!IsValid()) {
        return;
    }

    // Keyed on the thread, not on the handle's state, so it fires whether or
    // not the task has finished: a worker that parks here can hold the pool
    // slot its producer is queued for.
    assert(!WorkStealingThreadPool::IsComputeWorkerOf(m_JobSystem) &&
           "TaskHandle::Wait on a pool worker parks the worker while its producer may be queued "
           "behind it; register OnComplete or submit the continuation with Submit(fn, deps)");

    // `data` (a SharedPtr copy) outlives the whole wait, including the
    // WaiterGuard below — locals destroy in reverse order, so the decrement
    // runs while the ref is still held (recycle-safety, see
    // TaskData::Waiters).
    auto data = GetTaskData();
    if (!data) {
        return;
    }

    // Unregistered fast path: already done, no gate interaction needed.
    if (data->IsDone()) {
        return;
    }

    // Inline publication can defer at the stack bound. Joining such a child
    // must run the owning pool's ready work before parking; no worker exists
    // to wake this waiter. The completed-handle fast path above stays unchanged.
    if (m_JobSystem && m_JobSystem->IsInlineMode()) {
        m_JobSystem->DrainInlineTasksOnThisThread();
        if (data->IsDone())
            return;
    }

    // (W1) Register FIRST, with seq_cst — the Dekker pair with the
    // completer's terminal-store-then-count-load (full interleaving table
    // at TaskData::Waiters).
    data->Waiters.fetch_add(1, std::memory_order_seq_cst);

    // RAII deregistration: the decrement must run on every exit path
    // (terminal at the re-check, normal wake, or a throwing wait). Relaxed
    // suffices — the decrement only ever runs after this waiter observed
    // terminal state, i.e. after C1, so it can never hide a
    // parked-and-still-needing-notify registration from C2 (RMWs on one
    // atomic are totally ordered by modification order regardless).
    struct WaiterGuard {
        std::atomic<std::uint32_t>& Count;
        ~WaiterGuard() { Count.fetch_sub(1, std::memory_order_relaxed); }
    } guard{data->Waiters};

    // (W2) Re-check terminal state AFTER registering. If the completer's C1
    // already landed, we must not park: its C2 may have read Waiters == 0
    // (before our W1) and skipped the notify — but then C1 < C2 < W1 < W2
    // in the seq_cst order, so this load is guaranteed to see the terminal
    // status. IsDone()'s Status load is seq_cst (the default).
    if (data->IsDone()) {
        return;
    }

    // Park. The predicate re-evaluates under CompletionMutex, pairing with
    // the completer's fence (monitor argument — see
    // NotifyWaitersAfterTerminal).
    std::unique_lock<std::mutex> lock(data->CompletionMutex);
    data->CompletionCondition.wait(lock, [data]() {
        return data->IsDone();
    });
}

// ========================================
// EXCEPTION HANDLING
// ========================================

void TaskHandle::SetException(std::exception_ptr exception) {
    auto data = GetTaskData();
    if (!data) return;

    // Store the exception for later inspection by callers, without
    // affecting the task's lifecycle or status transitions.
    data->Exception = exception;
}

} // namespace JobSystem
