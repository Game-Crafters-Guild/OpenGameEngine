#pragma once

// MeshPoolGroupPlan: dense, session-stable ids for distinct geometry-bind
// identities (opaque draw consolidation).
//
// Two meshes can share ONE DrawIndexedIndirectCount call iff every state the
// consumer binds for them is identical: the vertex-flags bucket (pipeline
// vertex layout), every stream's sibling-pool buffer, the index pool + index
// type, and the primitive topology. This plan walks the MeshGPURegistry and
// assigns each distinct such identity a dense "pool group" id, publishing a
// gpuMeshIndex -> groupId map that both sides of the GPU draw stream consume:
//   - GPUDrawStreamBuilder's grouped batch tables merge (classKey, mesh) rows
//     into (classKey, group) rows, and the scatter routes instances by
//     meshPoolGroup[meshIndex] instead of meshIndex.
//   - The world color / depth / shadow consumers issue one indirect draw per
//     (classKey, group) instead of one per (classKey, mesh).
//
// Id stability: ids are allocated once per NEW identity and never reassigned
// (append-only across Refresh calls). This is load-bearing — batch keys are
// derived and consumer draws recorded at different points in the frame than
// the scatter's table snapshot, and a mid-frame mesh registration (thumbnail
// seam) must only APPEND ids, never shift existing ones, or a consumer lookup
// would miss the table row its instances were scattered into.

#include "Engine/Rendering/MeshGPURegistry.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// What Route A of the residency gate left out of the map, scoped so a zero can
// be read. `SkippedNonResident` is a census of the LAST Refresh only — Refresh
// rebuilds it and runs up to three times per frame (thumbnail seam, HZB
// occlusion recovery), so a poll that reads it zero cannot tell "never skipped"
// from "skipped and was rebuilt". The total is monotonic for the process and is
// what the steady-state criterion is checked against; `Refreshes` scopes both,
// because Route A does not run at all with draw consolidation off and a zero
// from a plan that never refreshed says nothing about the gate.
struct MeshPoolGroupResidencyStats
{
    uint32_t SkippedNonResident      = 0;
    uint64_t SkippedNonResidentTotal = 0;
    uint64_t Refreshes               = 0;
};

class MeshPoolGroupPlan
{
  public:
    /// Pseudo group for mesh rows with no live registry entry (tombstoned /
    /// failed-allocation meshes, or rows registered after this plan's
    /// Refresh). 24-bit so it rides the batch-table mesh field like a real
    /// group id. Absent rows DO survive the scatter's mesh-row checks (a live
    /// mesh-table row with no plan entry) and are scattered into the table's
    /// merged pseudo row; the row is never drawn because consumers filter
    /// kAbsentGroup before their range lookup — scattered-into, never drawn.
    /// Real group ids are asserted to stay below it.
    static constexpr uint32_t kAbsentGroup = 0xFFFFFFu;

    /// GE_DRAW_CONSOLIDATION: one DrawIndexedIndirectCount per (pipeline,
    /// pool group) instead of per (pipeline, mesh). Default ON; "0" selects
    /// the per-bucket path for A/B and fallback: table bytes and the consumer
    /// command sequence match pre-consolidation exactly, but the scatter PSO
    /// still differs (10-binding layout, dead mode-0 branch). Read once per
    /// process.
    static bool ConsolidationEnabled();

    /// Rebuild the gpuMeshIndex -> groupId map from the registry's live
    /// entries. `meshTableCount` is GPUScene's mesh-table size (the map is
    /// indexed by gpuMeshIndex). Existing identities keep their ids; new ones
    /// append. Call on the render thread at declaration time only — consumers
    /// read the map during pass record (same single-writer frame discipline as
    /// the scatter range map).
    void Refresh(const MeshGPURegistry& registry, uint32_t meshTableCount);

    /// gpuMeshIndex -> groupId (kAbsentGroup for rows with no live entry).
    /// Empty until the first Refresh. Read by the draw consumers (range-map
    /// keys); an empty span is the consolidation-OFF signal throughout.
    std::span<const uint32_t> MeshToGroupSpan() const { return m_MeshToGroup; }

    /// gpuMeshIndex -> ordered mesh axis: dense ranks over the live meshes
    /// sorted by (groupId, gpuMeshIndex), kAbsentGroup for absent rows. This
    /// is the mesh axis ScheduleUnifiedScatter keys its tables and binding-9
    /// map on: per-mesh rows whose group members are CONTIGUOUS, so the
    /// group-compact pass can pack each (class, group) region mesh-major
    /// (the GPU-busy fix — records of one mesh land adjacent instead of
    /// scatter-arrival interleaved). Unlike group ids these ranks have NO
    /// cross-Refresh stability and need none: every schedule call uploads the
    /// span it was built from alongside its tables (one-snapshot discipline),
    /// and consumers never see ordered ids — their ranges stay group-keyed.
    std::span<const uint32_t> MeshToOrderedSpan() const { return m_MeshToOrdered; }

    /// orderedId -> groupId (the inverse grouping of MeshToOrderedSpan);
    /// size == live mesh count at the last Refresh. The scatter scheduler
    /// uses it to key published ranges by group and to detect group-run
    /// boundaries in its sorted tables.
    std::span<const uint32_t> OrderedToGroupSpan() const { return m_OrderedToGroup; }

    /// Bounds-checked single lookup for consumers.
    uint32_t GroupOf(uint32_t meshIndex) const
    {
        return meshIndex < m_MeshToGroup.size() ? m_MeshToGroup[meshIndex] : kAbsentGroup;
    }

    /// Distinct identities ever assigned (monotonic).
    uint32_t GroupCount() const { return static_cast<uint32_t>(m_KeyToGroup.size()); }

    /// Distinct groups among live meshes at the last Refresh — the census
    /// number: the per-(pipeline x pool) draw floor's geometry axis.
    uint32_t LiveGroupCount() const { return m_LiveGroupCount; }

    /// Route A's half of the residency gate's observability.
    MeshPoolGroupResidencyStats ResidencyStats() const
    {
        return {m_SkippedNonResident, m_SkippedNonResidentTotal, m_Refreshes};
    }

  private:
    // Exact geometry-bind identity (no lossy hashing — a collision would merge
    // draws with different bound buffers). Field order mirrors the bind sites:
    // bucket flags, core + optional stream pools, extra UVs, index pool, index
    // type, topology.
    struct PoolIdentity
    {
        std::array<uint32_t, 18> Fields{};
        bool operator==(const PoolIdentity&) const = default;
    };
    struct PoolIdentityHash
    {
        size_t operator()(const PoolIdentity& k) const;
    };

    static PoolIdentity MakeIdentity(const MeshGPUEntry& entry);

    std::unordered_map<PoolIdentity, uint32_t, PoolIdentityHash> m_KeyToGroup;
    std::vector<uint32_t> m_MeshToGroup;
    std::vector<uint32_t> m_MeshToOrdered;
    std::vector<uint32_t> m_OrderedToGroup;
    uint32_t m_LiveGroupCount          = 0;
    uint32_t m_SkippedNonResident      = 0;
    uint64_t m_SkippedNonResidentTotal = 0;
    uint64_t m_Refreshes               = 0;
};

} // namespace Rendering
} // namespace GameEngine
