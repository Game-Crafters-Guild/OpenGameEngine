#pragma once

#include "JobSystem/Types.h"

#include <vector>

namespace JobSystem
{

/**
 * @brief What the pool is doing: its threads and what each one runs, its
 * lanes, its channels and its cumulative queue traffic.
 * WorkStealingThreadPool::GetStatistics() fills it; the editor's
 * get_job_system debug method reports it.
 *
 * Every gauge is a relaxed read taken while the pool runs, so two fields can
 * describe slightly different moments. The cumulative counters only grow.
 * An inline pool (0 workers) executes everything at publish and reports
 * zeroes.
 */
struct JobSystemStatistics
{
    uint32 ComputeWorkers = 0;
    // Blocking threads created so far for the pool's JobChannels; they are
    // never retired.
    uint32 BlockingThreads = 0;

    struct Lane
    {
        uint32 Queued = 0;  // jobs waiting in the lane
        uint32 Running = 0; // compute workers running a job of this class
    };
    Lane Normal;
    Lane Background;

    struct Channel
    {
        const char* Name = nullptr; // the channel's static literal
        uint32 Queued = 0;          // jobs waiting in the channel past its cap
        uint32 Running = 0;         // jobs holding one of its running slots
        uint32 Cap = 0;             // JobChannelDesc::MaxRunning
        uint64 QueueWaitNs = 0;     // cumulative time jobs waited in the channel for a slot
        uint64 Jobs = 0;            // cumulative jobs that took a running slot
    };
    // Every registered channel, in registration order.
    std::vector<Channel> Channels;

    enum class ThreadKind : uint8
    {
        Worker,   // a compute worker, "Job Worker #<Index>"
        Blocking, // a blocking thread, "Job Blocking #<Index>"
    };
    struct Occupancy
    {
        ThreadKind Kind = ThreadKind::Worker;
        uint32 Index = 0;
        // What the thread runs: "Normal" or "Background" on a compute worker,
        // the channel's name on a blocking thread; null when it is idle.
        const char* Running = nullptr;
        // When the running job started, in steady_clock nanoseconds since its
        // epoch: set for Background and channel jobs, 0 for Normal jobs (the
        // pool reads the clock only for the long-running kinds) and when idle.
        uint64 SinceNs = 0;
    };
    // One entry per compute worker, then one per blocking thread created so
    // far.
    std::vector<Occupancy> Threads;

    // Jobs dequeued from the compute lanes (GlobalPops + LocalPops + StealPops
    // + BackgroundPops) plus the channel jobs the blocking threads ran. Counts
    // dequeues, so a handle job the shutdown drain cancelled and a fork stub
    // whose job a participating Wait already ran are in it.
    uint64 TasksExecuted = 0;

    // The queue-topology census: where jobs were published and consumed.
    uint64 GlobalPushes = 0;          // into the global queue (single, bulk, graph tasks from outside the pool)
    uint64 LocalPushes = 0;           // graph tasks published on a worker, into its local queue
    uint64 BackgroundPushes = 0;      // into the Background lane (single, bulk)
    uint64 GlobalPops = 0;            // from the global queue
    uint64 LocalPops = 0;             // from a worker's own local queue
    uint64 StealPops = 0;             // stolen from another worker's local queue
    uint64 BackgroundPops = 0;        // from the Background lane
    uint64 StealMisses = 0;           // steal scans (the half-ring cutoff included) that found nothing
    uint64 EmptyLoopsWithBacklog = 0; // worker laps that found nothing while jobs were queued
    uint64 BackstopTimeouts = 0;      // sleeps that expired on the backstop with no job queued
};

} // namespace JobSystem
