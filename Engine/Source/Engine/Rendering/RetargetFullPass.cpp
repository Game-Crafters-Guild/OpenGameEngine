#include "Engine/Rendering/RetargetFullPass.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "Rendering/Core/PipelineTypes.h"

namespace GameEngine { namespace Engine { namespace Renderer {

using namespace GameEngine::Rendering;

void RetargetFullPass::Declare(RenderGraph::RGFrame& frame, RenderGraph::RGBuffer atlas) {
    if (!m_DataStore || m_ComputeShader.empty() || !atlas.IsValid()) return;
    if (m_DataStore->GetActiveCharacterCount() == 0) return;
    frame.AddPass("RetargetFullCompute",
                  static_cast<int32_t>(GameEngine::Rendering::PassPhase::kEarlySetup),
                  [&](RenderGraph::RGPassBuilder& p) { p.Write(atlas); },
                  [this, atlas](RenderGraph::RGContext& ctx)
                  { RecordDispatch(ctx.GetDevice(), ctx.Cmd, ctx.GetBuffer(atlas)); });
}

void RetargetFullPass::RecordDispatch(IDevice* dev, CommandList* cl,
                                      BufferHandle dispatchAtlasBuf) {
    if (!m_DataStore || m_ComputeShader.empty()) return;

    const uint32_t characterCount = m_DataStore->GetActiveCharacterCount();
    if (characterCount == 0) return;

    if (!cl || !dev) return;

    // Flush any per-frame uploads (charsBuf at minimum). Asset-load buffers
    // are only re-uploaded when their dirty bit fires.
    m_DataStore->FlushIfDirty();

    // Descriptor set: 4 storage buffers (clip, rig, chars, atlas).
    DescriptorSetLayoutDesc set0{};
    set0.debugName = "RetargetFull_Set0";
    for (uint32_t i = 0; i < 4; ++i) {
        DescriptorBinding b{};
        b.binding = i;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        set0.bindings.push_back(b);
    }

    if (!m_PipelineId.IsValid())
    {
        ::GameEngine::Rendering::ComputePipelineDesc cd{};
        cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(m_ComputeShader);
        cd.DescriptorSetLayouts.push_back(dev->InternDescriptorSetLayout(set0));
        cd.PushConstants.Size      = sizeof(uint32_t);
        cd.PushConstants.StageMask = kShaderStageCompute;
        cd.DebugName = "RetargetFull_Compute";
        m_PipelineId = dev->InternComputePipeline(cd);
    }
    PipelineHandle pipe = dev->GetOrCreateComputePipeline(m_PipelineId);
    if (!pipe) return;

    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = set0;
    dsDesc.transient = true;
    dsDesc.debugName = "RetargetFull_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsDesc);

    auto clipBuf  = m_DataStore->GetClipBuffer();
    auto rigBuf   = m_DataStore->GetRigBuffer();
    auto charsBuf = m_DataStore->GetCharsBuffer();
    auto atlasBuf = dispatchAtlasBuf;

    if (!clipBuf.IsValid() || !rigBuf.IsValid() || !charsBuf.IsValid() || !atlasBuf.IsValid())
        return;

    // Explicit sizes — UpdateStorageBufferBinding doesn't treat range=0 as
    // VK_WHOLE_SIZE in this engine; passing 0 produces an empty bind that
    // reads as zeros in the shader. Atlas is the one exception we leave at 0
    // because the pass's `ComputeTarget` declaration tracks its full extent.
    dev->UpdateStorageBufferBinding(ds, 0, clipBuf,  0, m_DataStore->GetClipUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 1, rigBuf,   0, m_DataStore->GetRigUsedBytes());
    dev->UpdateStorageBufferBinding(ds, 2, charsBuf, 0, characterCount * sizeof(GPURetargetCharacterParams));
    dev->UpdateStorageBufferBinding(ds, 3, atlasBuf, 0, 0);

    cl->BindDescriptorSet(0, ds, pipe);
    cl->SetPushConstants(characterCount);
    cl->Dispatch(characterCount, 1, 1);

    m_DataStore->MarkDispatchScheduledThisFrame();
}

}}} // namespace GameEngine::Engine::Renderer
