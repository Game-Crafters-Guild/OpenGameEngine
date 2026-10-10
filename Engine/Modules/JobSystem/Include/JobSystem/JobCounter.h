#pragma once

// JobCounter — caller-owned fork-join counter (slice 5, spec F16-F19).
//
// The ONE completion primitive for fire-and-wait work. Replaces the six
// hand-rolled remaining/mutex/condvar barrier copies (ParallelFor /
// ParallelForEach / ParallelSort / DispatchAndWait, the ECS
// vector<TaskHandle> wave joins, and MaterialSystem's prewarm drain).
//
// Contract (F16, normative):
//   - Add() at SUBMIT, on the submitting thread, BEFORE the task is
//     published. A Wait() issued after the submits on the same thread can
//     therefore never miss a task.
//   - Decrement() once per counted unit, on any thread, after the matching
//     Add(); a driver's completion thread is fine. A pool task's envelope
//     decrements from its completion path through a scope guard, so
//     exceptions always decrement.
//   - Zero-crossing = fetch_sub(acq_rel); a waiter observing zero with an
//     acquire load sees every task's writes (release sequence over the
//     counter RMWs) — task writes happen-before Wait() returns.
//   - The counter must reach zero before it is destroyed (Debug assert).
//     After Wait() returns it may be destroyed immediately or reused for
//     the next wave.
//   - Destruction license is SINGLE-consumer: exactly ONE thread may treat
//     its Wait() return as permission to destroy the counter. A second
//     concurrent external waiter can still be inside the condvar's wait
//     internals when the first destroys the counter — that is UB. Every
//     in-tree counter has a single joining thread; keep it that way, or
//     give the counter external lifetime that outlives all waiters.
//
// Stack-lifetime rationale (carried over VERBATIM from the pre-slice-5
// ParallelAlgorithms barrier; binding):
//   The decrement MUST happen under the mutex: the counter typically lives
//   on the waiting caller's stack, and the caller destroys it the moment it
//   observes count == 0. Decrementing outside the lock lets the waiter see
//   zero (and unwind) while this thread is still entering the notify
//   critical section — a use-after-free on the caller's stack. Holding the
//   mutex across the decrement means zero is only ever published from
//   inside the critical section, so the waiter's own mutex acquisition
//   orders the notifier's completion before any destruction. For the same
//   reason every zero observation that can lead to destruction (the Wait()
//   fast path included) is followed by an empty lock/unlock of the same
//   mutex before returning.
//
// Failure semantics (F19): there is NO per-task failure observation. A
// throwing task is logged, flips the sticky HasAnyFailed() flag (debug
// parity), and always decrements. This matches the pre-slice-5 ECS wave
// behavior, where nobody read HasFailed on wave handles (verified by audit).
// The flag is sticky WITHIN a counter lifetime unless Reset() re-arms it at
// quiescence — without Reset, a reused counter's later waves inherit an
// earlier wave's failure.

#include "JobSystem/Types.h"
#include "Logger/Logger.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 6326 6011)
#endif

#if __has_include(<concurrentqueue/moodycamel/concurrentqueue.h>)
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#elif __has_include(<concurrentqueue/concurrentqueue.h>)
#include <concurrentqueue/concurrentqueue.h>
#elif __has_include(<concurrentqueue.h>)
#include <concurrentqueue.h>
#else
#error "moodycamel concurrentqueue headers not found - ensure vcpkg concurrentqueue package is installed"
#endif

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace JobSystem
{

class JobCounter
{
  public:
    JobCounter() = default;

    ~JobCounter()
    {
        assert(m_Count.load(std::memory_order_acquire) == 0 &&
               "JobCounter destroyed with outstanding tasks — Wait() must complete first");
        // m_Jobs may legitimately outlive this counter: pool-side stubs hold
        // their own reference and find the queue EMPTY (any queued envelope
        // would be an outstanding count, excluded by the assert). The last
        // reference returns the queue for reuse (ReleaseTaggedJobs).
    }

    /**
     * @brief Count `n` tasks into the counter. F16: call at submit, on the
     * submitting thread, BEFORE publishing the tasks. Balanced by exactly one
     * Decrement() per counted task, on any thread.
     */
    void Add(uint32 n = 1)
    {
        m_Count.fetch_add(n, std::memory_order_seq_cst);
    }

    /**
     * @brief Release one counted unit: once per counted unit, on any thread,
     * after the matching Add(); a driver's completion thread is fine. Release
     * on the throwing path too (use a scope guard). The zero-publishing
     * decrement happens under the counter's mutex; see the stack-lifetime
     * rationale in the header comment.
     */
    void Decrement()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Count.fetch_sub(1, std::memory_order_acq_rel) == 1 && m_Waiters > 0)
        {
            m_Cv.notify_all();
        }
    }

    /**
     * @brief Lock-free zero probe. NOTE: observing zero here does NOT order a
     * concurrent notifier's critical section before the caller — never destroy
     * the counter off a bare IsZero() read while other threads may still be
     * decrementing. WorkStealingThreadPool::Wait() performs the required
     * synchronizing lock; use it as the join point.
     */
    bool IsZero() const
    {
        return m_Count.load(std::memory_order_acquire) == 0;
    }

    /**
     * @brief F19 sticky failure flag: true if any task counted into this
     * counter threw. No per-task attribution. Valid to read after Wait().
     * Sticky across waves unless Reset() re-arms the counter.
     */
    bool HasAnyFailed() const
    {
        return m_AnyFailed.load(std::memory_order_acquire);
    }

    /**
     * @brief Re-arm a reused counter for its next wave: clears the sticky
     * HasAnyFailed() flag. Legal ONLY at quiescence — count == 0 and no
     * parked waiters (both Debug-asserted): a Reset concurrent with live
     * tasks or waiters races the very failure writes it is clearing. Read
     * (and act on) HasAnyFailed() BEFORE calling Reset.
     */
    void Reset()
    {
#ifndef NDEBUG
        assert(m_Count.load(std::memory_order_acquire) == 0 &&
               "JobCounter::Reset with outstanding tasks — Wait() must complete first");
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            assert(m_Waiters == 0 && "JobCounter::Reset with parked waiters");
        }
#endif
        m_AnyFailed.store(false, std::memory_order_release);
    }

    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;

  private:
    friend class WorkStealingThreadPool;

    // Type-erased tagged-task envelope. Lives in the counter's own MPMC queue
    // (F17: filtered consumption — a participating waiter executes ONLY these,
    // never the pool's general queues). The envelope references the counter
    // directly: it only ever runs while counted, and count reaching zero
    // requires every envelope to have already executed, so the reference can
    // not dangle past Wait().
    struct IJob
    {
        virtual ~IJob() = default;
        virtual void Execute() = 0;
    };

    // Non-final: Run() wraps this in PooledEnvelope<Job<F>> (TaskSlabPool.h)
    // so the envelope allocates from the 128-byte slab freelist; the deleting
    // destructor of that most-derived wrapper is what routes the UniquePtr's
    // delete back to the pool.
    template <typename F>
    struct Job : IJob
    {
        JobCounter* Counter;
        F Fn;

        template <typename Fn2>
        Job(JobCounter* counter, Fn2&& fn) : Counter(counter), Fn(std::forward<Fn2>(fn)) {}

        void Execute() override
        {
            // F16: the guard decrements on EVERY exit path, including unwind.
            // The sticky failure flag is set inside the catch, i.e. BEFORE the
            // guard's decrement, so a waiter released by this task's zero-
            // crossing already observes it.
            struct DecrementGuard
            {
                JobCounter* C;
                ~DecrementGuard() { C->Decrement(); }
            } guard{Counter};

            try
            {
                Fn();
            }
            catch (const std::exception& e)
            {
                Counter->m_AnyFailed.store(true, std::memory_order_release);
                Logger::Log::Error("JobCounter task failed: {}", e.what());
            }
            catch (...)
            {
                Counter->m_AnyFailed.store(true, std::memory_order_release);
                Logger::Log::Error("JobCounter task threw a non-standard exception");
            }
        }
    };

    // The tagged-task queue is shared between the counter and the pool-side
    // stubs: a stub can outlive a stack-owned counter (the waiter may have
    // consumed the stub's envelope, and Wait() returns at zero without waiting
    // for stub retirement), so stubs reach the queue through their own
    // TaggedJobsRef — never through the counter.
    //
    // Queues are recycled: a fresh moodycamel queue costs four heap
    // allocations (the queue, its first block, its producer and the
    // producer's block index), and every ECS wave and parallel query forks
    // on a fresh stack counter. AcquireTaggedJobs takes the forking thread's
    // cached queue, else one from a fixed shared pool, and constructs one
    // only when both are empty; the last TaggedJobsRef gives it back whole,
    // its blocks intact and JobCount zero. Because the last reference may be
    // a late stub's, a stub never reaches a queue another counter is using.
    //
    // JobCount is the authoritative occupancy signal, maintained OUTSIDE the
    // moodycamel queue: incremented by the producer BEFORE its enqueue (the
    // F5 direction — over-count is the safe side; a producer whose enqueue
    // FAILS un-counts its increment inside the same critical section and
    // executes the envelope inline, see Run()), decremented by a consumer
    // AFTER its successful dequeue. It exists because moodycamel's
    // try_dequeue can fail spuriously while a committed element exists (the
    // consumer-side overcommit race under concurrent consumers). The pool's
    // own queues tolerate that via the workers' backstop re-poll; this
    // queue's consumers are one-shot stubs, so a spurious failure would
    // no-op a stub and strand its envelope forever (count never reaches
    // zero, the waiter parks for good — observed live as an ECS sweep hang
    // at 32 workers). RunOneTagged therefore retries until it either pops an
    // envelope or can PROVE emptiness via JobCount == 0.
    struct TaggedJobs
    {
        // One block to start with: the queue grows to a counter's high-water
        // mark and, recycled, keeps those blocks for the next counter.
        moodycamel::ConcurrentQueue<IJob*> Queue{moodycamel::ConcurrentQueue<IJob*>::BLOCK_SIZE};
        // The one producer every Run enqueues through. Run enqueues under
        // m_Mutex, so the token never serves two threads at once, and a
        // consumer's dequeue scans one producer however many threads have
        // forked into the queue over its life. (A tokenless enqueue adds an
        // implicit producer per thread, which the queue keeps for its life
        // and every dequeue scans.)
        moodycamel::ProducerToken Producer{Queue};
        std::atomic<size_t> JobCount{0};
        // Live TaggedJobsRef count: the counter's, one per queued stub, and a
        // participating waiter's local. Zero means the queue is cached or
        // pooled (or about to be).
        std::atomic<uint32> RefCount{0};

        ~TaggedJobs()
        {
            // Every envelope is consumed exactly once (stub or participating
            // waiter) before its counter reaches zero; a leftover here means a
            // counted task never ran, which the JobCounter dtor assert already
            // rules out. Delete defensively anyway so Release can't leak.
            IJob* job = nullptr;
            while (Queue.try_dequeue(job))
            {
                assert(false && "JobCounter: unexecuted tagged envelope at queue destruction");
                delete job;
            }
        }
    };

    // Owning reference to a TaggedJobs. Copies count up; the destructor of the
    // last one hands the queue to ReleaseTaggedJobs. A std::shared_ptr cannot
    // recycle allocation-free: a custom deleter allocates a control block per
    // queue, and allocate_shared destroys the queue (and frees its blocks)
    // at zero.
    class TaggedJobsRef
    {
      public:
        TaggedJobsRef() = default;

        // Adopts the reference AcquireTaggedJobs counted in.
        explicit TaggedJobsRef(TaggedJobs* jobs) noexcept : m_Jobs(jobs) {}

        TaggedJobsRef(const TaggedJobsRef& other) noexcept : m_Jobs(other.m_Jobs)
        {
            if (m_Jobs)
            {
                m_Jobs->RefCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        TaggedJobsRef(TaggedJobsRef&& other) noexcept : m_Jobs(std::exchange(other.m_Jobs, nullptr)) {}

        TaggedJobsRef& operator=(TaggedJobsRef other) noexcept
        {
            std::swap(m_Jobs, other.m_Jobs);
            return *this;
        }

        ~TaggedJobsRef()
        {
            // acq_rel: every holder's queue operations happen-before the
            // release that lets another counter reuse the queue.
            if (m_Jobs && m_Jobs->RefCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                ReleaseTaggedJobs(m_Jobs);
            }
        }

        explicit operator bool() const noexcept { return m_Jobs != nullptr; }
        TaggedJobs& operator*() const noexcept { return *m_Jobs; }
        TaggedJobs* operator->() const noexcept { return m_Jobs; }

      private:
        TaggedJobs* m_Jobs = nullptr;
    };

    // Takes the calling thread's cached queue, else one from the shared pool,
    // else constructs one, with RefCount 1 (the caller's TaggedJobsRef adopts
    // it). JobCounter.cpp.
    static TaggedJobs* AcquireTaggedJobs();

    // Caches a queue whose last reference dropped on the calling thread, or
    // pools it, or deletes it when both are full. JobCount must be zero
    // (Debug assert). Allocation-free and noexcept: stubs release on worker
    // threads, including during static destruction.
    static void ReleaseTaggedJobs(TaggedJobs* jobs) noexcept;

    // The calling thread's cached queue and its return to the shared pool at
    // thread exit; thread_local state in JobCounter.cpp.
    struct ThreadTaggedJobs;

    // Pops and executes ONE tagged envelope; returns false only when the
    // queue is provably empty (JobCount == 0). Used by the pool-side stub and
    // by participating waiters — the only two consumers of tagged tasks.
    //
    // The retry loop terminates: JobCount > 0 implies a committed element
    // (some retry succeeds once the concurrent operations hiding it retire),
    // a producer mid-publish (its enqueue completes independently), or
    // another consumer between its dequeue-success and its decrement (that
    // decrement lands and the load observes zero). Each miss yields. The
    // producer window (occupancy increment -> enqueue commit) is NOT a
    // few-instruction gap: the enqueue can take a block allocation and the
    // producer can be preempted arbitrarily inside it, so under adversarial
    // scheduling this is a yield-spin bounded by producer progress, not by
    // instruction count — it can burn cycles until the hidden element
    // commits or the count drains, but it can never deadlock.
    static bool RunOneTagged(TaggedJobs& jobs)
    {
        IJob* raw = nullptr;
        while (!jobs.Queue.try_dequeue(raw))
        {
            if (jobs.JobCount.load(std::memory_order_acquire) == 0)
            {
                return false;
            }
            std::this_thread::yield();
        }
        jobs.JobCount.fetch_sub(1, std::memory_order_acq_rel);
        UniquePtr<IJob> owned(raw);
        owned->Execute();
        return true;
    }

    // Lazily acquires the tagged queue (pure-barrier counters — ParallelFor and
    // friends — never pay for it). Guarded by the counter mutex: concurrent
    // first-Run() races and the participating waiter's re-read (under the same
    // mutex in its wait predicate) are both serialized here. Run() calls this
    // BEFORE it counts the task in (all allocations precede Add, so a throw
    // can never leave the counter over-counted) and skips it entirely on the
    // warm path via m_JobsCreated.
    void EnsureJobs()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Jobs)
        {
            m_Jobs = TaggedJobsRef(AcquireTaggedJobs());
            m_JobsCreated.store(true, std::memory_order_release);
        }
    }

    std::atomic<uint32> m_Count{0};
    std::atomic<bool> m_AnyFailed{false};
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    uint32 m_Waiters = 0; // guarded by m_Mutex
    TaggedJobsRef m_Jobs; // lazily acquired; writes/reads under m_Mutex
    // Warm-path hint for Run(): true once m_Jobs is set (it is never reset),
    // so repeat Runs skip EnsureJobs' lock and pay exactly ONE mutex
    // acquisition — the publish critical section. Correctness never rests on
    // this flag: m_Jobs itself is only ever read under m_Mutex.
    std::atomic<bool> m_JobsCreated{false};
};

} // namespace JobSystem
