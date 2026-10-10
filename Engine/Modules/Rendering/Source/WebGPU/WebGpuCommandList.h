#pragma once

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/QueryPool.h"

#include <webgpu/webgpu.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::Rendering
{

class WebGpuDevice;

// WebGPU records through a command encoder that spawns render and compute pass
// encoders; only one pass may be open at a time and copies are only legal on
// the bare encoder. This class is that state machine: the compute pass opens
// lazily on first dispatch and closes when a copy or render pass needs the
// encoder back.
//
// Binding state (pipeline, vertex/index buffers, viewport, bind groups) is
// tracked command-list-wide and re-applied when a pass opens, because WebGPU
// pass encoders start stateless while the engine's callers assume Vulkan's
// pass-spanning binds.
class WebGpuCommandList : public CommandList
{
  public:
    WebGpuCommandList(WebGpuDevice& device, IDevice::QueueType queue);
    ~WebGpuCommandList() override;

    void Begin() override;
    void End() override;

    void SetPipeline(PipelineHandle pipeline) override;
    void SetVertexBuffer(BufferHandle buffer, uint32_t slot = 0) override;
    void SetIndexBuffer(BufferHandle buffer, IndexType indexType = IndexType::Uint16) override;
    void SetConstants(uint32_t slot, size_t size, const void* data) override;
    void SetConstants(uint32_t slot, uint32_t offset, size_t size, const void* data) override;
    bool SetPushConstantsByName(const char* rangeName, const void* data, size_t size, uint32_t offset = 0) override;
    bool SetPushConstantsById(uint32_t rangeId, const void* data, size_t size, uint32_t offset = 0) override;

    void BindDescriptorSet(uint32_t set, DescriptorSetHandle descriptorSet, PipelineHandle pipeline) override;

    void Draw(uint32_t vertexCount, uint32_t instanceCount = 1) override;
    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1) override;
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                     uint32_t firstInstance) override;
    void DrawMeshTasks(uint32_t taskCount) override;
    void DrawIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) override;
    void DrawIndexedIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) override;
    void DrawIndexedIndirectCount(BufferHandle commandBuffer, BufferHandle countBuffer, uint32_t maxDrawCount,
                                  uint32_t stride, size_t commandBufferOffset = 0,
                                  size_t countBufferOffset = 0) override;

    void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) override;
    void DispatchIndirect(BufferHandle argsBuffer, size_t argsOffsetBytes = 0) override;

    void CopyBuffer(BufferHandle src, BufferHandle dst, size_t size, size_t srcOffset = 0,
                    size_t dstOffset = 0) override;
    void CopyTexture(TextureHandle src, TextureHandle dst) override;
    void FillBuffer(BufferHandle dst, size_t offset, size_t size, uint32_t value) override;
    void CopyTextureToBuffer(TextureHandle srcTexture, BufferHandle dstBuffer, uint32_t width, uint32_t height,
                             uint32_t SrcX = 0, uint32_t SrcY = 0, size_t DstOffsetBytes = 0,
                             size_t dstRowPitchBytes = 0) override;
    void CopyTextureSubresourceToBuffer(TextureHandle srcTexture, uint32_t mip, uint32_t layer, BufferHandle dstBuffer,
                                        uint32_t width, uint32_t height, uint32_t SrcX = 0, uint32_t SrcY = 0,
                                        size_t DstOffsetBytes = 0, size_t dstRowPitchBytes = 0, uint32_t depth = 1,
                                        size_t dstSlicePitchBytes = 0) override;
    void ClearColorImageSubresource(TextureHandle texture, uint32_t mip, uint32_t layer, const float rgba[4]) override;
    void CopyBufferToTextureSubresource(BufferHandle srcBuffer, TextureHandle dstTexture, uint32_t mip, uint32_t layer,
                                        uint32_t width, uint32_t height, size_t srcOffsetBytes = 0,
                                        size_t srcRowPitchBytes = 0, uint32_t depth = 1, size_t srcSlicePitchBytes = 0,
                                        uint32_t dstX = 0, uint32_t dstY = 0,
                                        ResourceState currentState = ResourceState::Undefined) override;
    void BlitImageMip(TextureHandle texture, uint32_t srcMip, uint32_t dstMip, uint32_t layer = 0) override;

    void BeginRenderPass(const RenderPassDesc& desc) override;
    void EndRenderPass() override;

    void SetViewport(float x, float y, float width, float height) override;
    void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
    void SetDepthBounds(float minDepth, float maxDepth) override;

    // WebGPU tracks resource hazards itself; engine barriers carry no work.
    void Barrier(const ResourceBarrier& barrier) override;
    void BarrierBatch(const std::vector<ResourceBarrier>& barriers) override;

    void BeginEvent(const char* name) override;
    void EndEvent() override;
    void SetMarker(const char* name) override;

    void ClearRenderTarget(TextureHandle target, float r, float g, float b, float a) override;
    void GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) override;

    // Hands the finished command buffer (+1) to the caller; the device owns the
    // submit. Null when nothing was recorded.
    WGPUCommandBuffer Detach();

    // The bare encoder, for a timestamp write — null while a render or compute
    // pass is open, because WebGPU only allows the write outside one. Opens the
    // encoder if recording has not yet touched it.
    WGPUCommandEncoder GetOpenEncoderForTimestamp();
    // Browser timestamps: the pool allocated `localQuery` for a scope boundary
    // that the encoder cannot write. The next pass begin writes a pending
    // SpanBegin; a SpanEnd aliases onto the end query of the last pass since.
    void NoteTimestamp(TimestampPoint point, uint32_t localQuery);
    IDevice::QueueType GetQueueType() const { return m_Queue; }

  private:
    static constexpr uint32_t kMaxVertexBufferSlots = 6;
    static constexpr uint32_t kMaxPushConstantBytes = 128;

    void EnsureEncoder();
    void ClosePasses();
    // Fills the pass descriptor's timestampWrites from the pending scope state;
    // returns false when nothing is to be written (no pool, or slot full).
    bool BeginPassTimestampWrites(WGPUPassTimestampWrites& outWrites);
    void CloseComputePass();
    WGPUComputePassEncoder EnsureComputePass();
    // Binds the pipeline, vertex/index buffers, bind groups and viewport the
    // next draw needs. Push constants are a draw's own: see PrepareDraw.
    void ApplyRenderState();
    // ApplyRenderState plus the push-constant block. False, and the draw
    // refused, when the bound pipeline has no render pipeline (the encoder
    // would run the previous draw's pipeline with this draw's bindings) or
    // when the block could not be bound (an unbound group invalidates the
    // frame's command buffer).
    bool PrepareDraw();
    // Browser path: write the push-constant shadow into this frame's ring
    // slice and bind it at WebGpuDevice::kPushConstantEmulationGroup (plus
    // empty groups for the layout's padded indices). True when bound or not
    // needed (immediates, or a pipeline without a block).
    bool ApplyPushConstantEmulation();
    void ReleaseTransientViews();
    void ReleaseTransientBuffers();
    // Copies `rowCount` rows of `rowBytes` from `source` into a scratch buffer
    // whose rows sit at `paddedRowPitch`, so the texture copy can state a
    // 256-aligned bytesPerRow. The row copies are encoded on this command
    // list's encoder, so the repack keeps the ordering of a direct copy.
    // Returns nullptr when the source geometry is not 4-byte addressable.
    WGPUBuffer RepackRowsToAlignedPitch(WGPUBuffer source, uint64_t sourceSize, uint64_t sourceOffset,
                                        uint64_t sourceRowPitch, uint64_t sourceSlicePitch, uint64_t rowBytes,
                                        uint32_t rowsPerSlice, uint32_t sliceCount, uint64_t paddedRowPitch);
    // Resolves the attachment view for a render-pass slot, creating a transient
    // view when the pass selects a specific mip/layer/aspect.
    WGPUTextureView ResolveAttachmentView(TextureHandle texture, bool useCustomView, const TextureViewDesc& viewDesc);

    WebGpuDevice& m_Device;
    IDevice::QueueType m_Queue;

    // Repeated-value scratch for non-zero FillBuffer, which WebGPU has no
    // encoder command for; kept between fills so the common small ones do not
    // reallocate.
    std::vector<uint32_t> m_FillPattern;

    WGPUCommandEncoder m_Encoder = nullptr;         // owned (+1) while recording
    WGPUCommandBuffer m_CommandBuffer = nullptr;    // owned (+1) after End()
    WGPURenderPassEncoder m_RenderPass = nullptr;   // owned (+1)
    WGPUComputePassEncoder m_ComputePass = nullptr; // owned (+1)
    std::vector<WGPUTextureView> m_TransientViews;  // owned (+1), released at End()
    // Row-repack scratch for unaligned buffer->texture copies. Owned (+1) and
    // released once the encoder is finished: the command buffer holds its own
    // reference until the submission retires.
    std::vector<WGPUBuffer> m_TransientBuffers;

    bool m_IsRecording = false;

    // Pending scope boundaries for pass-boundary timestamp writes (browser).
    static constexpr uint32_t kNoTimestampQuery = 0xFFFFFFFFu; // == WebGpuQueryPool::kInvalidQuery
    uint32_t m_PendingBeginQuery = kNoTimestampQuery; // written by the next pass begin
    uint32_t m_LastPassEndQuery = kNoTimestampQuery;  // end query of the last pass begun

    PipelineHandle m_CurrentPipeline{};
    PipelineHandle m_AppliedRenderPipeline{};
    // Mirrors RenderPassDesc::depthReadOnly for the open render pass: such a
    // pass rejects any pipeline that writes depth, so draws bind the pipeline's
    // depth-write-disabled twin instead.
    bool m_PassDepthReadOnly = false;
    BufferHandle m_IndexBuffer{};
    IndexType m_IndexType = IndexType::Uint16;
    // Indexed by engine vertex binding; ApplyRenderState maps each to the
    // current pipeline's WebGPU slot (WebGpuPipeline::vertexBindings).
    std::array<BufferHandle, kMaxVertexBufferSlots> m_VertexBuffers{};

    // Bind groups persist in the engine's command-list state but not in a WebGPU
    // pass encoder, so they are re-applied on the next draw or dispatch.
    static constexpr uint32_t kMaxBindGroups = 4;
    std::array<WGPUBindGroup, kMaxBindGroups> m_BindGroups{};
    // Debug-group balance per scope (see BeginEvent/EndEvent).
    uint32_t m_EncoderDebugDepth = 0;
    // Most recent debug-event name; labels the render pass it precedes.
    std::string m_EncoderScopeLabel;
    uint32_t m_RenderPassDebugDepth = 0;
    uint32_t m_ComputePassDebugDepth = 0;

    std::array<uint8_t, kMaxPushConstantBytes> m_PushConstantData{};
    uint32_t m_PushConstantDirtyBytes = 0;

    float m_ViewportX = 0.0f;
    float m_ViewportY = 0.0f;
    float m_ViewportWidth = 0.0f;
    float m_ViewportHeight = 0.0f;
    bool m_HasViewport = false;

    uint32_t m_RenderTargetWidth = 0;
    uint32_t m_RenderTargetHeight = 0;
};

} // namespace GameEngine::Rendering
