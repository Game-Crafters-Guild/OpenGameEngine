#include "CBTTerrain/CBTInstance.h"

#include "CBTTerrain/CBTKernelSet.h"
#include "CBTTerrain/CBTSphereRoots.h"
#include "CBTTerrain/SphereSculptLayer.h" // ResolveSculptPagePoolCount

#include "Rendering/Core/CommandList.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace GameEngine::CBTTerrain
{

using namespace GameEngine::Rendering;

namespace
{
uint32_t CeilDiv(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }

// Readback layout: indirect-draw records first, validation counters after.
constexpr uint64_t kReadbackDrawOffset = 0;
constexpr uint64_t kReadbackDrawBytes = static_cast<uint64_t>(kIndirectDrawWords) * 4u;
constexpr uint64_t kReadbackValidationOffset = kReadbackDrawBytes;
constexpr uint64_t kReadbackValidationBytes = static_cast<uint64_t>(kValidationWords) * 4u;
// Depth-target passes RefineUniform runs past one per level.
constexpr uint32_t kRefineUniformExtraPasses = 2u;
} // namespace

bool CBTInstance::Initialize(IDevice& device, const CBTKernelSet& kernelSet, uint32_t poolSize)
{
    Shutdown();
    m_Device = &device;
    m_KernelSet = &kernelSet;
    m_PoolSize = poolSize;
    if (!m_Resources.Initialize(device, kernelSet, poolSize))
        return false;
    m_DataBarriers.reserve(kCBTBindingCount - 1u);
    for (uint32_t i = 0; i < kCBTBindingCount; ++i)
    {
        if (static_cast<CBTBinding>(i) != CBTBinding::IndirectDispatch)
            m_DataBarriers.push_back(ResourceBarrier::CreateBufferBarrier(
                m_Resources.GetBuffer(static_cast<CBTBinding>(i)),
                ResourceState::UnorderedAccess, ResourceState::UnorderedAccess));
    }
    m_Ready = true;
    return true;
}

void CBTInstance::Shutdown()
{
    m_Resources.Shutdown();
    m_Device = nullptr;
    m_KernelSet = nullptr;
    m_PoolSize = 0;
    m_DispatchInIndirectState = false;
    m_Ready = false;
    m_DomainMode = kDomainPlanar;
    m_BaseDepth = kDefaultBaseDepth;
    m_RootCount = kRootHalfedgeCount;
    m_DrawResourcesInitialized = false;
    m_VerticesInitialized = false;
    m_DataBarriers.clear();
}

void CBTInstance::ReprovisionAfterDeviceRebuild()
{
    m_Resources.ReprovisionAfterDeviceRebuild();
    m_Device = nullptr;
    m_KernelSet = nullptr;
    m_PoolSize = 0;
    m_DispatchInIndirectState = false;
    m_Ready = false;
    m_DomainMode = kDomainPlanar;
    m_BaseDepth = kDefaultBaseDepth;
    m_RootCount = kRootHalfedgeCount;
    m_DrawResourcesInitialized = false;
    m_VerticesInitialized = false;
    m_DataBarriers.clear();
}

bool CBTInstance::InitializeRoots(uint32_t domainMode)
{
    if (!m_Ready)
        return false;

    const bool firstDrawResourceInit = !m_DrawResourcesInitialized;

    // A live re-seed can replace sculpt-ring descriptors as well as topology. Retire their
    // graphics users before rebinding; unrelated compute/transfer work does not need a drain.
    if (!firstDrawResourceInit)
        m_Device->WaitGpuSyncToken(m_Device->LastGraphicsSubmissionToken());

    // The sculpt rings follow the domain: full rings for a sphere, placeholders for a planar tree.
    // Before the domain fields change: a refused allocation leaves the instance on its previous
    // domain, tree and rings.
    if (!m_Resources.ProvisionSculptRings(domainMode == kDomainSpherical, ResolveSculptPagePoolCount()))
        return false;

    m_DomainMode = domainMode;
    m_BaseDepth = (domainMode == kDomainSpherical) ? kSphereBaseDepth : kDefaultBaseDepth;
    m_RootCount = (domainMode == kDomainSpherical) ? kSphereRootCount : kRootHalfedgeCount;
    const uint32_t rootCount = m_RootCount;
    const uint32_t baseDepth = m_BaseDepth;

    std::vector<uint64_t> heap(rootCount);
    std::vector<CBTNeighbors> neighbors(rootCount);
    std::vector<CBTBisectorData> bisectors(rootCount);
    std::vector<uint32_t> rootIndices(rootCount);
    if (domainMode == kDomainSpherical)
    {
        const auto roots = BuildSphereRoots();
        for (uint32_t i = 0; i < rootCount; ++i)
        {
            heap[i] = roots[i].HeapID;
            neighbors[i] = roots[i].Neighbors;
        }
    }
    else
    {
        heap[0] = (uint64_t(1) << baseDepth);
        heap[1] = heap[0] + 1u;
        neighbors[0] = CBTNeighbors{kInvalidPointer, kInvalidPointer, 1u, 0u};
        neighbors[1] = CBTNeighbors{kInvalidPointer, kInvalidPointer, 0u, 0u};
    }
    for (uint32_t i = 0; i < rootCount; ++i)
    {
        bisectors[i].Flags = kFlagVisible;
        rootIndices[i] = i;
    }
    const uint32_t rootBits = (1u << rootCount) - 1u;

    // Pack all host data into one upload. Device-local UpdateBuffer calls each submit a transfer
    // command list; recording the copies beside initialization avoids those extra submissions and
    // the intermediate zero-fill wait. Copy ranges and the clear ranges below never overlap.
    struct InitCopy
    {
        BufferHandle Buffer;
        size_t Offset;
        size_t Bytes;
        ResourceState FinalState;
    };
    std::vector<uint32_t> uploadWords;
    const uint32_t identityCount = firstDrawResourceInit ? IdentityIndexCount(m_PoolSize) : 0u;
    uploadWords.reserve(identityCount + rootCount * 16u + 2u);
    std::vector<InitCopy> copies;
    auto append = [&](BufferHandle buffer, const void* data, size_t bytes,
                      ResourceState finalState = ResourceState::UnorderedAccess) {
        const size_t offset = uploadWords.size() * sizeof(uint32_t);
        uploadWords.resize(uploadWords.size() + bytes / sizeof(uint32_t));
        std::memcpy(reinterpret_cast<uint8_t*>(uploadWords.data()) + offset, data, bytes);
        copies.push_back({buffer, offset, bytes, finalState});
    };
    append(m_Resources.GetBuffer(CBTBinding::HeapID), heap.data(), heap.size() * sizeof(uint64_t));
    append(m_Resources.GetBuffer(CBTBinding::NeighborsA), neighbors.data(),
           neighbors.size() * sizeof(CBTNeighbors));
    append(m_Resources.GetBuffer(CBTBinding::BisectorData), bisectors.data(),
           bisectors.size() * sizeof(CBTBisectorData));
    append(m_Resources.GetBuffer(CBTBinding::Bitfield), &rootBits, sizeof(rootBits));
    append(m_Resources.GetBuffer(CBTBinding::IndicesAll), rootIndices.data(),
           rootIndices.size() * sizeof(uint32_t));
    if (firstDrawResourceInit)
    {
        const size_t firstWord = uploadWords.size();
        uploadWords.resize(firstWord + identityCount);
        for (uint32_t i = 0; i < identityCount; ++i)
            uploadWords[firstWord + i] = i;
        copies.push_back({m_Resources.GetIdentityIndexBuffer(), firstWord * sizeof(uint32_t),
                          static_cast<size_t>(identityCount) * sizeof(uint32_t),
                          ResourceState::IndexBuffer});
        const uint32_t drawCount = kDrawCountValue;
        append(m_Resources.GetDrawCountBuffer(), &drawCount, sizeof(drawCount),
               ResourceState::IndirectArgs);
    }

    const size_t uploadBytes = uploadWords.size() * sizeof(uint32_t);
    const BufferHandle staging = m_Device->CreateUploadBuffer(uploadBytes, "CBT.RootUpload");
    if (!staging.IsValid())
        return false;
    m_Device->UpdateBuffer(staging, 0, uploadBytes, uploadWords.data());
    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    if (!cl)
    {
        m_Device->DestroyBuffer(staging);
        return false;
    }
    cl->Begin();
    std::vector<ResourceBarrier> initialized;
    auto clear = [&](CBTBinding binding, size_t offset, size_t bytes) {
        cl->FillBuffer(m_Resources.GetBuffer(binding), offset, bytes, 0u);
    };
    // HeapID and occupancy are the free-slot guards. Dead neighbor/vertex/payload/index records
    // are never read: root payloads are copied above, new children are fully initialized by Bisect,
    // and RecordUpdate refreshes root corners before Classify. Clearing those dead records moved
    // 184 MiB at the 1M pool, including queue entries whose counters already make them unreachable.
    clear(CBTBinding::HeapID, rootCount * sizeof(uint64_t),
          static_cast<size_t>(m_PoolSize - rootCount) * sizeof(uint64_t));
    clear(CBTBinding::Bitfield, sizeof(uint32_t),
          static_cast<size_t>(CBTBitfieldWords(m_PoolSize) - 1u) * sizeof(uint32_t));
    for (const auto [binding, words] : {
             std::pair{CBTBinding::WorkQueue, kWQCounterSlots},
             std::pair{CBTBinding::IndirectDispatch, kIndirectDispatchWords},
             std::pair{CBTBinding::IndirectDraw, kIndirectDrawWords},
             std::pair{CBTBinding::Validation, kValidationWords}})
    {
        clear(binding, 0, static_cast<size_t>(words) * sizeof(uint32_t));
        initialized.push_back(ResourceBarrier::CreateBufferBarrier(m_Resources.GetBuffer(binding),
            ResourceState::CopyDest, ResourceState::UnorderedAccess));
    }
    for (const auto& copy : copies)
    {
        cl->CopyBuffer(staging, copy.Buffer, copy.Bytes, copy.Offset, 0);
        initialized.push_back(ResourceBarrier::CreateBufferBarrier(copy.Buffer,
            ResourceState::CopyDest, copy.FinalState));
    }
    cl->BarrierBatch(initialized);

    if (firstDrawResourceInit)
    {
        for (const TextureHandle texture : {m_Resources.GetDefaultHeightTexture(),
                 m_Resources.GetDefaultAtlasHeightTexture(),
                 m_Resources.GetDefaultAtlasCoarseTexture()})
        {
            if (!texture.IsValid())
                continue;
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::Undefined,
                                                               ResourceState::CopyDest));
            const float zero[4] = {};
            cl->ClearColorImageSubresource(texture, 0, 0, zero);
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(texture, ResourceState::CopyDest,
                                                               ResourceState::ShaderResource));
        }
    }
    // Reduction overwrites every sum-tree element from the initialized occupancy bitfield.
    CBTPushConstants pc{};
    pc.BaseDepth = baseDepth;
    pc.PoolSize = m_PoolSize;
    pc.TotalElements = m_PoolSize;
    pc.NeighborsReadIsA = 1u;
    RecordReduce(*cl, pc);

    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    m_Device->ExecuteCommandLists(lists);
    // The submit above cleared the device's known-idle flag and advanced the graphics timeline, so
    // this destroy takes the timeline-deferred path keyed at this submission's value: the staging
    // buffer outlives the copies that read it.
    m_Device->DestroyBuffer(staging);

    m_NeighborsReadIsA = true;
    m_DispatchInIndirectState = false;
    m_DrawResourcesInitialized = true;
    m_VerticesInitialized = false;
    return true;
}

void CBTInstance::RefineUniform(uint32_t depth, uint32_t frameCounter)
{
    if (!m_Ready || depth <= m_BaseDepth)
        return;
    const CBTClassifyDesc classify{kClassifyDepthTarget, kFocusRootAll, depth};
    // One pass splits every leaf once; the passes past depth - base absorb the conforming splits
    // a pass defers, and are no-ops once every leaf is at `depth`.
    const uint32_t passes = (depth - m_BaseDepth) + kRefineUniformExtraPasses;
    for (uint32_t pass = 0; pass < passes; ++pass)
    {
        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
        if (!cl)
            return;
        cl->Begin();
        RecordUpdate(*cl, classify, CBTIdentityFrameParams(), frameCounter);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitGpuSyncToken(m_Device->LastGraphicsSubmissionToken());
    }
}

void CBTInstance::DispatchDirect(CommandList& cl, CBTKernel kernel, const CBTPushConstants& pc,
                                 uint32_t groupsX)
{
    PipelineHandle pipe = m_KernelSet->GetPipeline(kernel);
    cl.SetPipeline(pipe);
    cl.BindDescriptorSet(0, m_Resources.GetDescriptorSet(static_cast<uint32_t>(kernel)), pipe);
    cl.SetPushConstants(pc);
    cl.Dispatch(groupsX, 1, 1);
}

void CBTInstance::DispatchIndirectKernel(CommandList& cl, CBTKernel kernel,
                                         const CBTPushConstants& pc, uint32_t dispatchSlot)
{
    EnsureDispatchIndirect(cl);
    PipelineHandle pipe = m_KernelSet->GetPipeline(kernel);
    cl.SetPipeline(pipe);
    cl.BindDescriptorSet(0, m_Resources.GetDescriptorSet(static_cast<uint32_t>(kernel)), pipe);
    cl.SetPushConstants(pc);
    cl.DispatchIndirect(m_Resources.GetBuffer(CBTBinding::IndirectDispatch),
                        IndirectDispatchSlotByteOffset(dispatchSlot));
}

void CBTInstance::PrepareIndirect(CommandList& cl, CBTPushConstants pc, uint32_t counterSlot)
{
    EnsureDispatchUAV(cl);
    pc.PassIndex = counterSlot;
    DispatchDirect(cl, CBTKernel::PrepareIndirect, pc, 1);
    // The PrepareIndirect write to the dispatch buffer is ordered against the
    // following indirect fetch by EnsureDispatchIndirect's UAV->IndirectArgs
    // barrier, so no data barrier is needed here.
}

void CBTInstance::RecordDataBarrier(CommandList& cl)
{
    // UAV->UAV buffer barriers for every buffer except the indirect-dispatch
    // args (whose UAV<->IndirectArgs transitions are tracked separately). Derives
    // COMPUTE-write -> COMPUTE-read/write masks in the backend — the proven F1
    // buffer-barrier path, batched into one pipeline barrier.
    // Handles and barrier states are stable for the lifetime of these resources. Reuse the batch
    // instead of allocating and rebuilding it at every dependency in every frame.
    cl.BarrierBatch(m_DataBarriers);
}

void CBTInstance::RecordReduce(CommandList& cl, const CBTPushConstants& pc)
{
    // Rebuild the OCBT sum tree from the occupancy bitfield: packed-popcount
    // prepass -> unpack leaves and sum each workgroup's block up to level 11 ->
    // single-workgroup up-sweep to the root. Root == live count.
    DispatchDirect(cl, CBTKernel::ReducePrePass, pc,
                   CeilDiv(CBTLeafPackedWords(m_PoolSize), kComputeWorkgroupSize));
    RecordDataBarrier(cl);
    DispatchDirect(cl, CBTKernel::ReduceFirstPass, pc,
                   CeilDiv(CBTLeafNodeCount(m_PoolSize), kComputeWorkgroupSize));
    RecordDataBarrier(cl);
    DispatchDirect(cl, CBTKernel::ReduceSecondPass, pc, 1);
    RecordDataBarrier(cl);
}

void CBTInstance::EnsureDispatchUAV(CommandList& cl)
{
    if (m_DispatchInIndirectState)
    {
        cl.Barrier(ResourceBarrier::CreateBufferBarrier(
            m_Resources.GetBuffer(CBTBinding::IndirectDispatch),
            ResourceState::IndirectArgs, ResourceState::UnorderedAccess));
        m_DispatchInIndirectState = false;
    }
}

void CBTInstance::EnsureDispatchIndirect(CommandList& cl)
{
    if (!m_DispatchInIndirectState)
    {
        cl.Barrier(ResourceBarrier::CreateBufferBarrier(
            m_Resources.GetBuffer(CBTBinding::IndirectDispatch),
            ResourceState::UnorderedAccess, ResourceState::IndirectArgs));
        m_DispatchInIndirectState = true;
    }
}

bool CBTInstance::RefreshTerrainSources()
{
    if (!m_Ready || m_Device == nullptr)
        return false;
    // The retired heightmap is still alive (its owner defers the destroy behind a frames-in-flight
    // quarantine), but rewriting a ring element an in-flight frame is still sampling is a
    // descriptor-in-use hazard. The readers are all graphics-queue submissions — CBT.Update and the
    // forward draw are both RGQueue::Graphics — so retiring the graphics timeline's last submitted
    // value is the whole protection, with none of a device drain's reach: no compute/transfer wait,
    // no bulk deferred-resource flush, and no flip of the device's known-idle flag (which would
    // reroute every later-declaring node's destroys to the immediate path).
    m_Device->WaitGpuSyncToken(m_Device->LastGraphicsSubmissionToken());
    return m_Resources.RebindTerrainSourcesToDefault();
}

void CBTInstance::SetHeightSource(uint32_t frameCounter, TextureHandle heightTexture)
{
    m_Resources.SetHeightSource(frameCounter, heightTexture);
}

void CBTInstance::RegionFreeToBase(uint32_t freeSetMask)
{
    if (!m_Ready || m_Device == nullptr || freeSetMask == 0u)
        return;
    // Sphere-only: the region reseed's base-adjacency link kernel uses the cube-sphere root twin
    // table. A planar 2-triangle base never saturates a far field, so there is nothing to drain.
    if (m_DomainMode != kDomainSpherical)
        return;

    CBTPushConstants pc{};
    pc.BaseDepth = m_BaseDepth;
    pc.PoolSize = m_PoolSize;
    pc.TotalElements = m_PoolSize; // CLEAR + the reduces are whole-pool
    pc.NeighborsReadIsA = m_NeighborsReadIsA ? 1u : 0u;
    pc.DomainMode = m_DomainMode;
    pc.RegionFreeMask = freeSetMask;

    const uint32_t poolGroups = CeilDiv(m_PoolSize, kComputeWorkgroupSize);

    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    if (!cl)
        return;
    cl->Begin();
    // The prior update may have left the dispatch buffer in IndirectArgs; the reduces below dispatch
    // direct, so it is untouched, but keep the state machine consistent for a following update.
    EnsureDispatchUAV(*cl);
    // Order CLEAR's whole-pool writes after the prior frames' UnorderedAccess writes to the same
    // buffers (the InitializeRoots discipline): a pipeline barrier's first synchronization scope
    // spans earlier submissions to this queue, so no CPU drain is needed to express the WAW.
    RecordDataBarrier(*cl);

    // 1. Free every live bisector whose root is selected -> HeapID 0, bit cleared. Whole pool.
    cl->SetMarker("CBT.RegionFreeClear");
    DispatchDirect(*cl, CBTKernel::RegionFreeClear, pc, poolGroups);
    RecordDataBarrier(*cl);

    // 2. Rebuild the sum tree so SEED's free-slot decode sees the freed slots (the reference
    //    Allocate reads a fresh sum-tree snapshot). Same three-kernel reduce as the frame tail.
    cl->SetMarker("CBT.RegionFreeReduce1");
    RecordReduce(*cl, pc);

    // 3. Reseed each freed root's base bisector into a decoded free slot (rootCount threads, one
    //    group covers the 24 sphere roots). Bit-set deferred to LINK so all decodes see a stable
    //    post-CLEAR bitfield.
    CBTPushConstants pcRoot = pc;
    pcRoot.TotalElements = m_RootCount;
    cl->SetMarker("CBT.RegionFreeSeed");
    DispatchDirect(*cl, CBTKernel::RegionFreeSeed, pcRoot, 1u);
    RecordDataBarrier(*cl);

    // 4. Wire the reseeded roots' base neighbor links (INVALID toward a retained/visible neighbor)
    //    and set their occupancy bits.
    cl->SetMarker("CBT.RegionFreeLink");
    DispatchDirect(*cl, CBTKernel::RegionFreeLink, pcRoot, 1u);
    RecordDataBarrier(*cl);

    // 5. Final reduce so the sum-tree root (live count) reflects the freed + reseeded pool for the
    //    next frame's Reset (FreeCount) and free-slot decode.
    cl->SetMarker("CBT.RegionFreeReduce2");
    RecordReduce(*cl, pc);

    // 6. Rebuild the compact live-index stream. The reduce above fixed the live COUNT, but
    //    IndicesAll still lists the PRE-free live set, and every indirect-over-live kernel maps its
    //    compact index through IndicesAll[0 .. liveCount) — which, once the count collapses, is a
    //    prefix of the stale list holding mostly just-freed slots rather than the retained ones.
    //    Classify tolerates that (it only picks candidates, and re-picks next frame), but the
    //    neighbor carry-over cannot: a retained bisector missing from the carried set keeps a stale
    //    NEXT record and breaks link reciprocity. Reset runs first because BisectorIndexation
    //    atomicAdds into the indirect-draw records, which must start at zero; it also re-seeds the
    //    dispatch widths off the post-free live count. The next RecordUpdate's own Reset repeats
    //    both harmlessly.
    cl->SetMarker("CBT.RegionFreeReindex");
    DispatchDirect(*cl, CBTKernel::Reset, pc, 1u);
    RecordDataBarrier(*cl);
    DispatchDirect(*cl, CBTKernel::BisectorIndexation, pc, poolGroups);
    RecordDataBarrier(*cl);

    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    // The trailing data barrier above publishes the freed + reseeded pool to the next update: a
    // barrier's second synchronization scope covers later submissions to this queue, so nothing
    // has to be waited on the CPU here.
    m_Device->ExecuteCommandLists(lists);
    m_VerticesInitialized = false; // reseeded HeapIDs no longer match the retained corner payloads
}

void CBTInstance::RecordUpdate(CommandList& cl, const CBTClassifyDesc& classify,
                              const CBTFrameParams& params, uint32_t frameCounter)
{
    // Upload this frame's camera + terrain params to the UBO ring element the kernels
    // read, CBTFrameRingSlot(frameCounter). Host-visible write completes before submit;
    // the ring + frame fence keep it clear of the previous frame's in-flight read.
    m_Resources.UploadFrameParams(frameCounter, params);

    CBTPushConstants pc{};
    pc.BaseDepth = m_BaseDepth;
    pc.PoolSize = m_PoolSize;
    pc.TotalElements = m_PoolSize;
    pc.NeighborsReadIsA = m_NeighborsReadIsA ? 1u : 0u;
    pc.FocusRoot = classify.FocusRoot;
    pc.TargetDepth = classify.TargetDepth;
    // Pushed UNREDUCED: the shader applies CBT_FRAME_RING itself, which is the same
    // reduction CBTFrameRingSlot just made on the CPU, so both name one ring element.
    pc.FrameIndex = frameCounter;
    pc.ClassifyMode = classify.Mode;
    pc.DomainMode = m_DomainMode;
    // C5: forward this frame's edited region (UV) so Classify reclassifies the
    // overlapping bisectors (MODIFIED + dead-band collapse). Empty when no edit.
    pc.DirtyMinU = classify.DirtyMinU;
    pc.DirtyMinV = classify.DirtyMinV;
    pc.DirtyMaxU = classify.DirtyMaxU;
    pc.DirtyMaxV = classify.DirtyMaxV;
    // Planet editing (plan §planet-editing): the (face,rect) selector + the sculpt-sample
    // gate. On the sphere the dirty rect is face-local; DirtyFace names the face.
    pc.DirtyFace = classify.DirtyFace;
    pc.SphereSculptEnabled = classify.SphereSculptEnabled;
    // The narrow-heap kernels (no 64-bit integers: WebGPU) cannot bind the
    // modified index stream — it would be the 11th storage buffer against a
    // per-stage limit of 10 — so VertexEval always walks the full live stream there.
    const bool vertexEvalGateSupported = m_Device->GetCapabilities().supportsShaderInt64;
    pc.GateVertexEval = vertexEvalGateSupported ? classify.GateVertexEval : 0u;
    pc.EditRetessEnabled = classify.EditRetessEnabled;
    pc.NearFieldGate = classify.NearFieldGate;
    pc.DeepDecode = classify.DeepDecode;
    pc.PoolPressure = classify.PoolPressure;

    // Indexation and Validate still scan the whole pool (Indexation IS the pointer-cache
    // that rebuilds the live compact stream every frame; Validate's zombie check must see the
    // free slots too). Classify and VertexEval dispatch INDIRECT over the live count instead
    // (arc slice S3 pool-scaling): the idle cost of those two no longer grows with the pool, so
    // a bigger pool with the same live set keeps today's quiescent floor.
    const uint32_t poolGroups = CeilDiv(m_PoolSize, kComputeWorkgroupSize);

    // The prior frame ended with the dispatch buffer in IndirectArgs (VertexEval's indirect
    // fetch); Reset writes slot 0 (the live-count dispatch for Classify), so transition it back
    // to UAV first, ordering that write after last frame's indirect read.
    EnsureDispatchUAV(cl);

    // Per-group GPU markers: a device-lost reports the command list's last marker
    // (VulkanCommandList tracks it), so a future TDR names the guilty kernel group
    // in the log instead of being an anonymous hang.
    cl.SetMarker("CBT.Reset");
    // 1. Reset per-frame counters + indirect-draw records; recompute FreeCount
    //    from the sum-tree root (last frame's reduce).
    DispatchDirect(cl, CBTKernel::Reset, pc, 1);
    RecordDataBarrier(cl);

    // 1.5 Debug: the compact live-index stream this frame inherited must be exactly the live set,
    //     because Classify and the neighbor carry-over below both dereference it. This is the ONLY
    //     point that state is observable — step 8's Validate runs after Indexation has rebuilt the
    //     stream, so it can only ever see a fresh one. Costs nothing when validation is off (the
    //     kernel is not dispatched); when on it is one indirect dispatch over the live width, and
    //     the UAV->IndirectArgs transition it takes is the one Classify would have taken anyway.
    if (m_ValidateEachUpdate)
    {
        cl.SetMarker("CBT.ValidateCompactStream");
        DispatchIndirectKernel(cl, CBTKernel::ValidateCompactStream, pc, kDispatchSlotLive);
        // Deliberately no data barrier: this reads only state Classify also reads (IndicesAll, the
        // bitfield, the sum tree) and writes only the Validation counters, which nothing touches
        // again until Validate at step 8 — so it overlaps Classify for free, and the barrier after
        // Classify still orders it ahead of everything that mutates occupancy.
    }

    // A new root set has no corners yet. A forced refresh can also move every existing corner
    // (terrain transform/height-source changes). Refresh those BEFORE visibility and LOD read
    // them, or one frame culls the new terrain against its old location. The tail evaluation
    // still handles children and promoted parents created by this frame's topology changes.
    const bool refreshBeforeClassify = !m_VerticesInitialized ||
        (classify.Mode == kClassifyScreenSpace && classify.GateVertexEval == 0u);
    if (refreshBeforeClassify)
    {
        cl.SetMarker("CBT.RefreshVertices");
        CBTPushConstants refresh = pc;
        refresh.GateVertexEval = 0u;
        DispatchIndirectKernel(cl, CBTKernel::VertexEval, refresh, kDispatchSlotLive);
        RecordDataBarrier(cl);
    }

    // 1.6 Content-aware split: Classify reads the height-range pyramid of this frame's heights.
    const bool editRect = classify.DirtyMaxU > classify.DirtyMinU && classify.DirtyMaxV > classify.DirtyMinV;
    RecordHeightRangeBuild(cl, classify, params, pc, frameCounter, refreshBeforeClassify || editRect);

    // 2. Classify drives the deterministic depth-target metric. It reads the
    //    CurrentVertexBuffer written by the PREVIOUS frame's vertex-eval (step
    //    5.5): the tree is unchanged across the frame boundary, so those corners
    //    match the current topology. The refresh above supplies current corners on first use
    //    and forced changes; it never replaces the post-topology evaluation below.
    cl.SetMarker("CBT.Classify");
    // Over the live count: Reset seeds the stage slot with it before anything rewrites it.
    DispatchIndirectKernel(cl, CBTKernel::Classify, pc, kDispatchSlotStage);
    RecordDataBarrier(cl);

    // 3. Split path: compatibility-chain claim -> allocate free slots -> bisect.
    cl.SetMarker("CBT.SplitPath");
    PrepareIndirect(cl, pc, kWQSplitCounter);
    DispatchIndirectKernel(cl, CBTKernel::Split, pc, kDispatchSlotStage);
    RecordDataBarrier(cl);

    PrepareIndirect(cl, pc, kWQAllocateCounter);
    DispatchIndirectKernel(cl, CBTKernel::Allocate, pc, kDispatchSlotStage);
    RecordDataBarrier(cl);

    // Carry CURRENT -> NEXT so Bisect writes only the deltas (ping-pong). Indirect over the
    // frame's LIVE set (kDispatchSlotLive, seeded by Reset and untouched by the split path's
    // slot-0 rewrites above), scattering through IndicesAll — the live set is not a pool prefix,
    // so this cannot be a bounded range copy. Replaces a whole-pool CopyBuffer that moved
    // sizeof(CBTNeighbors) * poolSize (16 MiB at the 1M pool) on every update no matter how few
    // bisectors were live. The barrier after it is required, not cosmetic: the carry-over and
    // Bisect both store to NEXT at the split parents (a WAW).
    DispatchIndirectKernel(cl, CBTKernel::NeighborCopy, pc, kDispatchSlotLive);
    RecordDataBarrier(cl);
    DispatchIndirectKernel(cl, CBTKernel::Bisect, pc,
                           kDispatchSlotStage); // reuses the allocate count
    RecordDataBarrier(cl);

    PrepareIndirect(cl, pc, kWQPropagateBisectCounter);
    DispatchIndirectKernel(cl, CBTKernel::PropagateBisect, pc, kDispatchSlotStage);
    RecordDataBarrier(cl);

    // 4. Merge path (three kernels: densify -> merge -> scatter).
    cl.SetMarker("CBT.MergePath");
    PrepareIndirect(cl, pc, kWQSimplifyClassCounter);
    DispatchIndirectKernel(cl, CBTKernel::PrepareSimplify, pc, kDispatchSlotStage);
    RecordDataBarrier(cl);

    PrepareIndirect(cl, pc, kWQSimplifyCounter);
    DispatchIndirectKernel(cl, CBTKernel::Simplify, pc, kDispatchSlotStage);
    RecordDataBarrier(cl);

    PrepareIndirect(cl, pc, kWQPropagateSimplifyCounter);
    DispatchIndirectKernel(cl, CBTKernel::PropagateSimplify, pc, kDispatchSlotStage);
    RecordDataBarrier(cl);

    // 5. Reduce: rebuild the sum tree so the NEXT frame's Reset (FreeCount) and
    //    Allocate (free-slot decode) see the post-update occupancy.
    cl.SetMarker("CBT.Reduce");
    RecordReduce(cl, pc);

    // 6. Indexation FIRST (moved ahead of VertexEval for the S3 pool-scaling slice): it is the
    //    pointer-cache pass — it bins every live bisector into the compact IndicesAll stream (plus
    //    the VISIBLE / MODIFIED draw streams) and emits the draw counts. VertexEval then dispatches
    //    INDIRECT over that fresh compact list, so it touches only live slots (not the whole pool).
    //    Indexation reads only HeapID (free skip) + flags (both final after the split/merge path), so
    //    running it before VertexEval is order-independent for its own output; the children Bisect
    //    created and the survivors Simplify promoted are live (bit + HeapID set), so they are binned
    //    and thus reached by VertexEval — the C3 focus-hole invariant (new/deep bisectors get correct
    //    corners before the draw and before next frame's Classify) is preserved.
    cl.SetMarker("CBT.Indexation");
    DispatchDirect(cl, CBTKernel::BisectorIndexation, pc, poolGroups);
    RecordDataBarrier(cl);
    cl.SetMarker("CBT.FinalizeIndirect");
    // PrepareBisectorIndirect clamps the draw records to bounded, well-formed
    // VkDrawIndexedIndirectCommands (indexCount <= 3*pool, instanceCount == 1 — a racy/corrupt
    // count can't walk billions of vertex invocations and TDR) AND rewrites dispatch slot 0 to
    // ceil(thisFrameLive/64) for the VertexEval dispatch below. Transition the dispatch buffer
    // back to UAV first (a split/merge indirect dispatch left it in IndirectArgs).
    EnsureDispatchUAV(cl);
    DispatchDirect(cl, CBTKernel::PrepareBisectorIndirect, pc, 1);
    RecordDataBarrier(cl);

    // 7. Vertex-eval once the topology AND the pointer cache
    //    are final. Every live bisector this frame — including Bisect's children and Simplify's
    //    survivors — has correct conforming LEB corners before the vertex modifier reads them and
    //    before next frame's Classify. Gated (CBTClassifyDesc::GateVertexEval) so a quiescent frame
    //    launches zero workgroups when no geometry changed. Gated work uses the modified stream
    //    and slot; an ungated depth-target update retains the full live-stream evaluation.
    cl.SetMarker("CBT.VertexEval");
    CBTPushConstants vertexEval = pc;
    if (refreshBeforeClassify && vertexEvalGateSupported)
        vertexEval.GateVertexEval = 1u; // unchanged corners were already refreshed
    DispatchIndirectKernel(cl, CBTKernel::VertexEval, vertexEval,
                           vertexEval.GateVertexEval ? kDispatchSlotModified : kDispatchSlotStage);
    RecordDataBarrier(cl);
    m_VerticesInitialized = true;

    // 8. Validate — whole-pool debug invariants (link reciprocity / budget / zombie, plus the
    //    coverage half of the compact-stream invariant now that Indexation has rebuilt it). It
    //    scans the FREE slots too (the zombie check), so it cannot ride the live compact stream; skipping it
    //    in ship is what keeps the update's one remaining pool-linear per-frame kernel (besides the
    //    Indexation compaction) out of the idle floor at a bumped pool size. Tests keep it on.
    if (m_ValidateEachUpdate)
    {
        cl.SetMarker("CBT.Validate");
        DispatchDirect(cl, CBTKernel::Validate, pc, poolGroups);
        RecordDataBarrier(cl);
    }

    // 7. Flip the ping-pong parity: this frame's NEXT becomes next frame CURRENT.
    m_NeighborsReadIsA = !m_NeighborsReadIsA;
}

void CBTInstance::RecordHeightRangeBuild(CommandList& cl, const CBTClassifyDesc& classify,
                                         const CBTFrameParams& params, CBTPushConstants pc,
                                         uint32_t frameCounter, bool heightsChanged)
{
    const BufferHandle range = m_Resources.GetHeightRangeBuffer();
    if (!range.IsValid() || classify.Mode != kClassifyScreenSpace || m_DomainMode != kDomainPlanar ||
        !(params.WaterPlane[3] > 0.0f) || params.AtlasParams1[2] == 1.0f)
        return; // the atlas arm binds no unified height texture; the unified and paged arms bind the
                // terrain's when it has one (an atlas-backed terrain on its pages binds the 1x1
                // default, and the size check below declines it)
    const TextureHandle source = m_Resources.GetBoundHeightSource(frameCounter);
    if (!heightsChanged && source == m_HeightRangeSource)
        return;
    m_HeightRangeSource = source;
    uint32_t width = 0;
    uint32_t height = 0;
    cl.GetTextureSize(source, width, height);
    if (width < 2u || height < 2u)
        return;
    const uint32_t cellsX = width - 1u;
    const uint32_t cellsY = height - 1u;
    const uint32_t first = CBTHeightRangeFirstLevel(cellsX, cellsY);
    if (first == 0u)
        return;
    const uint32_t top = std::max(CBTHeightRangeTopLevel(cellsX, cellsY), first);
    const ResourceBarrier written =
        ResourceBarrier::CreateBufferBarrier(range, ResourceState::UnorderedAccess,
                                             ResourceState::UnorderedAccess);
    cl.SetMarker("CBT.HeightRangeBuild");
    for (uint32_t level = first; level <= top; ++level)
    {
        pc.PassIndex = level;
        const uint32_t entries = CBTHeightRangeDim(cellsX, level) * CBTHeightRangeDim(cellsY, level);
        DispatchDirect(cl, CBTKernel::HeightRangeBuild, pc, CeilDiv(entries, kComputeWorkgroupSize));
        cl.Barrier(written);
    }
}

void CBTInstance::RecordReadback(CommandList& cl)
{
    EnsureDispatchUAV(cl); // leave the dispatch buffer in a defined state
    BufferHandle draw = m_Resources.GetBuffer(CBTBinding::IndirectDraw);
    BufferHandle validation = m_Resources.GetBuffer(CBTBinding::Validation);
    BufferHandle readback = m_Resources.GetReadbackBuffer();

    cl.Barrier(ResourceBarrier::CreateBufferBarrier(draw, ResourceState::UnorderedAccess,
                                                    ResourceState::CopySource));
    cl.Barrier(ResourceBarrier::CreateBufferBarrier(validation, ResourceState::UnorderedAccess,
                                                    ResourceState::CopySource));
    cl.CopyBuffer(draw, readback, static_cast<size_t>(kReadbackDrawBytes), 0, kReadbackDrawOffset);
    cl.CopyBuffer(validation, readback, static_cast<size_t>(kReadbackValidationBytes), 0,
                  kReadbackValidationOffset);
    // Restore UnorderedAccess: both buffers are render-loop-owned, and the Read* accessors and the
    // next update's manual barriers all declare that as the old state.
    cl.Barrier(ResourceBarrier::CreateBufferBarrier(draw, ResourceState::CopySource,
                                                    ResourceState::UnorderedAccess));
    cl.Barrier(ResourceBarrier::CreateBufferBarrier(validation, ResourceState::CopySource,
                                                    ResourceState::UnorderedAccess));
}

uint32_t CBTInstance::ReadDrawIndexCount(uint32_t stream) const
{
    return ReadDrawRecordField(stream, kDrawIndexCountField);
}

uint32_t CBTInstance::ReadDrawRecordField(uint32_t stream, uint32_t field) const
{
    if (stream >= kDrawStreamCount || field >= kDrawStreamStride)
        return 0;
    const uint32_t word = stream * kDrawStreamStride + field;
    return ReadSharedReadbackWord(CBTBinding::IndirectDraw, kReadbackDrawOffset,
                                  kReadbackDrawBytes, word);
}

std::vector<uint32_t> CBTInstance::DebugReadWords(CBTBinding binding, uint32_t wordCount,
                                                  uint32_t firstWord)
{
    // Clamp against what remains of the source past firstWord, so the copy can never read past its
    // end (a caller may ask for the whole pool; a smaller binding must not overflow).
    const uint64_t srcWords = m_Resources.GetBufferByteSize(binding) / 4u;
    if (firstWord >= srcWords)
        return {};
    if (static_cast<uint64_t>(wordCount) > srcWords - firstWord)
        wordCount = static_cast<uint32_t>(srcWords - firstWord);
    std::vector<uint32_t> out(wordCount, 0u);
    if (wordCount == 0)
        return out;
    const size_t bytes = static_cast<size_t>(wordCount) * 4u;
    // A private destination: the shared readback buffer also holds the indirect-draw records and
    // the validation counters, and a copy landing at its offset 0 would overwrite them.
    BufferHandle readback = m_Device->CreateReadbackBuffer(bytes, "CBT.BulkReadback");
    if (!readback.IsValid())
        return out;
    BufferHandle src = m_Resources.GetBuffer(binding);
    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    // The leading barrier also orders the copy after the writes of every earlier submission on this
    // queue (a barrier's first synchronization scope spans them), so a stale read is not possible.
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(src, ResourceState::UnorderedAccess, ResourceState::CopySource));
    cl->CopyBuffer(src, readback, bytes, static_cast<size_t>(firstWord) * 4u, 0);
    // Restore UnorderedAccess: the sources are render-loop-owned, so reading one must not leave it
    // in a state the next update's manual barriers do not expect.
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(src, ResourceState::CopySource,
                                                     ResourceState::UnorderedAccess));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    m_Device->ExecuteCommandLists(lists);
    m_Device->WaitGpuSyncToken(m_Device->LastGraphicsSubmissionToken());
    const void* mapped = m_Device->MapBuffer(readback);
    if (mapped)
    {
        std::memcpy(out.data(), mapped, bytes);
        m_Device->UnmapBuffer(readback);
    }
    m_Device->DestroyBuffer(readback);
    return out;
}

uint32_t CBTInstance::ReadValidationErrorCount() const
{
    return ReadValidationCounter(kValidationErrorCounter);
}

uint32_t CBTInstance::ReadValidationCounter(uint32_t counterSlot) const
{
    if (counterSlot >= kValidationWords)
        return 0;
    return ReadSharedReadbackWord(CBTBinding::Validation, kReadbackValidationOffset,
                                  kReadbackValidationBytes, counterSlot);
}

// One word of a render-loop-owned buffer, through its region of the shared readback buffer. The
// copy must be recorded here: relying on a caller-recorded RecordReadback leaves the read mapping
// whatever host memory the allocator recycled into the buffer, and these words are the draw and
// tree-conformity oracles — a wrong-by-luck read is worse than no read.
uint32_t CBTInstance::ReadSharedReadbackWord(CBTBinding binding, uint64_t readbackOffset,
                                             uint64_t regionBytes, uint32_t word) const
{
    if (!m_Ready || m_Device == nullptr)
        return 0;
    BufferHandle src = m_Resources.GetBuffer(binding);
    BufferHandle readback = m_Resources.GetReadbackBuffer();
    if (!src.IsValid() || !readback.IsValid())
        return 0;

    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(src, ResourceState::UnorderedAccess,
                                                     ResourceState::CopySource));
    cl->CopyBuffer(src, readback, static_cast<size_t>(regionBytes), 0, readbackOffset);
    // Restore UnorderedAccess so the next update's manual barriers see the state they declare.
    cl->Barrier(ResourceBarrier::CreateBufferBarrier(src, ResourceState::CopySource,
                                                     ResourceState::UnorderedAccess));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    m_Device->ExecuteCommandLists(lists);
    m_Device->WaitGpuSyncToken(m_Device->LastGraphicsSubmissionToken());

    const void* mapped = m_Device->MapBuffer(readback);
    if (!mapped)
        return 0;
    uint32_t value = 0;
    std::memcpy(&value, static_cast<const uint8_t*>(mapped) + readbackOffset + word * 4u,
                sizeof(uint32_t));
    m_Device->UnmapBuffer(readback);
    return value;
}

int32_t CBTInstance::ReadWorkQueueCounter(uint32_t counterSlot)
{
    const std::vector<uint32_t> words = DebugReadWords(CBTBinding::WorkQueue, 1u, counterSlot);
    int32_t value = 0;
    std::memcpy(&value, words.data(), sizeof(int32_t));
    return value;
}

CBTTessellationStats CBTInstance::ReadTessellationStats()
{
    CBTTessellationStats st;
    st.PoolSize = m_PoolSize;
    st.RootCount = m_RootCount;
    if (!m_Ready || m_Device == nullptr)
        return st;

    // The counters live in the first kWQCounterSlots words.
    const std::vector<uint32_t> wq = DebugReadWords(CBTBinding::WorkQueue, kWQCounterSlots);
    auto sig = [&](uint32_t slot) {
        int32_t v = 0;
        if (slot < wq.size())
            std::memcpy(&v, &wq[slot], sizeof(int32_t));
        return v;
    };
    st.FreeCount = sig(kWQFreeCount);
    st.SplitDemand = sig(kWQSplitCounter);
    st.MergeDemand = sig(kWQSimplifyClassCounter);
    st.SplitServed = sig(kWQAllocateCounter);
    st.MergeServed = sig(kWQSimplifyCounter);
    st.OverflowTotal = sig(kWQOverflowCounter);
    st.DispatchClampTotal = sig(kWQDispatchClampCounter);
    st.PressureStep = sig(kWQPressureStep);
    st.OffFrustumKeepStep = sig(kWQOffFrustumKeepStep);

    // The sum-tree root (live bisector count) sits at the heap base, past the packed leaves.
    const uint32_t rootWord = CBTSumTreeHeapBase(m_PoolSize);
    const std::vector<uint32_t> sumTree = DebugReadWords(CBTBinding::SumTree, rootWord + 1u);
    if (rootWord < sumTree.size())
        st.LiveCount = sumTree[rootWord];
    return st;
}

} // namespace GameEngine::CBTTerrain
