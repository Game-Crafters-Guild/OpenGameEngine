#include "Engine/Rendering/MeshPoolGroupPlan.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <utility>

namespace GameEngine
{
namespace Rendering
{

bool MeshPoolGroupPlan::ConsolidationEnabled()
{
    static const bool s_Enabled = []
    {
        const char* env = std::getenv("GE_DRAW_CONSOLIDATION");
        return env == nullptr || env[0] != '0';
    }();
    return s_Enabled;
}

size_t MeshPoolGroupPlan::PoolIdentityHash::operator()(const PoolIdentity& k) const
{
    // FNV-1a over the identity words. Collisions are resolved by the map's
    // exact equality — the hash only spreads buckets.
    constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime  = 1099511628211ull;
    uint64_t h = kFnvOffset;
    for (uint32_t v : k.Fields)
    {
        h ^= v;
        h *= kFnvPrime;
    }
    return static_cast<size_t>(h);
}

MeshPoolGroupPlan::PoolIdentity MeshPoolGroupPlan::MakeIdentity(const MeshGPUEntry& entry)
{
    PoolIdentity id{};
    size_t i = 0;
    id.Fields[i++] = static_cast<uint32_t>(entry.bucketKey);
    id.Fields[i++] = entry.corePoolIndex;
    id.Fields[i++] = entry.tangentPoolIndex;
    id.Fields[i++] = entry.colorPoolIndex;
    id.Fields[i++] = entry.uv1PoolIndex;
    id.Fields[i++] = entry.jointsPoolIndex;
    id.Fields[i++] = entry.weightsPoolIndex;
    id.Fields[i++] = entry.joints1PoolIndex;
    id.Fields[i++] = entry.weights1PoolIndex;
    for (uint32_t extra : entry.extraUvPoolIndex)
        id.Fields[i++] = extra;
    id.Fields[i++] = entry.indexPoolIndex;
    id.Fields[i++] = entry.indexType;
    id.Fields[i++] = static_cast<uint32_t>(entry.topology);
    assert(i == id.Fields.size() && "PoolIdentity field count drifted");
    return id;
}

void MeshPoolGroupPlan::Refresh(const MeshGPURegistry& registry, uint32_t meshTableCount)
{
    m_MeshToGroup.assign(meshTableCount, kAbsentGroup);
    m_SkippedNonResident = 0;
    ++m_Refreshes;

    registry.ForEachEntry(
        [&](const MeshGPUEntry& entry)
        {
            if (entry.gpuMeshIndex == ~0u || entry.gpuMeshIndex >= meshTableCount)
                return;
            // Failed-allocation entries never draw; leave them absent.
            if (entry.bucketKey == VertexAttributeFlags::None || entry.indexCount == 0u)
                return;
            // Route A chokepoint. An entry whose uploaded bytes are not
            // readable by the GPU yet stays absent, so no (classKey, group)
            // range contains it and nothing draws over storage that has not
            // been filled. It reappears on the Refresh after its upload lands.
            if (!registry.IsFlushResident(entry))
            {
                ++m_SkippedNonResident;
                ++m_SkippedNonResidentTotal;
                return;
            }
            const auto [it, inserted] =
                m_KeyToGroup.try_emplace(MakeIdentity(entry),
                                         static_cast<uint32_t>(m_KeyToGroup.size()));
            assert(it->second < kAbsentGroup
                   && "pool group id domain exhausted (2^24 identities)");
            m_MeshToGroup[entry.gpuMeshIndex] = it->second;
        });

    // Live census: distinct groups actually referenced by the map this refresh.
    std::vector<bool> seen(m_KeyToGroup.size(), false);
    uint32_t live = 0;
    for (uint32_t g : m_MeshToGroup)
    {
        if (g != kAbsentGroup && g < seen.size() && !seen[g])
        {
            seen[g] = true;
            ++live;
        }
    }
    m_LiveGroupCount = live;

    // Ordered mesh axis: dense ranks over the live meshes sorted by
    // (groupId, gpuMeshIndex), so a group's members occupy a contiguous rank
    // range. Recomputed from scratch every Refresh — ranks are per-snapshot
    // data (each schedule call uploads the span it keyed its tables on), so
    // they carry none of the group ids' append-only burden.
    std::vector<std::pair<uint32_t, uint32_t>> liveMeshes; // (groupId, gpuMeshIndex)
    liveMeshes.reserve(m_MeshToGroup.size());
    for (uint32_t meshIndex = 0; meshIndex < m_MeshToGroup.size(); ++meshIndex)
    {
        if (m_MeshToGroup[meshIndex] != kAbsentGroup)
            liveMeshes.emplace_back(m_MeshToGroup[meshIndex], meshIndex);
    }
    if (liveMeshes.empty())
    {
        // Both-or-neither at the source: with ZERO live registry entries (a
        // scene-load streaming transient before the first mesh upload, or a
        // teardown seam) publish EMPTY spans on every axis, so the scatter
        // schedule, the range publisher, and the record-time consumers all
        // degrade to the per-mesh (consolidation-OFF) path coherently for the
        // frame. A non-empty all-absent mesh map paired with an empty
        // rank->group inverse would trip the schedule's paired-span assert —
        // live in DebugFast, the daily-driver config.
        m_MeshToGroup.clear();
        m_MeshToOrdered.clear();
        m_OrderedToGroup.clear();
        return;
    }
    std::sort(liveMeshes.begin(), liveMeshes.end());
    assert(liveMeshes.size() < kAbsentGroup && "ordered mesh axis domain exhausted (2^24 ranks)");
    m_MeshToOrdered.assign(meshTableCount, kAbsentGroup);
    m_OrderedToGroup.resize(liveMeshes.size());
    for (uint32_t rank = 0; rank < liveMeshes.size(); ++rank)
    {
        m_MeshToOrdered[liveMeshes[rank].second] = rank;
        m_OrderedToGroup[rank]                   = liveMeshes[rank].first;
    }
}

} // namespace Rendering
} // namespace GameEngine
