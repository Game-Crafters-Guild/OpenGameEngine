#include "JobSystem/JobCounter.h"

namespace JobSystem
{

namespace
{

// Queues the shared pool keeps beyond the threads' own caches: for a nested
// fork on a thread whose cache is already in use, and for a queue whose last
// reference dropped on a thread other than the one that forked (a late
// stub's). A release that finds every slot taken deletes its queue, so the
// process holds at most this many pooled queues plus one cached per thread
// that has forked; each is the queue object, one producer with its block
// index, and the blocks of that queue's high-water mark.
constexpr size_t kSharedTaggedJobsSlots = 64;

// Trivially destructible: stubs release queues on worker threads during
// static destruction, and an exiting thread's cached queue lands here.
std::atomic<void*> s_SharedTaggedJobs[kSharedTaggedJobsSlots];

// Each slot changes hands by one exchange of the whole pointer, so a stale
// read costs a retry, never a queue in two hands.
bool PushShared(void* jobs) noexcept
{
    for (std::atomic<void*>& slot : s_SharedTaggedJobs)
    {
        void* expected = nullptr;
        if (slot.load(std::memory_order_relaxed) == nullptr &&
            slot.compare_exchange_strong(expected, jobs, std::memory_order_release, std::memory_order_relaxed))
        {
            return true;
        }
    }
    return false;
}

void* PopShared() noexcept
{
    for (std::atomic<void*>& slot : s_SharedTaggedJobs)
    {
        if (slot.load(std::memory_order_relaxed) != nullptr)
        {
            if (void* jobs = slot.exchange(nullptr, std::memory_order_acquire))
            {
                return jobs;
            }
        }
    }
    return nullptr;
}

} // namespace

// A fork's last reference is normally the counter's, dropped by the thread
// that forked once its Wait returned (a stub usually retires before the
// waiter it woke runs again), so the next fork on that thread finds its queue
// here and touches nothing shared. The cache and the state are trivially
// destructible and alive for the whole thread. A thread caches only while
// Live: from its first fork, which registers the flush instance, until that
// instance's destructor returns the cached queue to the shared pool at thread
// exit. A release on a thread that never forked (a late stub's worker) or
// that is ending (a stub run from a static destructor) pools or deletes
// instead, so no cached queue outlives its thread.
struct JobCounter::ThreadTaggedJobs
{
    enum class State : uint8
    {
        Unregistered,
        Live,
        Exited,
    };

    static thread_local TaggedJobs* t_Cached;
    static thread_local State t_State;
    static thread_local ThreadTaggedJobs t_Flush;

    ~ThreadTaggedJobs()
    {
        t_State = State::Exited;
        if (TaggedJobs* jobs = std::exchange(t_Cached, nullptr); jobs != nullptr && !PushShared(jobs))
        {
            delete jobs;
        }
    }
};

thread_local JobCounter::TaggedJobs* JobCounter::ThreadTaggedJobs::t_Cached = nullptr;
thread_local JobCounter::ThreadTaggedJobs::State JobCounter::ThreadTaggedJobs::t_State =
    JobCounter::ThreadTaggedJobs::State::Unregistered;
thread_local JobCounter::ThreadTaggedJobs JobCounter::ThreadTaggedJobs::t_Flush;

JobCounter::TaggedJobs* JobCounter::AcquireTaggedJobs()
{
    if (ThreadTaggedJobs::t_State == ThreadTaggedJobs::State::Unregistered)
    {
        // Naming the flush instance registers its destructor with this thread.
        [[maybe_unused]] ThreadTaggedJobs& flush = ThreadTaggedJobs::t_Flush;
        ThreadTaggedJobs::t_State = ThreadTaggedJobs::State::Live;
    }
    TaggedJobs* jobs = std::exchange(ThreadTaggedJobs::t_Cached, nullptr);
    if (jobs == nullptr)
    {
        jobs = static_cast<TaggedJobs*>(PopShared());
    }
    if (jobs == nullptr)
    {
        jobs = new TaggedJobs();
    }
    assert(jobs->JobCount.load(std::memory_order_acquire) == 0 && "JobCounter: recycled tagged queue not empty");
    jobs->RefCount.store(1, std::memory_order_relaxed);
    return jobs;
}

void JobCounter::ReleaseTaggedJobs(TaggedJobs* jobs) noexcept
{
    assert(jobs->JobCount.load(std::memory_order_acquire) == 0 &&
           "JobCounter: tagged queue released with queued envelopes");
    if (ThreadTaggedJobs::t_State == ThreadTaggedJobs::State::Live && ThreadTaggedJobs::t_Cached == nullptr)
    {
        ThreadTaggedJobs::t_Cached = jobs;
        return;
    }
    if (!PushShared(jobs))
    {
        delete jobs;
    }
}

} // namespace JobSystem
