#pragma once

#include "PageStreaming/PageCache.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine
{
class AssetIOService;
}

namespace GameEngine::PageStreaming
{

struct PageSampleRect;

class GeneratedHeightPageProvider;
class PageStoreReader;

/// Where one field's pages come from: a cooked store (read through the asset reader threads) or a
/// generated provider (evaluated on Background jobs). Exactly one is set.
struct PageSource
{
    std::shared_ptr<const PageStoreReader> Store;
    std::shared_ptr<const GeneratedHeightPageProvider> Generated;
};

/// What the residency may spend, for every field of every terrain in the process together.
struct PageResidencyBudget
{
    /// Decoded page samples kept on the CPU between a page's load and its upload (design D3:
    /// 64 MiB); a page's samples are dropped once its cache has taken it. Loaded pages the caches
    /// can place are never dropped before then, so the budget in force is the larger of this and
    /// those pages' bytes.
    uint64 CpuBytes = 64ull * 1024ull * 1024ull;
    uint32 UploadsPerFrame = 64;          ///< new cache assignments a frame, all streams together
    uint32 UploadsPerFrameTeleport = 128; ///< the same after a teleport
    uint32 LoadsInFlight = 64;            ///< page reads and evaluations outstanding, all streams together
};

/// The one page residency of a process: every field of every terrain streams through it under one
/// budget (design D5 and #3042). Each terrain's field is a stream with its own source; the streams
/// of one field (every terrain's height, say) share that field's physical cache (PageCache), whose
/// slots are the field's budget for the profile (design D5), whatever the number of terrains.
///
/// Each frame a stream's owner states the pages its level rule wants (Request). Update then:
/// - drains the loads that finished (a failed read is logged once with its reason and retried with
///   a backoff; only a page absent from its store is never retried);
/// - orders every stream's wanted pages in one order, pinned first, then nearest, then coarsest,
///   and keeps of each field only as many as its cache has slots (what it can place), so the
///   terrains of a field divide its slots by that order;
/// - starts loads in that order up to the loads-in-flight budget, and gives the per-frame upload cap
///   to the placeable loaded pages in that order;
/// - trims the CPU samples to their budget (the larger of PageResidencyBudget::CpuBytes and the
///   placeable pages' bytes), least recently wanted first, never a placeable page. The trim runs only
///   when there is a page to drop, so it is an event, not a per-frame cost.
/// A page's samples stay on the CPU from its load until the Update after its cache took it (the
/// frame its owner uploads them), then are dropped: nothing on the CPU reads a resident page, and a
/// page released from its cache and wanted again is loaded again.
/// Every count it reports is edge-triggered, and a parked camera allocates nothing.
///
/// Main-thread only, except the load completions, which arrive on reader and job threads and are
/// queued under a lock until the next Update.
class PageResidencyManager
{
public:
    PageResidencyManager(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io, const PageResidencyBudget& budget);
    ~PageResidencyManager();

    PageResidencyManager(const PageResidencyManager&) = delete;
    PageResidencyManager& operator=(const PageResidencyManager&) = delete;

    /// Starts (or restarts, emptying it) field `field`'s physical cache of `slotCount` slots, the
    /// renderer's frames in flight and the arrival fade window. Its streams keep their loaded pages.
    void ConfigureField(uint32 field, uint32 slotCount, uint32 framesInFlight, float32 fadeSeconds);

    /// Starts (or restarts, dropping its pages) stream `stream` over `source`, its pages held in
    /// field `field`'s cache (configured first). A restart cancels the old source's outstanding
    /// loads; their samples never land.
    void Configure(uint32 stream, uint32 field, PageSource source);

    /// Ends stream `stream`, releases its pages from its field's cache and cancels its outstanding
    /// loads. They still count against the loads-in-flight budget until they resolve.
    void Forget(uint32 stream);

    /// The pages stream `stream` wants this frame (closed under parents: the level rule's set).
    void Request(uint32 stream, std::span<const PageWant> wanted);

    /// The content of stream `stream`'s pages over level-0 samples `level0Rect` changed in place
    /// (a modifier edit; its owner applies the change when it uploads a page). Each resident page
    /// that reads the rect (PageReadsLevel0Rect) is loaded again and rewritten into its slot
    /// (PageCache::Rewrite) once loaded, ahead of the wanted pages' loads; until then it keeps its
    /// slot and its old content, so an edit never opens a hole.
    void Refresh(uint32 stream, const PageSampleRect& level0Rect);

    /// One frame for every stream. `teleport` raises the upload cap for the frames after a jump.
    void Update(uint64 frameIndex, float32 deltaSeconds, bool teleport);

    /// Field `field`'s physical cache: every stream's assignments, uploads and transitions this
    /// frame, each page tagged with its stream.
    const PageCache& FieldCache(uint32 field) const;
    /// The decoded samples of a page of stream `stream` on the CPU (kPageSampleCount), or null: held
    /// from the page's load until the Update after its cache took it.
    const std::vector<float32>* Samples(uint32 stream, const PageAddress& address) const;

    uint32 LoadsStartedThisFrame() const { return m_LoadsStarted; }
    uint64 CpuBytes() const { return m_CpuBytes; }

private:
    struct LoadedPage
    {
        std::shared_ptr<const std::vector<float32>> Samples;
        uint64 LastWantedFrame = 0;
    };

    // A page whose load failed: when it may be tried again.
    struct LoadFailure
    {
        uint64 RetryAtFrame = 0;
        uint32 Attempts = 0;
    };

    // One finished load, from a reader or job thread.
    struct Completion
    {
        uint32 Stream = 0;
        uint64 Epoch = 0;
        PageAddress Address;
        std::shared_ptr<const std::vector<float32>> Samples; // null: the page did not load
        std::string Error;                                   // why, when it did not
        bool Absent = false;                                 // not in its source at all: never retried
    };

    struct Stream
    {
        PageSource Source;
        uint32 Field = 0;
        uint32 TopLevel = 0;
        uint64 Epoch = 0; // bumps on Configure, so a restarted stream ignores its old loads
        std::shared_ptr<std::atomic<bool>> Cancel = std::make_shared<std::atomic<bool>>(false);
        std::vector<PageWant> Wanted;
        bool WantedChanged = true;
        std::vector<PageWant> Placeable; // the wanted pages its field's cache can hold, in priority order
        std::unordered_set<PageAddress, PageAddressHash> PlaceableSet;
        std::unordered_map<PageAddress, LoadedPage, PageAddressHash> Loaded;
        std::unordered_set<PageAddress, PageAddressHash> InFlight;
        std::unordered_set<PageAddress, PageAddressHash> Absent;
        std::unordered_set<PageAddress, PageAddressHash> Stale; // resident pages to load and rewrite (Refresh)
        std::unordered_map<PageAddress, LoadFailure, PageAddressHash> Failures;
    };

    // A wanted page of some stream, in the one order across streams.
    struct Candidate
    {
        uint32 Stream = 0;
        PageWant Want;
    };

    struct CompletionQueue
    {
        std::mutex Mutex;
        std::vector<Completion> Completions;
    };

    void DropUploadedSamples();
    void DrainCompletions(uint64 frameIndex);
    void RebuildCandidates();
    void StartLoads(uint64 frameIndex);
    void StartLoad(uint32 streamId, Stream& stream, const PageAddress& address);
    void AssignLoaded(uint64 frameIndex, float32 deltaSeconds, bool teleport);
    void RewriteRefreshed(uint32 field, PageCache& cache);
    void TrimCpuSamples();

    JobSystem::WorkStealingThreadPool* m_Pool = nullptr;
    AssetIOService* m_Io = nullptr;
    PageResidencyBudget m_Budget;
    std::unordered_map<uint32, Stream> m_Streams;
    std::unordered_map<uint32, PageCache> m_Fields;
    std::vector<PageWant> m_Stamped; // the request being compared, its pages tagged (reused)
    std::shared_ptr<CompletionQueue> m_Completions = std::make_shared<CompletionQueue>();
    std::vector<Completion> m_Drained;     // reused each frame
    std::vector<Candidate> m_Candidates;   // every stream's placeable pages, in the one order
    std::unordered_map<uint32, uint32> m_Allowance; // per field: assignments its cache may make this frame
    std::unordered_map<uint32, uint32> m_FieldPlaced; // per field: pages placed while rebuilding the order
    std::vector<PageWant> m_LoadedPlaceable;        // reused per field each frame
    std::vector<std::pair<uint64, std::pair<uint32, PageAddress>>> m_TrimOrder; // reused by the trim
    uint64 m_PlaceableBytes = 0; // every stream's placeable pages, in bytes
    bool m_CandidatesStale = false; // a stream was forgotten or restarted
    uint64 m_CpuBytes = 0;
    uint32 m_InFlightCount = 0;
    uint32 m_LoadsStarted = 0;
    uint64 m_NextEpoch = 1;
};

} // namespace GameEngine::PageStreaming
