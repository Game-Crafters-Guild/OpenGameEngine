#pragma once

// The pool's queue envelopes: the concrete TaskBase types that
// WorkStealingThreadPool's submission paths build. Internal to the module;
// callers submit callables and never name these types.
//
// SubmitGraphTask writes its result through the pool's TaskData registry, so
// the pool's class must be complete here. WorkStealingThreadPool.h includes
// this header after its class definition and before its member templates,
// which instantiate these envelopes; the include below makes this header
// self-contained when it is included first.

#include "JobSystem/Task.h"
#include "JobSystem/TaskHandle.h"
#include "JobSystem/Types.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <type_traits>
#include <utility>

namespace JobSystem
{
namespace Detail
{

/**
 * @brief Envelope base for handle-tracked tasks outside the dependency graph
 * (the Submit(F&&) path). Carries the task's TaskData, so the execute side
 * adopts it instead of looking it up in the pool's registry.
 * kFlagInlineTaskData is set only here, which licenses the
 * static_cast<HandleTask*> off that flag.
 */
class HandleTask : public TaskBase
{
  public:
    const SharedPtr<TaskHandle::TaskData>& GetTaskData() const { return m_Data; }

  protected:
    HandleTask(TaskId taskId, SharedPtr<TaskHandle::TaskData> data)
        : TaskBase(taskId, kFlagNeedsTaskData | kFlagInlineTaskData), m_Data(std::move(data))
    {
    }

  private:
    SharedPtr<TaskHandle::TaskData> m_Data;
};

/**
 * @brief The Submit(F&&) envelope: runs the callable and, for a non-void
 * callable, stores the result on the task's TaskData. The pool's
 * ExecuteTaskOptimized does the handle bookkeeping (the cancel arbitration
 * CAS, the status flips, waiter wakeups, callbacks) off the inline TaskData.
 * F is the decayed closure type.
 */
template <typename F>
class SubmitTask : public HandleTask
{
  public:
    template <typename Fn>
    SubmitTask(TaskId taskId, SharedPtr<TaskHandle::TaskData> data, Fn&& fn)
        : HandleTask(taskId, std::move(data)), m_Fn(std::forward<Fn>(fn))
    {
    }

    void Execute() override
    {
        if constexpr (std::is_void_v<std::invoke_result_t<F&>>)
        {
            m_Fn();
        }
        else
        {
            // Stored before the pool flips the status to Completed (after
            // Execute returns), which is what gates TryGetResult<T>.
            GetTaskData()->Result = m_Fn();
        }
    }

  private:
    F m_Fn;
};

/**
 * @brief A JobChannel job's claim on its channel, carried by the job's
 * envelope. While the job waits in the channel's FIFO the claim holds
 * nothing; once the channel dispatches it the claim holds one of the
 * channel's running slots, and destroying the envelope releases that slot
 * (and lets the channel dispatch the head of its FIFO). The slot is released
 * on destruction, never on execution alone, so a job cancelled at dispatch,
 * drained at shutdown or destroyed with its channel releases exactly the slot
 * it took. Holds a reference to the channel's state, so a job that outlives
 * its channel touches only that state.
 */
class ChannelSlot
{
  public:
    explicit ChannelSlot(SharedPtr<JobChannelState> channel) : m_Channel(std::move(channel)) {}
    ~ChannelSlot();
    ChannelSlot(const ChannelSlot&) = delete;
    ChannelSlot& operator=(const ChannelSlot&) = delete;

    /** @brief The channel took a running slot for this job. Called under the channel's mutex. */
    void MarkDispatched() { m_Dispatched = true; }

  private:
    SharedPtr<JobChannelState> m_Channel;
    // Written under the channel's mutex before the envelope is published to
    // the blocking threads; read by the destructor, which runs after the
    // envelope has passed through that publication or never left the FIFO.
    bool m_Dispatched = false;
};

/**
 * @brief The JobChannel::Submit envelope: a Submit(F&&) envelope that carries
 * its channel slot. The slot is a member of the most derived type, so it is
 * released after the job's completion events have fired and before the
 * closure is destroyed.
 */
template <typename F>
class ChannelSubmitTask : public SubmitTask<F>
{
  public:
    template <typename Fn>
    ChannelSubmitTask(TaskId taskId, SharedPtr<TaskHandle::TaskData> data, Fn&& fn,
                      SharedPtr<JobChannelState> channel)
        : SubmitTask<F>(taskId, std::move(data), std::forward<Fn>(fn)), Slot(std::move(channel))
    {
    }

    ChannelSlot Slot;
};

/**
 * @brief The JobChannel::Enqueue envelope: a bare envelope (no handle, no
 * TaskData) that carries its channel slot. The slot is a member of the most
 * derived type, so it is released after the job ran and before the closure is
 * destroyed.
 */
template <typename F>
class ChannelEnqueueTask : public TaskBase
{
  public:
    template <typename Fn>
    ChannelEnqueueTask(Fn&& fn, SharedPtr<JobChannelState> channel)
        : TaskBase(TaskId{0}, kFlagNone), m_Fn(std::forward<Fn>(fn)), Slot(std::move(channel))
    {
    }

    void Execute() override { m_Fn(); }

  private:
    // Declared before Slot, so it is destroyed after the slot is released.
    F m_Fn;

  public:
    ChannelSlot Slot;
};

/**
 * @brief The Submit(F&&, dependencies) payload: a Task subclass, so graph
 * registration, the failure and cancel cascades and shutdown treat it like
 * any dependency-aware task. F is the decayed closure type.
 */
template <typename F>
class SubmitGraphTask : public Task
{
  public:
    template <typename Fn>
    SubmitGraphTask(WorkStealingThreadPool* jobSystem, Fn&& fn)
        : Task(jobSystem), m_Fn(std::forward<Fn>(fn))
    {
    }

    void Execute() override
    {
        if constexpr (std::is_void_v<std::invoke_result_t<F&>>)
        {
            m_Fn();
        }
        else
        {
            // Stored before CompleteGraphTask's terminal pre-store, which is
            // what gates TryGetResult<T> for released dependents.
            m_JobSystem->GetOrCreateTaskData(GetTaskId())->Result = m_Fn();
        }
    }

  private:
    F m_Fn;
};

/**
 * @brief Bare envelope for fire-and-forget work (EnqueueWork,
 * EnqueueWorkBatch and the Run() stub): no TaskHandle, no TaskData, no
 * dependency graph; the caller owns all synchronization. A TaskBase plus the
 * closure, so every in-tree closure fits a pooled slab (TaskSlabPool.h). F is
 * the decayed closure type.
 */
template <typename F>
class BareWorkTask : public TaskBase
{
  public:
    template <typename Fn>
    explicit BareWorkTask(Fn&& func) : TaskBase(TaskId{0}, kFlagNone), m_Func(std::forward<Fn>(func)) {}

    void Execute() override { m_Func(); }

  private:
    F m_Func;
};

} // namespace Detail
} // namespace JobSystem
