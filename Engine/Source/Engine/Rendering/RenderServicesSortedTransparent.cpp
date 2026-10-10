// Sorted transparent path (T2/S2): peel order-dependent Blend materials off
// the GPU-driven opaque batch path and draw them back-to-front as
// per-(surface×blend) RUNS — one DrawIndexedIndirectCount per run over a
// GPU-sorted indirect stream.
//
// Per frame per view:
//  - BuildSortedTransparentForView (world node, before the opaque peel):
//    collect order-dependent Blend submissions, frustum-cull them CPU-side
//    against their GPUScene bounding spheres (camera-relative, byte-identical
//    to the GPU cull's test), group survivors into runs (PSO identity ×
//    geometry-pool identity — SortedTransparentRuns.h), and slot-order the
//    runs coarsely back-to-front.
//  - AddSortedTransparentDrainForView (SortedTransparent node, after
//    HZBBuild): upload the records, dispatch the fused
//    gather+key+sort+scatter compute (sorted_transparent_drain.comp — one
//    single-workgroup dispatch, no atomics, no readback), then issue one
//    indirect draw per run through the GE_INSTANCED vertex path, which
//    fetches per-record transform/materialIndex/sectors/custom0/flags via
//    the standard buffer_reference indirection. Views whose visible set
//    exceeds kSortedTransparentSortCapacity take a CPU-sorted twin that
//    writes the identical indirect stream into a host-visible ring slot —
//    same draws, same run layout, CPU key order — so there is NO draw
//    ceiling and NO whole-view fallback at any count.
//
// Scope limits (deliberate, disclosed):
//  - Main-view (WorldRenderNode) pipelines only: reflections, probe faces,
//    and thumbnails keep the pre-T2 unsorted Blend behavior (no vanish — the
//    peel is inert there). Per-view strategy objects are design slice S3.
//  - Records draw LOD0 (MeshGPUEntry's base index range); GPU LOD selection
//    for the sorted stream is a follow-on.
//  - All records draw with the view's base winding (S1 parity): a
//    negative-determinant transparent that is not double-sided culls the
//    wrong face, exactly as it did on the push-constant path.

#include "Engine/Rendering/RenderServices.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/SortedTransparentCull.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
// Mirrors the DrainPC push-constant block in sorted_transparent_drain.comp.
struct SortedTransparentDrainPC
{
    float CameraPos[3];
    uint32_t LiveCount;
    float CameraForward[3];
    uint32_t RunCount;
};
static_assert(sizeof(SortedTransparentDrainPC) == 32,
              "must match the drain shader's push-constant block");

constexpr uint32_t kIndirectCommandStride = 5u * sizeof(uint32_t); // VkDrawIndexedIndirectCommand

// Pack one record's VkDrawIndexedIndirectCommand at sorted position `slot` —
// the CPU twin of the drain shader's scatter phase (keep in lockstep).
inline void WriteIndirectCommand(uint32_t* dst, const SortedTransparentRecord& rec, uint32_t slot)
{
    dst[0] = rec.IndexCount;
    dst[1] = 1u; // instanceCount: one record = one instance
    dst[2] = rec.FirstIndex;
    dst[3] = static_cast<uint32_t>(rec.VertexOffset);
    dst[4] = slot; // firstInstance -> gl_InstanceIndex -> indirection slot
}
} // namespace

void RenderServices::BuildSortedTransparentForView(Rendering::ViewId viewId)
{
    const uint64_t frame = m_Device ? m_Device->GetFrameIndex() : 0;
    SortedTransparentView& holder = m_SortedTransparentByView[viewId];
    if (holder.Frame == frame)
        return; // already built this frame

    // Captured before the reset: a false->true edge (the path engaging after
    // any build that staged nothing) is a loggable transition even when the
    // composition matches the last one logged.
    const bool stagedLastBuild = holder.StagedLastBuild;

    holder.Frame = frame;
    holder.Active = false;
    holder.StagedLastBuild = false;
    holder.LoggedBuildTransition = false;
    holder.Records.clear();
    holder.Runs.clear();
    holder.Partition = {};
    holder.Commands.clear();
    holder.Stats = {};

    // Empty-set early-out: a view with no submissions pays nothing.
    const auto submissions = m_WorldDrawBuilder.GetSubmissions(viewId);
    if (submissions.empty())
        return;

    Rendering::GPUScene* scene = GetGPUScene();
    if (!scene)
        return;
    // The drain draws through the GE_INSTANCED buffer-reference path; without
    // device addresses the whole GPU-driven pipeline is inert and Blend stays
    // on the (unsorted) batched path, matching the rest of the frame.
    if (!m_Device || !m_Device->GetCapabilities().supportsBufferDeviceAddress)
        return;
    const std::vector<Rendering::GPUInstance>& instances = scene->GetInstances();
    const auto& meshRegistry = GetMeshGPURegistry();

    // Collect ORDER-DEPENDENT Blend instances. Transmission glass is coerced
    // to Opaque alpha mode (MaterialRegistry), so filtering on Blend naturally
    // leaves the transmissive pass untouched; order-independent blends
    // (additive/multiply) stay on the batched path. The filter MUST match the
    // peel predicate in recordEntityBatch and the extraction class bit exactly
    // (Risk 3) — all three use IsOrderDependentBlendMaterial. Candidate Key
    // indexes `pending`.
    struct PendingRecord
    {
        uint32_t InstanceIndex;
        const Material* Mat;
        Rendering::MeshGPUHandle Mesh;
        const Rendering::MeshGPUEntry* Entry;
    };
    std::vector<PendingRecord> pending;
    std::vector<TransparentSphere> candidates;
    for (const WorldSubmissionRecord& sub : submissions)
    {
        if (!sub.material || !IsOrderDependentBlendMaterial(*sub.material))
            continue;
        if (sub.instanceIndex >= instances.size())
            continue; // not resident in GPUScene yet (first frame)
        // Mesh entry must be resident to stage draw args; a not-yet-uploaded
        // mesh is skipped exactly as the batched path skips it (recordEntityBatch's
        // mesh-entry-missing guard) — a one-frame absence, not a vanish.
        const Rendering::MeshGPUEntry* entry = meshRegistry.Find(sub.meshHandle);
        if (!entry || entry->indexCount == 0)
            continue;
        const Rendering::GPUInstance& inst = instances[sub.instanceIndex];
        candidates.push_back({static_cast<uint32_t>(pending.size()),
                              {inst.boundingCenter.x, inst.boundingCenter.y, inst.boundingCenter.z},
                              inst.boundingRadius});
        pending.push_back({sub.instanceIndex, sub.material, sub.meshHandle, entry});
    }
    if (candidates.empty())
        return;

    // Resolve the view frustum ONCE, in the SAME world space as GPUScene's
    // boundingCenter, then cull camera-relative: the planes and centres are
    // both rebased against the view's render origin — the EXACT arithmetic the
    // GPU cull performs (RenderServicesGpuDriven fills ViewCullingContext the
    // same way) — so sector-tagged (planetary) transparents cull correctly and
    // an instance moving between the batched and sorted paths never flips its
    // cull decision. Origin (0,0,0) makes every rebase a bit-for-bit no-op.
    const Rendering::CameraData cam = m_ViewRegistry.ResolveCameraData(viewId);
    const Rendering::CameraDerivedData camDerived = Rendering::DeriveCameraData(cam);
    Vector4 frustumPlanes[6];
    Rendering::ExtractFrustumPlanes(camDerived.ViewProjMatrix, frustumPlanes);
    Rendering::Vector3 origin{0.0f, 0.0f, 0.0f};
    {
        const auto originSector =
            ComputeRenderOriginSector(cam.cameraPos[0], cam.cameraPos[1], cam.cameraPos[2]);
        SectorToWorld(originSector, origin.x, origin.y, origin.z);
    }

    std::vector<TransparentCentroid> visible;
    visible.reserve(candidates.size());
    const uint32_t culledCount =
        CullTransparentsToFrustum(candidates, frustumPlanes, origin, visible);
    if (visible.empty())
        return; // everything off-screen; the batched path GPU-culls it identically

    // Stage records + discover runs. A run groups records whose PSO identity
    // (SortedTransparentRunMaterialKey) AND geometry-pool identity
    // (SortedTransparentRunGeometryKey) match — one representative then binds
    // for the whole run, per-record variance riding the bindless
    // materialIndex/instance fetch. Group count is small (ER: a handful), so
    // linear key scans beat any map.
    struct RunKeyEntry
    {
        SortedTransparentRunMaterialKey MatKey;
        SortedTransparentRunGeometryKey GeoKey;
    };
    std::vector<RunKeyEntry> runKeys;
    // Per-unique-material key cache (materials repeat heavily across records).
    std::vector<std::pair<const Material*, SortedTransparentRunMaterialKey>> matKeyCache;

    holder.Records.reserve(visible.size());
    const Rendering::Vector3 camPos = camDerived.Position;
    const Rendering::Vector3 camFwd = camDerived.Forward;
    for (const TransparentCentroid& vis : visible)
    {
        const PendingRecord& p = pending[vis.Key];

        const SortedTransparentRunMaterialKey* matKey = nullptr;
        for (const auto& [mat, key] : matKeyCache)
        {
            if (mat == p.Mat)
            {
                matKey = &key;
                break;
            }
        }
        if (!matKey)
        {
            matKeyCache.emplace_back(p.Mat, MakeSortedTransparentRunMaterialKey(*p.Mat));
            matKey = &matKeyCache.back().second;
        }

        SortedTransparentRunGeometryKey geoKey{};
        // Refused before the run key is built, never after: a run key made from
        // all-invalid bindings would compare EQUAL across two different
        // non-drawable meshes and collapse them into one run.
        if (!meshRegistry.TryGetDrawableBindings(*p.Entry, geoKey.Bindings))
            continue;
        geoKey.Topology = p.Entry->topology;
        geoKey.VertexFlags = p.Entry->vertexFlags;

        uint32_t group = static_cast<uint32_t>(runKeys.size());
        for (uint32_t g = 0; g < runKeys.size(); ++g)
        {
            if (runKeys[g].MatKey == *matKey && runKeys[g].GeoKey == geoKey)
            {
                group = g;
                break;
            }
        }
        if (group == runKeys.size())
        {
            runKeys.push_back({*matKey, geoKey});
            holder.Runs.push_back({p.Mat, p.Mesh});
        }

        const float viewDepth = (vis.Center[0] - camPos.x) * camFwd.x +
                                (vis.Center[1] - camPos.y) * camFwd.y +
                                (vis.Center[2] - camPos.z) * camFwd.z;
        holder.Records.push_back({p.InstanceIndex, p.Entry->indexCount, p.Entry->firstIndex,
                                  static_cast<int32_t>(p.Entry->vertexOffset), group, viewDepth});
    }

    holder.Partition = BuildSortedTransparentRunPartition(
        holder.Records, static_cast<uint32_t>(holder.Runs.size()));
    holder.CameraPos[0] = camPos.x;
    holder.CameraPos[1] = camPos.y;
    holder.CameraPos[2] = camPos.z;
    holder.CameraForward[0] = camFwd.x;
    holder.CameraForward[1] = camFwd.y;
    holder.CameraForward[2] = camFwd.z;

    holder.Stats.Collected = static_cast<uint32_t>(candidates.size());
    holder.Stats.Culled = culledCount;
    holder.Stats.Records = static_cast<uint32_t>(holder.Records.size());
    holder.Stats.Runs = static_cast<uint32_t>(holder.Runs.size());
    holder.Stats.CpuSorted = holder.Records.size() > kSortedTransparentSortCapacity;
    holder.Active = true;
    holder.StagedLastBuild = true;

    // Log on TRANSITION only — engagement or a composition change — never per
    // steady-state frame; that convention is what keeps the editor console
    // (Debug level) readable with Blend water/glass permanently on screen.
    // The level check first so shipped builds (GlobalMinLevel=Info) and
    // steady-state frames both skip every byte of string work below.
    if (Logger::ShouldLog(Logger::LogLevel::Debug, Logger::Log::GetLogLevel()) &&
        (!stagedLastBuild || holder.Stats != holder.LastLoggedStats))
    {
        holder.LastLoggedStats = holder.Stats;
        holder.LoggedBuildTransition = true;

        // The collected / culled / staged / run composition that shows how the
        // sorted path engaged.
        Logger::Log::Debug(
            "SortedTransparent: view {} collected {} order-dependent Blend candidates, "
            "culled {} (frustum), staged {} records in {} runs ({} sort).",
            static_cast<uint32_t>(viewId), holder.Stats.Collected, holder.Stats.Culled,
            holder.Stats.Records, holder.Stats.Runs, holder.Stats.CpuSorted ? "CPU" : "GPU");

        // Per-run [min,max] view-depth composition (R2-8): the coarse cross-run
        // order is farthest-member-first, so overlapping run depth ranges are the
        // one place run-granular compositing can differ from an exact global sort.
        // Without this line a field report of wrong-order glass is undiagnosable —
        // it rides the composition line's gate so it appears at every transition.
        const uint32_t slotCount = static_cast<uint32_t>(holder.Partition.RunCount.size());
        std::vector<float> minDepth(slotCount, std::numeric_limits<float>::max());
        std::vector<float> maxDepth(slotCount, std::numeric_limits<float>::lowest());
        for (const SortedTransparentRecord& rec : holder.Records)
        {
            const uint32_t slot = holder.Partition.SlotOfGroup[rec.RunGroup];
            minDepth[slot] = std::min(minDepth[slot], rec.ViewDepth);
            maxDepth[slot] = std::max(maxDepth[slot], rec.ViewDepth);
        }
        std::string runRanges;
        for (uint32_t slot = 0; slot < slotCount; ++slot)
        {
            runRanges += slot ? " r" : "r";
            runRanges += std::to_string(slot) + "[" + std::to_string(minDepth[slot]) + "," +
                         std::to_string(maxDepth[slot]) + "]x" +
                         std::to_string(holder.Partition.RunCount[slot]);
        }
        Logger::Log::Debug("SortedTransparent: view {} run depth ranges: {}",
                           static_cast<uint32_t>(viewId), runRanges);
    }
}

bool RenderServices::IsSortedTransparentActive(Rendering::ViewId viewId) const
{
    const uint64_t frame = m_Device ? m_Device->GetFrameIndex() : 0;
    const auto it = m_SortedTransparentByView.find(viewId);
    // Guard on the build frame so a stale Active never leaks to a later frame or to
    // a view the world node did not (re)build this frame.
    return it != m_SortedTransparentByView.end() && it->second.Frame == frame && it->second.Active;
}

bool RenderServices::EnsureSortedTransparentDrainPipeline()
{
    if (m_SortedTransparentDrainPipeline.IsValid())
        return true;
    // m_SortedTransparentDrainAttempted latches only DEFINITIVE failures (bad
    // SPIR-V rejected at intern). Package-load failures are treated as
    // transient (hot-reload can catch the pkg mid-write) and retried on a
    // frame backoff instead of sticking the drain on the CPU sorter for the
    // whole session (R1-F4).
    if (m_SortedTransparentDrainAttempted || !m_Device)
        return false;
    const uint64_t frame = m_Device->GetFrameIndex();
    if (frame < m_SortedTransparentDrainRetryFrame)
        return false;

    ShaderPackage pkg{};
    std::string err;
    if (!LoadShaderPkg("Shaders/sorted_transparent_drain.shaderpkg", m_Device->PreferredShaderSource(), pkg, &err))
    {
        m_SortedTransparentDrainRetryFrame = frame + kSortedTransparentDrainRetryInterval;
        Logger::Log::Warning(
            "SortedTransparent: failed to load sorted_transparent_drain.shaderpkg ({}); "
            "the drain will CPU-sort and retry in {} frames.",
            err, kSortedTransparentDrainRetryInterval);
        return false;
    }
    const auto itCs = pkg.stageBytes.find("cs");
    if (itCs == pkg.stageBytes.end() || itCs->second.empty())
    {
        m_SortedTransparentDrainRetryFrame = frame + kSortedTransparentDrainRetryInterval;
        Logger::Log::Warning(
            "SortedTransparent: sorted_transparent_drain.shaderpkg has no cs stage; "
            "the drain will CPU-sort and retry in {} frames.",
            kSortedTransparentDrainRetryInterval);
        return false;
    }

    m_SortedTransparentDrainLayout = {};
    m_SortedTransparentDrainLayout.debugName = "SortedTransparentDrain_SetLayout";
    const char* bindingNames[6] = {"Records", "Instances", "DrawArgs",
                                   "DrawCounts", "Indirection", "RunCounts"};
    for (uint32_t i = 0; i < 6u; ++i)
    {
        DescriptorBinding b{};
        b.binding = i;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1u;
        b.shaderStages = kShaderStageCompute;
        b.debugName = bindingNames[i];
        m_SortedTransparentDrainLayout.bindings.push_back(b);
    }

    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(itCs->second);
    cd.DescriptorSetLayouts.push_back(
        m_Device->InternDescriptorSetLayout(m_SortedTransparentDrainLayout));
    cd.PushConstants.Size = sizeof(SortedTransparentDrainPC);
    cd.PushConstants.StageMask = kShaderStageCompute;
    cd.DebugName = "SortedTransparentDrain";
    m_SortedTransparentDrainPipeline = m_Device->InternComputePipeline(cd);
    if (!m_SortedTransparentDrainPipeline.IsValid())
    {
        // Definitive: the loaded SPIR-V was rejected at intern — retrying the
        // same bytes cannot succeed, so latch off for the session (R1-F4).
        m_SortedTransparentDrainAttempted = true;
        Logger::Log::Warning(
            "SortedTransparent: sorted_transparent_drain compute pipeline intern failed; "
            "the drain will CPU-sort for the rest of the session.");
        return false;
    }
    return true;
}

void RenderServices::AddSortedTransparentDrainForView(Rendering::RenderGraph::RGFrame& frame,
                                                      Rendering::ViewId viewId,
                                                      const WorldPassTargetsRG& targets,
                                                      Rendering::MaterialKeyword passKeywords)
{
    namespace RenderGraph = Rendering::RenderGraph;

    const uint64_t deviceFrame = m_Device ? m_Device->GetFrameIndex() : 0;
    const auto it = m_SortedTransparentByView.find(viewId);
    if (it == m_SortedTransparentByView.end() || it->second.Frame != deviceFrame ||
        !it->second.Active)
        return;
    SortedTransparentView& holder = it->second;

    // Any declare-time bail below MUST hand the view's Blend draws back to the
    // batched path: Active is read by the opaque peel at EXEC time (after this
    // declaration), so flipping it here restores the unsorted-but-visible
    // batched draws in the SAME frame — a bail with Active left true would
    // vanish every sorted transparent silently (S2 review R1-F2). Warn once
    // per episode (the flag clears on the next successful drain), matching
    // S1's never-silent fallback contract.
    auto bailToBatched = [&](const char* reason)
    {
        holder.Active = false;
        if (!holder.WarnedDrainBail)
        {
            holder.WarnedDrainBail = true;
            Logger::Log::Warning(
                "SortedTransparent: view {} drain could not declare ({}); Blend draws "
                "fall back to the unsorted batched path until it recovers.",
                static_cast<uint32_t>(viewId), reason);
        }
    };

    if (holder.Records.empty() || !m_Device)
    {
        bailToBatched(!m_Device ? "no device" : "no staged records");
        return;
    }
    Rendering::GPUScene* scene = GetGPUScene();
    if (!scene || !scene->GetInstanceBuffer().IsValid())
    {
        bailToBatched("GPUScene instance buffer unavailable");
        return;
    }

    const uint32_t liveCount = static_cast<uint32_t>(holder.Records.size());
    const uint32_t runCount = static_cast<uint32_t>(holder.Runs.size());
    const bool gpuSort =
        liveCount <= kSortedTransparentSortCapacity && EnsureSortedTransparentDrainPipeline();
    holder.Stats.CpuSorted = !gpuSort; // the build's capacity guess, made exact

    Rendering::BufferHandle argsBuffer{};
    Rendering::BufferHandle countsBuffer{};
    size_t argsBase = 0;
    size_t countsBase = 0;
    uint64_t indirectionAddr = 0;
    // GPU-written buffers the draw pass must order after the drain dispatch.
    // Empty on the CPU path (host writes are visible at submit; no producer
    // pass exists to order against).
    std::vector<ForwardBufferReadRG> drawReads;

    if (gpuSort)
    {
        // Device-local per-view stream buffers from the persistent pool: fixed
        // capacity so the desc never changes (no realloc churn), eager physical
        // (the DrawCommands below need handles at declare), pool-managed
        // lifetime + device-rebuild reprovision for free.
        const std::string viewSuffix = ".View" + std::to_string(static_cast<uint32_t>(viewId));
        auto importStream = [&](const char* what, uint64_t bytes, uint32_t usage,
                                RenderGraph::RGBuffer& outRG, Rendering::BufferHandle& outPhys)
        {
            BufferDesc desc{};
            desc.size = bytes;
            desc.usage = usage;
            desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
            const std::string name = std::string("SortedTransparent.") + what + viewSuffix;
            desc.debugName = name.c_str();
            bool needsZeroInit = false;
            outRG = frame.ImportPersistentBuffer(name.c_str(), desc, &needsZeroInit);
            if (!outRG.IsValid())
                return false;
            // Hygiene only — every byte a draw can read is written by the drain
            // dispatch this frame (counts clamp the tail off) — but a fresh
            // physical is recycled pool memory and clean captures beat stale
            // garbage.
            if (needsZeroInit)
                frame.AddBufferZeroInit(outRG, "SortedTransparent.ZeroInit");
            outPhys = frame.PhysicalBuffer(outRG);
            return outPhys.IsValid();
        };

        RenderGraph::RGBuffer argsRG{};
        RenderGraph::RGBuffer countsRG{};
        RenderGraph::RGBuffer indirectionRG{};
        Rendering::BufferHandle indirectionBuffer{};
        const uint32_t streamUsage = static_cast<uint32_t>(BufferUsage::Storage) |
                                     static_cast<uint32_t>(BufferUsage::Indirect) |
                                     static_cast<uint32_t>(BufferUsage::TransferDst);
        const uint32_t indirectionUsage = static_cast<uint32_t>(BufferUsage::Storage) |
                                          static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress) |
                                          static_cast<uint32_t>(BufferUsage::TransferDst);
        if (!importStream("Args", kSortedTransparentSortCapacity * kIndirectCommandStride,
                          streamUsage, argsRG, argsBuffer) ||
            !importStream("Counts", kSortedTransparentSortCapacity * sizeof(uint32_t),
                          streamUsage, countsRG, countsBuffer) ||
            !importStream("Indirection", kSortedTransparentSortCapacity * sizeof(uint32_t),
                          indirectionUsage, indirectionRG, indirectionBuffer))
        {
            bailToBatched("stream buffer import failed");
            return;
        }
        indirectionAddr = m_Device->GetBufferDeviceAddress(indirectionBuffer);
        if (indirectionAddr == 0)
        {
            bailToBatched("indirection buffer has no device address");
            return;
        }

        // CPU-written inputs ride the frame upload ring (Storage usage, per-
        // frame rotation — no lifetime management here).
        const auto recordsAlloc =
            frame.AllocUpload(liveCount * sizeof(SortedTransparentRecordGPU));
        const auto runCountsAlloc = frame.AllocUpload(runCount * sizeof(uint32_t));
        if (!recordsAlloc.Valid() || !runCountsAlloc.Valid())
        {
            bailToBatched("upload ring allocation failed");
            return;
        }
        auto* recordsGPU = static_cast<SortedTransparentRecordGPU*>(recordsAlloc.Ptr);
        for (uint32_t i = 0; i < liveCount; ++i)
        {
            const SortedTransparentRecord& rec = holder.Records[i];
            recordsGPU[i] = {rec.InstanceIndex, rec.IndexCount, rec.FirstIndex, rec.VertexOffset,
                             holder.Partition.SlotOfGroup[rec.RunGroup]};
        }
        std::memcpy(runCountsAlloc.Ptr, holder.Partition.RunCount.data(),
                    runCount * sizeof(uint32_t));

        SortedTransparentDrainPC pc{};
        pc.CameraPos[0] = holder.CameraPos[0];
        pc.CameraPos[1] = holder.CameraPos[1];
        pc.CameraPos[2] = holder.CameraPos[2];
        pc.LiveCount = liveCount;
        pc.CameraForward[0] = holder.CameraForward[0];
        pc.CameraForward[1] = holder.CameraForward[1];
        pc.CameraForward[2] = holder.CameraForward[2];
        pc.RunCount = runCount;

        const auto sceneRG = scene->ImportFrameResources(frame);
        const Rendering::BufferHandle instanceBuffer = scene->GetInstanceBuffer();
        const std::string passName =
            "SortedTransparentDrain[View#" + std::to_string(static_cast<uint32_t>(viewId)) + "]";

        frame.AddComputePass(
            passName.c_str(), Rendering::PassPhase::kWorldRender,
            [&](RenderGraph::RGPassBuilder& p)
            {
                if (sceneRG.Instances.IsValid())
                    p.Read(sceneRG.Instances, RenderGraph::RGBufferRead::Storage);
                p.Write(argsRG, RenderGraph::RGBufferWrite::Storage);
                p.Write(countsRG, RenderGraph::RGBufferWrite::Storage);
                p.Write(indirectionRG, RenderGraph::RGBufferWrite::Storage);
            },
            [this, recordsAlloc, runCountsAlloc, argsBuffer, countsBuffer, indirectionBuffer,
             instanceBuffer, pc, liveCount, runCount](RenderGraph::RGContext& ctx)
            {
                auto* dev = ctx.GetDevice();
                auto* cl = ctx.Cmd;
                if (!dev || !cl)
                    return;
                const PipelineHandle pipe =
                    ctx.GetOrCreatePipelineVariant(m_SortedTransparentDrainPipeline);
                if (!pipe.IsValid())
                    return;

                DescriptorSetDesc dsDesc{};
                dsDesc.layout = m_SortedTransparentDrainLayout;
                dsDesc.transient = true;
                dsDesc.debugName = "SortedTransparentDrain.Set0";
                const DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);
                if (!ds.IsValid())
                    return;
                dev->UpdateStorageBufferBinding(ds, 0, recordsAlloc.Buffer, recordsAlloc.Offset,
                                                liveCount * sizeof(SortedTransparentRecordGPU));
                dev->UpdateStorageBufferBinding(ds, 1, instanceBuffer, 0, 0);
                dev->UpdateStorageBufferBinding(ds, 2, argsBuffer, 0, 0);
                dev->UpdateStorageBufferBinding(ds, 3, countsBuffer, 0, 0);
                dev->UpdateStorageBufferBinding(ds, 4, indirectionBuffer, 0, 0);
                dev->UpdateStorageBufferBinding(ds, 5, runCountsAlloc.Buffer,
                                                runCountsAlloc.Offset,
                                                runCount * sizeof(uint32_t));

                cl->SetPipeline(pipe);
                cl->BindDescriptorSet(0, ds, pipe);
                cl->SetPushConstants(pc);
                // Exactly ONE workgroup (F6): the fused sort's shared-memory
                // picture covers the whole capacity; a second workgroup would
                // race it.
                cl->Dispatch(1, 1, 1);
            });

        drawReads.push_back({argsRG, RenderGraph::RGBufferRead::Indirect});
        drawReads.push_back({countsRG, RenderGraph::RGBufferRead::Indirect});
        drawReads.push_back({indirectionRG, RenderGraph::RGBufferRead::Storage});
    }
    else
    {
        // CPU-sorted twin: identical indirect stream (same run layout, same
        // key order — BuildSortedTransparentCpuOrder mirrors the GPU key
        // bit-exactly), written into a host-visible per-frame-in-flight ring
        // slot. Engages past the GPU sort capacity (or if the drain shader
        // failed to load), so removing the old ceiling never drops or
        // reorders a draw — only the sorter moves.
        const std::vector<uint32_t> order =
            BuildSortedTransparentCpuOrder(holder.Records, holder.Partition);

        const uint64_t argsBytes = static_cast<uint64_t>(liveCount) * kIndirectCommandStride;
        const uint64_t countsBytes = static_cast<uint64_t>(runCount) * sizeof(uint32_t);
        const uint64_t indirectionBytes = static_cast<uint64_t>(liveCount) * sizeof(uint32_t);
        const uint64_t neededBytes = argsBytes + countsBytes + indirectionBytes;

        const uint32_t slot = static_cast<uint32_t>(deviceFrame);
        if (!holder.CpuDrainBuffers[slot].IsValid() ||
            holder.CpuDrainCapacityBytes[slot] < neededBytes)
        {
            if (holder.CpuDrainBuffers[slot].IsValid())
                m_Device->DestroyBuffer(holder.CpuDrainBuffers[slot]); // deferred by the device
            const uint64_t capacity =
                std::max<uint64_t>(neededBytes, holder.CpuDrainCapacityBytes[slot] * 2u);
            BufferDesc desc{};
            desc.size = capacity;
            desc.usage = static_cast<uint32_t>(BufferUsage::Storage) |
                         static_cast<uint32_t>(BufferUsage::Indirect) |
                         static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress);
            desc.memoryUsage = BufferMemoryUsage::Upload;
            // FrameSlotted: one element of a ring exactly as deep as the device paces.
            // Written only from AddSortedTransparentDrainForView, which runs behind
            // BeginFrame; a write from anywhere earlier would race a frame in flight.
            desc.flags = BufferCreateFlags::FrameSlotted;
            const std::string name =
                "SortedTransparent.CpuDrain.View" + std::to_string(static_cast<uint32_t>(viewId)) +
                ".Slot" + std::to_string(slot);
            desc.debugName = name.c_str();
            holder.CpuDrainBuffers[slot] = m_Device->CreateBuffer(desc);
            holder.CpuDrainCapacityBytes[slot] = holder.CpuDrainBuffers[slot].IsValid() ? capacity : 0;
        }
        if (!holder.CpuDrainBuffers[slot].IsValid())
        {
            bailToBatched("CPU drain buffer creation failed");
            return;
        }

        auto* mapped = static_cast<uint8_t*>(m_Device->MapBuffer(holder.CpuDrainBuffers[slot]));
        if (!mapped)
        {
            bailToBatched("CPU drain buffer map failed");
            return;
        }
        auto* args = reinterpret_cast<uint32_t*>(mapped);
        auto* counts = reinterpret_cast<uint32_t*>(mapped + argsBytes);
        auto* indirection = reinterpret_cast<uint32_t*>(mapped + argsBytes + countsBytes);
        for (uint32_t pos = 0; pos < liveCount; ++pos)
        {
            const SortedTransparentRecord& rec = holder.Records[order[pos]];
            WriteIndirectCommand(args + pos * 5u, rec, pos);
            indirection[pos] = rec.InstanceIndex;
        }
        for (uint32_t r = 0; r < runCount; ++r)
            counts[r] = holder.Partition.RunCount[r];
        m_Device->UnmapBuffer(holder.CpuDrainBuffers[slot]);

        argsBuffer = holder.CpuDrainBuffers[slot];
        countsBuffer = holder.CpuDrainBuffers[slot];
        argsBase = 0;
        countsBase = argsBytes;
        indirectionAddr = m_Device->GetBufferDeviceAddress(holder.CpuDrainBuffers[slot]);
        if (indirectionAddr == 0)
        {
            bailToBatched("CPU drain buffer has no device address");
            return;
        }
        indirectionAddr += argsBytes + countsBytes;
    }

    // Stage one indirect draw per run, in slot order (coarse back-to-front).
    // All runs share the 16-byte GE_INSTANCED push-constant block: the drain's
    // indirection buffer + GPUScene's per-frame instance buffer, both by
    // device address.
    ComposedPCInstanced pcInst{};
    pcInst.indirectionAddr = indirectionAddr;
    pcInst.gpuInstancesAddr = m_Device->GetBufferDeviceAddress(scene->GetInstanceBuffer());
    if (pcInst.gpuInstancesAddr == 0)
    {
        bailToBatched("GPUScene instance buffer has no device address");
        return;
    }
    holder.PcBytes.resize(sizeof(ComposedPCInstanced));
    std::memcpy(holder.PcBytes.data(), &pcInst, sizeof(ComposedPCInstanced));

    holder.Commands.clear();
    holder.Commands.reserve(runCount);
    for (uint32_t slot = 0; slot < runCount; ++slot)
    {
        const SortedTransparentView::Run& run = holder.Runs[holder.Partition.GroupOfSlot[slot]];
        DrawCommand cmd{};
        cmd.Material = run.Representative;
        cmd.Geometry.Mesh = run.Mesh;
        cmd.UseIndirect = true;
        cmd.IndirectCommandBuffer = argsBuffer;
        cmd.IndirectCountBuffer = countsBuffer;
        cmd.IndirectMaxDrawCount = holder.Partition.RunCount[slot];
        cmd.IndirectStride = kIndirectCommandStride;
        cmd.IndirectCommandOffset =
            argsBase + static_cast<size_t>(holder.Partition.RunStart[slot]) * kIndirectCommandStride;
        cmd.IndirectCountOffset = countsBase + static_cast<size_t>(slot) * sizeof(uint32_t);
        cmd.Bindings.PushConstants =
            std::span<const std::byte>(holder.PcBytes.data(), holder.PcBytes.size());
        holder.Commands.push_back(cmd);
    }

    // Captured before the clear: recovery from a bail episode is a transition
    // the drain line announces (the episode's start already warned once).
    const bool recoveredFromBail = holder.WarnedDrainBail;
    holder.WarnedDrainBail = false; // successful declare ends any bail episode

    // Same log-on-transition convention as the build's composition lines:
    // fire when the drain composition changes (run/record counts, the exact
    // GPU-vs-CPU sort decision), when the build line fired this frame (the
    // pair reads together), or on bail recovery. Steady state logs nothing.
    if (Logger::ShouldLog(Logger::LogLevel::Debug, Logger::Log::GetLogLevel()) &&
        (holder.LoggedBuildTransition || recoveredFromBail ||
         runCount != holder.LastLoggedDrain.Runs || liveCount != holder.LastLoggedDrain.Records ||
         gpuSort != holder.LastLoggedDrain.GpuSort))
    {
        holder.LastLoggedDrain = {runCount, liveCount, gpuSort};
        Logger::Log::Debug(
            "SortedTransparent: view {} drain issued {} run draws over {} records ({} sort).",
            static_cast<uint32_t>(viewId), runCount, liveCount, gpuSort ? "GPU" : "CPU");
    }

    AddForwardCommandPassForView(frame, viewId, targets, passKeywords, holder.Commands, {},
                                 "SortedTransparent", drawReads);
}

} // namespace Engine::Renderer
} // namespace GameEngine
