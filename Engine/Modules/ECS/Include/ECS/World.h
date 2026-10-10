#pragma once

#include "ECS/ArchetypeTable.h"
#include "ECS/ComponentConcepts.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/QueryPolicy.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"

#include <optional>

namespace GameEngine
{
namespace ECS
{

// Forward declarations for the Archetype stamping-facade friends.
class World;
class CachedQuery;
class BlobComponentHandler;
class ComponentHandler;
template <QualifiedComponent... Ts>
class Query;

// Archetype: owns a colocated ArchetypeTable where all components are stored
// together in 16KB SoA chunks. Entity locations are tracked as {chunkIndex,
// indexInChunk} in World::entityMetadata.
class Archetype
{
  private:
    ComponentSignature signature;  // Sorted-vector signature (archetype identity)
    mutable std::shared_mutex mutex; // Per-archetype RW lock
    World* world = nullptr;
    bool m_HasRemoveHooks = false; // Set at creation if World has any matching OnRemove hooks

    // Colocated chunk storage — sole storage for all component data.
    // All components for this archetype stored together in 16KB SoA chunks.
    std::optional<ArchetypeTable> m_Table;

    // Archetype graph edges: cached transitions for add/remove component operations.
    std::unordered_map<ComponentTypeId, Archetype*> m_AddEdges;
    std::unordered_map<ComponentTypeId, Archetype*> m_RemoveEdges;

    // The two entity-activity tags move together — SetEnabled(false) adds
    // Disabled and DisabledInHierarchy, SetEnabled(true) removes both unless
    // the parent is still off — so the transition gets its own cached edge and
    // costs ONE archetype move rather than two. Each is set only between
    // archetypes that differ by exactly those two tags.
    Archetype* m_DisableEdge = nullptr;
    Archetype* m_EnableEdge = nullptr;

    // Pre-baked copy plans for outbound MoveEntity transitions, keyed on
    // target archetype. Populated lazily on first transition through a given
    // edge. Each CopyStep describes a single memcpy between shared columns.
    // Only mutated under worldMutex exclusive (same as MoveEntity itself).
    struct CopyStep {
        uint32_t SrcOffset; // byte offset of the source column within a chunk
        uint32_t DstOffset; // byte offset of the destination column within a chunk
        uint32_t Size;      // bytes per element
    };
    std::unordered_map<Archetype*, std::vector<CopyStep>> m_MovePlans;

  public:
    explicit Archetype(const ComponentSignature& sig, World* w);

    // Add entity to the colocated table. Returns its location {chunkIndex, indexInChunk}.
    // Component data at the returned location is uninitialized — caller must write it.
    // Takes no archetype lock: every structural caller already holds worldMutex
    // exclusively (the locked variants were deleted after the last caller outside
    // worldMutex disappeared — the redundant lock measured 15-20% of
    // archetype-transition cost when MoveEntity dropped it).
    EntityLocation AddEntityUnsafe(EntityHandle entity);

    // Remove entity at the given location. Returns the entity that was swapped
    // into the vacated slot (for metadata fixup), or Invalid if none was moved.
    // Same locking contract as AddEntityUnsafe: caller must hold worldMutex.
    EntityHandle RemoveEntityUnsafe(EntityLocation loc);

    // Initialize the colocated ArchetypeTable from registered component sizes.
    void InitializeTable(const std::unordered_map<ComponentTypeId, std::size_t>& componentSizes);

    const ComponentSignature& GetSignature() const { return signature; }
    std::size_t GetEntityCount() const { return m_Table ? m_Table->GetEntityCount() : 0; }
    bool HasRemoveHooks() const { return m_HasRemoveHooks; }
    void RefreshRemoveHooks();

    // Colocated table access — always available after InitializeArrays.
    // Read-only: the mutable table (and with it every raw mutable column
    // surface) is private to the stamping facade (design §2.3/C2) — a write
    // path that bypasses stamping must not compile.
    bool HasTable() const { return m_Table.has_value(); }
    const ArchetypeTable& GetTable() const { return *m_Table; }

    // Typed read-only component access via table columns.
    template <Component T>
    const T* GetComponentAt(uint16_t chunkIdx, uint16_t idxInChunk) const
    {
        if (!m_Table) return nullptr;
        int colIdx = m_Table->FindColumnIndex(GetComponentTypeId<T>());
        if (colIdx < 0) return nullptr;
        auto chunks = m_Table->GetChunks();
        if (chunkIdx >= chunks.size()) return nullptr;
        assert(idxInChunk < chunks[chunkIdx].GetCount() && "idxInChunk out of range");
        return &chunks[chunkIdx].template GetColumn<T>(colIdx)[idxInChunk];
    }

    // Type-erased read-only component access (hooks, serialization).
    const void* GetComponentRawAt(uint16_t chunkIdx, uint16_t idxInChunk, ComponentTypeId typeId) const
    {
        if (!m_Table) return nullptr;
        int colIdx = m_Table->FindColumnIndex(typeId);
        if (colIdx < 0) return nullptr;
        auto chunks = m_Table->GetChunks();
        if (chunkIdx >= chunks.size()) return nullptr;
        return chunks[chunkIdx].GetComponentRaw(colIdx, idxInChunk);
    }

    // ------------------------------------------------------------------
    // Change-signaling write-grant stamps (ECS/ChangeFilter.h). The version
    // comes from the owning World's counter (World::NextGlobalSystemVersion)
    // — World is incomplete here, so callers pass it in.
    // ------------------------------------------------------------------

    // Stamp one component column of one chunk (data-only writes).
    void StampColumnVersion(uint32_t chunkIdx, ComponentTypeId typeId, uint64_t version)
    {
        if (!m_Table) return;
        const int colIdx = m_Table->FindColumnIndex(typeId);
        if (colIdx < 0 || chunkIdx >= m_Table->GetChunkCount()) return;
        m_Table->StampColumnVersion(chunkIdx, static_cast<std::size_t>(colIdx), version);
    }

    // Stamp every column of one chunk (structural writes: Append rewrote a
    // fresh row, SwapRemove rewrote the vacated slot).
    void StampAllColumnVersions(uint32_t chunkIdx, uint64_t version)
    {
        if (!m_Table || chunkIdx >= m_Table->GetChunkCount()) return;
        m_Table->StampAllColumnVersions(chunkIdx, version);
    }

    // Memory pre-allocation for batch operations.
    void Reserve(size_t count);

    // Compact the table by freeing empty chunks. Updates entity metadata for relocated chunks.
    // Returns the number of chunks freed. Caller must hold worldMutex.
    uint32_t CompactChunks();

    // Compatibility: collect entity handles from all table chunks into a vector.
    // Callers that iterated Archetype::GetEntities() can use this instead.
    std::vector<EntityHandle> CollectEntities() const;

    // Compatibility: chunk count (all columns share the same chunk structure).
    std::size_t GetChunkCount() const { return m_Table ? m_Table->GetChunkCount() : 0; }

    // Compatibility: get raw column data for a component type at a specific chunk index.
    // Returns {pointer, entityCount} for a given component column in a specific chunk.
    // The mutable overload is a write grant and stamps the column's change
    // version; the const overload never stamps (read-only ABI span path).
    std::pair<void*, std::size_t> GetChunkDataRaw(ComponentTypeId typeId, std::size_t chunkIndex);
    std::pair<const void*, std::size_t> GetChunkDataRaw(ComponentTypeId typeId, std::size_t chunkIndex) const;

    // Change-signaling: the write-grant version of one component column in
    // one chunk (0 = never written / missing column). Exposed for the ABI's
    // chunkVersion parameter (design M11).
    uint64_t GetColumnVersion(ComponentTypeId typeId, std::size_t chunkIndex) const
    {
        if (!m_Table || chunkIndex >= m_Table->GetChunkCount())
            return 0;
        const int colIdx = m_Table->FindColumnIndex(typeId);
        if (colIdx < 0)
            return 0;
        return m_Table->GetColumnVersion(chunkIndex, static_cast<std::size_t>(colIdx));
    }

    // Archetype graph edge lookups (used by World for fast add/remove transitions)
    Archetype* GetAddEdge(ComponentTypeId typeId) const
    {
        auto it = m_AddEdges.find(typeId);
        return (it != m_AddEdges.end()) ? it->second : nullptr;
    }
    Archetype* GetRemoveEdge(ComponentTypeId typeId) const
    {
        auto it = m_RemoveEdges.find(typeId);
        return (it != m_RemoveEdges.end()) ? it->second : nullptr;
    }
    void SetAddEdge(ComponentTypeId typeId, Archetype* target) { m_AddEdges[typeId] = target; }
    void SetRemoveEdge(ComponentTypeId typeId, Archetype* target) { m_RemoveEdges[typeId] = target; }

    Archetype* GetDisableEdge() const { return m_DisableEdge; }
    Archetype* GetEnableEdge() const { return m_EnableEdge; }
    void SetDisableEdge(Archetype* target) { m_DisableEdge = target; }
    void SetEnableEdge(Archetype* target) { m_EnableEdge = target; }

    // Look up or build the outbound move-plan for a target archetype. Caller
    // must hold worldMutex exclusively — mutates m_MovePlans on first use.
    const std::vector<CopyStep>& GetOrBakeMovePlan(Archetype* target);

    // Remove any graph edge that points to an archetype in the given set.
    // Called after PruneEmptyArchetypes to avoid dangling edge pointers.
    void EraseEdgesTo(const std::unordered_set<Archetype*>& pruned)
    {
        for (auto it = m_AddEdges.begin(); it != m_AddEdges.end();)
            it = pruned.count(it->second) ? m_AddEdges.erase(it) : std::next(it);
        for (auto it = m_RemoveEdges.begin(); it != m_RemoveEdges.end();)
            it = pruned.count(it->second) ? m_RemoveEdges.erase(it) : std::next(it);
        if (m_DisableEdge && pruned.count(m_DisableEdge))
            m_DisableEdge = nullptr;
        if (m_EnableEdge && pruned.count(m_EnableEdge))
            m_EnableEdge = nullptr;
        // Move plans are keyed on target archetype pointers — drop any plan
        // whose target was pruned to avoid dangling keys.
        for (auto it = m_MovePlans.begin(); it != m_MovePlans.end();)
            it = pruned.count(it->first) ? m_MovePlans.erase(it) : std::next(it);
    }

  private:
    // ------------------------------------------------------------------
    // Stamping facade (design §2.3/C2): raw MUTABLE column access is private.
    // Friends are audited per class: Query stamps at visit; World's unified
    // bodies and the structural paths stamp via AddEntityUnsafe/RemoveEntityUnsafe
    // or GetChunkDataRaw's own grant; CachedQuery's access is read-only; the
    // component-handler friends rely on their CALLERS' stamp sites (e.g.
    // World::CopyComponent) — a new handler call path must bring its own stamp.
    // A new write path outside these classes cannot compile.
    // ------------------------------------------------------------------

    ArchetypeTable& GetTable() { return *m_Table; }

    // Typed mutable component access. Callers stamp (World::GetComponentLookup,
    // the unified add-or-set body).
    template <Component T>
    T* GetComponentAt(uint16_t chunkIdx, uint16_t idxInChunk)
    {
        if (!m_Table) return nullptr;
        int colIdx = m_Table->FindColumnIndex(GetComponentTypeId<T>());
        if (colIdx < 0) return nullptr;
        auto chunks = m_Table->GetChunks();
        if (chunkIdx >= chunks.size()) return nullptr;
        assert(idxInChunk < chunks[chunkIdx].GetCount() && "idxInChunk out of range");
        return &chunks[chunkIdx].template GetColumn<T>(colIdx)[idxInChunk];
    }

    // Write typed component data at the given location. Callers stamp.
    template <Component T>
    void SetComponentAt(uint16_t chunkIdx, uint16_t idxInChunk, const T& component)
    {
        if (!m_Table) return;
        int colIdx = m_Table->FindColumnIndex(GetComponentTypeId<T>());
        if (colIdx < 0) return;
        auto chunks = m_Table->GetChunks();
        if (chunkIdx >= chunks.size()) return;
        chunks[chunkIdx].template GetColumn<T>(colIdx)[idxInChunk] = component;
    }

    // Type-erased mutable component access (hooks, serialization). Callers stamp.
    void* GetComponentRawAt(uint16_t chunkIdx, uint16_t idxInChunk, ComponentTypeId typeId)
    {
        if (!m_Table) return nullptr;
        int colIdx = m_Table->FindColumnIndex(typeId);
        if (colIdx < 0) return nullptr;
        auto chunks = m_Table->GetChunks();
        if (chunkIdx >= chunks.size()) return nullptr;
        return chunks[chunkIdx].GetComponentRaw(colIdx, idxInChunk);
    }

    // Write type-erased component data at a given location. Callers stamp.
    void SetComponentRawAt(uint16_t chunkIdx, uint16_t idxInChunk, ComponentTypeId typeId,
                           const void* data, std::size_t dataSize)
    {
        if (!m_Table) return;
        int colIdx = m_Table->FindColumnIndex(typeId);
        if (colIdx < 0) return;
        auto chunks = m_Table->GetChunks();
        if (chunkIdx >= chunks.size()) return;
        void* dst = chunks[chunkIdx].GetComponentRaw(colIdx, idxInChunk);
        std::memcpy(dst, data, dataSize);
    }

    friend class World;
    friend class CachedQuery;
    friend class BlobComponentHandler;
    friend class ComponentHandler;
    template <QualifiedComponent... Ts>
    friend class Query;
};

// World class definition is in Entity.h to avoid circular dependencies

} // namespace ECS
} // namespace GameEngine
