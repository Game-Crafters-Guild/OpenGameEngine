#include "Rendering/Core/PipelineCache.h"

#include "Rendering/Core/HashUtils.h"

#include <array>
#include <atomic>

namespace GameEngine::Rendering
{

// =====================================================================
// Thread-local L1 cache
//
// Per-thread fixed-size open-addressing slot table in front of the device-
// level concrete map. Lookup is mutex-free; on miss we fall through to the
// mutex-locked map and populate the L1 with the result.
//
// Invalidation: each L1 entry stores the cache's epoch at insert time; `Clear`,
// `InvalidateByTag*` and LRU eviction all move the cache to a fresh epoch,
// making old L1 entries fail the validity check on next lookup. A briefly-stale
// entry would still name a live VkPipeline — this cache never destroys what it
// evicts, it only forgets the map — so this exists to stop a recycled slot
// index from serving an unrelated key, not to prevent a use-after-free.
//
// The table is thread_local, so it is shared by every cache instance the thread
// touches and outlives the one that filled it. Epochs are drawn from a
// process-wide counter and never reissued, which is what keeps one cache from
// reading an entry another left behind — a `Cache == this` test could not, since
// the allocator hands a dead cache's address to the next one.
// =====================================================================

namespace
{
constexpr size_t kL1Slots = 256; // power of two for AND-mask indexing
constexpr size_t kL1IndexBits = 8;
struct L1Slot
{
    uint64_t       Key = 0;
    uint64_t       Epoch = 0;
    PipelineHandle Handle{};
};
thread_local std::array<L1Slot, kL1Slots> tls_L1{};

// Batched flush of the L1 hit counter. The hot path increments a thread-local
// running total; once it crosses `kL1HitFlushBatch` we publish to the cache's
// atomic counter in a single op. Trades exact stats for ~64× fewer atomic
// ops (and zero cross-core cache-line ping-pong on the hot path). Up to
// `kL1HitFlushBatch - 1` pending hits per thread are invisible until the
// next flush — acceptable for diagnostics.
thread_local uint32_t tls_L1HitsPending = 0;
constexpr uint32_t kL1HitFlushBatch = 64;

inline size_t L1IndexFor(uint64_t key) noexcept
{
    return static_cast<size_t>(key * 0x9E3779B97F4A7C15ull >> (64 - kL1IndexBits));
}
} // anonymous namespace

uint64_t PipelineCache::NextCacheEpoch() noexcept
{
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// =====================================================================
// Combined-key helpers
// =====================================================================

uint64_t PipelineCache::CombineGraphicsKey(GraphicsPipelineId id, const PipelineFormatKey& fk) noexcept
{
    const uint64_t fkHashLow = fk.Hash() & 0xFFFFFFFFull;
    return (static_cast<uint64_t>(id.Value) << 32) | fkHashLow;
}

uint64_t PipelineCache::CombineComputeKey(ComputePipelineId id) noexcept
{
    return static_cast<uint64_t>(id.Value) << 32; // formatHash low-32 = 0 for compute
}

// =====================================================================
// Intern tables — graphics
// =====================================================================

GraphicsPipelineId PipelineCache::InternGraphicsPipeline(GraphicsPipelineDesc desc)
{
    const uint64_t h = desc.ContentHash();
    std::scoped_lock lock(m_Mutex);
    if (auto it = m_GraphicsHashToId.find(h); it != m_GraphicsHashToId.end())
    {
        // Re-pin live entry (if a previous tombstone matched, that's a hash
        // collision — re-create rather than alias, see Live flag below).
        auto& entry = m_GraphicsEntries[it->second - 1];
        if (entry.Live)
            return GraphicsPipelineId{it->second};
        // Fall through to create a fresh entry (tombstone replaced).
    }
    m_GraphicsEntries.push_back({std::move(desc), 0u, false, true});
    const uint32_t newId = static_cast<uint32_t>(m_GraphicsEntries.size()); // 1-based
    m_GraphicsHashToId[h] = newId;
    return GraphicsPipelineId{newId};
}

ComputePipelineId PipelineCache::InternComputePipeline(ComputePipelineDesc desc)
{
    const uint64_t h = desc.ContentHash();
    std::scoped_lock lock(m_Mutex);
    if (auto it = m_ComputeHashToId.find(h); it != m_ComputeHashToId.end())
    {
        auto& entry = m_ComputeEntries[it->second - 1];
        if (entry.Live)
            return ComputePipelineId{it->second};
    }
    m_ComputeEntries.push_back({std::move(desc), 0u, false, true});
    const uint32_t newId = static_cast<uint32_t>(m_ComputeEntries.size());
    m_ComputeHashToId[h] = newId;
    return ComputePipelineId{newId};
}

DescriptorSetLayoutId PipelineCache::InternDescriptorSetLayout(DescriptorSetLayoutDesc desc)
{
    const uint64_t h = HashDescriptorSetLayoutDesc(desc);
    std::scoped_lock lock(m_Mutex);
    if (auto it = m_LayoutHashToId.find(h); it != m_LayoutHashToId.end())
    {
        auto& entry = m_LayoutEntries[it->second - 1];
        if (entry.Live)
            return DescriptorSetLayoutId{it->second};
    }
    m_LayoutEntries.push_back({std::move(desc), 0u, false, true});
    const uint32_t newId = static_cast<uint32_t>(m_LayoutEntries.size());
    m_LayoutHashToId[h] = newId;
    return DescriptorSetLayoutId{newId};
}

// =====================================================================
// Lookups
// =====================================================================

const GraphicsPipelineDesc* PipelineCache::LookupGraphicsPipeline(GraphicsPipelineId id) const
{
    if (!id.IsValid()) return nullptr;
    std::scoped_lock lock(m_Mutex);
    if (id.Value > m_GraphicsEntries.size()) return nullptr;
    const auto& entry = m_GraphicsEntries[id.Value - 1];
    return entry.Live ? &entry.Desc : nullptr;
}

const ComputePipelineDesc* PipelineCache::LookupComputePipeline(ComputePipelineId id) const
{
    if (!id.IsValid()) return nullptr;
    std::scoped_lock lock(m_Mutex);
    if (id.Value > m_ComputeEntries.size()) return nullptr;
    const auto& entry = m_ComputeEntries[id.Value - 1];
    return entry.Live ? &entry.Desc : nullptr;
}

const DescriptorSetLayoutDesc* PipelineCache::LookupDescriptorSetLayout(DescriptorSetLayoutId id) const
{
    if (!id.IsValid()) return nullptr;
    std::scoped_lock lock(m_Mutex);
    if (id.Value > m_LayoutEntries.size()) return nullptr;
    const auto& entry = m_LayoutEntries[id.Value - 1];
    return entry.Live ? &entry.Desc : nullptr;
}

bool PipelineCache::TryCopyDescriptorSetLayout(DescriptorSetLayoutId id, DescriptorSetLayoutDesc& outDesc) const
{
    if (!id.IsValid()) return false;
    std::scoped_lock lock(m_Mutex);
    if (id.Value > m_LayoutEntries.size()) return false;
    const auto& entry = m_LayoutEntries[id.Value - 1];
    if (!entry.Live) return false;
    outDesc = entry.Desc;
    return true;
}

bool PipelineCache::TryCopyGraphicsPipeline(GraphicsPipelineId id, GraphicsPipelineDesc& outDesc) const
{
    if (!id.IsValid()) return false;
    std::scoped_lock lock(m_Mutex);
    if (id.Value > m_GraphicsEntries.size()) return false;
    const auto& entry = m_GraphicsEntries[id.Value - 1];
    if (!entry.Live) return false;
    outDesc = entry.Desc;
    return true;
}

bool PipelineCache::TryCopyComputePipeline(ComputePipelineId id, ComputePipelineDesc& outDesc) const
{
    if (!id.IsValid()) return false;
    std::scoped_lock lock(m_Mutex);
    if (id.Value > m_ComputeEntries.size()) return false;
    const auto& entry = m_ComputeEntries[id.Value - 1];
    if (!entry.Live) return false;
    outDesc = entry.Desc;
    return true;
}

// =====================================================================
// Pinning
// =====================================================================

void PipelineCache::PinGraphicsPipeline(GraphicsPipelineId id)
{
    if (!id.IsValid()) return;
    std::scoped_lock lock(m_Mutex);
    if (id.Value > m_GraphicsEntries.size()) return;
    m_GraphicsEntries[id.Value - 1].Pinned = true;
}

void PipelineCache::PinComputePipeline(ComputePipelineId id)
{
    if (!id.IsValid()) return;
    std::scoped_lock lock(m_Mutex);
    if (id.Value > m_ComputeEntries.size()) return;
    m_ComputeEntries[id.Value - 1].Pinned = true;
}

// =====================================================================
// Concrete cache
// =====================================================================

bool PipelineCache::TryGetConcrete(uint64_t combinedKey, PipelineHandle& outHandle)
{
    const uint64_t epoch = m_CacheEpoch.load(std::memory_order_acquire);
    auto& slot = tls_L1[L1IndexFor(combinedKey)];
    if (slot.Epoch == epoch && slot.Key == combinedKey)
    {
        // Hot path: bump a thread-local counter; only touch the atomic
        // every `kL1HitFlushBatch` hits. This eliminates per-call atomic
        // ops (and the cache-line sharing they cause) on the steady-state
        // path where the same pipelines are looked up over and over.
        outHandle = slot.Handle;
        if (++tls_L1HitsPending >= kL1HitFlushBatch)
        {
            m_L1Hits.fetch_add(tls_L1HitsPending, std::memory_order_relaxed);
            tls_L1HitsPending = 0;
        }
        return true;
    }
    m_L1Misses.fetch_add(1, std::memory_order_relaxed);

    std::scoped_lock lock(m_Mutex);
    auto it = m_Concrete.find(combinedKey);
    if (it == m_Concrete.end())
    {
        m_Misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (m_Capacity != 0)
    {
        m_LruList.splice(m_LruList.begin(), m_LruList, it->second.LruIt);
    }
    outHandle = it->second.Handle;
    slot = {combinedKey, epoch, outHandle};
    m_SlowHits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool PipelineCache::InsertConcrete(uint64_t combinedKey, const ConcreteInsertInfo& info)
{
    std::scoped_lock lock(m_Mutex);
    if (info.InvalidationEpoch && *info.InvalidationEpoch != m_InvalidationEpoch.load(std::memory_order_relaxed))
        return false;

    // An existing key is a row whose pipeline died (IDevice's GetOrCreate*
    // self-heal rebuilds it): replace the handle. Two concurrent misses on one
    // key no longer both create — IDevice builds each key once
    // (PipelineBuildTable) — so a live handle is never displaced and leaked here.
    auto it = m_Concrete.find(combinedKey);
    if (it != m_Concrete.end())
    {
        it->second.Handle = info.Handle;
        if (m_Capacity != 0)
            m_LruList.splice(m_LruList.begin(), m_LruList, it->second.LruIt);
        return true;
    }

    // Bump intern refcount.
    if (info.IsCompute)
    {
        if (info.PipelineIdValue > 0 && info.PipelineIdValue <= m_ComputeEntries.size())
            ++m_ComputeEntries[info.PipelineIdValue - 1].ConcreteRefcount;
    }
    else
    {
        if (info.PipelineIdValue > 0 && info.PipelineIdValue <= m_GraphicsEntries.size())
            ++m_GraphicsEntries[info.PipelineIdValue - 1].ConcreteRefcount;
    }

    ConcreteEntry entry;
    entry.Handle = info.Handle;
    entry.FormatKey = info.FormatKey;
    entry.PipelineIdValue = info.PipelineIdValue;
    entry.IsCompute = info.IsCompute;
    entry.Pinned = info.Pinned;

    if (m_Capacity != 0)
    {
        m_LruList.push_front(combinedKey);
        entry.LruIt = m_LruList.begin();
    }

    m_Concrete.emplace(combinedKey, std::move(entry));
    m_Inserts.fetch_add(1, std::memory_order_relaxed);

    if (!info.Tags.empty())
    {
        m_KeyToTags[combinedKey] = info.Tags;
        for (uint64_t t : info.Tags)
            m_TagToKeys[t].insert(combinedKey);
    }

    if (m_Capacity != 0)
        EnforceCapacity_NoLock();
    return true;
}

// =====================================================================
// Capacity / eviction
// =====================================================================

void PipelineCache::SetCapacity(size_t maxEntries)
{
    std::scoped_lock lock(m_Mutex);
    const bool wasUnlimited = (m_Capacity == 0);
    m_Capacity = maxEntries;
    // Migrate existing entries into the LRU list when transitioning from
    // unlimited → bounded (so eviction works against them).
    if (wasUnlimited && m_Capacity != 0)
    {
        m_LruList.clear();
        for (auto& [key, entry] : m_Concrete)
        {
            m_LruList.push_front(key);
            entry.LruIt = m_LruList.begin();
        }
    }
    EnforceCapacity_NoLock();
}

size_t PipelineCache::GetCapacity() const
{
    std::scoped_lock lock(m_Mutex);
    return m_Capacity;
}

void PipelineCache::EnforceCapacity_NoLock()
{
    if (m_Capacity == 0) return;
    bool anyEvicted = false;
    while (m_Concrete.size() > m_Capacity && !m_LruList.empty())
    {
        // Walk from back finding an unpinned entry.
        auto victimIt = m_LruList.end();
        --victimIt; // last (oldest)
        bool found = false;
        while (true)
        {
            auto entryIt = m_Concrete.find(*victimIt);
            if (entryIt != m_Concrete.end() && !entryIt->second.Pinned)
            {
                found = true;
                break;
            }
            if (victimIt == m_LruList.begin())
                break;
            --victimIt;
        }
        if (!found)
            break; // every entry pinned; nothing evictable.

        const uint64_t victimKey = *victimIt;
        RemoveKey_NoLock(victimKey);
        anyEvicted = true;
    }
    // Bump generation on any LRU eviction so TLS L1 entries pointing at the
    // evicted pipeline fail their generation check and fall through to the
    // mutex-locked map. Eviction only forgets the map — the VkPipeline is never
    // destroyed — so a brief stale read would still resolve to a live pipeline,
    // but if a later overflow recycles the slot index for an unrelated key, the
    // L1 hit would return the wrong handle. The cost is one L1
    // invalidation per overflow event — only fires when the cache is full,
    // which is itself rare.
    if (anyEvicted)
        m_CacheEpoch.store(NextCacheEpoch(), std::memory_order_release);
}

size_t PipelineCache::RemoveKey_NoLock(uint64_t key)
{
    auto it = m_Concrete.find(key);
    if (it == m_Concrete.end()) return 0;

    DecrementInternRefcount_NoLock(it->second);

    if (m_Capacity != 0)
        m_LruList.erase(it->second.LruIt);

    m_Concrete.erase(it);
    m_Evictions.fetch_add(1, std::memory_order_relaxed);

    if (auto itkt = m_KeyToTags.find(key); itkt != m_KeyToTags.end())
    {
        for (uint64_t t : itkt->second)
        {
            if (auto its = m_TagToKeys.find(t); its != m_TagToKeys.end())
            {
                its->second.erase(key);
                if (its->second.empty())
                    m_TagToKeys.erase(its);
            }
        }
        m_KeyToTags.erase(itkt);
    }
    return 1;
}

void PipelineCache::DecrementInternRefcount_NoLock(const ConcreteEntry& entry)
{
    if (entry.PipelineIdValue == 0) return;
    if (entry.IsCompute)
    {
        if (entry.PipelineIdValue <= m_ComputeEntries.size())
        {
            auto& e = m_ComputeEntries[entry.PipelineIdValue - 1];
            if (e.ConcreteRefcount > 0)
                --e.ConcreteRefcount;
            if (e.ConcreteRefcount == 0 && !e.Pinned && e.Live)
            {
                // Tombstone — id slot keeps its index but Desc no longer
                // valid. Hash entry erased so a re-intern of the same desc
                // gets a fresh id.
                e.Live = false;
                e.Desc = ComputePipelineDesc{};
                // Erase reverse mapping by walking m_ComputeHashToId — since
                // ids are 1:1 with vector indices we know the value.
                for (auto hit = m_ComputeHashToId.begin(); hit != m_ComputeHashToId.end(); )
                {
                    if (hit->second == entry.PipelineIdValue)
                        hit = m_ComputeHashToId.erase(hit);
                    else
                        ++hit;
                }
            }
        }
    }
    else
    {
        if (entry.PipelineIdValue <= m_GraphicsEntries.size())
        {
            auto& e = m_GraphicsEntries[entry.PipelineIdValue - 1];
            if (e.ConcreteRefcount > 0)
                --e.ConcreteRefcount;
            if (e.ConcreteRefcount == 0 && !e.Pinned && e.Live)
            {
                e.Live = false;
                e.Desc = GraphicsPipelineDesc{};
                for (auto hit = m_GraphicsHashToId.begin(); hit != m_GraphicsHashToId.end(); )
                {
                    if (hit->second == entry.PipelineIdValue)
                        hit = m_GraphicsHashToId.erase(hit);
                    else
                        ++hit;
                }
            }
        }
    }
}

// =====================================================================
// Tag-based selective invalidation
// =====================================================================

size_t PipelineCache::InvalidateByTag(uint64_t tag)
{
    std::scoped_lock lock(m_Mutex);
    const size_t removed = InvalidateByTag_NoLock(tag);
    if (removed > 0)
    {
        m_CacheEpoch.store(NextCacheEpoch(), std::memory_order_release);
        m_InvalidationEpoch.fetch_add(1, std::memory_order_release);
    }
    return removed;
}

size_t PipelineCache::InvalidateByTags(const std::vector<uint64_t>& tags)
{
    std::scoped_lock lock(m_Mutex);
    size_t total = 0;
    for (uint64_t tag : tags)
        total += InvalidateByTag_NoLock(tag);
    if (total > 0)
    {
        m_CacheEpoch.store(NextCacheEpoch(), std::memory_order_release);
        m_InvalidationEpoch.fetch_add(1, std::memory_order_release);
    }
    return total;
}

size_t PipelineCache::InvalidateByTag_NoLock(uint64_t tag)
{
    auto it = m_TagToKeys.find(tag);
    if (it == m_TagToKeys.end()) return 0;
    // Snapshot — RemoveKey_NoLock mutates the indices.
    std::vector<uint64_t> keys(it->second.begin(), it->second.end());
    size_t removed = 0;
    for (uint64_t key : keys)
        removed += RemoveKey_NoLock(key);
    if (auto it2 = m_TagToKeys.find(tag); it2 != m_TagToKeys.end() && it2->second.empty())
        m_TagToKeys.erase(it2);
    return removed;
}

// =====================================================================
// Clear / Stats
// =====================================================================

void PipelineCache::Clear()
{
    std::scoped_lock lock(m_Mutex);
    m_Concrete.clear();
    m_LruList.clear();
    m_KeyToTags.clear();
    m_TagToKeys.clear();
    // Tombstone every unpinned intern entry so the desc storage can compact
    // and re-interns of the same content hash get a fresh id. Pinned entries
    // (e.g. compute pipelines explicitly held by feature code) stay Live so
    // their ids remain valid across hot-reload Clear()s. Without tombstoning
    // here, every recompile during a long edit session would leak the old
    // GraphicsPipelineDesc storage and old reverse-map entries.
    auto tombstoneUnpinned = [](auto& entries, auto& hashMap)
    {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            auto& e = entries[i];
            e.ConcreteRefcount = 0;
            if (e.Live && !e.Pinned)
            {
                e.Live = false;
                e.Desc = {};
                // Erase the (hash -> id+1) entry pointing at this index.
                // Linear hash-map iteration — Clear is rare and tables are
                // small (hundreds of entries in practice).
                const uint32_t idValue = static_cast<uint32_t>(i + 1);
                for (auto hit = hashMap.begin(); hit != hashMap.end(); )
                {
                    if (hit->second == idValue)
                        hit = hashMap.erase(hit);
                    else
                        ++hit;
                }
            }
        }
    };
    tombstoneUnpinned(m_GraphicsEntries, m_GraphicsHashToId);
    tombstoneUnpinned(m_ComputeEntries,  m_ComputeHashToId);
    m_Clears.fetch_add(1, std::memory_order_relaxed);
    m_CacheEpoch.store(NextCacheEpoch(), std::memory_order_release);
    m_InvalidationEpoch.fetch_add(1, std::memory_order_release);
}

void PipelineCache::ClearConcreteOnly()
{
    std::scoped_lock lock(m_Mutex);
    // The concrete PipelineHandles are already dead after an in-place device
    // rebuild — forget them without touching the device (no DestroyPipeline).
    m_Concrete.clear();
    m_LruList.clear();
    m_KeyToTags.clear();
    m_TagToKeys.clear();
    // Zero the intern entries' concrete refcounts (their concrete owners are
    // gone) but KEEP the descs Live so a lookup miss recompiles from the
    // retained SPIR-V rather than re-interning a fresh id.
    for (auto& e : m_GraphicsEntries)
        e.ConcreteRefcount = 0;
    for (auto& e : m_ComputeEntries)
        e.ConcreteRefcount = 0;
    m_Clears.fetch_add(1, std::memory_order_relaxed);
    m_CacheEpoch.store(NextCacheEpoch(), std::memory_order_release);
    m_InvalidationEpoch.fetch_add(1, std::memory_order_release);
}

PipelineCacheStats PipelineCache::GetStats() const
{
    // Flush this thread's pending L1 hits so the stats reflect them. Other
    // threads' pending counts (up to `kL1HitFlushBatch - 1` each) remain
    // invisible until they next flush — diagnostic stats only, accuracy
    // bounded by batch size.
    if (tls_L1HitsPending > 0)
    {
        m_L1Hits.fetch_add(tls_L1HitsPending, std::memory_order_relaxed);
        tls_L1HitsPending = 0;
    }

    std::scoped_lock lock(m_Mutex);
    PipelineCacheStats s;
    s.ConcreteSize = m_Concrete.size();
    s.ConcreteCapacity = m_Capacity;
    s.L1Hits = m_L1Hits.load(std::memory_order_relaxed);
    s.L1Misses = m_L1Misses.load(std::memory_order_relaxed);
    s.Hits = s.L1Hits + m_SlowHits.load(std::memory_order_relaxed);
    s.Misses = m_Misses.load(std::memory_order_relaxed);
    s.Inserts = m_Inserts.load(std::memory_order_relaxed);
    s.Evictions = m_Evictions.load(std::memory_order_relaxed);
    s.Clears = m_Clears.load(std::memory_order_relaxed);

    // Counts of LIVE entries (tombstoned == counted as removed).
    auto countLive = [](const auto& vec) -> uint32_t
    {
        uint32_t n = 0;
        for (const auto& e : vec) if (e.Live) ++n;
        return n;
    };
    s.GraphicsPipelineCount = countLive(m_GraphicsEntries);
    s.ComputePipelineCount = countLive(m_ComputeEntries);
    s.DescriptorSetLayoutCount = countLive(m_LayoutEntries);

    uint32_t pinned = 0;
    for (const auto& [_, entry] : m_Concrete) if (entry.Pinned) ++pinned;
    s.PinnedCount = pinned;
    return s;
}

// =====================================================================
// Backend creation feed — materialize a `PipelineDesc` that the backend's
// `CreatePipeline(PipelineDesc)` virtual consumes.
// =====================================================================

PipelineDesc PipelineCache::BuildPipelineDescForGraphics(const GraphicsPipelineDesc& gd,
                                                         const PipelineFormatKey& fk,
                                                         const PipelineCache& cache)
{
    PipelineDesc out{};
    out.type = (gd.Kind == GraphicsPipelineKind::MeshFragment)
                      ? PipelineType::Mesh
                      : PipelineType::Graphics;

    if (gd.VertexShader)        out.vertexShader        = *gd.VertexShader;
    if (gd.PixelShader)         out.pixelShader         = *gd.PixelShader;
    if (gd.MeshShader)          out.meshShader          = *gd.MeshShader;
    if (gd.AmplificationShader) out.amplificationShader = *gd.AmplificationShader;

    out.descriptorSetLayouts.reserve(gd.DescriptorSetLayouts.size());
    for (auto layoutId : gd.DescriptorSetLayouts)
    {
        if (const auto* dsl = cache.LookupDescriptorSetLayout(layoutId))
            out.descriptorSetLayouts.push_back(*dsl);
        else
            out.descriptorSetLayouts.emplace_back();
    }

    out.rasterizationState = gd.Rasterization;
    out.depthStencilState  = gd.DepthStencil;
    out.colorBlendState    = gd.ColorBlend;
    if (gd.DynamicState.has_value())
        out.dynamicState = *gd.DynamicState;

    out.pushConstantSize       = gd.PushConstants.Size;
    out.pushConstantStagesMask = gd.PushConstants.StageMask;
    out.pushConstantRanges.reserve(gd.NamedPushConstantRanges.size());
    for (const auto& r : gd.NamedPushConstantRanges)
    {
        PipelineDesc::PushConstantRangeDesc lr{};
        lr.name      = r.Name;
        lr.offset    = r.Offset;
        lr.size      = r.Size;
        lr.stagesMask = r.StageMask;
        out.pushConstantRanges.push_back(std::move(lr));
    }

    out.vertexBindings   = gd.VertexBindings;
    out.vertexAttributes = gd.VertexAttributes;
    out.topology         = gd.Topology;

    // Fill the format-bearing fields from the format key.
    out.colorAttachmentFormats.reserve(fk.ColorCount);
    for (uint8_t i = 0; i < fk.ColorCount && i < PipelineFormatKey::kMaxColors; ++i)
    {
        out.colorAttachmentFormats.push_back(static_cast<uint32_t>(fk.ColorFormats[i]));
    }
    out.depthAttachmentFormat   = static_cast<uint32_t>(fk.DepthFormat);
    out.stencilAttachmentFormat = static_cast<uint32_t>(fk.StencilFormat);
    out.rasterizationSamples    = fk.RasterizationSamples;

    out.debugName = gd.DebugName.empty() ? nullptr : gd.DebugName.c_str();
    out.specializationConstants = gd.Specialization.has_value() ? &(*gd.Specialization) : nullptr;

    return out;
}

PipelineDesc PipelineCache::BuildPipelineDescForCompute(const ComputePipelineDesc& cd,
                                                        const PipelineCache& cache)
{
    PipelineDesc out{};
    out.type = PipelineType::Compute;

    if (cd.ComputeShader) out.computeShader = *cd.ComputeShader;

    out.descriptorSetLayouts.reserve(cd.DescriptorSetLayouts.size());
    for (auto layoutId : cd.DescriptorSetLayouts)
    {
        if (const auto* dsl = cache.LookupDescriptorSetLayout(layoutId))
            out.descriptorSetLayouts.push_back(*dsl);
        else
            out.descriptorSetLayouts.emplace_back();
    }

    out.pushConstantSize       = cd.PushConstants.Size;
    out.pushConstantStagesMask = cd.PushConstants.StageMask;
    out.pushConstantRanges.reserve(cd.NamedPushConstantRanges.size());
    for (const auto& r : cd.NamedPushConstantRanges)
    {
        PipelineDesc::PushConstantRangeDesc lr{};
        lr.name      = r.Name;
        lr.offset    = r.Offset;
        lr.size      = r.Size;
        lr.stagesMask = r.StageMask;
        out.pushConstantRanges.push_back(std::move(lr));
    }

    out.debugName = cd.DebugName.empty() ? nullptr : cd.DebugName.c_str();
    out.specializationConstants = cd.Specialization.has_value() ? &(*cd.Specialization) : nullptr;

    return out;
}

} // namespace GameEngine::Rendering
