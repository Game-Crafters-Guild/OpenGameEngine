#pragma once

#include "JobSystem/TaskHandle.h"
#include "JobSystem/Types.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <functional>
#include <type_traits>
#include <utility>

namespace JobSystem
{

/**
 * @brief What a JobChannel is: its name and how many of its jobs may run at
 * once.
 */
struct JobChannelDesc
{
    // A static literal (a string that lives for the process): logs and
    // diagnostics name the channel by it.
    const char* Name = nullptr;
    // How many of the channel's jobs run at once; at least 1.
    uint32 MaxRunning = 1;
};

/**
 * @brief A FIFO of blocking jobs owned by the module that owns the work, with
 * a cap on how many of them run at once.
 *
 * A blocking job is one whose thread is off-CPU for most of the job: it waits
 * on another process (a compiler, cmake, a compile server), on the OS or on a
 * driver, and it finishes in bounded time. Such a job on a compute worker
 * holds that worker for its whole duration while the frame's work queues
 * behind it. A channel runs its jobs on the pool's blocking threads instead,
 * which are created on the first dispatch (up to the sum of the registered
 * channels' caps, never above the pool's budget) and never run compute work.
 *
 * Jobs past the cap wait in the channel and hold no thread. Dispatch is FIFO:
 * a queued job starts when a running job of the same channel ends. Two ways
 * in, one FIFO:
 *  - Submit: a job with a handle, with the same semantics as a Submit(F&&)
 *    task on a worker: Cancel() wins while it is queued (F12), a throw fails
 *    the handle, and the handle is usable as a dependency of
 *    Submit(fn, dependencies). For jobs a quit may abandon.
 *  - Enqueue: fire-and-forget and never dropped (the EnqueueWork contract):
 *    a queued Enqueue job runs even when its channel is destroyed or the pool
 *    shuts down first. For work that must happen once asked for, and for
 *    jobs whose scope guards release a counter someone waits on. A throw is
 *    logged and swallowed, as for EnqueueWork.
 *
 * Rules:
 *  - Long-lived: one channel per service, constructed after the pool and
 *    destroyed before it (the pool's destructor asserts that no channel is
 *    still registered). Never one per object or per frame.
 *  - A job must not wait on a job of its own channel: with a cap of 1 that
 *    wait never returns. Waiting on a compute handle is allowed. Waiting on
 *    another channel's job needs a free blocking thread for that job, which
 *    holds only while the pool's budget covers the sum of the registered
 *    channels' caps; below that, the waiter can hold the last thread and the
 *    job it waits on never starts. On the threaded web build (budget 1) no
 *    channel job may wait on another channel's job. The pool's shutdown order
 *    releases both kinds of wait.
 *  - The cap bounds concurrency until the pool's shutdown gate; it is not a
 *    lock. No consumer may use a cap of 1 as mutual exclusion over shared
 *    state.
 *  - Destroying the channel cancels its queued Submit jobs and hands its
 *    queued Enqueue jobs, in order, to the blocking threads, where they run
 *    with no cap and on the threads that already exist; its running jobs
 *    finish and keep the channel's internal state alive until they do.
 *  - After the pool's Shutdown() has started, Submit returns an invalid
 *    handle and Enqueue runs the job on the calling thread. The shutdown
 *    drain cancels queued Submit jobs and runs queued Enqueue jobs on the
 *    thread that called Shutdown().
 *  - Inline pools (0 workers) run a dispatched job on the submitting thread
 *    inside Submit or Enqueue.
 */
class JobChannel
{
  public:
    JobChannel(WorkStealingThreadPool& pool, const JobChannelDesc& desc);
    ~JobChannel();
    JobChannel(const JobChannel&) = delete;
    JobChannel& operator=(const JobChannel&) = delete;

    /**
     * @brief Queue `job` on this channel and get a handle to its outcome.
     * @return The job's handle: cancellable while the job is queued, usable as
     *         a dependency of WorkStealingThreadPool::Submit(fn, dependencies).
     *         Invalid once the pool's Shutdown() has been requested; the job
     *         then never runs. A Submit that passes the gate concurrently with
     *         Shutdown() returns a valid handle that reads Cancelled.
     */
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F&>>>
    TaskHandle Submit(F&& job);

    /**
     * @brief Queue `job` on this channel, fire-and-forget. The job is never
     * dropped: it runs on a blocking thread, or, once the pool's Shutdown()
     * has been requested, on the calling thread inside Enqueue (or on the
     * thread that called Shutdown(), when it was already queued).
     */
    template <typename F, typename = std::enable_if_t<std::is_invocable_v<F&>>>
    void Enqueue(F&& job);

    /**
     * @brief Test hook: run `hook` on the submitting thread after a Submit or
     * an Enqueue has taken a running slot and before it hands the job to the
     * blocking threads. Pass an empty function to clear it.
     */
    void SetBeforeDispatchHookForTests(std::function<void()> hook);

  private:
    // Takes a running slot for the job or appends it to the FIFO, reading the
    // pool's shutdown gate under the channel's mutex, and dispatches it when
    // it took a slot. Returns false when the gate refused the job, which is
    // then disposed of here: a Submit job is cancelled, an Enqueue job runs on
    // this thread.
    bool Admit(UniquePtr<TaskBase> envelope, Detail::ChannelSlot& slot);

    WorkStealingThreadPool& m_Pool;
    SharedPtr<Detail::JobChannelState> m_State;
};

template <typename F, typename>
TaskHandle JobChannel::Submit(F&& job)
{
    if (m_Pool.IsShuttingDown())
    {
        return TaskHandle();
    }
    UniquePtr<Detail::ChannelSubmitTask<std::decay_t<F>>> envelope;
    TaskHandle handle = m_Pool.CreateHandleEnvelope(envelope, std::forward<F>(job), m_State);
    Detail::ChannelSlot& slot = envelope->Slot;
    return Admit(std::move(envelope), slot) ? handle : TaskHandle();
}

template <typename F, typename>
void JobChannel::Enqueue(F&& job)
{
    auto envelope = MakeTaskEnvelope<Detail::ChannelEnqueueTask<std::decay_t<F>>>(std::forward<F>(job), m_State);
    Detail::ChannelSlot& slot = envelope->Slot;
    (void)Admit(std::move(envelope), slot);
}

} // namespace JobSystem
