// WorldDrawBuilder.cpp
#include "Engine/Rendering/WorldDrawBuilder.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialDeformationClassify.h"
#include "Engine/Rendering/MaterialVertexAnimation.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Rendering/Core/ThreadingUtils.h"

#include <algorithm>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

thread_local uint32 WorldDrawBuilder::t_MySlot = UINT32_MAX;

void WorldDrawBuilder::BeginFrame()
{
    // Each submitting thread owns one slot; slots on separate lines keep the
    // threads from invalidating each other's records.
    static_assert(alignof(decltype(m_ThreadBuffers)::value_type) == kFalseSharingSeparation);

    // Clear per-thread buffers (only those that were used last frame)
    const uint32 usedSlots = m_NextThreadSlot.load(std::memory_order_relaxed);
    for (uint32 i = 0; i < usedSlots; ++i)
    {
        m_ThreadBuffers[i].Value.records.clear();
        m_ThreadBuffers[i].Value.drained = 0;
    }
    m_NextThreadSlot.store(0, std::memory_order_relaxed);

    // Clear merged data
    for (auto& [viewId, submissions] : m_SubmissionsByView)
        submissions.clear();
    for (auto& [viewId, keys] : m_BatchKeysByView)
        keys.clear();
    for (auto& [viewId, keys] : m_DeformingKeysByView)
        keys.clear();
    for (auto& [viewId, hasCaster] : m_ShadowCasterByView)
        hasCaster = false;
    for (auto& [viewId, hasCaster] : m_TransmissiveCasterByView)
        hasCaster = false;
    for (auto& [viewId, hasDeformer] : m_AnimatedVertexModifierByView)
        hasDeformer = false;
    for (auto& [viewId, hasDeformingCaster] : m_AnimatedVertexModifierCasterByView)
        hasDeformingCaster = false;
}

void WorldDrawBuilder::Submit(std::span<const WorldSubmissionRecord> records)
{
    if (records.empty())
        return;

    // Each thread lazily acquires a unique slot (lock-free)
    uint32 slot = t_MySlot;
    if (slot >= m_NextThreadSlot.load(std::memory_order_relaxed) || slot == UINT32_MAX)
    {
        slot = m_NextThreadSlot.fetch_add(1, std::memory_order_relaxed);
        t_MySlot = slot;
    }
    assert(slot < kMaxSubmitThreads && "Too many threads submitting draws concurrently");

    auto& buf = m_ThreadBuffers[slot].Value.records;
    buf.insert(buf.end(), records.begin(), records.end());
}

void WorldDrawBuilder::MergeSubmissions()
{
    // Idempotent incremental drain: each slot tracks how many records it has
    // already been drained for this frame, so late submissions (e.g. thumbnail
    // views submitted after the main scene pipeline has already built keys)
    // still land in m_SubmissionsByView on the next MergeSubmissions() call.
    const uint32 count = m_NextThreadSlot.load(std::memory_order_acquire);
    for (uint32 i = 0; i < count; ++i)
    {
        auto& buf = m_ThreadBuffers[i].Value;
        const uint32 total = static_cast<uint32>(buf.records.size());
        for (uint32 r = buf.drained; r < total; ++r)
        {
            const auto& rec = buf.records[r];
            m_SubmissionsByView[rec.viewId].push_back(rec);
        }
        buf.drained = total;
    }
}

uint32_t WorldDrawBuilder::BuildBatchKeys(const Rendering::MeshGPURegistry& meshRegistry,
                                          std::span<const uint32_t> materialColorClass)
{
    MergeSubmissions();

    for (auto& [viewId, submissions] : m_SubmissionsByView)
        DeriveBatchKeysForView(viewId, submissions, meshRegistry, materialColorClass);

    return static_cast<uint32_t>(m_SubmissionsByView.size());
}

void WorldDrawBuilder::BuildBatchKeysForView(Rendering::ViewId viewId,
                                             const Rendering::MeshGPURegistry& meshRegistry,
                                             std::span<const uint32_t> materialColorClass)
{
    MergeSubmissions();
    auto itSub = m_SubmissionsByView.find(viewId);
    if (itSub == m_SubmissionsByView.end())
    {
        m_BatchKeysByView.erase(viewId);
        m_ShadowCasterByView.erase(viewId);
        m_TransmissiveCasterByView.erase(viewId);
        m_AnimatedVertexModifierByView.erase(viewId);
        m_AnimatedVertexModifierCasterByView.erase(viewId);
        return;
    }
    DeriveBatchKeysForView(viewId, itSub->second, meshRegistry, materialColorClass);
}

std::span<const WorldDrawBuilder::BatchKey> WorldDrawBuilder::GetBatchKeys(Rendering::ViewId viewId) const
{
    auto it = m_BatchKeysByView.find(viewId);
    if (it == m_BatchKeysByView.end() || it->second.empty())
    {
        return {};
    }
    const auto& vec = it->second;
    return std::span<const BatchKey>(vec.data(), vec.size());
}

std::span<const WorldSubmissionRecord> WorldDrawBuilder::GetSubmissions(Rendering::ViewId viewId) const
{
    auto it = m_SubmissionsByView.find(viewId);
    if (it == m_SubmissionsByView.end() || it->second.empty())
        return {};
    const auto& vec = it->second;
    return std::span<const WorldSubmissionRecord>(vec.data(), vec.size());
}

std::span<const WorldDrawBuilder::BatchKey> WorldDrawBuilder::GetDeformingBatchKeys(
    Rendering::ViewId viewId) const
{
    auto it = m_DeformingKeysByView.find(viewId);
    if (it == m_DeformingKeysByView.end() || it->second.empty())
        return {};
    const auto& vec = it->second;
    return std::span<const BatchKey>(vec.data(), vec.size());
}

bool WorldDrawBuilder::HasShadowCastingSubmissions(Rendering::ViewId viewId) const
{
    const auto it = m_ShadowCasterByView.find(viewId);
    return it != m_ShadowCasterByView.end() && it->second;
}

bool WorldDrawBuilder::HasTransmissiveCasterSubmissions(Rendering::ViewId viewId) const
{
    const auto it = m_TransmissiveCasterByView.find(viewId);
    return it != m_TransmissiveCasterByView.end() && it->second;
}

bool WorldDrawBuilder::HasAnimatedVertexModifierSubmissions(Rendering::ViewId viewId) const
{
    const auto it = m_AnimatedVertexModifierByView.find(viewId);
    return it != m_AnimatedVertexModifierByView.end() && it->second;
}

bool WorldDrawBuilder::HasAnimatedVertexModifierCasterSubmissions(Rendering::ViewId viewId) const
{
    const auto it = m_AnimatedVertexModifierCasterByView.find(viewId);
    return it != m_AnimatedVertexModifierCasterByView.end() && it->second;
}

void WorldDrawBuilder::DeriveBatchKeysForView(
    Rendering::ViewId viewId,
    const std::vector<WorldSubmissionRecord>& submissions,
    const Rendering::MeshGPURegistry& meshRegistry,
    std::span<const uint32_t> materialColorClass)
{
    auto& keys = m_BatchKeysByView[viewId];
    keys.clear();
    keys.reserve(submissions.size());
    bool shadowCaster = false;
    bool transmissiveCaster = false;
    bool animatedVertexModifier = false;
    bool animatedVertexModifierCaster = false;

    for (const auto& rec : submissions)
    {
        // A census of submitted casters, ahead of every drawability filter.
        shadowCaster = shadowCaster || (rec.flags & kSubmissionFlagCastShadows) != 0u;
        if (!rec.material)
            continue;
        // Ahead of the pipeline and mesh-residency filters on purpose: this is a
        // census of what was submitted, not of what is drawable this frame. A
        // deforming caster whose mesh or pipeline is still pending invalidates
        // the caches from its first submitted frame, a re-render on frames that
        // already pay for the upload, rather than from the frame it first draws.
        // Either way the caches never hold a layer rasterized without a drawn
        // caster, because this derivation precedes every reader in the frame.
        // A caster implies a renderable, so the first one found ends the search.
        if (!animatedVertexModifierCaster && MaterialDeformsVertices(*rec.material))
        {
            animatedVertexModifier = true;
            animatedVertexModifierCaster = (rec.flags & kSubmissionFlagCastShadows) != 0u;
        }
        if (!rec.material->GetGraphicsPipelineId().IsValid())
            continue;
        const auto* entry = meshRegistry.Find(rec.meshHandle);
        if (!entry || entry->gpuMeshIndex == ~0u || entry->indexCount == 0u)
            continue;
        // bit 0 == castsShadows (ComputeInstanceFlags), the same bit the shadow
        // cull tests before an instance can reach a cascade stream.
        if (!transmissiveCaster && (rec.flags & 1u) != 0u &&
            Rendering::HasKeyword(rec.material->GetVariantKey().materialKeywords,
                                  Rendering::MaterialKeyword::Transmission))
            transmissiveCaster = true;
        const uint32_t materialIndex = rec.material->GetGpuSceneMaterialIndex();
        // colorClassId == materialIndex when the merge is off (empty map) or the
        // material is not merge-eligible (identity entry) -- so the OFF path is
        // exactly the old (materialIndex, meshIndex) key.
        const uint32_t colorClassId = materialIndex < materialColorClass.size()
                                          ? materialColorClass[materialIndex]
                                          : materialIndex;
        keys.push_back(BatchKey{
            rec.material,
            rec.meshHandle,
            materialIndex,
            entry->gpuMeshIndex,
            colorClassId});
    }

    // Sort by (colorClassId, meshIndex, materialIndex): grouping by
    // (colorClassId, meshIndex) makes the merge members contiguous for the
    // std::unique dedup below, and ordering materialIndex last makes the FIRST
    // survivor of each group the lowest-materialIndex member -- the
    // deterministic class representative (P2-d). With the merge off,
    // colorClassId == materialIndex, so this reduces to the old
    // (materialIndex, meshIndex) order exactly.
    std::sort(keys.begin(), keys.end(),
              [](const BatchKey& a, const BatchKey& b) {
                  if (a.colorClassId != b.colorClassId)
                      return a.colorClassId < b.colorClassId;
                  if (a.meshIndex != b.meshIndex)
                      return a.meshIndex < b.meshIndex;
                  return a.materialIndex < b.materialIndex;
              });
#if !defined(NDEBUG)
    for (size_t i = 1; i < keys.size(); ++i)
    {
        const auto& prev = keys[i - 1];
        const auto& cur = keys[i];
        // The sort must group by colorClassId so the (colorClassId, meshIndex)
        // dedup collapses ALL members of a class -- a non-monotonic sequence
        // would leave members non-adjacent and double-draw the merged range.
        assert(prev.colorClassId <= cur.colorClassId
               && "batch keys must be grouped by color class for the merge dedup");
        // Identity invariant preserved: a (materialIndex, meshIndex) pair still
        // maps to exactly one (Material*, mesh) -- materialIndex is owned by
        // Material, meshIndex by MeshGPURegistry -- and a material maps to a
        // single color class. Different materials sharing (colorClassId,
        // meshIndex) is the merge, NOT a violation.
        if (prev.materialIndex == cur.materialIndex && prev.meshIndex == cur.meshIndex)
        {
            assert((prev.material == cur.material && prev.mesh == cur.mesh
                    && prev.colorClassId == cur.colorClassId)
                   && "BatchKey (matIdx, meshIdx) maps to multiple (Material*/mesh/class)");
        }
    }
#endif
    // Collapse to one representative per (colorClassId, meshIndex): merged
    // opaque materials of a class draw the shared range once; identity keys
    // (merge off / material-dependent) keep their per-material row because
    // colorClassId == materialIndex there.
    keys.erase(std::unique(keys.begin(), keys.end(),
                           [](const BatchKey& a, const BatchKey& b) {
                               return a.colorClassId == b.colorClassId
                                   && a.meshIndex == b.meshIndex;
                           }),
               keys.end());

    m_ShadowCasterByView[viewId] = shadowCaster;
    m_TransmissiveCasterByView[viewId] = transmissiveCaster;
    m_AnimatedVertexModifierByView[viewId] = animatedVertexModifier;
    m_AnimatedVertexModifierCasterByView[viewId] = animatedVertexModifierCaster;

    // The deforming subset, taken after the dedup so it names the keys the
    // producer will actually record. A deforming material is never
    // colour-merge eligible (its depth class is MaterialDependent), so its
    // colorClassId equals its materialIndex and its row survives the dedup as
    // itself — the representative here is always the deforming material, never
    // a merge partner standing in for it.
    auto& deforming = m_DeformingKeysByView[viewId];
    deforming.clear();
    for (const auto& key : keys)
    {
        if (key.material && SupportsDeformationMotion(*key.material))
            deforming.push_back(key);
    }
}

void WorldDrawBuilder::ClearView(Rendering::ViewId viewId)
{
    m_SubmissionsByView.erase(viewId);
    m_BatchKeysByView.erase(viewId);
    m_DeformingKeysByView.erase(viewId);
    m_ShadowCasterByView.erase(viewId);
    m_TransmissiveCasterByView.erase(viewId);
    m_AnimatedVertexModifierByView.erase(viewId);
    m_AnimatedVertexModifierCasterByView.erase(viewId);
}

void WorldDrawBuilder::Clear()
{
    m_SubmissionsByView.clear();
    m_BatchKeysByView.clear();
    m_DeformingKeysByView.clear();
    m_ShadowCasterByView.clear();
    m_TransmissiveCasterByView.clear();
    m_AnimatedVertexModifierByView.clear();
    m_AnimatedVertexModifierCasterByView.clear();
}

} // namespace Engine::Renderer
} // namespace GameEngine
