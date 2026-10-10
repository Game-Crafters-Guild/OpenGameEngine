#include "Engine/Rendering/CpuDrawStreamBuilder.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Engine/Rendering/WorldDrawBuilder.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "JobSystem/ParallelAlgorithms.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUScene.h"

#include <algorithm>

namespace GameEngine
{
namespace Engine::Renderer
{
namespace
{
// Candidate::State bits. State 0 (no kResolvedBit) covers every reason a
// submission produces no draw at all — no material, no pipeline, mesh not
// resident, instance not in GPUScene yet. That is the same filter
// WorldDrawBuilder applies when it derives the batch keys, so the lists and the
// keys can never disagree about which batches exist.
enum CandidateState : uint8_t
{
    kRejected = 0,
    kResolvedBit = 1u << 0,
    kVisibleBit = 1u << 1, // inside the view frustum
    kCasterBit = 1u << 2,  // castShadows submission flag
};

// Below this, the fan-out costs more than the work. The per-instance body is a
// registry lookup plus a 6-plane sphere test, so the batch has to be large
// enough to dwarf a task publish.
constexpr size_t kCullMinBatchSize = 512;
// Sorting is the other O(n log n) term; the same reasoning sets its threshold.
constexpr size_t kSortThreshold = 4096;

uint64_t MakeBatchKey(uint32_t materialIndex, uint32_t meshIndex)
{
    return (static_cast<uint64_t>(materialIndex) << 32) | meshIndex;
}
} // namespace

void CpuDrawStreamBuilder::Clear()
{
    m_ByView.clear();
    m_Stats = {};
}

void CpuDrawStreamBuilder::Build(const WorldDrawBuilder& drawBuilder,
                                 const Rendering::MeshGPURegistry& meshRegistry,
                                 const Rendering::GPUScene& scene, const ViewRegistry& views,
                                 JobSystem::WorkStealingThreadPool* pool)
{
    m_Stats = {};
    // Keep the per-view slots (their vectors are the frame-to-frame scratch);
    // only their contents go stale.
    for (auto& [viewId, viewLists] : m_ByView)
    {
        viewLists.Indices.clear();
        viewLists.Index[0].clear();
        viewLists.Index[1].clear();
    }

    for (const Rendering::ViewDesc& view : views.GetViews())
        BuildView(view.id, drawBuilder, meshRegistry, scene, views, pool);
}

void CpuDrawStreamBuilder::BuildForView(Rendering::ViewId viewId,
                                        const WorldDrawBuilder& drawBuilder,
                                        const Rendering::MeshGPURegistry& meshRegistry,
                                        const Rendering::GPUScene& scene, const ViewRegistry& views,
                                        JobSystem::WorkStealingThreadPool* pool)
{
    auto it = m_ByView.find(viewId);
    if (it != m_ByView.end())
    {
        it->second.Indices.clear();
        it->second.Index[0].clear();
        it->second.Index[1].clear();
    }
    BuildView(viewId, drawBuilder, meshRegistry, scene, views, pool);
}

void CpuDrawStreamBuilder::BuildView(Rendering::ViewId viewId, const WorldDrawBuilder& drawBuilder,
                                     const Rendering::MeshGPURegistry& meshRegistry,
                                     const Rendering::GPUScene& scene, const ViewRegistry& views,
                                     JobSystem::WorkStealingThreadPool* pool)
{
    const std::span<const WorldSubmissionRecord> submissions = drawBuilder.GetSubmissions(viewId);
    if (submissions.empty())
        return;

    // Cull camera-relative, in the SAME arithmetic the GPU frustum cull uses
    // (SortedTransparentCull.h documents the equivalence): planes translated by
    // the view's render origin, centres differenced against it. At origin
    // (0,0,0) both steps are bit-for-bit no-ops.
    const Rendering::CameraData cam = views.ResolveCameraData(viewId);
    const Rendering::CameraDerivedData camDerived = Rendering::DeriveCameraData(cam);
    Rendering::Vector4 planes[6];
    Rendering::ExtractFrustumPlanes(camDerived.ViewProjMatrix, planes);
    Rendering::Vector3 origin{0.0f, 0.0f, 0.0f};
    {
        const auto originSector =
            ComputeRenderOriginSector(cam.cameraPos[0], cam.cameraPos[1], cam.cameraPos[2]);
        SectorToWorld(originSector, origin.x, origin.y, origin.z);
    }
    Rendering::MakeFrustumPlanesCameraRelative(planes, origin);

    const std::vector<Rendering::GPUInstance>& instances = scene.GetInstances();
    const size_t instanceCount = instances.size();

    m_Candidates.resize(submissions.size());
    Candidate* candidates = m_Candidates.data();
    const WorldSubmissionRecord* records = submissions.data();

    // Resolve + cull every submission independently: no shared writes, one
    // slot per input, so the fan-out needs no synchronisation and the
    // compaction below stays deterministic regardless of chunk order.
    auto resolveRange = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
        {
            const WorldSubmissionRecord& rec = records[i];
            Candidate& out = candidates[i];
            out.State = kRejected;

            if (!rec.material || !rec.material->GetGraphicsPipelineId().IsValid())
                continue;
            const Rendering::MeshGPUEntry* entry = meshRegistry.Find(rec.meshHandle);
            if (!entry || entry->gpuMeshIndex == ~0u || entry->indexCount == 0u)
                continue;
            if (rec.instanceIndex >= instanceCount)
                continue; // not resident in GPUScene yet (first frame after spawn)

            out.Key = MakeBatchKey(rec.material->GetGpuSceneMaterialIndex(), entry->gpuMeshIndex);
            out.Instance = rec.instanceIndex;

            const Rendering::GPUInstance& inst = instances[rec.instanceIndex];
            const Rendering::Vector3 center(inst.boundingCenter.x - origin.x,
                                            inst.boundingCenter.y - origin.y,
                                            inst.boundingCenter.z - origin.z);
            out.State = kResolvedBit;
            if (Rendering::TestSphereFrustum(center, inst.boundingRadius, planes))
                out.State |= kVisibleBit;
            if ((rec.flags & kSubmissionFlagCastShadows) != 0u)
                out.State |= kCasterBit;
        }
    };
    JobSystem::ParallelFor(pool, 0, submissions.size(), resolveRange, kCullMinBatchSize);

    m_CameraEntries.clear();
    m_ShadowEntries.clear();
    m_CameraEntries.reserve(submissions.size());
    m_ShadowEntries.reserve(submissions.size());
    for (size_t i = 0; i < submissions.size(); ++i)
    {
        const Candidate& c = candidates[i];
        if ((c.State & kResolvedBit) == 0u)
            continue;
        ++m_Stats.Candidates;
        if ((c.State & kVisibleBit) != 0u)
            m_CameraEntries.push_back(KeyedInstance{c.Key, c.Instance});
        if ((c.State & kCasterBit) != 0u)
            m_ShadowEntries.push_back(KeyedInstance{c.Key, c.Instance});
    }
    m_Stats.CameraVisible += static_cast<uint32_t>(m_CameraEntries.size());
    m_Stats.ShadowCasters += static_cast<uint32_t>(m_ShadowEntries.size());

    if (m_CameraEntries.empty() && m_ShadowEntries.empty())
        return;

    ViewLists& viewLists = m_ByView[viewId];
    m_Stats.CameraBatches += EmitLists(viewLists, InstanceSet::Camera, m_CameraEntries, pool);
    m_Stats.ShadowBatches += EmitLists(viewLists, InstanceSet::ShadowCasters, m_ShadowEntries, pool);
}

uint32_t CpuDrawStreamBuilder::EmitLists(ViewLists& view, InstanceSet set,
                                         std::vector<KeyedInstance>& entries,
                                         JobSystem::WorkStealingThreadPool* pool)
{
    if (entries.empty())
        return 0u;

    // Instance order inside a batch is the GPUScene order, so neighbouring
    // vertex-stage invocations read neighbouring GPUInstances.
    const auto byKeyThenInstance = [](const KeyedInstance& a, const KeyedInstance& b) {
        if (a.Key != b.Key)
            return a.Key < b.Key;
        return a.Instance < b.Instance;
    };
    if (entries.size() >= kSortThreshold)
        JobSystem::ParallelSort(pool, entries.begin(), entries.end(), byKeyThenInstance);
    else
        std::sort(entries.begin(), entries.end(), byKeyThenInstance);

    auto& index = view.Index[static_cast<size_t>(set)];
    uint32_t batches = 0;
    size_t keyBegin = 0;
    while (keyBegin < entries.size())
    {
        const uint64_t key = entries[keyBegin].Key;
        const uint32_t first = static_cast<uint32_t>(view.Indices.size());
        view.Indices.push_back(entries[keyBegin].Instance);
        size_t keyEnd = keyBegin + 1;
        for (; keyEnd < entries.size() && entries[keyEnd].Key == key; ++keyEnd)
        {
            // The sort makes duplicates adjacent; an instance submitted twice
            // for one batch would otherwise draw twice.
            if (entries[keyEnd].Instance != entries[keyEnd - 1].Instance)
                view.Indices.push_back(entries[keyEnd].Instance);
        }
        index.emplace(key, InstanceList{first, static_cast<uint32_t>(view.Indices.size()) - first});
        ++batches;
        keyBegin = keyEnd;
    }
    return batches;
}

CpuDrawStreamBuilder::InstanceList CpuDrawStreamBuilder::GetInstances(
    Rendering::ViewId viewId, uint32_t materialIndex, uint32_t meshIndex, InstanceSet set) const
{
    auto itView = m_ByView.find(viewId);
    if (itView == m_ByView.end())
        return {};
    const auto& index = itView->second.Index[static_cast<size_t>(set)];
    auto itKey = index.find(MakeBatchKey(materialIndex, meshIndex));
    if (itKey == index.end())
        return {};
    return itKey->second;
}

std::span<const uint32_t> CpuDrawStreamBuilder::GetIndexList(Rendering::ViewId viewId) const
{
    auto itView = m_ByView.find(viewId);
    if (itView == m_ByView.end())
        return {};
    return itView->second.Indices;
}

} // namespace Engine::Renderer
} // namespace GameEngine
