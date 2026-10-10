#pragma once

#include "JobSystem/JobSystemStatistics.h"
#include "JobSystem/Types.h"

#include <deque>
#include <functional>
#include <mutex>

namespace JobSystem
{
class TaskBase;

namespace Detail
{
class ChannelSlot;

/**
 * @brief A channel job outside its channel's FIFO: the envelope and the name
 * of the channel it belongs to, which the thread that runs it runs the body
 * as (the blocking threads, the shutdown drain, a refused dispatch).
 */
struct ChannelJob
{
    UniquePtr<TaskBase> Envelope;
    // The channel's static literal.
    const char* Channel = nullptr;
};

/**
 * @brief The state a JobChannel shares with its in-flight jobs: the cap, the
 * running count and the FIFO of jobs past the cap. Owned through a SharedPtr
 * by the channel and by every job's ChannelSlot, so a job that finishes after
 * its channel was destroyed releases its slot into live state.
 *
 * Its mutex is a leaf (lock order, jobsystem design 5.7): decisions are made
 * under it, dispatch happens outside it, and no user code and no pool mutex
 * runs under it. Its reads of the pool's shutdown gate (in Admit and
 * ReleaseSlot) happen under it and the pool's drain empties the FIFO under
 * it, but the drain does not depend on them: the gate read it depends on is
 * BlockingThreads::Push's, under the blocking threads' FIFO mutex, which
 * refuses every dispatch after the drain. A job these reads miss stays in this
 * FIFO until the registry sweep or the channel's destructor; the reads only
 * free its closure earlier.
 */
class JobChannelState
{
  public:
    JobChannelState(WorkStealingThreadPool& pool, const char* name, uint32 maxRunning);

    WorkStealingThreadPool& Pool() const { return m_Pool; }
    const char* Name() const { return m_Name; }
    uint32 MaxRunning() const { return m_MaxRunning; }

    enum class Admission
    {
        Dispatch, // the job took a running slot; the caller dispatches it outside the lock
        Queued,   // the FIFO took the job
        Refused,  // the pool's shutdown gate is set; the caller disposes of the job
    };

    /**
     * @brief Takes a running slot for a new job, or appends it to the FIFO.
     * `envelope` is moved from only on Admission::Queued.
     */
    Admission Admit(UniquePtr<TaskBase>& envelope, ChannelSlot& slot);

    /**
     * @brief A dispatched job's envelope died: frees its running slot and,
     * unless the pool's shutdown gate is set, dispatches the head of the FIFO
     * outside the lock. Once the gate is set the drain alone empties the FIFO.
     */
    void ReleaseSlot();

    /** @brief Moves every queued job out of the FIFO (the drain and the channel's destructor). */
    void TakeQueued(Vector<ChannelJob>& out);

    /** @brief The channel's row of JobSystemStatistics, read under its mutex. */
    JobSystemStatistics::Channel Statistics();

    void SetBeforeDispatchHook(std::function<void()> hook);
    void RunBeforeDispatchHook();

  private:
    struct QueuedJob
    {
        UniquePtr<TaskBase> Envelope;
        // Points into Envelope; valid while the envelope lives.
        ChannelSlot* Slot;
        // When it entered the FIFO (steady_clock ns), for the queue wait.
        uint64 QueuedAtNs;
    };

    WorkStealingThreadPool& m_Pool;
    const char* const m_Name;
    const uint32 m_MaxRunning;

    std::mutex m_Mutex;
    uint32 m_Running = 0;
    std::deque<QueuedJob> m_Queued;
    // Jobs that took a running slot, and the time the promoted ones waited
    // in the FIFO first. The clock is read only for a job that queues.
    uint64 m_Jobs = 0;
    uint64 m_QueueWaitNs = 0;
    std::function<void()> m_BeforeDispatchHookForTests;
};

} // namespace Detail
} // namespace JobSystem
