#include "PageStreaming/PageResidency.h"

#include "PageStreaming/HeightPageOverlay.h"

#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageStoreReader.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::PageStreaming
{
namespace
{

constexpr uint64 kPageSampleBytes = static_cast<uint64>(kPageSampleCount) * sizeof(float32);
// The longest wait before a failed page is tried again, in frames; the wait doubles up to it.
constexpr uint32 kMaxRetryFrames = 64;

uint32 TopLevelOf(const PageSource& source)
{
    if (source.Store)
        return static_cast<uint32>(source.Store->Layout().Levels.size()) - 1u;
    if (source.Generated && !source.Generated->Levels().empty())
        return static_cast<uint32>(source.Generated->Levels().size()) - 1u;
    return 0;
}

// The one order of wanted pages: pinned first, then nearest, then coarsest (a parent's footprint
// holds its child's, so it is never farther, and the level breaks the tie).
bool LoadsBefore(const PageWant& a, const PageWant& b)
{
    if (a.Pinned != b.Pinned)
        return a.Pinned;
    if (a.Distance != b.Distance)
        return a.Distance < b.Distance;
    return a.Address.Level > b.Address.Level;
}

} // namespace

PageResidencyManager::PageResidencyManager(JobSystem::WorkStealingThreadPool* pool, AssetIOService* io,
                                           const PageResidencyBudget& budget)
    : m_Pool(pool)
    , m_Io(io)
    , m_Budget(budget)
{
}

PageResidencyManager::~PageResidencyManager()
{
    for (auto& [id, stream] : m_Streams)
        stream.Cancel->store(true);
}

void PageResidencyManager::ConfigureField(uint32 field, uint32 slotCount, uint32 framesInFlight, float32 fadeSeconds)
{
    m_Fields[field].Configure(slotCount, framesInFlight, fadeSeconds);
    m_CandidatesStale = true; // what each field can place changed
}

void PageResidencyManager::Configure(uint32 stream, uint32 field, PageSource source)
{
    assert(m_Fields.count(field) && "a stream's field is configured before the stream");
    Forget(stream);
    Stream& state = m_Streams[stream];
    state.Source = std::move(source);
    state.Field = field;
    state.TopLevel = TopLevelOf(state.Source);
    state.Epoch = m_NextEpoch++;
}

void PageResidencyManager::Forget(uint32 stream)
{
    const auto it = m_Streams.find(stream);
    if (it == m_Streams.end())
        return;
    // Its outstanding loads resolve as cancelled and are counted out of the budget as they drain.
    it->second.Cancel->store(true);
    m_CpuBytes -= static_cast<uint64>(it->second.Loaded.size()) * kPageSampleBytes;
    if (const auto field = m_Fields.find(it->second.Field); field != m_Fields.end())
        field->second.ReleaseStream(stream);
    m_Streams.erase(it);
    m_CandidatesStale = true; // the candidate list names the forgotten stream
}

void PageResidencyManager::Request(uint32 stream, std::span<const PageWant> wanted)
{
    const auto it = m_Streams.find(stream);
    assert(it != m_Streams.end() && "Request on a stream that was not configured");
    if (it == m_Streams.end())
        return;
    m_Stamped.assign(wanted.begin(), wanted.end());
    for (PageWant& want : m_Stamped)
    {
        want.Stream = stream;
        want.Top = want.Address.Level >= it->second.TopLevel;
    }
    if (m_Stamped == it->second.Wanted)
        return;
    it->second.Wanted.swap(m_Stamped);
    it->second.WantedChanged = true;
}

const PageCache& PageResidencyManager::FieldCache(uint32 field) const
{
    return m_Fields.at(field);
}

const std::vector<float32>* PageResidencyManager::Samples(uint32 stream, const PageAddress& address) const
{
    const auto it = m_Streams.find(stream);
    if (it == m_Streams.end())
        return nullptr;
    const auto page = it->second.Loaded.find(address);
    return page == it->second.Loaded.end() ? nullptr : page->second.Samples.get();
}

void PageResidencyManager::DropUploadedSamples()
{
    // The caches' uploads of the last frame: their owners have written the texels since.
    for (const auto& [field, cache] : m_Fields)
        for (const PageUploadRequest& upload : cache.Uploads())
        {
            const auto stream = m_Streams.find(upload.Page.Stream);
            if (stream != m_Streams.end() && stream->second.Loaded.erase(upload.Page.Address) != 0)
                m_CpuBytes -= kPageSampleBytes;
        }
}

void PageResidencyManager::DrainCompletions(uint64 frameIndex)
{
    m_Drained.clear();
    {
        std::lock_guard lock(m_Completions->Mutex);
        m_Drained.swap(m_Completions->Completions);
    }
    for (Completion& done : m_Drained)
    {
        --m_InFlightCount; // every load started is counted out once, its stream alive or not
        const auto it = m_Streams.find(done.Stream);
        if (it == m_Streams.end() || it->second.Epoch != done.Epoch)
            continue; // a load of a stream since forgotten or restarted
        Stream& stream = it->second;
        stream.InFlight.erase(done.Address);
        if (done.Samples)
        {
            stream.Loaded[done.Address] = LoadedPage{std::move(done.Samples), frameIndex};
            stream.Failures.erase(done.Address);
            m_CpuBytes += kPageSampleBytes;
            continue;
        }
        if (done.Absent)
        {
            stream.Absent.insert(done.Address);
            continue;
        }
        LoadFailure& failure = stream.Failures[done.Address];
        if (failure.Attempts == 0)
            Logger::Log::Warning("Terrain page (level {}, {}, {}) did not load: {}. It is retried.",
                                 done.Address.Level, done.Address.X, done.Address.Z, done.Error);
        failure.RetryAtFrame = frameIndex + std::min(kMaxRetryFrames, 1u << std::min(failure.Attempts, 6u));
        ++failure.Attempts;
    }
}

void PageResidencyManager::RebuildCandidates()
{
    bool changed = m_CandidatesStale;
    m_CandidatesStale = false;
    for (auto& [id, stream] : m_Streams)
    {
        changed = changed || stream.WantedChanged;
        stream.WantedChanged = false;
    }
    if (!changed)
        return; // a parked camera: the order stands
    m_Candidates.clear();
    for (auto& [id, stream] : m_Streams)
    {
        stream.Placeable.clear();
        stream.PlaceableSet.clear();
        for (const PageWant& want : stream.Wanted)
            m_Candidates.push_back(Candidate{id, want});
    }
    std::sort(m_Candidates.begin(), m_Candidates.end(),
              [](const Candidate& a, const Candidate& b) { return LoadsBefore(a.Want, b.Want); });
    // Each field places as many pages as its cache has slots, its streams taking them in the one order.
    for (auto& [field, taken] : m_FieldPlaced)
        taken = 0;
    std::size_t kept = 0;
    for (const Candidate& candidate : m_Candidates)
    {
        Stream& stream = m_Streams.at(candidate.Stream);
        uint32& taken = m_FieldPlaced[stream.Field];
        if (taken >= m_Fields.at(stream.Field).SlotCount())
            continue;
        ++taken;
        stream.Placeable.push_back(candidate.Want);
        stream.PlaceableSet.insert(candidate.Want.Address);
        m_Candidates[kept++] = candidate;
    }
    m_Candidates.resize(kept);
    m_PlaceableBytes = static_cast<uint64>(kept) * kPageSampleBytes;
}

void PageResidencyManager::StartLoad(uint32 streamId, Stream& stream, const PageAddress& address)
{
    const std::shared_ptr<CompletionQueue> queue = m_Completions;
    const uint64 epoch = stream.Epoch;
    stream.InFlight.insert(address);
    ++m_InFlightCount;
    ++m_LoadsStarted;
    if (stream.Source.Store && m_Io)
    {
        PageLoadRequest request;
        request.Store = stream.Source.Store;
        request.Address = address;
        request.Priority = AssetLoadPriority::High;
        request.Cancel = stream.Cancel;
        request.OnLoaded = [queue, streamId, epoch](const PageAddress& page, std::vector<float32> samples) {
            std::lock_guard lock(queue->Mutex);
            queue->Completions.push_back(
                {streamId, epoch, page, std::make_shared<const std::vector<float32>>(std::move(samples)), {}, false});
        };
        request.OnFailed = [queue, streamId, epoch, store = stream.Source.Store](const PageAddress& page,
                                                                                 const std::string& error) {
            std::lock_guard lock(queue->Mutex);
            queue->Completions.push_back({streamId, epoch, page, nullptr, error, !store->Layout().FindPresent(page)});
        };
        SubmitHeightPageLoad(*m_Io, std::move(request));
        return;
    }
    // A generated page (or a store with no reader threads): evaluated or read on a Background job.
    const auto fill = [queue, streamId, epoch, address, source = stream.Source, cancel = stream.Cancel]() {
        Completion done{streamId, epoch, address, nullptr, {}, false};
        if (cancel->load())
        {
            done.Error = "the stream was restarted";
        }
        else if (source.Store && !source.Store->Layout().FindPresent(address))
        {
            done.Absent = true;
        }
        else
        {
            auto samples = std::make_shared<std::vector<float32>>(kPageSampleCount);
            const bool filled = source.Generated ? source.Generated->FillPage(address, *samples)
                                                 : source.Store->ReadHeightPage(address, *samples);
            if (filled)
                done.Samples = std::move(samples);
            else if (source.Generated)
                done.Absent = true; // outside the generated pyramid
            else
                done.Error = "the page store could not be read";
        }
        std::lock_guard lock(queue->Mutex);
        queue->Completions.push_back(std::move(done));
    };
    if (m_Pool)
        m_Pool->Submit(fill, JobSystem::JobPriority::Background);
    else
        fill();
}

void PageResidencyManager::Refresh(uint32 streamId, const PageSampleRect& level0Rect)
{
    const auto it = m_Streams.find(streamId);
    if (it == m_Streams.end())
        return;
    Stream& stream = it->second;
    for (const CachedPage& page : m_Fields.at(stream.Field).Resident())
        if (page.Stream == streamId && PageReadsLevel0Rect(page.Address, level0Rect))
            stream.Stale.insert(page.Address);
}

void PageResidencyManager::StartLoads(uint64 frameIndex)
{
    // A refreshed page is shown with its old content until it is rewritten: reload those first.
    for (auto& [id, stream] : m_Streams)
        for (const PageAddress& address : stream.Stale)
        {
            if (m_InFlightCount >= m_Budget.LoadsInFlight)
                return;
            if (!stream.Loaded.count(address) && !stream.InFlight.count(address))
                StartLoad(id, stream, address);
        }
    for (const Candidate& candidate : m_Candidates)
    {
        if (m_InFlightCount >= m_Budget.LoadsInFlight)
            return;
        Stream& stream = m_Streams.at(candidate.Stream);
        const PageAddress& address = candidate.Want.Address;
        if (const auto it = stream.Loaded.find(address); it != stream.Loaded.end())
        {
            it->second.LastWantedFrame = frameIndex;
            continue;
        }
        if (m_Fields.at(stream.Field).SlotOf(candidate.Want.Page()) != kNoResidentSlot)
            continue; // resident: its samples were uploaded and dropped
        if (stream.InFlight.count(address) || stream.Absent.count(address))
            continue;
        if (const auto failed = stream.Failures.find(address);
            failed != stream.Failures.end() && frameIndex < failed->second.RetryAtFrame)
            continue;
        StartLoad(candidate.Stream, stream, address);
    }
}

void PageResidencyManager::AssignLoaded(uint64 frameIndex, float32 deltaSeconds, bool teleport)
{
    // The upload cap goes to the loaded, placeable, not yet resident pages in the one order.
    uint32 uploads = teleport ? m_Budget.UploadsPerFrameTeleport : m_Budget.UploadsPerFrame;
    for (auto& [field, allowance] : m_Allowance)
        allowance = 0;
    for (const Candidate& candidate : m_Candidates)
    {
        if (uploads == 0)
            break;
        const Stream& stream = m_Streams.at(candidate.Stream);
        if (stream.Loaded.count(candidate.Want.Address) &&
            m_Fields.at(stream.Field).SlotOf(candidate.Want.Page()) == kNoResidentSlot)
        {
            ++m_Allowance[stream.Field];
            --uploads;
        }
    }
    for (auto& [field, cache] : m_Fields)
    {
        m_LoadedPlaceable.clear();
        for (const auto& [id, stream] : m_Streams)
        {
            if (stream.Field != field)
                continue;
            for (const PageWant& want : stream.Placeable)
                if (stream.Loaded.count(want.Address) || cache.SlotOf(want.Page()) != kNoResidentSlot)
                    m_LoadedPlaceable.push_back(want);
        }
        const auto allowance = m_Allowance.find(field);
        cache.Update(frameIndex, deltaSeconds, m_LoadedPlaceable, allowance == m_Allowance.end() ? 0u : allowance->second);
        RewriteRefreshed(field, cache);
    }
}

void PageResidencyManager::RewriteRefreshed(uint32 field, PageCache& cache)
{
    for (auto& [id, stream] : m_Streams)
    {
        if (stream.Field != field)
            continue;
        for (auto page = stream.Stale.begin(); page != stream.Stale.end();)
        {
            const CachedPage cached{id, *page};
            if (cache.SlotOf(cached) == kNoResidentSlot)
                page = stream.Stale.erase(page); // released meanwhile: a later assign loads it afresh
            else if (stream.Loaded.count(*page) && cache.Rewrite(cached))
                page = stream.Stale.erase(page);
            else
                ++page;
        }
    }
}

void PageResidencyManager::TrimCpuSamples()
{
    // Every loaded placeable page is kept, so over this budget at least one loaded page is not
    // placeable: the trim runs only when it has something to drop.
    const uint64 budget = std::max(m_Budget.CpuBytes, m_PlaceableBytes);
    if (m_CpuBytes <= budget)
        return;
    m_TrimOrder.clear();
    for (const auto& [id, stream] : m_Streams)
        for (const auto& [address, page] : stream.Loaded)
            if (!stream.PlaceableSet.count(address))
                m_TrimOrder.push_back({page.LastWantedFrame, {id, address}});
    std::sort(m_TrimOrder.begin(), m_TrimOrder.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [lastWanted, page] : m_TrimOrder)
    {
        if (m_CpuBytes <= budget)
            break;
        m_Streams.at(page.first).Loaded.erase(page.second);
        m_CpuBytes -= kPageSampleBytes;
    }
}

void PageResidencyManager::Update(uint64 frameIndex, float32 deltaSeconds, bool teleport)
{
    m_LoadsStarted = 0;
    DropUploadedSamples();
    DrainCompletions(frameIndex);
    RebuildCandidates();
    StartLoads(frameIndex);
    AssignLoaded(frameIndex, deltaSeconds, teleport);
    TrimCpuSamples();
}

} // namespace GameEngine::PageStreaming
