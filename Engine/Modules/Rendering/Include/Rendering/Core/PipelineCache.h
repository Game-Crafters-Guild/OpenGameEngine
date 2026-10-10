// PipelineCache.h — pipeline interning + concrete (id, formats) cache.
//
// Three responsibilities:
//
//  1) Intern tables — `InternGraphicsPipeline(desc)`, `InternComputePipeline`,
//     `InternDescriptorSetLayout`. Identical content (after the desc's
//     content hash) yields the same `Id`. Storage is owned by the cache so
//     interned descs outlive caller stack frames (essential for the
//     `std::optional`-owning fields `DynamicState` and `Specialization`).
//
//  2) Concrete cache — combined-key map of `(graphicsPipelineId.Value << 32)
//     | (formatKey.Hash() & 0xFFFFFFFF)` → `PipelineHandle`. Miss creation
//     is driven by `IDevice`; the cache exposes `TryGetConcrete` /
//     `InsertConcrete`.
//
//  3) Tag-based selective invalidation: `RenderingHotReloadBridge` invalidates
//     entries by SPIR-V or material tag.

#pragma once

#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine::Rendering
{

// (`PipelineCacheStats` lives in `PipelineIdentifiers.h` so `IDevice` can
// declare `GetPipelineCacheStats()` without including the full cache header.)

class PipelineCache
{
public:
    PipelineCache() = default;
    ~PipelineCache() = default;

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    // ---- Intern tables ----

    // Returns a stable id for the canonicalized desc. Idempotent: identical
    // content (after `ContentHash`) returns the same id. The interned desc
    // is owned by the cache and outlives any caller stack frame.
    GraphicsPipelineId InternGraphicsPipeline(GraphicsPipelineDesc desc);
    ComputePipelineId  InternComputePipeline(ComputePipelineDesc desc);
    DescriptorSetLayoutId InternDescriptorSetLayout(DescriptorSetLayoutDesc desc);

    // Lookup (mutex-guarded). Returns nullptr for invalid/freed ids.
    // NOTE: the returned pointer aliases internal storage that concurrent
    // Intern* calls may reallocate — callers that hold the result across
    // other cache operations (or do slow work, like backend shader
    // compilation) must use TryCopyDescriptorSetLayout instead.
    const GraphicsPipelineDesc*    LookupGraphicsPipeline(GraphicsPipelineId id) const;
    const ComputePipelineDesc*     LookupComputePipeline(ComputePipelineId id) const;
    const DescriptorSetLayoutDesc* LookupDescriptorSetLayout(DescriptorSetLayoutId id) const;

    // Copies the interned desc under the cache mutex; safe to use from any
    // thread regardless of concurrent interning. Returns false for
    // invalid/freed ids. Cheap: pipeline shader bytes are shared_ptr-backed.
    bool TryCopyDescriptorSetLayout(DescriptorSetLayoutId id, DescriptorSetLayoutDesc& outDesc) const;
    bool TryCopyGraphicsPipeline(GraphicsPipelineId id, GraphicsPipelineDesc& outDesc) const;
    bool TryCopyComputePipeline(ComputePipelineId id, ComputePipelineDesc& outDesc) const;

    // Pin: prevent the interned desc from being freed when its concrete
    // refcount drops to zero. Used for material variants of active scenes
    // and for compute pipelines (eviction is pointless for those).
    void PinGraphicsPipeline(GraphicsPipelineId id);
    void PinComputePipeline(ComputePipelineId id);

    // ---- Concrete cache ----

    // Combine `(id.Value << 32) | (formatKey.Hash() & 0xFFFFFFFF)` for
    // graphics; for compute use `(id.Value << 32)` (formatHash low 32 = 0).
    // The 32-bit format-hash collision domain over ~10⁴ entries is fine.
    static uint64_t CombineGraphicsKey(GraphicsPipelineId id, const PipelineFormatKey& fk) noexcept;
    static uint64_t CombineComputeKey(ComputePipelineId id) noexcept;

    // Lookup. Returns true and sets `outHandle` on hit; updates LRU; bumps
    // hit/miss counters. Caller is responsible for going to the device on
    // miss and inserting.
    bool TryGetConcrete(uint64_t combinedKey, PipelineHandle& outHandle);

    // Insert a concrete entry. Bumps the interned desc's refcount and may
    // trigger LRU eviction. Returns false, inserting nothing, when
    // `InvalidationEpoch` is set and the cache was invalidated since that value
    // was read from GetInvalidationEpoch: a build that started before a hot
    // reload or a device rebuild must not publish into the cleared cache.
    struct ConcreteInsertInfo
    {
        PipelineHandle Handle{};
        PipelineFormatKey FormatKey{}; // ignored for compute (use default)
        uint32_t PipelineIdValue = 0;  // GraphicsPipelineId or ComputePipelineId .Value
        bool IsCompute = false;
        bool Pinned = false;
        std::vector<uint64_t> Tags; // for hot-reload selective invalidation
        std::optional<uint64_t> InvalidationEpoch;
    };
    bool InsertConcrete(uint64_t combinedKey, const ConcreteInsertInfo& info);

    // Moves on Clear, ClearConcreteOnly and an InvalidateByTag* that removed an
    // entry, and on nothing else (LRU eviction leaves it alone). A pipeline build
    // reads it when it starts and hands it back to InsertConcrete.
    uint64_t GetInvalidationEpoch() const { return m_InvalidationEpoch.load(std::memory_order_acquire); }

    // ---- Capacity / eviction ----

    // 0 == unlimited. When non-zero, LRU evicts when size exceeds capacity.
    void SetCapacity(size_t maxEntries);
    size_t GetCapacity() const;

    // ---- Tag-based selective invalidation (hot reload) ----

    // Removes all entries whose `Tags` contained the given tag. Returns
    // number of removed entries.
    size_t InvalidateByTag(uint64_t tag);
    size_t InvalidateByTags(const std::vector<uint64_t>& tags);

    void Clear();

    // Q6 device-lost re-provision (design §8): drop every concrete backend pipeline
    // entry while KEEPING the intern tables (desc + SPIR-V) intact, so PSOs
    // lazily recompile from RAM against the warm-reloaded backend pipeline cache. The
    // concrete PipelineHandles are already dead (the in-place rebuild destroyed
    // the backend pipelines and deregistered the handles), so this must NOT route
    // through DestroyPipeline — it only forgets the map. Unlike Clear(), it does
    // not tombstone interned descs (that would force a full re-intern instead of
    // a warm recompile).
    void ClearConcreteOnly();

    // ---- Stats ----

    PipelineCacheStats GetStats() const;

    // ---- Backend creation feed ----
    //
    // Materialize a `PipelineDesc` from an interned graphics or compute desc
    // (plus a `PipelineFormatKey` for graphics). The IDevice cache-miss path
    // builds one and forwards it to the backend's `CreatePipeline` virtual.
    static PipelineDesc BuildPipelineDescForGraphics(
        const GraphicsPipelineDesc& gd, const PipelineFormatKey& fk,
        const PipelineCache& cache);
    static PipelineDesc BuildPipelineDescForCompute(
        const ComputePipelineDesc& cd, const PipelineCache& cache);

private:
    // Concrete entry (per `(pipelineId, formatKey)` pair).
    struct ConcreteEntry
    {
        PipelineHandle Handle{};
        PipelineFormatKey FormatKey{};
        uint32_t PipelineIdValue = 0;
        bool IsCompute = false;
        bool Pinned = false;
        // For LRU mode only — iterator into m_LruList. Unused when capacity == 0.
        std::list<uint64_t>::iterator LruIt{};
    };

    // Interned graphics-pipeline entry. Refcount = number of live concrete
    // entries; when refcount hits 0 AND not pinned, the entry's slot is
    // freed (id slot becomes a tombstone — ids are monotonic, never reused).
    template<typename DescT>
    struct InternEntry
    {
        DescT Desc;
        uint32_t ConcreteRefcount = 0;
        bool Pinned = false;
        bool Live = true; // false == tombstone
    };

    // Mutex protects everything. Hot path passes through here once on miss
    // and once on insert; lookups via `TryGetConcrete` also acquire briefly.
    // Substep 2.7 adds a TLS L1 in front of this so most graphics-pipeline
    // lookups never hit the mutex.
    mutable std::mutex m_Mutex;

    // ---- Concrete map ----
    std::unordered_map<uint64_t, ConcreteEntry> m_Concrete;
    std::list<uint64_t> m_LruList; // front = most recent. LRU mode only.
    size_t m_Capacity = 0;         // 0 == unlimited

    // ---- Tag indices ----
    std::unordered_map<uint64_t, std::vector<uint64_t>> m_KeyToTags;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> m_TagToKeys;

    // ---- Intern tables ----
    // Hash → id; vector indexed by id.Value-1.
    std::unordered_map<uint64_t, uint32_t> m_GraphicsHashToId;
    std::vector<InternEntry<GraphicsPipelineDesc>> m_GraphicsEntries;

    std::unordered_map<uint64_t, uint32_t> m_ComputeHashToId;
    std::vector<InternEntry<ComputePipelineDesc>> m_ComputeEntries;

    std::unordered_map<uint64_t, uint32_t> m_LayoutHashToId;
    std::vector<InternEntry<DescriptorSetLayoutDesc>> m_LayoutEntries;

    // ---- Stats counters (atomic so GetStats() is read-only) ----
    // `m_L1Hits` covers fast-path hits, `m_SlowHits` covers mutex-locked
    // map hits. `Hits` reported by GetStats() is the sum so the L1 hot
    // path only pays for one atomic increment. `m_L1Hits` is mutable so
    // const `GetStats()` can flush the calling thread's batched count.
    std::atomic<uint64_t> m_SlowHits{0};
    std::atomic<uint64_t> m_Misses{0};
    std::atomic<uint64_t> m_Inserts{0};
    std::atomic<uint64_t> m_Evictions{0};
    std::atomic<uint64_t> m_Clears{0};
    mutable std::atomic<uint64_t> m_L1Hits{0};
    std::atomic<uint64_t> m_L1Misses{0};

    // Hands out the epoch values that stamp TLS L1 entries. Strictly increasing
    // and never reused for the process lifetime, so an epoch identifies one
    // cache instance in one state; an address cannot, because the allocator
    // reissues a dead cache's address to the next one.
    static uint64_t NextCacheEpoch() noexcept;

    // Redrawn on `Clear`, `InvalidateByTag*` and LRU eviction to signal TLS L1
    // entries that they're stale, and drawn once at construction so no entry an
    // earlier cache left behind can validate against this one. Eviction never
    // destroys the pipeline it drops from the map, so this guards against a
    // recycled L1 slot index serving an unrelated key — not against a dangling
    // handle.
    std::atomic<uint64_t> m_CacheEpoch{NextCacheEpoch()};

    // See GetInvalidationEpoch. Written under m_Mutex, so InsertConcrete's check
    // and an invalidation cannot interleave.
    std::atomic<uint64_t> m_InvalidationEpoch{0};

    // Internal: caller holds m_Mutex.
    void EnforceCapacity_NoLock();
    size_t RemoveKey_NoLock(uint64_t key);
    void DecrementInternRefcount_NoLock(const ConcreteEntry& entry);
    size_t InvalidateByTag_NoLock(uint64_t tag);
};

} // namespace GameEngine::Rendering
