#pragma once

#include "JobChannelState.h"
#include "JobSystem/Types.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace JobSystem
{
class TaskBase;

namespace Detail
{

/**
 * @brief The pool's blocking threads: one FIFO of channel jobs that already
 * hold a channel slot, served by threads that run nothing else.
 *
 * Threads are created on demand when a job arrives and no thread is idle, up
 * to min(demand, budget), where the demand is the sum of the registered
 * channels' caps; they are never retired. A thread is spawned outside the
 * FIFO's mutex: Push reserves the spawn under the lock and the new thread
 * enters the list under it afterwards, so the shutdown join can wait for
 * every reservation before it snapshots the list. The FIFO's mutex is a leaf
 * (lock order, jobsystem design 5.7).
 *
 * The threads stay outside the compute workers' wake protocol: they never
 * read or write the queued-task counters, the sleeper and spinner counts or
 * the census. Each one is a pool thread (s_CurrentPool is the pool) that is
 * no compute worker (s_CurrentWorkerId is SIZE_MAX): a job on it publishes to
 * the global lanes like any thread outside the pool, and TaskHandle::Wait's
 * worker assert does not fire there. Each runs its jobs as bodies of their
 * channel and writes its own occupancy slot.
 */
class BlockingThreads
{
  public:
    BlockingThreads(WorkStealingThreadPool& pool, size_t budget);
    ~BlockingThreads();
    BlockingThreads(const BlockingThreads&) = delete;
    BlockingThreads& operator=(const BlockingThreads&) = delete;

    enum class Spawn
    {
        // A dispatched job: create a thread when none is idle, up to
        // min(demand, budget).
        IfNeeded,
        // A destroyed channel's queued Enqueue job: the existing threads take
        // it. One exists, because the channel only queued the job while its
        // cap was full of dispatched jobs.
        Never,
    };

    /**
     * @brief Append a channel job. The pool's shutdown gate is read under the
     * FIFO's mutex: once it is set the job is not taken and is returned to the
     * caller to dispose of; otherwise it returns null.
     */
    UniquePtr<TaskBase> Push(ChannelJob job, Spawn spawn);

    /** @brief A channel registered (+cap) or unregistered (-cap). */
    void AddDemand(int64 delta);

    /**
     * @brief Wake every idle thread after the shutdown gate was set; each one
     * exits without taking further work.
     */
    void WakeForShutdown();

    /**
     * @brief Join every thread. Waits for the spawns reserved before the gate
     * to enter the list first, so no thread is created after the join and
     * none outlives the pool.
     */
    void Join();

    /** @brief Move every queued job out of the FIFO (the shutdown drain). */
    void TakeQueued(Vector<ChannelJob>& out);

    /** @brief Threads created so far (they are never retired). */
    uint32 ThreadCount();

    /** @brief Cumulative channel jobs these threads took from the FIFO and ran. */
    uint64 JobsRun();

  private:
    void SpawnReservedThread();
    void ThreadLoop(size_t index);

    WorkStealingThreadPool& m_Pool;
    const size_t m_Budget;

    std::mutex m_Mutex;
    std::condition_variable m_WorkAvailable;
    std::condition_variable m_SpawnsSettled;
    std::deque<ChannelJob> m_Queue;
    Vector<std::thread> m_Threads;
    uint64 m_JobsRun = 0;
    size_t m_Idle = 0;
    size_t m_ReservedSpawns = 0;
    size_t m_NextThreadIndex = 0;
    int64 m_Demand = 0;
};

} // namespace Detail
} // namespace JobSystem
