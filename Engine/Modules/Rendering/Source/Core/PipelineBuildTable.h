#pragma once

// PipelineBuildTable: the single-flight record behind IDevice's pipeline builds.
//
// One row per pipeline-cache key (PipelineCache::Combine*Key) whose build is
// queued, running, or failed. A key that built successfully has no row: the
// concrete cache holds it, and IDevice derives Warm from the cache probe, so a
// row can never claim a pipeline is warm after an invalidation or a device
// rebuild dropped it.
//
// Every build of a key goes through one row, which makes each key build once
// however many threads ask for it:
//   - an asynchronous request queues the row (Queued) and hands the build to the
//     device's dispatcher, tagged with the row's ticket;
//   - the dispatched job claims the row only while it is still Queued under that
//     ticket (Running), so a job whose request was cancelled or taken over does
//     nothing;
//   - a synchronous caller takes a Queued row over and builds it at once, and
//     waits on a Running one, which is always progressing because a running
//     build waits on nothing the table owns.
// A dispatched job that is destroyed without running (the dispatcher dropped it
// at shutdown) releases its Queued row, so the key reads as never requested
// rather than pending forever.
//
// A failed row remembers the pipeline cache's invalidation epoch it failed
// under; once the cache moves past it (hot reload) the key may be requested
// again.

#include "Rendering/Core/PipelineIdentifiers.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace GameEngine::Rendering
{

class PipelineBuildTable
{
  public:
    // Asynchronous request. Ticket is non-zero exactly when the caller must
    // dispatch a build for it (the row was just queued).
    struct RequestResult
    {
        PipelineBuildState State = PipelineBuildState::Pending;
        uint64_t Ticket = 0;
    };
    RequestResult Request(uint64_t key, uint64_t invalidationEpoch);

    // Dispatched job: true when the row is still Queued under `ticket`, which is
    // now Running and the job must build it and call Finish.
    bool ClaimQueued(uint64_t key, uint64_t ticket);

    // Synchronous build. Returns the ticket to build under (the caller then
    // calls Finish), after taking over a Queued row or replacing a Failed one, or
    // nullopt after waiting for another thread's build of the key to end.
    // `waitedBuildFailed` reports whether that build failed.
    std::optional<uint64_t> ClaimForSynchronousBuild(uint64_t key, bool& waitedBuildFailed);

    // Ends a claimed build: a success removes the row, a failure marks it Failed
    // at `invalidationEpoch`. A no-op when the row was cancelled meanwhile.
    void Finish(uint64_t key, uint64_t ticket, bool succeeded, uint64_t invalidationEpoch);

    // A dispatched job destroyed without running: forget its row if it is still
    // Queued under `ticket`.
    void ReleaseUnrun(uint64_t key, uint64_t ticket);

    // Pending for a Queued or Running row, Failed for a row failed at
    // `invalidationEpoch`, nullopt otherwise.
    std::optional<PipelineBuildState> Find(uint64_t key, uint64_t invalidationEpoch) const;

    // Before the device's GPU objects die: forget Queued and Failed rows (their
    // jobs then do nothing, and a build that failed against a dying device is
    // not remembered), and wait for every Running build to end.
    void Quiesce();

  private:
    enum class Status : uint8_t
    {
        Queued,
        Running,
        Failed,
    };
    struct Row
    {
        Status RowStatus = Status::Queued;
        uint64_t Ticket = 0;
        uint64_t FailedAtEpoch = 0;
    };

    mutable std::mutex m_Mutex;
    std::condition_variable m_BuildEnded;
    std::unordered_map<uint64_t, Row> m_Rows;
    uint64_t m_NextTicket = 1;
};

} // namespace GameEngine::Rendering
