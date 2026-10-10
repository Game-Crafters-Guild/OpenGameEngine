#pragma once

#include "Device.h"
#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

// Enhanced render pass description with comprehensive clear control
struct RenderPassDesc
{
    TextureHandle colorTargets[8] = {INVALID_TEXTURE_HANDLE};
    TextureHandle depthTarget = INVALID_TEXTURE_HANDLE;
    uint32_t colorTargetCount = 0;

    // Optional per-color resolve targets (for MSAA resolves)
    TextureHandle resolveColorTargets[8] = {INVALID_TEXTURE_HANDLE};
    // Optional depth resolve target (for MSAA depth resolves)
    TextureHandle resolveDepthTarget = INVALID_TEXTURE_HANDLE;
    // Optional stencil resolve target (for MSAA stencil resolves)
    TextureHandle resolveStencilTarget = INVALID_TEXTURE_HANDLE;

    // Depth read-only hint (selects READ_ONLY layout in dynamic rendering)
    bool depthReadOnly = false;

    // Per-attachment clear control
    bool clearColor[8] = {true, false, false, false, false, false, false, false};
    bool clearDepth = true;
    bool clearStencil = false; // ✅ FIXED: Added missing stencil clear support

    // Clear values
    float clearColorValue[8][4] = {
        {0.0f, 0.0f, 1.0f, 1.0f}, // RT0 default: blue
        {0.0f, 0.0f, 0.0f, 0.0f}, // RT1-7 default: black
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f}};
    float clearDepthValue = 0.0f;  // reverse-Z: clear to far
    uint32_t clearStencilValue = 0; // ✅ FIXED: Added stencil clear value

    // Resolve mode selection (engine-level; backends map to native resolve flags)
    enum class ResolveMode : uint8_t
    {
        Average = 0,
        SampleZero = 1,
        Min = 2,
        Max = 3
    };

    // Load/Store operations for fine-grained control
    enum class LoadOp
    {
        Load,
        Clear,
        DontCare
    };
    enum class StoreOp
    {
        Store,
        DontCare,
        // Preserve the contents WITHOUT performing an attachment write. Only
        // valid for an attachment nothing writes during the pass (read-only
        // depth); a write makes the contents undefined. Store is the fallback
        // where the backend cannot express it — the contents survive either
        // way, but Store is a write and so must be covered by the source scope
        // of the next barrier that touches the image.
        None
    };

    LoadOp colorLoadOp[8] = {LoadOp::Clear, LoadOp::DontCare, LoadOp::DontCare, LoadOp::DontCare,
                             LoadOp::DontCare, LoadOp::DontCare, LoadOp::DontCare, LoadOp::DontCare};
    StoreOp colorStoreOp[8] = {StoreOp::Store, StoreOp::DontCare, StoreOp::DontCare, StoreOp::DontCare,
                               StoreOp::DontCare, StoreOp::DontCare, StoreOp::DontCare, StoreOp::DontCare};
    LoadOp depthLoadOp = LoadOp::Clear;
    StoreOp depthStoreOp = StoreOp::Store;
    LoadOp stencilLoadOp = LoadOp::DontCare;
    StoreOp stencilStoreOp = StoreOp::DontCare;

    // Optional per-attachment custom views (select aspect/mip/layer)
    bool useColorView[8] = {false, false, false, false, false, false, false, false};
    TextureViewDesc colorViewDesc[8] = {};
    bool useDepthView = false;
    TextureViewDesc depthViewDesc = {};

    // Optional explicit views for resolve attachments
    bool useColorResolveView[8] = {false, false, false, false, false, false, false, false};

    // Optional explicit views and modes for stencil resolve
    bool useStencilResolveView = false;
    TextureViewDesc stencilResolveViewDesc = {};

    // Optional resolve mode overrides
    bool useColorResolveMode[8] = {false, false, false, false, false, false, false, false};
    ResolveMode colorResolveMode[8] = {};
    bool useDepthResolveMode = false;
    ResolveMode depthResolveMode = ResolveMode::SampleZero;
    bool useStencilResolveMode = false;
    ResolveMode stencilResolveMode = ResolveMode::SampleZero;

    TextureViewDesc colorResolveViewDesc[8] = {};
    bool useDepthResolveView = false;
    TextureViewDesc depthResolveViewDesc = {};

    // When true, the render pass declares that its contents come from secondary
    // command buffers: the primary CB cannot record draws directly, it may only
    // execute the secondaries. Use this when secondary command buffers will be
    // recorded in parallel and executed here.
    bool useSecondaryCommandBuffers = false;
};

// Lightweight capability helpers for resolve modes.
// Bit positions are defined by the engine; backends map these bits to their native resolve flags.
inline uint32_t ResolveModeToBit(RenderPassDesc::ResolveMode m)
{
    switch (m)
    {
    case RenderPassDesc::ResolveMode::SampleZero:
        return 0x1u;
    case RenderPassDesc::ResolveMode::Average:
        return 0x2u;
    case RenderPassDesc::ResolveMode::Min:
        return 0x4u;
    case RenderPassDesc::ResolveMode::Max:
        return 0x8u;
    }
    return 0u;
}
inline bool SupportsDepthResolveMode(const IDevice& device, RenderPassDesc::ResolveMode mode)
{
    const auto& caps = device.GetCapabilities();
    return (caps.supportedDepthResolveModes & ResolveModeToBit(mode)) != 0;
}
inline bool SupportsStencilResolveMode(const IDevice& device, RenderPassDesc::ResolveMode mode)
{
    const auto& caps = device.GetCapabilities();
    return (caps.supportedStencilResolveModes & ResolveModeToBit(mode)) != 0;
}
inline bool SupportsDepthResolveMode(const RenderingDeviceCapabilities& caps, RenderPassDesc::ResolveMode mode)
{
    return (caps.supportedDepthResolveModes & ResolveModeToBit(mode)) != 0;
}
inline bool SupportsStencilResolveMode(const RenderingDeviceCapabilities& caps, RenderPassDesc::ResolveMode mode)
{
    return (caps.supportedStencilResolveModes & ResolveModeToBit(mode)) != 0;
}

// Forward declare ResourceState from RenderGraph.h
enum class ResourceState : uint32_t;

// Index type for indexed draws
enum class IndexType : uint8_t
{
    Uint16 = 0,
    Uint32 = 1
};

// Resource barrier description (extended with subresource and queue ownership)
struct ResourceBarrier
{
    enum Type
    {
        Buffer,
        Texture,
        Memory
    } type;

    // Resource handles
    BufferHandle bufferHandle;
    TextureHandle textureHandle;

    // Abstract resource states (for fallback mapping if explicit masks are not provided)
    ResourceState stateBefore;
    ResourceState stateAfter;

    // Subresource range for textures (ignored for buffers)
    struct Subresource
    {
        uint32_t baseMip;
        uint32_t levelCount;
        uint32_t baseLayer;
        uint32_t layerCount;
    } subresource;

    // Queue ownership transfer (use kQueueFamilyIgnored when not transferring)
    uint32_t srcQueueFamily;
    uint32_t dstQueueFamily;

    // Link back to RenderGraph resource and optional version (0 when not applicable)
    uint32_t rgResourceId;
    uint32_t rgVersionId;

    // stateBefore is the render graph's compiled per-subresource truth: the
    // recording layer takes oldLayout from it verbatim — no tracker
    // reconciliation. Set only by RenderGraph recording for texture barriers.
    // Swapchain images keep their acquire/present special case regardless (the
    // graph does not model acquire).
    bool rgAuthoritativeLayout;

    // Optional precomputed stage/access masks (0 means: derive from state)
    uint64_t srcStageMask;
    uint64_t dstStageMask;
    uint64_t srcAccessMask;
    uint64_t dstAccessMask;

    // Default constructor
    ResourceBarrier()
        : type(Buffer),
          bufferHandle(INVALID_HANDLE),
          textureHandle(INVALID_HANDLE),
          stateBefore(ResourceState::Undefined),
          stateAfter(ResourceState::Undefined),
          subresource{0u, 1u, 0u, 1u},
          srcQueueFamily(0xFFFFFFFFu),
          dstQueueFamily(0xFFFFFFFFu),
          rgResourceId(0u),
          rgVersionId(0u),
          rgAuthoritativeLayout(false),
          srcStageMask(0), dstStageMask(0), srcAccessMask(0), dstAccessMask(0) {}

    // Constructor for buffer barriers
    static ResourceBarrier CreateBufferBarrier(BufferHandle buffer, ResourceState before, ResourceState after)
    {
        ResourceBarrier barrier;
        barrier.type = Buffer;
        barrier.bufferHandle = buffer;
        barrier.textureHandle = INVALID_HANDLE;
        barrier.stateBefore = before;
        barrier.stateAfter = after;
        // subresource ignored for buffers
        return barrier;
    }

    // Constructor for texture barriers
    static ResourceBarrier CreateTextureBarrier(TextureHandle texture, ResourceState before, ResourceState after,
                                                uint32_t baseMip = 0u, uint32_t levelCount = 1u,
                                                uint32_t baseLayer = 0u, uint32_t layerCount = 1u)
    {
        ResourceBarrier barrier;
        barrier.type = Texture;
        barrier.bufferHandle = INVALID_HANDLE;
        barrier.textureHandle = texture;
        barrier.stateBefore = before;
        barrier.stateAfter = after;
        barrier.subresource = {baseMip, levelCount, baseLayer, layerCount};
        return barrier;
    }

    // Constructor for global memory barriers (no specific resource)
    static ResourceBarrier CreateMemoryBarrier(uint64_t srcStageMask,
                                               uint64_t dstStageMask,
                                               uint64_t srcAccessMask,
                                               uint64_t dstAccessMask)
    {
        ResourceBarrier barrier;
        barrier.type = Memory;
        barrier.bufferHandle = INVALID_HANDLE;
        barrier.textureHandle = INVALID_HANDLE;
        // For memory barriers, explicit stage/access masks must be provided; states are ignored
        barrier.srcStageMask = srcStageMask;
        barrier.dstStageMask = dstStageMask;
        barrier.srcAccessMask = srcAccessMask;
        barrier.dstAccessMask = dstAccessMask;
        return barrier;
    }
};

// Special constant for queue family indices (no ownership transfer)
constexpr uint32_t kQueueFamilyIgnored = 0xFFFFFFFFu;

/**
 * @brief Command list interface for recording GPU commands
 *
 * Provides unified interface for recording rendering commands
 * that can be executed on Vulkan or DirectX 12.
 */
class CommandList
{
  public:
    virtual ~CommandList() = default;

    // Command list management
    virtual void Begin() = 0;
    virtual void End() = 0;

    // Pipeline state
    virtual void SetPipeline(PipelineHandle pipeline) = 0;
    virtual void SetVertexBuffer(BufferHandle buffer, uint32_t slot = 0) = 0;
    virtual void SetIndexBuffer(BufferHandle buffer, IndexType indexType = IndexType::Uint16) = 0;
    // Note: Push constant payloads larger than 128 bytes are not supported; use a UBO for larger data.
    // In debug builds, over‑limit calls to SetConstants/SetPushConstants will assert and log an error.
    // Future versions may narrow per‑pipeline ranges via reflection; 128 bytes is the conservative default.

    // Legacy form (offset=0)
    virtual void SetConstants(uint32_t slot, size_t size, const void* data) = 0;
    // New: explicit offset variant
    virtual void SetConstants(uint32_t slot, uint32_t offset, size_t size, const void* data) = 0;

    // Strongly-typed push constants helper; forwards to SetConstants(slot=0)
    template <typename T>
    void SetPushConstants(const T& data, uint32_t offset = 0)
    {
        SetConstants(0, offset, sizeof(T), &data);
    }

    // Push constants by named range (multi-range support). Returns false if no such range on current pipeline
    virtual bool SetPushConstantsByName(const char* rangeName, const void* data, size_t size, uint32_t offset = 0) = 0;
    template <typename T>
    bool SetPushConstantsByName(const char* rangeName, const T& data, uint32_t offset = 0)
    {
        return SetPushConstantsByName(rangeName, &data, sizeof(T), offset);
    }

    // Push constants by range id (fast path once name -> id is resolved)
    virtual bool SetPushConstantsById(uint32_t rangeId, const void* data, size_t size, uint32_t offset = 0) = 0;
    template <typename T>
    bool SetPushConstantsById(uint32_t rangeId, const T& data, uint32_t offset = 0)
    {
        return SetPushConstantsById(rangeId, &data, sizeof(T), offset);
    }

    // Backend-agnostic binding (preferred)
    virtual void BindDescriptorSet(uint32_t set, DescriptorSetHandle descriptorSet, PipelineHandle pipeline) = 0;

    // Descriptor-buffer bindings. Backend-agnostic shape; the Vulkan backend maps
    // address to a device address and kind to the matching buffer usage. Base class provides
    // no-op default so callers can write the DB path safely on backends/builds where the
    // feature is not yet wired; Vulkan override also no-ops when
    // IsDescriptorBufferEnabled() is false.
    enum class DescriptorBufferKind : uint8_t
    {
        Resource = 0, // Block holding resource descriptors
        Sampler  = 1, // Block holding sampler descriptors
    };
    struct DescriptorBufferBinding
    {
        uint64_t             address = 0; // device address of the descriptor-buffer block
        DescriptorBufferKind kind    = DescriptorBufferKind::Resource;
    };
    virtual void BindDescriptorBuffers(const DescriptorBufferBinding* bindings, uint32_t count)
    {
        (void)bindings; (void)count;
    }
    virtual void SetDescriptorBufferOffsets(PipelineHandle pipeline,
                                            uint32_t firstSet,
                                            const uint32_t* bufferIndices,
                                            const uint64_t* offsets,
                                            uint32_t setCount)
    {
        (void)pipeline; (void)firstSet; (void)bufferIndices; (void)offsets; (void)setCount;
    }

    // Drawing commands
    // Legacy forms (no subrange)
    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount = 1) = 0;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1) = 0;
    virtual void DrawMeshTasks(uint32_t taskCount) = 0;

    // Preferred subrange overloads (backends should override). Base versions fall back to legacy forms.
    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance = 0)
    {
        (void)firstVertex;
        (void)firstInstance;
        Draw(vertexCount, instanceCount);
    }
    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance = 0)
    {
        (void)firstIndex;
        (void)vertexOffset;
        (void)firstInstance;
        DrawIndexed(indexCount, instanceCount);
    }

    // Indirect drawing commands (GPU-driven rendering)
    virtual void DrawIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) = 0;
    virtual void DrawIndexedIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) = 0;

    // Indirect-count variant. `countBuffer` stores a single uint32 at
    // `countBufferOffset` -- the actual draw count, capped at `maxDrawCount`.
    // Useful when the producer is a compute pass that doesn't know the final
    // draw count until after dispatch (bucketer / draw_command_scatter). Backed
    // by vkCmdDrawIndexedIndirectCount on Vulkan (core 1.2+ / KHR earlier).
    virtual void DrawIndexedIndirectCount(BufferHandle commandBuffer,
                                          BufferHandle countBuffer,
                                          uint32_t     maxDrawCount,
                                          uint32_t     stride,
                                          size_t       commandBufferOffset = 0,
                                          size_t       countBufferOffset   = 0) = 0;

    // Compute commands
    virtual void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) = 0;

    // Indirect compute dispatch. `argsBuffer` supplies the workgroup counts at
    // `argsOffsetBytes` in the indirect-dispatch layout: three tightly
    // packed uint32 (groupCountX, groupCountY, groupCountZ). The producing pass
    // must transition the buffer to ResourceState::IndirectArgs (RGBufferRead::
    // Indirect) before this consumes it, so the GPU sees the written counts.
    // Backed by vkCmdDispatchIndirect on Vulkan; the args read happens at the
    // DRAW_INDIRECT stage, same as indirect draws.
    virtual void DispatchIndirect(BufferHandle argsBuffer, size_t argsOffsetBytes = 0) = 0;

    // Resource operations
    virtual void CopyBuffer(BufferHandle src, BufferHandle dst, size_t size,
                            size_t srcOffset = 0, size_t dstOffset = 0) = 0;
    virtual void CopyTexture(TextureHandle src, TextureHandle dst) = 0;

    // GPU-side buffer fill: writes `value` (a u32) across [offset, offset+size)
    // of `dst` in the GPU timeline, sequenced with subsequent commands. Use
    // when CPU-immediate UpdateBuffer would race with in-flight frames or with
    // later reads in the same submission. `size` and `offset` must be 4-byte
    // aligned and within the buffer's range. (Vulkan: vkCmdFillBuffer.)
    virtual void FillBuffer(BufferHandle dst, size_t offset, size_t size, uint32_t value) = 0;
    // Readback helpers
    // Copy mip 0, layer 0 of a 2D texture to a buffer
    virtual void CopyTextureToBuffer(TextureHandle srcTexture, BufferHandle dstBuffer,
                                     uint32_t width, uint32_t height, uint32_t SrcX = 0, uint32_t SrcY = 0,
                                     size_t DstOffsetBytes = 0, size_t dstRowPitchBytes = 0) = 0;
    // Copy a specific mip/layer of a texture into a buffer.
    //
    // For 2D and 2D-array images: `layer` is the array layer; `depth` must be 1.
    // For 3D images: `layer` is the Z offset in texels; `depth` is the Z extent
    // to copy. `dstSlicePitchBytes` is bytes between Z slices in the destination
    // buffer (0 = tightly packed slices of height × row pitch). The same Vulkan
    // accepts gapped / D3D12 requires tight asymmetry applies as for the upload
    // helper above.
    virtual void CopyTextureSubresourceToBuffer(TextureHandle srcTexture, uint32_t mip, uint32_t layer,
                                                BufferHandle dstBuffer,
                                                uint32_t width, uint32_t height,
                                                uint32_t SrcX = 0, uint32_t SrcY = 0,
                                                size_t DstOffsetBytes = 0, size_t dstRowPitchBytes = 0,
                                                uint32_t depth = 1, size_t dstSlicePitchBytes = 0) = 0;

    // Utility: clear a color image subresource (mip/layer) directly via transfer
    virtual void ClearColorImageSubresource(TextureHandle texture, uint32_t mip, uint32_t layer, const float rgba[4]) = 0;

    // Copy a buffer region into a color image. Handles layout transition to copy dest internally.
    //
    // For 2D and 2D-array images: `layer` is the array layer; `depth` must be 1; `imageExtent.depth` is 1.
    // For 3D images (`extent.depth` > 1): `layer` is the Z offset in texels; `depth` is the copy extent
    // in Z. `srcSlicePitchBytes` is bytes between Z slices (0 = tightly packed slices of height × row pitch).
    //
    // `srcRowPitchBytes` 0 means tightly packed: every backend resolves it to
    // width * bytes-per-texel of the destination format.
    //
    // Backend asymmetry:
    //   - Vulkan and Metal accept a gapped slice pitch and any row pitch.
    //   - D3D12 requires Z slices to be tightly packed in the source buffer (PLACED_FOOTPRINT
    //     has no slice-pitch field) AND requires srcRowPitchBytes to be a multiple of
    //     D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256). The same applies to srcOffsetBytes
    //     (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, 512). Violations are refused, not recorded.
    //   - WebGPU requires a multi-row copy's row pitch to be a multiple of
    //     COPY_BYTES_PER_ROW_ALIGNMENT (256). The backend meets that itself: an unaligned
    //     pitch is restrided into scratch on the GPU, costing one buffer copy per row of
    //     the upload. Only a pitch that is not 4-byte addressable (the granularity of a
    //     buffer copy) is refused.
    // A caller that wants the D3D12 constraints handled for it too can use
    // UploadTexturesBatched, which repacks and aligns from caps.textureCopyRowPitchAlignment.
    //
    // No backend records a copy it has already rejected: an invalid copy poisons the whole
    // command buffer, so the surrounding commands would be lost along with it.
    //
    // `dstX`/`dstY` are the destination texel origin of the copy (default 0,0 = top-left).
    // The copied rect must satisfy dstX+width <= mipWidth and dstY+height <= mipHeight.
    //
    // `currentState` is the subresource's layout/state at the moment of the copy, as known
    // by the caller. It exists for PARTIAL copies to a persistent texture whose contents
    // outside the copied rect must survive: when a real state is supplied, the internal
    // pre-copy transition moves FROM that layout (content-preserving) with the matching
    // source stage/access, so it also serializes against in-flight readers on the queue.
    // When left Undefined (the default, and correct for whole-subresource rewrites), the
    // old layout is resolved from command-list-local tracking, which for a fresh list means
    // UNDEFINED — a legal content DISCARD. A sub-region copy that leaves this Undefined will
    // therefore corrupt the untouched texels; region callers MUST supply the resting state.
    virtual void CopyBufferToTextureSubresource(BufferHandle srcBuffer, TextureHandle dstTexture,
                                                uint32_t mip, uint32_t layer,
                                                uint32_t width, uint32_t height,
                                                size_t srcOffsetBytes = 0, size_t srcRowPitchBytes = 0,
                                                uint32_t depth = 1, size_t srcSlicePitchBytes = 0,
                                                uint32_t dstX = 0, uint32_t dstY = 0,
                                                ResourceState currentState = ResourceState::Undefined) = 0;

    // Utility: blit between two mips of the same 2D texture (layer 0 by default)
    virtual void BlitImageMip(TextureHandle texture, uint32_t srcMip, uint32_t dstMip, uint32_t layer = 0) = 0;

    // Render passes
    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void EndRenderPass() = 0;

    // Secondary command buffer support (Phase 4: parallel recording).
    // The render pass state a secondary CB inherits from the primary.
    struct SecondaryBeginInfo
    {
        uint32_t colorAttachmentCount = 0;
        uint32_t colorFormats[8] = {};       // TextureFormat cast to uint32_t
        uint32_t depthFormat = 0;            // 0 = no depth
        uint32_t sampleCount = 1;
        // Mirrors RenderPassDesc::depthReadOnly for the render pass this
        // secondary is executed in. It MUST be conveyed, not defaulted: a pass
        // filled by secondaries can record no state on the primary, so the
        // secondary is the only place depth-write-enable is ever set. Left
        // false in a read-only pass, an opaque pipeline's compiled
        // depthWriteEnable=true is applied and the pass writes an attachment
        // declared read-only — which also makes StoreOp::None undefined.
        bool depthReadOnly = false;
    };

    // Begin recording as a secondary command buffer inheriting the given
    // render pass state. Only valid on command lists created via
    // IDevice::CreateSecondaryCommandList(). Must be paired with End().
    virtual void BeginSecondary(const SecondaryBeginInfo& info) { (void)info; }

    // Execute previously recorded secondary command lists from within
    // a render pass that was begun with useSecondaryCommandBuffers=true.
    // Only valid on primary command lists, inside BeginRenderPass/EndRenderPass.
    virtual void ExecuteSecondary(CommandList* const* secondaries, uint32_t count) { (void)secondaries; (void)count; }

    // Viewport and scissor
    virtual void SetViewport(float x, float y, float width, float height) = 0;
    virtual void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;

    // Depth bounds (requires depth-bounds testing support and, on some backends, appropriate dynamic state enablement)
    virtual void SetDepthBounds(float minDepth, float maxDepth) = 0;

    // Synchronization
    virtual void Barrier(const ResourceBarrier& barrier) = 0;

    // Batch multiple barriers in one call (default implementations should emit a single backend barrier call)
    virtual void BarrierBatch(const std::vector<ResourceBarrier>& barriers) = 0;

    // Debug and profiling
    virtual void BeginEvent(const char* name) = 0;
    virtual void EndEvent() = 0;
    virtual void SetMarker(const char* name) = 0;

    virtual void ClearRenderTarget(TextureHandle target, float r, float g, float b, float a) = 0;

    // Query helpers
    virtual void GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) = 0;
};

} // namespace Rendering
} // namespace GameEngine
