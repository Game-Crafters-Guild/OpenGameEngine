#pragma once
#include "ECS/ChunkCallAdapter.h"

#include "ECS/ChangeFilter.h"
#include "ECS/ComponentConcepts.h"
#include "ECS/ECS.h"
#include "ECS/Entity.h"
#include "ECS/QueryAccessProbe.h"
#include "ECS/QueryPolicy.h"
#include "ECS/World.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include <array>
#include <cassert>
#include <memory>
#include <shared_mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace GameEngine
{
namespace ECS
{

// Forward declarations for advanced query features
template <Component... Ts>
class ExcludeQuery;
template <Component... Ts>
class OptionalQuery;

// Compile-time type tag
template <typename T>
struct TypeTag
{
    using type = T;
};

// Query interface — iterates colocated ArchetypeTable chunks directly.
// All components live in the same 16KB cache block — no ChunkCursor needed.
//
// Access semantics (change-signaling design §4.3): read/write access per
// component is the union of declared qualifiers (Read<T>/Write<T>) and
// per-parameter constness inference (QueryAccessProbe.h). Declared and
// inferred reads are INVOKED as const; visiting a chunk with write access
// stamps its written columns (stamp-at-visit, §4.4) so Changed<>-filtered
// consumers can skip clean chunks.
template <QualifiedComponent... Ts>
class Query
{
  private:
    template <typename T>
    using U = Underlying<T>;

    template <std::size_t I>
    using NthType = Detail::NthTypeOf<I, Ts...>;

    World* world;
    mutable std::vector<Archetype*> m_CachedArchetypes;
    mutable bool m_CacheValid = false;
    mutable std::shared_mutex m_CacheMutex;
    mutable std::size_t m_CachedStructuralVersion = 0;

    // Filters set by Without<>/With<>. Each names its types at the call site,
    // so both point at the shared signature of that pack (SignatureOf) rather
    // than owning one; null means the filter is not set.
    const ComponentSignature* m_ExcludeSignature = nullptr;
    const ComponentSignature* m_IncludeSignature = nullptr;

    // Enable-state opt-in (IncludeDisabled). Both default to "off", which is
    // the exclude-disabled-by-default rule; the per-type form points at the
    // shared signature of its pack, like the filters above.
    const ComponentSignature* m_IncludeDisabledSignature = nullptr;
    bool m_IncludeAllDisabled = false;

    // Changed<> filter state (design §4.2). Slots index into Ts....
    static constexpr std::size_t kMaxChangedTypes = 4;
    std::array<int, kMaxChangedTypes> m_ChangedSlots{};
    uint32_t m_ChangedSlotCount = 0;
    uint64_t m_ChangedGate = 0;
    bool m_HasChangedGate = false;

    void UpdateCache() const;

    // The three signatures the type list determines. Each is built once per
    // Query<Ts...> instantiation and read by every construction of it, so
    // constructing a query costs no allocation: the type list cannot differ
    // between two queries of the same instantiation.
    //
    // Read/Write are the declared UPPER BOUND only — effective access is
    // computed per dispatch from the callback signature (design M13); stamping
    // and future wave validation use the effective sets, not these. Required
    // holds the types an archetype must have, which is every type but the
    // Optional ones.
    static ComponentSignature BuildReadSignature()
    {
        ComponentSignature signature;
        ((IsRead<Ts>::value ? void(signature.Add(GetComponentTypeId<U<Ts>>())) : void()), ...);
        return signature;
    }

    static ComponentSignature BuildWriteSignature()
    {
        ComponentSignature signature;
        ((IsRead<Ts>::value ? void() : void(signature.Add(GetComponentTypeId<U<Ts>>()))), ...);
        return signature;
    }

    static ComponentSignature BuildRequiredSignature()
    {
        ComponentSignature signature;
        ((IsOptional<Ts>::value ? void() : void(signature.Add(GetComponentTypeId<U<Ts>>()))), ...);
        return signature;
    }

    static const ComponentSignature& ReadSignature()
    {
        static const ComponentSignature signature = BuildReadSignature();
        return signature;
    }

    static const ComponentSignature& WriteSignature()
    {
        static const ComponentSignature signature = BuildWriteSignature();
        return signature;
    }

    static const ComponentSignature& RequiredSignature()
    {
        static const ComponentSignature signature = BuildRequiredSignature();
        return signature;
    }

    // Requiring either activity tag opts the query into inactive rows: the two
    // are one state, and a query that asks for it wants the rows that carry it.
    static constexpr bool kRequiresActivityTag =
        ((!IsOptional<Ts>::value && (std::is_same_v<U<Ts>, Disabled> ||
                                     std::is_same_v<U<Ts>, DisabledInHierarchy>)) ||
         ...);

    // The enable-state tags whose presence on an archetype takes it out of
    // this query: entity activity (Disabled, DisabledInHierarchy) plus
    // ComponentDisabled<T> for every REQUIRED T — an Optional T that is
    // switched off reads as absent instead (ClearDisabledOptional below).
    // A required type is never its own exclusion.
    static ComponentSignature BuildDisabledExcludeSignature()
    {
        ComponentSignature signature;
        if constexpr (!kRequiresActivityTag)
        {
            signature.Add(GetComponentTypeId<Disabled>());
            signature.Add(GetComponentTypeId<DisabledInHierarchy>());
        }
        ((IsOptional<Ts>::value
              ? void()
              : void(signature.Add(GetComponentTypeId<ComponentDisabled<U<Ts>>>()))),
         ...);
        for (ComponentTypeId id : RequiredSignature().GetComponents())
            signature.Remove(id);
        return signature;
    }

    static const ComponentSignature& DisabledExcludeSignature()
    {
        static const ComponentSignature signature = BuildDisabledExcludeSignature();
        return signature;
    }

    // Slots whose Optional<T> must read as absent where T is switched off:
    // every Optional parameter the query did not opt back in.
    uint32_t DisabledOptionalMask() const
    {
        static_assert(sizeof...(Ts) <= 32, "one mask bit per query parameter");
        if (m_IncludeAllDisabled)
            return 0u;
        uint32_t mask = 0u;
        std::size_t slot = 0;
        (((IsOptional<Ts>::value &&
           !(m_IncludeDisabledSignature != nullptr &&
             m_IncludeDisabledSignature->Contains(GetComponentTypeId<ComponentDisabled<U<Ts>>>()))
               ? void(mask |= (1u << slot))
               : void()),
          ++slot),
         ...);
        return mask;
    }

    // Per archetype, next to the column lookup it corrects.
    template <std::size_t I>
    void ClearDisabledOptional(const ColumnLayout& layout,
                               std::array<int, sizeof...(Ts)>& colIdx, uint32_t mask) const
    {
        if constexpr (IsOptional<NthType<I>>::value)
        {
            if (((mask >> I) & 1u) != 0u && colIdx[I] >= 0 &&
                layout.FindColumnIndex(GetComponentTypeId<ComponentDisabled<U<NthType<I>>>>()) >= 0)
            {
                colIdx[I] = -1;
            }
        }
    }

    void ApplyDisabledOptionalRule(const ColumnLayout& layout,
                                   std::array<int, sizeof...(Ts)>& colIdx, uint32_t mask) const
    {
        if (mask == 0u)
            return;
        [&]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            (ClearDisabledOptional<Is>(layout, colIdx, mask), ...);
        }(std::make_index_sequence<sizeof...(Ts)>{});
    }

    ArchetypeFilter BuildArchetypeFilter() const
    {
        ArchetypeFilter filter;
        filter.Required = &RequiredSignature();
        filter.Include = m_IncludeSignature;
        filter.Exclude = m_ExcludeSignature;
        filter.DisabledExclude = m_IncludeAllDisabled ? nullptr : &DisabledExcludeSignature();
        filter.DisabledInclude = m_IncludeDisabledSignature;
        return filter;
    }

    // Compile-time slot lookup of component C among Underlying<Ts>...
    template <typename C>
    static constexpr int SlotOf()
    {
        if constexpr (sizeof...(Ts) == 0)
        {
            return -1;
        }
        else
        {
            constexpr bool matches[] = {std::is_same_v<C, U<Ts>>...};
            for (std::size_t i = 0; i < sizeof...(Ts); ++i)
            {
                if (matches[i])
                    return static_cast<int>(i);
            }
            return -1;
        }
    }

    // Resolve the filtered slots to this archetype's column indices.
    void ResolveChangedColumns(const std::array<int, sizeof...(Ts)>& colIdx,
                               std::array<int, kMaxChangedTypes>& outCols) const
    {
        for (uint32_t k = 0; k < m_ChangedSlotCount; ++k)
            outCols[k] = colIdx[static_cast<std::size_t>(m_ChangedSlots[k])];
    }

    bool ChunkPassesGate(const ArchetypeTable& table, std::size_t chunkIdx,
                         const std::array<int, kMaxChangedTypes>& changedCols) const
    {
        return table.AnyColumnChangedSince(
            chunkIdx, std::span<const int>(changedCols.data(), m_ChangedSlotCount), m_ChangedGate);
    }

    // Stamp-at-visit (design §4.4, C14): store the dispatch's version into
    // each effective-write column of a visited chunk BEFORE the callback.
    // Skipped (filtered / empty) chunks are never stamped.
    static void StampWriteColumns(ArchetypeTable& table, std::size_t chunkIdx,
                                  const std::array<int, sizeof...(Ts)>& colIdx,
                                  uint32_t writeMask, uint64_t version)
    {
        for (std::size_t s = 0; s < sizeof...(Ts); ++s)
        {
            if (((writeMask >> s) & 1u) != 0u && colIdx[s] >= 0)
                table.StampColumnVersion(chunkIdx, static_cast<std::size_t>(colIdx[s]), version);
        }
    }

  public:
    explicit Query(World* w) : world(w) {}

    // Sequential iteration (PascalCase canonical)
    template <typename Func>
    void Each(Func&& func);

    // Batch iteration: callback receives raw pointers + count for SIMD-friendly processing.
    template <typename Func>
    void BatchEach(Func&& func);

    // Parallel iteration using job system. Synchronous fork-join: batches are
    // dispatched through a JobCounter and joined before returning (slice 5 —
    // no more vector<TaskHandle> wave joins). Safe to call from a worker
    // thread: the join participates (executes only this query's batches).
    template <typename Func>
    void Parallel(Func&& func, std::size_t minBatchSize = 1000);

    // Parallel batch iteration: combines Parallel's chunk-batching with BatchEach's
    // raw-pointer callback. Callback signature: func(U<Ts>*..., size_t count).
    // Invoked once per chunk, multiple chunks may run on the same task.
    // Fastest path for SIMD workloads over large entity populations.
    // minBatchSize==0 uses QueryPolicy::MinBatchSizeDefaultBatch (10000 by default).
    // Synchronous fork-join (see Parallel).
    template <typename Func>
    void ParallelBatchEach(Func&& func, std::size_t minBatchSize = 0);

    // Direct chunk-based iteration for maximum performance
    template <typename Func>
    void ForEachChunk(Func&& func);

    // Parallel chunk iteration. Synchronous fork-join (see Parallel).
    template <typename Func>
    void ParallelChunks(Func&& func);

    // Adaptive parallelization
    template <typename Func>
    void Adaptive(Func&& func);

    template <typename Func>
    void Adaptive(Func&& func, std::size_t sequentialThreshold, std::size_t chunkThreshold);

    // Get count of matching entities. Ignores any Changed<> filter
    // (archetype-level; design §4.2 — no CountChanged in v1).
    std::size_t Count() const;

    // Changed-only filtering (design §4.2): skip chunks whose filtered
    // columns were not write-granted after gate.LastRunVersion. Multiple
    // types = OR semantics (chunk visited if ANY filtered column is newer),
    // matching DOTS. Honored by every iteration mode, including Adaptive's
    // forwarding (M14).
    //
    // Q2: each C must be one of the query's component types Ts...
    // (static_assert) — filtering un-queried types would force column
    // resolution the iteration loops never do. With<>-included types are not
    // supported as filter keys in v1 (no consumer needs it — minimal API).
    //
    // Consumer contract: entry-sample World::GetGlobalSystemVersion() BEFORE
    // iterating and write that sample back as the gate for the next run
    // (design M14 — never an end-of-run resample). See ChangeGate.
    template <Component... Cs>
    Query& Changed(const ChangeGate& gate)
    {
        static_assert(sizeof...(Cs) >= 1, "Changed<> requires at least one component type");
        static_assert(sizeof...(Cs) <= kMaxChangedTypes,
                      "Changed<> supports at most kMaxChangedTypes component types");
        static_assert(((SlotOf<Cs>() >= 0) && ...),
                      "ECS Query: Changed<T> requires T to be one of the query's component "
                      "types (design Q2 -- un-queried types are never column-resolved)");
        m_ChangedSlots = {};
        std::size_t k = 0;
        ((m_ChangedSlots[k++] = SlotOf<Cs>()), ...);
        m_ChangedSlotCount = static_cast<uint32_t>(sizeof...(Cs));
        m_ChangedGate = gate.LastRunVersion;
        m_HasChangedGate = true;
        return *this;
    }

    // Include components beyond Ts..., matching only archetypes that have all
    // of them. One call per query: it replaces the include filter rather than
    // adding to it, so name every included type in the same call. Naming the
    // same pack again is the same filter, which is what a retained query does
    // every frame; naming a different one asserts in dev builds.
    template <Component... IncludeTs>
    Query<Ts...>& With()
    {
        const ComponentSignature& signature = SignatureOf<IncludeTs...>();
        assert((m_IncludeSignature == nullptr || *m_IncludeSignature == signature) &&
               "With<> replaces the include filter: name every included type in one call");
        m_IncludeSignature = &signature;
        m_CacheValid = false;
        return *this;
    }

    // Exclude components, matching only archetypes that have none of them. One
    // call per query: it replaces the exclude filter rather than adding to it,
    // so name every excluded type in the same call. Naming the same pack again
    // is the same filter, which is what a retained query does every frame;
    // naming a different one asserts in dev builds.
    template <Component... ExcludeTs>
    Query<Ts...>& Without()
    {
        const ComponentSignature& signature = SignatureOf<ExcludeTs...>();
        assert((m_ExcludeSignature == nullptr || *m_ExcludeSignature == signature) &&
               "Without<> replaces the exclude filter: name every excluded type in one call");
        m_ExcludeSignature = &signature;
        m_CacheValid = false;
        return *this;
    }

    // See the rows the enable model hides. Queries exclude them by default:
    // an inactive entity, and a row whose required T is switched off, are not
    // visited at all. A query whose job is to maintain or present entities as
    // data — scene IO, GPU-slot cleanup, the hierarchy, undo — opts back in.
    //
    // The unqualified form sees everything. The per-type form sees rows whose
    // named components are off but still skips inactive entities, and it also
    // turns off the Optional null rule for those types.
    Query<Ts...>& IncludeDisabled()
    {
        m_IncludeAllDisabled = true;
        m_CacheValid = false;
        return *this;
    }

    // One call per query, like With<>/Without<>: it replaces the opt-in rather
    // than adding to it, so name every type in the same call.
    template <Component IncludeT, Component... MoreTs>
    Query<Ts...>& IncludeDisabled()
    {
        const ComponentSignature& signature =
            SignatureOf<ComponentDisabled<IncludeT>, ComponentDisabled<MoreTs>...>();
        assert((m_IncludeDisabledSignature == nullptr || *m_IncludeDisabledSignature == signature) &&
               "IncludeDisabled<> replaces the opt-in: name every type in one call");
        m_IncludeDisabledSignature = &signature;
        m_CacheValid = false;
        return *this;
    }

    // Expose read/write sets for external schedulers. Declared upper bound
    // only — see the signature comment (M13).
    const ComponentSignature& GetReadSet() const { return ReadSignature(); }
    const ComponentSignature& GetWriteSet() const { return WriteSignature(); }
};

// Simplified ExcludeQuery placeholder
template <Component... Ts>
class ExcludeQuery
{
  private:
    World* world;

  public:
    explicit ExcludeQuery(World* w) : world(w) {}

    template <typename Func>
    void Each(Func&& func)
    {
    }

    std::size_t Count() const
    {
        return 0;
    }
};

// ============================================================================
// Template implementations — all iteration uses colocated ArchetypeTable chunks.
// ============================================================================

template <QualifiedComponent... Ts>
void Query<Ts...>::UpdateCache() const
{
    std::shared_lock vlock(m_CacheMutex);
    if (m_CacheValid && m_CachedStructuralVersion == world->structuralChangeVersion.load(std::memory_order_relaxed))
    {
        return;
    }
    vlock.unlock();

    std::unique_lock lock(m_CacheMutex);
    world->CollectMatchingArchetypes(BuildArchetypeFilter(), m_CachedArchetypes);

    m_CachedStructuralVersion = world->structuralChangeVersion.load(std::memory_order_relaxed);
    m_CacheValid = true;
}

// Sequential iteration — one loop per colocated chunk, all data in same 16KB block.
// Column byte offsets are cached per-archetype to avoid m_Layout->Columns[] indirection per-chunk.
// Supported callback shapes:
//   func(EntityHandle, args...)  — with entity handle
//   func(args...)                — component-only (skips EH load)
// where each arg is const U&/U& (Optional: const U*/U*) per the effective
// access computed by QueryAccessProbe.h.
template <QualifiedComponent... Ts>
template <typename Func>
void Query<Ts...>::Each(Func&& func)
{
    UpdateCache();

    using F = std::decay_t<Func>;
    // Shape hypothesis: entity-first wins when both arities match (existing
    // dispatch precedence).
    constexpr bool kWantsEntity = std::is_invocable_v<F, EntityHandle, Detail::PlainRefArg<Ts>...>;
    constexpr bool kComponentOnly = std::is_invocable_v<F, Detail::PlainRefArg<Ts>...>;
    static_assert(kWantsEntity || kComponentOnly,
                  "Unsupported functor signature for Query::Each -- expected "
                  "(EntityHandle, components...) or (components...)");

    using Probe = Detail::AccessProbe<
        F, kWantsEntity ? Detail::CallShape::EntityRef : Detail::CallShape::PlainRef, Ts...>;
    constexpr uint32_t kReadMask = Probe::kReadMask;
    constexpr uint32_t kWriteMask = Probe::kWriteMask;

    const uint64_t stampVersion = (kWriteMask != 0u) ? world->NextGlobalSystemVersion() : 0u;
    const uint32_t disabledOptionalMask = DisabledOptionalMask();

    for (auto* archetype : m_CachedArchetypes)
    {
        if (archetype->GetEntityCount() == 0 || !archetype->HasTable())
            continue;

        auto& table = archetype->GetTable();
        const auto& layout = table.GetLayout();

        // Resolve column indices AND cache byte offsets (once per archetype, not per chunk).
        std::array<int, sizeof...(Ts)> colIdx = {
            layout.FindColumnIndex(GetComponentTypeId<U<Ts>>())...};
        ApplyDisabledOptionalRule(layout, colIdx, disabledOptionalMask);

        // Verify required columns exist.
        {
            int ci = 0;
            bool valid = true;
            ((IsOptional<Ts>::value ? (++ci, void()) : void(valid = valid && colIdx[ci++] >= 0)), ...);
            if (!valid)
                continue;
        }

        // Cache byte offsets to eliminate m_Layout->Columns[] lookup per-chunk.
        std::array<uint32_t, sizeof...(Ts)> colOffsets;
        for (std::size_t k = 0; k < sizeof...(Ts); ++k)
            colOffsets[k] = (colIdx[k] >= 0) ? layout.Columns[colIdx[k]].Offset : 0;
        const uint32_t ehOffset = layout.EntityHandleOffset;

        std::array<int, kMaxChangedTypes> changedCols{};
        if (m_HasChangedGate)
            ResolveChangedColumns(colIdx, changedCols);

        auto chunks = table.GetChunks();
        for (std::size_t chunkIdx = 0; chunkIdx < chunks.size(); ++chunkIdx)
        {
            auto& chunk = chunks[chunkIdx];
            const uint32_t chunkCount = chunk.GetCount();
            if (chunkCount == 0)
                continue;
            if (m_HasChangedGate && !ChunkPassesGate(table, chunkIdx, changedCols))
                continue;
            if constexpr (kWriteMask != 0u)
                StampWriteColumns(table, chunkIdx, colIdx, kWriteMask, stampVersion);

            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
                // Resolve column pointers using cached byte offsets (one pointer-add each).
                auto columnPtrs = std::make_tuple(
                    (colIdx[Is] >= 0
                         ? chunk.template GetColumnByOffset<U<NthType<Is>>>(colOffsets[Is])
                         : static_cast<U<NthType<Is>>*>(nullptr))...);

                // Per-slot argument with the effective (inferred) constness.
                const auto argAt = [&]<std::size_t J>(std::integral_constant<std::size_t, J>,
                                                      uint32_t i) -> decltype(auto)
                {
                    using Q = NthType<J>;
                    constexpr bool kIsRead = ((kReadMask >> J) & 1u) != 0u;
                    auto* ptr = std::get<J>(columnPtrs);
                    if constexpr (IsOptional<Q>::value)
                    {
                        if constexpr (kIsRead)
                            return static_cast<const U<Q>*>(ptr ? &ptr[i] : nullptr);
                        else
                            return ptr ? &ptr[i] : nullptr;
                    }
                    else
                    {
                        if constexpr (kIsRead)
                            return static_cast<const U<Q>&>(ptr[i]);
                        else
                            return static_cast<U<Q>&>(ptr[i]);
                    }
                };

                if constexpr (kWantsEntity)
                {
                    const EntityHandle* entityPtr = chunk.GetEntityHandlesByOffset(ehOffset);
                    for (uint32_t i = 0; i < chunkCount; ++i)
                        func(entityPtr[i], argAt(std::integral_constant<std::size_t, Is>{}, i)...);
                }
                else
                {
                    for (uint32_t i = 0; i < chunkCount; ++i)
                        func(argAt(std::integral_constant<std::size_t, Is>{}, i)...);
                }
            }(std::make_index_sequence<sizeof...(Ts)>{});
        }
    }
}

// Batch iteration — one batch per colocated chunk, all pointers aligned.
// Callback: func(U<Ts>*..., size_t count) with const U* for effective-read
// slots. Optional components get nullptr.
template <QualifiedComponent... Ts>
template <typename Func>
void Query<Ts...>::BatchEach(Func&& func)
{
    UpdateCache();

    using F = std::decay_t<Func>;
    constexpr bool kWantsEntity =
        std::is_invocable_v<F, const EntityHandle*, Detail::PlainPtrArg<Ts>..., std::size_t>;
    static_assert(kWantsEntity || std::is_invocable_v<F, Detail::PlainPtrArg<Ts>..., std::size_t>,
                  "Unsupported functor signature for Query::BatchEach -- expected "
                  "(T*..., size_t) or (const EntityHandle*, T*..., size_t)");

    using Probe = Detail::AccessProbe<
        F, kWantsEntity ? Detail::CallShape::EntityPtrCount : Detail::CallShape::PtrCount, Ts...>;
    constexpr uint32_t kReadMask = Probe::kReadMask;
    constexpr uint32_t kWriteMask = Probe::kWriteMask;

    const uint64_t stampVersion = (kWriteMask != 0u) ? world->NextGlobalSystemVersion() : 0u;
    const uint32_t disabledOptionalMask = DisabledOptionalMask();

    for (auto* archetype : m_CachedArchetypes)
    {
        if (archetype->GetEntityCount() == 0 || !archetype->HasTable())
            continue;

        auto& table = archetype->GetTable();
        const auto& layout = table.GetLayout();

        std::array<int, sizeof...(Ts)> colIdx = {
            layout.FindColumnIndex(GetComponentTypeId<U<Ts>>())...};
        ApplyDisabledOptionalRule(layout, colIdx, disabledOptionalMask);

        {
            int ci = 0;
            bool valid = true;
            ((IsOptional<Ts>::value ? (++ci, void()) : void(valid = valid && colIdx[ci++] >= 0)), ...);
            if (!valid)
                continue;
        }

        // Cache byte offsets per-archetype.
        std::array<uint32_t, sizeof...(Ts)> colOffsets;
        for (std::size_t k = 0; k < sizeof...(Ts); ++k)
            colOffsets[k] = (colIdx[k] >= 0) ? layout.Columns[colIdx[k]].Offset : 0;

        const uint32_t ehOffset = layout.EntityHandleOffset;

        std::array<int, kMaxChangedTypes> changedCols{};
        if (m_HasChangedGate)
            ResolveChangedColumns(colIdx, changedCols);

        auto chunks = table.GetChunks();
        for (std::size_t chunkIdx = 0; chunkIdx < chunks.size(); ++chunkIdx)
        {
            auto& chunk = chunks[chunkIdx];
            const uint32_t chunkCount = chunk.GetCount();
            if (chunkCount == 0)
                continue;
            if (m_HasChangedGate && !ChunkPassesGate(table, chunkIdx, changedCols))
                continue;
            if constexpr (kWriteMask != 0u)
                StampWriteColumns(table, chunkIdx, colIdx, kWriteMask, stampVersion);

            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
                if constexpr (kWantsEntity)
                {
                    func(
                        static_cast<const EntityHandle*>(chunk.GetEntityHandlesByOffset(ehOffset)),
                        Detail::EffPtrCast<((kReadMask >> Is) & 1u) != 0u>(
                            colIdx[Is] >= 0
                                ? chunk.template GetColumnByOffset<U<NthType<Is>>>(colOffsets[Is])
                                : static_cast<U<NthType<Is>>*>(nullptr))...,
                        static_cast<std::size_t>(chunkCount));
                }
                else
                {
                    func(
                        Detail::EffPtrCast<((kReadMask >> Is) & 1u) != 0u>(
                            colIdx[Is] >= 0
                                ? chunk.template GetColumnByOffset<U<NthType<Is>>>(colOffsets[Is])
                                : static_cast<U<NthType<Is>>*>(nullptr))...,
                        static_cast<std::size_t>(chunkCount));
                }
            }(std::make_index_sequence<sizeof...(Ts)>{});
        }
    }
}

// ParallelBatchEach — combines Parallel's chunk batching with BatchEach's raw-pointer callback.
// Callback signature: func(U<Ts>*..., size_t count), or the entity-aware form
// func(const EntityHandle*, U<Ts>*..., size_t count) — the chunk's parallel EntityHandle
// column (index i corresponds to component index i; used by dirty-feed producers).
// Invoked once per chunk; multiple chunks may run on the same task. Fastest path for
// SIMD workloads over large entity populations.
//
// IMPORTANT:
// - The functor is shared across all tasks. It must be thread-safe (no mutable captures
//   unless protected by atomics or thread_local storage).
// - Optional<T> components pass nullptr when the archetype lacks the column. Callbacks
//   MUST check for null before dereferencing optional pointers.
// - Callers must ensure no structural changes (entity create/destroy, add/remove component,
//   CompactChunks, PruneEmptyArchetypes) run on the same World during iteration.
template <QualifiedComponent... Ts>
template <typename Func>
void Query<Ts...>::ParallelBatchEach(Func&& func, [[maybe_unused]] std::size_t minBatchSize)
{
    UpdateCache();

    if (!world->jobSystem)
    {
        BatchEach(std::forward<Func>(func));
        return;
    }

    using F = std::decay_t<Func>;
    constexpr bool kWantsEntity =
        std::is_invocable_v<F, const EntityHandle*, Detail::PlainPtrArg<Ts>..., std::size_t>;
    static_assert(kWantsEntity || std::is_invocable_v<F, Detail::PlainPtrArg<Ts>..., std::size_t>,
                  "Unsupported functor signature for Query::ParallelBatchEach -- expected "
                  "(T*..., size_t) or (const EntityHandle*, T*..., size_t)");

    using Probe = Detail::AccessProbe<
        F, kWantsEntity ? Detail::CallShape::EntityPtrCount : Detail::CallShape::PtrCount, Ts...>;
    constexpr uint32_t kReadMask = Probe::kReadMask;
    constexpr uint32_t kWriteMask = Probe::kWriteMask;

    const uint64_t stampVersion = (kWriteMask != 0u) ? world->NextGlobalSystemVersion() : 0u;
    const uint32_t disabledOptionalMask = DisabledOptionalMask();

    const QueryPolicy& policy = world->GetQueryPolicy();
    if (minBatchSize == 0)
        minBatchSize = policy.MinBatchSizeDefaultBatch;
    auto js = world->jobSystem;
    if (policy.EnableBackpressure && js)
    {
        const size_t qsize = js->GetApproximateQueueSize();
        const size_t pending = js->GetPendingTasksApprox();
        const size_t workers = std::max<size_t>(1, js->GetWorkerCount());
        if (qsize > workers * policy.QueuePressureFactor || pending > workers * policy.PendingPressureFactor)
        {
            minBatchSize = std::max(minBatchSize, policy.PressuredMinBatchFloor);
        }
    }

    JobSystem::JobCounter counter;
    auto fnShared = std::make_shared<std::decay_t<Func>>(std::forward<Func>(func));

    // Changed<> gate: clean chunks are excluded from batch accounting so a
    // clean frame dispatches zero tasks, and re-checked inside the task
    // (batch ranges may straddle skipped chunks; stamps are monotonic, so
    // the re-check can only ADD a visit, never lose one).
    const bool kHasGate = m_HasChangedGate;
    const uint64_t kGateVersion = m_ChangedGate;

    for (auto* archetype : m_CachedArchetypes)
    {
        if (archetype->GetEntityCount() == 0 || !archetype->HasTable())
            continue;

        auto& table = archetype->GetTable();
        const auto& layout = table.GetLayout();

        std::array<int, sizeof...(Ts)> colIdx = {
            layout.FindColumnIndex(GetComponentTypeId<U<Ts>>())...};
        ApplyDisabledOptionalRule(layout, colIdx, disabledOptionalMask);

        {
            int ci = 0;
            bool valid = true;
            ((IsOptional<Ts>::value ? (++ci, void()) : void(valid = valid && colIdx[ci++] >= 0)), ...);
            if (!valid)
                continue;
        }

        std::array<uint32_t, sizeof...(Ts)> colOffsets;
        for (std::size_t k = 0; k < sizeof...(Ts); ++k)
            colOffsets[k] = (colIdx[k] >= 0) ? layout.Columns[colIdx[k]].Offset : 0;

        const uint32_t ehOffset = layout.EntityHandleOffset;

        std::array<int, kMaxChangedTypes> changedCols{};
        if (kHasGate)
            ResolveChangedColumns(colIdx, changedCols);
        const uint32_t changedColCount = m_ChangedSlotCount;

        auto chunks = table.GetChunks();
        const std::size_t numChunks = chunks.size();
        ArchetypeTable* tablePtr = &table;

        // Batch chunks until we reach minBatchSize entities, then submit one task per batch.
        std::size_t batchStart = 0;
        std::size_t batchEntities = 0;

        for (std::size_t chunkIdx = 0; chunkIdx <= numChunks; ++chunkIdx)
        {
            if (chunkIdx < numChunks)
            {
                uint32_t count = chunks[chunkIdx].GetCount();
                if (count == 0)
                    continue;
                if (kHasGate && !table.AnyColumnChangedSince(
                                    chunkIdx,
                                    std::span<const int>(changedCols.data(), changedColCount),
                                    kGateVersion))
                    continue;
                batchEntities += count;
            }

            bool flush = (chunkIdx == numChunks && batchEntities > 0) ||
                         (batchEntities >= minBatchSize);
            if (!flush)
                continue;

            const std::size_t batchEnd = (chunkIdx < numChunks) ? chunkIdx + 1 : numChunks;

            auto task = [chunks, batchStart, batchEnd, colIdx, colOffsets, ehOffset, fnShared,
                         tablePtr, kHasGate, kGateVersion, changedCols, changedColCount,
                         stampVersion]()
            {
                for (std::size_t ci = batchStart; ci < batchEnd; ++ci)
                {
                    auto& chunk = chunks[ci];
                    uint32_t chunkCount = chunk.GetCount();
                    if (chunkCount == 0)
                        continue;
                    if (kHasGate && !tablePtr->AnyColumnChangedSince(
                                        ci,
                                        std::span<const int>(changedCols.data(), changedColCount),
                                        kGateVersion))
                        continue;
                    if constexpr (kWriteMask != 0u)
                        StampWriteColumns(*tablePtr, ci, colIdx, kWriteMask, stampVersion);

                    [&]<std::size_t... Is>(std::index_sequence<Is...>)
                    {
                        if constexpr (kWantsEntity)
                        {
                            (*fnShared)(
                                static_cast<const EntityHandle*>(
                                    chunk.GetEntityHandlesByOffset(ehOffset)),
                                Detail::EffPtrCast<((kReadMask >> Is) & 1u) != 0u>(
                                    colIdx[Is] >= 0
                                        ? chunk.template GetColumnByOffset<U<NthType<Is>>>(colOffsets[Is])
                                        : static_cast<U<NthType<Is>>*>(nullptr))...,
                                static_cast<std::size_t>(chunkCount));
                        }
                        else
                        {
                            (*fnShared)(
                                Detail::EffPtrCast<((kReadMask >> Is) & 1u) != 0u>(
                                    colIdx[Is] >= 0
                                        ? chunk.template GetColumnByOffset<U<NthType<Is>>>(colOffsets[Is])
                                        : static_cast<U<NthType<Is>>*>(nullptr))...,
                                static_cast<std::size_t>(chunkCount));
                        }
                    }(std::make_index_sequence<sizeof...(Ts)>{});
                }
            };

            world->jobSystem->Run(std::move(task), counter);
            batchStart = chunkIdx + 1;
            batchEntities = 0;
        }
    }

    world->jobSystem->Wait(counter);
}

// Parallel iteration — batches multiple chunks per task to reduce job submission overhead.
// With ~583 entities per colocated chunk and minBatchSize=1000, typically 2 chunks per task.
// IMPORTANT: The functor is shared across all tasks. It must be thread-safe — no mutable
// captures unless protected by atomics or thread_local storage.
template <QualifiedComponent... Ts>
template <typename Func>
void Query<Ts...>::Parallel(Func&& func, [[maybe_unused]] std::size_t minBatchSize)
{
    UpdateCache();

    if (!world->jobSystem)
    {
        Each(std::forward<Func>(func));
        return;
    }

    using F = std::decay_t<Func>;
    // Parallel only supports callbacks that take EntityHandle as first arg.
    // For component-only callbacks (func(T&...)), use Each or ParallelBatchEach.
    static_assert(std::is_invocable_v<F, EntityHandle, Detail::PlainRefArg<Ts>...>,
                  "Parallel callback must accept (EntityHandle, ...). "
                  "For component-only callbacks, use Each() or ParallelBatchEach().");

    using Probe = Detail::AccessProbe<F, Detail::CallShape::EntityRef, Ts...>;
    // maybe_unused: kReadMask is consumed only as a template argument inside
    // the per-batch generic lambda below; MSVC's C4189 does not count that
    // dependent-context use as a reference.
    [[maybe_unused]] constexpr uint32_t kReadMask = Probe::kReadMask;
    constexpr uint32_t kWriteMask = Probe::kWriteMask;

    const uint64_t stampVersion = (kWriteMask != 0u) ? world->NextGlobalSystemVersion() : 0u;
    const uint32_t disabledOptionalMask = DisabledOptionalMask();

    // Adaptive minBatchSize based on job-system queue pressure
    const QueryPolicy& policy = world->GetQueryPolicy();
    if (minBatchSize == 0)
        minBatchSize = policy.MinBatchSizeDefault;
    auto js = world->jobSystem;
    if (policy.EnableBackpressure && js)
    {
        const size_t qsize = js->GetApproximateQueueSize();
        const size_t pending = js->GetPendingTasksApprox();
        const size_t workers = std::max<size_t>(1, js->GetWorkerCount());
        if (qsize > workers * policy.QueuePressureFactor || pending > workers * policy.PendingPressureFactor)
        {
            minBatchSize = std::max(minBatchSize, policy.PressuredMinBatchFloor);
        }
    }

    JobSystem::JobCounter counter;
    auto fnShared = std::make_shared<std::decay_t<Func>>(std::forward<Func>(func));

    const bool kHasGate = m_HasChangedGate;
    const uint64_t kGateVersion = m_ChangedGate;

    for (auto* archetype : m_CachedArchetypes)
    {
        if (archetype->GetEntityCount() == 0 || !archetype->HasTable())
            continue;

        auto& table = archetype->GetTable();
        const auto& layout = table.GetLayout();

        std::array<int, sizeof...(Ts)> colIdx = {
            layout.FindColumnIndex(GetComponentTypeId<U<Ts>>())...};
        ApplyDisabledOptionalRule(layout, colIdx, disabledOptionalMask);

        {
            int ci = 0;
            bool valid = true;
            ((IsOptional<Ts>::value ? (++ci, void()) : void(valid = valid && colIdx[ci++] >= 0)), ...);
            if (!valid)
                continue;
        }

        // Cache byte offsets per-archetype.
        std::array<uint32_t, sizeof...(Ts)> colOffsets;
        for (std::size_t k = 0; k < sizeof...(Ts); ++k)
            colOffsets[k] = (colIdx[k] >= 0) ? layout.Columns[colIdx[k]].Offset : 0;
        const uint32_t ehOffset = layout.EntityHandleOffset;

        std::array<int, kMaxChangedTypes> changedCols{};
        if (kHasGate)
            ResolveChangedColumns(colIdx, changedCols);
        const uint32_t changedColCount = m_ChangedSlotCount;

        auto chunks = table.GetChunks();
        const std::size_t numChunks = chunks.size();
        ArchetypeTable* tablePtr = &table;

        // Batch multiple chunks per task to amortize job submission overhead.
        // Accumulate chunks until we reach minBatchSize entities, then submit.
        std::size_t batchStart = 0;
        std::size_t batchEntities = 0;

        for (std::size_t chunkIdx = 0; chunkIdx <= numChunks; ++chunkIdx)
        {
            if (chunkIdx < numChunks)
            {
                uint32_t count = chunks[chunkIdx].GetCount();
                if (count == 0)
                    continue;
                if (kHasGate && !table.AnyColumnChangedSince(
                                    chunkIdx,
                                    std::span<const int>(changedCols.data(), changedColCount),
                                    kGateVersion))
                    continue;
                batchEntities += count;
            }

            // Flush the batch when we hit the target size or reach the end.
            bool flush = (chunkIdx == numChunks && batchEntities > 0) ||
                         (batchEntities >= minBatchSize);
            if (!flush)
                continue;

            // At sentinel (chunkIdx == numChunks), don't add 1 — there's no chunk there.
            const std::size_t batchEnd = (chunkIdx < numChunks) ? chunkIdx + 1 : numChunks;

            auto task = [chunks, batchStart, batchEnd, colIdx, colOffsets, ehOffset, fnShared,
                         tablePtr, kHasGate, kGateVersion, changedCols, changedColCount,
                         stampVersion]()
            {
                for (std::size_t ci = batchStart; ci < batchEnd; ++ci)
                {
                    auto& chunk = chunks[ci];
                    uint32_t chunkCount = chunk.GetCount();
                    if (chunkCount == 0)
                        continue;
                    if (kHasGate && !tablePtr->AnyColumnChangedSince(
                                        ci,
                                        std::span<const int>(changedCols.data(), changedColCount),
                                        kGateVersion))
                        continue;
                    if constexpr (kWriteMask != 0u)
                        StampWriteColumns(*tablePtr, ci, colIdx, kWriteMask, stampVersion);

                    const EntityHandle* entityPtr = chunk.GetEntityHandlesByOffset(ehOffset);

                    [&]<std::size_t... Is>(std::index_sequence<Is...>)
                    {
                        auto columnPtrs = std::make_tuple(
                            (colIdx[Is] >= 0
                                 ? chunk.template GetColumnByOffset<U<NthType<Is>>>(colOffsets[Is])
                                 : static_cast<U<NthType<Is>>*>(nullptr))...);

                        const auto argAt = [&]<std::size_t J>(
                                               std::integral_constant<std::size_t, J>,
                                               uint32_t i) -> decltype(auto)
                        {
                            using Q = NthType<J>;
                            constexpr bool kIsRead = ((kReadMask >> J) & 1u) != 0u;
                            auto* ptr = std::get<J>(columnPtrs);
                            if constexpr (IsOptional<Q>::value)
                            {
                                if constexpr (kIsRead)
                                    return static_cast<const U<Q>*>(ptr ? &ptr[i] : nullptr);
                                else
                                    return ptr ? &ptr[i] : nullptr;
                            }
                            else
                            {
                                if constexpr (kIsRead)
                                    return static_cast<const U<Q>&>(ptr[i]);
                                else
                                    return static_cast<U<Q>&>(ptr[i]);
                            }
                        };

                        for (uint32_t i = 0; i < chunkCount; ++i)
                            (*fnShared)(entityPtr[i],
                                        argAt(std::integral_constant<std::size_t, Is>{}, i)...);
                    }(std::make_index_sequence<sizeof...(Ts)>{});
                }
            };

            world->jobSystem->Run(std::move(task), counter);
            batchStart = chunkIdx + 1;
            batchEntities = 0;
        }
    }

    world->jobSystem->Wait(counter);
}

// Direct chunk-based iteration
template <QualifiedComponent... Ts>
template <typename Func>
void Query<Ts...>::ForEachChunk(Func&& func)
{
    UpdateCache();

    using F = std::decay_t<Func>;
    constexpr bool kWithCount = std::is_invocable_v<F, Detail::PlainPtrArg<Ts>..., std::size_t>;
    constexpr bool kNoCount = std::is_invocable_v<F, Detail::PlainPtrArg<Ts>...>;
    static_assert(kWithCount || kNoCount,
                  "Unsupported functor signature for chunk iteration; expected (T*..., size_t) or (T*...)");

    using Probe = Detail::AccessProbe<
        F, kWithCount ? Detail::CallShape::PtrCount : Detail::CallShape::Ptr, Ts...>;
    constexpr uint32_t kReadMask = Probe::kReadMask;
    constexpr uint32_t kWriteMask = Probe::kWriteMask;

    const uint64_t stampVersion = (kWriteMask != 0u) ? world->NextGlobalSystemVersion() : 0u;
    const uint32_t disabledOptionalMask = DisabledOptionalMask();

    auto adapter = Detail::ChunkCallAdapter<std::decay_t<Func>>{std::forward<Func>(func)};

    for (auto* archetype : m_CachedArchetypes)
    {
        if (archetype->GetEntityCount() == 0 || !archetype->HasTable())
            continue;

        auto& table = archetype->GetTable();
        const auto& layout = table.GetLayout();

        std::array<int, sizeof...(Ts)> colIdx = {
            layout.FindColumnIndex(GetComponentTypeId<U<Ts>>())...};
        ApplyDisabledOptionalRule(layout, colIdx, disabledOptionalMask);

        {
            int ci = 0;
            bool valid = true;
            ((IsOptional<Ts>::value ? (++ci, void()) : void(valid = valid && colIdx[ci++] >= 0)), ...);
            if (!valid)
                continue;
        }

        // Cache byte offsets per-archetype.
        std::array<uint32_t, sizeof...(Ts)> colOffsets;
        for (std::size_t k = 0; k < sizeof...(Ts); ++k)
            colOffsets[k] = (colIdx[k] >= 0) ? layout.Columns[colIdx[k]].Offset : 0;

        std::array<int, kMaxChangedTypes> changedCols{};
        if (m_HasChangedGate)
            ResolveChangedColumns(colIdx, changedCols);

        auto chunks = table.GetChunks();
        for (std::size_t chunkIdx = 0; chunkIdx < chunks.size(); ++chunkIdx)
        {
            auto& chunk = chunks[chunkIdx];
            const uint32_t count = chunk.GetCount();
            if (count == 0)
                continue;
            if (m_HasChangedGate && !ChunkPassesGate(table, chunkIdx, changedCols))
                continue;
            if constexpr (kWriteMask != 0u)
                StampWriteColumns(table, chunkIdx, colIdx, kWriteMask, stampVersion);

            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
                auto chunkPtrs = std::make_tuple(
                    (colIdx[Is] >= 0
                         ? chunk.template GetColumnByOffset<U<NthType<Is>>>(colOffsets[Is])
                         : static_cast<U<NthType<Is>>*>(nullptr))...);

                adapter.template operator()<
                    Detail::EffPtrArg<((kReadMask >> Is) & 1u) != 0u, NthType<Is>>...>(
                    Detail::EffPtrCast<((kReadMask >> Is) & 1u) != 0u>(std::get<Is>(chunkPtrs))...,
                    count);
            }(std::make_index_sequence<sizeof...(Ts)>{});
        }
    }
}

// Parallel chunk iteration
template <QualifiedComponent... Ts>
template <typename Func>
void Query<Ts...>::ParallelChunks(Func&& func)
{
    UpdateCache();

    if (!world->jobSystem)
    {
        ForEachChunk(std::forward<Func>(func));
        return;
    }

    using F = std::decay_t<Func>;
    constexpr bool kWithCount = std::is_invocable_v<F, Detail::PlainPtrArg<Ts>..., std::size_t>;
    constexpr bool kNoCount = std::is_invocable_v<F, Detail::PlainPtrArg<Ts>...>;
    static_assert(kWithCount || kNoCount,
                  "Unsupported functor signature for chunk iteration; expected (T*..., size_t) or (T*...)");

    using Probe = Detail::AccessProbe<
        F, kWithCount ? Detail::CallShape::PtrCount : Detail::CallShape::Ptr, Ts...>;
    constexpr uint32_t kReadMask = Probe::kReadMask;
    constexpr uint32_t kWriteMask = Probe::kWriteMask;

    const uint64_t stampVersion = (kWriteMask != 0u) ? world->NextGlobalSystemVersion() : 0u;
    const uint32_t disabledOptionalMask = DisabledOptionalMask();

    JobSystem::JobCounter counter;

    // Share the functor ONCE across all tasks and all archetypes.
    // Callers must ensure the functor is thread-safe (no mutable shared state).
    auto fnShared = std::make_shared<std::decay_t<Func>>(std::forward<Func>(func));

    const bool kHasGate = m_HasChangedGate;
    const uint64_t kGateVersion = m_ChangedGate;

    for (auto* archetype : m_CachedArchetypes)
    {
        if (archetype->GetEntityCount() == 0 || !archetype->HasTable())
            continue;

        auto& table = archetype->GetTable();
        const auto& layout = table.GetLayout();

        std::array<int, sizeof...(Ts)> colIdx = {
            layout.FindColumnIndex(GetComponentTypeId<U<Ts>>())...};
        ApplyDisabledOptionalRule(layout, colIdx, disabledOptionalMask);

        {
            int ci = 0;
            bool valid = true;
            ((IsOptional<Ts>::value ? (++ci, void()) : void(valid = valid && colIdx[ci++] >= 0)), ...);
            if (!valid)
                continue;
        }

        std::array<int, kMaxChangedTypes> changedCols{};
        if (kHasGate)
            ResolveChangedColumns(colIdx, changedCols);
        const uint32_t changedColCount = m_ChangedSlotCount;

        auto chunks = table.GetChunks();
        ArchetypeTable* tablePtr = &table;

        for (std::size_t chunkIdx = 0; chunkIdx < chunks.size(); ++chunkIdx)
        {
            if (kHasGate && !table.AnyColumnChangedSince(
                                chunkIdx,
                                std::span<const int>(changedCols.data(), changedColCount),
                                kGateVersion))
                continue;

            auto task = [chunks, chunkIdx, colIdx, fnShared, tablePtr, stampVersion]()
            {
                auto& chunk = chunks[chunkIdx];
                uint32_t count = chunk.GetCount();
                if (count == 0)
                    return;

                if constexpr (kWriteMask != 0u)
                    StampWriteColumns(*tablePtr, chunkIdx, colIdx, kWriteMask, stampVersion);

                auto adapter = Detail::ChunkCallAdapter<std::decay_t<Func>>{*fnShared};

                [&]<std::size_t... Is>(std::index_sequence<Is...>)
                {
                    auto chunkPtrs = std::make_tuple(
                        [&]() -> U<NthType<Is>>*
                        {
                            int ci = colIdx[Is];
                            if (ci < 0) return nullptr;
                            return chunk.template GetColumn<U<NthType<Is>>>(ci);
                        }()...);

                    // Use positional std::get<Is> (not type-based) to handle duplicate types.
                    adapter.template operator()<
                        Detail::EffPtrArg<((kReadMask >> Is) & 1u) != 0u, NthType<Is>>...>(
                        Detail::EffPtrCast<((kReadMask >> Is) & 1u) != 0u>(
                            std::get<Is>(chunkPtrs))...,
                        count);
                }(std::make_index_sequence<sizeof...(Ts)>{});
            };

            world->jobSystem->Run(std::move(task), counter);
        }
    }

    world->jobSystem->Wait(counter);
}

template <QualifiedComponent... Ts>
std::size_t Query<Ts...>::Count() const
{
    UpdateCache();

    std::size_t total = 0;
    for (auto* archetype : m_CachedArchetypes)
    {
        total += archetype->GetEntityCount();
    }
    return total;
}

// Adaptive parallelization with default thresholds
template <QualifiedComponent... Ts>
template <typename Func>
void Query<Ts...>::Adaptive(Func&& func)
{
    Adaptive(std::forward<Func>(func), 1000, 10000);
}

// Adaptive parallelization with custom thresholds. Forwards the Changed<>
// gate to whichever mode it picks — the gate lives on this Query object, so
// no dispatch path can silently drop the filter (design M14, pinned by
// AdaptiveForwardsGate).
template <QualifiedComponent... Ts>
template <typename Func>
void Query<Ts...>::Adaptive(Func&& func, std::size_t sequentialThreshold, std::size_t chunkThreshold)
{
    std::size_t entityCount = Count();

    if (entityCount < sequentialThreshold)
    {
        Each(std::forward<Func>(func));
    }
    else if (entityCount < chunkThreshold)
    {
        std::size_t batchSize = std::max(std::size_t(100), entityCount / 8);
        Parallel(std::forward<Func>(func), batchSize);
    }
    else
    {
        if constexpr (std::is_invocable_v<std::decay_t<Func>, Detail::PlainPtrArg<Ts>..., std::size_t>)
        {
            ParallelChunks(std::forward<Func>(func));
        }
        else
        {
            std::size_t batchSize = std::max(std::size_t(500), entityCount / 16);
            Parallel(std::forward<Func>(func), batchSize);
        }
    }
}

} // namespace ECS
} // namespace GameEngine
