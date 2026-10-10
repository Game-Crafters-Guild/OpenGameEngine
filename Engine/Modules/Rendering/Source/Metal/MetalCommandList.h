#pragma once

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/QueryPool.h"

#include "MetalIndirectCountEncoder.h"
#include "MetalQueryPool.h"

#include <Metal/Metal.hpp>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class MetalDevice;
struct MetalTexture;

// Metal command list. Metal records through per-domain encoders (render /
// blit / compute) that must be ended before switching domains, so this class
// is a small encoder state machine: the active encoder is opened lazily on
// first use and closed when the domain changes or recording ends.
class MetalCommandList : public CommandList
{
  public:
    MetalCommandList(MetalDevice& device, IDevice::QueueType queue);
    ~MetalCommandList() override;

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
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
    void DrawMeshTasks(uint32_t taskCount) override;
    void DrawIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) override;
    void DrawIndexedIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) override;
    void DrawIndexedIndirectCount(BufferHandle commandBuffer, BufferHandle countBuffer, uint32_t maxDrawCount,
                                  uint32_t stride, size_t commandBufferOffset = 0, size_t countBufferOffset = 0) override;

    void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) override;
    void DispatchIndirect(BufferHandle argsBuffer, size_t argsOffsetBytes = 0) override;

    void CopyBuffer(BufferHandle src, BufferHandle dst, size_t size, size_t srcOffset = 0, size_t dstOffset = 0) override;
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
                                        size_t srcRowPitchBytes = 0, uint32_t depth = 1,
                                        size_t srcSlicePitchBytes = 0, uint32_t dstX = 0, uint32_t dstY = 0,
                                        ResourceState currentState = ResourceState::Undefined) override;
    void BlitImageMip(TextureHandle texture, uint32_t srcMip, uint32_t dstMip, uint32_t layer = 0) override;

    void BeginRenderPass(const RenderPassDesc& desc) override;
    void EndRenderPass() override;

    void SetViewport(float x, float y, float width, float height) override;
    void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
    void SetDepthBounds(float minDepth, float maxDepth) override;

    // Metal tracks hazards automatically for tracked resources; engine
    // barriers are recorded as encoder-boundary hints only.
    void Barrier(const ResourceBarrier& barrier) override;
    void BarrierBatch(const std::vector<ResourceBarrier>& barriers) override;

    void BeginEvent(const char* name) override;
    void EndEvent() override;
    void SetMarker(const char* name) override;

    void ClearRenderTarget(TextureHandle target, float r, float g, float b, float a) override;
    void GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) override;

    // Ends open encoders and hands the command buffer (+1, uncommitted) to
    // the caller — the device's submit queues own the commit so semaphore
    // waits can be satisfied CPU-side first. Returns nullptr if nothing was
    // recorded.
    // Opens (or returns) this command buffer's acceleration-structure encoder,
    // ending any other open encoder first. Used by MetalAccelerationStructures
    // to record BLAS/TLAS builds; that encoder boundary is also what orders
    // builds against prior GPU work (Metal tracks the hazards itself).
    MTL::AccelerationStructureCommandEncoder* EnsureAccelerationStructureEncoder();

    MTL::CommandBuffer* Detach();
    bool IsRecording() const { return m_IsRecording; }
    IDevice::QueueType GetQueueType() const { return m_Queue; }

    // Timestamp aliasing (see MetalQueryPool): binds the query to the encoder
    // boundary sample that matches the span end it marks — a span's begin to
    // the start of the encoder that carries the bracketed work, its end to that
    // encoder's end.
    void RecordTimestampQuery(MetalQueryPool& pool, uint32_t queryIndex, TimestampPoint point);
    // Set the active render encoder's visibility-result mode for occlusion
    // queries. `mode` is an MTL::VisibilityResultMode; `offsetBytes` indexes the
    // pass's attached visibility-result buffer. No-op outside a render pass.
    void SetVisibilityResultMode(uint32_t mode, uint64_t offsetBytes);

  private:
    // Reserved Metal buffer index for engine push constants. Matches the
    // SPIRV-Cross MSL default the milestone-3 translator will pin.
    static constexpr uint32_t kPushConstantBufferIndex = 30;
    static constexpr uint32_t kMaxPushConstantBytes = 128;

    void EndActiveEncoders();
    void EndRenderEncoder();
    void MarkEncoderWork() { ++m_EncoderWorkMark; }
    // A blit that writes `destination` may rewrite draw records or counts.
    void InvalidateIndirectCountTranslationIfWritten(const MetalBuffer* destination);
    MTL::BlitCommandEncoder* EnsureBlitEncoder();
    MTL::ComputeCommandEncoder* EnsureComputeEncoder();
    // ClearColorImageSubresource for a texture without render-target usage.
    void ClearColorImageSubresourceByBlit(const MetalTexture& tex, uint32_t mip, uint32_t layer,
                                          const float rgba[4]);
    void ApplyRenderState();
    void ApplyPushConstants();

    // Claims start/end boundary samples for a new encoder and binds pending
    // span-begin queries to the start. False when profiling is inactive or the
    // slice budget is exhausted (encoder is then created without samples).
    bool TryBeginEncoderSamples(bool splitStages, MTL::CounterSampleBuffer*& outBuffer,
                                MetalQueryPool::EncoderSampleSlots& outSlots);
    void LatchEncoderEndSample();
    void FlushPendingTimestampQueries();

    MetalDevice& m_Device;
    IDevice::QueueType m_Queue;

    MTL::CommandBuffer* m_CommandBuffer = nullptr; // owned (+1) while recording
    MTL::RenderCommandEncoder* m_RenderEncoder = nullptr;   // owned (+1)
    MTL::BlitCommandEncoder* m_BlitEncoder = nullptr;       // owned (+1)
    MTL::ComputeCommandEncoder* m_ComputeEncoder = nullptr; // owned (+1)
    MTL::AccelerationStructureCommandEncoder* m_AccelerationStructureEncoder = nullptr; // owned (+1)

    bool m_IsRecording = false;
    bool m_InRenderPass = false;

    PipelineHandle m_CurrentPipelineHandle{};
    // Render pipeline whose full state (PSO + DSS + cull/winding/fill/bias) is
    // currently applied to the active render encoder. Reset per encoder so a
    // fresh (stateless) encoder always re-applies; lets ApplyRenderState skip
    // the ~6 redundant Metal calls when the same PSO is re-set within a pass.
    PipelineHandle m_AppliedRenderPipeline{};
    BufferHandle m_IndexBuffer{};
    MTL::IndexType m_IndexType = MTL::IndexTypeUInt16;

    // Vulkan vertex binds persist across render-pass boundaries; Metal encoder
    // state does not. Track binds command-list-wide and reapply per encoder.
    static constexpr uint32_t kMaxVertexBufferSlots = 6;
    std::array<BufferHandle, kMaxVertexBufferSlots> m_VertexBuffers{};

    std::array<uint8_t, kMaxPushConstantBytes> m_PushConstantData{};
    uint32_t m_PushConstantDirtyBytes = 0;

    uint32_t m_RenderTargetWidth = 0;
    uint32_t m_RenderTargetHeight = 0;
    bool m_PassHasDepth = false;
    bool m_PassDepthReadOnly = false;
    // Attachment formats of the open pass — gates sticky pipeline reapply
    // (Metal validates formats at bind time, Vulkan only at draw time).
    PipelineFormatKey m_PassFormatKey{};

    // Active BeginEvent name — applied as the label of encoders opened inside
    // the event so GPU fault attribution names the render-graph pass.
    std::string m_CurrentDebugEventName;

    // GPU timestamp state (stage-boundary counter sampling).
    MetalQueryPool* m_TimestampPool = nullptr;
    // Span-begin queries waiting for the encoder that will carry their work,
    // with the work mark they were opened at (see m_EncoderWorkMark).
    struct PendingSpanBegin
    {
        uint32_t Query = UINT32_MAX;
        uint32_t WorkMark = 0;
    };
    std::vector<PendingSpanBegin> m_PendingTimestampQueries;
    // Counts commands that needed a GPU encoder. A span whose mark is unchanged
    // at its end recorded no GPU work, which is what separates "shared the
    // encoder that was already open" from "has nothing to measure".
    uint32_t m_EncoderWorkMark = 0;
    uint32_t m_OpenEncoderStartSample = UINT32_MAX; // start sample of the open encoder
    uint32_t m_OpenEncoderEndSample = UINT32_MAX;   // end sample of the open encoder
    uint32_t m_LastEndSample = UINT32_MAX;          // end sample of the last closed encoder

    // Per-encoder binding dedup. Residency declarations and argument-buffer
    // binds persist for the encoder's lifetime, so repeating them per draw is
    // pure overhead (Metal guidance: declare residency once per encoder, in
    // batches). Reset whenever a new encoder opens.
    void ResetEncoderBindingState();
    static constexpr uint32_t kMaxDescriptorSets = 8;
    // The argument table bound at each set index of the open encoder. Transient
    // sets share their frame's arena pages, so a table is its buffer and offset.
    struct BoundArgumentTable
    {
        MTL::Buffer* Buffer = nullptr;
        size_t Offset = 0;
    };
    std::array<BoundArgumentTable, kMaxDescriptorSets> m_EncoderBoundSets{};
    std::unordered_map<MTL::Resource*, MTL::ResourceUsage> m_EncoderResidents;
    std::vector<MTL::Resource*> m_ScratchReadResidents;
    std::vector<MTL::Resource*> m_ScratchWriteResidents;

    // Residency-set path (MetalDevice::UsesResidencySet): the BDA compute->draw
    // hazard is ordered with the device fence instead of useResources tracking.
    // A compute encoder signals the fence at end; the next render/compute
    // encoder waits before reading. True once a compute has signalled in the
    // current command buffer (the fence orders within a CB only); reset in
    // Begin() so a fresh CB does not wait on a stale signal.
    bool m_GpuDrivenFenceSignaled = false;

    MetalIndirectCountEncoder m_IndirectCount;
};

} // namespace Rendering
} // namespace GameEngine
