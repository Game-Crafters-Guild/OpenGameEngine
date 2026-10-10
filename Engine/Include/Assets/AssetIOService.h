#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Types/Types.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace JobSystem
{
class TaskHandle;
class WorkStealingThreadPool;
} // namespace JobSystem

namespace GameEngine
{

class AssetDecodeGate;

/**
 * @brief Dedicated blocking-read front-end for asset loads.
 *
 * Owns a small number of reader threads that do exactly one thing: claim the
 * highest-priority read request the decode gate admits, run the blocking file
 * read (including the hot-reload rename/write retry window), then submit the
 * CPU continuation to the JobSystem as a JobPriority::Background job and forget. Blocking file
 * I/O never runs on JobSystem workers; the continuation's TaskHandle is the
 * load's completion and cancellation surface.
 *
 * Exactly one of OnSubmitted / OnFailure fires for every accepted request:
 * OnSubmitted after the continuation is submitted, OnFailure when no
 * continuation will ever exist (read failure, JobSystem shutdown gate,
 * service stopped). SubmitRead on a stopped service fails the request inline
 * on the calling thread.
 *
 * Shutdown ordering: Stop() must run before the JobSystem pool shuts down —
 * the reader threads are Submit producers. A submit that loses the race
 * against pool shutdown returns an invalid handle and is routed to OnFailure.
 *
 * When the pool runs in inline mode (0 workers, e.g. single-threaded wasm),
 * no reader threads are spawned: SubmitRead runs the read and its decode
 * continuation synchronously on the calling thread.
 */
class AssetIOService
{
  public:
    struct ReadRequest
    {
        GUID AssetGuid;
        AssetMetadata Metadata;
        AssetLoadPriority Priority = AssetLoadPriority::Normal;

        // A byte range of Metadata.Path to read instead of the whole file: RangeBytes bytes from
        // RangeOffset (a page of a page store). The range must lie inside the file; a short read
        // fails the request. RangeBytes 0 reads the whole file.
        uint64 RangeOffset = 0;
        uint64 RangeBytes = 0;

        // Shared cancel flag (the in-flight load's CancelRequested). Checked
        // when a reader claims the request and again after the read, before
        // the decode continuation is submitted: a cancelled request resolves
        // through OnFailure and never produces a decode job. Null = not
        // cancellable.
        std::shared_ptr<std::atomic<bool>> CancelRequested;

        // Parses and registers the asset from the raw file bytes. Runs as a
        // JobPriority::Background pool job; the return value becomes the
        // continuation handle's result (null = processing failure).
        Function<SharedPtr<Asset>(Vector<uint8>)> ProcessData;

        // Invoked on the reader thread right after the continuation is
        // submitted, with its TaskHandle — the caller's cancellation surface.
        Function<void(const JobSystem::TaskHandle&)> OnSubmitted;

        // Invoked when no continuation will ever be submitted: read failure
        // (reader thread), pool shutdown gate (reader thread), queue
        // abandonment during Stop() (stopping thread), or SubmitRead on a
        // stopped service (calling thread). The owner must resolve the load
        // here.
        Function<void(const String& error)> OnFailure;
    };

    AssetIOService() = default;
    ~AssetIOService();

    AssetIOService(const AssetIOService&) = delete;
    AssetIOService& operator=(const AssetIOService&) = delete;

    /**
     * @brief Spawn the reader threads (none when the pool is in inline mode;
     * reads then run synchronously on the SubmitRead caller).
     * @param threadCount 0 = GE_ASSET_IO_THREADS env override, default 2.
     */
    void Start(JobSystem::WorkStealingThreadPool& jobSystem, size_t threadCount = 0);

    /**
     * @brief Stop accepting requests, join the reader threads, then fail any
     * requests still queued (their OnFailure runs on the calling thread).
     * A request already claimed by a reader thread runs to completion.
     * Idempotent.
     */
    void Stop();

    /**
     * @brief Enqueue a read at the request's priority (FIFO within a level,
     * among the requests the decode gate admits; see Admission).
     * On a stopped service the request's OnFailure fires inline instead.
     */
    void SubmitRead(ReadRequest request);

    /**
     * @brief Remove the queued reads for one cancelled load: only requests
     * whose CancelRequested is the given flag object (the load generation's
     * identity) are removed; each removed request resolves through OnFailure
     * on the calling thread. The identity match keeps a stale canceller from
     * withdrawing a newer load of the same GUID, whose requests carry a fresh
     * flag. Exactly-once holds by ownership: a request is either popped by a
     * reader (which resolves it) or removed here, never both. A read already
     * claimed by a reader runs to completion — the flag makes the reader
     * discard the result instead of submitting the decode continuation.
     * @return True when at least one queued request was removed.
     */
    bool CancelRequest(const GUID& guid, const std::shared_ptr<std::atomic<bool>>& cancelRequested);

    /**
     * @brief Accepted reads that no reader thread has claimed yet, summed over
     * every priority level. A request leaves this count the moment a reader
     * pops it, which is the point from which CancelRequest can no longer
     * withdraw it and the reader is committed to running it.
     */
    size_t GetQueuedReadCount() const;

    /**
     * @brief The gate every decode claimed here takes a slot from, which texture
     * cooks take their helper slots from too (TextureCookWorkers); set by Start,
     * null in inline mode (no workers to protect) and before Start.
     */
    std::shared_ptr<AssetDecodeGate> GetDecodeGate() const { return m_DecodeGate; }

  private:
    static constexpr size_t kPriorityLevels = 4; // AssetLoadPriority Low..Critical

    // The read queue. A reader claims a request together with its slot in the
    // decode gate (AssetDecodeGate), under this mutex, so it never holds a request
    // it cannot decode: it skips past queued requests whose class the gate has
    // full, and waits here until a request is queued or a slot is released.
    //
    // The gate caps how many pool workers asset work occupies at once. The
    // frame's ECS waves run on the SAME pool; Background priority only orders
    // the queue — it cannot preempt a running cook. A cold-asset storm (bulk
    // import: hundreds of texture/model cooks) otherwise occupies every worker
    // and the main thread parks in SystemManager::UpdateWaveBased for the
    // storm's duration (observed live: 62-minute wedge, AppHangTransient).
    // Texture decodes, and the helper bands of their cooks, take only the gate's
    // texture share: a texture decode that misses the cook cache holds its slot
    // for the whole cook, minutes for a large texture, so without the share a
    // cook storm fills the gate and a short decode the frame waits on (a material
    // during scene resolve) queues behind whole cooks. Texture work therefore
    // never takes the slots those decodes need.
    //
    // Shared with the gate, which wakes the readers on every release, possibly
    // after this service during teardown.
    struct Admission
    {
        std::mutex Mutex;
        std::condition_variable Changed;                 // a request queued, a slot released, or Stop
        std::deque<ReadRequest> Queues[kPriorityLevels]; // indexed by AssetLoadPriority
        bool Running = false;
        bool InlineMode = false; // set in Start(); no reader threads, reads run inline
    };

    void ReaderThreadMain();
    // `holdsDecodeSlot`: the claim took a gate slot for this request, which the
    // read's decode job (or an early failure) releases.
    void ExecuteRead(ReadRequest& request, bool holdsDecodeSlot);

    // The highest-priority queued request the gate admits now (FIFO within a
    // level), taking its slot; false when none is admissible. Admission::Mutex held.
    static bool TryClaim(Admission& admission, AssetDecodeGate& gate, ReadRequest& request);

    // Blocking read with the hot-reload tolerance: retries briefly when the
    // file is mid-rename/write, and accepts stale advisory sizes for
    // re-statable or text-based assets.
    //
    // Non-throwing by contract. A missing or half-written file is a routine
    // outcome on a watched tree (an asset mount can be re-staged underneath a
    // running editor), not an exceptional one, so it is reported through the
    // return value and resolved via OnFailure. Throwing here would put an
    // exception on the reader thread, whose only handler is std::terminate.
    static bool ReadFileWithRetry(const AssetMetadata& metadata, Vector<uint8>& outData, String& outError);

    // One seek + read of a request's byte range; non-throwing, as ReadFileWithRetry. No retry: a
    // range is read from a cooked store, which only its writer replaces, by rename.
    static bool ReadFileRange(const ReadRequest& request, Vector<uint8>& outData, String& outError);

    JobSystem::WorkStealingThreadPool* m_JobSystem = nullptr;
    std::shared_ptr<Admission> m_Admission = std::make_shared<Admission>();
    std::shared_ptr<AssetDecodeGate> m_DecodeGate; // set in Start(); null in inline mode

    std::vector<std::thread> m_Threads;
};

} // namespace GameEngine
