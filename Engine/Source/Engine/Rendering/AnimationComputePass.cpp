#include "Engine/Rendering/AnimationComputePass.h"
#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "Rendering/Core/PipelineTypes.h"

namespace GameEngine { namespace Engine { namespace Renderer {

using namespace GameEngine::Rendering;

void AnimationComputePass::Declare(RenderGraph::RGFrame& frame, RenderGraph::RGBuffer atlas,
                                   RenderGraph::RGBuffer runtimeVisible) {
    if (!m_DataStore || m_ComputeShader.empty() || !atlas.IsValid()) return;
    // No declare-time instance-count gate: thumbnail/preview worlds queue
    // instances AFTER the main world's declaration (old-arm contract), and
    // RecordDispatch re-checks the live count at exec — an unused dispatch
    // declaration is effectively free.
    const bool gate = m_GateEnabled && runtimeVisible.IsValid();
    frame.AddPass("AnimSkinningCompute",
                  static_cast<int32_t>(GameEngine::Rendering::PassPhase::kEarlySetup),
                  [&](RenderGraph::RGPassBuilder& p) {
                      p.Write(atlas);
                      if (gate)
                          p.Read(runtimeVisible); // BEFORE the aggregate's Write — the WAR edge
                  },
                  [this, atlas, runtimeVisible, gate](RenderGraph::RGContext& ctx) {
                      RecordDispatch(ctx.GetDevice(), ctx.Cmd, ctx.GetBuffer(atlas),
                                     gate ? ctx.GetBuffer(runtimeVisible) : BufferHandle{});
                  });
}

void AnimationComputePass::RecordDispatch(IDevice* dev, CommandList* cl,
                                          BufferHandle dispatchAtlasBuf,
                                          BufferHandle runtimeVisBuf) {
    if (!m_DataStore || m_ComputeShader.empty() || !cl || !dev) return;

    const uint32_t instanceCount = m_DataStore->GetInstanceCount();
    if (instanceCount == 0) return;

    m_DataStore->FlushInstances();

    // Descriptor set layout: 7 storage buffers (added binding 6 for the
    // Phase 6-iii runtime-visibility flag buffer).
    DescriptorSetLayoutDesc set0{};
    set0.debugName = "AnimCompute_Set0";
    for (uint32_t i = 0; i < 7; ++i) {
        DescriptorBinding b{};
        b.binding = i;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        set0.bindings.push_back(b);
    }

    // Phase 6-iii push constants: (instanceCount, gateEnabled, runtimeVisibleCap).
    struct AnimSkinningPC
    {
        uint32_t instanceCount;
        uint32_t gateEnabled;
        uint32_t runtimeVisibleCap;
    };

    if (!m_PipelineId.IsValid())
    {
        ::GameEngine::Rendering::ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(m_ComputeShader);
        cd.DescriptorSetLayouts.push_back(dev->InternDescriptorSetLayout(set0));
        cd.PushConstants.Size      = sizeof(AnimSkinningPC);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "AnimSkinning_Compute";
        m_PipelineId = dev->InternComputePipeline(cd);
    }
    PipelineHandle pipe = dev->GetOrCreateComputePipeline(m_PipelineId);
    if (!pipe) return;

    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = set0;
    dsDesc.transient = true;
    dsDesc.debugName = "AnimCompute_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);

    auto skelHeaderBuf = m_DataStore->GetSkeletonHeaderBuffer();
    auto skelDataBuf   = m_DataStore->GetSkeletonDataBuffer();
    auto clipHeaderBuf = m_DataStore->GetClipHeaderBuffer();
    auto clipDataBuf   = m_DataStore->GetClipDataBuffer();
    auto instanceBuf   = m_DataStore->GetInstanceBuffer();
    auto atlasBuf      = dispatchAtlasBuf;

    if (!skelHeaderBuf.IsValid() || !skelDataBuf.IsValid() ||
        !clipHeaderBuf.IsValid() || !clipDataBuf.IsValid() ||
        !instanceBuf.IsValid()   || !atlasBuf.IsValid())
        return;

    // Bind the exact written byte range so out-of-bound shader reads are caught by
    // robust-buffer-access rather than silently returning stale data from the upload
    // buffer's tail. Execute already returns when instanceCount == 0, which implies
    // EnsureSkeletonUploaded/EnsureClipUploaded ran at least once, so the used-byte
    // accessors are guaranteed > 0 here. Atlas remains whole-buffer — the compute
    // shader writes to per-instance offsets that can span the full allocation.
    dev->UpdateStorageBufferBinding(ds, 0, skelHeaderBuf, 0, m_DataStore->GetSkeletonHeaderUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 1, skelDataBuf,   0, m_DataStore->GetSkeletonDataUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 2, clipHeaderBuf, 0, m_DataStore->GetClipHeaderUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 3, clipDataBuf,   0, m_DataStore->GetClipDataUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 4, instanceBuf,   m_DataStore->GetInstanceBufferOffset(),
                                    instanceCount * sizeof(GPUAnimInstance));
    dev->UpdateStorageBufferBinding(ds, 5, atlasBuf,      0, 0);

    // Phase 6-iii binding 6: runtime-visibility flag buffer. Bind the live
    // buffer when the gate is enabled and the buffer is valid; otherwise
    // fall back to the instance buffer (any valid buffer suffices — the
    // shader's `if (gateEnabled != 0u)` predicate skips the read entirely).
    const bool useGate = runtimeVisBuf.IsValid();
    GameEngine::Rendering::BufferHandle runtimeVisBindBuf =
        useGate ? runtimeVisBuf : instanceBuf;
    dev->UpdateStorageBufferBinding(ds, 6, runtimeVisBindBuf, 0, 0);

    cl->BindDescriptorSet(0, ds, pipe);

    AnimSkinningPC pc{};
    pc.instanceCount     = instanceCount;
    pc.gateEnabled       = useGate ? 1u : 0u;
    pc.runtimeVisibleCap = useGate ? m_RuntimeVisibleCap : 0u;
    cl->SetPushConstants(pc);
    cl->Dispatch(instanceCount, 1, 1);
}

}}} // namespace GameEngine::Engine::Renderer
