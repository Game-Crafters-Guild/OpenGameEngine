/**
 * @file VulkanCommandList.h
 * @brief Vulkan implementation of CommandList
 */

#pragma once

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Handle.h"
#include <vulkan/vulkan.h>
#include <array>
#include <string>

namespace GameEngine {
namespace Rendering {

    // Thread-safe ring of the most recent GPU debug markers recorded across ALL
    // command lists (fed by VulkanCommandList::SetMarker). The VulkanDevice
    // device-lost handler dumps it so a TDR names the recently-recorded passes
    // (e.g. the CBT.* kernel groups) instead of being an anonymous hang. CPU-side:
    // recording runs ahead of GPU execution, so this is a locality hint (read
    // newest-last), not a precise executed-culprit. The executed side comes from
    // GPU checkpoints (VulkanGpuCheckpoints.h), which SetMarker also emits when
    // GE_VK_GPU_CHECKPOINTS is set and the device supports them; the two are
    // dumped together, and the gap between them brackets the faulting work.
    void RecordDiagnosticMarker(const char* name);
    std::string DumpRecentDiagnosticMarkers();

    // Forward declaration
    class VulkanDevice;
    struct VulkanPipeline;

    /**
     * @brief Vulkan implementation of CommandList
     *
     * Clean command buffer implementation following reference patterns.
     * Focuses on core functionality without excessive validation.
     */
    class VulkanCommandList : public CommandList {
    public:
        VulkanCommandList(VulkanDevice* device, IDevice::QueueType queue, VkCommandBuffer commandBuffer, VkCommandPool commandPool);
        ~VulkanCommandList() override;

        // Queue type accessor
        IDevice::QueueType GetQueueType() const { return m_QueueType; }

        // Called by QueueSubmit after the VkCommandBuffer has been added to usedCmd.
        // Releases command-buffer-scoped helpers through the device's deferred
        // destruction path, then nulls out m_CommandBuffer so the destructor won't
        // double-recycle it.
        void OnSubmitted();

        // Access the underlying VkCommandBuffer (for submit/tracking).
        VkCommandBuffer GetVkCommandBuffer() const { return m_CommandBuffer; }

        // Per-thread pool ownership for thread-safe command buffer recycling.
        void SetThreadPoolOwner(void* owner) { m_ThreadPoolOwner = owner; }
        void* GetThreadPoolOwner() const { return m_ThreadPoolOwner; }

        // Secondary command buffer support for parallel recording.
        // Describes the rendering state inherited from the primary command buffer.
        struct SecondaryInheritance
        {
            uint32_t colorAttachmentCount = 0;
            VkFormat colorFormats[8] = {};
            VkFormat depthFormat = VK_FORMAT_UNDEFINED;
            VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
            VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
            uint32_t viewMask = 0;
            // The inherited render pass declares depth read-only, so this
            // secondary must force vkCmdSetDepthWriteEnable(VK_FALSE) — nothing
            // on the primary sets it for a pass filled by secondaries.
            bool depthReadOnly = false;
        };

        // Begin as a secondary command buffer that will be executed inside a
        // dynamic rendering scope on a primary command buffer.
        void BeginSecondary(const SecondaryInheritance& inheritance);

        bool IsSecondary() const { return m_IsSecondary; }

        // Execute secondary command buffers from this primary command buffer.
        // Must be called inside a BeginRenderPass/EndRenderPass scope that was
        // opened with useSecondaryCommandBuffers = true.
        void ExecuteSecondaryCommandBuffers(const VkCommandBuffer* secondaries, uint32_t count);

        // VK_ATTACHMENT_STORE_OP_NONE preserves an attachment's contents only for
        // as long as nothing writes it during the render pass; a write voids the
        // guarantee and leaves the contents undefined. `storeOpNoneUsable` asserts
        // both halves — the device can express NONE, and this render pass actually
        // suppresses the writes. Without it NONE degrades to STORE, which preserves
        // the contents unconditionally; DONT_CARE is never substituted because it
        // would discard depth a later pass still samples.
        static VkAttachmentStoreOp ResolveStoreOp(RenderPassDesc::StoreOp op, bool storeOpNoneUsable);

        // CommandList interface
        void Begin() override;
        void End() override;

        // Pipeline state
        void SetPipeline(PipelineHandle pipeline) override;

        // Resource binding
        void SetVertexBuffer(BufferHandle buffer, uint32_t slot = 0) override;
        void SetIndexBuffer(BufferHandle buffer, IndexType indexType = IndexType::Uint16) override;
        void SetConstants(uint32_t slot, size_t size, const void* data) override;
        void SetConstants(uint32_t slot, uint32_t offset, size_t size, const void* data) override;
        void BindDescriptorSet(uint32_t set, DescriptorSetHandle descriptorSet, PipelineHandle pipeline) override;
        void BindDescriptorBuffers(const DescriptorBufferBinding* bindings, uint32_t count) override;
        void SetDescriptorBufferOffsets(PipelineHandle pipeline,
                                        uint32_t firstSet,
                                        const uint32_t* bufferIndices,
                                        const uint64_t* offsets,
                                        uint32_t setCount) override;
        bool SetPushConstantsByName(const char* rangeName, const void* data, size_t size, uint32_t offset = 0) override;


        bool SetPushConstantsById(uint32_t rangeId, const void* data, size_t size, uint32_t offset = 0) override;

        // Drawing
        void Draw(uint32_t vertexCount, uint32_t instanceCount = 1) override;
        void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance = 0) override;
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1) override;
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance = 0) override;
        void DrawMeshTasks(uint32_t taskCount) override;

        // Indirect drawing (GPU-driven rendering)
        void DrawIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) override;
        void DrawIndexedIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride) override;
        void DrawIndexedIndirectCount(BufferHandle commandBuffer,
                                      BufferHandle countBuffer,
                                      uint32_t     maxDrawCount,
                                      uint32_t     stride,
                                      size_t       commandBufferOffset = 0,
                                      size_t       countBufferOffset   = 0) override;

        // Compute
        void Dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) override;
        void DispatchIndirect(BufferHandle argsBuffer, size_t argsOffsetBytes = 0) override;

        // Render passes
        void BeginRenderPass(const RenderPassDesc& desc) override;
        void EndRenderPass() override;

        // Abstract secondary CB interface (Phase 4)
        void BeginSecondary(const SecondaryBeginInfo& info) override;
        void ExecuteSecondary(CommandList* const* secondaries, uint32_t count) override;

        // Viewport and scissor
        void SetViewport(float x, float y, float width, float height) override;
        void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;

        // Depth bounds
        void SetDepthBounds(float minDepth, float maxDepth) override;

        // Resource operations
        void CopyBuffer(BufferHandle src, BufferHandle dst, size_t size, size_t srcOffset = 0, size_t dstOffset = 0) override;
        void CopyTexture(TextureHandle src, TextureHandle dst) override;
        void FillBuffer(BufferHandle dst, size_t offset, size_t size, uint32_t value) override;

	        // Readback helper implementation
	        void CopyTextureToBuffer(TextureHandle srcTexture, BufferHandle dstBuffer,
	                                 uint32_t width, uint32_t height, uint32_t SrcX = 0, uint32_t SrcY = 0,
	                                 size_t DstOffsetBytes = 0, size_t dstRowPitchBytes = 0) override;


	        // Readback helper for specific mip/layer (or 3D Z-range)
	        void CopyTextureSubresourceToBuffer(TextureHandle srcTexture, uint32_t mip, uint32_t layer,
	                                           BufferHandle dstBuffer,
	                                           uint32_t width, uint32_t height,
	                                           uint32_t SrcX = 0, uint32_t SrcY = 0,
	                                           size_t DstOffsetBytes = 0, size_t dstRowPitchBytes = 0,
	                                           uint32_t depth = 1, size_t dstSlicePitchBytes = 0) override;


                // Utility: clear a color image subresource directly
                void ClearColorImageSubresource(TextureHandle texture, uint32_t mip, uint32_t layer, const float rgba[4]) override;

                // Utility: upload from buffer into a mip/layer (or Z range for 3D) of a color image
                void CopyBufferToTextureSubresource(BufferHandle srcBuffer, TextureHandle dstTexture,
                                                   uint32_t mip, uint32_t layer,
                                                   uint32_t width, uint32_t height,
                                                   size_t srcOffsetBytes, size_t srcRowPitchBytes,
                                                   uint32_t depth, size_t srcSlicePitchBytes,
                                                   uint32_t dstX, uint32_t dstY,
                                                   ResourceState currentState) override;


	                // Utility: blit between two mips on the same image
	                void BlitImageMip(TextureHandle texture, uint32_t srcMip, uint32_t dstMip, uint32_t layer = 0) override;



        // Synchronization
        void Barrier(const ResourceBarrier& barrier) override;

        void BarrierBatch(const std::vector<ResourceBarrier>& barriers) override;

        // Debug events
        void BeginEvent(const char* name) override;
        void EndEvent() override;
        void SetMarker(const char* name) override;

        // Recording/execution state
        bool IsRecording() const { return m_IsRecording; }
        bool IsReadyToSubmit() const { return m_CommandBuffer != VK_NULL_HANDLE && m_HasBegun && m_HasEnded && !m_IsRecording; }

        // Query helpers
        void GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) override;
        void ClearRenderTarget(TextureHandle target, float r, float g, float b, float a) override;

        // Resources named by BeginRenderPass attachments and by the copy/fill commands.
        // Dispatch, Draw and BindDescriptorSet record nothing here, so this is not a
        // complete access list and must not be used as one. Its only consumer is the
        // device's "did this list touch a swapchain image" test, which reads
        // ResourceType and Id; the remaining fields have no reader.
        struct UsedResource {
            enum class Type { Buffer, Texture } ResourceType;
            Handle Id;
            bool Writes;
            VkPipelineStageFlags StageMask;
            VkAccessFlags AccessMask;
            VkImageLayout Layout; // valid only for textures
            // Subresource precision for textures
            uint32_t BaseMip = 0;
            uint32_t LevelCount = 0xFFFFFFFFu;
            uint32_t BaseLayer = 0;
            uint32_t LayerCount = 0xFFFFFFFFu;
            VkImageAspectFlags AspectMask = 0; // optional; if 0, derive from handle
        };
        const std::vector<UsedResource>& GetUsedResources() const { return m_UsedResources; }

    private:
        // Single source of truth for vkCmdPushConstants stage flags — every push
        // path (SetConstants x4, by-name, by-id) resolves through here so the
        // flags exactly match the bound pipeline layout's declared range
        // (VUID-vkCmdPushConstants-offset-01795/01796): classic graphics layouts
        // carry the canonical kGraphicsPushConstantStages range, compute layouts
        // COMPUTE_BIT, mesh layouts their stored metadata stages. Asserts against
        // VulkanPipeline::layoutPushStageFlags when the pipeline recorded it.
        VkShaderStageFlags ResolvePushConstantStageFlags(const VulkanPipeline* pipeline,
                                                         VkShaderStageFlags metadataStages) const;

        VulkanDevice* m_Device;
        VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
        IDevice::QueueType m_QueueType = IDevice::QueueType::Graphics;
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        void* m_ThreadPoolOwner = nullptr; // owning ThreadCommandPoolEntry* (opaque to avoid header dep)
        // Thread-pool generation the buffer + owner were acquired at (Q6 slice 4).
        // A mismatch means a device rebuild freed that pool; the buffer + owner are
        // dead and must not be reset or recycled.
        uint32_t m_PoolGeneration = 0;
        bool m_IsSecondary = false; // true if this is a secondary command buffer

        // Per-CB layout journal (mip,layer), cleared every Begin(). Serves
        // NON-graph surfaces only: oldLayout recovery for barriers recorded
        // outside the render graph, plus utility ops (readback copies, mip-gen,
        // render-pass attachment fallbacks) that interleave with graph work on
        // this CB. Graph-owned barriers never read it — their oldLayout is the
        // RG's compiled stateBefore (ResourceBarrier::rgAuthoritativeLayout).
        // Key: (textureId << 32) | (mip << 8) | layer
        std::unordered_map<uint64_t, VkImageLayout> m_SubresourceLayouts;
        static inline uint64_t MakeSubKey(Handle h, uint32_t mip, uint32_t layer) {
            return (static_cast<uint64_t>(h.id) << 32) | (static_cast<uint64_t>(mip & 0xFF) << 8) | static_cast<uint64_t>(layer & 0xFF);
        }

        // Scratch barrier buffers for batched barriers (reused per call)
        std::vector<VkMemoryBarrier> m_ScratchMemBarriers;
        std::vector<VkBufferMemoryBarrier> m_ScratchBufBarriers;
        std::vector<VkImageMemoryBarrier> m_ScratchImgBarriers;
        std::vector<VkMemoryBarrier2> m_ScratchMemBarriers2;
        std::vector<VkBufferMemoryBarrier2> m_ScratchBufBarriers2;
        std::vector<VkImageMemoryBarrier2> m_ScratchImgBarriers2;
        std::vector<VkImageMemoryBarrier2> m_ScratchMergedImgBarriers2;

        // State tracking
        bool m_IsRecording = false;
        bool m_HasBegun = false;
        bool m_HasEnded = false;
        VkPipelineBindPoint m_CurrentBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        VkPipelineLayout m_CurrentPipelineLayout = VK_NULL_HANDLE;
        // Warn-once latches, so a broken pass reports itself without flooding the log
        // every frame: the first for a dispatch with nothing bound, the second for a
        // SetPipeline whose handle no longer resolves to a VkPipeline.
        bool m_WarnedNoComputePipeline = false;
        bool m_WarnedNullPipelineBind = false;
        bool m_DynamicRenderingActive = false;
        bool m_DynamicRenderingBegun = false;
        bool m_DepthReadOnly = false; // Current render pass depth read-only state
        bool m_LegacyRenderPassBegun = false;
        // Captured used resources for this command list
        std::vector<UsedResource> m_UsedResources;

        // Track current dynamic rendering color formats (for validation)
        std::vector<VkFormat> m_CurrentRenderingColorFormats;
        uint32_t m_CurrentColorTargetCount = 0;

        // Cache bound descriptor sets so we can safely rebind after pipeline switches
        std::array<VkDescriptorSet, 8> m_BoundDescriptorSets{}; // sets 0..7
        std::array<PipelineHandle, 8> m_BoundSetPipelines{};    // pipeline used when binding each set
        // Descriptor buffers bound to this command buffer, in bind order. Usually
        // one (the pool's single backing buffer, shared by the persistent and every
        // per-frame region), so a single bind per command buffer. The pool spills to
        // a SEPARATE VkBuffer when one of those regions overflows;
        // a draw whose sets span the primary buffer and a spill block must reference
        // each set's own buffer index (vkCmdSetDescriptorBufferOffsetsEXT). This list
        // is append-only within a command buffer so an earlier set's already-recorded
        // buffer index (and thus its offset) stays valid after a later set forces a
        // re-bind. Cleared in Begin() and on a legacy-pool bind (which invalidates the
        // descriptor-buffer bindings).
        struct BoundDescriptorBuffer
        {
            VkBuffer Buffer = VK_NULL_HANDLE;
            VkDeviceAddress BlockBaseAddress = 0;
        };
        std::vector<BoundDescriptorBuffer> m_BoundDescriptorBuffers;
        uint32_t m_BoundDescriptorSetCount = 0; // contiguous from set 0
        // Track last pipeline layouts per bind point to avoid unnecessary descriptor invalidations
        VkPipelineLayout m_LastGraphicsLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_LastComputeLayout = VK_NULL_HANDLE;


        // Track current pipeline for typed push constants
        PipelineHandle m_CurrentPipelineHandle = INVALID_HANDLE;

        // Track current attachment samples for debug validation
        TextureHandle m_CurrentColorAttachment = INVALID_HANDLE;
        VkSampleCountFlagBits m_CurrentColorSamples = VK_SAMPLE_COUNT_1_BIT;

        // Render pass present tracking
        bool m_LastRenderPassWasPresent = false;
        VkImage m_LastRenderPassImage = VK_NULL_HANDLE;

        // The dedicated legacy swapchain pass implicitly ends in PRESENT_SRC_KHR.
        // Both layout journals must reflect that transition before the next barrier.
        TextureHandle m_LegacySwapchainAttachment = INVALID_HANDLE;

        // One-off legacy framebuffer/render-pass objects. Accumulated per-pass
        // and destroyed in Begin() on the next use, when the previous frame's
        // fence guarantees the command buffer has completed execution.
        std::vector<VkFramebuffer> m_PendingFramebufferDestroys;
        std::vector<VkRenderPass> m_PendingRenderPassDestroys;
        bool m_LegacyOneOffUsesRP2 = false;

        // Views created for attachments during BeginRenderPass, cached by
        // (texture handle, view desc). Attachment views recur every frame (cube
        // faces during sliced probe bakes, array slices for shadow cascades), so
        // they are reused across submits instead of created and deferred-destroyed
        // per render pass. Lookups revalidate against the texture's CURRENT
        // VkImage — generational handle ids alone can't be trusted here because
        // swapchain handles are synthetic (no generation, byte-identical across
        // swapchain recreation) and the 8-bit generation can wrap while a dormant
        // command list holds an entry. Dead entries are swept on Begin and on
        // submit; the destructor releases everything.
        struct AttachmentViewKey
        {
            uint64_t Texture = 0;        // TextureHandle.id (index + generation)
            uint32_t FormatOverride = 0;
            uint32_t Packed = 0;         // aspect | viewType | r/g/b/a swizzles
            uint32_t BaseMip = 0;
            uint32_t LevelCount = 0;
            uint32_t BaseLayer = 0;
            uint32_t LayerCount = 0;
            bool operator==(const AttachmentViewKey&) const = default;
        };
        struct AttachmentViewKeyHasher
        {
            size_t operator()(const AttachmentViewKey& k) const noexcept
            {
                uint64_t h = 1469598103934665603ull;
                auto mix = [&](uint64_t x) { h ^= x; h *= 1099511628211ull; };
                mix(k.Texture);
                mix((static_cast<uint64_t>(k.FormatOverride) << 32) | k.Packed);
                mix((static_cast<uint64_t>(k.BaseMip) << 32) | k.LevelCount);
                mix((static_cast<uint64_t>(k.BaseLayer) << 32) | k.LayerCount);
                return static_cast<size_t>(h);
            }
        };
        struct AttachmentViewEntry
        {
            TextureViewHandle View{};
            VkImage Image = VK_NULL_HANDLE; // image the view was created on (hit revalidation)
        };
        std::unordered_map<AttachmentViewKey, AttachmentViewEntry, AttachmentViewKeyHasher>
            m_AttachmentViewCache;

        TextureViewHandle GetOrCreateAttachmentView(TextureHandle texture, const TextureViewDesc& desc);
        // destroyAll = destructor teardown; otherwise drops only entries whose
        // texture died (view destruction is timeline-deferred, GPU-safe).
        void ReleaseAttachmentViews(bool destroyAll);

        // Last debug marker/pass name (for diagnostics)
        std::string m_LastMarkerName;

        // True iff a compute pipeline is currently bound. Dispatching without one is
        // undefined behaviour in Vulkan, and SetPipeline skips the bind whenever it
        // resolves a dead handle (which a device rebuild leaves behind, since stale
        // handles still read IsValid()). Returns false — after warning once and, in
        // debug, asserting — so the caller can refuse the dispatch in every config
        // instead of Release silently executing one with no pipeline bound.
        bool RequireComputePipelineBound(const char* op);


        // Resolves the texture's format to pick an aspect mask, so it needs the
        // device. The pure ResourceState translations live in VulkanMappings.h.
        VkImageAspectFlags GetVkImageAspectFlags(TextureHandle textureHandle);

        // Verify a pipeline's recorded color attachment formats match the
        // currently-begun dynamic-rendering scope. Logs full diagnostics on
        // mismatch and asserts in debug builds. No-op in release. Called from
        // every site that binds a pipeline or issues a draw inside a render
        // pass so the failure point pinpoints the misuse.
        void ValidatePipelineMatchesActiveScope(PipelineHandle pipeline);

        // Record bound descriptor sets at draw time when capture is enabled
        void MaybeCaptureDescriptorState();
    };


} // namespace Rendering
} // namespace GameEngine
