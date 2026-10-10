#pragma once

#include "ECS/ECS.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#ifdef _MSC_VER
#include <malloc.h> // _aligned_malloc / _aligned_free
#else
#include <cstdlib> // aligned_alloc / free
#endif

namespace GameEngine::ECS
{

// ============================================================================
// Column layout — computed once per archetype signature, shared by all chunks.
// ============================================================================

inline constexpr std::size_t kArchetypeChunkBytes = 16 * 1024; // Target size; oversized rows get a larger chunk.
inline constexpr std::size_t kColumnAlignment = 16;            // SSE-friendly

struct ColumnDesc
{
    ComponentTypeId TypeId = 0;
    uint32_t Offset = 0; // Byte offset from chunk memory start
    uint32_t Size = 0;   // sizeof(T) for this component
    uint32_t Align = 0;  // alignof(T)
};

struct ColumnLayout
{
    std::vector<ColumnDesc> Columns; // One per component type, sorted by TypeId
    uint32_t EntityHandleOffset = 0; // Byte offset of EntityHandle array (always 0)
    uint32_t Capacity = 0;           // Max entities per chunk
    uint32_t TotalBytesUsed = 0;     // Actual bytes consumed per chunk with Capacity entities
    std::size_t AllocationBytes = kArchetypeChunkBytes; // Includes trailing allocator alignment padding
    std::size_t AllocationAlignment = kCacheLineSize;

    // Lookup column index by type ID. Returns -1 if not found.
    // Called rarely (query cache build); hot path uses cached column indices.
    int FindColumnIndex(ComponentTypeId typeId) const
    {
        for (int i = 0; i < static_cast<int>(Columns.size()); ++i)
        {
            if (Columns[i].TypeId == typeId)
                return i;
        }
        return -1;
    }
};

// Metadata needed to build a column layout.
struct ComponentMeta
{
    ComponentTypeId TypeId;
    uint32_t Size;
    uint32_t Align;
};

// Round up to next power of two (for alignment values).
inline constexpr uint32_t RoundUpPow2(uint32_t v)
{
    if (v == 0) return 1;
    if (v > (uint32_t{1} << 31))
        throw std::length_error("Component alignment exceeds the column layout range");
    return std::bit_ceil(v);
}

// Compute column offsets for a given capacity. Returns total bytes used.
inline std::size_t ComputeColumnOffsets(ColumnLayout& layout, uint32_t capacity,
                                        std::span<const ComponentMeta> componentMetas)
{
    layout.Capacity = capacity;
    // Widen before arithmetic: offsets and strides are uint32_t, including on
    // 32-bit targets. Reject unrepresentable layouts instead of truncating them.
    uint64_t cursor = 0;
    layout.AllocationAlignment = std::max(kCacheLineSize, alignof(EntityHandle));

    // EntityHandle column (always first).
    constexpr std::size_t ehAlign = std::max(alignof(EntityHandle), kColumnAlignment);
    static_assert((ehAlign & (ehAlign - 1)) == 0, "EntityHandle alignment must be power of two");
    cursor = (cursor + ehAlign - 1) & ~(ehAlign - 1);
    layout.EntityHandleOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t{sizeof(EntityHandle)} * capacity;
    if (cursor > std::numeric_limits<uint32_t>::max())
        throw std::length_error("Entity handle column exceeds the column layout range");

    // Component columns.
    layout.Columns.clear();
    layout.Columns.reserve(componentMetas.size());
    for (const auto& meta : componentMetas)
    {
        // Alignment must be power-of-two for bitmask rounding to work.
        const uint64_t colAlign = std::max(static_cast<std::size_t>(RoundUpPow2(meta.Align)), kColumnAlignment);
        cursor = (cursor + colAlign - 1) & ~(colAlign - 1);
        const uint64_t columnEnd = cursor + uint64_t{meta.Size} * capacity;
        if (columnEnd > std::numeric_limits<uint32_t>::max())
            throw std::length_error("Archetype chunk exceeds the column layout range");
        layout.AllocationAlignment = std::max(layout.AllocationAlignment, static_cast<std::size_t>(colAlign));

        ColumnDesc desc;
        desc.TypeId = meta.TypeId;
        desc.Offset = static_cast<uint32_t>(cursor);
        desc.Size = meta.Size;
        desc.Align = meta.Align;
        layout.Columns.push_back(desc);

        cursor = columnEnd;
    }

    const uint64_t alignment = layout.AllocationAlignment;
    const uint64_t allocationBytes = (std::max(cursor, uint64_t{kArchetypeChunkBytes}) + alignment - 1)
                                   & ~(alignment - 1);
    if (allocationBytes > std::numeric_limits<std::size_t>::max())
        throw std::length_error("Archetype chunk exceeds the allocation size range");
    layout.TotalBytesUsed = static_cast<uint32_t>(cursor);
    layout.AllocationBytes = static_cast<std::size_t>(allocationBytes);
    return static_cast<std::size_t>(cursor);
}

// Build a ColumnLayout for a given set of component types.
// componentMetas must be sorted by TypeId (ascending).
inline ColumnLayout BuildColumnLayout(std::span<const ComponentMeta> componentMetas)
{
    ColumnLayout layout;

    // Per-entity cost: EntityHandle + sum of component sizes.
    uint64_t perEntityBytes = sizeof(EntityHandle);
    for (const auto& meta : componentMetas)
    {
        perEntityBytes += meta.Size;
        if (perEntityBytes > std::numeric_limits<uint32_t>::max())
            throw std::length_error("Archetype row exceeds the column layout range");
    }

    // Conservative capacity estimate: reserve alignment padding overhead.
    std::size_t maxPaddingOverhead =
        (std::min(componentMetas.size(), kArchetypeChunkBytes / kColumnAlignment) + 1) * kColumnAlignment;
    std::size_t usableBytes =
        kArchetypeChunkBytes > maxPaddingOverhead ? kArchetypeChunkBytes - maxPaddingOverhead : 0;
    uint32_t capacity =
        (usableBytes > 0 && perEntityBytes > 0) ? static_cast<uint32_t>(usableBytes / perEntityBytes) : 0;

    // At least 1 entity per chunk for very large component sets.
    if (capacity == 0 && perEntityBytes > 0)
        capacity = 1;

    // Compute offsets, reducing capacity if alignment padding overflows.
    std::size_t totalBytes = ComputeColumnOffsets(layout, capacity, componentMetas);
    while (totalBytes > kArchetypeChunkBytes && layout.Capacity > 1)
    {
        totalBytes = ComputeColumnOffsets(layout, layout.Capacity - 1, componentMetas);
    }

    return layout;
}

// Build the sorted ComponentMeta list for a set of component type ids from a type-id -> size
// map, skipping unknown (size 0) types. The single source of truth for per-component column
// size + alignment, shared by Archetype::InitializeTable and hot-reload component migration so
// the two can never derive a different stride for the same archetype.
inline std::vector<ComponentMeta> BuildSortedComponentMetas(
    std::span<const ComponentTypeId> componentIds,
    const std::unordered_map<ComponentTypeId, std::size_t>& sizes)
{
    std::vector<ComponentMeta> metas;
    metas.reserve(componentIds.size());
    for (ComponentTypeId typeId : componentIds)
    {
        const auto it = sizes.find(typeId);
        if (it == sizes.end() || it->second == 0)
            continue; // unknown component type — skip
        if (it->second > std::numeric_limits<uint32_t>::max())
            throw std::length_error("Component size exceeds the column layout range");
        const uint32_t size = static_cast<uint32_t>(it->second);
        const uint32_t align = RoundUpPow2(std::min(size, static_cast<uint32_t>(16)));
        metas.push_back({typeId, size, align});
    }
    std::sort(metas.begin(), metas.end(),
              [](const ComponentMeta& a, const ComponentMeta& b) { return a.TypeId < b.TypeId; });
    return metas;
}

// ============================================================================
// ArchetypeChunk — a cache-line-aligned SoA block, normally 16KB.
// ============================================================================

class ArchetypeChunk
{
  public:
    ArchetypeChunk(std::byte* memory, const ColumnLayout* layout)
        : m_Memory(memory), m_Layout(layout)
    {
    }

    uint32_t GetCount() const { return m_Count; }
    uint32_t GetCapacity() const { return m_Layout->Capacity; }
    bool IsFull() const { return m_Count >= m_Layout->Capacity; }
    bool IsEmpty() const { return m_Count == 0; }

    // Raw chunk memory base — used by pre-baked move plans that compute
    // offsets from a single base pointer rather than per-column indirection.
    std::byte* GetMemory() { return m_Memory; }
    const std::byte* GetMemory() const { return m_Memory; }

    // ------------------------------------------------------------------
    // Column access.
    // ------------------------------------------------------------------

    // Standard access: looks up offset from layout (1 indirection per call).
    template <typename T>
    T* GetColumn(int columnIndex)
    {
        return reinterpret_cast<T*>(m_Memory + m_Layout->Columns[columnIndex].Offset);
    }

    template <typename T>
    const T* GetColumn(int columnIndex) const
    {
        return reinterpret_cast<const T*>(m_Memory + m_Layout->Columns[columnIndex].Offset);
    }

    void* GetColumnRaw(int columnIndex)
    {
        return m_Memory + m_Layout->Columns[columnIndex].Offset;
    }

    const void* GetColumnRaw(int columnIndex) const
    {
        return m_Memory + m_Layout->Columns[columnIndex].Offset;
    }

    // Fast access: caller has pre-cached the byte offset from the layout.
    // Avoids per-chunk m_Layout->Columns[] indirection in tight iteration loops.
    template <typename T>
    T* GetColumnByOffset(uint32_t byteOffset) { return reinterpret_cast<T*>(m_Memory + byteOffset); }
    template <typename T>
    const T* GetColumnByOffset(uint32_t byteOffset) const { return reinterpret_cast<const T*>(m_Memory + byteOffset); }

    EntityHandle* GetEntityHandles()
    {
        return reinterpret_cast<EntityHandle*>(m_Memory + m_Layout->EntityHandleOffset);
    }

    const EntityHandle* GetEntityHandles() const
    {
        return reinterpret_cast<const EntityHandle*>(m_Memory + m_Layout->EntityHandleOffset);
    }

    // Fast entity handle access with pre-cached offset.
    EntityHandle* GetEntityHandlesByOffset(uint32_t ehOffset) { return reinterpret_cast<EntityHandle*>(m_Memory + ehOffset); }
    const EntityHandle* GetEntityHandlesByOffset(uint32_t ehOffset) const { return reinterpret_cast<const EntityHandle*>(m_Memory + ehOffset); }

    // ------------------------------------------------------------------
    // Structural operations (called by ArchetypeTable under its lock).
    // ------------------------------------------------------------------

    // Append an entity. Returns index within this chunk.
    // Caller must have verified !IsFull(). Component data at idx is
    // uninitialized; caller must write into it.
    uint32_t Append(EntityHandle entity)
    {
        uint32_t idx = m_Count++;
        GetEntityHandles()[idx] = entity;
        return idx;
    }

    // Remove entity at index via swap-and-pop with the last element.
    // Returns the entity that was moved into `index`, or Invalid if
    // `index` was the last element (no swap needed).
    EntityHandle SwapRemove(uint32_t index)
    {
        uint32_t last = m_Count - 1;
        EntityHandle movedEntity = EntityHandle::Invalid();

        if (index != last)
        {
            movedEntity = GetEntityHandles()[last];
            GetEntityHandles()[index] = movedEntity;

            // Swap component data for every column
            for (const auto& col : m_Layout->Columns)
            {
                std::byte* base = m_Memory + col.Offset;
                std::byte* dst = base + static_cast<std::size_t>(index) * col.Size;
                std::byte* src = base + static_cast<std::size_t>(last) * col.Size;
                std::memcpy(dst, src, col.Size);
            }
        }

        --m_Count;
        return movedEntity;
    }

    // Copy matching columns from src[srcIndex] into dst[dstIndex].
    // Both layouts must have columns sorted by TypeId (merge-join).
    static void CopyRow(const ArchetypeChunk& src, uint32_t srcIndex,
                        ArchetypeChunk& dst, uint32_t dstIndex)
    {
        const auto& srcCols = src.m_Layout->Columns;
        const auto& dstCols = dst.m_Layout->Columns;
        int si = 0, di = 0;
        const int sn = static_cast<int>(srcCols.size());
        const int dn = static_cast<int>(dstCols.size());

        while (si < sn && di < dn)
        {
            if (srcCols[si].TypeId < dstCols[di].TypeId)
            {
                ++si;
            }
            else if (srcCols[si].TypeId > dstCols[di].TypeId)
            {
                ++di;
            }
            else
            {
                const auto& sc = srcCols[si];
                const auto& dc = dstCols[di];
                const std::byte* srcPtr =
                    src.m_Memory + sc.Offset + static_cast<std::size_t>(srcIndex) * sc.Size;
                std::byte* dstPtr =
                    dst.m_Memory + dc.Offset + static_cast<std::size_t>(dstIndex) * dc.Size;
                std::memcpy(dstPtr, srcPtr, std::min(sc.Size, dc.Size));
                ++si;
                ++di;
            }
        }
    }

    // Direct byte access for type-erased operations (hooks, serialization).
    void* GetComponentRaw(int columnIndex, uint32_t entityIndex)
    {
        const auto& col = m_Layout->Columns[columnIndex];
        return m_Memory + col.Offset + static_cast<std::size_t>(entityIndex) * col.Size;
    }

    const void* GetComponentRaw(int columnIndex, uint32_t entityIndex) const
    {
        const auto& col = m_Layout->Columns[columnIndex];
        return m_Memory + col.Offset + static_cast<std::size_t>(entityIndex) * col.Size;
    }

    const ColumnLayout* GetLayout() const { return m_Layout; }

    // Re-seat the layout pointer (used after move-constructing the owning table).
    void SetLayout(const ColumnLayout* layout) { m_Layout = layout; }

  private:
    std::byte* m_Memory = nullptr;
    const ColumnLayout* m_Layout = nullptr;
    uint32_t m_Count = 0;
};

// ============================================================================
// ArchetypeTable — manages all chunks for a single archetype.
// ============================================================================

class ArchetypeTable
{
  public:
    explicit ArchetypeTable(std::span<const ComponentMeta> componentMetas)
        : m_Layout(BuildColumnLayout(componentMetas))
    {
    }

    ~ArchetypeTable()
    {
        for (auto* mem : m_ChunkMemory)
            FreeChunkMemory(mem);
    }

    ArchetypeTable(const ArchetypeTable&) = delete;
    ArchetypeTable& operator=(const ArchetypeTable&) = delete;

    ArchetypeTable(ArchetypeTable&& other) noexcept
        : m_Layout(std::move(other.m_Layout)),
          m_ChunkMemory(std::move(other.m_ChunkMemory)),
          m_Chunks(std::move(other.m_Chunks)), // preserves m_Count in each chunk
          m_ColumnVersions(std::move(other.m_ColumnVersions)),
          m_StampCount(other.m_StampCount),
          m_EntityCount(other.m_EntityCount)
    {
        other.m_EntityCount = 0;
        // Patch layout pointers: chunks still reference the moved-from object's m_Layout address.
        for (auto& chunk : m_Chunks)
            chunk.SetLayout(&m_Layout);
    }

    ArchetypeTable& operator=(ArchetypeTable&& other) noexcept
    {
        if (this != &other)
        {
            for (auto* mem : m_ChunkMemory)
                FreeChunkMemory(mem);

            m_Layout = std::move(other.m_Layout);
            m_ChunkMemory = std::move(other.m_ChunkMemory);
            m_Chunks = std::move(other.m_Chunks);
            m_ColumnVersions = std::move(other.m_ColumnVersions);
            m_StampCount = other.m_StampCount;
            m_EntityCount = other.m_EntityCount;
            other.m_EntityCount = 0;

            for (auto& chunk : m_Chunks)
                chunk.SetLayout(&m_Layout);
        }
        return *this;
    }

    const ColumnLayout& GetLayout() const { return m_Layout; }
    uint32_t GetEntityCount() const { return m_EntityCount; }
    std::size_t GetChunkCount() const { return m_Chunks.size(); }

    std::span<ArchetypeChunk> GetChunks() { return std::span<ArchetypeChunk>(m_Chunks); }
    std::span<const ArchetypeChunk> GetChunks() const { return std::span<const ArchetypeChunk>(m_Chunks); }

    int FindColumnIndex(ComponentTypeId typeId) const { return m_Layout.FindColumnIndex(typeId); }

    // ------------------------------------------------------------------
    // Per-(chunk x column) change versions (ECS/ChangeFilter.h).
    //
    // Flat SoA side array, index-aligned with m_Chunks:
    // [chunkIndex * columnCount + columnIndex] -> last write-grant version.
    // ZERO-INIT = "NEVER CHANGED" (design C7) — under the monotonic compare,
    // stamp 0 is the OLDEST possible value, not the newest. New-chunk
    // visibility depends ENTIRELY on the write-path stamping rules (rows
    // enter chunks only via Append, whose caller stamps all columns). Do not
    // treat zero-init as a safety net.
    //
    // The array swaps entries in CompactChunks alongside m_Chunks /
    // m_ChunkMemory, travels with move ctor/assign, and grows in
    // AllocateNewChunk (which Reserve and AddEntity share) — all four
    // maintenance paths (design M14, pinned by CompactRelocationKeepsStamps).
    //
    // Relaxed atomics: parallel dispatches partition chunks disjointly so two
    // workers never race one chunk's stamps, and consumers read after a
    // JobCounter join which provides the ordering. Reads use atomic_ref too —
    // plain loads racing worker atomic stores are UB (M14).
    // ------------------------------------------------------------------

    std::size_t GetColumnCount() const { return m_Layout.Columns.size(); }

    // libc++ (macOS) has no std::atomic_ref<const T> (cv-qualified atomic_ref is
    // C++26/P3323), so const readers form a non-const ref via const_cast. Sound:
    // load() never writes, and these members are never part of a const-qualified
    // complete object (tables live in World-owned containers). Keeps the members
    // themselves const-protected — only the non-const stamp paths may write them.
    static uint64_t LoadCounterRelaxed(const uint64_t& counter)
    {
        return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(counter))
            .load(std::memory_order_relaxed);
    }

    uint64_t GetColumnVersion(std::size_t chunkIdx, std::size_t columnIdx) const
    {
        return LoadCounterRelaxed(m_ColumnVersions[chunkIdx * GetColumnCount() + columnIdx]);
    }

    void StampColumnVersion(std::size_t chunkIdx, std::size_t columnIdx, uint64_t version)
    {
        std::atomic_ref<uint64_t>(m_ColumnVersions[chunkIdx * GetColumnCount() + columnIdx])
            .store(version, std::memory_order_relaxed);
        std::atomic_ref<uint64_t>(m_StampCount).fetch_add(1, std::memory_order_relaxed);
    }

    void StampAllColumnVersions(std::size_t chunkIdx, uint64_t version)
    {
        const std::size_t cc = GetColumnCount();
        for (std::size_t k = 0; k < cc; ++k)
        {
            std::atomic_ref<uint64_t>(m_ColumnVersions[chunkIdx * cc + k])
                .store(version, std::memory_order_relaxed);
        }
        std::atomic_ref<uint64_t>(m_StampCount).fetch_add(cc, std::memory_order_relaxed);
    }

    // Total column stamps taken on this table since creation (telemetry for
    // the IdleEditorStampCount gate — an idle editor frame's world-wide delta
    // must be ~0). Relaxed monotonic counter; never used for correctness.
    uint64_t GetStampCount() const
    {
        return LoadCounterRelaxed(m_StampCount);
    }

    // True iff any of the given columns was write-granted after `gate`.
    // columnIndices entries < 0 (missing optional columns) are skipped.
    bool AnyColumnChangedSince(std::size_t chunkIdx, std::span<const int> columnIndices,
                               uint64_t gate) const
    {
        for (int col : columnIndices)
        {
            if (col >= 0 && GetColumnVersion(chunkIdx, static_cast<std::size_t>(col)) > gate)
                return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    // Structural operations
    // ------------------------------------------------------------------

    // Add an entity. Returns its location (chunkIndex, indexInChunk).
    // Component data at the returned location is uninitialized.
    EntityLocation AddEntity(EntityHandle entity)
    {
        ArchetypeChunk* target = nullptr;
        uint32_t chunkIdx = 0;

        if (!m_Chunks.empty() && !m_Chunks.back().IsFull())
        {
            chunkIdx = static_cast<uint32_t>(m_Chunks.size() - 1);
            target = &m_Chunks.back();
        }
        else
        {
            chunkIdx = static_cast<uint32_t>(m_Chunks.size());
            AllocateNewChunk();
            target = &m_Chunks.back();
        }

        uint32_t indexInChunk = target->Append(entity);
        ++m_EntityCount;
        return {chunkIdx, indexInChunk};
    }

    // Remove entity at the given location via swap-and-pop within the chunk.
    // Returns the entity that was swapped into the vacated slot (for metadata
    // fixup), or Invalid if the removed entity was the last in its chunk.
    // Empty chunks stay allocated to avoid cascading metadata fixups for all
    // entities in a relocated chunk. Chunk compaction is a future optimization.
    EntityHandle RemoveEntity(EntityLocation loc)
    {
        auto& chunk = m_Chunks[loc.ChunkIndex];
        EntityHandle moved = chunk.SwapRemove(loc.IndexInChunk);
        --m_EntityCount;
        return moved;
    }

    // Reserve enough chunks for at least `entityCount` total entities.
    void Reserve(uint32_t entityCount)
    {
        if (m_Layout.Capacity == 0)
            return;
        uint32_t chunksNeeded = (entityCount + m_Layout.Capacity - 1) / m_Layout.Capacity;
        while (m_Chunks.size() < chunksNeeded)
            AllocateNewChunk();
    }

    // Compact: remove empty chunks by swapping them with the last non-empty chunk.
    // The callback is invoked for each relocated chunk so the caller can update
    // entity metadata: callback(oldChunkIndex, newChunkIndex, chunk).
    // Returns the number of chunks freed.
    template <typename RelocateCallback>
    uint32_t CompactChunks(RelocateCallback&& onRelocate)
    {
        uint32_t freed = 0;
        // Scan forward for empty chunks; swap with last non-empty chunk.
        std::size_t writeIdx = 0;
        std::size_t readEnd = m_Chunks.size();

        while (writeIdx < readEnd)
        {
            if (m_Chunks[writeIdx].GetCount() > 0)
            {
                ++writeIdx;
                continue;
            }

            // Found an empty chunk at writeIdx. Find the last non-empty chunk to swap with.
            while (readEnd > writeIdx + 1 && m_Chunks[readEnd - 1].GetCount() == 0)
            {
                // Trailing empty chunks can just be freed.
                FreeChunkMemory(m_ChunkMemory[readEnd - 1]);
                m_ChunkMemory.pop_back();
                m_Chunks.pop_back();
                PopColumnVersionRow();
                --readEnd;
                ++freed;
            }

            if (writeIdx >= readEnd)
                break;

            if (m_Chunks[writeIdx].GetCount() == 0 && readEnd > writeIdx + 1)
            {
                // Swap non-empty chunk at readEnd-1 into writeIdx.
                uint32_t fromIdx = static_cast<uint32_t>(readEnd - 1);
                uint32_t toIdx = static_cast<uint32_t>(writeIdx);

                std::swap(m_ChunkMemory[toIdx], m_ChunkMemory[fromIdx]);
                std::swap(m_Chunks[toIdx], m_Chunks[fromIdx]);
                SwapColumnVersionRows(toIdx, fromIdx);

                // Notify caller so entity metadata can be updated.
                onRelocate(fromIdx, toIdx, m_Chunks[toIdx]);

                // Free the now-empty chunk at the end.
                FreeChunkMemory(m_ChunkMemory[fromIdx]);
                m_ChunkMemory.pop_back();
                m_Chunks.pop_back();
                PopColumnVersionRow();
                --readEnd;
                ++freed;
            }
            else if (m_Chunks[writeIdx].GetCount() == 0)
            {
                // Last chunk is empty — just free it.
                FreeChunkMemory(m_ChunkMemory[writeIdx]);
                m_ChunkMemory.pop_back();
                m_Chunks.pop_back();
                PopColumnVersionRow();
                --readEnd;
                ++freed;
            }

            ++writeIdx;
        }

        return freed;
    }

    // Iterate all non-empty chunks.
    template <typename Func>
    void ForEachChunk(Func&& func)
    {
        for (auto& chunk : m_Chunks)
        {
            if (chunk.GetCount() > 0)
                func(chunk);
        }
    }

  private:
    std::byte* AllocateChunkMemory() const
    {
#ifdef _MSC_VER
        auto* ptr = static_cast<std::byte*>(_aligned_malloc(m_Layout.AllocationBytes, m_Layout.AllocationAlignment));
#else
        auto* ptr = static_cast<std::byte*>(std::aligned_alloc(m_Layout.AllocationAlignment, m_Layout.AllocationBytes));
#endif
        if (!ptr) [[unlikely]]
        {
            throw std::bad_alloc();
        }
        return ptr;
    }

    static void FreeChunkMemory(std::byte* ptr)
    {
#ifdef _MSC_VER
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }

    void AllocateNewChunk()
    {
        std::unique_ptr<std::byte, decltype(&FreeChunkMemory)> memory(AllocateChunkMemory(), &FreeChunkMemory);
        std::byte* mem = memory.get();
        // Skip the defensive zero-fill in release. Every live slot is written
        // before it is read: AddEntity calls Append() immediately after any
        // new chunk alloc; component columns are populated by the caller
        // (CreateHandle / Set / MoveEntity CopyRow). A read-before-write is
        // a latent bug that zero-fill would silently mask anyway.
        // Debug keeps the fill to help catch such bugs with an obvious
        // signature (zeroed handles are invalid, zeroed floats are 0.0).
#ifndef NDEBUG
        std::memset(mem, 0, m_Layout.AllocationBytes);
#endif
        try
        {
            // One version row per chunk; zero = never changed (design C7).
            m_ColumnVersions.resize(m_ColumnVersions.size() + GetColumnCount(), 0);
            m_Chunks.emplace_back(mem, &m_Layout);
            m_ChunkMemory.push_back(mem);
        }
        catch (...)
        {
            // Keep the three side arrays in sync if any vector growth fails.
            if (m_Chunks.size() > m_ChunkMemory.size())
                m_Chunks.pop_back();
            m_ColumnVersions.resize(m_ChunkMemory.size() * GetColumnCount());
            throw;
        }
        memory.release();
    }

    void SwapColumnVersionRows(std::size_t a, std::size_t b)
    {
        const std::size_t cc = GetColumnCount();
        for (std::size_t k = 0; k < cc; ++k)
            std::swap(m_ColumnVersions[a * cc + k], m_ColumnVersions[b * cc + k]);
    }

    void PopColumnVersionRow()
    {
        m_ColumnVersions.resize(m_ColumnVersions.size() - GetColumnCount());
    }

    ColumnLayout m_Layout;
    std::vector<std::byte*> m_ChunkMemory;   // Raw memory ownership
    std::vector<ArchetypeChunk> m_Chunks;     // Lightweight views over memory
    // Per-(chunk x column) write-grant versions — see the accessor block above.
    std::vector<uint64_t> m_ColumnVersions;
    // Stamp telemetry — see GetStampCount(). Accessed via atomic_ref.
    uint64_t m_StampCount = 0;
    uint32_t m_EntityCount = 0;
};

} // namespace GameEngine::ECS
