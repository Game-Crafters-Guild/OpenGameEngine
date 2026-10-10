// ModelThumbnailHandler, part: The folder bake queue ("Generate Thumbnails"): queueing, starting
// bakes on free slots, finishing them and the report.

#include "Thumbnails/ModelThumbnailHandler.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <vector>

namespace GameEngine
{

namespace
{
// A folder bake that has not persisted its PNG by then cannot (an import that
// never ends, a model with nothing to draw); it gives its slot back. Time held
// for a pipeline build does not count (Slot::bakeClockStart).
constexpr std::chrono::seconds kBakeTimeout{30};
} // namespace

size_t ModelThumbnailHandler::QueueBakes(const std::vector<BakeItem>& items,
                                         const std::vector<std::filesystem::path>& missingFromDisk)
{
    size_t queued = 0;
    for (const BakeItem& item : items)
    {
        if (item.guid.IsNull() || !m_BakeQueued.insert(item.guid).second)
            continue;
        m_BakeQueue.push_back(item);
        ++queued;
    }
    if ((queued > 0 || !missingFromDisk.empty()) && !m_BakeBatch.active)
        m_BakeBatch = {};
    m_BakeBatch.queued += queued;
    m_BakeBatch.missingFromDisk.insert(m_BakeBatch.missingFromDisk.end(), missingFromDisk.begin(),
                                       missingFromDisk.end());
    m_BakeBatch.active = m_BakeBatch.active || queued > 0;
    PublishFolderBakeReport();
    return queued;
}

void ModelThumbnailHandler::SetFolderBakeListener(std::function<void(const FolderBakeReport&)> listener)
{
    m_FolderBakeListener = std::move(listener);
    m_PublishedBakeReport = {};
    PublishFolderBakeReport();
}

void ModelThumbnailHandler::PublishFolderBakeReport()
{
    // Runs every tick a bake can change; the file lists only grow, so equal
    // counts and sizes mean an equal report and nothing is copied.
    const FolderBakeReport& last = m_PublishedBakeReport;
    if (last.Queued == m_BakeBatch.queued && last.Generated == m_BakeBatch.baked &&
        last.AlreadyCurrent == m_BakeBatch.skipped && last.Failed.size() == m_BakeBatch.failed.size() &&
        last.MissingFromDisk.size() == m_BakeBatch.missingFromDisk.size() && last.Running == m_BakeBatch.active)
        return;
    const FolderBakeReport report{m_BakeBatch.queued, m_BakeBatch.baked, m_BakeBatch.skipped,
                                  m_BakeBatch.failed, m_BakeBatch.missingFromDisk, m_BakeBatch.active};
    m_PublishedBakeReport = report;
    if (m_FolderBakeListener)
        m_FolderBakeListener(report);
}

bool ModelThumbnailHandler::HasCurrentCachedPng(const GUID& guid, bool isMaterial) const
{
    AssetMetadata meta;
    if (!m_AssetManager || !m_AssetManager->GetRegistry().TryGetAssetMetadata(guid, meta) || meta.Path.empty())
        return false;
    const std::filesystem::path cachePath =
        isMaterial ? ComputeMaterialCachePath(guid, GetPreviewIblEnabled()) : ComputeModelCachePath(guid);
    if (cachePath.empty())
        return false;
    std::error_code sourceError, cacheError;
    const auto sourceTime = std::filesystem::last_write_time(meta.Path, sourceError);
    const auto cacheTime = std::filesystem::last_write_time(cachePath, cacheError);
    return !sourceError && !cacheError && cacheTime >= sourceTime;
}

// GE_THUMBNAIL_CACHE_BAKES_BEGIN
void ModelThumbnailHandler::StartQueuedBakes(WindowState& ws)
{
    if (m_BakeQueue.empty() || !ws.pending.empty() || m_DiskCacheRoot.empty())
        return;
    size_t running = static_cast<size_t>(std::count_if(ws.slots.begin(), ws.slots.end(),
        [](const Slot& slot) { return slot.occupied && slot.bakeOnly; }));
    bool freeSlot = std::any_of(ws.slots.begin(), ws.slots.end(), [](const Slot& slot) { return !slot.occupied; });
    for (size_t inspected = 0; inspected < kMaxBakeItemsInspectedPerTick && freeSlot &&
                               running < kRenderLaneCount - kFirstQueueLane && !m_BakeQueue.empty();
         ++inspected)
    {
        const BakeItem item = m_BakeQueue.front();
        m_BakeQueue.pop_front();
        m_BakeQueued.erase(item.guid);
        const auto& tiles = item.isMaterial
            ? (GetPreviewIblEnabled() ? ws.guidToMaterialSlot : ws.guidToMaterialNoIblSlot)
            : ws.guidToSlot;
        // A visible tile renders the asset itself. One that could not (the
        // model does not load, or its render gave up) is a failure; one whose
        // PNG is written is current; one still on its way is asked about
        // again later, so it is counted by its outcome.
        if (const auto tile = tiles.find(item.guid); tile != tiles.end())
        {
            const bool broken = (m_AssetManager && m_AssetManager->IsLoadSuppressed(item.guid)) ||
                                ws.slots[tile->second].renderGaveUp;
            if (broken)
            {
                NoteBakeFailure(item.guid);
            }
            else if (m_DiskCacheRequested.contains(item.guid))
            {
                ++m_BakeBatch.skipped;
            }
            else
            {
                m_BakeQueue.push_back(item);
                m_BakeQueued.insert(item.guid);
            }
            continue;
        }
        // Its PNG is current: written or served this session
        // (m_DiskCacheRequested, no file query), else on disk.
        if (m_DiskCacheRequested.contains(item.guid) || HasCurrentCachedPng(item.guid, item.isMaterial))
        {
            ++m_BakeBatch.skipped;
            continue;
        }
        const size_t slotIdx = AcquireSlotForGuid(ws, item.guid, false, item.isMaterial, std::nullopt);
        Slot& slot = ws.slots[slotIdx];
        slot.bakeOnly = true;
        slot.bakeClockStart = std::chrono::steady_clock::now();
        EnqueuePending(ws, item.guid, false, item.isMaterial, std::nullopt);
        ++running;
        freeSlot = std::any_of(ws.slots.begin(), ws.slots.end(), [](const Slot& other) { return !other.occupied; });
    }
}
// GE_THUMBNAIL_CACHE_BAKES_END

void ModelThumbnailHandler::NoteBakeFailure(const GUID& guid)
{
    AssetMetadata meta;
    if (m_AssetManager && m_AssetManager->GetRegistry().TryGetAssetMetadata(guid, meta))
    {
        Logger::Log::Warning("Thumbnails: could not bake '{}'", meta.Path.string());
        m_BakeBatch.failed.push_back(meta.Path);
    }
    else
    {
        m_BakeBatch.failed.push_back(guid.ToString());
    }
}

void ModelThumbnailHandler::FinishBakes(WindowState& ws)
{
    const auto now = std::chrono::steady_clock::now();
    for (size_t slotIdx = 0; slotIdx < ws.slots.size(); ++slotIdx)
    {
        Slot& slot = ws.slots[slotIdx];
        if (!slot.occupied || !slot.bakeOnly || slot.inFlight)
            continue;
        // The capture requests a PNG only from a settled, ready slot.
        const bool readbackPending = m_PendingDiskCache && m_PendingDiskCache->guid == slot.guid;
        const bool persisted = slot.ready && m_DiskCacheRequested.contains(slot.guid) && !readbackPending;
        const bool cannotBake = (m_AssetManager && m_AssetManager->IsLoadSuppressed(slot.guid)) ||
                                (slot.ready && slot.renderGaveUp) ||
                                now - slot.bakeClockStart > kBakeTimeout;
        if (!persisted && !cannotBake)
            continue;
        if (persisted)
        {
            ++m_BakeBatch.baked;
        }
        else
        {
            NoteBakeFailure(slot.guid);
        }
        ReleaseSlotAssignment(ws, slotIdx);
        slot.occupied = false;
        slot.bakeOnly = false;
    }

    if (!m_BakeBatch.active || !m_BakeQueue.empty())
        return;
    for (const auto& [windowId, window] : m_Windows)
    {
        (void)windowId;
        for (const Slot& slot : window.slots)
        {
            if (slot.occupied && slot.bakeOnly)
                return;
        }
    }
    Logger::Log::Info("Thumbnails: folder bake done: {} baked, {} already current, {} could not bake, "
                      "{} listed but not on disk",
                      m_BakeBatch.baked, m_BakeBatch.skipped, m_BakeBatch.failed.size(),
                      m_BakeBatch.missingFromDisk.size());
    // The counts stay for the report until the next batch starts.
    m_BakeBatch.active = false;
}

} // namespace GameEngine
