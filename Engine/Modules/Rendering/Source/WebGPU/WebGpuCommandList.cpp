#include "WebGpuCommandList.h"

#include "WebGpuConversions.h"
#include "WebGpuDevice.h"
#include "WebGpuQueryPool.h"
#include "WebGpuUnsupported.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace GameEngine::Rendering
{
namespace
{
constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1u) / alignment * alignment;
}

// True the first time a draw is refused in the pass labelled `pass`, for every device and command
// list: each pass that draws without a pipeline logs once, however many frames repeat it.
bool FirstRefusalInPass(const std::string& pass)
{
    static std::mutex mutex;
    static std::unordered_set<std::string> reported;
    std::lock_guard<std::mutex> lock(mutex);
    return reported.emplace(pass).second;
}
} // namespace

WebGpuCommandList::WebGpuCommandList(WebGpuDevice& device, IDevice::QueueType queue)
    : m_Device(device), m_Queue(queue)
{
}

WebGpuCommandList::~WebGpuCommandList()
{
    ClosePasses();
    ReleaseTransientViews();
    ReleaseTransientBuffers();
    if (m_Encoder != nullptr)
    {
        wgpuCommandEncoderRelease(m_Encoder);
        m_Encoder = nullptr;
    }
    if (m_CommandBuffer != nullptr)
    {
        wgpuCommandBufferRelease(m_CommandBuffer);
        m_CommandBuffer = nullptr;
    }
}

void WebGpuCommandList::Begin()
{
    m_PendingBeginQuery = kNoTimestampQuery;
    m_LastPassEndQuery = kNoTimestampQuery;
    if (m_CommandBuffer != nullptr)
    {
        wgpuCommandBufferRelease(m_CommandBuffer);
        m_CommandBuffer = nullptr;
    }
    EnsureEncoder();
    m_IsRecording = true;
    m_CurrentPipeline = PipelineHandle{};
    m_AppliedRenderPipeline = PipelineHandle{};
    m_IndexBuffer = BufferHandle{};
    m_VertexBuffers.fill(BufferHandle{});
    m_BindGroups.fill(nullptr);
    m_PushConstantDirtyBytes = 0;
    m_HasViewport = false;
}

void WebGpuCommandList::End()
{
    if (!m_IsRecording)
    {
        return;
    }
    ClosePasses();

    if (m_Encoder != nullptr)
    {
        while (m_EncoderDebugDepth > 0)
        {
            wgpuCommandEncoderPopDebugGroup(m_Encoder);
            --m_EncoderDebugDepth;
        }
        WGPUCommandBufferDescriptor desc{};
        desc.label = WebGpu::MakeStringView("GameEngine CommandBuffer");
        m_CommandBuffer = wgpuCommandEncoderFinish(m_Encoder, &desc);
        wgpuCommandEncoderRelease(m_Encoder);
        m_Encoder = nullptr;
    }
    ReleaseTransientViews();
    ReleaseTransientBuffers();
    m_IsRecording = false;
}

WGPUCommandBuffer WebGpuCommandList::Detach()
{
    if (m_IsRecording)
    {
        End();
    }
    WGPUCommandBuffer buffer = m_CommandBuffer;
    m_CommandBuffer = nullptr;
    return buffer;
}

void WebGpuCommandList::NoteTimestamp(TimestampPoint point, uint32_t localQuery)
{
    auto* pool = static_cast<WebGpuQueryPool*>(m_Device.GetQueryPool());
    if (pool == nullptr || localQuery == kNoTimestampQuery)
        return;
    if (point == TimestampPoint::SpanBegin)
    {
        // A new span: passes recorded before it do not belong to it.
        m_LastPassEndQuery = kNoTimestampQuery;
        if (m_PendingBeginQuery == kNoTimestampQuery)
            m_PendingBeginQuery = localQuery;
        else
            pool->AliasQuery(localQuery, m_PendingBeginQuery); // nested begin at the same boundary
        return;
    }
    if (m_LastPassEndQuery != kNoTimestampQuery)
    {
        pool->AliasQuery(localQuery, m_LastPassEndQuery);
    }
    else if (m_PendingBeginQuery != kNoTimestampQuery)
    {
        // No pass between begin and end: a zero-length span, and the begin
        // query is never written either — both read as the next boundary.
        pool->AliasQuery(localQuery, m_PendingBeginQuery);
    }
}

bool WebGpuCommandList::BeginPassTimestampWrites(WGPUPassTimestampWrites& outWrites)
{
    auto* pool = static_cast<WebGpuQueryPool*>(m_Device.GetQueryPool());
    if (pool == nullptr || pool->QuerySet() == nullptr)
        return false;
    // The browser shim forwards the C-API "undefined" index verbatim and the
    // browser rejects it, so a pass always writes both ends: a scope begin
    // that is pending rides the beginning, otherwise the pass gets its own.
    uint32_t beginQuery = m_PendingBeginQuery;
    if (beginQuery == kNoTimestampQuery)
        beginQuery = pool->ReserveQuery();
    const uint32_t endQuery = pool->ReserveQuery();
    if (beginQuery == kNoTimestampQuery || endQuery == kNoTimestampQuery)
        return false; // slot full: the pass runs untimed
    outWrites = WGPUPassTimestampWrites{};
    outWrites.querySet = pool->QuerySet();
    outWrites.beginningOfPassWriteIndex = pool->AbsoluteIndex(beginQuery);
    outWrites.endOfPassWriteIndex = pool->AbsoluteIndex(endQuery);
    m_PendingBeginQuery = kNoTimestampQuery;
    m_LastPassEndQuery = endQuery;
    return true;
}

WGPUCommandEncoder WebGpuCommandList::GetOpenEncoderForTimestamp()
{
    if (m_RenderPass != nullptr || m_ComputePass != nullptr)
        return nullptr;
    EnsureEncoder();
    return m_Encoder;
}

void WebGpuCommandList::EnsureEncoder()
{
    if (m_Encoder != nullptr)
    {
        return;
    }
    WGPUCommandEncoderDescriptor desc{};
    desc.label = WebGpu::MakeStringView("GameEngine CommandEncoder");
    m_Encoder = wgpuDeviceCreateCommandEncoder(m_Device.GetWgpuDevice(), &desc);
}

void WebGpuCommandList::ClosePasses()
{
    if (m_RenderPass != nullptr)
    {
        while (m_RenderPassDebugDepth > 0)
        {
            wgpuRenderPassEncoderPopDebugGroup(m_RenderPass);
            --m_RenderPassDebugDepth;
        }
        wgpuRenderPassEncoderEnd(m_RenderPass);
        wgpuRenderPassEncoderRelease(m_RenderPass);
        m_RenderPass = nullptr;
        m_AppliedRenderPipeline = PipelineHandle{};
        m_PassDepthReadOnly = false;
    }
    CloseComputePass();
}

void WebGpuCommandList::CloseComputePass()
{
    if (m_ComputePass != nullptr)
    {
        while (m_ComputePassDebugDepth > 0)
        {
            wgpuComputePassEncoderPopDebugGroup(m_ComputePass);
            --m_ComputePassDebugDepth;
        }
        wgpuComputePassEncoderEnd(m_ComputePass);
        wgpuComputePassEncoderRelease(m_ComputePass);
        m_ComputePass = nullptr;
    }
}

WGPUComputePassEncoder WebGpuCommandList::EnsureComputePass()
{
    if (m_ComputePass != nullptr)
    {
        return m_ComputePass;
    }
    if (m_RenderPass != nullptr)
    {
        // A dispatch inside an engine render pass is a caller error; closing the
        // render pass keeps recording legal rather than dropping the work.
        WebGpuLogUnsupportedOnce("compute dispatch inside a render pass");
        ClosePasses();
    }
    EnsureEncoder();

    WGPUComputePassDescriptor desc{};
    desc.label = WebGpu::MakeStringView("ComputePass");
    WGPUPassTimestampWrites timestampWrites{};
    if (BeginPassTimestampWrites(timestampWrites))
        desc.timestampWrites = &timestampWrites;
    m_ComputePass = wgpuCommandEncoderBeginComputePass(m_Encoder, &desc);

    // Pass encoders start stateless: reapply what the caller bound earlier.
    if (m_ComputePass != nullptr)
    {
        if (WebGpuPipeline* pipeline = m_Device.GetPipeline(m_CurrentPipeline))
        {
            if (pipeline->computePipeline != nullptr)
            {
                wgpuComputePassEncoderSetPipeline(m_ComputePass, pipeline->computePipeline);
            }
        }
        for (uint32_t set = 0; set < kMaxBindGroups; ++set)
        {
            if (m_BindGroups[set] != nullptr)
            {
                wgpuComputePassEncoderSetBindGroup(m_ComputePass, set, m_BindGroups[set], 0, nullptr);
            }
        }
    }
    return m_ComputePass;
}

void WebGpuCommandList::ReleaseTransientViews()
{
    for (WGPUTextureView view : m_TransientViews)
    {
        wgpuTextureViewRelease(view);
    }
    m_TransientViews.clear();
}

void WebGpuCommandList::ReleaseTransientBuffers()
{
    for (WGPUBuffer buffer : m_TransientBuffers)
    {
        wgpuBufferRelease(buffer);
    }
    m_TransientBuffers.clear();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

void WebGpuCommandList::SetPipeline(PipelineHandle pipeline)
{
    m_CurrentPipeline = pipeline;
    WebGpuPipeline* entry = m_Device.GetPipeline(pipeline);
    if (entry == nullptr)
    {
        return;
    }
    if (entry->type == PipelineType::Compute && m_ComputePass != nullptr && entry->computePipeline != nullptr)
    {
        wgpuComputePassEncoderSetPipeline(m_ComputePass, entry->computePipeline);
    }
    else if (entry->type == PipelineType::Graphics && m_RenderPass != nullptr)
    {
        ApplyRenderState();
    }
}

// `slot` is the engine's vertex binding (VertexLayoutBuilder.h). The WebGPU
// slot it lands in depends on the pipeline, so ApplyRenderState binds it before
// each draw.
void WebGpuCommandList::SetVertexBuffer(BufferHandle buffer, uint32_t slot)
{
    if (slot >= kMaxVertexBufferSlots)
    {
        return;
    }
    m_VertexBuffers[slot] = buffer;
}

void WebGpuCommandList::SetIndexBuffer(BufferHandle buffer, IndexType indexType)
{
    m_IndexBuffer = buffer;
    m_IndexType = indexType;
    if (m_RenderPass != nullptr)
    {
        if (WebGpuBuffer* entry = m_Device.GetBuffer(buffer))
        {
            wgpuRenderPassEncoderSetIndexBuffer(m_RenderPass, entry->buffer, WebGpu::ToWgpuIndexFormat(indexType), 0,
                                                entry->size);
        }
    }
}

void WebGpuCommandList::ApplyRenderState()
{
    if (m_RenderPass == nullptr)
    {
        return;
    }

    WebGpuPipeline* pipeline = m_Device.GetPipeline(m_CurrentPipeline);
    if (pipeline != nullptr && pipeline->renderPipeline != nullptr && m_AppliedRenderPipeline != m_CurrentPipeline)
    {
        // A read-only-depth pass forces depth writes off for every pipeline it
        // draws with; the twin carries that. Its absence means the pipeline
        // already writes no depth, so the original is what the pass wants.
        WGPURenderPipeline bound =
            (m_PassDepthReadOnly && pipeline->renderPipelineDepthReadOnly != nullptr)
                ? pipeline->renderPipelineDepthReadOnly
                : pipeline->renderPipeline;
        wgpuRenderPassEncoderSetPipeline(m_RenderPass, bound);
        m_AppliedRenderPipeline = m_CurrentPipeline;
    }

    if (pipeline != nullptr)
    {
        for (uint32_t slot = 0; slot < pipeline->vertexBindings.size(); ++slot)
        {
            const uint32_t binding = pipeline->vertexBindings[slot];
            if (binding >= kMaxVertexBufferSlots)
            {
                continue;
            }
            if (WebGpuBuffer* entry = m_Device.GetBuffer(m_VertexBuffers[binding]))
            {
                wgpuRenderPassEncoderSetVertexBuffer(m_RenderPass, slot, entry->buffer, 0, entry->size);
            }
        }
    }
    if (WebGpuBuffer* entry = m_Device.GetBuffer(m_IndexBuffer))
    {
        wgpuRenderPassEncoderSetIndexBuffer(m_RenderPass, entry->buffer, WebGpu::ToWgpuIndexFormat(m_IndexType), 0,
                                            entry->size);
    }
    for (uint32_t set = 0; set < kMaxBindGroups; ++set)
    {
        if (m_BindGroups[set] != nullptr)
        {
            wgpuRenderPassEncoderSetBindGroup(m_RenderPass, set, m_BindGroups[set], 0, nullptr);
        }
    }
    if (m_HasViewport)
    {
        wgpuRenderPassEncoderSetViewport(m_RenderPass, m_ViewportX, m_ViewportY, m_ViewportWidth, m_ViewportHeight,
                                         0.0f, 1.0f);
    }
#if !defined(__EMSCRIPTEN__)
    // Immediates are a wgpu-native extension; SupportsImmediates() is always
    // false on browser WebGPU.
    if (m_PushConstantDirtyBytes > 0 && m_Device.SupportsImmediates())
    {
        wgpuRenderPassEncoderSetImmediates(m_RenderPass, 0, m_PushConstantData.data(), m_PushConstantDirtyBytes);
    }
#endif
}

bool WebGpuCommandList::PrepareDraw()
{
    const WebGpuPipeline* pipeline = m_Device.GetPipeline(m_CurrentPipeline);
    if (pipeline == nullptr || pipeline->renderPipeline == nullptr)
    {
        if (FirstRefusalInPass(m_EncoderScopeLabel))
            Logger::Log::Warning("WebGPU: refused a draw in '{}': its pipeline (handle {}) has no render pipeline, "
                                 "because the pipeline's creation failed (see the earlier error) or its handle was "
                                 "destroyed. The caller must not draw with it. Further refusals in this pass are not "
                                 "logged.",
                                 m_EncoderScopeLabel, static_cast<uint64_t>(m_CurrentPipeline));
        return false;
    }
    ApplyRenderState();
    return ApplyPushConstantEmulation();
}

bool WebGpuCommandList::ApplyPushConstantEmulation()
{
    if (m_Device.SupportsImmediates())
    {
        return true;
    }
    WebGpuPipeline* pipeline = m_Device.GetPipeline(m_CurrentPipeline);
    if (pipeline == nullptr || pipeline->pushConstantSize == 0)
    {
        return true;
    }

    // The pipeline layout padded groups [descriptorSetCount, emulation group)
    // with the shared empty layout; a draw needs a group bound at each index.
    WGPUBindGroup emptyGroup = m_Device.GetOrCreateEmptyBindGroup();
    for (uint32_t set = pipeline->descriptorSetCount; set < WebGpuDevice::kPushConstantEmulationGroup; ++set)
    {
        if (m_BindGroups[set] != nullptr)
        {
            continue;
        }
        if (m_RenderPass != nullptr)
        {
            wgpuRenderPassEncoderSetBindGroup(m_RenderPass, set, emptyGroup, 0, nullptr);
        }
        else if (m_ComputePass != nullptr)
        {
            wgpuComputePassEncoderSetBindGroup(m_ComputePass, set, emptyGroup, 0, nullptr);
        }
    }

    // Bind the per-frame push-constant ring at a dynamic offset. A new UBO per
    // draw would stay alive until queue-done, which on the browser path can
    // fail to fire and grow wasm linear memory without bound.
    return m_Device.BindEmulatedPushConstants(m_RenderPass, m_ComputePass, m_PushConstantData.data(),
                                              pipeline->pushConstantSize, m_EncoderScopeLabel, pipeline->debugName);
}

void WebGpuCommandList::SetConstants(uint32_t slot, size_t size, const void* data)
{
    SetConstants(slot, 0, size, data);
}

void WebGpuCommandList::SetConstants(uint32_t /*slot*/, uint32_t offset, size_t size, const void* data)
{
    if (data == nullptr || size == 0 || offset + size > kMaxPushConstantBytes)
    {
        return;
    }
    std::memcpy(m_PushConstantData.data() + offset, data, size);
    m_PushConstantDirtyBytes = std::max(m_PushConstantDirtyBytes, static_cast<uint32_t>(offset + size));

    if (!m_Device.SupportsImmediates())
    {
        // No immediates: the shadow copy above is flushed as a UBO at the
        // reserved emulation group by ApplyPushConstantEmulation on the next
        // draw/dispatch.
        return;
    }
#if !defined(__EMSCRIPTEN__)
    if (m_RenderPass != nullptr)
    {
        wgpuRenderPassEncoderSetImmediates(m_RenderPass, offset, data, size);
    }
    else if (m_ComputePass != nullptr)
    {
        wgpuComputePassEncoderSetImmediates(m_ComputePass, offset, data, size);
    }
#endif
}

bool WebGpuCommandList::SetPushConstantsByName(const char* rangeName, const void* data, size_t size, uint32_t offset)
{
    uint32_t rangeId = 0;
    if (!m_Device.FindPipelinePushConstantRangeId(m_CurrentPipeline, rangeName, rangeId))
    {
        return false;
    }
    return SetPushConstantsById(rangeId, data, size, offset);
}

bool WebGpuCommandList::SetPushConstantsById(uint32_t rangeId, const void* data, size_t size, uint32_t offset)
{
    PushConstantRangeInfo info{};
    if (!m_Device.GetPipelinePushConstantRangeInfo(m_CurrentPipeline, rangeId, info))
    {
        return false;
    }
    if (offset + size > info.size)
    {
        return false;
    }
    SetConstants(0, info.offset + offset, size, data);
    return true;
}

void WebGpuCommandList::BindDescriptorSet(uint32_t set, DescriptorSetHandle descriptorSet, PipelineHandle /*pipeline*/)
{
    if (set >= kMaxBindGroups)
    {
        WebGpuLogUnsupportedOnce("bind group index beyond WebGPU's four-group limit");
        return;
    }
    WGPUBindGroup bindGroup = m_Device.ResolveBindGroup(descriptorSet);
    if (bindGroup == nullptr)
    {
        return;
    }
    m_BindGroups[set] = bindGroup;

    if (m_RenderPass != nullptr)
    {
        wgpuRenderPassEncoderSetBindGroup(m_RenderPass, set, bindGroup, 0, nullptr);
    }
    else if (m_ComputePass != nullptr)
    {
        wgpuComputePassEncoderSetBindGroup(m_ComputePass, set, bindGroup, 0, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Draws and dispatches
// ---------------------------------------------------------------------------

void WebGpuCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount)
{
    Draw(vertexCount, instanceCount, 0, 0);
}

void WebGpuCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    if (m_RenderPass == nullptr)
    {
        return;
    }
    if (!PrepareDraw())
    {
        return;
    }
    wgpuRenderPassEncoderDraw(m_RenderPass, vertexCount, instanceCount, firstVertex, firstInstance);
}

void WebGpuCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount)
{
    DrawIndexed(indexCount, instanceCount, 0, 0, 0);
}

void WebGpuCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                    int32_t vertexOffset, uint32_t firstInstance)
{
    if (m_RenderPass == nullptr)
    {
        return;
    }
    if (!PrepareDraw())
    {
        return;
    }
    wgpuRenderPassEncoderDrawIndexed(m_RenderPass, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void WebGpuCommandList::DrawMeshTasks(uint32_t /*taskCount*/)
{
    WebGpuLogUnsupportedOnce("DrawMeshTasks");
}

void WebGpuCommandList::DrawIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride)
{
    if (m_RenderPass == nullptr)
    {
        return;
    }
    WebGpuBuffer* buffer = m_Device.GetBuffer(commandBuffer);
    if (buffer == nullptr || buffer->buffer == nullptr)
    {
        return;
    }
    if (!PrepareDraw())
    {
        return;
    }
    // Multi-draw is a wgpu native extension; iterating the records is the
    // portable form and is what the browser will run anyway.
    for (uint32_t i = 0; i < drawCount; ++i)
    {
        wgpuRenderPassEncoderDrawIndirect(m_RenderPass, buffer->buffer, static_cast<uint64_t>(i) * stride);
    }
}

void WebGpuCommandList::DrawIndexedIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride)
{
    if (m_RenderPass == nullptr)
    {
        return;
    }
    WebGpuBuffer* buffer = m_Device.GetBuffer(commandBuffer);
    if (buffer == nullptr || buffer->buffer == nullptr)
    {
        return;
    }
    if (!PrepareDraw())
    {
        return;
    }
    for (uint32_t i = 0; i < drawCount; ++i)
    {
        wgpuRenderPassEncoderDrawIndexedIndirect(m_RenderPass, buffer->buffer, static_cast<uint64_t>(i) * stride);
    }
}

void WebGpuCommandList::DrawIndexedIndirectCount(BufferHandle commandBuffer, BufferHandle /*countBuffer*/,
                                                 uint32_t maxDrawCount, uint32_t stride,
                                                 size_t commandBufferOffset, size_t /*countBufferOffset*/)
{
    // Core WebGPU has no GPU-side draw count, so this is the emulated arm the
    // capability struct advertises (supportsDrawIndirectCountNative == false):
    // ignore the count buffer and walk ALL maxDrawCount records. Producers
    // uphold the documented contract by zero-filling unused trailing records
    // (GPUDrawStreamBuilder::ScheduleUnified; same contract as the
    // Vulkan/MoltenVK fallback in VulkanCommandList) — a degenerate
    // indexCount=0 record is a no-op draw, not a ghost.
    if (m_RenderPass == nullptr)
    {
        return;
    }
    // The walk costs one encoder call per record, count or no count. Small
    // producers (CBT terrain and terrain grass submit exactly 1 record with an
    // exact GPU-maintained indexCount) are free; the GPU-driven mesh batches
    // submit their snapshot CAPACITY as maxDrawCount — hundreds of thousands
    // of records — and walking those took a frame from 60 fps to 0.5. Until
    // the mesh stream grows a web shape (GPU-compacted single draw or a
    // CPU-known count), those batches stay dropped here, exactly as the old
    // stub dropped them — meshes on web render through the classic path.
    constexpr uint32_t kEmulatedWalkLimit = 64u;
    if (maxDrawCount > kEmulatedWalkLimit)
    {
        static bool s_LoggedOversized = false;
        if (!s_LoggedOversized)
        {
            s_LoggedOversized = true;
            Logger::Log::Warning(
                "WebGpuCommandList: DrawIndexedIndirectCount with maxDrawCount={} exceeds the "
                "emulated walk limit ({}); dropping this batch. The GPU-driven mesh stream "
                "needs a web-shaped submit (GPU-compacted draw or CPU-known count).",
                maxDrawCount, kEmulatedWalkLimit);
        }
        return;
    }
    WebGpuBuffer* buffer = m_Device.GetBuffer(commandBuffer);
    if (buffer == nullptr || buffer->buffer == nullptr)
    {
        return;
    }
    if (!PrepareDraw())
    {
        return;
    }
    for (uint32_t i = 0; i < maxDrawCount; ++i)
    {
        wgpuRenderPassEncoderDrawIndexedIndirect(
            m_RenderPass, buffer->buffer,
            static_cast<uint64_t>(commandBufferOffset) + static_cast<uint64_t>(i) * stride);
    }
}

void WebGpuCommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z)
{
    WGPUComputePassEncoder pass = EnsureComputePass();
    if (pass == nullptr)
    {
        return;
    }
    if (!ApplyPushConstantEmulation())
    {
        return;
    }
    wgpuComputePassEncoderDispatchWorkgroups(pass, x, y, z);
}

void WebGpuCommandList::DispatchIndirect(BufferHandle argsBuffer, size_t argsOffsetBytes)
{
    WebGpuBuffer* buffer = m_Device.GetBuffer(argsBuffer);
    if (buffer == nullptr || buffer->buffer == nullptr)
    {
        return;
    }
    WGPUComputePassEncoder pass = EnsureComputePass();
    if (pass == nullptr)
    {
        return;
    }
    if (!ApplyPushConstantEmulation())
    {
        return;
    }
    wgpuComputePassEncoderDispatchWorkgroupsIndirect(pass, buffer->buffer, argsOffsetBytes);
}

// ---------------------------------------------------------------------------
// Copies
// ---------------------------------------------------------------------------

void WebGpuCommandList::CopyBuffer(BufferHandle src, BufferHandle dst, size_t size, size_t srcOffset, size_t dstOffset)
{
    WebGpuBuffer* source = m_Device.GetBuffer(src);
    WebGpuBuffer* destination = m_Device.GetBuffer(dst);
    if (source == nullptr || destination == nullptr)
    {
        return;
    }
    ClosePasses();
    EnsureEncoder();
    wgpuCommandEncoderCopyBufferToBuffer(m_Encoder, source->buffer, srcOffset, destination->buffer, dstOffset, size);
}

void WebGpuCommandList::CopyTexture(TextureHandle src, TextureHandle dst)
{
    WebGpuTexture* source = m_Device.GetTexture(src);
    WebGpuTexture* destination = m_Device.GetTexture(dst);
    if (source == nullptr || destination == nullptr || source->texture == nullptr || destination->texture == nullptr)
    {
        return;
    }
    if ((source->usage & static_cast<uint32_t>(TextureUsage::TransferSrc)) == 0 ||
        (destination->usage & static_cast<uint32_t>(TextureUsage::TransferDst)) == 0)
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            sLogged = true;
            Logger::Log::Error("WebGpuCommandList::CopyTexture: '{}' -> '{}' missing CopySrc/CopyDst "
                               "(src usage 0x{:x}, dst usage 0x{:x}); copy skipped",
                               source->debugName, destination->debugName, source->usage,
                               destination->usage);
        }
        return;
    }
    ClosePasses();
    EnsureEncoder();

    WGPUTexelCopyTextureInfo sourceInfo{};
    sourceInfo.texture = source->texture;
    sourceInfo.aspect = WGPUTextureAspect_All;

    WGPUTexelCopyTextureInfo destinationInfo{};
    destinationInfo.texture = destination->texture;
    destinationInfo.aspect = WGPUTextureAspect_All;

    WGPUExtent3D extent{};
    extent.width = std::min(source->width, destination->width);
    extent.height = std::min(source->height, destination->height);
    extent.depthOrArrayLayers = 1;

    wgpuCommandEncoderCopyTextureToTexture(m_Encoder, &sourceInfo, &destinationInfo, &extent);
}

void WebGpuCommandList::FillBuffer(BufferHandle dst, size_t offset, size_t size, uint32_t value)
{
    WebGpuBuffer* buffer = m_Device.GetBuffer(dst);
    if (buffer == nullptr || buffer->buffer == nullptr)
    {
        return;
    }
    if (value != 0)
    {
        // WebGPU's only encoder-side fill is a zero clear, so a non-zero fill
        // goes through the queue as a written pattern. That is not a free
        // substitution: a queue write executes before EVERY command in the
        // buffers submitted after it, not at this point in the stream. It is
        // correct for the one shape the engine actually fills with — seed a
        // buffer, then consume it later in the same frame (SDSM sentinels,
        // culling's previous-visibility reset, indirect-arg and cursor
        // priming) — and wrong for a fill meant to overwrite something an
        // earlier pass in this same command buffer produced. Doing nothing,
        // which is what the unsupported path did, is wrong for both.
        const size_t dwordCount = size / sizeof(uint32_t);
        if (dwordCount == 0)
        {
            return;
        }
        m_FillPattern.assign(dwordCount, value);
        m_Device.UpdateBuffer(dst, offset, dwordCount * sizeof(uint32_t), m_FillPattern.data());
        return;
    }
    ClosePasses();
    EnsureEncoder();
    wgpuCommandEncoderClearBuffer(m_Encoder, buffer->buffer, offset, size);
}

void WebGpuCommandList::CopyTextureToBuffer(TextureHandle srcTexture, BufferHandle dstBuffer, uint32_t width,
                                            uint32_t height, uint32_t SrcX, uint32_t SrcY, size_t DstOffsetBytes,
                                            size_t dstRowPitchBytes)
{
    CopyTextureSubresourceToBuffer(srcTexture, 0, 0, dstBuffer, width, height, SrcX, SrcY, DstOffsetBytes,
                                   dstRowPitchBytes, 1, 0);
}

void WebGpuCommandList::CopyTextureSubresourceToBuffer(TextureHandle srcTexture, uint32_t mip, uint32_t layer,
                                                       BufferHandle dstBuffer, uint32_t width, uint32_t height,
                                                       uint32_t SrcX, uint32_t SrcY, size_t DstOffsetBytes,
                                                       size_t dstRowPitchBytes, uint32_t depth,
                                                       size_t dstSlicePitchBytes)
{
    WebGpuTexture* texture = m_Device.GetTexture(srcTexture);
    WebGpuBuffer* buffer = m_Device.GetBuffer(dstBuffer);
    if (texture == nullptr || buffer == nullptr || texture->texture == nullptr || buffer->buffer == nullptr)
    {
        return;
    }
    ClosePasses();
    EnsureEncoder();

    const TextureFormat engineFormat = WebGpu::FromWgpuTextureFormat(texture->format);
    const uint32_t bytesPerPixel = BytesPerPixel(engineFormat);
    const uint32_t rowPitch = dstRowPitchBytes != 0 ? static_cast<uint32_t>(dstRowPitchBytes) : width * bytesPerPixel;

    // WebGPU rejects (wgpu-native: aborts on) texture->buffer copies whose
    // bytesPerRow is not 256-aligned. Unlike an upload this cannot be repacked
    // behind the caller's back: the destination is the buffer the caller is
    // about to map, so its layout is part of the contract. Refuse instead —
    // recording a copy the backend has already rejected invalidates the whole
    // command buffer, losing every other command in it too.
    const bool readbackSingleRow = height <= 1 && depth <= 1;
    if (!readbackSingleRow && rowPitch % kWebGpuCopyBytesPerRowAlignment != 0)
    {
        Logger::Log::Error(
            "WebGpuCommandList::CopyTextureSubresourceToBuffer: bytesPerRow {} is not "
            "256-aligned (WebGPU COPY_BYTES_PER_ROW_ALIGNMENT); copy skipped — pass an "
            "aligned dstRowPitchBytes",
            rowPitch);
        return;
    }

    WGPUTexelCopyTextureInfo source{};
    source.texture = texture->texture;
    source.mipLevel = mip;
    source.origin = WGPUOrigin3D{SrcX, SrcY, layer};
    source.aspect = WGPUTextureAspect_All;

    WGPUTexelCopyBufferInfo destination{};
    destination.buffer = buffer->buffer;
    destination.layout.offset = DstOffsetBytes;
    destination.layout.bytesPerRow = readbackSingleRow ? WGPU_COPY_STRIDE_UNDEFINED : rowPitch;
    destination.layout.rowsPerImage =
        dstSlicePitchBytes != 0 && rowPitch != 0 ? static_cast<uint32_t>(dstSlicePitchBytes / rowPitch) : height;

    WGPUExtent3D extent{width, height, depth};
    wgpuCommandEncoderCopyTextureToBuffer(m_Encoder, &source, &destination, &extent);
}

void WebGpuCommandList::CopyBufferToTextureSubresource(BufferHandle srcBuffer, TextureHandle dstTexture, uint32_t mip,
                                                       uint32_t layer, uint32_t width, uint32_t height,
                                                       size_t srcOffsetBytes, size_t srcRowPitchBytes, uint32_t depth,
                                                       size_t srcSlicePitchBytes, uint32_t dstX, uint32_t dstY,
                                                       ResourceState /*currentState*/)
{
    WebGpuBuffer* buffer = m_Device.GetBuffer(srcBuffer);
    WebGpuTexture* texture = m_Device.GetTexture(dstTexture);
    if (buffer == nullptr || texture == nullptr || buffer->buffer == nullptr || texture->texture == nullptr)
    {
        return;
    }
    ClosePasses();
    EnsureEncoder();

    const TextureFormat engineFormat = WebGpu::FromWgpuTextureFormat(texture->format);
    const uint32_t bytesPerPixel = BytesPerPixel(engineFormat);
    const uint32_t sliceCount = depth != 0 ? depth : 1u;
    const uint64_t tightRowBytes = static_cast<uint64_t>(width) * bytesPerPixel;
    const uint64_t rowPitch = srcRowPitchBytes != 0 ? srcRowPitchBytes : tightRowBytes;
    const uint64_t slicePitch = srcSlicePitchBytes != 0 ? srcSlicePitchBytes : rowPitch * height;

    WGPUBuffer sourceBuffer = buffer->buffer;
    uint64_t sourceOffset = srcOffsetBytes;
    uint64_t bytesPerRow = rowPitch;
    uint32_t rowsPerImage = slicePitch != 0 && rowPitch != 0 ? static_cast<uint32_t>(slicePitch / rowPitch) : height;

    // bytesPerRow is only meaningful — and only required to be 256-aligned —
    // when the copy spans rows, so a tight 1xN upload (dummy/white textures)
    // passes it undefined.
    const bool singleRow = height <= 1 && sliceCount <= 1;
    if (!singleRow && rowPitch % kWebGpuCopyBytesPerRowAlignment != 0)
    {
        // A tightly packed row is the natural pitch for most image sources and
        // is legal on every other backend, so the alignment is met here rather
        // than pushed onto callers: rows are restrided into scratch on the GPU
        // and the texture copy reads the padded pitch.
        const uint64_t paddedRowPitch = AlignUp(rowPitch, kWebGpuCopyBytesPerRowAlignment);
        const uint64_t rowBytes = tightRowBytes != 0 ? std::min(tightRowBytes, rowPitch) : rowPitch;
        WGPUBuffer scratch = RepackRowsToAlignedPitch(buffer->buffer, buffer->size, srcOffsetBytes, rowPitch,
                                                      slicePitch, rowBytes, height, sliceCount, paddedRowPitch);
        if (scratch == nullptr)
        {
            Logger::Log::Error(
                "WebGpuCommandList::CopyBufferToTextureSubresource: rowPitch {} on '{}' ({}x{} mip {} "
                "layer {}) is neither 256-aligned nor 4-byte addressable, so it cannot be repacked; "
                "copy skipped — pad the staging pitch to caps.textureCopyRowPitchAlignment",
                rowPitch, texture->debugName, width, height, mip, layer);
            return;
        }
        sourceBuffer = scratch;
        sourceOffset = 0;
        bytesPerRow = paddedRowPitch;
        rowsPerImage = height;
    }

    WGPUTexelCopyBufferInfo source{};
    source.buffer = sourceBuffer;
    source.layout.offset = sourceOffset;
    source.layout.bytesPerRow = singleRow ? WGPU_COPY_STRIDE_UNDEFINED : static_cast<uint32_t>(bytesPerRow);
    source.layout.rowsPerImage = rowsPerImage;

    WGPUTexelCopyTextureInfo destination{};
    destination.texture = texture->texture;
    destination.mipLevel = mip;
    destination.origin = WGPUOrigin3D{dstX, dstY, layer};
    destination.aspect = WGPUTextureAspect_All;

    WGPUExtent3D extent{width, height, sliceCount};
    wgpuCommandEncoderCopyBufferToTexture(m_Encoder, &source, &destination, &extent);
}

WGPUBuffer WebGpuCommandList::RepackRowsToAlignedPitch(WGPUBuffer source, uint64_t sourceSize, uint64_t sourceOffset,
                                                       uint64_t sourceRowPitch, uint64_t sourceSlicePitch,
                                                       uint64_t rowBytes, uint32_t rowsPerSlice, uint32_t sliceCount,
                                                       uint64_t paddedRowPitch)
{
    if (rowBytes == 0 || rowsPerSlice == 0 || sliceCount == 0)
    {
        return nullptr;
    }

    // Buffer-to-buffer copies address in 4-byte units, so each row's source
    // offset and its length must land on that grid. Rounding the length up
    // spills the next row's leading bytes into this row's padding, which the
    // texture copy never reads — but the final row must still stay inside the
    // source allocation.
    const uint64_t copyBytes = AlignUp(rowBytes, kWebGpuCopyBufferAlignment);
    if (sourceOffset % kWebGpuCopyBufferAlignment != 0 || sourceRowPitch % kWebGpuCopyBufferAlignment != 0 ||
        sourceSlicePitch % kWebGpuCopyBufferAlignment != 0)
    {
        return nullptr;
    }
    const uint64_t lastRowOffset = sourceOffset + static_cast<uint64_t>(sliceCount - 1u) * sourceSlicePitch +
                                   static_cast<uint64_t>(rowsPerSlice - 1u) * sourceRowPitch;
    if (lastRowOffset + copyBytes > sourceSize)
    {
        return nullptr;
    }

    WGPUBufferDescriptor desc{};
    desc.label = WebGpu::MakeStringView("CopyRowRepackScratch");
    desc.size = paddedRowPitch * rowsPerSlice * sliceCount;
    desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    WGPUBuffer scratch = wgpuDeviceCreateBuffer(m_Device.GetWgpuDevice(), &desc);
    if (scratch == nullptr)
    {
        return nullptr;
    }
    m_TransientBuffers.push_back(scratch);

    for (uint32_t z = 0; z < sliceCount; ++z)
    {
        for (uint32_t y = 0; y < rowsPerSlice; ++y)
        {
            const uint64_t from = sourceOffset + static_cast<uint64_t>(z) * sourceSlicePitch +
                                  static_cast<uint64_t>(y) * sourceRowPitch;
            const uint64_t to = (static_cast<uint64_t>(z) * rowsPerSlice + y) * paddedRowPitch;
            wgpuCommandEncoderCopyBufferToBuffer(m_Encoder, source, from, scratch, to, copyBytes);
        }
    }
    return scratch;
}

void WebGpuCommandList::ClearColorImageSubresource(TextureHandle /*texture*/, uint32_t /*mip*/, uint32_t /*layer*/,
                                                   const float /*rgba*/[4])
{
    // Core WebGPU clears only through a render pass load op.
    WebGpuLogUnsupportedOnce("ClearColorImageSubresource");
}

void WebGpuCommandList::BlitImageMip(TextureHandle /*texture*/, uint32_t /*srcMip*/, uint32_t /*dstMip*/,
                                     uint32_t /*layer*/)
{
    // Scaled blits need a render-pass downsample; the mip chain generator lands
    // with the compatibility profile.
    WebGpuLogUnsupportedOnce("BlitImageMip");
}

// ---------------------------------------------------------------------------
// Render passes
// ---------------------------------------------------------------------------

WGPUTextureView WebGpuCommandList::ResolveAttachmentView(TextureHandle texture, bool useCustomView,
                                                         const TextureViewDesc& viewDesc)
{
    WebGpuTexture* entry = m_Device.GetTexture(texture);
    if (entry == nullptr || entry->texture == nullptr)
    {
        return nullptr;
    }
    if (!useCustomView)
    {
        return entry->defaultView;
    }

    WGPUTextureViewDescriptor desc{};
    desc.label = WebGpu::MakeStringView("AttachmentView");
    desc.format = viewDesc.formatOverride != 0
                      ? WebGpu::ToWgpuTextureFormat(static_cast<TextureFormat>(viewDesc.formatOverride))
                      : entry->format;
    desc.dimension = WebGpu::ToWgpuTextureViewDimension(viewDesc.viewType);
    desc.baseMipLevel = viewDesc.baseMip;
    desc.mipLevelCount = viewDesc.levelCount != 0 ? viewDesc.levelCount : 1;
    desc.baseArrayLayer = viewDesc.baseLayer;
    desc.arrayLayerCount = viewDesc.layerCount != 0 ? viewDesc.layerCount : 1;
    desc.aspect = WebGpu::ToWgpuTextureAspect(viewDesc.aspect);

    WGPUTextureView view = wgpuTextureCreateView(entry->texture, &desc);
    if (view != nullptr)
    {
        m_TransientViews.push_back(view);
    }
    return view;
}

void WebGpuCommandList::BeginRenderPass(const RenderPassDesc& desc)
{
    ClosePasses();
    EnsureEncoder();

    std::vector<WGPURenderPassColorAttachment> colorAttachments;
    colorAttachments.reserve(desc.colorTargetCount);
    for (uint32_t i = 0; i < desc.colorTargetCount && i < 8; ++i)
    {
        WGPUTextureView view = ResolveAttachmentView(desc.colorTargets[i], desc.useColorView[i], desc.colorViewDesc[i]);
        if (view == nullptr)
        {
            continue;
        }

        WGPURenderPassColorAttachment attachment{};
        attachment.view = view;
        attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        attachment.loadOp = desc.clearColor[i] ? WGPULoadOp_Clear : WebGpu::ToWgpuLoadOp(desc.colorLoadOp[i]);
        attachment.storeOp = WebGpu::ToWgpuStoreOp(desc.colorStoreOp[i]);
        attachment.clearValue = WGPUColor{desc.clearColorValue[i][0], desc.clearColorValue[i][1],
                                          desc.clearColorValue[i][2], desc.clearColorValue[i][3]};
        if (desc.resolveColorTargets[i].IsValid())
        {
            attachment.resolveTarget = ResolveAttachmentView(desc.resolveColorTargets[i], desc.useColorResolveView[i],
                                                             desc.colorResolveViewDesc[i]);
        }
        colorAttachments.push_back(attachment);

        if (WebGpuTexture* entry = m_Device.GetTexture(desc.colorTargets[i]))
        {
            m_RenderTargetWidth = entry->width;
            m_RenderTargetHeight = entry->height;
        }
    }

    WGPURenderPassDepthStencilAttachment depthAttachment{};
    bool hasDepth = false;
    if (desc.depthTarget.IsValid())
    {
        WGPUTextureView view = ResolveAttachmentView(desc.depthTarget, desc.useDepthView, desc.depthViewDesc);
        if (view != nullptr)
        {
            hasDepth = true;
            depthAttachment.view = view;
            depthAttachment.depthReadOnly = desc.depthReadOnly ? 1u : 0u;
            if (desc.depthReadOnly)
            {
                depthAttachment.depthLoadOp = WGPULoadOp_Undefined;
                depthAttachment.depthStoreOp = WGPUStoreOp_Undefined;
            }
            else
            {
                depthAttachment.depthLoadOp = desc.clearDepth ? WGPULoadOp_Clear : WebGpu::ToWgpuLoadOp(desc.depthLoadOp);
                depthAttachment.depthStoreOp = WebGpu::ToWgpuStoreOp(desc.depthStoreOp);
            }
            depthAttachment.depthClearValue = desc.clearDepthValue;

            const WebGpuTexture* depthTexture = m_Device.GetTexture(desc.depthTarget);
            const bool hasStencilAspect =
                depthTexture != nullptr && (depthTexture->format == WGPUTextureFormat_Depth24PlusStencil8 ||
                                            depthTexture->format == WGPUTextureFormat_Depth32FloatStencil8 ||
                                            depthTexture->format == WGPUTextureFormat_Stencil8);
            if (hasStencilAspect)
            {
                depthAttachment.stencilReadOnly = desc.depthReadOnly ? 1u : 0u;
                if (!desc.depthReadOnly)
                {
                    depthAttachment.stencilLoadOp =
                        desc.clearStencil ? WGPULoadOp_Clear : WebGpu::ToWgpuLoadOp(desc.stencilLoadOp);
                    depthAttachment.stencilStoreOp = WebGpu::ToWgpuStoreOp(desc.stencilStoreOp);
                    depthAttachment.stencilClearValue = desc.clearStencilValue;
                }
            }
        }
    }

    WGPURenderPassDescriptor passDesc{};
    passDesc.label =
        WebGpu::MakeStringView(m_EncoderScopeLabel.empty() ? "RenderPass" : m_EncoderScopeLabel.c_str());
    passDesc.colorAttachmentCount = colorAttachments.size();
    passDesc.colorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
    passDesc.depthStencilAttachment = hasDepth ? &depthAttachment : nullptr;
    WGPUPassTimestampWrites timestampWrites{};
    if (BeginPassTimestampWrites(timestampWrites))
        passDesc.timestampWrites = &timestampWrites;

    m_RenderPass = wgpuCommandEncoderBeginRenderPass(m_Encoder, &passDesc);
    m_AppliedRenderPipeline = PipelineHandle{};
    m_PassDepthReadOnly = hasDepth && desc.depthReadOnly;
    // Graphics bindings are pass-scoped and are dropped at the boundary rather
    // than replayed into the new encoder. A render pipeline is only valid for
    // the attachment state it was built against, and a bind group carried in
    // adds that pass's textures to this pass's synchronization scope — either
    // one makes the encoder, and so the whole frame's submission, invalid.
    // Every pass declares the pipeline and sets it draws with.
    if (const WebGpuPipeline* previous = m_Device.GetPipeline(m_CurrentPipeline);
        previous != nullptr && previous->type == PipelineType::Graphics)
    {
        m_CurrentPipeline = PipelineHandle{};
    }
    m_BindGroups.fill(nullptr);
}

void WebGpuCommandList::EndRenderPass()
{
    if (m_RenderPass == nullptr)
    {
        return;
    }
    wgpuRenderPassEncoderEnd(m_RenderPass);
    wgpuRenderPassEncoderRelease(m_RenderPass);
    m_RenderPass = nullptr;
    m_AppliedRenderPipeline = PipelineHandle{};
    m_PassDepthReadOnly = false;
}

// The engine's viewport is a framebuffer pixel rect in one orientation: clip
// +Y lands on the rect's TOP row. Vulkan needs a negative viewport height to
// get there and its command list negates what callers pass; WebGPU's clip
// space is already Y-up against a top-left framebuffer, so the rect goes
// through untouched.
//
// The opposite orientation is not expressible here. WebGPU rejects a negative
// extent outright, and one rejected viewport invalidates the render pass, the
// encoder and the frame's submit — a dead frame, not a mirrored pass. A caller
// that still asks for the flip gets the rect it covers plus a one-time notice;
// its content draws mirrored until that pass maps to clip space itself, as
// Shaders/ui_sdf.vert does.
void WebGpuCommandList::SetViewport(float x, float y, float width, float height)
{
    if (height < 0.0f)
    {
        WebGpuLogUnsupportedOnce("Y-flipped viewport (negative height)");
        y += height;
        height = -height;
    }
    m_ViewportX = x;
    m_ViewportY = y;
    m_ViewportWidth = width;
    m_ViewportHeight = height;
    m_HasViewport = true;
    if (m_RenderPass != nullptr)
    {
        wgpuRenderPassEncoderSetViewport(m_RenderPass, x, y, width, height, 0.0f, 1.0f);
    }
}

void WebGpuCommandList::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (m_RenderPass != nullptr)
    {
        wgpuRenderPassEncoderSetScissorRect(m_RenderPass, x, y, width, height);
    }
}

void WebGpuCommandList::SetDepthBounds(float /*minDepth*/, float /*maxDepth*/)
{
    WebGpuLogUnsupportedOnce("SetDepthBounds (WebGPU has no depth-bounds test)");
}

void WebGpuCommandList::Barrier(const ResourceBarrier& /*barrier*/)
{
    // WebGPU derives hazards from usage; there is nothing to record.
}

void WebGpuCommandList::BarrierBatch(const std::vector<ResourceBarrier>& /*barriers*/)
{
}

// Begin/EndEvent pairs from the render graph do not respect pass boundaries
// (an event can open before a pass exists and close inside one, or span a
// pass close). WebGPU debug groups must balance within each encoder/pass
// scope, so pushes are counted per scope and pops only ever hit a scope that
// this list actually pushed on; Close*Pass and End drain what remains.
void WebGpuCommandList::BeginEvent(const char* name)
{
    const WGPUStringView label = WebGpu::MakeStringView(name);
    // The render graph opens a pass's event before its render pass, so the last
    // event name is the pass a subsequent BeginRenderPass belongs to. Dawn
    // quotes the encoder label in validation messages, and an unnamed
    // "RenderPass" cannot be attributed to a pass.
    m_EncoderScopeLabel = name ? name : "";
    if (m_RenderPass != nullptr)
    {
        wgpuRenderPassEncoderPushDebugGroup(m_RenderPass, label);
        ++m_RenderPassDebugDepth;
        return;
    }
    if (m_ComputePass != nullptr)
    {
        wgpuComputePassEncoderPushDebugGroup(m_ComputePass, label);
        ++m_ComputePassDebugDepth;
        return;
    }
    EnsureEncoder();
    if (m_Encoder != nullptr)
    {
        wgpuCommandEncoderPushDebugGroup(m_Encoder, label);
        ++m_EncoderDebugDepth;
    }
}

void WebGpuCommandList::EndEvent()
{
    if (m_RenderPass != nullptr && m_RenderPassDebugDepth > 0)
    {
        wgpuRenderPassEncoderPopDebugGroup(m_RenderPass);
        --m_RenderPassDebugDepth;
        return;
    }
    if (m_ComputePass != nullptr && m_ComputePassDebugDepth > 0)
    {
        wgpuComputePassEncoderPopDebugGroup(m_ComputePass);
        --m_ComputePassDebugDepth;
        return;
    }
    if (m_RenderPass == nullptr && m_ComputePass == nullptr && m_Encoder != nullptr && m_EncoderDebugDepth > 0)
    {
        wgpuCommandEncoderPopDebugGroup(m_Encoder);
        --m_EncoderDebugDepth;
    }
    // Otherwise the matching push landed on a scope that already closed (its
    // Close drained it); dropping the pop keeps every scope balanced.
}

void WebGpuCommandList::SetMarker(const char* name)
{
    const WGPUStringView label = WebGpu::MakeStringView(name);
    if (m_RenderPass != nullptr)       wgpuRenderPassEncoderInsertDebugMarker(m_RenderPass, label);
    else if (m_ComputePass != nullptr) wgpuComputePassEncoderInsertDebugMarker(m_ComputePass, label);
    else if (m_Encoder != nullptr)     wgpuCommandEncoderInsertDebugMarker(m_Encoder, label);
}

void WebGpuCommandList::ClearRenderTarget(TextureHandle target, float r, float g, float b, float a)
{
    RenderPassDesc desc{};
    desc.colorTargets[0] = target;
    desc.colorTargetCount = 1;
    desc.clearColor[0] = true;
    desc.colorLoadOp[0] = RenderPassDesc::LoadOp::Clear;
    desc.colorStoreOp[0] = RenderPassDesc::StoreOp::Store;
    desc.clearColorValue[0][0] = r;
    desc.clearColorValue[0][1] = g;
    desc.clearColorValue[0][2] = b;
    desc.clearColorValue[0][3] = a;
    desc.clearDepth = false;
    desc.depthLoadOp = RenderPassDesc::LoadOp::DontCare;

    BeginRenderPass(desc);
    EndRenderPass();
}

void WebGpuCommandList::GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight)
{
    m_Device.GetTextureSize(texture, outWidth, outHeight);
}

} // namespace GameEngine::Rendering
