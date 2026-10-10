#pragma once

#include "ECS/ECS.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine::ECS
{

class World;
class Archetype;

// Persistent query that caches matching archetypes across frames.
// Re-evaluates the archetype list only when the world's structural version changes.
// Not thread-safe: intended for single-threaded iteration (e.g. scripting ABI tick).
class CachedQuery
{
  public:
    CachedQuery(World* world,
                std::vector<ComponentTypeId> required,
                std::vector<ComponentTypeId> excluded);

    // Re-scan archetypes if the world has undergone structural changes since last refresh.
    void Refresh();

    // ------------------------------------------------------------------
    // Changed-filter parity with Query::Changed<> (change-signaling design
    // M11): the type-erased query grows the same gate so managed-facing
    // iteration is not a divergent second query surface. Set the filtered
    // types once; Refresh() resolves their column indices per cached
    // archetype. Multiple types = OR semantics. The managed-side filter
    // LOOP is the committed follow-up — this is the native capability plus
    // the ABI's chunkVersion plumbing.
    // ------------------------------------------------------------------

    void SetChangedFilter(std::vector<ComponentTypeId> types);

    // True iff any filtered column of the chunk was write-granted after
    // `gate` (World::GetGlobalSystemVersion entry samples — see ChangeGate).
    // With no filter set, every chunk passes.
    bool ChunkChangedSince(std::size_t archetypeIndex, std::size_t chunkIndex,
                           uint64_t gate) const;

    std::size_t GetArchetypeCount() const { return m_CachedArchetypes.size(); }
    Archetype* GetArchetype(std::size_t index) const;

    World* GetWorld() const { return m_World; }
    const std::vector<ComponentTypeId>& GetRequiredTypeIds() const { return m_RequiredTypeIds; }

    // True if the archetype at the given index carries the component and it is
    // not switched off there: the managed Optional<T> reads a disabled T as
    // absent, as the native Optional does.
    bool ArchetypeHasComponent(std::size_t archetypeIndex, ComponentTypeId typeId) const;

    // Populate a scratch buffer with entity IDs (as uint32_t) for a given archetype
    // and chunk range. Returns pointer into the internal buffer and the count.
    std::pair<uint32_t*, std::size_t> GetChunkEntityIds(std::size_t archetypeIndex,
                                                        std::size_t chunkIndex);

    // Entity-offset-based entity ID access. Returns a slice of entity IDs starting
    // at entityOffset, up to maxCount. Uses the internal scratch buffer.
    std::pair<uint32_t*, std::size_t> GetEntityIdSlice(std::size_t archetypeIndex,
                                                       std::size_t entityOffset,
                                                       std::size_t maxCount);

  private:
    World* m_World = nullptr;
    std::vector<ComponentTypeId> m_RequiredTypeIds;
    DenseSignature m_Required;
    DenseSignature m_Excluded;
    bool m_HasExcluded = false;
    // The enable-state tags that take an archetype out of this query, the
    // native Query's rule by type id: both entity tags (unless the query
    // requires one of them) plus each required type's ComponentDisabled tag,
    // minus the required types themselves.
    DenseSignature m_DisabledExclude;
    std::vector<Archetype*> m_CachedArchetypes;
    std::size_t m_CachedVersion = 0;
    bool m_HasRefreshed = false;
    // Scratch buffer for entity IDs returned by GetChunkEntityIds.
    // Reused across calls to avoid per-call allocation.
    std::vector<uint32_t> m_EntityIdScratch;

    // Changed-filter state (M11): filtered type ids + per-archetype resolved
    // column indices, [archetypeIndex * m_ChangedTypes.size() + k] -> column
    // index or -1 (missing column = never changed for that archetype).
    std::vector<ComponentTypeId> m_ChangedTypes;
    std::vector<int> m_ChangedColumns;

    void ResolveChangedColumns();
};

} // namespace GameEngine::ECS
