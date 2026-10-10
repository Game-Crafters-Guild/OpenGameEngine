/**
 * @file BatchRegistry.h
 * @brief Live-instance count per (materialIndex, meshIndex) draw batch.
 *
 * Maintained incrementally by GPUScene's three instance mutators — the only
 * places an instance's batch membership can change (extraction, particles,
 * and Clear/Release all route through them). The counts feed the R1.3/R1.4
 * scatter bucketer: the sorted key set becomes the GPU batch table and the
 * counts become exact per-batch output-region capacities, which is what lets
 * draw offsets be CPU-known while draw counts stay GPU-written.
 *
 * Not a hot path: entries mutate only when an instance's batch membership
 * changes (spawn/despawn/mesh-or-material swap), not per frame.
 */
#pragma once

#include <cassert>
#include <cstdint>
#include <unordered_map>

namespace GameEngine
{
namespace Rendering
{

class BatchRegistry
{
  public:
    /// Live-instance count for one (materialIndex, meshIndex) batch, split by
    /// winding parity so the scatter table builders can emit a parity-1 sibling
    /// row for mirrored (negative-determinant) instances. Even = parity-0
    /// (non-mirrored); Odd = parity-1 (mirrored). The pre-parity total is
    /// exactly Even + Odd — parity partitions the count, never adds to it (the
    /// conservation invariant the region prefix-sum depends on).
    struct ParityCounts
    {
        uint32_t Even = 0;
        uint32_t Odd  = 0;
        uint32_t Total() const { return Even + Odd; }
        bool operator==(const ParityCounts&) const = default;
    };

    /// 24-bit fields, matching GPUDrawStreamBuilder::MakeStreamKey's caps.
    /// Sort order of the packed key == (materialIndex, meshIndex) lexicographic,
    /// the same order WorldDrawBuilder sorts its per-view batch keys.
    static constexpr uint64_t MakeKey(uint32_t materialIndex, uint32_t meshIndex)
    {
        return (static_cast<uint64_t>(materialIndex & 0xFFFFFFu) << 24)
             | static_cast<uint64_t>(meshIndex & 0xFFFFFFu);
    }

    static constexpr uint32_t MaterialIndexFromKey(uint64_t key)
    {
        return static_cast<uint32_t>((key >> 24) & 0xFFFFFFu);
    }

    static constexpr uint32_t MeshIndexFromKey(uint64_t key)
    {
        return static_cast<uint32_t>(key & 0xFFFFFFu);
    }

    /// Tombstoned / cleared instances carry 0xFFFFFFFF mesh/material and must
    /// never enter the registry: an unguarded decrement on the tombstone key
    /// underflows to ~4·10⁹ and the scatter prefix sum would demand a region
    /// that large. Guarded on BOTH the increment and decrement side.
    static constexpr bool IsTracked(uint32_t materialIndex, uint32_t meshIndex)
    {
        return materialIndex != 0xFFFFFFFFu && meshIndex != 0xFFFFFFFFu;
    }

    void OnInstanceAdded(uint32_t materialIndex, uint32_t meshIndex, bool mirrored)
    {
        if (!IsTracked(materialIndex, meshIndex))
            return;
        ParityCounts& c = m_Counts[MakeKey(materialIndex, meshIndex)];
        if (mirrored)
            ++c.Odd;
        else
            ++c.Even;
    }

    void OnInstanceRemoved(uint32_t materialIndex, uint32_t meshIndex, bool mirrored)
    {
        if (!IsTracked(materialIndex, meshIndex))
            return;
        auto it = m_Counts.find(MakeKey(materialIndex, meshIndex));
        // A miss (or an empty parity bucket) here means a count-drift bug
        // upstream; tolerate in Release (skip) so the failure mode is a
        // conservative over-allocation, never an underflow.
        assert(it != m_Counts.end() && (mirrored ? it->second.Odd : it->second.Even) > 0u);
        if (it == m_Counts.end())
            return;
        uint32_t& lane = mirrored ? it->second.Odd : it->second.Even;
        if (lane > 0u)
            --lane;
        if (it->second.Total() == 0u)
            m_Counts.erase(it);
    }

    void OnInstanceUpdated(uint32_t oldMaterialIndex, uint32_t oldMeshIndex, bool oldMirrored,
                           uint32_t newMaterialIndex, uint32_t newMeshIndex, bool newMirrored)
    {
        if (oldMaterialIndex == newMaterialIndex && oldMeshIndex == newMeshIndex
            && oldMirrored == newMirrored)
            return;
        OnInstanceRemoved(oldMaterialIndex, oldMeshIndex, oldMirrored);
        OnInstanceAdded(newMaterialIndex, newMeshIndex, newMirrored);
    }

    /// Key → per-parity live instance counts. Keys iterate unordered; snapshot
    /// consumers sort by key (== (materialIndex, meshIndex)) before
    /// prefix-summing, and split each batch into even/odd sibling rows.
    const std::unordered_map<uint64_t, ParityCounts>& Counts() const { return m_Counts; }

    size_t BatchCount() const { return m_Counts.size(); }

    void Clear() { m_Counts.clear(); }

  private:
    std::unordered_map<uint64_t, ParityCounts> m_Counts;
};

} // namespace Rendering
} // namespace GameEngine
