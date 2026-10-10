#include "MetalCommandList.h"

#include "MetalClearTexel.h"
#include "MetalDevice.h"
#include "MetalMappings.h"
#include "MetalQueryPool.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cstring>

namespace GameEngine
{
namespace Rendering
{

MetalCommandList::MetalCommandList(MetalDevice& device, IDevice::QueueType queue)
    : m_Device(device), m_Queue(queue), m_IndirectCount(device)
{
}

MetalCommandList::~MetalCommandList()
{
    EndActiveEncoders();
    if (m_CommandBuffer != nullptr)
    {
        m_CommandBuffer->release();
        m_CommandBuffer = nullptr;
    }
}

void MetalCommandList::Begin()
{
    if (m_IsRecording)
    {
        return;
    }
    MTL::CommandQueue* queue = m_Device.GetQueue(m_Queue);
    if (queue == nullptr)
    {
        Logger::Log::Error("MetalCommandList: no command queue available");
        return;
    }
    if (m_CommandBuffer != nullptr)
    {
        m_CommandBuffer->release();
    }
    // GE_METAL_CB_ERRORS=1: attribute GPU faults to the encoder that caused
    // them (the completed-handler error then carries per-encoder info).
    static const bool kEncoderErrorInfo = []() {
        const char* env = std::getenv("GE_METAL_CB_ERRORS");
        return env != nullptr && env[0] != '0';
    }();
    if (kEncoderErrorInfo)
    {
        MTL::CommandBufferDescriptor* desc = MTL::CommandBufferDescriptor::alloc()->init();
        desc->setErrorOptions(MTL::CommandBufferErrorOptionEncoderExecutionStatus);
        m_CommandBuffer = queue->commandBuffer(desc); // autoreleased
        desc->release();
    }
    else
    {
        m_CommandBuffer = queue->commandBuffer(); // autoreleased
    }
    if (m_CommandBuffer == nullptr)
    {
        Logger::Log::Error("MetalCommandList: failed to obtain MTLCommandBuffer");
        return;
    }
    m_CommandBuffer->retain();
    m_IsRecording = true;
    m_CurrentPipelineHandle = PipelineHandle{};
    m_IndexBuffer = BufferHandle{};
    m_PushConstantDirtyBytes = 0;
    m_PendingTimestampQueries.clear();
    m_OpenEncoderStartSample = UINT32_MAX;
    m_OpenEncoderEndSample = UINT32_MAX;
    m_LastEndSample = UINT32_MAX;
    // The GPU-driven fence orders encoders within this command buffer only.
    m_GpuDrivenFenceSignaled = false;
    m_IndirectCount.BeginCommandBuffer();
}

void MetalCommandList::End()
{
    EndActiveEncoders();
    FlushPendingTimestampQueries();
    m_IsRecording = false;
}

void MetalCommandList::SetVisibilityResultMode(uint32_t mode, uint64_t offsetBytes)
{
    if (m_RenderEncoder == nullptr)
    {
        return;
    }
    m_RenderEncoder->setVisibilityResultMode(static_cast<MTL::VisibilityResultMode>(mode),
                                             static_cast<NS::UInteger>(offsetBytes));
}

void MetalCommandList::RecordTimestampQuery(MetalQueryPool& pool, uint32_t queryIndex,
                                            TimestampPoint point)
{
    m_TimestampPool = &pool;
    const bool encoderOpen =
        m_RenderEncoder != nullptr || m_BlitEncoder != nullptr || m_ComputeEncoder != nullptr;

    if (point == TimestampPoint::SpanBegin)
    {
        // Which encoder carries this span's work is not knowable yet: an
        // encoder open right now may be ended BY that work (a render pass ends
        // the open blit encoder), and the work may equally land in an encoder
        // that does not exist yet. Defer to whichever encoder opens first, and
        // resolve the "no new encoder" case at the span's end.
        m_PendingTimestampQueries.push_back({queryIndex, m_EncoderWorkMark});
        return;
    }

    // SpanEnd: the bracketed work is recorded, so the last boundary the GPU
    // reaches for it is the open encoder's end, else the last closed one's.
    const uint32_t endSlot = (encoderOpen && m_OpenEncoderEndSample != UINT32_MAX)
                                 ? m_OpenEncoderEndSample
                                 : m_LastEndSample;
    if (endSlot == UINT32_MAX)
    {
        // Nothing has been sampled in this command list yet. Defer the end too,
        // so it lands on the same boundary as its still-pending begin.
        m_PendingTimestampQueries.push_back({queryIndex, m_EncoderWorkMark});
        return;
    }

    // Reaching here with the begin still pending means no encoder opened inside
    // the span (one that did would have claimed the begin at creation, and this
    // branch is then skipped). So either the work went into an encoder that was
    // already open — the span is that encoder's, shared with whoever else
    // recorded into it — or the span recorded no GPU work at all and has nothing
    // to measure, reported as a zero span rather than as the gap to the next
    // encoder.
    if (!m_PendingTimestampQueries.empty())
    {
        const PendingSpanBegin begin = m_PendingTimestampQueries.back();
        m_PendingTimestampQueries.pop_back();
        const bool recordedWork = m_EncoderWorkMark != begin.WorkMark;
        const bool sharedOpenEncoder =
            recordedWork && encoderOpen && m_OpenEncoderStartSample != UINT32_MAX;
        pool.BindQueryToSample(begin.Query, sharedOpenEncoder ? m_OpenEncoderStartSample : endSlot);
    }
    pool.BindQueryToSample(queryIndex, endSlot);
}

bool MetalCommandList::TryBeginEncoderSamples(bool splitStages, MTL::CounterSampleBuffer*& outBuffer,
                                              MetalQueryPool::EncoderSampleSlots& outSlots)
{
    if (m_TimestampPool == nullptr || !m_TimestampPool->ShouldAttachSamples())
    {
        return false;
    }
    outBuffer = m_TimestampPool->GetCurrentSampleBuffer();
    if (outBuffer == nullptr || !m_TimestampPool->AllocateEncoderSampleSlots(splitStages, outSlots))
    {
        // Out of sample budget: pending queries stay unresolved this frame.
        m_PendingTimestampQueries.clear();
        return false;
    }
    for (const PendingSpanBegin& pending : m_PendingTimestampQueries)
    {
        m_TimestampPool->BindQueryToSample(pending.Query, outSlots.Start);
    }
    m_PendingTimestampQueries.clear();
    m_OpenEncoderStartSample = outSlots.Start;
    m_OpenEncoderEndSample = outSlots.End;
    return true;
}

void MetalCommandList::LatchEncoderEndSample()
{
    if (m_OpenEncoderEndSample != UINT32_MAX)
    {
        m_LastEndSample = m_OpenEncoderEndSample;
        m_OpenEncoderEndSample = UINT32_MAX;
    }
    m_OpenEncoderStartSample = UINT32_MAX;
}

void MetalCommandList::FlushPendingTimestampQueries()
{
    if (m_TimestampPool == nullptr || m_PendingTimestampQueries.empty())
    {
        return;
    }
    if (m_LastEndSample != UINT32_MAX)
    {
        // Spans left open at End() never got an encoder — zero, not the tail
        // of whatever ran last.
        for (const PendingSpanBegin& pending : m_PendingTimestampQueries)
        {
            m_TimestampPool->BindQueryToSample(pending.Query, m_LastEndSample);
        }
    }
    m_PendingTimestampQueries.clear();
}

MTL::CommandBuffer* MetalCommandList::Detach()
{
    if (m_CommandBuffer == nullptr)
    {
        return nullptr;
    }
    EndActiveEncoders();
    FlushPendingTimestampQueries();
    m_IndirectCount.FinishCommandBuffer(m_CommandBuffer);
    MTL::CommandBuffer* cb = m_CommandBuffer;
    // GPU faults abort the command buffer and silently discard its writes —
    // surface them, they are otherwise invisible (no API error, no crash).
    cb->addCompletedHandler([](MTL::CommandBuffer* completed) {
        if (completed->status() == MTL::CommandBufferStatusError)
        {
            NS::Error* error = completed->error();
            Logger::Log::Error("MetalCommandList: command buffer '{}' failed on the GPU: {}",
                               completed->label() != nullptr ? completed->label()->utf8String() : "unnamed",
                               error != nullptr ? error->localizedDescription()->utf8String() : "unknown error");
            // With GE_METAL_CB_ERRORS=1 the error carries per-encoder fault
            // attribution (encoder label = the render-graph pass debug group).
            if (error != nullptr && error->userInfo() != nullptr)
            {
                auto* infos = static_cast<NS::Array*>(
                    error->userInfo()->object(MTL::CommandBufferEncoderInfoErrorKey));
                if (infos != nullptr)
                {
                    for (NS::UInteger i = 0; i < infos->count(); ++i)
                    {
                        auto* info = static_cast<MTL::CommandBufferEncoderInfo*>(infos->object(i));
                        if (info == nullptr || info->errorState() == MTL::CommandEncoderErrorStateCompleted)
                        {
                            continue;
                        }
                        std::string signposts;
                        if (NS::Array* posts = info->debugSignposts())
                        {
                            for (NS::UInteger p = 0; p < posts->count(); ++p)
                            {
                                if (auto* post = static_cast<NS::String*>(posts->object(p)))
                                {
                                    signposts += signposts.empty() ? "" : " > ";
                                    signposts += post->utf8String();
                                }
                            }
                        }
                        Logger::Log::Error("  encoder '{}' state={} signposts: {}",
                                           info->label() != nullptr ? info->label()->utf8String() : "unnamed",
                                           static_cast<int>(info->errorState()), signposts);
                    }
                }
            }
        }
    });
    // GE_METAL_DUMP_CB_TIMING=1: per-command-buffer GPU occupancy from the
    // free kernel/GPU timestamps — enough to localize GPU time and scheduling
    // bubbles without a full Xcode capture.
    static const bool kDumpCbTiming = []() {
        const char* env = std::getenv("GE_METAL_DUMP_CB_TIMING");
        return env != nullptr && env[0] != '0';
    }();
    if (kDumpCbTiming)
    {
        cb->addCompletedHandler([](MTL::CommandBuffer* completed) {
            const double gpuMs = (completed->GPUEndTime() - completed->GPUStartTime()) * 1000.0;
            const double schedMs = (completed->kernelEndTime() - completed->kernelStartTime()) * 1000.0;
            Logger::Log::Warning("MetalCbTiming: '{}' gpu={:.3f}ms sched={:.3f}ms gpuStart={:.6f}",
                                 completed->label() != nullptr ? completed->label()->utf8String() : "unnamed",
                                 gpuMs, schedMs, completed->GPUStartTime());
        });
    }
    m_CommandBuffer = nullptr;
    m_IsRecording = false;
    return cb; // ownership (+1) transfers to the caller, uncommitted
}

void MetalCommandList::EndActiveEncoders()
{
    if (m_RenderEncoder != nullptr)
    {
        if (m_InRenderPass)
        {
            // A blit/compute/clear request arrived between BeginRenderPass and
            // EndRenderPass. Vulkan command buffers allow that; Metal cannot —
            // ending the encoder here would silently drop every draw recorded
            // after the interruption.
            static bool s_Logged = false;
            if (!s_Logged)
            {
                Logger::Log::Error("MetalCommandList: render encoder interrupted mid-pass — subsequent draws in "
                                   "this pass will be dropped (mid-pass blit/compute/clear)");
                s_Logged = true;
            }
        }
        EndRenderEncoder();
        m_InRenderPass = false;
    }
    if (m_BlitEncoder != nullptr)
    {
        m_BlitEncoder->endEncoding();
        m_BlitEncoder->release();
        m_BlitEncoder = nullptr;
    }
    if (m_ComputeEncoder != nullptr)
    {
        // Residency-set path: signal that this compute's writes (draw-stream /
        // visibility via BDA) are done, so the next render/compute encoder can
        // order against them with waitForFence.
        if (m_Device.UsesResidencySet())
        {
            m_ComputeEncoder->updateFence(m_Device.GpuDrivenFence());
            m_GpuDrivenFenceSignaled = true;
        }
        m_ComputeEncoder->endEncoding();
        m_ComputeEncoder->release();
        m_ComputeEncoder = nullptr;
    }
    if (m_AccelerationStructureEncoder != nullptr)
    {
        m_AccelerationStructureEncoder->endEncoding();
        m_AccelerationStructureEncoder->release();
        m_AccelerationStructureEncoder = nullptr;
    }
    LatchEncoderEndSample();
}

MTL::BlitCommandEncoder* MetalCommandList::EnsureBlitEncoder()
{
    if (m_CommandBuffer == nullptr || !m_IsRecording)
    {
        return nullptr;
    }
    MarkEncoderWork();
    if (m_BlitEncoder != nullptr)
    {
        return m_BlitEncoder;
    }
    EndActiveEncoders();
    MTL::CounterSampleBuffer* sampleBuffer = nullptr;
    MetalQueryPool::EncoderSampleSlots slots;
    if (TryBeginEncoderSamples(/*splitStages=*/false, sampleBuffer, slots))
    {
        MTL::BlitPassDescriptor* bpd = MTL::BlitPassDescriptor::blitPassDescriptor(); // autoreleased
        auto* attachment = bpd->sampleBufferAttachments()->object(0);
        attachment->setSampleBuffer(sampleBuffer);
        attachment->setStartOfEncoderSampleIndex(slots.Start);
        attachment->setEndOfEncoderSampleIndex(slots.End);
        m_BlitEncoder = m_CommandBuffer->blitCommandEncoder(bpd); // autoreleased
    }
    else
    {
        m_BlitEncoder = m_CommandBuffer->blitCommandEncoder(); // autoreleased
    }
    if (m_BlitEncoder != nullptr)
    {
        m_BlitEncoder->retain();
    }
    return m_BlitEncoder;
}

MTL::ComputeCommandEncoder* MetalCommandList::EnsureComputeEncoder()
{
    if (m_CommandBuffer == nullptr || !m_IsRecording)
    {
        return nullptr;
    }
    MarkEncoderWork();
    if (m_ComputeEncoder != nullptr)
    {
        return m_ComputeEncoder;
    }
    EndActiveEncoders();
    // Any compute work may write draw records or counts.
    m_IndirectCount.InvalidateTranslation();
    MTL::CounterSampleBuffer* sampleBuffer = nullptr;
    MetalQueryPool::EncoderSampleSlots slots;
    if (TryBeginEncoderSamples(/*splitStages=*/false, sampleBuffer, slots))
    {
        MTL::ComputePassDescriptor* cpd = MTL::ComputePassDescriptor::computePassDescriptor(); // autoreleased
        auto* attachment = cpd->sampleBufferAttachments()->object(0);
        attachment->setSampleBuffer(sampleBuffer);
        attachment->setStartOfEncoderSampleIndex(slots.Start);
        attachment->setEndOfEncoderSampleIndex(slots.End);
        m_ComputeEncoder = m_CommandBuffer->computeCommandEncoder(cpd); // autoreleased
    }
    else
    {
        m_ComputeEncoder = m_CommandBuffer->computeCommandEncoder(); // autoreleased
    }
    if (m_ComputeEncoder != nullptr)
    {
        m_ComputeEncoder->retain();
        if (!m_CurrentDebugEventName.empty())
        {
            m_ComputeEncoder->setLabel(NS::String::string(m_CurrentDebugEventName.c_str(), NS::UTF8StringEncoding));
        }
        ResetEncoderBindingState();
        m_Device.DeclareDeviceAddressResidency(m_ComputeEncoder, m_EncoderResidents);
        // Residency-set path: order this compute after any prior compute that
        // wrote BDA buffers in this command buffer (the useResources tracking
        // that did this implicitly is gone).
        if (m_Device.UsesResidencySet() && m_GpuDrivenFenceSignaled)
        {
            m_ComputeEncoder->waitForFence(m_Device.GpuDrivenFence());
        }
    }
    return m_ComputeEncoder;
}

MTL::AccelerationStructureCommandEncoder* MetalCommandList::EnsureAccelerationStructureEncoder()
{
    if (m_CommandBuffer == nullptr || !m_IsRecording)
    {
        return nullptr;
    }
    MarkEncoderWork();
    if (m_AccelerationStructureEncoder != nullptr)
    {
        return m_AccelerationStructureEncoder;
    }
    EndActiveEncoders();
    m_AccelerationStructureEncoder = m_CommandBuffer->accelerationStructureCommandEncoder(); // autoreleased
    if (m_AccelerationStructureEncoder != nullptr)
    {
        m_AccelerationStructureEncoder->retain();
        if (!m_CurrentDebugEventName.empty())
        {
            m_AccelerationStructureEncoder->setLabel(
                NS::String::string(m_CurrentDebugEventName.c_str(), NS::UTF8StringEncoding));
        }
    }
    return m_AccelerationStructureEncoder;
}

// ---------------------------------------------------------------------------
// Render passes
// ---------------------------------------------------------------------------

void MetalCommandList::BeginRenderPass(const RenderPassDesc& desc)
{
    if (m_CommandBuffer == nullptr || !m_IsRecording)
    {
        return;
    }
    EndActiveEncoders();

    MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::alloc()->init();

    m_RenderTargetWidth = 0;
    m_RenderTargetHeight = 0;
    m_PassHasDepth = false;
    m_PassDepthReadOnly = false;
    m_PassFormatKey = {};

    // Per-attachment custom views (mip/layer/aspect selection) become
    // transient MTLTexture views, released once the frame's GPU work is done.
    auto resolveAttachmentTexture = [&](TextureHandle handle, bool useView,
                                        const TextureViewDesc& viewDesc) -> MTL::Texture* {
        MetalTexture* tex = m_Device.GetMetalTexture(handle);
        if (tex == nullptr || tex->texture == nullptr)
        {
            return nullptr;
        }
        if (!useView)
        {
            return tex->texture;
        }
        const uint32_t levelCount = viewDesc.levelCount != 0 ? viewDesc.levelCount : 1;
        const uint32_t layerCount = viewDesc.layerCount != 0 ? viewDesc.layerCount : 1;
        MTL::PixelFormat format = viewDesc.formatOverride != 0
                                      ? MetalMappings::ToMTLPixelFormat(static_cast<TextureFormat>(viewDesc.formatOverride))
                                      : tex->texture->pixelFormat();
        MTL::TextureType type = layerCount > 1 ? MTL::TextureType2DArray : MTL::TextureType2D;
        if (tex->sampleCount > 1)
        {
            type = MTL::TextureType2DMultisample;
        }
        MTL::Texture* view = tex->texture->newTextureView(format, type, NS::Range(viewDesc.baseMip, levelCount),
                                                          NS::Range(viewDesc.baseLayer, layerCount));
        if (view != nullptr)
        {
            m_Device.DeferRelease(view);
        }
        else
        {
            // A null view silently drops the whole attachment below, which
            // leaves an array layer neither cleared nor written — reverse-Z
            // reads that as 1.0 (fully occluding) rather than as "no shadow".
            // Name it: a texture missing PixelFormatView usage is the usual
            // cause, and the fix is at creation, not here.
            Logger::Log::Warning(
                "MetalCommandList: newTextureView failed (mip {}+{}, layer {}+{}, fmt {}) — "
                "attachment dropped; the texture likely lacks PixelFormatView usage",
                viewDesc.baseMip, levelCount, viewDesc.baseLayer, layerCount,
                static_cast<int>(format));
        }
        return view;
    };

    for (uint32_t i = 0; i < desc.colorTargetCount && i < 8; ++i)
    {
        MTL::Texture* texture =
            resolveAttachmentTexture(desc.colorTargets[i], desc.useColorView[i], desc.colorViewDesc[i]);
        MetalTexture* tex = m_Device.GetMetalTexture(desc.colorTargets[i]);
        if (texture == nullptr || tex == nullptr)
        {
            continue;
        }
        MTL::RenderPassColorAttachmentDescriptor* att = rpd->colorAttachments()->object(i);
        att->setTexture(texture);
        const RenderPassDesc::LoadOp loadOp =
            desc.clearColor[i] ? RenderPassDesc::LoadOp::Clear : desc.colorLoadOp[i];
        att->setLoadAction(MetalMappings::ToMTLLoadAction(loadOp));
        att->setClearColor(MTL::ClearColor(desc.clearColorValue[i][0], desc.clearColorValue[i][1],
                                           desc.clearColorValue[i][2], desc.clearColorValue[i][3]));

        MTL::StoreAction store = MetalMappings::ToMTLStoreAction(desc.colorStoreOp[i]);
        if (desc.resolveColorTargets[i].IsValid())
        {
            MTL::Texture* resolve = resolveAttachmentTexture(
                desc.resolveColorTargets[i], desc.useColorResolveView[i], desc.colorResolveViewDesc[i]);
            if (resolve != nullptr)
            {
                att->setResolveTexture(resolve);
                store = store == MTL::StoreActionStore ? MTL::StoreActionStoreAndMultisampleResolve
                                                       : MTL::StoreActionMultisampleResolve;
            }
        }
        att->setStoreAction(store);

        m_RenderTargetWidth = std::max(m_RenderTargetWidth, std::max(1u, tex->width >> desc.colorViewDesc[i].baseMip));
        m_RenderTargetHeight = std::max(m_RenderTargetHeight, std::max(1u, tex->height >> desc.colorViewDesc[i].baseMip));

        const uint32_t formatOverride = desc.useColorView[i] ? desc.colorViewDesc[i].formatOverride : 0u;
        m_PassFormatKey.ColorFormats[i] =
            formatOverride != 0 ? static_cast<TextureFormat>(formatOverride) : tex->format;
        m_PassFormatKey.ColorCount = static_cast<uint8_t>(i + 1);
        m_PassFormatKey.RasterizationSamples = static_cast<uint8_t>(std::max(1u, tex->sampleCount));
    }

    if (desc.depthTarget.IsValid())
    {
        MTL::Texture* depthTexture =
            resolveAttachmentTexture(desc.depthTarget, desc.useDepthView, desc.depthViewDesc);
        MetalTexture* depth = m_Device.GetMetalTexture(desc.depthTarget);
        if (depthTexture == nullptr || depth == nullptr)
        {
            // Dropping the depth attachment turns a clear+rasterize pass into a
            // no-op against its target: say so instead of rendering nothing.
            Logger::Log::Warning(
                "MetalCommandList: depth attachment dropped (useView={} baseLayer={}) — the pass "
                "will neither clear nor write its depth target",
                desc.useDepthView, desc.depthViewDesc.baseLayer);
        }
        if (depthTexture != nullptr && depth != nullptr)
        {
            MTL::RenderPassDepthAttachmentDescriptor* att = rpd->depthAttachment();
            att->setTexture(depthTexture);
            // Vulkan parity: read-only depth passes always LOAD — the engine
            // leaves clearDepth at its default in those descs and the Vulkan
            // backend overrides it (VulkanCommandList BeginRenderPass).
            const RenderPassDesc::LoadOp loadOp =
                desc.depthReadOnly ? RenderPassDesc::LoadOp::Load
                                   : (desc.clearDepth ? RenderPassDesc::LoadOp::Clear : desc.depthLoadOp);
            att->setLoadAction(MetalMappings::ToMTLLoadAction(loadOp));
            att->setClearDepth(static_cast<double>(desc.clearDepthValue));

            MTL::StoreAction store = MetalMappings::ToMTLStoreAction(desc.depthStoreOp);
            if (desc.resolveDepthTarget.IsValid())
            {
                MTL::Texture* resolve = resolveAttachmentTexture(desc.resolveDepthTarget, desc.useDepthResolveView,
                                                                 desc.depthResolveViewDesc);
                if (resolve != nullptr)
                {
                    att->setResolveTexture(resolve);
                    store = store == MTL::StoreActionStore ? MTL::StoreActionStoreAndMultisampleResolve
                                                           : MTL::StoreActionMultisampleResolve;
                    const RenderPassDesc::ResolveMode mode =
                        desc.useDepthResolveMode ? desc.depthResolveMode : RenderPassDesc::ResolveMode::SampleZero;
                    switch (mode)
                    {
                    case RenderPassDesc::ResolveMode::Min:
                        att->setDepthResolveFilter(MTL::MultisampleDepthResolveFilterMin);
                        break;
                    case RenderPassDesc::ResolveMode::Max:
                        att->setDepthResolveFilter(MTL::MultisampleDepthResolveFilterMax);
                        break;
                    case RenderPassDesc::ResolveMode::SampleZero:
                    case RenderPassDesc::ResolveMode::Average:
                    default:
                        att->setDepthResolveFilter(MTL::MultisampleDepthResolveFilterSample0);
                        break;
                    }
                }
            }
            att->setStoreAction(store);

            // Combined depth/stencil formats need the stencil attachment set
            // alongside depth or Metal validation rejects the pass.
            if (depthTexture->pixelFormat() == MTL::PixelFormatDepth32Float_Stencil8)
            {
                MTL::RenderPassStencilAttachmentDescriptor* stencil = rpd->stencilAttachment();
                stencil->setTexture(depthTexture);
                const RenderPassDesc::LoadOp stencilLoad =
                    desc.clearStencil ? RenderPassDesc::LoadOp::Clear : desc.stencilLoadOp;
                stencil->setLoadAction(MetalMappings::ToMTLLoadAction(stencilLoad));
                stencil->setStoreAction(MetalMappings::ToMTLStoreAction(desc.stencilStoreOp));
                stencil->setClearStencil(desc.clearStencilValue);
            }

            m_RenderTargetWidth = std::max(m_RenderTargetWidth, depth->width);
            m_RenderTargetHeight = std::max(m_RenderTargetHeight, depth->height);
            m_PassHasDepth = true;
            m_PassDepthReadOnly = desc.depthReadOnly;
            m_PassFormatKey.DepthFormat = depth->format;
            m_PassFormatKey.RasterizationSamples = static_cast<uint8_t>(std::max(1u, depth->sampleCount));
        }
    }

    static const bool kDumpPasses = []() {
        const char* env = std::getenv("GE_METAL_DUMP_PASSES");
        return env != nullptr && env[0] != '0';
    }();
    if (kDumpPasses)
    {
        Logger::Log::Warning(
            "MetalPass: colors={} depth={} readOnly={} clearDepth={} clearVal={} loadOp={} storeOp={}",
            desc.colorTargetCount, desc.depthTarget.IsValid(), desc.depthReadOnly, desc.clearDepth,
            desc.clearDepthValue, static_cast<int>(desc.depthLoadOp), static_cast<int>(desc.depthStoreOp));
    }

    {
        // All four stage boundaries: the vertex (tiling) and fragment stages of
        // one encoder are scheduled independently on Apple GPUs, so charging a
        // pass the whole vertex-start..fragment-end extent bills it for the wait
        // between them. Sampling each stage separately keeps the wait out.
        MTL::CounterSampleBuffer* sampleBuffer = nullptr;
        MetalQueryPool::EncoderSampleSlots slots;
        if (TryBeginEncoderSamples(/*splitStages=*/true, sampleBuffer, slots))
        {
            auto* attachment = rpd->sampleBufferAttachments()->object(0);
            attachment->setSampleBuffer(sampleBuffer);
            attachment->setStartOfVertexSampleIndex(slots.Start);
            attachment->setEndOfVertexSampleIndex(slots.FirstStageEnd);
            attachment->setStartOfFragmentSampleIndex(slots.SecondStageBegin);
            attachment->setEndOfFragmentSampleIndex(slots.End);
        }
    }

    // Attach the query pool's per-frame visibility-result buffer so occlusion
    // queries issued during this pass have a destination. Cheap pointer set;
    // nothing is written unless a draw runs under setVisibilityResultMode.
    if (IQueryPool* qp = m_Device.GetQueryPool())
    {
        if (MTL::Buffer* vb = static_cast<MetalQueryPool*>(qp)->GetCurrentVisibilityBuffer())
        {
            rpd->setVisibilityResultBuffer(vb);
        }
    }

    // Encoded ahead of the render encoder: the pass's DrawIndexedIndirectCount
    // commands come from it. It reads the draw records, so it orders after
    // device-address writers the way a render pass does (below).
    MTL::Fence* producerFence =
        m_Device.UsesResidencySet() && m_GpuDrivenFenceSignaled ? m_Device.GpuDrivenFence() : nullptr;
    m_IndirectCount.PrepareRenderPass(m_CommandBuffer, producerFence);
    MarkEncoderWork();
    m_RenderEncoder = m_CommandBuffer->renderCommandEncoder(rpd); // autoreleased
    rpd->release();
    if (m_RenderEncoder == nullptr)
    {
        Logger::Log::Error("MetalCommandList: failed to create render command encoder");
        return;
    }
    m_RenderEncoder->retain();
    if (!m_CurrentDebugEventName.empty())
    {
        m_RenderEncoder->setLabel(NS::String::string(m_CurrentDebugEventName.c_str(), NS::UTF8StringEncoding));
    }
    m_InRenderPass = true;

    ResetEncoderBindingState();
    m_Device.DeclareDeviceAddressResidency(m_RenderEncoder, m_EncoderResidents);
    // Residency-set path: order this render pass after any compute that wrote
    // BDA buffers it consumes (indirect draw args / instance data are read in
    // the vertex stage). Only waits when a compute has signalled in this CB.
    if (m_Device.UsesResidencySet() && m_GpuDrivenFenceSignaled)
    {
        m_RenderEncoder->waitForFence(m_Device.GpuDrivenFence(), MTL::RenderStageVertex);
    }

    // Reapply sticky state on the fresh encoder.
    for (uint32_t slot = 0; slot < kMaxVertexBufferSlots; ++slot)
    {
        if (!m_VertexBuffers[slot].IsValid())
        {
            continue;
        }
        MetalBuffer* buf = m_Device.GetMetalBuffer(m_VertexBuffers[slot]);
        if (buf != nullptr && buf->buffer != nullptr)
        {
            m_RenderEncoder->setVertexBuffer(buf->buffer, 0, MetalVertexBufferIndex(slot));
        }
    }
    ApplyRenderState();
    ApplyPushConstants();
}

void MetalCommandList::EndRenderPass()
{
    if (m_RenderEncoder != nullptr)
    {
        EndRenderEncoder();
        LatchEncoderEndSample();
    }
    m_InRenderPass = false;
}

void MetalCommandList::EndRenderEncoder()
{
    m_IndirectCount.EndRenderEncoder(m_RenderEncoder);
    m_RenderEncoder->endEncoding();
    m_RenderEncoder->release();
    m_RenderEncoder = nullptr;
}

void MetalCommandList::InvalidateIndirectCountTranslationIfWritten(const MetalBuffer* destination)
{
    if (destination != nullptr && (destination->usage & BufferUsage::Indirect) != BufferUsage::None)
    {
        m_IndirectCount.InvalidateTranslation();
    }
}

void MetalCommandList::ApplyRenderState()
{
    if (m_RenderEncoder == nullptr || !m_CurrentPipelineHandle.IsValid())
    {
        return;
    }
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    if (pipeline == nullptr || pipeline->renderPipeline == nullptr)
    {
        return;
    }
    // Sticky pipelines carry across pass boundaries (Vulkan only validates at
    // draw time); skip the reapply when the PSO was specialized for different
    // attachment formats — Metal validates at bind time. A compatible
    // SetPipeline always lands before the pass's draws.
    if (pipeline->formatKey != m_PassFormatKey)
    {
        return;
    }
    // Skip the redundant state binds when this PSO's full state is already
    // applied to the current encoder (same encoder => same pass-scoped depth/
    // format inputs, so the result is identical). Reset per encoder.
    if (m_AppliedRenderPipeline == m_CurrentPipelineHandle)
    {
        return;
    }
    m_RenderEncoder->setRenderPipelineState(pipeline->renderPipeline);
    // Binding a depth-writing DSS in a pass without a depth attachment is a
    // Metal validation error; the attachment-less default (Always/no-write)
    // is what such passes want anyway.
    if (pipeline->depthStencilState != nullptr && m_PassHasDepth)
    {
        // GE_METAL_FORCE_DEPTH_ALWAYS=1: bisection aid — replaces every
        // pipeline's depth compare with Always/write-on to separate "draw
        // produces no fragments" from "fragments rejected by the depth test".
        static const bool kForceAlways = []() {
            const char* env = std::getenv("GE_METAL_FORCE_DEPTH_ALWAYS");
            return env != nullptr && env[0] != '0';
        }();
        if (kForceAlways)
        {
            m_RenderEncoder->setDepthStencilState(m_Device.GetDebugAlwaysDepthState());
        }
        else if (m_PassDepthReadOnly)
        {
            // Vulkan parity: read-only depth passes force writes off
            // regardless of the pipeline's depthWriteEnable.
            m_RenderEncoder->setDepthStencilState(m_Device.GetReadOnlyDepthState(pipeline->depthCompare));
        }
        else
        {
            m_RenderEncoder->setDepthStencilState(pipeline->depthStencilState);
        }
    }
    m_RenderEncoder->setCullMode(pipeline->cullMode);
    m_RenderEncoder->setFrontFacingWinding(pipeline->winding);
    m_RenderEncoder->setTriangleFillMode(pipeline->fillMode);
    m_RenderEncoder->setDepthClipMode(pipeline->depthClipMode);
    // Vulkan bakes depth bias into the pipeline; Metal sets it per-encoder.
    if (pipeline->depthBiasEnable)
    {
        m_RenderEncoder->setDepthBias(pipeline->depthBiasConstant, pipeline->depthBiasSlope,
                                      pipeline->depthBiasClamp);
    }
    else
    {
        m_RenderEncoder->setDepthBias(0.0f, 0.0f, 0.0f);
    }
    m_AppliedRenderPipeline = m_CurrentPipelineHandle;
}

void MetalCommandList::ApplyPushConstants()
{
    if (m_PushConstantDirtyBytes == 0)
    {
        return;
    }
    if (m_RenderEncoder != nullptr)
    {
        m_RenderEncoder->setVertexBytes(m_PushConstantData.data(), m_PushConstantDirtyBytes, kPushConstantBufferIndex);
        m_RenderEncoder->setFragmentBytes(m_PushConstantData.data(), m_PushConstantDirtyBytes, kPushConstantBufferIndex);
    }
    if (m_ComputeEncoder != nullptr)
    {
        m_ComputeEncoder->setBytes(m_PushConstantData.data(), m_PushConstantDirtyBytes, kPushConstantBufferIndex);
    }
}

// ---------------------------------------------------------------------------
// Pipeline + vertex state
// ---------------------------------------------------------------------------

void MetalCommandList::SetPipeline(PipelineHandle pipeline)
{
    m_CurrentPipelineHandle = pipeline;
    const MetalPipeline* mp = m_Device.GetMetalPipeline(pipeline);
    if (mp == nullptr)
    {
        return;
    }
    if (mp->type == PipelineType::Compute)
    {
        MTL::ComputeCommandEncoder* enc = EnsureComputeEncoder();
        if (enc != nullptr && mp->computePipeline != nullptr)
        {
            enc->setComputePipelineState(mp->computePipeline);
        }
        return;
    }
    ApplyRenderState();
}

void MetalCommandList::SetVertexBuffer(BufferHandle buffer, uint32_t slot)
{
    if (slot < kMaxVertexBufferSlots)
    {
        m_VertexBuffers[slot] = buffer;
    }
    if (m_RenderEncoder == nullptr)
    {
        // No encoder yet — the bind is reapplied when the next render pass
        // opens (Vulkan binds persist across pass boundaries; Metal's don't).
        return;
    }
    MetalBuffer* buf = m_Device.GetMetalBuffer(buffer);
    if (buf != nullptr && buf->buffer != nullptr)
    {
        // Engine slot s -> Metal buffer index 29 - s; low indices belong to
        // descriptor-set argument buffers (see MetalArgumentBufferLayout.h).
        m_RenderEncoder->setVertexBuffer(buf->buffer, 0, MetalVertexBufferIndex(slot));
    }
}

void MetalCommandList::SetIndexBuffer(BufferHandle buffer, IndexType indexType)
{
    m_IndexBuffer = buffer;
    m_IndexType = MetalMappings::ToMTLIndexType(indexType);
}

void MetalCommandList::SetConstants(uint32_t slot, size_t size, const void* data)
{
    SetConstants(slot, 0, size, data);
}

void MetalCommandList::SetConstants(uint32_t /*slot*/, uint32_t offset, size_t size, const void* data)
{
    if (data == nullptr || offset + size > kMaxPushConstantBytes)
    {
        Logger::Log::Error("MetalCommandList::SetConstants: payload exceeds the 128-byte push constant policy");
        return;
    }
    std::memcpy(m_PushConstantData.data() + offset, data, size);
    m_PushConstantDirtyBytes = std::max(m_PushConstantDirtyBytes, static_cast<uint32_t>(offset + size));
    ApplyPushConstants();
}

bool MetalCommandList::SetPushConstantsByName(const char* rangeName, const void* data, size_t size, uint32_t offset)
{
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    if (pipeline == nullptr || rangeName == nullptr)
    {
        return false;
    }
    for (const auto& range : pipeline->pushRanges)
    {
        if (range.name == rangeName)
        {
            SetConstants(0, range.offset + offset, size, data);
            return true;
        }
    }
    static bool s_LoggedMiss = false;
    if (!s_LoggedMiss)
    {
        Logger::Log::Warning("MetalCommandList: push constant range '{}' not found on pipeline '{}' ({} ranges)",
                             rangeName, pipeline->debugName, pipeline->pushRanges.size());
        s_LoggedMiss = true;
    }
    return false;
}

bool MetalCommandList::SetPushConstantsById(uint32_t rangeId, const void* data, size_t size, uint32_t offset)
{
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    if (pipeline == nullptr || rangeId >= pipeline->pushRanges.size())
    {
        return false;
    }
    const auto& range = pipeline->pushRanges[rangeId];
    SetConstants(0, range.offset + offset, size, data);
    return true;
}

namespace
{
// GE_METAL_VALIDATE_BINDINGS=1: compare the bound set's slot assignment with
// the layout the pipeline was translated against. A divergence means the GPU
// reads a different struct position than the CPU wrote — exactly the class of
// bug that renders garbage without any API validation error.
void ValidateSetAgainstPipeline(uint32_t setIndex, const MetalDescriptorSet& ds, const MetalPipeline& pipeline)
{
    static const bool kValidate = []() {
        const char* env = std::getenv("GE_METAL_VALIDATE_BINDINGS");
        return env != nullptr && env[0] != '0';
    }();
    if (!kValidate || setIndex >= pipeline.setLayouts.size())
    {
        return;
    }
    const MetalArgumentBufferLayout& expected = pipeline.setLayouts[setIndex];
    for (const MetalArgumentSlot& want : expected.Slots)
    {
        const MetalArgumentSlot* have = ds.layout.FindBinding(want.Binding);
        if (have == nullptr)
        {
            Logger::Log::Error(
                "MetalBindingValidate: pipeline '{}' set {} expects binding {} (type {}) but the bound set "
                "'{}' does not contain it",
                pipeline.debugName, setIndex, want.Binding, static_cast<int>(want.Type), ds.debugName);
            continue;
        }
        if (have->BufferId != want.BufferId || have->TextureId != want.TextureId ||
            have->SamplerId != want.SamplerId ||
            have->AccelerationStructureId != want.AccelerationStructureId ||
            have->Count != want.Count || have->Type != want.Type)
        {
            Logger::Log::Error(
                "MetalBindingValidate: pipeline '{}' set {} binding {}: shader expects ids buf={} tex={} smp={} "
                "as={} n={} type={} but set '{}' wrote buf={} tex={} smp={} as={} n={} type={}",
                pipeline.debugName, setIndex, want.Binding, want.BufferId, want.TextureId, want.SamplerId,
                want.AccelerationStructureId, want.Count, static_cast<int>(want.Type), ds.debugName,
                have->BufferId, have->TextureId, have->SamplerId, have->AccelerationStructureId,
                have->Count, static_cast<int>(have->Type));
        }
    }
}
} // namespace

void MetalCommandList::BindDescriptorSet(uint32_t set, DescriptorSetHandle descriptorSet, PipelineHandle pipeline)
{
    MetalDescriptorSet* ds = m_Device.GetMetalDescriptorSet(descriptorSet);
    if (ds == nullptr || ds->argumentBuffer == nullptr)
    {
        return;
    }
    if (const MetalPipeline* mp = m_Device.GetMetalPipeline(pipeline))
    {
        ValidateSetAgainstPipeline(set, *ds, *mp);
    }

    if (ds->residentsDirty)
    {
        ds->RebuildCompactResidents();
    }

    // GE_METAL_DUMP_CAMERA=1: print the first uVP column of every uniform
    // buffer bound to set 0 in depth-touching render passes — distinguishes
    // "prepass and forward read different camera data" from shader-side
    // depth differences. Shared storage makes the bytes CPU-readable.
    static const bool kDumpCamera = []() {
        const char* env = std::getenv("GE_METAL_DUMP_CAMERA");
        return env != nullptr && env[0] != '0';
    }();
    if (kDumpCamera && m_RenderEncoder != nullptr && m_PassHasDepth && set == 0)
    {
        static int s_Budget = 60;
        if (s_Budget > 0)
        {
            const uint64_t* entries = ds->entries;
            for (const MetalArgumentSlot& slot : ds->layout.Slots)
            {
                if (slot.Type != DescriptorType::UniformBuffer || slot.BufferId == MetalArgumentSlot::kUnused)
                {
                    continue;
                }
                const uint64_t address = entries[slot.BufferId];
                uint64_t offset = 0;
                MTL::Buffer* buffer = m_Device.FindBufferByGpuAddress(address, offset);
                if (buffer == nullptr || offset + 144 > buffer->length())
                {
                    continue;
                }
                float vp[4] = {0, 0, 0, 0};
                std::memcpy(vp, static_cast<const uint8_t*>(buffer->contents()) + offset + 128, sizeof(vp));
                Logger::Log::Warning("MetalCameraProbe: readOnly={} binding={} addr={:#x} uVPcol0=({:.9f},{:.9f},{:.9f},{:.9f})",
                                     m_PassDepthReadOnly, slot.Binding, address, vp[0], vp[1], vp[2], vp[3]);
                --s_Budget;
            }
        }
    }

    // Argument buffers carry resource handles, not the resources themselves —
    // hazard tracking still applies, but residency must be declared. Both the
    // binds and the declarations persist for the encoder's lifetime, so dedup
    // per encoder and batch new declarations into single useResources calls
    // (per-draw useResource repetition was the dominant GPU cost in many-draw
    // passes: UI Overlay measured 16.6ms vs Vulkan's 2.8ms).
    if (m_RenderEncoder == nullptr && m_ComputeEncoder == nullptr)
    {
        return;
    }
    // Switching between tables in one buffer (the frame's transient sets) only
    // moves the bound offset.
    const BoundArgumentTable bound = set < kMaxDescriptorSets ? m_EncoderBoundSets[set] : BoundArgumentTable{};
    const bool sameBuffer = bound.Buffer == ds->argumentBuffer;
    const bool alreadyBound = sameBuffer && bound.Offset == ds->argumentOffset;
    m_ScratchReadResidents.clear();
    m_ScratchWriteResidents.clear();
    for (const auto& [resource, writable] : ds->compactResidents)
    {
        const MTL::ResourceUsage usage = writable ? (MTL::ResourceUsageRead | MTL::ResourceUsageWrite)
                                                  : MTL::ResourceUsageRead;
        auto [it, inserted] = m_EncoderResidents.try_emplace(resource, usage);
        if (!inserted)
        {
            if ((it->second & usage) == usage)
            {
                continue;
            }
            it->second = static_cast<MTL::ResourceUsage>(it->second | usage);
        }
        (writable ? m_ScratchWriteResidents : m_ScratchReadResidents).push_back(resource);
    }

    if (m_RenderEncoder != nullptr)
    {
        if (sameBuffer && !alreadyBound)
        {
            m_RenderEncoder->setVertexBufferOffset(ds->argumentOffset, set);
            m_RenderEncoder->setFragmentBufferOffset(ds->argumentOffset, set);
        }
        else if (!sameBuffer)
        {
            m_RenderEncoder->setVertexBuffer(ds->argumentBuffer, ds->argumentOffset, set);
            m_RenderEncoder->setFragmentBuffer(ds->argumentBuffer, ds->argumentOffset, set);
        }
        // A pass that may write draw records or counts: later passes must not
        // draw from a translation encoded before it.
        if (ds->writesIndirectArguments)
        {
            m_IndirectCount.InvalidateTranslation();
        }
        constexpr MTL::RenderStages kAllStages = MTL::RenderStageVertex | MTL::RenderStageFragment;
        if (!m_ScratchReadResidents.empty())
        {
            m_RenderEncoder->useResources(m_ScratchReadResidents.data(), m_ScratchReadResidents.size(),
                                          MTL::ResourceUsageRead, kAllStages);
        }
        if (!m_ScratchWriteResidents.empty())
        {
            m_RenderEncoder->useResources(m_ScratchWriteResidents.data(), m_ScratchWriteResidents.size(),
                                          MTL::ResourceUsageRead | MTL::ResourceUsageWrite, kAllStages);
        }
    }
    else
    {
        if (sameBuffer && !alreadyBound)
        {
            m_ComputeEncoder->setBufferOffset(ds->argumentOffset, set);
        }
        else if (!sameBuffer)
        {
            m_ComputeEncoder->setBuffer(ds->argumentBuffer, ds->argumentOffset, set);
        }
        if (!m_ScratchReadResidents.empty())
        {
            m_ComputeEncoder->useResources(m_ScratchReadResidents.data(), m_ScratchReadResidents.size(),
                                           MTL::ResourceUsageRead);
        }
        if (!m_ScratchWriteResidents.empty())
        {
            m_ComputeEncoder->useResources(m_ScratchWriteResidents.data(), m_ScratchWriteResidents.size(),
                                           MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
        }
    }
    if (set < kMaxDescriptorSets)
    {
        m_EncoderBoundSets[set] = {ds->argumentBuffer, ds->argumentOffset};
    }
}

void MetalCommandList::ResetEncoderBindingState()
{
    m_EncoderBoundSets.fill({});
    m_EncoderResidents.clear();
    // A fresh encoder carries no pipeline state — force the next ApplyRenderState
    // to re-apply rather than skip on a stale match.
    m_AppliedRenderPipeline = {};
}

// ---------------------------------------------------------------------------
// Draws / dispatch
// ---------------------------------------------------------------------------

void MetalCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount)
{
    Draw(vertexCount, instanceCount, 0, 0);
}

void MetalCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    if (m_RenderEncoder == nullptr)
    {
        return;
    }
    MarkEncoderWork();
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    const MTL::PrimitiveType prim = pipeline != nullptr ? pipeline->primitiveType : MTL::PrimitiveTypeTriangle;
    m_RenderEncoder->drawPrimitives(prim, static_cast<NS::UInteger>(firstVertex),
                                    static_cast<NS::UInteger>(vertexCount),
                                    static_cast<NS::UInteger>(instanceCount),
                                    static_cast<NS::UInteger>(firstInstance));
}

void MetalCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount)
{
    DrawIndexed(indexCount, instanceCount, 0, 0, 0);
}

void MetalCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                   int32_t vertexOffset, uint32_t firstInstance)
{
    if (m_RenderEncoder == nullptr)
    {
        return;
    }
    MarkEncoderWork();
    MetalBuffer* indexBuffer = m_Device.GetMetalBuffer(m_IndexBuffer);
    if (indexBuffer == nullptr || indexBuffer->buffer == nullptr)
    {
        Logger::Log::Error("MetalCommandList::DrawIndexed: no index buffer bound");
        return;
    }
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    const MTL::PrimitiveType prim = pipeline != nullptr ? pipeline->primitiveType : MTL::PrimitiveTypeTriangle;
    const size_t indexSize = (m_IndexType == MTL::IndexTypeUInt16) ? 2 : 4;
    m_RenderEncoder->drawIndexedPrimitives(prim, static_cast<NS::UInteger>(indexCount), m_IndexType,
                                           indexBuffer->buffer,
                                           static_cast<NS::UInteger>(firstIndex * indexSize),
                                           static_cast<NS::UInteger>(instanceCount),
                                           static_cast<NS::Integer>(vertexOffset),
                                           static_cast<NS::UInteger>(firstInstance));
}

void MetalCommandList::DrawMeshTasks(uint32_t taskCount)
{
    if (m_RenderEncoder == nullptr || taskCount == 0)
    {
        return;
    }
    MarkEncoderWork();
    const MetalPipeline* mp = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    if (mp == nullptr || !mp->isMeshPipeline)
    {
        static bool s_Logged = false;
        if (!s_Logged)
        {
            Logger::Log::Error("MetalCommandList::DrawMeshTasks: no mesh pipeline bound");
            s_Logged = true;
        }
        return;
    }
    // taskCount mesh threadgroups; mesh-only pipelines use a 1-thread object
    // stage, and threads-per-mesh is the mesh shader's workgroup size.
    const MTL::Size grid = MTL::Size::Make(taskCount, 1, 1);
    const MTL::Size objectThreads = MTL::Size::Make(1, 1, 1);
    const MTL::Size meshThreads = MTL::Size::Make(std::max(1u, mp->localSizeX),
                                                  std::max(1u, mp->localSizeY),
                                                  std::max(1u, mp->localSizeZ));
    m_RenderEncoder->drawMeshThreadgroups(grid, objectThreads, meshThreads);
}

void MetalCommandList::DrawIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride)
{
    if (m_RenderEncoder == nullptr)
    {
        return;
    }
    MarkEncoderWork();
    MetalBuffer* buf = m_Device.GetMetalBuffer(commandBuffer);
    if (buf == nullptr || buf->buffer == nullptr)
    {
        return;
    }
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    const MTL::PrimitiveType prim = pipeline != nullptr ? pipeline->primitiveType : MTL::PrimitiveTypeTriangle;
    for (uint32_t i = 0; i < drawCount; ++i)
    {
        m_RenderEncoder->drawPrimitives(prim, buf->buffer, static_cast<NS::UInteger>(i) * stride);
    }
}

void MetalCommandList::DrawIndexedIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride)
{
    if (m_RenderEncoder == nullptr)
    {
        return;
    }
    MarkEncoderWork();
    MetalBuffer* cmdBuf = m_Device.GetMetalBuffer(commandBuffer);
    MetalBuffer* indexBuffer = m_Device.GetMetalBuffer(m_IndexBuffer);
    if (cmdBuf == nullptr || cmdBuf->buffer == nullptr || indexBuffer == nullptr || indexBuffer->buffer == nullptr)
    {
        return;
    }
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    const MTL::PrimitiveType prim = pipeline != nullptr ? pipeline->primitiveType : MTL::PrimitiveTypeTriangle;
    for (uint32_t i = 0; i < drawCount; ++i)
    {
        m_RenderEncoder->drawIndexedPrimitives(prim, m_IndexType, indexBuffer->buffer, 0, cmdBuf->buffer,
                                               static_cast<NS::UInteger>(i) * stride);
    }
}

namespace
{
// Bytes of one draw record: VkDrawIndexedIndirectCommand, which is also
// MTLDrawIndexedPrimitivesIndirectArguments.
constexpr uint32_t kDrawRecordBytes = 5 * sizeof(uint32_t);

// The Vulkan valid-usage rules for vkCmdDrawIndexedIndirectCount that keep the
// translate kernel's reads inside the caller's buffers. Returns why the draw is
// invalid, or nullptr.
const char* FindIndirectCountDrawError(const MetalBuffer* records, const MetalBuffer* count,
                                       const MetalBuffer* indices, uint32_t maxDrawCount, uint32_t stride,
                                       size_t recordsOffset, size_t countOffset)
{
    if (records == nullptr || records->buffer == nullptr || count == nullptr || count->buffer == nullptr)
    {
        return "record or count buffer is not a live buffer";
    }
    if (indices == nullptr || indices->buffer == nullptr)
    {
        return "no index buffer bound";
    }
    if ((records->usage & BufferUsage::Indirect) == BufferUsage::None ||
        (count->usage & BufferUsage::Indirect) == BufferUsage::None)
    {
        return "record and count buffers must be created with BufferUsage::Indirect";
    }
    if (stride < kDrawRecordBytes || stride % sizeof(uint32_t) != 0 || recordsOffset % sizeof(uint32_t) != 0 ||
        countOffset % sizeof(uint32_t) != 0)
    {
        return "stride must be at least 20 bytes, and stride and offsets multiples of 4";
    }
    const size_t recordsEnd = recordsOffset + static_cast<size_t>(maxDrawCount - 1) * stride + kDrawRecordBytes;
    if (recordsEnd > records->size || countOffset + sizeof(uint32_t) > count->size)
    {
        return "maxDrawCount records or the count lie outside their buffers";
    }
    return nullptr;
}
} // namespace

void MetalCommandList::DrawIndexedIndirectCount(BufferHandle commandBuffer, BufferHandle countBuffer,
                                                uint32_t maxDrawCount, uint32_t stride, size_t commandBufferOffset,
                                                size_t countBufferOffset)
{
    if (m_RenderEncoder == nullptr)
    {
        return;
    }
    MarkEncoderWork();
    if (maxDrawCount == 0)
    {
        return;
    }
    const MetalBuffer* records = m_Device.GetMetalBuffer(commandBuffer);
    const MetalBuffer* count = m_Device.GetMetalBuffer(countBuffer);
    const MetalBuffer* indices = m_Device.GetMetalBuffer(m_IndexBuffer);
    if (const char* error = FindIndirectCountDrawError(records, count, indices, maxDrawCount, stride,
                                                        commandBufferOffset, countBufferOffset))
    {
        Logger::Log::Error("MetalCommandList::DrawIndexedIndirectCount: {}; the draw is skipped", error);
        return;
    }
    // The commands reach the index buffer through its GPU address, which
    // Metal does not see; declare it like an argument-buffer resource.
    if (m_EncoderResidents.try_emplace(indices->buffer, MTL::ResourceUsageRead).second)
    {
        m_RenderEncoder->useResource(indices->buffer, MTL::ResourceUsageRead, MTL::RenderStageVertex);
    }
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    MetalIndirectCountEncoder::Draw draw{};
    draw.Records = records;
    draw.RecordsOffset = commandBufferOffset;
    draw.RecordStride = stride;
    draw.Count = count;
    draw.CountOffset = countBufferOffset;
    draw.MaxDrawCount = maxDrawCount;
    draw.Indices = indices;
    draw.IndexType = m_IndexType;
    draw.PrimitiveType = pipeline != nullptr ? pipeline->primitiveType : MTL::PrimitiveTypeTriangle;
    m_IndirectCount.EncodeDraw(m_RenderEncoder, draw);
}

void MetalCommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z)
{
    MTL::ComputeCommandEncoder* enc = EnsureComputeEncoder();
    if (enc == nullptr)
    {
        return;
    }
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    if (pipeline == nullptr || pipeline->computePipeline == nullptr)
    {
        Logger::Log::Error("MetalCommandList::Dispatch: no compute pipeline bound");
        return;
    }
    enc->setComputePipelineState(pipeline->computePipeline);
    ApplyPushConstants();
    // Engine Dispatch() takes threadgroup counts (like vkCmdDispatch); the
    // per-group size is the shader's SPIR-V workgroup size, captured at
    // translation time. Hand-written MSL kernels fall back to a 1D group
    // sized by the PSO's execution width.
    NS::UInteger w = pipeline->localSizeX;
    NS::UInteger h = pipeline->localSizeY;
    NS::UInteger d = pipeline->localSizeZ;
    if (w == 0)
    {
        w = pipeline->computePipeline->threadExecutionWidth();
        h = 1;
        d = 1;
    }
    enc->dispatchThreadgroups(MTL::Size(x, y, z), MTL::Size(w, std::max<NS::UInteger>(1, h), std::max<NS::UInteger>(1, d)));
}

void MetalCommandList::DispatchIndirect(BufferHandle argsBuffer, size_t argsOffsetBytes)
{
    MTL::ComputeCommandEncoder* enc = EnsureComputeEncoder();
    if (enc == nullptr)
    {
        return;
    }
    const MetalPipeline* pipeline = m_Device.GetMetalPipeline(m_CurrentPipelineHandle);
    if (pipeline == nullptr || pipeline->computePipeline == nullptr)
    {
        Logger::Log::Error("MetalCommandList::DispatchIndirect: no compute pipeline bound");
        return;
    }
    MetalBuffer* args = m_Device.GetMetalBuffer(argsBuffer);
    if (args == nullptr || args->buffer == nullptr)
    {
        return;
    }
    enc->setComputePipelineState(pipeline->computePipeline);
    ApplyPushConstants();
    // The indirect buffer supplies groupCountX/Y/Z (VkDispatchIndirectCommand
    // layout); threadsPerThreadgroup is still the PSO's local size — same
    // resolution as Dispatch(), with the execution-width fallback for
    // hand-written MSL kernels that don't carry a SPIR-V workgroup size.
    NS::UInteger w = pipeline->localSizeX;
    NS::UInteger h = pipeline->localSizeY;
    NS::UInteger d = pipeline->localSizeZ;
    if (w == 0)
    {
        w = pipeline->computePipeline->threadExecutionWidth();
        h = 1;
        d = 1;
    }
    enc->dispatchThreadgroups(args->buffer, static_cast<NS::UInteger>(argsOffsetBytes),
                              MTL::Size(w, std::max<NS::UInteger>(1, h), std::max<NS::UInteger>(1, d)));
}

// ---------------------------------------------------------------------------
// Copies / clears
// ---------------------------------------------------------------------------

void MetalCommandList::CopyBuffer(BufferHandle src, BufferHandle dst, size_t size, size_t srcOffset, size_t dstOffset)
{
    MTL::BlitCommandEncoder* enc = EnsureBlitEncoder();
    MetalBuffer* srcBuf = m_Device.GetMetalBuffer(src);
    MetalBuffer* dstBuf = m_Device.GetMetalBuffer(dst);
    if (enc == nullptr || srcBuf == nullptr || dstBuf == nullptr)
    {
        return;
    }
    InvalidateIndirectCountTranslationIfWritten(dstBuf);
    enc->copyFromBuffer(srcBuf->buffer, srcOffset, dstBuf->buffer, dstOffset, size);
}

void MetalCommandList::CopyTexture(TextureHandle src, TextureHandle dst)
{
    MTL::BlitCommandEncoder* enc = EnsureBlitEncoder();
    MetalTexture* srcTex = m_Device.GetMetalTexture(src);
    MetalTexture* dstTex = m_Device.GetMetalTexture(dst);
    if (enc == nullptr || srcTex == nullptr || dstTex == nullptr ||
        srcTex->texture == nullptr || dstTex->texture == nullptr)
    {
        return;
    }
    enc->copyFromTexture(srcTex->texture, dstTex->texture);
}

void MetalCommandList::FillBuffer(BufferHandle dst, size_t offset, size_t size, uint32_t value)
{
    MetalBuffer* dstBuf = m_Device.GetMetalBuffer(dst);
    if (dstBuf == nullptr || dstBuf->buffer == nullptr || size == 0)
    {
        return;
    }
    // MTLBlitCommandEncoder fills bytes; the engine contract fills u32s. Use
    // the blit fast path when the value byte-replicates, else a tiny compute
    // kernel writes the words.
    const uint8_t b0 = static_cast<uint8_t>(value & 0xFF);
    const bool byteReplicable = ((value >> 8) & 0xFF) == b0 && ((value >> 16) & 0xFF) == b0 && ((value >> 24) & 0xFF) == b0;
    if (byteReplicable)
    {
        MTL::BlitCommandEncoder* enc = EnsureBlitEncoder();
        if (enc != nullptr)
        {
            InvalidateIndirectCountTranslationIfWritten(dstBuf);
            enc->fillBuffer(dstBuf->buffer, NS::Range(offset, size), b0);
        }
        return;
    }

    MTL::ComputePipelineState* fill = m_Device.GetFillBufferPipeline();
    MTL::ComputeCommandEncoder* enc = EnsureComputeEncoder();
    if (fill == nullptr || enc == nullptr)
    {
        return;
    }
    struct
    {
        uint32_t value;
        uint32_t count;
    } params{value, static_cast<uint32_t>(size / 4)};
    enc->setComputePipelineState(fill);
    enc->setBuffer(dstBuf->buffer, offset, 0);
    enc->setBytes(&params, sizeof(params), 1);
    const NS::UInteger groupSize = 64;
    const NS::UInteger groups = (params.count + groupSize - 1) / groupSize;
    enc->dispatchThreadgroups(MTL::Size(groups, 1, 1), MTL::Size(groupSize, 1, 1));
    // The encoder state no longer matches the bound engine pipeline; force
    // re-binding on the next compute use.
    EndActiveEncoders();
}

void MetalCommandList::CopyTextureToBuffer(TextureHandle srcTexture, BufferHandle dstBuffer, uint32_t width,
                                           uint32_t height, uint32_t SrcX, uint32_t SrcY, size_t DstOffsetBytes,
                                           size_t dstRowPitchBytes)
{
    CopyTextureSubresourceToBuffer(srcTexture, 0, 0, dstBuffer, width, height, SrcX, SrcY, DstOffsetBytes,
                                   dstRowPitchBytes);
}

void MetalCommandList::CopyTextureSubresourceToBuffer(TextureHandle srcTexture, uint32_t mip, uint32_t layer,
                                                      BufferHandle dstBuffer, uint32_t width, uint32_t height,
                                                      uint32_t SrcX, uint32_t SrcY, size_t DstOffsetBytes,
                                                      size_t dstRowPitchBytes, uint32_t depth,
                                                      size_t dstSlicePitchBytes)
{
    MTL::BlitCommandEncoder* enc = EnsureBlitEncoder();
    MetalTexture* tex = m_Device.GetMetalTexture(srcTexture);
    MetalBuffer* buf = m_Device.GetMetalBuffer(dstBuffer);
    if (enc == nullptr || tex == nullptr || tex->texture == nullptr || buf == nullptr || buf->buffer == nullptr)
    {
        return;
    }
    InvalidateIndirectCountTranslationIfWritten(buf);
    const TightCopyRows tight = ComputeTightCopyRows(tex->format, width, height);
    const size_t rowPitch = dstRowPitchBytes != 0 ? dstRowPitchBytes : tight.RowBytes;
    const size_t slicePitch = dstSlicePitchBytes != 0 ? dstSlicePitchBytes : rowPitch * tight.RowCount;
    enc->copyFromTexture(tex->texture, layer, mip, MTL::Origin(SrcX, SrcY, 0), MTL::Size(width, height, depth),
                         buf->buffer, DstOffsetBytes, rowPitch, depth > 1 ? slicePitch : 0);

    // GE_METAL_DEPTH_PROBE=1: log the exact float at the readback's center
    // pixel once the GPU finishes — readback buffers are shared storage, so
    // this gives ulp-accurate depth values without a GPU capture.
    static const bool kDepthProbe = []() {
        const char* env = std::getenv("GE_METAL_DEPTH_PROBE");
        return env != nullptr && env[0] != '0';
    }();
    if (kDepthProbe && tex->format == TextureFormat::D32_FLOAT && m_CommandBuffer != nullptr)
    {
        MTL::Buffer* dstBuffer = buf->buffer;
        const size_t centerOffset = DstOffsetBytes + (height / 2) * rowPitch + (width / 2) * 4;
        const uint32_t w = width;
        const uint32_t h = height;
        m_CommandBuffer->addCompletedHandler([dstBuffer, centerOffset, w, h](MTL::CommandBuffer*) {
            float value = 0.0f;
            std::memcpy(&value, static_cast<const uint8_t*>(dstBuffer->contents()) + centerOffset, sizeof(value));
            Logger::Log::Warning("MetalDepthProbe: {}x{} center depth = {:.9f}", w, h, value);
        });
    }
}

void MetalCommandList::ClearColorImageSubresource(TextureHandle texture, uint32_t mip, uint32_t layer,
                                                  const float rgba[4])
{
    if (m_CommandBuffer == nullptr || !m_IsRecording)
    {
        return;
    }
    MetalTexture* tex = m_Device.GetMetalTexture(texture);
    if (tex == nullptr || tex->texture == nullptr)
    {
        return;
    }
    // The contract is a transfer clear (vkCmdClearColorImage on Vulkan), which
    // needs only TransferDst usage. A render-pass clear needs
    // MTLTextureUsageRenderTarget, so it serves only textures that declared
    // one; everything else is cleared by a blit, which needs no usage at all.
    if ((tex->texture->usage() & MTL::TextureUsageRenderTarget) == 0)
    {
        ClearColorImageSubresourceByBlit(*tex, mip, layer, rgba);
        return;
    }
    EndActiveEncoders();
    MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* att = rpd->colorAttachments()->object(0);
    att->setTexture(tex->texture);
    att->setLevel(mip);
    att->setSlice(layer);
    att->setLoadAction(MTL::LoadActionClear);
    att->setStoreAction(MTL::StoreActionStore);
    att->setClearColor(MTL::ClearColor(rgba[0], rgba[1], rgba[2], rgba[3]));
    MTL::RenderCommandEncoder* enc = m_CommandBuffer->renderCommandEncoder(rpd);
    rpd->release();
    if (enc != nullptr)
    {
        enc->endEncoding();
    }
}

void MetalCommandList::ClearColorImageSubresourceByBlit(const MetalTexture& tex, uint32_t mip, uint32_t layer,
                                                        const float rgba[4])
{
    ClearTexel texel{};
    const uint32_t texelBytes = EncodeClearTexel(tex.format, rgba, texel);
    if (texelBytes == 0)
    {
        Logger::Log::Error("MetalCommandList::ClearColorImageSubresource: '{}' has no render-target usage and its "
                           "format cannot be cleared by a copy",
                           tex.debugName);
        return;
    }
    if (mip >= tex.mipLevels)
    {
        return;
    }
    const uint32_t width = std::max(1u, tex.width >> mip);
    const uint32_t height = std::max(1u, tex.height >> mip);
    // A volume's subresource is the whole mip (vkCmdClearColorImage clears
    // every depth slice of a 3D image's layer 0).
    const bool isVolume = tex.depth > 1;
    const uint32_t depth = isVolume ? std::max(1u, tex.depth >> mip) : 1u;
    const uint32_t slice = isVolume ? 0u : layer;

    // One band of rows, copied as many times as the subresource needs: a large
    // target costs a bounded staging buffer rather than one the size of the mip.
    constexpr size_t kMaxStagingBytes = size_t{1} << 20;
    const size_t rowBytes = static_cast<size_t>(width) * texelBytes;
    const uint32_t bandRows =
        static_cast<uint32_t>(std::clamp<size_t>(kMaxStagingBytes / rowBytes, 1, height));
    const size_t stagingBytes = rowBytes * bandRows;
    MTL::Buffer* staging = m_Device.GetMTLDevice()->newBuffer(stagingBytes, MTL::ResourceStorageModeShared);
    if (staging == nullptr)
    {
        Logger::Log::Error("MetalCommandList::ClearColorImageSubresource: staging allocation failed for '{}'",
                           tex.debugName);
        return;
    }
    auto* bytes = static_cast<uint8_t*>(staging->contents());
    for (size_t offset = 0; offset < stagingBytes; offset += texelBytes)
    {
        std::memcpy(bytes + offset, texel.data(), texelBytes);
    }

    MTL::BlitCommandEncoder* enc = EnsureBlitEncoder();
    if (enc == nullptr)
    {
        staging->release();
        return;
    }
    for (uint32_t z = 0; z < depth; ++z)
    {
        for (uint32_t y = 0; y < height; y += bandRows)
        {
            const uint32_t rows = std::min(bandRows, height - y);
            enc->copyFromBuffer(staging, 0, rowBytes, 0, MTL::Size(width, rows, 1), tex.texture, slice, mip,
                                MTL::Origin(0, y, z));
        }
    }
    // The copies read the staging buffer when the GPU runs them, so it lives
    // until this command buffer completes.
    m_CommandBuffer->addCompletedHandler([staging](MTL::CommandBuffer*) { staging->release(); });
}

void MetalCommandList::CopyBufferToTextureSubresource(BufferHandle srcBuffer, TextureHandle dstTexture, uint32_t mip,
                                                      uint32_t layer, uint32_t width, uint32_t height,
                                                      size_t srcOffsetBytes, size_t srcRowPitchBytes, uint32_t depth,
                                                      size_t srcSlicePitchBytes, uint32_t dstX, uint32_t dstY,
                                                      ResourceState currentState)
{
    // Metal tracks resource hazards implicitly within a command buffer, so the
    // caller-supplied layout is not needed to preserve contents on a partial copy.
    (void)currentState;
    MTL::BlitCommandEncoder* enc = EnsureBlitEncoder();
    MetalBuffer* buf = m_Device.GetMetalBuffer(srcBuffer);
    MetalTexture* tex = m_Device.GetMetalTexture(dstTexture);
    if (enc == nullptr || buf == nullptr || buf->buffer == nullptr || tex == nullptr || tex->texture == nullptr)
    {
        return;
    }
    const TightCopyRows tight = ComputeTightCopyRows(tex->format, width, height);
    const size_t rowPitch = srcRowPitchBytes != 0 ? srcRowPitchBytes : tight.RowBytes;
    const size_t slicePitch = srcSlicePitchBytes != 0 ? srcSlicePitchBytes : rowPitch * tight.RowCount;
    enc->copyFromBuffer(buf->buffer, srcOffsetBytes, rowPitch, depth > 1 ? slicePitch : 0,
                        MTL::Size(width, height, depth), tex->texture, layer, mip,
                        MTL::Origin(dstX, dstY, 0));
}

void MetalCommandList::BlitImageMip(TextureHandle texture, uint32_t srcMip, uint32_t dstMip, uint32_t layer)
{
    MetalTexture* tex = m_Device.GetMetalTexture(texture);
    if (m_CommandBuffer == nullptr || !m_IsRecording || tex == nullptr || tex->texture == nullptr)
    {
        return;
    }
    const uint32_t srcW = std::max(1u, tex->width >> srcMip);
    const uint32_t srcH = std::max(1u, tex->height >> srcMip);
    const uint32_t dstW = std::max(1u, tex->width >> dstMip);
    const uint32_t dstH = std::max(1u, tex->height >> dstMip);
    if (srcW == dstW && srcH == dstH)
    {
        MTL::BlitCommandEncoder* enc = EnsureBlitEncoder();
        if (enc != nullptr)
        {
            enc->copyFromTexture(tex->texture, layer, srcMip, MTL::Origin(0, 0, 0), MTL::Size(srcW, srcH, 1),
                                 tex->texture, layer, dstMip, MTL::Origin(0, 0, 0));
        }
        return;
    }

    // Scaled blit: MTLBlitCommandEncoder copies cannot scale (vkCmdBlitImage
    // can), so sample the source mip view and draw into the destination mip
    // view with an internal fullscreen-triangle pipeline.
    if ((tex->texture->usage() & MTL::TextureUsageRenderTarget) == 0)
    {
        Logger::Log::Error("MetalCommandList::BlitImageMip: scaled blit needs RenderTarget usage on '{}'",
                           tex->debugName);
        return;
    }
    MTL::RenderPipelineState* pipeline = m_Device.GetMipBlitPipeline(tex->texture->pixelFormat());
    MTL::SamplerState* sampler = m_Device.GetMipBlitSampler();
    if (pipeline == nullptr || sampler == nullptr)
    {
        return;
    }

    EndActiveEncoders();
    MTL::Texture* srcView = tex->texture->newTextureView(tex->texture->pixelFormat(), MTL::TextureType2D,
                                                         NS::Range::Make(srcMip, 1), NS::Range::Make(layer, 1));
    MTL::Texture* dstView = tex->texture->newTextureView(tex->texture->pixelFormat(), MTL::TextureType2D,
                                                         NS::Range::Make(dstMip, 1), NS::Range::Make(layer, 1));
    if (srcView == nullptr || dstView == nullptr)
    {
        if (srcView != nullptr)
        {
            srcView->release();
        }
        if (dstView != nullptr)
        {
            dstView->release();
        }
        return;
    }
    MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::alloc()->init();
    auto* attachment = rpd->colorAttachments()->object(0);
    attachment->setTexture(dstView);
    attachment->setLoadAction(MTL::LoadActionDontCare);
    attachment->setStoreAction(MTL::StoreActionStore);
    MTL::RenderCommandEncoder* blitEncoder = m_CommandBuffer->renderCommandEncoder(rpd); // autoreleased
    rpd->release();
    if (blitEncoder != nullptr)
    {
        static NS::String* const kMipBlitLabel =
            NS::String::string("GE MipBlit", NS::UTF8StringEncoding)->retain();
        blitEncoder->setLabel(kMipBlitLabel);
        blitEncoder->setRenderPipelineState(pipeline);
        blitEncoder->setFragmentTexture(srcView, 0);
        blitEncoder->setFragmentSamplerState(sampler, 0);
        blitEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle, static_cast<NS::UInteger>(0),
                                    static_cast<NS::UInteger>(3));
        blitEncoder->endEncoding();
    }
    // The command buffer retains the views until execution completes.
    srcView->release();
    dstView->release();
}

// ---------------------------------------------------------------------------
// Dynamic state
// ---------------------------------------------------------------------------

void MetalCommandList::SetViewport(float x, float y, float width, float height)
{
    if (m_RenderEncoder == nullptr)
    {
        return;
    }
    // Callers pass Vulkan-style positive-height rects; the engine's Y-up
    // convention is achieved in Vulkan with a negative viewport, which is
    // Metal's native NDC orientation — no flip here.
    MTL::Viewport viewport{};
    viewport.originX = static_cast<double>(x);
    viewport.originY = static_cast<double>(y);
    viewport.width = static_cast<double>(width);
    viewport.height = static_cast<double>(height);
    viewport.znear = 0.0;
    viewport.zfar = 1.0;
    m_RenderEncoder->setViewport(viewport);
}

void MetalCommandList::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (m_RenderEncoder == nullptr)
    {
        return;
    }
    // Metal validation requires the scissor rect to stay inside the render
    // target; clamp defensively.
    MTL::ScissorRect rect{};
    rect.x = std::min(x, m_RenderTargetWidth);
    rect.y = std::min(y, m_RenderTargetHeight);
    rect.width = std::min<uint32_t>(width, m_RenderTargetWidth - rect.x);
    rect.height = std::min<uint32_t>(height, m_RenderTargetHeight - rect.y);
    m_RenderEncoder->setScissorRect(rect);
}

void MetalCommandList::SetDepthBounds(float /*minDepth*/, float /*maxDepth*/)
{
    // Metal has no depth-bounds test; the engine gates usage on capabilities.
}

// ---------------------------------------------------------------------------
// Barriers — Metal's automatic hazard tracking covers tracked resources.
// ---------------------------------------------------------------------------

void MetalCommandList::Barrier(const ResourceBarrier& /*barrier*/)
{
}

void MetalCommandList::BarrierBatch(const std::vector<ResourceBarrier>& /*barriers*/)
{
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

void MetalCommandList::BeginEvent(const char* name)
{
    if (m_CommandBuffer == nullptr || name == nullptr)
    {
        return;
    }
    m_CurrentDebugEventName = name;
    NS::String* label = NS::String::string(name, NS::UTF8StringEncoding);
    if (m_RenderEncoder != nullptr)
    {
        m_RenderEncoder->pushDebugGroup(label);
    }
    else if (m_ComputeEncoder != nullptr)
    {
        m_ComputeEncoder->pushDebugGroup(label);
    }
    else if (m_BlitEncoder != nullptr)
    {
        m_BlitEncoder->pushDebugGroup(label);
    }
    else
    {
        m_CommandBuffer->pushDebugGroup(label);
    }
}

void MetalCommandList::EndEvent()
{
    if (m_RenderEncoder != nullptr)
    {
        m_RenderEncoder->popDebugGroup();
    }
    else if (m_ComputeEncoder != nullptr)
    {
        m_ComputeEncoder->popDebugGroup();
    }
    else if (m_BlitEncoder != nullptr)
    {
        m_BlitEncoder->popDebugGroup();
    }
    else if (m_CommandBuffer != nullptr)
    {
        m_CommandBuffer->popDebugGroup();
    }
    m_CurrentDebugEventName.clear();
}

void MetalCommandList::SetMarker(const char* name)
{
    if (name == nullptr)
    {
        return;
    }
    NS::String* label = NS::String::string(name, NS::UTF8StringEncoding);
    if (m_RenderEncoder != nullptr)
    {
        m_RenderEncoder->insertDebugSignpost(label);
    }
    else if (m_BlitEncoder != nullptr)
    {
        m_BlitEncoder->insertDebugSignpost(label);
    }
}

void MetalCommandList::ClearRenderTarget(TextureHandle target, float r, float g, float b, float a)
{
    const float rgba[4] = {r, g, b, a};
    ClearColorImageSubresource(target, 0, 0, rgba);
}

void MetalCommandList::GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight)
{
    outWidth = 0;
    outHeight = 0;
    MetalTexture* tex = m_Device.GetMetalTexture(texture);
    if (tex != nullptr)
    {
        outWidth = tex->width;
        outHeight = tex->height;
    }
}

} // namespace Rendering
} // namespace GameEngine
