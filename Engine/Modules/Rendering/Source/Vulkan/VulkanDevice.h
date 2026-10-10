/**
 * @file VulkanDevice.h
 * @brief Vulkan implementation of the IDevice interface
 */

#pragma once

#include "Rendering/Core/BufferManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/IPipelineCache.h"
#include "Rendering/Core/PipelineManager.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/QueryPool.h"
#include "Rendering/Core/ResourceManager.h"
#include "Rendering/Core/TextureManager.h"
#include "VulkanPipelineCache.h"

#include "DescriptorSetAllocator.h"
#include "DeviceHealthState.h"
#include "PendingWorkerDeviceLoss.h"
#include "VulkanDebugUtilsLabels.h"
#include "VulkanDescriptorBufferPool.h"
#include "VulkanFaultInjection.h"
#include "VulkanGpuCheckpoints.h"
#include "FenceRegistration.h"
#include "Rendering/Materials/ReflectionCache.h"
#include "VulkanQueryPool.h"
#include "VulkanQueueRetireTracking.h"
#include "VulkanQueueSubmitContext.h"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _MSC_VER
#pragma warning(push)
// VMA is a 3rd-party header; MSVC /analyze can produce noisy (often false-positive)
// warnings inside it. Suppress them locally without mutating vcpkg sources.
#pragma warning(disable : 6326 6386 6387)
#endif
#include <vk_mem_alloc.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <vulkan/vulkan.h>

#include <cassert>

#ifndef GE_ENABLE_DEBUG_BARRIERS
#define GE_ENABLE_DEBUG_BARRIERS 0
#endif

#if RENDERING_ENABLE_SPIRV_REFLECTION
#include "Rendering/Materials/ShaderMeta.h"
#endif

namespace GameEngine
{
namespace Rendering
{

// GE_VK_WATCH_IMAGE diagnostic (defined in VulkanDevice.cpp): logs layout transitions
// recorded on the watched image so barrier placement can be attributed. No-op unless
// the env var names a texture.
void WatchImageBarrierIfWatched(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                const VkImageSubresourceRange& range, const char* site);

/**
 * @brief Vulkan buffer resource
 */
struct VulkanBuffer
{
    VkDevice device = VK_NULL_HANDLE; // Track device for proper cleanup
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    BufferUsage usage = BufferUsage::None;
    // Properties the requested BufferMemoryUsage asks for. Drives the
    // map-directly-vs-stage choice in MapBuffer / UpdateBuffer, and is the
    // search key for the non-VMA allocation path.
    VkMemoryPropertyFlags memoryProperties = 0;
    // Properties of the memory type VMA actually chose. The requested class is
    // a hint — VMA resolves the type from usage plus host-access flags — so
    // this is the only sound answer to "where did these bytes land".
    VkMemoryPropertyFlags resolvedMemoryProperties = 0;
    VmaAllocation allocation = nullptr;
    void* persistentMappedData = nullptr; // For persistently mapped VMA buffers
    // As requested at creation. Only FrameSlotted is read back after the fact,
    // by the developer-build guard on host writes.
    BufferCreateFlags createFlags = BufferCreateFlags::None;
    std::string debugName;
};

/**
 * @brief Vulkan texture resource
 */
struct VulkanTexture
{
    VkDevice device = VK_NULL_HANDLE; // Track device for proper cleanup
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE; // For fallback when VMA not available
    VmaAllocation allocation = nullptr;     // VMA allocation for proper memory management
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    TextureUsage usage = TextureUsage::None;
    uint32_t sampleCount = 1; // Track sample count for MSAA resolve validation
    std::string debugName;
    // Flat cross-frame layout seed for NON-graph surfaces (whole-image barrier
    // updates only; per-layer state is untracked by design). Graph-owned
    // barriers never read it — the render graph's compiled stateBefore is the
    // per-subresource authority. Swapchain images are tracked separately
    // (m_SwapchainImageLayouts).
    VkImageLayout trackedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Image array/mip dimensions needed to determine whether a barrier covers the whole image.
    uint32_t arrayLayers = 1;
    uint32_t mipLevels = 1;
    // GENERAL-resident sampled texture (TextureDesc::sampledInGeneralLayout): its
    // sampled / combined-image-sampler descriptors claim VK_IMAGE_LAYOUT_GENERAL,
    // not SHADER_READ_ONLY, because it lives in GENERAL for storage co-use.
    bool sampledInGeneralLayout = false;
    // Number of live views referencing this texture (debug/lifetime hygiene).
    // Mutated only under VulkanDevice::m_TextureViewMutex, held exclusively.
    int viewRefCount = 0;
};

// Canonical push-constant range stages for classic (VS/FS) graphics pipelines.
// Every classic graphics pipeline layout declares the SAME single range —
// these stages, [0, policy max) — regardless of what the shaders reflect
// (Vulkan permits a range to cover stages that never read it). Identical
// ranges make any two graphics pipeline layouts that agree on descriptor set
// layouts dedup to the SAME VkPipelineLayout, which keeps descriptor bindings
// that survive a pipeline switch (MaterialBinder sticky binds) valid under the
// pipeline-layout-compatibility rules: layouts differing only in push-range
// stage flags are NOT compatible (VUID-vkCmdDrawIndexedIndirectCount-None-08600).
// vkCmdPushConstants stage flags must exactly match the ranges covering the
// written bytes, so the same constant feeds layout creation, the pipeline's
// push metadata, and the command-list push paths. Mesh (task/mesh) and compute
// pipelines keep reflected stages — their layouts never participate in the
// classic sticky-bind domain.
inline constexpr VkShaderStageFlags kGraphicsPushConstantStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

/**
 * @brief Vulkan pipeline resource
 */
struct VulkanPipeline
{
    VkDevice device = VK_NULL_HANDLE; // Track device for proper cleanup
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    PipelineType type = PipelineType::Graphics;
    std::string debugName;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE; // Store descriptor set layout for compute pipelines
    // Debug: store rasterization sample count used when creating this pipeline
    uint32_t rasterizationSamples = 1;
    // For dynamic rendering: expected color attachment formats at creation (empty if not specified)
    std::vector<VkFormat> colorAttachmentFormats;
    // Push constant layout for this pipeline (for typed push constants)
    uint32_t pushConstantSize = 0;       // 0 means none declared
    uint32_t pushConstantStagesMask = 0; // backend numeric mask
    // Stage flags of the VkPipelineLayout's single push-constant range as it
    // was actually CREATED (0 = the layout declares no range). vkCmdPushConstants
    // must use exactly these flags (VUID-vkCmdPushConstants-offset-01795/01796);
    // VulkanCommandList::ResolvePushConstantStageFlags asserts against it.
    uint32_t layoutPushStageFlags = 0;
    struct PushRange
    {
        std::string name;
        uint32_t offset = 0;
        uint32_t size = 0;
        uint32_t stagesMask = 0;
    };
    std::vector<PushRange> pushRanges; // preserved order
    // Cached from PipelineDesc so the command list can set the dynamic depth
    // write enable state per-pipeline (VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE).
    bool depthWriteEnable = true;
};

/**
 * @brief Vulkan descriptor set resource
 */
struct VulkanDescriptorSet
{
    VkDevice device = VK_NULL_HANDLE; // Track device for proper cleanup
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;        // Track which pool this came from
    VkDescriptorSetLayout layout = VK_NULL_HANDLE; // Track the layout used
    std::string debugName;
};

class VulkanAccelerationStructures;
class VulkanCommandList;

/**
 * @brief Vulkan implementation of the device interface
 */
class VulkanDevice : public IDevice
{
    friend class VulkanCommandList; // destructor needs RecycleCmdBuffer

  public:
    // Pipeline creation result structure
    struct PipelineCreationResult
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE; // For compute pipelines
    };

    VulkanDevice();
    ~VulkanDevice() override;

    // IDevice interface
    bool Initialize(const DeviceDesc& desc) override;
    void Shutdown() override;

    GraphicsAPI GetAPI() const override { return GraphicsAPI::Vulkan; }
    const RenderingDeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
    DeviceHealth GetDeviceHealth() const override { return m_Health.Load(); }
    uint64_t GetDeviceRebuildGeneration() const override
    {
        return m_DeviceRebuildGeneration.load(std::memory_order_relaxed);
    }
    // Thread-command-pool generation (bumped by DestroyAllThreadPools on a rebuild).
    // A VulkanCommandList stamps this when it acquires a command buffer + owner, and
    // compares it before recycling to a dead pool owner after an in-place rebuild.
    uint32_t GetThreadPoolGeneration() const { return m_DeviceGeneration.load(std::memory_order_relaxed); }
    bool RebuildDevice() override;
    void TickDeviceRecovery() override;
    void NotifyReprovisionComplete() override;
    std::string GetHardwareDescription() const override
    {
        const double vramMB = static_cast<double>(m_Capabilities.dedicatedVideoMemory) / (1024.0 * 1024.0);
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "%s  Vulkan %u.%u.%u  driver %u.%u.%u  VRAM %.0f MB",
                      m_DeviceProperties.deviceName,
                      VK_VERSION_MAJOR(m_DeviceProperties.apiVersion),
                      VK_VERSION_MINOR(m_DeviceProperties.apiVersion),
                      VK_VERSION_PATCH(m_DeviceProperties.apiVersion),
                      VK_VERSION_MAJOR(m_DeviceProperties.driverVersion),
                      VK_VERSION_MINOR(m_DeviceProperties.driverVersion),
                      VK_VERSION_PATCH(m_DeviceProperties.driverVersion),
                      vramMB);
        return std::string(buf);
    }
    void PrintCapabilityReport() const override
    {
        printf("=== Device Capability Report (Vulkan) ===\n");
        printf("Vulkan API version: %u.%u.%u\n",
               VK_VERSION_MAJOR(m_DeviceProperties.apiVersion),
               VK_VERSION_MINOR(m_DeviceProperties.apiVersion),
               VK_VERSION_PATCH(m_DeviceProperties.apiVersion));
        printf("Synchronization2: %s\n", m_SupportsSync2 ? "Yes" : "No");
        auto printModes = [](uint32_t flags)
        {
            printf("%s%s%s%s",
                   (flags & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) ? " SampleZero" : "",
                   (flags & VK_RESOLVE_MODE_AVERAGE_BIT) ? " Average" : "",
                   (flags & VK_RESOLVE_MODE_MIN_BIT) ? " Min" : "",
                   (flags & VK_RESOLVE_MODE_MAX_BIT) ? " Max" : "");
        };
        printf("Depth resolve modes:");
        printModes(m_Capabilities.supportedDepthResolveModes);
        printf("\n");
        printf("Stencil resolve modes:");
        printModes(m_Capabilities.supportedStencilResolveModes);
        printf("\n");
        printf("Independent stencil resolve: %s\n", m_Capabilities.supportsIndependentStencilResolve ? "Yes" : "No");
        printf("Dynamic rendering enabled: %s\n", m_EnableDynamicRendering ? "Yes" : "No");
        printf("Bindless resources: %s (max textures: %u)\n", m_Capabilities.supportsBindlessResources ? "Yes" : "No", m_Capabilities.maxBindlessTextures);
        printf("-- Descriptor buffer (VK_EXT_descriptor_buffer) --\n");
        printf("  extension available: %s\n", m_DescBufferDiag.extensionAvailable ? "Yes" : "No");
        if (m_DescBufferDiag.extensionAvailable)
        {
            printf("  descriptorBuffer: %s\n", m_DescBufferDiag.descriptorBuffer ? "Yes" : "No");
            printf("  descriptorBufferCaptureReplay: %s\n", m_DescBufferDiag.descriptorBufferCaptureReplay ? "Yes" : "No");
            printf("  descriptorBufferImageLayoutIgnored: %s\n", m_DescBufferDiag.descriptorBufferImageLayoutIgnored ? "Yes" : "No");
            printf("  descriptorBufferPushDescriptors: %s\n", m_DescBufferDiag.descriptorBufferPushDescriptors ? "Yes" : "No");
            printf("  offsetAlignment: %llu bytes\n", static_cast<unsigned long long>(m_DescBufferDiag.descriptorBufferOffsetAlignment));
            printf("  descriptor sizes (bytes): sampler=%u CIS=%u sampledImg=%u storageImg=%u UBO=%u SBO=%u inputAtt=%u\n",
                   m_DescBufferDiag.samplerDescriptorSize,
                   m_DescBufferDiag.combinedImageSamplerDescriptorSize,
                   m_DescBufferDiag.sampledImageDescriptorSize,
                   m_DescBufferDiag.storageImageDescriptorSize,
                   m_DescBufferDiag.uniformBufferDescriptorSize,
                   m_DescBufferDiag.storageBufferDescriptorSize,
                   m_DescBufferDiag.inputAttachmentDescriptorSize);
            printf("  combinedImageSamplerDescriptorSingleArray: %s\n", m_DescBufferDiag.combinedImageSamplerDescriptorSingleArray ? "Yes" : "No");
            printf("  bufferlessPushDescriptors: %s\n", m_DescBufferDiag.bufferlessPushDescriptors ? "Yes" : "No");
            printf("  maxResourceDescriptorBufferRange: %llu\n", static_cast<unsigned long long>(m_DescBufferDiag.maxResourceDescriptorBufferRange));
            printf("  maxSamplerDescriptorBufferRange: %llu\n", static_cast<unsigned long long>(m_DescBufferDiag.maxSamplerDescriptorBufferRange));
        }
        printf("  maxPushDescriptors (VK_KHR_push_descriptor): %u\n", m_DescBufferDiag.maxPushDescriptors);
        printf("========================================\n");
    }
    uint32_t GetGraphicsQueueFamilyIndex() const override { return m_GraphicsQueueFamily; }
    uint32_t GetComputeQueueFamilyIndex() const override
    {
        return (m_ComputeQueueFamily != kInvalidQueueFamilyIndex) ? m_ComputeQueueFamily : m_GraphicsQueueFamily;
    }
    uint32_t GetTransferQueueFamilyIndex() const override
    {
        return (m_TransferQueueFamily != kInvalidQueueFamilyIndex) ? m_TransferQueueFamily : m_GraphicsQueueFamily;
    }

    DescriptorAllocatorStats GetDescriptorAllocatorStats() const override;
    bool GetPipelinePushConstantInfo(PipelineHandle pipeline, PipelinePushConstantInfo& outInfo) const override;

    // Optional lightweight per-frame bind counters
    DebugBindCounters DebugGetBindCounters() const override;
    void DebugResetBindCounters() override;

    // Validation-layer telemetry (snapshot of the process-wide ValidationStatsStore)
    ValidationStats GetValidationStats() const override;
    void ResetValidationStats() override;

    // Live debug instrumentation on this device plus whatever tools are attached
    // to it. Queries the runtime on every call, so a tool that attaches after
    // startup shows up.
    GpuToolingReport GetGpuToolingReport() const override;

    // Names a Vulkan object for the validation layer / RenderDoc
    // (vkSetDebugUtilsObjectNameEXT). No-op when VK_EXT_debug_utils is absent.
    void SetVkObjectName(VkObjectType objectType, uint64_t objectHandle, const char* name) const;

    // Per-pass descriptor capture overrides
    void DebugSetDescriptorCaptureEnabled(bool enabled, const std::string& passFilter) override;
    bool DebugIsDescriptorCaptureEnabled() const override;
    bool DebugFillsNewResourcesWithNaN() const override { return m_FillNewTargetsWithNaN; }
    void DebugClearDescriptorCaptures() override;
    std::vector<DebugDescriptorCapture> DebugGetDescriptorCaptures() const override;

    // Parameter-based resource creation (preferred)
    BufferHandle CreateBuffer(const BufferDesc& desc) override;
    TextureHandle CreateTexture(const TextureDesc& desc) override;
    SamplerHandle CreateSampler(const SamplerDesc& desc) override;
    PipelineHandle CreateConcreteGraphicsPipeline(
        const GraphicsPipelineDesc& gd, const PipelineFormatKey& fk) override;
    PipelineHandle CreateConcreteComputePipeline(
        const ComputePipelineDesc& cd) override;

    // Multi-queue command lists and submission
    std::unique_ptr<CommandList> CreateCommandList(QueueType queue) override;
    std::unique_ptr<CommandList> CreateSecondaryCommandList(QueueType queue) override;
    void RetireSecondaryCommandLists(CommandList* const* lists, uint32_t count) override;
    bool QueueSubmit(QueueType queue,
                     const std::vector<CommandList*>& cmdLists,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& waitSemaphores,
                     const std::vector<std::pair<SemaphoreHandle, uint64_t>>& signalSemaphores) override;
    GpuSyncToken SubmitTextureUploads(const TextureUploadRequest* requests, uint32_t count) override;
    SemaphoreHandle CreateTimelineSemaphore(uint64_t initialValue = 0) override;
    void DestroySemaphore(SemaphoreHandle) override;
    bool GetTimelineSemaphoreValue(SemaphoreHandle sem, uint64_t& outValue) const override;
    bool WaitTimelineSemaphoreValue(SemaphoreHandle sem, uint64_t value, uint64_t timeoutNs = ~0ull) override;
    GpuSyncToken LastGraphicsSubmissionToken() const override;
    bool IsPreviousFrameGraphicsComplete() const override;

    // Dynamic rendering support

    // Introspection for push constant ranges
    uint32_t GetPipelinePushConstantRangeCount(PipelineHandle pipeline) const override;
    bool GetPipelinePushConstantRangeInfo(PipelineHandle pipeline, uint32_t id, PushConstantRangeInfo& outInfo) const override;
    bool FindPipelinePushConstantRangeId(PipelineHandle pipeline, const char* name, uint32_t& outId) const override;

    // Config
    bool IsDynamicRenderingEnabled() const { return m_EnableDynamicRendering; }
    void SetDynamicRenderingEnabled(bool enabled) { m_EnableDynamicRendering = enabled; }

    bool SupportsDynamicRendering() const { return m_SupportsDynamicRendering; }
    // VK_ATTACHMENT_STORE_OP_NONE ("preserve the contents, perform no write"):
    // Vulkan 1.3 core, the same version that introduced vkCmdSetDepthWriteEnable.
    // Capability only, and not on its own a licence to record NONE: it preserves
    // the contents only for as long as nothing writes the attachment during the
    // render pass, and the caller is what must establish that (see
    // VulkanCommandList::ResolveStoreOp — a legacy render pass suppresses no
    // depth write, whatever the device version).
    bool SupportsAttachmentStoreOpNone() const
    {
        return m_DeviceProperties.apiVersion >= VK_API_VERSION_1_3;
    }
    // Synchronization2 capability: Vulkan 1.3 core or VK_KHR_synchronization2 on 1.2
    bool SupportsSynchronization2() const { return m_SupportsSync2; }
    bool SupportsDrawIndirectCount() const { return m_SupportsDrawIndirectCount; }
    bool SupportsMultiDrawIndirect() const { return m_SupportsMultiDrawIndirect; }
    // sync2 helpers
    void CmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* dep);

    // Expose function pointers for command list
    PFN_vkCmdPipelineBarrier2 GetCmdPipelineBarrier2() const { return m_FpCmdPipelineBarrier2; }
    PFN_vkCmdBeginRendering GetCmdBeginRendering() const { return m_FpCmdBeginRendering; }
    PFN_vkCmdEndRendering GetCmdEndRendering() const { return m_FpCmdEndRendering; }
    PFN_vkCmdSetDepthWriteEnable GetCmdSetDepthWriteEnable() const { return m_FpCmdSetDepthWriteEnable; }
    PFN_vkCmdDrawMeshTasksEXT GetCmdDrawMeshTasksEXT() const { return m_FpCmdDrawMeshTasksEXT; }
    bool SupportsMeshShadersEXT() const { return m_SupportsMeshShaderEXT; }
    bool SupportsNegativeViewportHeight() const { return m_SupportsNegativeViewportHeight; }
#if GE_ENABLE_DEBUG_BARRIERS
    // Debug capture of last emitted barriers (for tests)
    struct DebugBarrierInfo
    {
        bool isImage;
        VkImageMemoryBarrier2 image2{};
        VkBufferMemoryBarrier2 buffer2{};

        VkImageMemoryBarrier image{};
        VkBufferMemoryBarrier buffer{};
    };
    void ClearDebugBarriers();
    const std::vector<DebugBarrierInfo>& GetDebugBarriers() const { return m_DebugBarriers; }
    size_t GetDebugImageBarrierCount() const
    {
        size_t n = 0;
        for (const auto& b : m_DebugBarriers)
            if (b.isImage)
                ++n;
        return n;
    }
    size_t GetDebugBufferBarrierCount() const
    {
        size_t n = 0;
        for (const auto& b : m_DebugBarriers)
            if (!b.isImage)
                ++n;
        return n;
    }
    void PushDebugBarrier(const DebugBarrierInfo& info)
    {
        // Debug-only verification: ensure we never emit queue ownership transfers
#ifdef _DEBUG
        if (info.isImage)
        {
            if (info.image2.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2)
            {
                assert(info.image2.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
                       info.image2.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
            }
            else if (info.image.sType == VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER)
            {
                assert(info.image.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
                       info.image.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
            }
        }
        else
        { // buffer
            if (info.buffer2.sType == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2)
            {
                assert(info.buffer2.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
                       info.buffer2.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
            }
            else if (info.buffer.sType == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER)
            {
                assert(info.buffer.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED &&
                       info.buffer.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
            }
        }
#endif
        m_DebugBarriers.push_back(info);
        if (m_DebugBarriers.size() > m_DebugBarriersHighWater)
            m_DebugBarriersHighWater = m_DebugBarriers.size();
    }
#else
    struct DebugBarrierInfo
    {
    };
    void ClearDebugBarriers() {}
    const std::vector<DebugBarrierInfo>& GetDebugBarriers() const
    {
        static const std::vector<DebugBarrierInfo> kEmpty;
        return kEmpty;
    }
    size_t GetDebugImageBarrierCount() const { return 0; }
    size_t GetDebugBufferBarrierCount() const { return 0; }
    void PushDebugBarrier(const DebugBarrierInfo&) {}
#endif
    // Count of vkCmdPipelineBarrier2 calls issued this frame (reset in BeginFrame), in
    // every configuration; the per-barrier capture above is Debug only.
    void DebugIncPipelineBarrier2Calls() { ++m_DebugPipelineBarrier2Calls; }
    uint32_t GetDebugPipelineBarrier2Calls() const { return m_DebugPipelineBarrier2Calls; }

    // Internal helper to get unique queue family indices for sharing
    void GetUniqueQueueFamilies(uint32_t* outIndices, uint32_t& outCount) const
    {
        uint32_t fam[3] = {m_GraphicsQueueFamily, m_ComputeQueueFamily, m_TransferQueueFamily};
        outCount = 0;
        for (uint32_t i = 0; i < 3; ++i)
        {
            uint32_t f = fam[i];
            if (f == kInvalidQueueFamilyIndex)
                continue;
            bool seen = false;
            for (uint32_t j = 0; j < outCount; ++j)
            {
                if (outIndices[j] == f)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
                outIndices[outCount++] = f;
        }
    }

    PFN_vkQueueSubmit2 GetQueueSubmit2() const { return m_FpQueueSubmit2; }

    // Resource destruction
    void DestroyBuffer(BufferHandle handle) override;
    void DestroyTexture(TextureHandle handle) override;
    void DestroySampler(SamplerHandle handle) override;
    void DestroyPipeline(PipelineHandle handle) override;

    // Resource access
    void* MapBuffer(BufferHandle handle) override;
    void UnmapBuffer(BufferHandle handle) override;
    void UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) override;
    void UpdateBufferRanges(BufferHandle handle, std::span<const BufferUpdateRange> ranges) override;
    uint64_t GetBufferDeviceAddress(BufferHandle handle) override;
    IAccelerationStructureBackend* GetAccelerationStructureBackend() override;

    // Command recording
    void ExecuteCommandLists(const std::vector<CommandList*>& commandLists) override;

    // What each queue's submit signalled, so a caller can defer its own resources against its
    // own submit. An invalid token means that queue carried no work, or its submit failed —
    // either way nothing may be retired against it. The timeline in a token is the one the work
    // actually ran on, which is the graphics timeline for a role with no dedicated queue.
    struct SubmittedBatchTokens
    {
        GpuSyncToken graphics;
        GpuSyncToken compute;
        GpuSyncToken transfer;
    };

    // ExecuteCommandLists, reporting what it signalled. Callers that defer resources against a
    // submit MUST use this and the token it returns: re-reading a queue's counter afterwards
    // yields whatever value the latest submit on that queue reached, which another thread may
    // already have advanced past this caller's own.
    SubmittedBatchTokens ExecuteCommandListsTracked(const std::vector<CommandList*>& commandLists);

    // Synchronization
    void WaitForIdle() override;
    uint64_t GetIdleDrainCount() const override { return m_IdleDrainCount; }

    // Frame management
    bool BeginFrame() override;

    // Device info helpers
    uint32_t GetMaxPushConstantsSize() const { return m_DeviceProperties.limits.maxPushConstantsSize; }

    void Present() override;
    void FinalizeFrame() override;

    // Frames-in-flight introspection
    uint32_t GetFramesInFlight() const override { return static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT); }
    uint32_t GetFrameIndex() const override { return m_CurrentFrame.load(std::memory_order_relaxed); }
    uint32_t GetFrameIndexForWindowTarget(WindowTargetHandle target) const override;
    uint32_t GetActiveWindowTargetFrameIndex() const override;
    TextureFormat GetWindowTargetSwapchainFormat(WindowTargetHandle target) const override;

    // New: user-extensible per-frame waits for Present
    void RegisterEndOfFrameWait(const IDevice::SemaphoreWait& wait) override;
    void ClearEndOfFrameWaits() override;
    void Present(const std::vector<IDevice::SemaphoreWait>& extraWaits) override;

    // Window presentation abstraction methods for backend compatibility
    WindowTargetHandle CreateWindowTarget(void* windowHandle, uint32_t width, uint32_t height) override;
    bool DestroyWindowTarget(WindowTargetHandle target) override;
    bool DrainWindowTarget(WindowTargetHandle target, uint64_t timeoutNs = ~0ull) override;
    bool SetActiveWindowTarget(WindowTargetHandle target) override;
    WindowTargetHandle GetActiveWindowTarget() const override;
    bool RecreateWindowTargetSwapchain(WindowTargetHandle target, uint32_t width, uint32_t height) override;
    bool GetWindowTargetSize(WindowTargetHandle target, uint32_t& outWidth, uint32_t& outHeight) const override;
    void SetVsync(bool vsync) override;
    bool IsVsyncEnabled() const override { return m_Vsync; }
    TextureHandle GetSwapchainImage(uint32_t index) const override;
    bool GetSwapchainSize(uint32_t& outWidth, uint32_t& outHeight) const override;
    bool IsTextureHandleLive(TextureHandle texture) const override;

    // Texture views
    TextureViewHandle CreateTextureView(TextureHandle texture, const TextureViewDesc& desc) override;
    void DestroyTextureView(TextureViewHandle) override;

    // Note: AcquireNextImage and PresentImage are declared below as Vulkan-specific methods
    // Note: GetSwapchainImageCount is implemented inline below
    // They implement the abstract interface with override keyword

    // Resource management
    ResourceManager* GetResourceManager() override;

    // Optional debug and profiling (inline no-op implementations)
    void BeginEvent(const char* /*name*/) override {}
    void EndEvent() override {}
    void SetMarker(const char* /*name*/) override {}

    // Indirect commands moved to CommandList interface

    // Simplified descriptor set management
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) override;
    void DestroyDescriptorSet(DescriptorSetHandle descriptorSet) override;
    void UpdateDescriptorSet(DescriptorSetHandle descriptorSet, const DescriptorSetUpdate& update) override;
    void UpdateDescriptorSetBatch(DescriptorSetHandle descriptorSet, std::span<const DescriptorSetUpdate> updates) override;

    // Vulkan-specific accessors
    VkDevice GetVkDevice() const { return m_Device; }
    VkPhysicalDevice GetVkPhysicalDevice() const { return m_PhysicalDevice; }
    VmaAllocator GetVmaAllocator() const { return reinterpret_cast<VmaAllocator>(m_VmaAllocator); }

    // Property flags of the memory type VMA resolved for this buffer, or 0 for
    // an unknown handle. The requested BufferMemoryUsage is a hint, so this is
    // what residency assertions and diagnostics must read.
    VkMemoryPropertyFlags GetBufferResolvedMemoryProperties(BufferHandle handle) const;

    // Backend-neutral form of the above, plus the heap the allocation landed
    // in. This is the IDevice-level view engine code reads; the raw flags above
    // stay for Vulkan-side assertions and tests.
    BufferMemoryResidency GetBufferMemoryResidency(BufferHandle handle) const override;

    VkInstance GetVkInstance() const { return m_Instance; }

    /// Semaphores the most recently destroyed VkDevice created and never destroyed.
    /// Vulkan requires every child object to be gone before vkDestroyDevice, so a
    /// clean generation reports 0; anything higher is a leaked handle. Only readable
    /// after a device has been destroyed — a rebuild or Shutdown.
    uint64_t GetLastDeviceGenerationSemaphoreResidual() const { return m_LastDeviceGenerationSemaphoreResidual; }

    VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
    VkCommandPool GetCommandPool() const { return m_CommandPool; }

    VkCommandPool GetCommandPool(IDevice::QueueType queue) const
    {
        switch (queue)
        {
        case IDevice::QueueType::Compute:
            return m_ComputeCommandPool ? m_ComputeCommandPool : m_CommandPool;
        case IDevice::QueueType::Transfer:
            return m_TransferCommandPool ? m_TransferCommandPool : m_CommandPool;
        default:
            return m_CommandPool;
        }
    }

    // Surface and swapchain support
    bool CreateSurface(void* windowHandle);
    bool CreateSwapchain(uint32_t width, uint32_t height);
    bool RecreateActiveSwapchain(uint32_t width, uint32_t height);
    void DestroySwapchain();
    bool AcquireNextImage(uint32_t& imageIndex) override;
    bool PresentImage(uint32_t imageIndex) override;

    bool GetLastFrameSyncTimings(IDevice::FrameSyncTimings& out) const override
    {
        out = m_LastFrameSyncTimings;
        return true;
    }
    VkSurfaceKHR GetSurface() const { return m_Surface; }
    VkSwapchainKHR GetSwapchain() const { return m_Swapchain; }

    // Swapchain image access (inline implementation satisfies abstract interface)
    uint32_t GetSwapchainImageCount() const override { return static_cast<uint32_t>(m_SwapchainImages.size()); }

    // Set in CreateSwapchain when the surface advertised VK_IMAGE_USAGE_TRANSFER_SRC_BIT.
    bool SwapchainSupportsReadback() const override { return m_SwapchainSupportsReadback; }

    // Vulkan-specific swapchain access (renamed to avoid conflict with abstract interface)
    VkImage GetVulkanSwapchainImage(uint32_t index) const { return m_SwapchainImages[index]; }
    VkImageView GetSwapchainImageView(uint32_t index) const { return m_SwapchainImageViews[index]; }
    VkFormat GetSwapchainFormat() const { return m_SwapchainImageFormat; }
    VkExtent2D GetSwapchainExtent() const { return m_SwapchainExtent; }
    uint32_t GetCurrentSwapchainImageIndex() const { return m_CurrentSwapchainImage; }

    // Abstract interface overrides for format queries
    TextureFormat GetTextureFormat(TextureHandle texture) const override;
    uint32_t GetTextureSampleCount(TextureHandle texture) const override;
    void GetTextureSize(TextureHandle texture, uint32_t& outWidth,
                        uint32_t& outHeight) const override;
    uint32_t GetTextureArrayLayers(TextureHandle texture) const override;
    bool GetTextureSampledInGeneralLayout(TextureHandle texture) const override;
    bool IsTextureAlive(TextureHandle texture) const override;
    bool IsPipelineAlive(PipelineHandle pipeline) const override;
    TextureFormat GetSwapchainTextureFormat() const override;
    HdrOutputState GetHdrOutputState() const override { return m_HdrState; }
    HdrOutputMode GetActiveHdrOutputMode() const override { return m_HdrState.activeMode; }
    bool SetHdrOutputMode(HdrOutputMode mode, const HdrStaticMetadata* metadata = nullptr,
                          HdrSwapchainBitDepth bitDepth = HdrSwapchainBitDepth::Bit10) override;
    bool IsTextureFormatSupported(TextureFormat format, uint32_t usageFlags) const override;

    // Debug/testing helpers for VMA allocation tracking (headless tests)
    size_t DebugGetAllocationCount() const override;
    size_t DebugGetBufferRegistryCount() const override;
    size_t DebugGetImageRegistryCount() const override;
    size_t DebugGetAllocatedBytes() const override;
    size_t DebugGetTextureBytes() const override;
    size_t DebugGetBufferBytes() const override;
    void DebugEnumerateResources(const std::function<void(const DebugResourceInfo&)>& fn) const override;
    size_t DebugGetRetiredWindowTargetSemaphoreCount() const { return m_RetiredWindowTargetSemaphores.size(); }
    size_t DebugGetPendingWindowTargetRetirementCount() const { return m_PendingWindowTargetRetirements.size(); }
    uint64_t DebugGetWindowTargetRetirementForcedIdleCount() const { return m_WindowTargetRetirementForcedIdleCount; }
    double DebugGetWindowTargetRetirementForcedIdleTotalMs() const { return m_WindowTargetRetirementForcedIdleTotalMs; }
    double DebugGetWindowTargetRetirementForcedIdleMaxMs() const { return m_WindowTargetRetirementForcedIdleMaxMs; }

    // Debug/testing: retirement counters (last BeginFrame)
    size_t DebugGetLastRetiredTextureCount() const override { return m_LastRetiredTexturesCount; }
    size_t DebugGetLastRetiredBufferCount() const override { return m_LastRetiredBuffersCount; }

    ResourcePoolStats GetResourcePoolStats() const override;

    // Preferences
    void SetPreferTenBitSwapchain(bool prefer) { m_PreferTenBitSwapchain = prefer; }
    bool GetPreferTenBitSwapchain() const { return m_PreferTenBitSwapchain; }
    // Prefer an 8-bit UNORM SDR swapchain over an 8-bit _SRGB one, so the
    // terminal encode pass exists on every SDR frame (#767 P6a). OFF on every
    // host; see ChooseSwapSurfaceFormat for the macOS default decision and its
    // cost gate.
    void SetPreferUnormSdrSwapchain(bool prefer) { m_PreferUnormSdrSwapchain = prefer; }
    bool GetPreferUnormSdrSwapchain() const { return m_PreferUnormSdrSwapchain; }

    // Ensure current swapchain image is ready for use as COLOR_ATTACHMENT in DR (first-use safety)
    void EnsureSwapchainColorAttachmentReady(VkCommandBuffer cmd);
    // Record present-time transition for the current swapchain image
    void RecordPresentTransition(VkCommandBuffer cmd, VkImage image);

    // Markers for per-frame backbuffer rendering to select correct present waits
    void NotifyBackbufferRenderedThisFrame() { m_DidBackbufferRenderThisFrame = true; }

    // Get TextureHandle for current swapchain image (for render graph integration)
    TextureHandle GetCurrentSwapchainImageHandle() override;

    // Pipeline and buffer access for command lists
    VkPipeline GetVkPipeline(PipelineHandle handle);
    VkPipelineLayout GetVkPipelineLayout(PipelineHandle handle);
    PipelineType GetPipelineType(PipelineHandle handle) const;

    // Expose current pipeline handle for typed push constants (tracked by CommandList)
    PipelineHandle GetCurrentPipelineHandle() const { return m_CurrentPipeline; }
    void SetCurrentPipelineHandle(PipelineHandle h) { m_CurrentPipeline = h; }

    VkBuffer GetVkBuffer(BufferHandle handle);
    VkImage GetVkImage(TextureHandle handle);
    VkFormat GetVkImageFormat(TextureHandle handle);
    VkImageView GetVkImageView(TextureHandle handle);
    VkExtent3D GetVkImageExtent(TextureHandle handle);
    VkSampleCountFlagBits GetVkImageSamples(TextureHandle handle);

    // Image view accessor for explicit TextureViewHandle
    VkImageView GetVkImageView(TextureViewHandle handle);

    // Cached render pass/framebuffer creation for arbitrary attachments
    VkRenderPass GetOrCreateRenderPass(
        uint32_t colorCount,
        const VkFormat* colorFormats,
        const VkAttachmentLoadOp* colorLoadOps,
        const VkAttachmentStoreOp* colorStoreOps,
        bool hasDepth,
        VkFormat depthFormat,
        VkAttachmentLoadOp depthLoadOp,
        VkAttachmentStoreOp depthStoreOp,
        VkAttachmentLoadOp stencilLoadOp,
        VkAttachmentStoreOp stencilStoreOp,
        bool colorFinalPresent,
        uint32_t samples = 1);

    bool IsSwapchainTextureHandle(TextureHandle handle) const;
    // Tracks the last known VkImageLayout for swapchain images across frames.
    // This is needed because we explicitly transition swapchain images to PRESENT
    // in VulkanDevice::Present(), so the next frame must transition from PRESENT
    // back to COLOR_ATTACHMENT_OPTIMAL (dynamic rendering path).
    VkImageLayout GetTrackedSwapchainImageLayout(TextureHandle handle) const;
    VkImageLayout GetTrackedImageLayout(TextureHandle handle) const;
    void SetTrackedSwapchainImageLayout(TextureHandle handle, VkImageLayout layout);

    VkFramebuffer GetOrCreateFramebuffer(
        VkRenderPass renderPass,
        uint32_t attachmentCount,
        const VkImageView* attachmentViews,
        uint32_t width,
        uint32_t height);

    VkRenderPass GetOrCreateSwapchainRenderPass();
    VkFramebuffer GetCurrentSwapchainFramebuffer();
    VkPipelineLayout GetPipelineLayout(PipelineHandle handle) const;
    // Test/helper: access first descriptor set layout tracked in pipeline (may be VK_NULL_HANDLE)
    VkDescriptorSetLayout GetFirstDescriptorSetLayoutForPipeline(PipelineHandle handle) const;

    // VMA (Vulkan Memory Allocator) methods
    bool InitializeVMA();
    void ShutdownVMA();

    // Pipeline Cache methods
    VulkanPipelineCache* GetVkDiskPipelineCache() { return m_VkDiskPipelineCache.get(); }
    const VulkanPipelineCache::Statistics& GetVkDiskPipelineCacheStatistics() const;

    // Query Pool methods
    IQueryPool* GetQueryPool() override { return m_QueryPool.get(); }
    const IQueryPool::ProfilingStats& GetProfilingStats() const;

    // Direct memory allocation tracking
    void TrackDirectMemoryAllocation(VkDeviceMemory memory);
    void UntrackDirectMemoryAllocation(VkDeviceMemory memory);
    void CleanupDirectMemoryAllocations();

    void BindDescriptorSet(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout, VkDescriptorSet descriptorSet);
    void BindComputeDescriptorSet(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout, VkDescriptorSet descriptorSet);
    VkPipelineLayout GetPipelineLayout(PipelineHandle pipeline);

    // Improved descriptor set access (use these instead)
    VkDescriptorSet GetVkDescriptorSet(DescriptorSetHandle handle);

    // Access to allocator (for tests/integration, optional)
    DescriptorSetAllocator& GetDescriptorSetAllocator() { return m_DsAllocator; }

    // Tracked for typed push constants (current bound pipeline)
    PipelineHandle m_CurrentPipeline = INVALID_HANDLE;

  private:
    // Best-effort sync timing for stall diagnosis (written on render thread).
    IDevice::FrameSyncTimings m_LastFrameSyncTimings{};

    // Frame-wide GPU bracket: a single timestamp written at the end of each
    // present-transition command buffer. The delta between consecutive
    // captured values (read back when the slot's fence signals next cycle)
    // is the GPU's wall-clock period per frame, including everything the
    // per-pass profiler can't see (submit overhead, layout transitions,
    // present-transition submit itself, cross-queue timeline waits).
    // Sized for 3 frames-in-flight (must match MAX_FRAMES_IN_FLIGHT below).
    // UINT32_MAX = no query pending for that slot yet.
    uint32_t m_FrameEndQuery[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint64_t m_LastReadFrameEndTimestamp = 0;
    bool     m_HaveReadAnyFrameEndTimestamp = false;
    // Immediate destruction helpers used when GPU completion is confirmed (e.g., retirement path)
    void DestroyBufferImmediate(BufferHandle handle);
    void DestroyTextureImmediate(TextureHandle handle);
    // Arm per-frame fences for compute/transfer queues that had commands submitted this frame.
    void ArmComputeTransferFences();
    // Rotate onto the next frame slot at end of frame. The new slot's fences are
    // unwaited until BeginFrame waits them, so this also drops the "slot fenced"
    // state every host write into a frame-slotted buffer is checked against.
    void AdvanceFrameIndex();
    // Developer-build diagnosis for the host-write entry points: reports a write
    // into a BufferCreateFlags::FrameSlotted buffer while the current slot's
    // fence is unwaited. Compiles to nothing without GE_DEV_DIAG.
    void DiagnoseFrameSlottedHostWrite(const VulkanBuffer& buffer) const;

    // Track owning textures for views to maintain simple view->texture backrefs
    // Last-frame retirement counters
    size_t m_LastRetiredTexturesCount = 0;
    size_t m_LastRetiredBuffersCount = 0;
    // High-water snapshot for resource pool diagnostics.
    IDevice::ResourcePoolStats m_ResourcePoolHighWater{};
    void UpdateResourcePoolHighWater();
    void LogResourcePoolHighWater(const char* reason) const;
    void MaybeLogLiveBufferBreakdown();

    // Track descriptor set -> layout for proper cleanup
    // Track allocated sets to avoid double-free in threaded tests
    std::unordered_set<VkDescriptorSet> m_LiveDescriptorSets;
    std::mutex m_LiveDescriptorSetsMutex;

    std::unordered_map<VkDescriptorSet, VkDescriptorSetLayout> m_DescriptorSetLayouts;
    std::unordered_map<VkDescriptorSet, DescriptorSetLayoutDesc> m_DescriptorSetLayoutDescs; // for validation
    // Validation metadata for transient pool sets, partitioned by frame slot.
    // Transient sets are recycled in bulk by DescriptorSetAllocator::BeginFrameReset(slot)
    // and never individually destroyed, so they must not enter m_DescriptorSetLayoutDescs
    // (those entries would accumulate every frame). Each slot's map is instead purged at
    // the exact point BeginFrame rewinds that slot's pools, mirroring the
    // m_DescriptorBufferSets purge. Only populated when m_EnableDescriptorValidation is
    // set; guarded by m_DescriptorSetLayoutsMutex. Sized by the compile-time upper bound;
    // the runtime frame index never exceeds MAX_FRAMES_IN_FLIGHT.
    std::array<std::unordered_map<VkDescriptorSet, DescriptorSetLayoutDesc>, kMaxSupportedFramesInFlight>
        m_TransientDescriptorSetLayoutDescs;
    std::mutex m_DescriptorSetLayoutsMutex;

    // Fetch the layout metadata recorded for a descriptor set handle (pool-persistent,
    // pool-transient, or descriptor-buffer). Returns false when no metadata exists;
    // callers treat that as "cannot validate" and proceed (legacy/untracked handles).
    bool TryGetDescriptorLayoutDescForValidation(DescriptorSetHandle handle, DescriptorSetLayoutDesc& out);
    // Check one update against the declared layout: binding exists, type matches,
    // arrayElement and write count stay inside the binding's array. Logs + asserts and
    // returns false on violation so release builds turn the write into a validated no-op.
    bool ValidateDescriptorUpdateAgainstLayout(const DescriptorSetLayoutDesc& layoutDesc,
                                               const DescriptorSetUpdate& update) const;

    // DescriptorSetLayout and PipelineLayout caches for deduplication
    struct LayoutCacheEntry
    {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        uint32_t refCount = 0;
        bool descriptorBufferEligible = false; // mirrors DescriptorSetLayoutDesc flag
    };
    struct PipelineLayoutCacheEntry
    {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        uint32_t refCount = 0;
    };
    std::unordered_map<uint64_t, LayoutCacheEntry> m_SetLayoutCache;              // key -> entry
    std::unordered_map<VkDescriptorSetLayout, uint64_t> m_SetLayoutReverse;       // layout -> key
    std::unordered_map<uint64_t, PipelineLayoutCacheEntry> m_PipelineLayoutCache; // key -> entry
    std::unordered_map<VkPipelineLayout, uint64_t> m_PipelineLayoutReverse;       // layout -> key
    std::mutex m_LayoutCacheMutex;

    bool CreateInstance(const DeviceDesc& desc);
    bool SelectPhysicalDevice();
    bool CreateLogicalDevice();
    bool CreateCommandPool();
    bool CreateSyncObjects();
    void QueryDeviceCapabilities();
    // One line at device creation, so the memory arrangement every later
    // residency decision rests on is in the log before anything reads it.
    void LogMemoryTopology() const;
    // One line at device creation naming the vendor, whether the engine's debug
    // instrumentation is live, and which tools are attached. Stated up front
    // because "was that measured through a capture layer, and were labels even
    // callable" cannot be recovered from the run afterwards.
    void LogGpuToolingState() const;
    bool CheckExtensionSupport(const char* extensionName);
    void CleanupVulkan();

    // Device + per-frame + VMA + pipeline-cache bringup shared by Initialize and
    // RebuildDevice (Q6 slice 2). Runs everything from logical-device creation
    // through the disk pipeline cache; the caller must have already established
    // the instance + physical device (both survive a device loss, so RebuildDevice
    // does NOT re-run them — design F8: never re-increment the instance refcount).
    bool CreateDeviceAndResources(const DeviceDesc& desc);

    // --- Q6 Tier-2 in-place rebuild (slice 2) --------------------------------
    // Tear down every device-scoped Vulkan object while KEEPING the shared
    // VkInstance and the per-window VkSurfaceKHR (both instance-scoped). Mirrors
    // the device-scoped portion of Shutdown()/CleanupVulkan() but skips the
    // instance/surface teardown and the refcount decrement, and unconditionally
    // purges the timeline-keyed deferred lists (the device is dead — do not wait
    // on completion). Each surviving window target's surface + extent is captured
    // into `outTargets` so the swapchains can be recreated after bringup.
    struct RetainedWindowTarget
    {
        uint64_t id = 0;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        HdrOutputState hdrState{};
        bool wasActive = false;
    };
    void TeardownDeviceScopedForRebuild(std::vector<RetainedWindowTarget>& outTargets);
    // Recreate a swapchain for each retained window target on the fresh device,
    // reusing the kept surfaces. Returns false if any target failed to come back.
    bool RecreateWindowTargetSwapchainsAfterRebuild(const std::vector<RetainedWindowTarget>& targets);
    // Reset the per-frame index + timeline counters + fence-armed flags to
    // fresh-device semantics (design §5.2: Initialize never touches these; a new
    // device's timeline semaphores start at 0, so EnsureGlobalGpuIdle would wait
    // forever without this). Runs AFTER the deferred purge, BEFORE bringup.
    void ResetFrameAndTimelineCountersForRebuild();

    // Error handling utilities
    bool CheckVkResult(VkResult result, const char* operation) const;
    const char* VkResultToString(VkResult result) const;

    // Surface and swapchain helpers
    bool CheckSurfaceSupport();
    VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height);
    void CreateSwapchainFramebuffers();
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    std::vector<char> ReadShaderFile(const std::string& filename);
    VkShaderModule CreateShaderModule(const std::vector<char>& code);
    VkShaderModule CreateShaderModuleFromBytes(const std::vector<uint8_t>& code);

    PipelineCreationResult CreateVulkanPipelineFromDesc(const PipelineDesc& desc, VkShaderModule vertShader, VkShaderModule fragShader);
    PipelineCreationResult CreateComputePipeline(const std::vector<uint8_t>& computeShaderCode,
                                                 const std::vector<DescriptorSetLayoutDesc>& descriptorSetLayouts);
    PipelineCreationResult CreateComputePipelineFromDesc(const PipelineDesc& desc);

    // Internal Vulkan-side pipeline creation. The public `CreateConcrete*`
    // overrides above translate `(GraphicsPipelineDesc, PipelineFormatKey)`
    // / `ComputePipelineDesc` into this shape and forward.
    PipelineHandle CreatePipelineInternal(const PipelineDesc& desc);

    // Helper methods
    VkDescriptorSetLayout CreateVulkanDescriptorSetLayout(const DescriptorSetLayoutDesc& layoutDesc);

    // Deduplicated layout creation
    // Overload that accepts full push-constant ranges for layout creation and caching
    VkPipelineLayout GetOrCreatePipelineLayoutCached(const std::vector<VkDescriptorSetLayout>& setLayouts,
                                                     const std::vector<VkPushConstantRange>& pushRanges,
                                                     uint64_t& outKey);

    VkDescriptorSetLayout GetOrCreateDescriptorSetLayoutCached(const DescriptorSetLayoutDesc& layoutDesc, uint64_t& outKey);
    VkPipelineLayout GetOrCreatePipelineLayoutCached(const std::vector<VkDescriptorSetLayout>& setLayouts, uint32_t pushConstantSize, uint32_t pushConstantStagesMask, uint64_t& outKey);
    // Release helpers for cached layouts (decrement refcount and destroy if last)
    void ReleaseDescriptorSetLayoutCached(VkDescriptorSetLayout layout);
    void ReleasePipelineLayoutCached(VkPipelineLayout layout);

    // Return true if any of the supplied set layouts was created with
    // VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT (i.e. is DB-eligible).
    // Pipeline creation sites OR VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT into their
    // VkPipelineCreateFlags when this returns true — VUID-08115/08117 require the
    // pipeline flag to match the binding mechanism used at draw time.
    bool AnySetLayoutIsDescriptorBufferEligible(const std::vector<VkDescriptorSetLayout>& setLayouts);

    // Vulkan objects
    VkInstance m_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;

    // Texture view storage (backend-owned views) and the per-view metadata the
    // descriptor paths need. Creation is concurrent by contract: ECS extraction
    // workers reach CreateTextureView through
    // TextureService::RegisterTextureBindless, while the render thread resolves
    // already-created views for every descriptor write. Reads therefore vastly
    // outnumber writes, hence a shared mutex rather than extending the exclusive
    // m_ResourceTrackingMutex over the per-descriptor lookup.
    //
    // m_TextureViewMutex guards ALL FIVE of: m_TextureViews, m_TextureViewAspects,
    // m_TextureViewFormats, m_TextureViewOwners, and VulkanTexture::viewRefCount.
    // The three maps are written and erased together, one entry each per live view.
    // Lock ordering: m_DeferredDestroyMutex < m_TextureViewMutex < m_ResourceTrackingMutex.
    // Note m_TextureViews hands out interior pointers, so a reader must hold the
    // shared lock across the dereference, not merely across the lookup.
    mutable std::shared_mutex m_TextureViewMutex;
    GenerationalVector<VkImageView> m_TextureViews;
    std::unordered_map<uint64_t, VkImageAspectFlags> m_TextureViewAspects;
    std::unordered_map<uint64_t, VkFormat> m_TextureViewFormats;
    std::unordered_map<uint32_t, TextureHandle> m_TextureViewOwners;

    // Everything a descriptor write needs about a view, resolved in one pass so
    // the shared lock is taken once per element instead of once per lookup.
    struct TextureViewDescriptorInfo
    {
        VkImageView view = VK_NULL_HANDLE;
        VkImageAspectFlags aspect = 0;
        // Owning texture is GENERAL-resident (TextureDesc::sampledInGeneralLayout):
        // the sampled descriptor must claim GENERAL like the texture-handle path,
        // matching the RG's SampledInGeneralLayout constraint, or VUID-vkCmdDraw-None-09600.
        bool sampledInGeneral = false;
    };
    TextureViewDescriptorInfo ResolveTextureViewForDescriptor(TextureViewHandle view) const;

    VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
    VkQueue m_PresentQueue = VK_NULL_HANDLE;
    VkCommandPool m_CommandPool = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
    // VK_EXT_debug_utils command entry points, resolved once per logical device.
    // Null when the extension is not enabled on the instance — the object-naming
    // and label call sites then no-op.
    PFN_vkSetDebugUtilsObjectNameEXT m_SetVkObjectNameFn = nullptr;
    DebugUtilsLabelFns m_DebugUtilsLabelFns;
    void ResolveDebugUtilsEntryPoints();

    // Surface and swapchain
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_SwapchainImages;
    std::vector<VkImageView> m_SwapchainImageViews;
    // Best-effort swapchain image layout tracking by image index (0..N-1).
    // Initialized to UNDEFINED on swapchain creation; updated as we transition
    // images for rendering and present.
    std::vector<VkImageLayout> m_SwapchainImageLayouts;
    VkFormat m_SwapchainImageFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR m_SwapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D m_SwapchainExtent = {0, 0};
    // True when the swapchain imageUsage includes TRANSFER_SRC (surface-dependent),
    // enabling direct backbuffer readback for full-window screenshots.
    bool m_SwapchainSupportsReadback = false;
#if defined(__APPLE__)
    bool m_PreferTenBitSwapchain = false;
#else
    bool m_PreferTenBitSwapchain = true;
#endif
    // #767 P6a: default OFF on every platform — no host changes behaviour
    // until someone opts in. macOS is the candidate to default it ON, pending
    // the cost measurement quoted in ChooseSwapSurfaceFormat.
    bool m_PreferUnormSdrSwapchain = false;
    bool m_SwapchainColorSpaceExtEnabled = false;
    bool m_HdrMetadataExtEnabled = false;
    // True on Vulkan Portability implementations (MoltenVK on macOS). Such drivers enumerate
    // VkSurfaceFormatKHR colorspaces as a static formats x colorspaces product without consulting
    // real display capability, so the HDR10 PQ / HLG colorspace enumeration is not a trustworthy
    // capability signal there (only scRGB / EDR is a real, honored HDR path on macOS).
    bool m_IsPortabilitySubsetDevice = false;
#ifdef VK_EXT_HDR_METADATA_EXTENSION_NAME
    PFN_vkSetHdrMetadataEXT m_FpSetHdrMetadataEXT = nullptr;
#endif
    HdrOutputState m_HdrState{};

    struct WindowTargetState
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> swapchainImages;
        std::vector<VkImageView> swapchainImageViews;
        std::vector<VkImageLayout> swapchainImageLayouts;
        VkFormat swapchainImageFormat = VK_FORMAT_UNDEFINED;
        VkColorSpaceKHR swapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        VkExtent2D swapchainExtent = {0, 0};
        bool swapchainSupportsReadback = false;
        HdrOutputState hdrState{};
        VkRenderPass swapchainRenderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> swapchainFramebuffers;
        std::vector<VkSemaphore> presentReadySemaphores;
        VkFence presentCompleteFence = VK_NULL_HANDLE;
        bool presentFenceArmed = false;
        std::unordered_map<TextureHandle, VulkanTexture> swapchainTextures;
        uint32_t frameIndex = 0;
        // Per-target progress used by target-scoped drain/retirement.
        uint64_t lastGraphicsTimelineValue = 0;
        uint32_t lastArmedFrameIndex = UINT32_MAX;
    };
    struct PendingWindowTargetRetirement
    {
        uint64_t targetId = 0;
        WindowTargetState state{};
        uint64_t retireTimelineValue = 0;
        uint64_t enqueueFrameCounter = 0;
    };
    std::unordered_map<uint64_t, WindowTargetState> m_WindowTargets;
    std::vector<PendingWindowTargetRetirement> m_PendingWindowTargetRetirements;
    uint64_t m_NextWindowTargetId = 1;
    uint64_t m_ActiveWindowTargetId = 0;
    bool m_HasActiveWindowTarget = false;
    uint32_t m_ActiveWindowTargetFrameIndex = 0;
    VkFence m_ActiveWindowTargetPresentCompleteFence = VK_NULL_HANDLE;
    bool m_ActiveWindowTargetPresentFenceArmed = false;
    uint64_t m_ActiveWindowTargetLastGraphicsTimelineValue = 0;
    uint32_t m_ActiveWindowTargetLastArmedFrameIndex = UINT32_MAX;
    uint64_t m_WindowTargetRetirementFrameCounter = 0;

    WindowTargetState CaptureWindowTargetState() const;
    void ApplyWindowTargetState(const WindowTargetState& state);
    void ResetWindowTargetMembers();
    bool CreateActiveWindowTargetPresentFence();
    void DestroyWindowTargetState(WindowTargetState& state);
    void EnqueueWindowTargetRetirement(uint64_t targetId, WindowTargetState&& state, uint64_t retireTimelineValue);
    void ProcessPendingWindowTargetRetirements(bool allowBlockingFallback);
    void TeardownAllWindowTargetsForShutdown();
    void SaveActiveWindowTargetState();
    bool EnsureGlobalGpuIdle(uint64_t timeoutNs = UINT64_MAX);

    // Q6 unified device-loss observation. Every site that can see a lost or hung
    // device classifies its VkResult and routes through these two choke points
    // (DeviceRecoveryTests' DeviceLossRouting lint holds the authoritative list).
    // They drive the DeviceHealthState machine and emit the one-time diagnostic
    // (marker-ring dump + GPU checkpoints) on the first transition edge.
    //
    // Render thread only: OnDeviceLostObserved drives DeviceHealthState's
    // recover-relose bookkeeping, whose counters are plain (non-atomic) members,
    // and retrieves checkpoints from queues the render thread submits to. Worker
    // threads hand off through NoteDeviceLostOnWorkerThread instead.
    //
    // @param deviceActuallyLost False when the caller manufactured the result
    //        rather than reading it off the API — fault injection, and the
    //        hung-escalation cap. Those run against a device that is still alive,
    //        so every driver query whose validity requires the _lost_ state has to
    //        be skipped; the health machine and the CPU-side dumps still run.
    void OnDeviceLostObserved(const char* site, VkResult result, bool deviceActuallyLost);
    void OnFenceTimeout(const char* queueName);

    // Worker-thread device-loss hand-off. A job thread that sees VK_ERROR_DEVICE_LOST
    // on its own submit records it here; BeginFrame performs the observation on the
    // render thread. The record is stamped with the device generation it was taken
    // on, so an observation that a rebuild has already overtaken is dropped rather
    // than re-latching a loss onto the fresh device.
    void NoteDeviceLostOnWorkerThread();
    void DrainWorkerDeviceLossObservation();
    // Surface "GPU busy" (log-based, throttled) once a Hung episode passes the
    // perceptible threshold; the last-good frame stays on screen meanwhile.
    void MaybeSurfaceHungFrame(const char* queueName, std::chrono::steady_clock::time_point now);
    // Why a timeline wait ended. A lost device and an expired budget are different
    // facts about the GPU and must not reach a diagnostic as the same message.
    enum class TimelineWaitOutcome
    {
        Completed,
        TimedOut,
        DeviceLost,
        NotATimeline, // handle is unregistered or binary — no wait was attempted
        Failed,
    };
    // The one implementation; WaitTimelineSemaphoreValue is the IDevice narrowing of
    // it to "did the wait complete".
    TimelineWaitOutcome WaitTimelineSemaphore(SemaphoreHandle sem, uint64_t value, uint64_t timeoutNs);
    static const char* TimelineWaitOutcomeToString(TimelineWaitOutcome outcome);

    bool IsFenceSignaled(VkFence fence) const;
    bool WaitForFence(VkFence fence, uint64_t timeoutNs);
    bool WaitForSpecificGraphicsFence(uint32_t frameIndex, uint64_t timeoutNs);
    bool WaitForArmedGraphicsFences(uint64_t timeoutNs);
    void FlushPerFrameDeferredResourcesAfterIdle();
    void FlushTimelineDeferredResourcesAfterIdle();
    void CompactLiveTrackingVectors();
    void FlushDeferredResourcesAndCompactLiveTracking();
    void RecordActiveWindowTargetGraphicsProgress(uint64_t graphicsTimelineValue);
    void RecordActiveWindowTargetFenceArmed(uint32_t frameIndex);
    void RetireOrDestroyWindowTargetSemaphore(VkSemaphore sem, bool safeToDestroyNow);
    void FlushRetiredWindowTargetSemaphores();
    void MaybeWarnOrForceWindowTargetRetirement();

    // Offscreen framebuffer resources (for dummy swapchain)
    VkImage m_OffscreenImage = VK_NULL_HANDLE;
    VkImageView m_OffscreenImageView = VK_NULL_HANDLE;
    VkDeviceMemory m_OffscreenImageMemory = VK_NULL_HANDLE;

    // Every VkSemaphore this backend owns is created and destroyed through these
    // two, so the ledger below counts the whole population. Nothing else in the
    // backend may call vkCreateSemaphore / vkDestroySemaphore directly.
    VkResult CreateSemaphoreTracked(const VkSemaphoreCreateInfo& createInfo, VkSemaphore& outSemaphore);
    void DestroySemaphoreTracked(VkSemaphore semaphore);
    // Called immediately before vkDestroyDevice: records how many semaphores this
    // device generation created and never destroyed, then re-arms for the next one.
    void CloseSemaphoreLedgerForDeviceGeneration();

    // Synchronization objects
    std::vector<VkSemaphore> m_ImageAvailableSemaphores;
    // One per swapchain image, signaled after the present-transition barrier submit.
    // Created and destroyed by CreateSwapchain; ownership then rides WindowTargetState
    // and is released by DestroyWindowTargetState. Empty whenever no swapchain exists,
    // so ResetWindowTargetMembers' clear() can never orphan a handle.
    std::vector<VkSemaphore> m_PresentReadySemaphores;
    // Present semaphores retired from torn-down window targets when immediate destruction is not yet safe.
    std::vector<VkSemaphore> m_RetiredWindowTargetSemaphores;
    size_t m_NextRetiredWindowSemaphoreWarnThreshold = 128;
    size_t m_NextPendingRetirementWarnThreshold = 8;
    uint64_t m_WindowTargetRetirementForcedIdleCount = 0;
    double m_WindowTargetRetirementForcedIdleTotalMs = 0.0;
    double m_WindowTargetRetirementForcedIdleMaxMs = 0.0;

    // Semaphore ledger for the CURRENT VkDevice. Reset when a generation closes,
    // so the difference is that generation's live population rather than a
    // process-lifetime total. Atomic because timeline semaphores are created and
    // destroyed off the render thread.
    std::atomic<uint64_t> m_SemaphoresCreated{0};
    std::atomic<uint64_t> m_SemaphoresDestroyed{0};
    uint64_t m_LastDeviceGenerationSemaphoreResidual = 0;

    // Small fence registration helper for per-frame CPU pacing
    FenceRegistration m_FenceReg;

    // Advanced only on the main thread (BeginFrame/Present and the QueueSubmit /
    // ExecuteCommandLists render paths), but READ cross-thread through
    // GetFrameIndex(): BindlessResourceManager's park clock samples it from
    // whichever thread registers a bindless texture, and ECS extraction workers
    // do exactly that. Atomic so that read is defined; relaxed is enough because
    // the clock only needs coherence (a slot value never goes backwards for an
    // observer), and a stale observation merely delays a parked descriptor
    // index's retirement — the conservative direction.
    std::atomic<uint32_t> m_CurrentFrame{0};
    // True while the slot m_CurrentFrame names has had its fences waited: set by
    // BeginFrame once all three queue waits pass, cleared by AdvanceFrameIndex.
    // Starts true because nothing has been submitted under slot 0 yet. Read from
    // whatever thread performs a host write, hence atomic.
    std::atomic<bool> m_CurrentFrameSlotFenced{true};
    // Device-loss health state machine (Q6). Replaces the old one-way m_DeviceLost
    // latch: unifies the submit / fence-wait / acquire / present observation sites
    // into one tri-state read once per frame in BeginFrame plus on error paths.
    DeviceHealthState m_Health;
    // Monotonic successful-rebuild counter (Q6 slice 3b trigger). Bumped in
    // RebuildDevice on success; polled by poll-based recovery consumers (ECS
    // component-handle pass, EZTree regen, HLOD re-reconcile) that hold no
    // device-rebuilt callback. Relaxed atomic: a single writer (the render thread
    // in RebuildDevice) and readers on the same or the main thread; only atomicity
    // and monotonicity matter, not ordering against other state.
    std::atomic<uint64_t> m_DeviceRebuildGeneration{0};
    // GE_DEVICE_RECOVERY kill switch, latched at Initialize. When false the device
    // behaves exactly as before Q6 (latch + suppress, no poll-resume / no shutdown
    // health-gating / no mid-frame short-circuit).
    bool m_RecoveryEnabled = true;
    // A device loss seen on a job thread, awaiting observation by the render thread
    // at the top of BeginFrame. See PendingWorkerDeviceLoss for the claim rules.
    PendingWorkerDeviceLoss m_PendingWorkerDeviceLoss;
    // Throttle for the "GPU busy" surfacing while Hung (render-thread only).
    std::chrono::steady_clock::time_point m_LastHungSurfaceLog{};
    // Fault injection (Q6 slice 5), parsed once at Initialize. Inert unless an
    // env var is set; the counters below advance only while it is Active().
    VulkanFaultInjection m_FaultInjection;
    uint64_t m_FaultFrameCounter = 0;  // 1-based BeginFrame ordinal
    uint64_t m_FaultSubmitCounter = 0; // 1-based graphics-submit ordinal
    // Start time of the most recent rebuild attempt; TickDeviceRecovery spaces the
    // next attempt by RebuildBackoffFor() so a real TDR reset settles first.
    std::chrono::steady_clock::time_point m_LastRebuildAttemptTime{};
    // GE_VK_FORCE_GPU_HANG: submit a non-terminating compute dispatch to provoke a
    // REAL adapter TDR. Pipeline is lazy-built from embedded SPIR-V and cached (the
    // rebuild teardown drops it; it re-builds on the rebuilt device if triggered again).
    VkPipeline m_GpuHangPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_GpuHangPipelineLayout = VK_NULL_HANDLE;
    void TriggerGpuHang();
    // DeviceDesc stashed at Initialize so RebuildDevice (Q6 slice 2) can re-run
    // device+resource bringup with the original configuration without the caller
    // re-supplying it.
    DeviceDesc m_InitDesc;
    // CPU-side frames-in-flight:
    // - how many frames worth of per-frame resources we keep (fences, command buffers, transient allocators, etc.)
    // - how far the CPU is allowed to get ahead of the GPU before we have to wait
    //
    // This is intentionally independent of the swapchain image count. A swapchain may have 3+ images while we keep
    // CPU frames in flight separately tuned for the latency-vs-overlap tradeoff.
    //
    // Bumped from 2 to 3 to break the steady-state graphics fence wait every frame: with N=2, by the time the CPU
    // comes back around to reuse slot N's command pool, the GPU may still be ~1ms behind, so BeginFrame stalls on
    // the slot's fence. Adding a third slot gives the CPU a full extra frame of breathing room before reuse, so
    // the wait fires only when the GPU is genuinely behind, not as a baseline cost. Memory cost: ~50% more for
    // per-frame buffer pools (PerFrameWritePool, FrameBufferAllocator, GPUScene per-frame instance/visibility
    // buffers, sky LUTs). Latency cost: up to one extra frame of input lag worst-case (less in practice when GPU
    // finishes early). The compile-time upper bound IDevice::kMaxSupportedFramesInFlight=4 already accommodates
    // this; downstream ring-sized structures use that constant for array sizing and pick up the runtime count
    // via device->GetFramesInFlight() at construction.
    static const int MAX_FRAMES_IN_FLIGHT = 3;

    // Render pass and framebuffers
    VkRenderPass m_SwapchainRenderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_SwapchainFramebuffers;

    // Per-thread command pool set for thread-safe command buffer allocation.
    // Each recording thread lazily gets its own VkCommandPool per queue type
    // to avoid external synchronization on the Vulkan command pool.
    static constexpr uint32_t kMaxThreadCommandPools = 16;
    struct ThreadCommandPoolEntry
    {
        // Free/used lists are segregated by command-buffer level. A primary and a
        // secondary must never share a recycle bucket: a retired secondary handed
        // back out as a primary (or the reverse) is a level mismatch that Vulkan
        // rejects. AcquireThreadCmdBuffer pops the primary bucket;
        // AcquireThreadSecondaryCmdBuffer pops the secondary bucket.
        static constexpr int kLevelPrimary = 0;
        static constexpr int kLevelSecondary = 1;
        static constexpr int kLevelCount = 2;

        VkCommandPool graphicsPool = VK_NULL_HANDLE;
        VkCommandPool computePool  = VK_NULL_HANDLE;
        VkCommandPool transferPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> freeCmd[kLevelCount][3];  // [level][queueOrdinal]
        // Per-frame-slot used lists. CBs submitted in frame slot N go into
        // usedCmdPerFrame[N]. They are only recycled to freeCmd when the
        // fence for frame slot N is waited (at BeginFrame N+framesInFlight).
        static constexpr uint32_t kMaxFrameSlots = 4;
        std::vector<VkCommandBuffer> usedCmdPerFrame[kMaxFrameSlots][kLevelCount][3]; // [frameSlot][level][queueOrdinal]
        std::thread::id threadId;
        // Mutex protecting freeCmd/usedCmd for cross-thread recycling.
        // The owning thread pops from freeCmd; any thread may push to freeCmd
        // via the VulkanCommandList destructor when a CL is destroyed on a
        // different thread than the one that created it.
        std::mutex recycleMutex;
    };
    mutable std::mutex m_ThreadPoolsMutex;
    std::vector<std::unique_ptr<ThreadCommandPoolEntry>> m_ThreadPools;
    // Rebuild generation, scoped to THIS device object: bumped when its thread
    // pools are torn down, so a thread-local cache taken before a rebuild is
    // rejected after it. It starts at 0 in every device and therefore says
    // nothing about which device a cache entry came from — that is m_DeviceId's
    // job.
    // Atomic like m_DeviceRebuildGeneration: bumped on the render thread during a
    // rebuild but read from worker threads (AcquireThreadPoolEntry's t_CachedPool
    // staleness check) and from VulkanCommandList on any thread — a plain read would
    // be a data race even though the bump happens under the exclusive rebuild window.
    std::atomic<uint32_t> m_DeviceGeneration{0};

    // Process-unique identity, assigned at construction and never reused.
    // t_CachedPool is `static thread_local` — ONE slot per thread for the whole
    // process, not one per device — so its key has to name the device. With a
    // generation-only key, a second live device (both at generation 0) is handed
    // the first device's pools and submits its command buffers to the wrong
    // queue: VUID-VkSubmitInfo-commonparent, and an access violation inside the
    // driver. Never-reused ids also survive a device being freed and a new one
    // landing on the same address, which a `this` comparison would not.
    static uint64_t NextDeviceId();
    const uint64_t m_DeviceId = NextDeviceId();

    struct ThreadPoolCache
    {
        ThreadCommandPoolEntry* entry = nullptr;
        uint64_t deviceId = 0;
        uint32_t generation = 0;
    };
    static thread_local ThreadPoolCache t_CachedPool;

    ThreadCommandPoolEntry* AcquireThreadPoolEntry(IDevice::QueueType queue);
    VkCommandBuffer AcquireThreadCmdBuffer(IDevice::QueueType queue);
    VkCommandBuffer AcquireThreadSecondaryCmdBuffer(IDevice::QueueType queue);
    void RecycleThreadPoolsForFrame(uint32_t frameSlot);
    void DestroyAllThreadPools();

    // Per-frame command buffer tracking (simple pool per queue)
    struct PerQueueFrame
    {
        VkCommandPool pool = VK_NULL_HANDLE; // reserved for future split pools
        std::vector<VkCommandBuffer> freeCmd;
        std::vector<VkCommandBuffer> usedCmd;
        // GPU-safe deferred resource destruction for backends without timeline semaphores:
        // these are destroyed at the next BeginFrame() after the per-frame fence is waited.
        std::vector<BufferHandle> deferredBuffers;
        std::vector<TextureHandle> deferredTextures;
        std::vector<TextureViewHandle> deferredTextureViews;
        std::vector<SamplerHandle> deferredSamplers;
        VkFence fence = VK_NULL_HANDLE;                // signals when this frame finishes (graphics path)
        bool fenceArmed = false;                       // true only if we submitted using this fence last time this frame index was active
        // Compute only: true once any command list was routed on the compute queue in this slot
        // since its fence was last armed, including thread-pool lists that retire by timeline and
        // never enter usedCmd. Written and read under the queue's submit lock
        // (RouteSubmittedCommandList, ArmComputeTransferFences).
        bool submittedSinceArm = false;
    };
    struct FrameResources
    {
        PerQueueFrame graphics;
        PerQueueFrame compute;
        PerQueueFrame transfer;
    };
    std::vector<FrameResources> m_Frames; // sized to MAX_FRAMES_IN_FLIGHT

    // Empty graphics submit whose only job is to signal `queueFrame`'s fence. Signals no
    // timeline value, so it takes the graphics queue lock without consuming one. Returns the
    // submit's VkResult, or VK_ERROR_INITIALIZATION_FAILED when there is no graphics queue to
    // submit to; the fence is armed exactly when the result is VK_SUCCESS.
    VkResult ArmGraphicsFrameFence(PerQueueFrame& queueFrame);

    // Retire tags for one deferred destruction, one per submitting queue (QueueRetireTags).
    //
    // A resource destroyed while a frame is being recorded can be referenced by
    // work on any queue, and WHICH queues is not knowable at destroy time, so
    // every queue records the value its next submit will signal.
    //
    // Tags for a destruction requested now. Caller must hold m_DeferredDestroyMutex.
    QueueRetireTags NextSubmitTagsLocked() const;

    // Samples every submit context's timeline and counter. Takes no lock; must not
    // be called while holding m_DeferredDestroyMutex.
    QueueTimelineProgress SampleQueueTimelines() const;

    // Completed value of each distinct submit context's timeline, sampled once per retire
    // sweep. Contexts that alias one physical queue share a timeline and yield one entry.
    QueueTimelineCompletions SampleQueueTimelineCompletions() const;

    // Completed value for the timeline a deferred entry recorded. Falls back to querying the
    // semaphore directly for a timeline outside the submit contexts, and to 0 (never retired)
    // when even that fails — resolving by role instead would compare against a timeline the
    // entry's submit never signalled.
    uint64_t CompletedValueFor(SemaphoreHandle timeline,
                               const QueueTimelineCompletions& completions) const;

    // Additional GPU-safe deferred destruction using timeline semaphore values (only when sync2/timelines are supported)
    struct DeferredTexture
    {
        TextureHandle handle;
        QueueRetireTags tags;
    };
    std::vector<DeferredTexture> m_DeferredTexturesTimeline; // destroyed when every queue passes its tag
    struct DeferredBuffer
    {
        BufferHandle handle;
        QueueRetireTags tags;
    };
    std::vector<DeferredBuffer> m_DeferredBuffersTimeline; // destroyed when every queue passes its tag
    // Guard against duplicate deferred destruction scheduling for the same buffer handle.
    std::unordered_set<uint64_t> m_PendingBufferDestroyIds;
    // Guard against duplicate deferred destruction scheduling for the same texture handle.
    std::unordered_set<uint64_t> m_PendingTextureDestroyIds;
    // Guard against duplicate deferred destruction scheduling for the same texture-view handle.
    std::unordered_set<uint64_t> m_PendingTextureViewDestroyIds;
    // Guard against duplicate deferred destruction scheduling for the same sampler handle.
    std::unordered_set<uint64_t> m_PendingSamplerDestroyIds;
    struct DeferredTextureView
    {
        TextureViewHandle handle;
        QueueRetireTags tags;
    };
    std::vector<DeferredTextureView> m_DeferredTextureViewsTimeline; // destroyed when every queue passes its tag
    struct DeferredSampler
    {
        SamplerHandle handle;
        QueueRetireTags tags;
    };
    std::vector<DeferredSampler> m_DeferredSamplersTimeline; // destroyed when every queue passes its tag

    // Guards m_Deferred{Buffers,Textures,TextureViews,Samplers}Timeline, the
    // m_Pending*DestroyIds sets, the per-frame
    // PerQueueFrame::deferred{Buffers,Textures,TextureViews,Samplers} vectors,
    // and m_DeferralFrameSlot.
    //
    // The public Destroy* entry points are genuinely concurrent: ECS extraction
    // systems (TerrainExtractionSystem, RenderExtractionSystem) resolve bindless
    // textures on JobSystem workers and reach them through TextureService. That is
    // the same contract m_ResourceTrackingMutex already declares for the live
    // vectors; these queues were the piece left unguarded.
    //
    // Lock ordering: m_DeferredDestroyMutex < m_ResourceTrackingMutex. Drains take
    // this lock only to move ready entries into a local buffer, then release it
    // before calling the Destroy*Immediate helpers (which take the tracking mutex),
    // so the two are never held together.
    mutable std::mutex m_DeferredDestroyMutex;

    // Frame slot that deferred destroys attach to on a device with no usable
    // timeline semaphore — the only case the timeline-keyed path cannot serve.
    // Published by BeginFrame; the Destroy* entry points read it instead of
    // m_CurrentFrame, which stays main-thread-only. A stale read can only name an
    // older slot, whose fence is waited later than the current one — safe in the
    // same direction as the deferral itself.
    //
    // This slot's drain waits the graphics fence alone, so it cannot cover
    // cross-queue references. That is the pre-existing behaviour for such
    // devices, which have no timeline to do better with.
    std::atomic<uint32_t> m_DeferralFrameSlot{0};

    // Graphics queue state of the frame slot used by the no-timeline fallback, or
    // nullptr when no frame slots exist. Caller must hold m_DeferredDestroyMutex.
    PerQueueFrame* DeferralFrameSlotLocked();

    // Queue a resource for GPU-safe destruction. Thread-safe; takes
    // m_DeferredDestroyMutex. Callers must have excluded the teardown case
    // (InTeardown()) first — these never destroy anything themselves.
    void QueueDeferredBufferDestroy(BufferHandle handle);
    void QueueDeferredTextureDestroy(TextureHandle handle);
    void QueueDeferredTextureViewDestroy(TextureViewHandle handle);
    void QueueDeferredSamplerDestroy(SamplerHandle handle);

    struct DeferredStagingBuffer
    {
        BufferHandle handle;
        SemaphoreHandle timeline;   // queue timeline whose completion releases this buffer
        uint64_t timelineValue = 0; // value the copy's submit signaled on `timeline`
    };
    std::vector<DeferredStagingBuffer> m_DeferredStagingBuffers;
    void RetireDeferredStagingBuffers();

    // Thread-pool command buffers awaiting GPU completion. Every submit path that uses a
    // NULL fence — SubmitTextureUploads on the transfer queue, and the main-thread
    // QueueSubmit / ExecuteCommandLists render paths (including PSO/material bring-up side
    // submissions) — signals its queue's timeline but passes no fence of its own. A slot's
    // frame fence covers such a CB only if it was submitted on that queue before the slot's
    // fence was armed: a CB routed after the arm (a worker submit before AdvanceFrameIndex)
    // and every transfer upload are not covered, so recycling by frame slot can reset a CB
    // still pending on the GPU. Instead each entry records the timeline its submit signaled and
    // the signaled value; RetireDeferredRecycleCommandBuffers returns the CB to its owning
    // pool's freelist only once the GPU has passed that value. Pool pointers stay valid for
    // the device lifetime — thread pools are destroyed only at teardown, after this list is
    // drained in FlushTimelineDeferredResourcesAfterIdle.
    struct DeferredRecycleCommandBuffer
    {
        ThreadCommandPoolEntry* pool = nullptr;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        int freeListOrdinal = 0;    // pool->freeCmd[] bucket (nominal queue ordinal)
        SemaphoreHandle timeline;   // queue timeline whose completion releases this CB
        uint64_t timelineValue = 0; // value the CB's submit signaled on `timeline`
    };
    // Shares m_DeferredStagingMutex (same producers/drain points as the staging buffers).
    // That lock nests under a queue's submit mutex (SubmitBatchToQueue routes CBs while holding
    // it) but is never held while taking recycleMutex — the retire/flush passes collect ready
    // CBs under it, then recycle outside it.
    std::vector<DeferredRecycleCommandBuffer> m_DeferredRecycleCmdBuffers;
    void RetireDeferredRecycleCommandBuffers();
    // Route a submitted thread-pool CB for recycling by timeline value (see above); falls
    // back to frame-slot recycling only on devices without timeline semaphores. Main-thread
    // submit paths only (the fallback reads m_CurrentFrame).
    void RouteThreadPoolCmdBufferForRecycle(ThreadCommandPoolEntry* pool, VkCommandBuffer cb,
                                            int freeListOrdinal, SemaphoreHandle timeline,
                                            uint64_t timelineValue);

    // Routes one just-submitted list's command buffer for recycling against `timelineValue` and
    // marks the list submitted. Call only for lists whose buffer actually reached vkQueueSubmit.
    void RouteSubmittedCommandList(VulkanCommandList* list, SemaphoreHandle timeline,
                                   uint64_t timelineValue);

    /// The device-level submit chokepoint: submits one batch to `context`'s queue and routes
    /// each of `commandLists` for deferred recycling against the value THIS submit signalled.
    ///
    /// `submitFn(uint64_t signalValue) -> bool` issues the single vkQueueSubmit; see
    /// QueueSubmitContext::SubmitAndSignal for the contract it must honour. Routing runs inside
    /// the queue's lock on success, so a command buffer is tagged with its own submit's value by
    /// construction — never with a re-read of the queue counter, which another thread's submit
    /// may already have advanced past it. Tagging low is what frees a buffer mid-execution.
    template <typename SubmitFn>
    QueueSubmitContext::SubmitResult SubmitBatchToQueue(QueueSubmitContext& context,
                                                       std::span<VulkanCommandList* const> commandLists,
                                                       SubmitFn&& submitFn)
    {
        return context.SubmitAndSignal([&](uint64_t signalValue) {
            if (!submitFn(signalValue))
                return false;
            for (VulkanCommandList* list : commandLists)
                RouteSubmittedCommandList(list, context.Timeline(), signalValue);
            return true;
        });
    }

    // Internal helper: destroy view immediately (no deferral)
    void DestroyTextureViewImmediate(TextureViewHandle h);
    void DestroySamplerImmediate(SamplerHandle handle);

    VkCommandBuffer AcquireCmdBuffer(IDevice::QueueType queue);
    void RecycleCmdBuffer(IDevice::QueueType queue, VkCommandBuffer cmd);

    // Query current completed graphics timeline value (0 if not supported)
    bool HasGraphicsTimelineSemaphore() const;
    uint64_t GetGraphicsTimelineCompletedValue() const;

    uint32_t m_CurrentSwapchainImage = 0;
    // Descriptor set pooling
    DescriptorSetAllocator m_DsAllocator;

    // Caches for render passes and framebuffers (simple 64-bit keyed caches)
    std::unordered_map<uint64_t, VkRenderPass> m_RenderPassCache;
    std::unordered_map<uint64_t, VkFramebuffer> m_FramebufferCache;

    // Pipeline layouts and descriptor set layouts are owned by VulkanPipeline structs via
    // the global resource managers, avoiding per-device ownership and double-destruction.

    // Queue family indices (unassigned slots use kInvalidQueueFamilyIndex — same value as VK_QUEUE_FAMILY_IGNORED;
    // never pass that to vkCreateCommandPool.)
    static constexpr uint32_t kInvalidQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uint32_t m_GraphicsQueueFamily = kInvalidQueueFamilyIndex;
    uint32_t m_PresentQueueFamily = kInvalidQueueFamilyIndex;
    // Whether to enable WSI/swapchain path (device extension gating)
    bool m_UseSwapchain = false;
    // When true, ChooseSwapPresentMode selects FIFO (hard vsync); otherwise prefers MAILBOX.
    bool m_Vsync = false;

    // Optional dedicated queues
    VkQueue m_ComputeQueue = VK_NULL_HANDLE;
    VkQueue m_TransferQueue = VK_NULL_HANDLE;
    uint32_t m_ComputeQueueFamily = kInvalidQueueFamilyIndex;
    uint32_t m_TransferQueueFamily = kInvalidQueueFamilyIndex;

    // One submit chokepoint per PHYSICAL queue (see QueueSubmitContext). Storage holds the
    // distinct queues; m_SubmitContextByRole points each role at the context of the queue its
    // work actually runs on, so roles that alias one queue share one lock and one timeline:
    // compute or transfer falling back to graphics when no dedicated family exists, and compute
    // and transfer resolving to a single shared family. Bound by BindQueueSubmitContexts once
    // the queues and their timelines exist; every entry points at the graphics context until
    // then, so a pre-bringup read sees an unbound context rather than a null pointer.
    //
    // Lock ordering: a queue's submit mutex sits under m_DeviceRebuildMutex (OUTERMOST),
    // independent of m_ResourceTrackingMutex and m_ThreadPoolsMutex, and may nest above
    // m_DeferredStagingMutex, m_DeferredDestroyMutex and a pool's recycleMutex.
    // m_DeferredDestroyMutex is taken under it on EVERY submit path, not just transfer:
    // SubmitBatchToQueue routes each list inside the queue's lock, and RouteSubmittedCommandList
    // -> VulkanCommandList::OnSubmitted -> ReleaseAttachmentViews -> DestroyTextureView reaches
    // QueueDeferredTextureViewDestroy. Nothing takes a queue mutex from under
    // m_DeferredDestroyMutex — NextSubmitTagsLocked reads the contexts' atomic counters only —
    // so the nesting is one-directional.
    // Never hold two queue mutexes at once: submit to one queue, release, then the next.
    std::array<QueueSubmitContext, kQueueRoleCount> m_SubmitContextStorage;
    std::array<QueueSubmitContext*, kQueueRoleCount> m_SubmitContextByRole{};

    void BindQueueSubmitContexts();
    QueueSubmitContext& SubmitContextFor(QueueType queue);
    const QueueSubmitContext& SubmitContextFor(QueueType queue) const;
    // Gates device/allocator-touching entry points against an in-place device
    // rebuild. Every create/update/map/destroy entry reachable off the render
    // thread holds this SHARED for its duration via DeviceRebuildSharedGuard
    // (SubmitTextureUploads holds it across staging-buffer/VMA prep through
    // submit); RebuildDevice holds it EXCLUSIVE across the entire
    // teardown+bringup window. This closes the prep-phase TOCTOU: a worker that
    // passes a health check then deschedules across a loss cannot resume into
    // vmaCreateBuffer on an allocator ShutdownVMA is destroying.
    // Lock ordering: m_DeviceRebuildMutex is OUTERMOST — acquired before any
    // queue's submit mutex (workers) and never nested under one. Hence the one
    // exemption: DestroyTextureView takes no entry guard, because it runs under
    // a queue's submit mutex on every submit path (see the routing note above)
    // and reaches only the mutex-protected deferred queues; its worker-reachable
    // paths all sit inside an outer guarded entry on the same thread.
    std::shared_mutex m_DeviceRebuildMutex;
    // Holder of the exclusive rebuild lock while RebuildDevice runs. Guarded
    // entries compare-and-skip against it: std::shared_mutex is not recursive,
    // and bringup (CreateDeviceAndResources) legitimately re-enters create
    // paths under the exclusive lock.
    std::atomic<std::thread::id> m_RebuildExclusiveThread{};
    // True from the start of rebuild teardown until bringup begins. The
    // per-device cache cleanups run in this window (inside the exclusive lock,
    // by contract — see RegisterPerDeviceCacheCleanup): destroy handles and drop
    // state only. Create-class entries assert against it so a cleanup that
    // creates or uploads is loud in dev builds.
    std::atomic<bool> m_RebuildTeardownPhase{false};
    // Thread that drives this device's frames and teardown (captured at
    // CreateDeviceAndResources, refreshed each BeginFrame). The immediate
    // destroy helpers assert against it: they touch VkDevice/VMA with no
    // deferral and are only sound from this thread (deferred-drain sweeps,
    // WaitForIdle, Shutdown, rebuild teardown).
    std::atomic<std::thread::id> m_DeviceOwnerThread{};
    // RAII shared acquisition of m_DeviceRebuildMutex for device-touching entry
    // points. Skips when this thread already holds the mutex — as the rebuild
    // thread (exclusive), or via an outer guarded entry (shared): recursive
    // shared_mutex acquisition deadlocks once a writer queues between the two
    // acquisitions. Kind::Create additionally asserts the rebuild-teardown
    // no-create contract.
    class DeviceRebuildSharedGuard
    {
      public:
        enum class Kind : uint8_t
        {
            Create,
            Access
        };
        DeviceRebuildSharedGuard(VulkanDevice& device, Kind kind);
        ~DeviceRebuildSharedGuard();
        DeviceRebuildSharedGuard(const DeviceRebuildSharedGuard&) = delete;
        DeviceRebuildSharedGuard& operator=(const DeviceRebuildSharedGuard&) = delete;

      private:
        VulkanDevice* m_LockedDevice = nullptr;
        const VulkanDevice* m_PreviousHeld = nullptr;
    };

  public:
    // Bounded map..use..unmap window for off-render-thread callers. MapBuffer's
    // own guard covers only the vmaMapMemory call — the returned pointer
    // outlives it, so a device rebuild could free the allocation under a
    // caller's read/write. This holds the rebuild guard for the whole scope:
    // map at construction, unmap + release at destruction. Keep the scope
    // short — the rebuild's exclusive acquisition waits on it. Long-lived
    // persistent mappings (rings) are render-thread-owned and do not use this.
    class ScopedBufferMap
    {
      public:
        ScopedBufferMap(VulkanDevice& device, BufferHandle handle)
            : m_Guard(device, DeviceRebuildSharedGuard::Kind::Access), m_Device(device), m_Handle(handle)
        {
            // The nested MapBuffer skips re-acquisition via the guard's thread-local.
            m_Mapped = device.MapBuffer(handle);
        }
        ~ScopedBufferMap()
        {
            // Unmap in the body, so it runs BEFORE m_Guard's destructor releases
            // the shared lock.
            if (m_Mapped)
                m_Device.UnmapBuffer(m_Handle);
        }
        ScopedBufferMap(const ScopedBufferMap&) = delete;
        ScopedBufferMap& operator=(const ScopedBufferMap&) = delete;
        void* Get() const { return m_Mapped; }
        explicit operator bool() const { return m_Mapped != nullptr; }

      private:
        DeviceRebuildSharedGuard m_Guard;
        VulkanDevice& m_Device;
        BufferHandle m_Handle;
        void* m_Mapped = nullptr;
    };

  private:
    // Protects m_DeferredStagingBuffers (accessed from worker threads via
    // SubmitTextureUploads and from the main thread via RetireDeferredStagingBuffers).
    // Mutable so const observers (GetResourcePoolStats) can sample the depth
    // under the lock instead of racing the producers.
    mutable std::mutex m_DeferredStagingMutex;

    // Per-queue command pools
    VkCommandPool m_ComputeCommandPool = VK_NULL_HANDLE;
    VkCommandPool m_TransferCommandPool = VK_NULL_HANDLE;

    // Per-frame aggregator of waits for Present()
    std::vector<IDevice::SemaphoreWait> m_EndOfFrameWaits;
    bool m_DidBackbufferRenderThisFrame = false;

    bool m_AnyGraphicsSubmitThisFrame = false;

    // Timeline semaphore registry. Counter values are never mirrored here: the authority on
    // "how far has this queue been submitted to" is the owning QueueSubmitContext, and the
    // authority on "how far has the GPU got" is vkGetSemaphoreCounterValue.
    struct TimelineSemaphore
    {
        VkSemaphore sem = VK_NULL_HANDLE;
        bool isTimeline = true;
    };
    std::unordered_map<uint64_t, TimelineSemaphore> m_TimelineSemaphores; // keyed by Handle id
    // Optional binary relay used to bridge timeline waits to present wait binaries
    SemaphoreHandle m_RenderGraphPresentRelay;

    // Per-queue timelines. The last value signalled on each lives in that queue's
    // QueueSubmitContext, which is the only thing allowed to advance it.
    SemaphoreHandle m_GraphicsTimeline;
    SemaphoreHandle m_ComputeTimeline;
    SemaphoreHandle m_TransferTimeline;

    // Track swapchain acquisition and whether we've consumed imageAvailable in this frame
    bool m_AcquiredThisFrame = false;
    bool m_WaitedOnImageAvailableThisFrame = false;
    VkSemaphore m_AcquireSemaphoreThisFrame = VK_NULL_HANDLE;

    // Handle manager for opaque handles
    HandleManager m_HandleManager;

    // Device properties
    VkPhysicalDeviceProperties m_DeviceProperties{};
    VkPhysicalDeviceFeatures m_DeviceFeatures{};
    VkPhysicalDeviceMemoryProperties m_MemoryProperties{};

    // Derived once in SelectPhysicalDevice from the two members above and NEVER
    // cleared, which is what makes it safe to read from a job worker: CreateBuffer
    // resolves every UploadDeviceLocalPreferred allocation against it, and those
    // run on job threads (MeshGPURegistry::UploadMesh) with no lock. An in-place
    // device rebuild re-runs QueryDeviceCapabilities but NOT SelectPhysicalDevice,
    // so the physical device and its memory properties survive and this value
    // cannot change; publishing it through the m_Capabilities reset instead would
    // leave a window in which a worker reads a disengaged topology and silently
    // pins that pool to system RAM for its lifetime.
    std::optional<DeviceMemoryTopology> m_MemoryTopology;

    // Capabilities
    RenderingDeviceCapabilities m_Capabilities{};

    // Phase-0 diagnostics for the planned VK_EXT_descriptor_buffer migration.
    // Read-only, populated once in QueryDeviceCapabilities.
    struct DescriptorBufferDiagnostics
    {
        bool extensionAvailable = false;
        // Features
        bool descriptorBuffer = false;
        bool descriptorBufferCaptureReplay = false;
        bool descriptorBufferImageLayoutIgnored = false;
        bool descriptorBufferPushDescriptors = false;
        // Properties
        uint64_t descriptorBufferOffsetAlignment = 0;
        uint32_t samplerDescriptorSize = 0;
        uint32_t combinedImageSamplerDescriptorSize = 0;
        uint32_t sampledImageDescriptorSize = 0;
        uint32_t storageImageDescriptorSize = 0;
        uint32_t uniformBufferDescriptorSize = 0;
        uint32_t storageBufferDescriptorSize = 0;
        uint32_t inputAttachmentDescriptorSize = 0;
        // Only reported when VK_KHR_acceleration_structure is also present;
        // stays 0 otherwise, which the write path treats as "type unsupported".
        uint32_t accelerationStructureDescriptorSize = 0;
        bool combinedImageSamplerDescriptorSingleArray = false;
        bool bufferlessPushDescriptors = false;
        uint64_t maxResourceDescriptorBufferRange = 0;
        uint64_t maxSamplerDescriptorBufferRange = 0;
        // Max resource descriptor buffers bindable to one command buffer at once
        // (vkCmdBindDescriptorBuffersEXT). Spec floor is 3; desktop drivers ~8.
        // The transient pool spills a new buffer per region overflow, so a heavy
        // command buffer can approach this — the bind path caps against it.
        uint32_t maxResourceDescriptorBufferBindings = 0;
        // Push descriptor (VK_KHR_push_descriptor / Vulkan 1.4 core)
        uint32_t maxPushDescriptors = 0;
    } m_DescBufferDiag;

    // No more per-device resource containers!
    // Resources are now managed globally through specialized managers

    // Separate storage for swapchain images that use predetermined handles in a reserved range.
    // Entries are lazily populated from GetCurrentSwapchainImageHandle and cleared on
    // swapchain recreation and during final VulkanDevice cleanup.
    std::unordered_map<TextureHandle, VulkanTexture> m_SwapchainTextures;

    // Debug
    bool m_DebugLayerEnabled = false;
    // VK_EXT_debug_utils on the instance. Independent of the validation layer:
    // labels and object names are enabled in every config, so a capture or a
    // driver crash report reads the same in DebugFast and Release as in Debug.
    bool m_DebugUtilsExtEnabled = false;
    // The apiVersion the instance was CREATED with (min of loader and 1.3).
    // Spec-legality of core physical-device queries is judged against this, not
    // against what the loader could have offered.
    uint32_t m_InstanceApiVersion = 0;
    bool m_EnableDescriptorValidation = true;
    // Gate for descriptor-update layout validation (metadata tracking + per-update
    // checks). Always on in assert-enabled builds (Debug/DebugFast — NDEBUG
    // undefined) so type/bounds misuse fails loudly where developers run: on the
    // descriptor-buffer path an unchecked array overflow writes raw bytes into the
    // ADJACENT set's descriptors. An explicit enableDescriptorValidation=false is
    // honored only where asserts are compiled out (Release), keeping the shipped
    // opt-out at literal zero cost — no tracking, no per-update lookup.
    bool DescriptorValidationActive() const
    {
#ifndef NDEBUG
        return true;
#else
        return m_EnableDescriptorValidation;
#endif
    }
    uint32_t m_MaxPushConstantBytes = 128;
    std::string m_ApplicationName;

  public:
    uint32_t GetPolicyMaxPushConstantsSize() const { return m_MaxPushConstantBytes; }

    // Multi-threading support: allocate secondary command buffers from per-thread pools.
    VkCommandBuffer AcquireSecondaryCmdBuffer(IDevice::QueueType queue) { return AcquireThreadSecondaryCmdBuffer(queue); }

    // VMA (Vulkan Memory Allocator)
    void* m_VmaAllocator = nullptr; // VmaAllocator handle

    // Pipeline cache for performance optimization

    // Dynamic rendering capability flag
    // Debug capture buffer
#if GE_ENABLE_DEBUG_BARRIERS
    std::vector<DebugBarrierInfo> m_DebugBarriers;
    size_t m_DebugBarriersHighWater = 0;
#endif

    // Debug counter: number of ownership-transfer barriers emitted this frame
    uint32_t m_DebugOwnershipTransfers = 0;
    // Debug counter: number of vkCmdPipelineBarrier2 calls this frame
    uint32_t m_DebugPipelineBarrier2Calls = 0;
    // Debug counts: number of barriers included in the last vkCmdPipelineBarrier2 batch
    uint32_t m_DebugLastDepImageCount = 0;
    uint32_t m_DebugLastDepBufferCount = 0;
    uint32_t m_DebugLastDepMemoryCount = 0;

    // Debug helpers

    // Debug counters: pipeline binds per bind point (per frame)
    uint32_t m_DebugPipelineBindGraphics = 0;
    uint32_t m_DebugPipelineBindCompute = 0;
    void DebugIncPipelineBindGraphics() { ++m_DebugPipelineBindGraphics; }
    void DebugIncPipelineBindCompute() { ++m_DebugPipelineBindCompute; }

  public:
    uint32_t GetDebugOwnershipTransfers() const { return m_DebugOwnershipTransfers; }
    void DebugIncOwnershipTransfers() { ++m_DebugOwnershipTransfers; }

    // Debug counters: descriptor set binds per bind point and set index (per frame)
    uint32_t m_DebugDescBindGraphics[8] = {0};
    uint32_t m_DebugDescBindCompute[8] = {0};

    // Debug helpers for last dependency counts
    void DebugSetLastDependencyCounts(uint32_t img, uint32_t buf, uint32_t mem)
    {
        m_DebugLastDepImageCount = img;
        m_DebugLastDepBufferCount = buf;
        m_DebugLastDepMemoryCount = mem;
    }
    uint32_t GetDebugLastDependencyImageCount() const { return m_DebugLastDepImageCount; }
    uint32_t GetDebugLastDependencyBufferCount() const { return m_DebugLastDepBufferCount; }
    uint32_t GetDebugLastDependencyMemoryCount() const { return m_DebugLastDepMemoryCount; }

    // Debug helpers for descriptor bind counters
    void DebugIncDescriptorBindGraphics(uint32_t set)
    {
        if (set < 8)
            ++m_DebugDescBindGraphics[set];
    }
    void DebugIncDescriptorBindCompute(uint32_t set)
    {
        if (set < 8)
            ++m_DebugDescBindCompute[set];
    }
    uint32_t GetDebugDescriptorBindGraphics(uint32_t set) const { return set < 8 ? m_DebugDescBindGraphics[set] : 0; }
    uint32_t GetDebugDescriptorBindCompute(uint32_t set) const { return set < 8 ? m_DebugDescBindCompute[set] : 0; }

    // Per-pass descriptor set capture for GPU debugging.
    // When enabled, draw calls record the currently bound descriptor sets
    // and their binding metadata into m_DescriptorCaptures.
    struct DescriptorBindingRecord
    {
        uint32_t Binding = 0;
        std::string ResourceType; // "Buffer", "Texture", "Sampler", "CombinedImageSampler", "StorageBuffer", "StorageImage"
        uint64_t ResourceHandle = 0; // Physical VkBuffer or VkImage handle
        std::string DebugName;
    };

    struct DescriptorCaptureEntry
    {
        std::string PassName;
        uint32_t DrawIndex = 0;
        uint64_t DescriptorSetHandle = 0;
        uint32_t SetIndex = 0;
        std::vector<DescriptorBindingRecord> Bindings;
    };

    void SetDescriptorCaptureEnabled(bool enabled, const std::string& passFilter)
    {
        m_DescriptorCapturePassFilter = passFilter;
        m_DescriptorCaptureEnabled.store(enabled, std::memory_order_release);
    }
    bool IsDescriptorCaptureEnabled() const { return m_DescriptorCaptureEnabled.load(std::memory_order_acquire); }
    const std::string& GetDescriptorCapturePassFilter() const { return m_DescriptorCapturePassFilter; }

    void ClearDescriptorCaptures() { m_DescriptorCaptures.clear(); m_DescriptorCaptureDrawIndex = 0; }
    void PushDescriptorCapture(DescriptorCaptureEntry entry)
    {
        m_DescriptorCaptures.push_back(std::move(entry));
    }
    const std::vector<DescriptorCaptureEntry>& GetDescriptorCaptures() const { return m_DescriptorCaptures; }
    uint32_t NextDescriptorCaptureDrawIndex() { return m_DescriptorCaptureDrawIndex++; }

    // Function pointers for sync2 (core or KHR)
    PFN_vkCmdPipelineBarrier2 m_FpCmdPipelineBarrier2 = nullptr;
    PFN_vkQueueSubmit2 m_FpQueueSubmit2 = nullptr;
    PFN_vkCmdBeginRendering m_FpCmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering m_FpCmdEndRendering = nullptr;
    PFN_vkCmdSetDepthWriteEnable m_FpCmdSetDepthWriteEnable = nullptr;

    // Mesh shader function pointer (EXT)
    PFN_vkCmdDrawMeshTasksEXT m_FpCmdDrawMeshTasksEXT = nullptr;
    bool m_SupportsMeshShaderEXT = false;

    // VK_EXT_descriptor_buffer function pointers. Populated iff
    // m_Capabilities.supportsDescriptorBuffer is true (see CreateLogicalDevice).
    PFN_vkGetDescriptorSetLayoutSizeEXT          m_FpGetDescriptorSetLayoutSizeEXT          = nullptr;
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT m_FpGetDescriptorSetLayoutBindingOffsetEXT = nullptr;
    PFN_vkGetDescriptorEXT                       m_FpGetDescriptorEXT                       = nullptr;
    PFN_vkCmdBindDescriptorBuffersEXT            m_FpCmdBindDescriptorBuffersEXT            = nullptr;
    PFN_vkCmdSetDescriptorBufferOffsetsEXT       m_FpCmdSetDescriptorBufferOffsetsEXT       = nullptr;

    // VK_NV_device_diagnostic_checkpoints function pointers. Populated iff the
    // extension was enabled at device creation (see CreateLogicalDevice); both
    // are resolved or neither is.
    PFN_vkCmdSetCheckpointNV       m_FpCmdSetCheckpointNV       = nullptr;
    PFN_vkGetQueueCheckpointDataNV m_FpGetQueueCheckpointDataNV = nullptr;
    // Mirrors the extension enable across the QueryDeviceCapabilities reset, the
    // same stash pattern the descriptor-buffer flag uses.
    bool m_DeviceDiagnosticCheckpointsEnabledAtInit = false;

    // VK_EXT_device_fault retrieval. Populated iff the extension was enabled and
    // its deviceFault feature accepted at device creation (see CreateLogicalDevice).
    // Independent of descriptor buffers and of checkpoint stage granularity: this
    // is the only path that can name a faulting GPU virtual address.
    PFN_vkGetDeviceFaultInfoEXT m_FpGetDeviceFaultInfoEXT = nullptr;
    bool m_DeviceFaultEnabledAtInit = false;
    // Formatted fault records from the most recent device loss, kept alongside
    // m_LastGpuCheckpointReport so upper layers can surface both together.
    std::string m_LastDeviceFaultReport;

    // Emission gate: extension available AND GE_VK_GPU_CHECKPOINTS requested.
    // Read on the marker path, so it stays a plain bool.
    bool m_GpuCheckpointsEnabled = false;
    // checkpointExecutionStageMask per queue family index, probed once at device
    // creation. Empty when the extension is not enabled. A zero entry means that
    // family reports no checkpoint stages, so the loss dump must say "blind"
    // rather than "nothing executed" for any queue drawn from it.
    std::vector<VkPipelineStageFlags> m_QueueFamilyCheckpointStages;
    // Interned checkpoint payloads. Outlives every command buffer and the device
    // loss itself (see GpuCheckpointNameTable) and is deliberately kept across a
    // device rebuild so recorded ordinals stay comparable.
    std::unique_ptr<GpuCheckpointNameTable> m_GpuCheckpointNames;
    // Formatted checkpoint report from the most recent device loss, kept so the
    // upper layers can surface it alongside the CPU marker dump.
    std::string m_LastGpuCheckpointReport;

    // GPU-AV request state, decided at instance creation and reported after device
    // creation: the shader-instrumentation half depends on whether
    // VK_EXT_descriptor_buffer ended up enabled, which is not known that early.
    // Survives device rebuilds, which do not re-run instance creation.
    bool m_GpuAvChained = false;
    bool m_GpuAvShaderInstrumentationRequested = false;
    // Whether VK_EXT_descriptor_buffer was in the enabled-extension list of the
    // device that was actually created. This, not m_DescriptorBufferEnabledAtInit,
    // is what the validation layer keys its shader-instrumentation gate on — the
    // engine can decline the descriptor-buffer path on a device that still has the
    // extension enabled, and reporting instrumentation live in that case would be
    // exactly the false all-clear this reporting exists to prevent.
    bool m_DescriptorBufferExtensionEnabledOnDevice = false;

    // Caller-supplied policy from DeviceDesc::descriptorBuffers. Captured at the top
    // of Initialize so CreateLogicalDevice / extension-enable / capability-rehydrate
    // all see the same value regardless of ordering.
    DescriptorBufferMode m_RequestedDescriptorBuffers = DescriptorBufferMode::Auto;
    // Runtime enable flag driven by GE_VK_USE_DESCRIPTOR_BUFFER env var at device init.
    bool m_UseDescriptorBuffer = false;
    // GE_VK_FILL_NEW_TARGETS_NAN: read once at Initialize. When set, every new
    // float color target (render target or storage) and every new storage
    // buffer is filled with NaN at creation, so a pass that reads one before
    // its first write shows NaN deterministically instead of whatever freed
    // memory held. Off: one branch per CreateTexture and CreateBuffer, nothing
    // per frame.
    bool m_FillNewTargetsWithNaN = false;
    void FillNewTargetWithNaN(TextureHandle texture, const TextureDesc& desc);
    void FillNewStorageBufferWithNaN(BufferHandle buffer, uint64_t sizeBytes);
    // Mirrors m_Capabilities.supportsDescriptorBuffer, but persists across the
    // QueryDeviceCapabilities() m_Capabilities-reset so the cap is rehydratable.
    bool m_DescriptorBufferEnabledAtInit = false;
    // Stash for Vulkan 1.2 hostQueryReset. Set in CreateLogicalDevice and
    // consumed by the query pool config after device creation.
    bool m_HostQueryResetEnabledAtInit = false;
    // Stash for Vulkan 1.2 bufferDeviceAddress. Set in CreateLogicalDevice,
    // consumed by QueryDeviceCapabilities (which zeroes m_Capabilities before
    // repopulating). Without this stash, the VMA allocator would be created
    // without BUFFER_DEVICE_ADDRESS_BIT and any subsequent buffer with
    // ShaderDeviceAddress usage would trip a VMA assertion.
    bool m_BufferDeviceAddressEnabledAtInit = false;
    // Stash for the Vulkan 1.2 descriptor-indexing feature set required by the
    // bindless material layout. Physical-device support is not enough; the
    // logical device must have enabled these bits before RenderServices can
    // safely advertise bindless resources.
    bool m_DescriptorIndexingEnabledAtInit = false;
    // Stash for the ray-query enablement (VK_KHR_acceleration_structure +
    // VK_KHR_ray_query + VK_KHR_deferred_host_operations with core feature
    // bits enabled at device creation). Consumed by QueryDeviceCapabilities
    // (which zeroes m_Capabilities before repopulating) to publish
    // supportsRayQuery. Stays false on both vkCreateDevice fallback paths —
    // they drop optional extensions, so RT silently stays unavailable.
    bool m_RayQueryEnabledAtInit = false;
    // AS backend for the RT shadow-mask lane. Non-null iff ray query was
    // enabled at init AND every KHR entry point resolved; reset early in
    // Shutdown (after the global idle) while VkDevice and VMA are alive.
    std::unique_ptr<VulkanAccelerationStructures> m_AccelerationStructures;
    // Shared descriptor-buffer allocator, lifetime tied to the device. Null when the
    // runtime path is off.
    std::unique_ptr<VulkanDescriptorBufferPool> m_DescriptorBufferPool;

  public:
    // Sentinel frameSlot value for persistent DB sets. Chosen so that the
    // BeginFrameReset "entry.frameSlot == m_CurrentFrame" purge predicate can
    // never match (m_CurrentFrame is always in [0, framesInFlight-1]).
    static constexpr uint32_t kPersistentFrameSlot = UINT32_MAX;

    // DB set-entry layout declared here so the private map below can reference it.
    struct VulkanDescriptorBufferSetEntry
    {
        DescriptorBufferAllocation alloc;
        VkDescriptorSetLayout      layout = VK_NULL_HANDLE;
        uint32_t                   frameSlot = 0;
        std::string                debugName;
        // Layout metadata for UpdateDescriptorSet validation; empty when
        // m_EnableDescriptorValidation is off. Deliberately NOT part of the per-draw
        // LookupDescriptorBufferSet POD view (audit §7.1.2 #3) — it is only copied
        // out on descriptor updates, which are far rarer than binds.
        DescriptorSetLayoutDesc    layoutDesc;
    };

  private:
    // DB-backed descriptor sets indexed by tagged handle id. Entries for a given
    // frameSlot are purged at BeginFrameReset(frameSlot) since their backing memory
    // is rewound. Entries with frameSlot == kPersistentFrameSlot live in the DB
    // pool's app-lifetime region and are never purged.
    //
    // shared_mutex so the hot-path lookup (per-draw in BindDescriptorSet) takes
    // a shared lock — many parallel-recording workers can resolve handles at
    // once. Writes (CreateDescriptorSet emplace, BeginFrameReset purge, swapchain
    // recreate purge) take the exclusive lock. See audit §7.1.2 item #3.
    mutable std::shared_mutex m_DescriptorBufferSetsMutex;
    std::unordered_map<uint64_t, VulkanDescriptorBufferSetEntry> m_DescriptorBufferSets;
    std::atomic<uint64_t> m_NextDescriptorBufferSetId{1};

  public:
    // Backend-agnostic gate (overrides IDevice): descriptor-buffer code paths are live
    // iff both the device reports the feature AND the env flag turned them on.
    bool IsDescriptorBufferEnabled() const override
    {
        return m_Capabilities.supportsDescriptorBuffer && m_UseDescriptorBuffer;
    }

    // Descriptor-buffer descriptor size lookups (populated by Phase-0 diagnostic query).
    // Callers (DescriptorBufferPool, tests) use these to compute layout sizes and write
    // the correct number of bytes per descriptor type.
    VkDeviceSize GetDescriptorBufferOffsetAlignment() const { return m_DescBufferDiag.descriptorBufferOffsetAlignment; }
    uint32_t GetMaxResourceDescriptorBufferBindings() const { return m_DescBufferDiag.maxResourceDescriptorBufferBindings; }
    uint32_t GetSamplerDescriptorSize() const { return m_DescBufferDiag.samplerDescriptorSize; }
    uint32_t GetCombinedImageSamplerDescriptorSize() const { return m_DescBufferDiag.combinedImageSamplerDescriptorSize; }
    uint32_t GetSampledImageDescriptorSize() const { return m_DescBufferDiag.sampledImageDescriptorSize; }
    uint32_t GetStorageImageDescriptorSize() const { return m_DescBufferDiag.storageImageDescriptorSize; }
    uint32_t GetUniformBufferDescriptorSize() const { return m_DescBufferDiag.uniformBufferDescriptorSize; }
    uint32_t GetStorageBufferDescriptorSize() const { return m_DescBufferDiag.storageBufferDescriptorSize; }

    // Shared descriptor-buffer pool. Null when feature disabled.
    VulkanDescriptorBufferPool* GetDescriptorBufferPool() const { return m_DescriptorBufferPool.get(); }

    // Unified DescriptorSetHandle scheme: the handle's id either casts back to a
    // VkDescriptorSet (pool-backed) or has the high bit set and references an entry
    // in m_DescriptorBufferSets (DB-backed). See CreateDescriptorSet for routing.
    static constexpr uint64_t kDescriptorBufferHandleTag = 1ULL << 63;
    static bool IsDescriptorBufferHandle(DescriptorSetHandle h)
    {
        return (h.id & kDescriptorBufferHandleTag) != 0;
    }

    // Thread-safe accessor. Returns a POD view of the entry (no std::string /
    // allocating fields) so the shared_lock-guarded copy is a pure memcpy —
    // critical because this is called per-draw from BindDescriptorSet. Earlier
    // shape returned the full VulkanDescriptorBufferSetEntry which heap-alloc'd
    // debugName on each bind when the name exceeded SSO. See audit §7.1.2 #3.
    // The bool is false when the handle is not a DB handle or is not in the map.
    struct DescriptorBufferSetLookup
    {
        DescriptorBufferAllocation alloc;
        VkDescriptorSetLayout      layout    = VK_NULL_HANDLE;
        uint32_t                   frameSlot = 0;
        bool                       Found     = false;
    };
    DescriptorBufferSetLookup LookupDescriptorBufferSet(DescriptorSetHandle handle);

    // The set's debug name, for diagnostics only. Deliberately separate from
    // LookupDescriptorBufferSet, which stays POD so the per-draw bind path never
    // copies a std::string; call this only on a path that is already failing.
    std::string DescriptorBufferSetDebugName(DescriptorSetHandle handle) const;

    // Allocate enough storage for one descriptor set of the given layout from the
    // descriptor-buffer pool. Returns an invalid allocation if DB is off. Asserts in
    // debug if the layout has UPDATE_AFTER_BIND bindings — DB layouts must be
    // created without that flag (vkGetDescriptorSetLayoutSizeEXT VUID).
    // When persistent=true, the memory is allocated from the pool's app-lifetime
    // region and is never rewound by BeginFrameReset. Intended for long-lived
    // descriptor sets like the bindless texture array.
    DescriptorBufferAllocation AllocateDescriptorSetBytes(VkDescriptorSetLayout layout, bool persistent = false);

    // Public accessor for the cached VkDescriptorSetLayout corresponding to a
    // DescriptorSetLayoutDesc. DB-eligible descs bucket separately (Phase 1c);
    // callers that need the raw handle to drive the descriptor-buffer path go
    // through this. Refcounts the cache entry on return — caller is responsible
    // for ReleaseDescriptorSetLayoutCached if they want to drop the ref.
    VkDescriptorSetLayout GetOrCreateSetLayoutHandle(const DescriptorSetLayoutDesc& desc);

    // Write one descriptor into an allocation at the binding's layout-computed offset.
    // `binding` is the binding index from the layout. Caller supplies a
    // VkDescriptorGetInfoEXT pre-populated with type + pImage/pBuffer/pSampler data.
    void WriteDescriptorAt(const DescriptorBufferAllocation& alloc,
                           VkDescriptorSetLayout layout,
                           uint32_t binding,
                           const VkDescriptorGetInfoEXT& getInfo,
                           size_t descriptorSize);

    // Type-specific wrapper: resolve tex/sampler to Vulkan handles, build a
    // combined-image-sampler VkDescriptorGetInfoEXT, forward to WriteDescriptorAt.
    // Returns false if resolution fails (invalid handles / view unavailable).
    bool WriteCombinedImageSamplerDescriptor(const DescriptorBufferAllocation& alloc,
                                             VkDescriptorSetLayout layout,
                                             uint32_t binding,
                                             TextureHandle texture,
                                             SamplerHandle sampler);

    // Translate an engine-level DescriptorSetUpdate into a DB descriptor write.
    // Used by UpdateDescriptorSet / UpdateDescriptorSetBatch when the handle is
    // DB-backed (see IsDescriptorBufferHandle).
    void WriteDescriptorBufferUpdate(DescriptorSetHandle handle, const DescriptorSetUpdate& update);

    // Descriptor-buffer function-pointer accessors. Null when feature disabled.
    PFN_vkGetDescriptorSetLayoutSizeEXT          FpGetDescriptorSetLayoutSizeEXT() const { return m_FpGetDescriptorSetLayoutSizeEXT; }
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT FpGetDescriptorSetLayoutBindingOffsetEXT() const { return m_FpGetDescriptorSetLayoutBindingOffsetEXT; }
    PFN_vkGetDescriptorEXT                       FpGetDescriptorEXT() const { return m_FpGetDescriptorEXT; }
    PFN_vkCmdBindDescriptorBuffersEXT            FpCmdBindDescriptorBuffersEXT() const { return m_FpCmdBindDescriptorBuffersEXT; }
    PFN_vkCmdSetDescriptorBufferOffsetsEXT       FpCmdSetDescriptorBufferOffsetsEXT() const { return m_FpCmdSetDescriptorBufferOffsetsEXT; }

    /// Cached VK_EXT_debug_utils label entry points. Callers check Available()
    /// once and then call through, rather than resolving per command.
    const DebugUtilsLabelFns& GetDebugUtilsLabelFns() const noexcept { return m_DebugUtilsLabelFns; }

    // GPU execution breadcrumbs (VK_NV_device_diagnostic_checkpoints). The CPU
    // marker ring says how far RECORDING got; checkpoints say how far EXECUTION
    // got, and the gap between them brackets the work that faulted.
    //
    // Emission is opt-in (GE_VK_GPU_CHECKPOINTS=1) because the marker sites are
    // on per-frame paths. Disabled, a marker pays one bool load for the gate
    // below and nothing else, so callers must gate on it rather than relying on
    // EmitGpuCheckpoint to bail out.
    bool GpuCheckpointsEnabled() const noexcept { return m_GpuCheckpointsEnabled; }
    void EmitGpuCheckpoint(VkCommandBuffer commandBuffer, const char* name);

    /// True when the extension is enabled and its entry points resolved,
    /// independent of whether emission was requested.
    bool GpuCheckpointsAvailable() const noexcept { return m_FpGetQueueCheckpointDataNV != nullptr; }

    /// The family's VkQueueFamilyCheckpointPropertiesNV::checkpointExecutionStageMask,
    /// or 0 when it reports none, was never probed, or is out of range. Zero means
    /// breadcrumbs are blind on every queue from that family — distinct from that
    /// queue having executed nothing.
    VkPipelineStageFlags CheckpointStagesForFamily(uint32_t family) const noexcept
    {
        return family < m_QueueFamilyCheckpointStages.size() ? m_QueueFamilyCheckpointStages[family] : 0u;
    }

    /// Markers recorded as GPU checkpoints so far; 0 when emission is disabled.
    uint64_t DebugGpuCheckpointsRecorded() const noexcept
    {
        return m_GpuCheckpointNames ? m_GpuCheckpointNames->RecordedCount() : 0;
    }

    /// The checkpoint report produced by the most recent device loss, or empty if
    /// no loss has been reported (or checkpoints were unavailable at the time).
    const std::string& LastGpuCheckpointReport() const noexcept { return m_LastGpuCheckpointReport; }

    /// True when VK_EXT_device_fault is enabled and vkGetDeviceFaultInfoEXT
    /// resolved. Checkpoints report only WHERE execution stopped; this reports
    /// WHAT the hardware faulted on, so the two answer different questions.
    bool DeviceFaultReportingAvailable() const noexcept { return m_FpGetDeviceFaultInfoEXT != nullptr; }

    /// The fault records retrieved on the most recent device loss, or empty if no
    /// loss has been reported (or the extension was unavailable at the time).
    const std::string& LastDeviceFaultReport() const noexcept { return m_LastDeviceFaultReport; }

  private:

    // Query every queue for the checkpoints the GPU last executed and log them
    // beside the CPU marker dump. Called on the device-lost edge only.
    void ReportGpuCheckpointsOnLoss();

    // Retrieve and log VK_EXT_device_fault records. Called on the device-lost
    // edge only, before any rebuild disposes the lost device, and only for a loss
    // the driver actually reported: VUID-vkGetDeviceFaultInfoEXT-device-07336
    // requires the device to be in the _lost_ state.
    void ReportDeviceFaultOnLoss(bool deviceActuallyLost);

    // Per-pass descriptor capture state (debug only, gated by atomic flag)
    std::atomic<bool> m_DescriptorCaptureEnabled{false};
    std::string m_DescriptorCapturePassFilter;
    std::vector<DescriptorCaptureEntry> m_DescriptorCaptures;
    uint32_t m_DescriptorCaptureDrawIndex = 0;

    // Configurable dynamic rendering toggle

    // Negative viewport height support (VK_KHR_maintenance1)

    bool m_SupportsNegativeViewportHeight = false;

    bool m_EnableDynamicRendering = true; // default on; can be driven from DeviceDesc

    // Config: allow gating bindless support even if the device supports it
    bool m_ForceDisableBindlessResources = false;

    bool m_SupportsDynamicRendering = false;

    std::unique_ptr<VulkanPipelineCache> m_VkDiskPipelineCache;
    // Backend-agnostic pipeline cache interface (wraps native Vulkan cache)
    std::unique_ptr<IPipelineCache> m_PipelineCacheIface;

    // Query pool for GPU profiling and performance analysis

    // Small cache for dynamic rendering VkFormat arrays (keyed by 64-bit signature)
    mutable std::unordered_map<uint64_t, std::vector<VkFormat>> m_DynRenderingFormatCache;
    mutable std::mutex m_DynRenderingFormatCacheMutex;

    // sync2 support cache
    bool m_SupportsSync2 = false;
    bool m_SupportsDrawIndirectCount = false;
    bool m_SupportsMultiDrawIndirect = false;
    static uint64_t HashFormats(const std::vector<uint32_t>& formats)
    {
        // 64-bit FNV-1a
        uint64_t h = 1469598103934665603ull;
        for (auto f : formats)
        {
            h ^= static_cast<uint64_t>(f);
            h *= 1099511628211ull;
        }
        return h;
    }

    const std::vector<VkFormat>& GetOrCacheVkFormats(const std::vector<uint32_t>& formats) const
    {
        if (formats.empty())
        {
            static const std::vector<VkFormat> kEmpty;
            return kEmpty;
        }
        uint64_t key = HashFormats(formats);
        {
            std::lock_guard<std::mutex> lock(m_DynRenderingFormatCacheMutex);
            auto it = m_DynRenderingFormatCache.find(key);
            if (it != m_DynRenderingFormatCache.end())
                return it->second;
        }
        std::vector<VkFormat> vkFormats;
        vkFormats.reserve(formats.size());
        for (auto v : formats)
            vkFormats.push_back(static_cast<VkFormat>(v));
        {
            std::lock_guard<std::mutex> lock(m_DynRenderingFormatCacheMutex);
            auto [it, inserted] = m_DynRenderingFormatCache.emplace(key, std::move(vkFormats));

            return it->second;
        }
    }

    std::unique_ptr<VulkanQueryPool> m_QueryPool;

    // Direct memory allocation tracking (for non-VMA allocations)
    std::vector<VkDeviceMemory> m_DirectMemoryAllocations;

    // Resource management
    std::unique_ptr<ResourceManager> m_ResourceManager;

    // Live resource tracking (ensures VMA-backed resources are destroyed before allocator shutdown)
    // Protected by m_ResourceTrackingMutex for thread-safe resource creation/destruction.
    //
    // Lock ordering (to prevent deadlocks):
    //   m_ResourceTrackingMutex < m_ThreadPoolsMutex < ThreadCommandPoolEntry::recycleMutex
    //   m_DeferredDestroyMutex < m_TextureViewMutex < m_ResourceTrackingMutex
    //   QueueSubmitContext's submit mutex < m_DeferredDestroyMutex (every submit path routes
    //     its lists under the queue lock; see m_SubmitContextStorage)
    //   m_BindlessMutex is independent (never held with any of the above)
    //   m_LayoutCacheMutex is independent
    mutable std::mutex m_ResourceTrackingMutex;
    std::vector<BufferHandle> m_LiveBuffers;
    std::vector<TextureHandle> m_LiveTextures;
    std::vector<TextureViewHandle> m_LiveTextureViews;
    std::vector<SamplerHandle> m_LiveSamplers;

    // Defensive VMA allocation registry (catches any untracked VMA-backed buffers/images)
    // Protected by m_ResourceTrackingMutex.
    std::unordered_map<void*, VkBuffer> m_VmaBufferAllocs; // VmaAllocation* -> VkBuffer
    std::unordered_map<void*, VkImage> m_VmaImageAllocs;   // VmaAllocation* -> VkImage

    // Shutdown guard to avoid double-shutdown side effects in some drivers/tests.
    // Atomic: read by the concurrent Destroy* entry points.
    std::atomic<bool> m_IsShutdown{false};
    // Q6 slice 2: true only while RebuildDevice tears down + rebuilds the
    // device-scoped objects. CleanupVulkan reads it to KEEP the shared VkInstance
    // (no refcount decrement, no vkDestroyInstance) since the same IDevice object
    // is being reused with a fresh VkDevice.
    // Atomic: read by the concurrent Destroy* entry points.
    std::atomic<bool> m_RebuildInProgress{false};
    // Teardown window: the only state in which resource destruction bypasses the
    // deferred queues. Both cases destroy device-scoped objects wholesale with no
    // frames left to protect — Shutdown() after the final drain, and RebuildDevice
    // between its bounded drain and the bringup of the replacement VkDevice.
    // Runtime code never satisfies this, so it can never reach the immediate
    // destruction helpers.
    bool InTeardown() const { return m_IsShutdown || m_RebuildInProgress; }
    // Set true after a full GPU drain (WaitForIdle / rebuild) until the next
    // successful queue submit. Gates the window-target retirement machinery, which
    // finalizes swapchain state that no fence or timeline value covers.
    // Atomic: SubmitTextureUploads clears it from worker threads.
    std::atomic<bool> m_DeviceKnownIdle{false};
    // Counts WaitForIdle() entries — every device-wide drain request, including the ones
    // EnsureGlobalGpuIdle short-circuits, because the policy forbids the CALL, not just the
    // queue wait. Main-thread only (WaitForIdle is not called off-thread), so a plain
    // integer is sufficient.
    uint64_t m_IdleDrainCount = 0;
    // Internal guard for WaitForIdle() bulk flush/compaction.
    // While true, per-resource O(N) live-vector removals are skipped.
    bool m_BulkDestroyInProgress = false;
    size_t m_NextLiveBufferBreakdownThreshold = 200000;

    // Clamp arbitrary sample requests to hardware-supported values.
    // `usageFlags` uses TextureUsage bits; pass 0 to use conservative color+depth intersection.
    uint32_t ResolveSupportedSampleCount(uint32_t requestedSamples, uint32_t usageFlags) const;
    void LogSampleCountFallbackIfNeeded(uint32_t requestedSamples, uint32_t resolvedSamples, uint32_t usageFlags) const;

    mutable std::mutex m_SampleCountWarningMutex;
    mutable std::unordered_set<uint64_t> m_LoggedSampleCountFallbacks;
};

// Struct definitions moved to top of file to fix template instantiation

} // namespace Rendering
} // namespace GameEngine
