#include "BlockingThreads.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Platform/Thread.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <utility>

namespace JobSystem
{
namespace Detail
{

BlockingThreads::BlockingThreads(WorkStealingThreadPool& pool, size_t budget) : m_Pool(pool), m_Budget(budget)
{
    assert(budget >= 1 && "JobSystem: a pool with compute workers needs a blocking-thread budget of at least 1");
    assert(budget <= kMaxBlockingThreadBudget && "JobSystem: the blocking-thread budget exceeds kMaxBlockingThreadBudget");
}

BlockingThreads::~BlockingThreads()
{
    assert(m_Threads.empty() && "JobSystem: blocking threads destroyed before Join");
}

UniquePtr<TaskBase> BlockingThreads::Push(ChannelJob job, Spawn spawnPolicy)
{
    bool spawn = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Pool.IsShuttingDown())
        {
            return std::move(job.Envelope);
        }
        m_Queue.push_back(std::move(job));
        // At least one thread whatever the demand: a channel destroyed while
        // its last job runs unregisters its cap, and that job's release can
        // still promote work it had queued.
        const size_t limit = std::min(static_cast<size_t>(std::max<int64>(m_Demand, 1)), m_Budget);
        if (spawnPolicy == Spawn::IfNeeded && m_Queue.size() > m_Idle &&
            m_Threads.size() + m_ReservedSpawns < limit)
        {
            ++m_ReservedSpawns;
            spawn = true;
        }
    }
    m_WorkAvailable.notify_one();
    if (spawn)
    {
        SpawnReservedThread();
    }
    return nullptr;
}

void BlockingThreads::AddDemand(int64 delta)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Demand += delta;
    assert(m_Demand >= 0 && "JobSystem: blocking-thread demand underflow");
}

void BlockingThreads::WakeForShutdown()
{
    // The empty critical section pairs with an idle thread's predicate read
    // under the same mutex: either it sees the gate or it is already waiting
    // when the notify lands.
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
    }
    m_WorkAvailable.notify_all();
}

void BlockingThreads::Join()
{
    Vector<std::thread> threads;
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_SpawnsSettled.wait(lock, [this] { return m_ReservedSpawns == 0; });
        threads.swap(m_Threads);
    }
    m_WorkAvailable.notify_all();
    for (std::thread& thread : threads)
    {
        thread.join();
    }
}

void BlockingThreads::TakeQueued(Vector<ChannelJob>& out)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (ChannelJob& job : m_Queue)
    {
        out.push_back(std::move(job));
    }
    m_Queue.clear();
}

uint32 BlockingThreads::ThreadCount()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32>(m_Threads.size());
}

uint64 BlockingThreads::JobsRun()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_JobsRun;
}

void BlockingThreads::SpawnReservedThread()
{
    size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        index = m_NextThreadIndex++;
    }
    std::thread thread;
    try
    {
        thread = std::thread(&BlockingThreads::ThreadLoop, this, index);
    }
    catch (const std::system_error& e)
    {
        bool noThread = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            --m_ReservedSpawns;
            noThread = m_Threads.empty() && m_ReservedSpawns == 0;
        }
        m_SpawnsSettled.notify_all();
        // With no blocking thread at all the queued job would wait for the
        // shutdown drain, and its waiter with it: fail loudly instead.
        std::fprintf(stderr, "JobSystem: creating a blocking thread failed: %s\n", e.what());
        std::fflush(stderr);
        if (noThread)
        {
            std::abort();
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Threads.push_back(std::move(thread));
        --m_ReservedSpawns;
    }
    m_SpawnsSettled.notify_all();
}

void BlockingThreads::ThreadLoop(size_t index)
{
    m_Pool.BindBlockingThread(index);

    char threadName[32];
    std::snprintf(threadName, sizeof(threadName), "Job Blocking #%zu", index);
    GameEngine::Platform::SetCurrentThreadName(threadName);

    std::unique_lock<std::mutex> lock(m_Mutex);
    for (;;)
    {
        ++m_Idle;
        m_WorkAvailable.wait(lock, [this] { return m_Pool.IsShuttingDown() || !m_Queue.empty(); });
        --m_Idle;
        // After the gate the drain owns whatever is queued; a thread only
        // finishes the job it already holds.
        if (m_Pool.IsShuttingDown())
        {
            return;
        }
        ChannelJob job = std::move(m_Queue.front());
        m_Queue.pop_front();
        ++m_JobsRun;
        lock.unlock();
        m_Pool.ExecuteTaskOptimized(std::move(job.Envelope), WorkStealingThreadPool::ChannelContext(job.Channel));
        m_Pool.FlushPendingCleanupOnThisThread();
        lock.lock();
    }
}

} // namespace Detail
} // namespace JobSystem
