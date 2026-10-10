/**
 * @file VulkanCommandList.cpp
 * @brief Vulkan implementation of CommandList
 */

#include "VulkanCommandList.h"
#include "VulkanDevice.h"
#include "VulkanMappings.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <mutex>
#include <vector>

#include "Logger/Logger.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/RenderingDebugFlags.h"

#include "TextureUsagePolicy.h"
#include "VulkanHandleHelpers.h"

namespace GameEngine
{
namespace Rendering
{

// Forward declaration for VulkanResourceUtils (defined in VulkanResources.cpp;
// no public header — mirrors the forward decl in VulkanDevice.cpp). Used to map
// the engine TextureFormat carried in SecondaryBeginInfo to the native VkFormat.
namespace VulkanResourceUtils
{
VkFormat GetVulkanFormat(TextureFormat format);
} // namespace VulkanResourceUtils

namespace
{
#if !defined(NDEBUG)
// Debug diagnostic for a render-graph barrier whose source stage the recording
// queue cannot express: log the first occurrence and every stride-th repeat, so a
// per-barrier condition cannot flood the log.
constexpr uint64_t kGraphSrcClampLogStride = 256;
#endif

// Map a VkImageLayout to the pipeline stage + access mask that should be used
// on the "src" side of a barrier transitioning OUT of that layout. Used when
// the previous layout is known from m_SubresourceLayouts tracking — guarantees
// the prior writer is flushed and ordered before the new use. UNDEFINED maps
// to TOP_OF_PIPE / 0 because the spec allows discarding contents.
struct SrcStageAccess
{
    VkPipelineStageFlags Stage;
    VkAccessFlags Access;
};

SrcStageAccess SrcMasksForOldLayout(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_UNDEFINED:
    case VK_IMAGE_LAYOUT_PREINITIALIZED:
        return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0};
    case VK_IMAGE_LAYOUT_GENERAL:
    default:
        // Conservative catch-all: wait for everything and make every write visible.
        return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT};
    }
}
} // namespace

VulkanCommandList::VulkanCommandList(VulkanDevice* device, IDevice::QueueType queue, VkCommandBuffer commandBuffer, VkCommandPool commandPool)
    : m_Device(device), m_CommandPool(commandPool)
{
    m_CommandBuffer = commandBuffer; // Command buffers now provided by per-frame pool
    m_QueueType = queue;
    // Q6 slice 4: stamp the thread-pool generation the buffer + owner belong to, so
    // a rebuild (which frees the pool + bumps the generation) is detectable before
    // this list recycles to a now-dead owner.
    m_PoolGeneration = m_Device ? m_Device->GetThreadPoolGeneration() : 0;
    // Enable dynamic rendering path based on device capabilities and config
    m_DynamicRenderingActive = (m_Device && m_Device->IsDynamicRenderingEnabled() && m_Device->SupportsDynamicRendering());
    // PERF: pre-reserve resource tracking to avoid repeated growth allocations.
    // A typical frame uses 16-64 resource entries across all render passes.
    m_UsedResources.reserve(64);
}

VulkanCommandList::~VulkanCommandList()
{
    // Q6 slice 4: after an in-place device rebuild, DestroyAllThreadPools freed the
    // pool this list's buffer + owner came from (and every framebuffer / render pass /
    // attachment view it cached is an OLD-device handle) and bumped the generation.
    // Touching ANY of them — destroying old-device objects on the fresh device, or
    // recycling the dead buffer to the freed owner's mutex — is a use-after-free.
    // Drop everything without touching the device.
    const bool poolIsStale = m_Device && m_Device->GetThreadPoolGeneration() != m_PoolGeneration;
    if (m_Device && !poolIsStale)
    {
        VkDevice vkDev = m_Device->GetVkDevice();
        for (VkFramebuffer fb : m_PendingFramebufferDestroys)
            vkDestroyFramebuffer(vkDev, fb, nullptr);
        for (VkRenderPass rp : m_PendingRenderPassDestroys)
            vkDestroyRenderPass(vkDev, rp, nullptr);
        ReleaseAttachmentViews(true);
    }
    m_PendingFramebufferDestroys.clear();
    m_PendingRenderPassDestroys.clear();
    if (m_CommandBuffer != VK_NULL_HANDLE && m_Device && !poolIsStale)
    {
        // Recycle to the owning thread pool if present, otherwise legacy per-frame pool.
        // Lock required: destructor may run on a different thread than the pool owner.
        if (m_ThreadPoolOwner)
        {
            auto* entry = static_cast<VulkanDevice::ThreadCommandPoolEntry*>(m_ThreadPoolOwner);
            int ord = 0;
            if (m_QueueType == IDevice::QueueType::Compute) ord = 1;
            else if (m_QueueType == IDevice::QueueType::Transfer) ord = 2;
            // Route to the matching level bucket: a secondary recycled as a primary
            // (or the reverse) is a level mismatch Vulkan rejects on next acquire.
            const int level = m_IsSecondary ? VulkanDevice::ThreadCommandPoolEntry::kLevelSecondary
                                            : VulkanDevice::ThreadCommandPoolEntry::kLevelPrimary;
            std::lock_guard<std::mutex> lock(entry->recycleMutex);
            entry->freeCmd[level][ord].push_back(m_CommandBuffer);
        }
        else
        {
            m_Device->RecycleCmdBuffer(m_QueueType, m_CommandBuffer);
        }
    }
    m_CommandBuffer = VK_NULL_HANDLE;
    m_CommandPool = VK_NULL_HANDLE;
}

void VulkanCommandList::ExecuteSecondaryCommandBuffers(const VkCommandBuffer* secondaries, uint32_t count)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || count == 0 || !secondaries) return;
    vkCmdExecuteCommands(m_CommandBuffer, count, secondaries);
}

// Abstract CommandList::BeginSecondary override — delegates to the
// Vulkan-specific BeginSecondary(SecondaryInheritance) with format conversion.
void VulkanCommandList::BeginSecondary(const SecondaryBeginInfo& info)
{
    // SecondaryBeginInfo carries engine TextureFormat values (see the struct doc:
    // "TextureFormat cast to uint32_t"); map each to the native VkFormat through
    // the same conversion texture creation uses, so the secondary's inheritance
    // rendering formats match the primary render pass's attachment formats. (A raw
    // static_cast<VkFormat> here is a bug — the two enums do not share numbering.)
    SecondaryInheritance inheritance{};
    inheritance.colorAttachmentCount = info.colorAttachmentCount;
    for (uint32_t i = 0; i < info.colorAttachmentCount; ++i)
        inheritance.colorFormats[i] =
            VulkanResourceUtils::GetVulkanFormat(static_cast<TextureFormat>(info.colorFormats[i]));
    // depthFormat == 0 is the "no depth" sentinel (mirrors PipelineFormatKey). This
    // branch is belt-and-suspenders: TextureFormat 0 is Unknown, which the format
    // table already maps to VK_FORMAT_UNDEFINED — the explicit sentinel just makes
    // the "no depth attachment inherited" intent obvious.
    inheritance.depthFormat =
        info.depthFormat == 0
            ? VK_FORMAT_UNDEFINED
            : VulkanResourceUtils::GetVulkanFormat(static_cast<TextureFormat>(info.depthFormat));
    inheritance.sampleCount = static_cast<VkSampleCountFlagBits>(info.sampleCount);
    inheritance.depthReadOnly = info.depthReadOnly;
    BeginSecondary(inheritance);
}

// Abstract CommandList::ExecuteSecondary override — extracts VkCommandBuffers
// from the VulkanCommandList wrappers and calls vkCmdExecuteCommands.
void VulkanCommandList::ExecuteSecondary(CommandList* const* secondaries, uint32_t count)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || count == 0 || !secondaries) return;
    std::vector<VkCommandBuffer> vkCBs(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        auto* vcl = static_cast<VulkanCommandList*>(secondaries[i]);
        vkCBs[i] = vcl->GetVkCommandBuffer();
    }
    vkCmdExecuteCommands(m_CommandBuffer, count, vkCBs.data());
}

void VulkanCommandList::BeginSecondary(const SecondaryInheritance& inheritance)
{
    if (m_CommandBuffer == VK_NULL_HANDLE) return;
    // BeginSecondary must only be called on secondary-level command buffers
    // (allocated via AcquireSecondaryCmdBuffer / AcquireThreadSecondaryCmdBuffer).
    // Calling this on a primary-level CB will produce a Vulkan validation error.

    m_IsSecondary = true;
    // The primary records no state for a pass filled by secondaries, so this is
    // the ONLY place the inherited render pass's read-only depth reaches the
    // depth-write-enable dynamic state (SetPipeline). Assigned unconditionally:
    // a recycled list must not carry the previous pass's value.
    m_DepthReadOnly = inheritance.depthReadOnly;

    VkResult resetResult = vkResetCommandBuffer(m_CommandBuffer, 0);
    if (resetResult != VK_SUCCESS) return;

    // Build inheritance rendering info for dynamic rendering
    VkCommandBufferInheritanceRenderingInfo inheritRendering{};
    inheritRendering.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
    inheritRendering.colorAttachmentCount = inheritance.colorAttachmentCount;
    inheritRendering.pColorAttachmentFormats = inheritance.colorFormats;
    inheritRendering.depthAttachmentFormat = inheritance.depthFormat;
    inheritRendering.stencilAttachmentFormat = inheritance.stencilFormat;
    inheritRendering.rasterizationSamples = inheritance.sampleCount;
    inheritRendering.viewMask = inheritance.viewMask;

    VkCommandBufferInheritanceInfo inheritInfo{};
    inheritInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritInfo.pNext = &inheritRendering;
    inheritInfo.renderPass = VK_NULL_HANDLE; // dynamic rendering
    inheritInfo.framebuffer = VK_NULL_HANDLE;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                    | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    beginInfo.pInheritanceInfo = &inheritInfo;

    VkResult result = vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);
    if (result == VK_SUCCESS)
    {
        m_IsRecording = true;
        m_HasBegun = true;
        m_HasEnded = false;
        m_UsedResources.clear();
        m_SubresourceLayouts.clear();
        m_LegacySwapchainAttachment = INVALID_HANDLE;
        m_BoundDescriptorSets.fill(VK_NULL_HANDLE);
        m_BoundDescriptorSetCount = 0;
        m_BoundDescriptorBuffers.clear();
        m_LastGraphicsLayout = VK_NULL_HANDLE;
        m_LastComputeLayout = VK_NULL_HANDLE;
    }
}

TextureViewHandle VulkanCommandList::GetOrCreateAttachmentView(TextureHandle texture,
                                                               const TextureViewDesc& desc)
{
    if (!m_Device)
        return {};

    // Tripwire: the key below must cover every identity field of TextureViewDesc
    // (all but debugName). A new/widened field that isn't keyed makes two distinct
    // descs alias one cache entry — a silently wrong attachment view.
    static_assert(sizeof(TextureViewDesc) == 56,
                  "TextureViewDesc changed — update AttachmentViewKey to cover the new field");

    AttachmentViewKey key;
    key.Texture = texture.id;
    key.FormatOverride = desc.formatOverride;
    key.Packed = ((static_cast<uint32_t>(desc.aspect) & 0xFFu) << 24) |
                 ((static_cast<uint32_t>(desc.viewType) & 0xFFu) << 16) |
                 ((static_cast<uint32_t>(desc.r) & 0xFu) << 12) |
                 ((static_cast<uint32_t>(desc.g) & 0xFu) << 8) |
                 ((static_cast<uint32_t>(desc.b) & 0xFu) << 4) |
                 (static_cast<uint32_t>(desc.a) & 0xFu);
    key.BaseMip = desc.baseMip;
    key.LevelCount = desc.levelCount;
    key.BaseLayer = desc.baseLayer;
    key.LayerCount = desc.layerCount;

    // Hits revalidate against the texture's CURRENT VkImage: swapchain handles
    // are byte-identical across swapchain recreation and the 8-bit handle
    // generation can wrap on a dormant command list — the image pointer is the
    // ground truth either way. A mismatch retires the stale view (deferred,
    // GPU-safe) and falls through to create.
    const VkImage image = m_Device->GetVkImage(texture);
    auto it = m_AttachmentViewCache.find(key);
    if (it != m_AttachmentViewCache.end())
    {
        if (image != VK_NULL_HANDLE && it->second.Image == image)
            return it->second.View;
        if (it->second.View.IsValid())
            m_Device->DestroyTextureView(it->second.View);
        m_AttachmentViewCache.erase(it);
    }

    TextureViewHandle view = m_Device->CreateTextureView(texture, desc);
    if (view.IsValid())
        m_AttachmentViewCache.emplace(key, AttachmentViewEntry{view, image});
    return view;
}

void VulkanCommandList::ReleaseAttachmentViews(bool destroyAll)
{
    if (!m_Device || m_AttachmentViewCache.empty())
        return;

    for (auto it = m_AttachmentViewCache.begin(); it != m_AttachmentViewCache.end();)
    {
        const TextureHandle tex{it->first.Texture};
        // Swapchain handles are synthetic (absent from the managed texture
        // registry, so IsTextureHandleLive is false even while live); keep their
        // entries — lookups revalidate them against the current VkImage.
        const bool live = m_Device->IsSwapchainTextureHandle(tex) ||
                          m_Device->IsTextureHandleLive(tex);
        if (destroyAll || !live)
        {
            if (it->second.View.IsValid())
                m_Device->DestroyTextureView(it->second.View);
            it = m_AttachmentViewCache.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void VulkanCommandList::OnSubmitted()
{
    ReleaseAttachmentViews(false);
    m_CommandBuffer = VK_NULL_HANDLE;
}

void VulkanCommandList::Begin()
{
    if (m_CommandBuffer == VK_NULL_HANDLE)
    {
        // Buffer was submitted (OnSubmitted nulled it) or not yet assigned.
        // Acquire a fresh one from the per-thread pool. The pool's fence wait
        // in VulkanDevice::BeginFrame() guarantees returned buffers are GPU-idle.
        if (m_Device)
        {
            m_CommandBuffer = m_Device->AcquireThreadCmdBuffer(m_QueueType);
        }
        if (m_CommandBuffer == VK_NULL_HANDLE)
        {
            return;
        }
    }

    VkResult resetResult = vkResetCommandBuffer(m_CommandBuffer, 0);
    if (resetResult != VK_SUCCESS)
    {
        return;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = 0;

    VkResult result = vkBeginCommandBuffer(m_CommandBuffer, &beginInfo);
    if (result == VK_SUCCESS)
    {
        m_IsRecording = true;
        m_HasBegun = true;
        m_HasEnded = false;
        m_UsedResources.clear();
        m_SubresourceLayouts.clear();
        m_LegacySwapchainAttachment = INVALID_HANDLE;
        m_BoundDescriptorSets.fill(VK_NULL_HANDLE);
        m_BoundDescriptorSetCount = 0;
        m_BoundDescriptorBuffers.clear();
        m_LastGraphicsLayout = VK_NULL_HANDLE;
        m_LastComputeLayout = VK_NULL_HANDLE;
        // Sweep dead-texture attachment views on reactivation too: a command
        // list whose previous recording was abandoned (no submit) or that sat
        // dormant would otherwise pin views for destroyed textures until its
        // next successful submit.
        ReleaseAttachmentViews(false);

        if (!m_PendingFramebufferDestroys.empty() || !m_PendingRenderPassDestroys.empty())
        {
            VkDevice vkDev = m_Device->GetVkDevice();
            for (VkFramebuffer fb : m_PendingFramebufferDestroys)
                vkDestroyFramebuffer(vkDev, fb, nullptr);
            m_PendingFramebufferDestroys.clear();
            for (VkRenderPass rp : m_PendingRenderPassDestroys)
                vkDestroyRenderPass(vkDev, rp, nullptr);
            m_PendingRenderPassDestroys.clear();
        }
    }
}

void VulkanCommandList::End()
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
    // Ensure any active rendering scopes are ended before finishing the command buffer
    if (m_DynamicRenderingBegun || m_LegacyRenderPassBegun)
    {
        EndRenderPass();
    }

    VkResult result = vkEndCommandBuffer(m_CommandBuffer);
    if (result == VK_SUCCESS)
    {
        m_IsRecording = false;
        m_HasEnded = true;
    }
}

void VulkanCommandList::MaybeCaptureDescriptorState()
{
    if (!m_Device || !m_Device->IsDescriptorCaptureEnabled())
        return;

    const auto& passFilter = m_Device->GetDescriptorCapturePassFilter();
    if (!passFilter.empty() && m_LastMarkerName != passFilter)
        return;

    uint32_t drawIndex = m_Device->NextDescriptorCaptureDrawIndex();

    for (uint32_t setIdx = 0; setIdx < m_BoundDescriptorSetCount; ++setIdx)
    {
        VkDescriptorSet vkSet = m_BoundDescriptorSets[setIdx];
        if (vkSet == VK_NULL_HANDLE)
            continue;

        VulkanDevice::DescriptorCaptureEntry entry;
        entry.PassName = m_LastMarkerName;
        entry.DrawIndex = drawIndex;
        entry.DescriptorSetHandle = reinterpret_cast<uint64_t>(vkSet);
        entry.SetIndex = setIdx;

        // Look up the layout desc for this descriptor set to enumerate bindings.
        DescriptorSetLayoutDesc layoutDesc{};
        {
            auto& layoutDescs = m_Device->m_DescriptorSetLayoutDescs;
            auto& layoutMutex = m_Device->m_DescriptorSetLayoutsMutex;
            std::lock_guard lock(layoutMutex);
            auto it = layoutDescs.find(vkSet);
            if (it != layoutDescs.end())
                layoutDesc = it->second;
        }

        for (const auto& binding : layoutDesc.bindings)
        {
            VulkanDevice::DescriptorBindingRecord rec;
            rec.Binding = binding.binding;

            switch (binding.type)
            {
            case DescriptorType::UniformBuffer:
                rec.ResourceType = "UniformBuffer";
                break;
            case DescriptorType::StorageBuffer:
                rec.ResourceType = "StorageBuffer";
                break;
            case DescriptorType::Texture:
                rec.ResourceType = "Texture";
                break;
            case DescriptorType::CombinedImageSampler:
                rec.ResourceType = "CombinedImageSampler";
                break;
            case DescriptorType::Sampler:
                rec.ResourceType = "Sampler";
                break;
            case DescriptorType::StorageImage:
                rec.ResourceType = "StorageImage";
                break;
            case DescriptorType::AccelerationStructure:
                rec.ResourceType = "AccelerationStructure";
                break;
            default:
                rec.ResourceType = "Unknown";
                break;
            }

            if (binding.debugName)
                rec.DebugName = binding.debugName;

            entry.Bindings.push_back(std::move(rec));
        }

        m_Device->PushDescriptorCapture(std::move(entry));
    }
}

#ifndef NDEBUG
namespace
{
const char* VkFormatName(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
    case VK_FORMAT_R16G16B16A16_SFLOAT:      return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_B8G8R8A8_UNORM:           return "B8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM:           return "R8G8B8A8_UNORM";
    default:                                 return "UNKNOWN";
    }
}
} // anonymous namespace
#endif

void VulkanCommandList::ValidatePipelineMatchesActiveScope(PipelineHandle pipeline)
{
#ifdef NDEBUG
    (void)pipeline;
#else
    if (m_CurrentBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS)
        return;
    if (!m_DynamicRenderingBegun)
        return;
    if (m_CurrentColorTargetCount == 0)
        return;

    const VulkanPipeline* vp = GetVulkanPipeline(pipeline);
    if (!vp || vp->colorAttachmentFormats.empty())
    {
        const char* dbg = vp ? vp->debugName.c_str() : "<null>";
        std::cerr << "[RG Assert] Pipeline='" << (dbg ? dbg : "")
                  << "' has no recorded color formats; Current targets: [";
        for (size_t i = 0; i < m_CurrentRenderingColorFormats.size(); ++i)
        {
            if (i) std::cerr << ", ";
            std::cerr << VkFormatName(m_CurrentRenderingColorFormats[i])
                      << "(" << (uint32_t)m_CurrentRenderingColorFormats[i] << ")";
        }
        std::cerr << "]\n";
        return;
    }

    bool mismatch = false;
    if (vp->colorAttachmentFormats.size() != m_CurrentColorTargetCount)
    {
        mismatch = true;
        std::cerr << "[RG Assert] DynamicRendering: pipeline color attachment count ("
                  << vp->colorAttachmentFormats.size() << ") differs from current render target count ("
                  << m_CurrentColorTargetCount << ")";
        if (!m_LastMarkerName.empty())
            std::cerr << " in pass '" << m_LastMarkerName << "'";
        std::cerr << "\n";
    }
    else if (vp->colorAttachmentFormats.size() != m_CurrentRenderingColorFormats.size())
    {
        mismatch = true;
        std::cerr << "[RG Assert] DynamicRendering: internal color format vector size mismatch (pipeline="
                  << vp->colorAttachmentFormats.size() << ", current=" << m_CurrentRenderingColorFormats.size() << ")\n";
    }
    else
    {
        for (size_t i = 0; i < vp->colorAttachmentFormats.size(); ++i)
        {
            if (vp->colorAttachmentFormats[i] != m_CurrentRenderingColorFormats[i])
            {
                mismatch = true;
                std::cerr << "[RG Assert] DynamicRendering: color format mismatch at slot " << i
                          << ": pipeline=" << (uint32_t)vp->colorAttachmentFormats[i]
                          << " (" << VkFormatName(vp->colorAttachmentFormats[i]) << ")"
                          << ", current=" << (uint32_t)m_CurrentRenderingColorFormats[i]
                          << " (" << VkFormatName(m_CurrentRenderingColorFormats[i]) << ")\n";
            }
        }
    }

    if (!mismatch)
        return;

    const char* dbg = vp->debugName.c_str();
    std::cerr << "[RG Assert] Pipeline='" << (dbg ? dbg : "")
              << "' handle=" << (uint64_t)pipeline
              << " pipelineColorCount=" << vp->colorAttachmentFormats.size()
              << " currentColorCount=" << m_CurrentRenderingColorFormats.size()
              << " formats: [";
    for (size_t i = 0; i < vp->colorAttachmentFormats.size(); ++i)
    {
        if (i) std::cerr << ", ";
        std::cerr << VkFormatName(vp->colorAttachmentFormats[i])
                  << "(" << (uint32_t)vp->colorAttachmentFormats[i] << ")";
    }
    std::cerr << "] vs Current: [";
    for (size_t i = 0; i < m_CurrentRenderingColorFormats.size(); ++i)
    {
        if (i) std::cerr << ", ";
        std::cerr << VkFormatName(m_CurrentRenderingColorFormats[i])
                  << "(" << (uint32_t)m_CurrentRenderingColorFormats[i] << ")";
    }
    std::cerr << "]\n";

    assert(false && "DynamicRendering format mismatch: check console output for details");
#endif
}

void VulkanCommandList::SetPipeline(PipelineHandle pipeline)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    VkPipeline vkPipeline = m_Device->GetVkPipeline(pipeline);
    if (vkPipeline != VK_NULL_HANDLE)
    {
        PipelineType pipelineType = m_Device->GetPipelineType(pipeline);
        VkPipelineBindPoint bindPoint = (pipelineType == PipelineType::Compute) ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;

        m_CurrentBindPoint = bindPoint;
        m_CurrentPipelineLayout = m_Device->GetPipelineLayout(pipeline);
        m_CurrentPipelineHandle = pipeline;
        m_Device->SetCurrentPipelineHandle(pipeline);

// Debug-only: verify pipeline rasterizationSamples vs current color attachment samples if inside a render pass
#ifndef NDEBUG
        if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS && m_DynamicRenderingBegun && m_CurrentColorAttachment != INVALID_HANDLE)
        {
            // Retrieve pipeline rasterization samples from pipeline object (stored at creation time)
            uint32_t pipelineSamples = 1;
            if (const VulkanPipeline* pl = GetVulkanPipeline(pipeline))
            {
                pipelineSamples = pl->rasterizationSamples ? pl->rasterizationSamples : 1;
            }
            uint32_t attachmentSamples = static_cast<uint32_t>(m_CurrentColorSamples);
            if (attachmentSamples == 0)
                attachmentSamples = 1;
            // If color attachment is multisampled (>1), pipeline must match
            // If color attachment is single-sample, pipeline should be 1 sample
            if (!((attachmentSamples == 1 && pipelineSamples == 1) || (attachmentSamples > 1 && pipelineSamples == attachmentSamples)))
            {

                assert(false && "Pipeline rasterizationSamples must match color attachment sample count");
            }
        }
#endif
        ValidatePipelineMatchesActiveScope(pipeline);

        // Debug: count pipeline binds per frame
        if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
            m_Device->DebugIncPipelineBindGraphics();
        else
            m_Device->DebugIncPipelineBindCompute();

        vkCmdBindPipeline(m_CommandBuffer, bindPoint, vkPipeline);

        // Set dynamic depth write enable per-pipeline (Vulkan 1.3+).
        // Read-only depth passes force OFF: it is what makes the pass actually
        // write nothing, and therefore what makes its read-only layout legal and
        // its VK_ATTACHMENT_STORE_OP_NONE content-preserving.
        // Non-read-only passes respect each pipeline's compiled depthWriteEnable.
        // A secondary command buffer (A2.4-P0-R parallel record) inherits the
        // primary's dynamic render pass but never runs BeginRenderPass, so it must
        // set this dynamic state itself — dynamic state does not cross
        // vkCmdExecuteCommands, and BeginRenderPass records none of it on the
        // primary when the pass is filled by secondaries. m_DepthReadOnly gets
        // there through SecondaryBeginInfo::depthReadOnly (RGFrame's fork phase
        // reads it off the same RenderPassDesc the primary is begun with); a
        // forked pass that attaches depth read-only is not hypothetical.
        if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
            && (m_DynamicRenderingBegun || m_IsSecondary)
            && m_Device->GetCmdSetDepthWriteEnable())
        {
            auto setDepthWriteEnable = m_Device->GetCmdSetDepthWriteEnable();
            if (m_DepthReadOnly)
            {
                setDepthWriteEnable(m_CommandBuffer, VK_FALSE);
            }
            else if (const VulkanPipeline* vp = GetVulkanPipeline(pipeline))
            {
                setDepthWriteEnable(m_CommandBuffer, vp->depthWriteEnable ? VK_TRUE : VK_FALSE);
            }
        }

        // Refined policy: only invalidate descriptor cache if pipeline layout changed
        VkPipelineLayout& lastLayout = (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) ? m_LastGraphicsLayout : m_LastComputeLayout;
        if (lastLayout != m_CurrentPipelineLayout)
        {
            m_BoundDescriptorSets.fill(VK_NULL_HANDLE);
            m_BoundDescriptorSetCount = 0;
            lastLayout = m_CurrentPipelineLayout;
        }
    }
    else
    {
        // The bind cannot happen, so forget whatever was tracked from a previous
        // SetPipeline: otherwise a following Dispatch/Draw passes the "a pipeline is
        // bound" test and silently runs against the WRONG pipeline. A dead-but-
        // IsValid() handle here is exactly what an in-place device rebuild leaves in
        // any cache that outlived it. Every consumer of m_CurrentPipelineLayout
        // already early-returns on VK_NULL_HANDLE, so clearing is safe.
        m_CurrentPipelineHandle = PipelineHandle{};
        m_CurrentPipelineLayout = VK_NULL_HANDLE;
        if (!m_WarnedNullPipelineBind)
        {
            m_WarnedNullPipelineBind = true;
            Logger::Log::Error("VulkanCommandList: SetPipeline(handle={}) resolved to a null VkPipeline — bind "
                               "skipped and the tracked binding cleared. Last marker/pass='{}'",
                               pipeline.id, m_LastMarkerName);
        }
    }
}

void VulkanCommandList::SetVertexBuffer(BufferHandle buffer, uint32_t slot)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

#ifndef NDEBUG
    // Debug helper: detect binding buffers as vertex buffers without Vertex usage
    if (VulkanBuffer* vb = GetVulkanBuffer(buffer))
    {
        const uint32_t usageBits = static_cast<uint32_t>(vb->usage);
        if ((usageBits & static_cast<uint32_t>(BufferUsage::Vertex)) == 0u)
        {
            std::cerr << "VulkanCommandList: SetVertexBuffer binding buffer without Vertex usage. "
                      << "handle=" << buffer
                      << ", debugName='" << vb->debugName << "', usageBits=0x" << std::hex << usageBits << std::dec
                      << std::endl;
        }
    }
#endif

    VkBuffer vkBuffer = m_Device->GetVkBuffer(buffer);
    if (vkBuffer != VK_NULL_HANDLE)
    {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(m_CommandBuffer, slot, 1, &vkBuffer, &offset);
    }
}

void VulkanCommandList::SetIndexBuffer(BufferHandle buffer, IndexType indexType)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    VkBuffer vkBuffer = m_Device->GetVkBuffer(buffer);
    if (vkBuffer != VK_NULL_HANDLE)
    {
        VkIndexType vkIndexType = (indexType == IndexType::Uint32) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        vkCmdBindIndexBuffer(m_CommandBuffer, vkBuffer, 0, vkIndexType);
    }
}

VkShaderStageFlags VulkanCommandList::ResolvePushConstantStageFlags(
    const VulkanPipeline* pipeline, VkShaderStageFlags metadataStages) const
{
    VkShaderStageFlags resolved;
    if (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE)
    {
        // Compute layouts carry exactly a COMPUTE_BIT range
        // (VUID-VkPushConstantRange-stageFlags-01304 enforcement at creation).
        resolved = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    else if (!pipeline || pipeline->type == PipelineType::Graphics)
    {
        // Classic graphics layouts carry the canonical range — the push flags
        // must match it exactly (see kGraphicsPushConstantStages).
        resolved = kGraphicsPushConstantStages;
    }
    else
    {
        // Mesh: mirrors the mesh layout construction (reflected mask, ALL when
        // the desc declared none). Never strip to ALL_GRAPHICS here — that
        // would drop the task/mesh bits the layout's range actually declares.
        resolved = metadataStages ? metadataStages
                                  : static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_ALL);
    }
    assert((!pipeline || pipeline->layoutPushStageFlags == 0 ||
            resolved == static_cast<VkShaderStageFlags>(pipeline->layoutPushStageFlags)) &&
           "push stage flags must exactly match the pipeline layout's declared range");
    return resolved;
}

void VulkanCommandList::SetConstants(uint32_t slot, size_t size, const void* data)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    if (!data || slot != 0)
    {
        return;
    }

    VkPipelineLayout pipelineLayout = m_CurrentPipelineLayout;
    if (pipelineLayout == VK_NULL_HANDLE)
    {
        return;
    }

    // Determine the pipeline's declared push constant layout (size/mask)
    uint32_t declaredSize = 0;
    VkShaderStageFlags declaredStages = static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_ALL);
    if (const VulkanPipeline* pl = GetVulkanPipeline(m_CurrentPipelineHandle))
    {
        declaredSize = pl->pushConstantSize;
        if (pl->pushConstantStagesMask != 0)
        {
            declaredStages = pl->pushConstantStagesMask;
        }
    }

    // Enforce policy: DeviceDesc-configured limit when no declared size is present; otherwise enforce declared size
    const uint32_t policyMax = m_Device->GetPolicyMaxPushConstantsSize();
    const uint32_t effectiveMax = declaredSize ? declaredSize : policyMax;
    if (size > effectiveMax)
    {
        assert(false && "Push constants exceed declared/effective limit; use a UBO.");
        return;
    }
    // Enforce device limit for safety
    const uint32_t deviceLimit = m_Device->GetMaxPushConstantsSize();
    if (size > deviceLimit)
    {
        assert(false && "Push constants exceed device maxPushConstantsSize");
        return;
    }
    // Determine stage flags from actual declared ranges when available; otherwise fallback.
    const VulkanPipeline* pl = GetVulkanPipeline(m_CurrentPipelineHandle);
    if (pl && !pl->pushRanges.empty())
    {
        VkShaderStageFlags stages = 0;
        const uint32_t writeStart = 0;
        const uint32_t writeEnd = static_cast<uint32_t>(size);
        for (const auto& r : pl->pushRanges)
        {
            const uint32_t rStart = r.offset;
            const uint32_t rEnd = r.offset + r.size;
            const bool overlap = (writeStart < rEnd) && (rStart < writeEnd);
            if (overlap)
            {
                VkShaderStageFlags mask = r.stagesMask;
                if (mask == 0)
                {
                    mask = static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_ALL);
                }
                stages |= mask;
            }
        }
        if (stages == 0)
        {
            // No overlapping declared range => nothing to push for this pipeline layout
            return;
        }
        stages = ResolvePushConstantStageFlags(pl, stages);
        vkCmdPushConstants(m_CommandBuffer, pipelineLayout, stages, 0, static_cast<uint32_t>(size), data);
    }
    else
    {
        // If pipeline declared no ranges and no size, skip to avoid validation error
        if (declaredSize == 0)
        {
            return;
        }
        const VkShaderStageFlags stageFlags = ResolvePushConstantStageFlags(pl, declaredStages);
        vkCmdPushConstants(m_CommandBuffer, pipelineLayout, stageFlags, 0, static_cast<uint32_t>(size), data);
    }
}

void VulkanCommandList::SetConstants(uint32_t slot, uint32_t offset, size_t size, const void* data)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
    if (!data || slot != 0)
    {
        return;
    }
    VkPipelineLayout pipelineLayout = m_CurrentPipelineLayout;
    if (pipelineLayout == VK_NULL_HANDLE)
    {
        return;
    }
    // Determine declared push constant size/stages
    uint32_t declaredSize = 0;
    VkShaderStageFlags declaredStages = static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_ALL);
    if (const VulkanPipeline* pl = GetVulkanPipeline(m_CurrentPipelineHandle))
    {
        declaredSize = pl->pushConstantSize;
        if (pl->pushConstantStagesMask != 0)
        {
            declaredStages = pl->pushConstantStagesMask;
        }
    }
    const uint32_t policyMax = m_Device->GetPolicyMaxPushConstantsSize();
    const uint32_t effectiveMax = declaredSize ? declaredSize : policyMax;
    // Bounds check: offset+size within limit
    if (size > effectiveMax || (offset + size) > effectiveMax)
    {
        assert(false && "Push constants exceed declared/effective limit (offset)");
        return;
    }
    const uint32_t deviceLimit = m_Device->GetMaxPushConstantsSize();
    if ((offset + size) > deviceLimit)
    {
        assert(false && "Push constants exceed device maxPushConstantsSize (offset)");
        return;
    }
    // Prefer declared push constant ranges to derive exact stages for the written span
    const VulkanPipeline* pl = GetVulkanPipeline(m_CurrentPipelineHandle);
    if (pl && !pl->pushRanges.empty())
    {
        VkShaderStageFlags stages = 0;
        const uint32_t writeStart = offset;
        const uint32_t writeEnd = offset + static_cast<uint32_t>(size);
        for (const auto& r : pl->pushRanges)
        {
            const uint32_t rStart = r.offset;
            const uint32_t rEnd = r.offset + r.size;
            const bool overlap = (writeStart < rEnd) && (rStart < writeEnd);
            if (overlap)
            {
                VkShaderStageFlags mask = r.stagesMask;
                if (mask == 0)
                {
                    mask = static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_ALL);
                }
                stages |= mask;
            }
        }
        if (stages == 0)
        {
            // No overlap with declared ranges => skip to avoid validation error
            return;
        }
        stages = ResolvePushConstantStageFlags(pl, stages);
        vkCmdPushConstants(m_CommandBuffer, pipelineLayout, stages, offset, static_cast<uint32_t>(size), data);
    }
    else
    {
        // No declared ranges: if pipeline size is 0, layout has no PC ranges -> skip
        if (declaredSize == 0)
        {
            return;
        }
        const VkShaderStageFlags stageFlags = ResolvePushConstantStageFlags(pl, declaredStages);
        vkCmdPushConstants(m_CommandBuffer, pipelineLayout, stageFlags, offset, static_cast<uint32_t>(size), data);
    }
}

bool VulkanCommandList::SetPushConstantsByName(const char* rangeName, const void* data, size_t size, uint32_t offset)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
        return false;
    if (!data || !rangeName || !*rangeName)
        return false;
    VkPipelineLayout pipelineLayout = m_CurrentPipelineLayout;
    if (pipelineLayout == VK_NULL_HANDLE)
        return false;
    const VulkanPipeline* pl = GetVulkanPipeline(m_CurrentPipelineHandle);
    if (!pl)
        return false;
    // Find range by name
    uint32_t rangeOffset = 0, rangeSize = 0;
    VkShaderStageFlags rangeStages = 0;
    bool found = false;
    for (const auto& r : pl->pushRanges)
    {
        if (r.name == rangeName)
        {
            rangeOffset = r.offset;
            rangeSize = r.size;
            rangeStages = r.stagesMask;
            found = true;
            break;
        }
    }
    if (!found)
        return false;
    // Bounds checks: declared range size is the contract
    if ((offset + size) > rangeSize)
    {
        assert(false && "Push constants write exceeds declared range size");
        return false;
    }
    // Device/policy limits
    const uint32_t deviceLimit = m_Device->GetMaxPushConstantsSize();
    if ((rangeOffset + offset + size) > deviceLimit)
    {
        assert(false && "Push constants exceed device max");
        return false;
    }
    rangeStages = ResolvePushConstantStageFlags(pl, rangeStages);
    vkCmdPushConstants(m_CommandBuffer, pipelineLayout, rangeStages, rangeOffset + offset, static_cast<uint32_t>(size), data);
    return true;
}

bool VulkanCommandList::SetPushConstantsById(uint32_t rangeId, const void* data, size_t size, uint32_t offset)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
        return false;
    if (!data)
        return false;
    VkPipelineLayout pipelineLayout = m_CurrentPipelineLayout;
    if (pipelineLayout == VK_NULL_HANDLE)
        return false;

    const VulkanPipeline* pl = GetVulkanPipeline(m_CurrentPipelineHandle);
    if (!pl)
        return false;
    if (rangeId >= pl->pushRanges.size())
        return false;

    const auto& r = pl->pushRanges[rangeId];
    if ((offset + size) > r.size)
    {
        assert(false && "Push constants write exceeds declared range size");
        return false;
    }
    const uint32_t deviceLimit = m_Device->GetMaxPushConstantsSize();
    if ((r.offset + offset + size) > deviceLimit)
    {
        assert(false && "Push constants exceed device max");
        return false;
    }
    const VkShaderStageFlags stages = ResolvePushConstantStageFlags(pl, r.stagesMask);
    vkCmdPushConstants(m_CommandBuffer, pipelineLayout, stages, r.offset + offset, static_cast<uint32_t>(size), data);

    return true;
}

void VulkanCommandList::BindDescriptorSet(uint32_t set, DescriptorSetHandle descriptorSet, PipelineHandle pipeline)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
    if (!descriptorSet.IsValid() || !pipeline.IsValid())
    {
        return;
    }
    VkPipelineLayout vkLayout = m_Device->GetPipelineLayout(pipeline);
    if (vkLayout == VK_NULL_HANDLE)
    {
        return;
    }
    VkPipelineBindPoint bindPoint = m_CurrentBindPoint;

    // Descriptor-buffer handle? Bind the backing buffer (on first use in this command
    // buffer, or when the buffer differs from what's currently bound) then set the
    // per-set offset.
    if (VulkanDevice::IsDescriptorBufferHandle(descriptorSet))
    {
        const auto lookup = m_Device->LookupDescriptorBufferSet(descriptorSet);
        if (!lookup.Found || !lookup.alloc.IsValid())
            return;
        auto fpBind = m_Device->FpCmdBindDescriptorBuffersEXT();
        auto fpOff  = m_Device->FpCmdSetDescriptorBufferOffsetsEXT();
        if (!fpBind || !fpOff) return;

        // Locate this allocation's backing buffer among those already bound; if new,
        // append it and re-issue the bind with the full array. Appended-only ordering
        // keeps every earlier set's buffer index stable, so a set placed in a spill
        // block cannot shift an earlier set's descriptors out from under its offset —
        // the single-buffer bind this replaced hardcoded index 0 for every set, which
        // corrupted any draw whose sets straddled the primary region and a spill block.
        uint32_t bufIdx = 0;
        bool found = false;
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_BoundDescriptorBuffers.size()); ++i)
        {
            if (m_BoundDescriptorBuffers[i].Buffer == lookup.alloc.Buffer)
            {
                bufIdx = i;
                found = true;
                break;
            }
        }
        if (!found)
        {
            // vkCmdBindDescriptorBuffersEXT must not bind more than
            // maxResourceDescriptorBufferBindings buffers (spec floor 3, desktop ~8);
            // an over-count bind is itself a device-lost. The descriptor-buffer pool
            // spills a new VkBuffer per region overflow with no block cap — from either
            // the persistent or a per-frame region — so a heavy command buffer could in
            // principle exceed the device limit. If appending would, refuse loudly and
            // leave this set unbound — its draw reads a stale/wrong set (a visible
            // artifact) rather than the whole device faulting. The remedy when this
            // fires is to stop the pool spilling; its own overflow warning names the
            // exhausted region and the remedy that fits it, which differs per region.
            const uint32_t deviceMax = m_Device->GetMaxResourceDescriptorBufferBindings();
            const uint32_t maxBindings = deviceMax != 0u ? deviceMax : 3u; // spec floor if unqueried
            if (m_BoundDescriptorBuffers.size() >= maxBindings)
            {
                Logger::Log::Error(
                    "VulkanCommandList: command buffer already binds {} descriptor buffers "
                    "(device limit {}); set {} left unbound to avoid an illegal bind. Every "
                    "VulkanDescriptorBufferPool spill block costs one of these slots — its "
                    "overflow warning names the region that is spilling and the remedy for it.",
                    m_BoundDescriptorBuffers.size(), maxBindings, set);
                return;
            }
            // block-base device address = allocation address minus its within-block offset
            m_BoundDescriptorBuffers.push_back(
                {lookup.alloc.Buffer, lookup.alloc.Address - lookup.alloc.Offset});
            bufIdx = static_cast<uint32_t>(m_BoundDescriptorBuffers.size() - 1u);

            std::vector<VkDescriptorBufferBindingInfoEXT> infos(m_BoundDescriptorBuffers.size());
            for (size_t i = 0; i < m_BoundDescriptorBuffers.size(); ++i)
            {
                infos[i].sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
                infos[i].address = m_BoundDescriptorBuffers[i].BlockBaseAddress;
                infos[i].usage   = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
            }
            fpBind(m_CommandBuffer, static_cast<uint32_t>(infos.size()), infos.data());
        }
        const VkDeviceSize offset = lookup.alloc.Offset;
        fpOff(m_CommandBuffer, bindPoint, vkLayout, set, 1, &bufIdx, &offset);

        if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
            m_Device->DebugIncDescriptorBindGraphics(set);
        else
            m_Device->DebugIncDescriptorBindCompute(set);
        if (set < m_BoundDescriptorSets.size())
        {
            m_BoundDescriptorSets[set] = VK_NULL_HANDLE; // not a VkDescriptorSet
            m_BoundSetPipelines[set] = pipeline;
            uint32_t desired = set + 1;
            if (m_BoundDescriptorSetCount < desired)
                m_BoundDescriptorSetCount = desired;
        }
        return;
    }

    // Legacy pool path
    VkDescriptorSet vkSet = m_Device->GetVkDescriptorSet(descriptorSet);
    if (vkSet == VK_NULL_HANDLE)
    {
        return;
    }
    vkCmdBindDescriptorSets(
        m_CommandBuffer,
        bindPoint,
        vkLayout,
        set,
        1,
        &vkSet,
        0,
        nullptr);
    // Per the Vulkan spec (VU for vkCmdBindDescriptorSets): a legacy pool-path
    // bind invalidates the command buffer's descriptor-buffer bindings. Drop our
    // cached handle so the next DB BindDescriptorSet on this cmdlist re-issues
    // vkCmdBindDescriptorBuffersEXT — otherwise the subsequent
    // vkCmdSetDescriptorBufferOffsetsEXT fires VUID-...-08065 (no DB bound).
    m_BoundDescriptorBuffers.clear();
    // Debug: count descriptor binds per frame
    if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
        m_Device->DebugIncDescriptorBindGraphics(set);
    else
        m_Device->DebugIncDescriptorBindCompute(set);
    // Cache for potential rebind on pipeline switches
    if (set < m_BoundDescriptorSets.size())
    {
        m_BoundDescriptorSets[set] = vkSet;
        m_BoundSetPipelines[set] = pipeline;
        // Maintain contiguous count from 0
        if (vkSet != VK_NULL_HANDLE)
        {
            uint32_t desired = set + 1;
            if (m_BoundDescriptorSetCount < desired)
                m_BoundDescriptorSetCount = desired;
        }
    }

    if (GameEngine::Rendering::RenderingDebugFlags::Get().srgbDiag && m_LastMarkerName == "FinalSRGBEncode")
    {
        Logger::Log::Info(
            "[SRGBEncodeBindDiag] marker='{}' cmdBuf={} set={} vkSet={} pipelineHandle={} vkLayout={}",
            m_LastMarkerName,
            reinterpret_cast<uint64_t>(m_CommandBuffer),
            set,
            reinterpret_cast<uint64_t>(vkSet),
            static_cast<uint64_t>(pipeline),
            reinterpret_cast<uint64_t>(vkLayout));
    }
}

void VulkanCommandList::GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight)
{
    outWidth = 0;
    outHeight = 0;
    VkExtent3D extent = m_Device->GetVkImageExtent(texture);
    outWidth = extent.width;
    outHeight = extent.height;
}

void VulkanCommandList::BindDescriptorBuffers(const DescriptorBufferBinding* bindings, uint32_t count)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
        return;
    if (!m_Device->IsDescriptorBufferEnabled())
        return;
    auto fp = m_Device->FpCmdBindDescriptorBuffersEXT();
    if (!fp || count == 0 || bindings == nullptr)
        return;

    // Spec minimum for maxResourceDescriptorBufferBindings is 3; most desktop drivers expose 8.
    // Cap at 8 defensively and log when a caller exceeds that — silent truncation would
    // produce very confusing draw-call behaviour.
    constexpr uint32_t kMaxBindings = 8;
    const uint32_t n = count < kMaxBindings ? count : kMaxBindings;
    if (count > kMaxBindings)
    {
        Logger::Log::Error("VulkanCommandList::BindDescriptorBuffers: caller supplied {} bindings; truncating to {}",
                           count, kMaxBindings);
    }
    VkDescriptorBufferBindingInfoEXT infos[kMaxBindings]{};
    for (uint32_t i = 0; i < n; ++i)
    {
        infos[i].sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
        infos[i].address = static_cast<VkDeviceAddress>(bindings[i].address);
        infos[i].usage   = (bindings[i].kind == DescriptorBufferKind::Sampler)
                               ? VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT
                               : VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
    }
    fp(m_CommandBuffer, n, infos);
}

void VulkanCommandList::SetDescriptorBufferOffsets(PipelineHandle pipeline,
                                                   uint32_t firstSet,
                                                   const uint32_t* bufferIndices,
                                                   const uint64_t* offsets,
                                                   uint32_t setCount)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
        return;
    if (!m_Device->IsDescriptorBufferEnabled())
        return;
    auto fp = m_Device->FpCmdSetDescriptorBufferOffsetsEXT();
    if (!fp || setCount == 0 || bufferIndices == nullptr || offsets == nullptr)
        return;

    VkPipelineLayout vkLayout = m_Device->GetPipelineLayout(pipeline);
    if (vkLayout == VK_NULL_HANDLE)
        return;

    // Translate uint64_t offsets to VkDeviceSize in a small stack buffer to avoid an alloc.
    constexpr uint32_t kMaxSets = 8;
    VkDeviceSize vkOffsets[kMaxSets]{};
    const uint32_t n = setCount < kMaxSets ? setCount : kMaxSets;
    for (uint32_t i = 0; i < n; ++i)
        vkOffsets[i] = static_cast<VkDeviceSize>(offsets[i]);

    fp(m_CommandBuffer,
       m_CurrentBindPoint,
       vkLayout,
       firstSet,
       n,
       bufferIndices,
       vkOffsets);
}

void VulkanCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

#ifndef NDEBUG
    // Must be inside a render pass/rendering scope to draw
    assert(m_DynamicRenderingBegun || m_LegacyRenderPassBegun);
    // Sanity: ensure a graphics pipeline is bound when drawing graphics
    if (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
    {
        if (m_CurrentPipelineLayout == VK_NULL_HANDLE)
        {
            std::cerr << "[RG Assert] Draw without pipeline. Last marker/pass='" << m_LastMarkerName << "'\n";
            assert(false && "Draw called without a graphics pipeline bound (SetPipeline missing in this pass)");
        }
    }
#endif
    // Validate dynamic rendering color formats at draw time as well
    ValidatePipelineMatchesActiveScope(m_CurrentPipelineHandle);

    if (GameEngine::Rendering::RenderingDebugFlags::Get().srgbDiag && m_LastMarkerName == "FinalSRGBEncode")
    {
        std::string boundSets;
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_BoundDescriptorSets.size()); ++i)
        {
            if (m_BoundDescriptorSets[i] == VK_NULL_HANDLE)
                continue;
            if (!boundSets.empty())
                boundSets += ", ";
            boundSets += std::to_string(i);
            boundSets += ":";
            boundSets += std::to_string(reinterpret_cast<uint64_t>(m_BoundDescriptorSets[i]));
        }
        Logger::Log::Info(
            "[SRGBEncodeDrawDiag] marker='{}' cmdBuf={} pipelineHandle={} vkLayout={} boundSets=[{}]",
            m_LastMarkerName,
            reinterpret_cast<uint64_t>(m_CommandBuffer),
            static_cast<uint64_t>(m_CurrentPipelineHandle),
            reinterpret_cast<uint64_t>(m_CurrentPipelineLayout),
            boundSets.empty() ? "<none>" : boundSets);
    }

    MaybeCaptureDescriptorState();
    vkCmdDraw(m_CommandBuffer, vertexCount, instanceCount, 0, 0);
}

void VulkanCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount,
                             uint32_t firstVertex, uint32_t firstInstance)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
#ifndef NDEBUG
    assert(m_DynamicRenderingBegun || m_LegacyRenderPassBegun);
#endif
#ifndef NDEBUG
    if (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS && m_CurrentPipelineLayout == VK_NULL_HANDLE)
    {
        std::cerr << "[RG Assert] Draw (firstVertex variant) without pipeline. Last marker/pass='" << m_LastMarkerName << "'\n";
        assert(false && "Draw called without a graphics pipeline bound (SetPipeline missing in this pass)");
    }
#endif

    // Validate at draw as well
    ValidatePipelineMatchesActiveScope(m_CurrentPipelineHandle);
    MaybeCaptureDescriptorState();
    vkCmdDraw(m_CommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
#ifndef NDEBUG
    // Must be in a rendering scope and have a pipeline bound for graphics draws
    assert(m_DynamicRenderingBegun || m_LegacyRenderPassBegun);
    if (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS && m_CurrentPipelineLayout == VK_NULL_HANDLE)
    {
        assert(false && "DrawIndexed called without a graphics pipeline bound (SetPipeline missing in this pass)");
    }
#endif

    // Additional runtime validation at draw time to ensure visibility even if pipeline was bound pre-rendering
    ValidatePipelineMatchesActiveScope(m_CurrentPipelineHandle);
    MaybeCaptureDescriptorState();
    vkCmdDrawIndexed(m_CommandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    MaybeCaptureDescriptorState();
    vkCmdDrawIndexed(m_CommandBuffer, indexCount, instanceCount, 0, 0, 0);
}

void VulkanCommandList::DrawMeshTasks(uint32_t taskCount)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
    if (m_Device && m_Device->SupportsMeshShadersEXT() && m_Device->GetCmdDrawMeshTasksEXT())
    {
        m_Device->GetCmdDrawMeshTasksEXT()(m_CommandBuffer, taskCount, 0, 0);
    }
    else
    {
        // Fallback: issue a normal draw if a vertex pipeline is bound (dev convenience)
        vkCmdDraw(m_CommandBuffer, taskCount * 3, 1, 0, 0);
    }
}

void VulkanCommandList::DrawIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    VkBuffer vkBuffer = m_Device->GetVkBuffer(commandBuffer);
    if (vkBuffer != VK_NULL_HANDLE)
    {
        MaybeCaptureDescriptorState();
        vkCmdDrawIndirect(m_CommandBuffer, vkBuffer, 0, drawCount, stride);
    }
}

void VulkanCommandList::DrawIndexedIndirect(BufferHandle commandBuffer, uint32_t drawCount, uint32_t stride)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    VkBuffer vkBuffer = m_Device->GetVkBuffer(commandBuffer);
    if (vkBuffer != VK_NULL_HANDLE)
    {
        MaybeCaptureDescriptorState();
        vkCmdDrawIndexedIndirect(m_CommandBuffer, vkBuffer, 0, drawCount, stride);
    }
}

void VulkanCommandList::DrawIndexedIndirectCount(BufferHandle commandBuffer,
                                                 BufferHandle countBuffer,
                                                 uint32_t     maxDrawCount,
                                                 uint32_t     stride,
                                                 size_t       commandBufferOffset,
                                                 size_t       countBufferOffset)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    VkBuffer vkCmdBuf   = m_Device->GetVkBuffer(commandBuffer);
    VkBuffer vkCountBuf = m_Device->GetVkBuffer(countBuffer);
    if (vkCmdBuf == VK_NULL_HANDLE || vkCountBuf == VK_NULL_HANDLE)
    {
        return;
    }

    MaybeCaptureDescriptorState();
    if (m_Device && !m_Device->SupportsDrawIndirectCount())
    {
        static bool s_LoggedFallback = false;
        if (!s_LoggedFallback)
        {
            Logger::Log::Warning(
                "VulkanCommandList: drawIndirectCount unsupported; falling back to "
                "vkCmdDrawIndexedIndirect over the full record range (relies on "
                "unused records having indexCount=0).");
            s_LoggedFallback = true;
        }
        // IMPLICIT CONTRACT — important: this fallback ignores the count buffer
        // and draws ALL maxDrawCount records. Producers MUST zero-fill any
        // record slot beyond the count they actually emit, so the trailing
        // entries become degenerate (indexCount=0) no-ops. See
        // GPUDrawStreamBuilder::ScheduleUnified for the producer-side fill.
        const VkDeviceSize baseOffset = static_cast<VkDeviceSize>(commandBufferOffset);
        if (m_Device->SupportsMultiDrawIndirect())
        {
            // Single batched call. The driver iterates `maxDrawCount` records
            // internally — strictly cheaper than the N-call CPU loop below.
            vkCmdDrawIndexedIndirect(
                m_CommandBuffer, vkCmdBuf, baseOffset, maxDrawCount, stride);
        }
        else
        {
            // Driver doesn't expose multi-draw indirect either: emit one call
            // per record. Rare on MoltenVK builds we care about, but kept as
            // a defensive last-resort path.
            const VkDeviceSize recordStride = static_cast<VkDeviceSize>(stride);
            for (uint32_t i = 0; i < maxDrawCount; ++i)
            {
                vkCmdDrawIndexedIndirect(
                    m_CommandBuffer,
                    vkCmdBuf,
                    baseOffset + (static_cast<VkDeviceSize>(i) * recordStride),
                    1,
                    stride);
            }
        }
        return;
    }

    // Core in Vulkan 1.2+; engine requires API ≥ 1.2 for descriptor indexing
    // anyway (see VulkanDevice apiVersion gate), so no fallback path needed.
    vkCmdDrawIndexedIndirectCount(
        m_CommandBuffer,
        vkCmdBuf,   static_cast<VkDeviceSize>(commandBufferOffset),
        vkCountBuf, static_cast<VkDeviceSize>(countBufferOffset),
        maxDrawCount, stride);
}

bool VulkanCommandList::RequireComputePipelineBound(const char* op)
{
    if (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE && m_CurrentPipelineLayout != VK_NULL_HANDLE)
    {
        return true;
    }
    if (!m_WarnedNoComputePipeline)
    {
        m_WarnedNoComputePipeline = true;
        Logger::Log::Error("VulkanCommandList: {} with no compute pipeline bound — skipped. Either the pass "
                           "never called SetPipeline, or SetPipeline resolved a dead handle (e.g. a pipeline "
                           "cached across a device rebuild). Last marker/pass='{}'",
                           op, m_LastMarkerName);
    }
    // Compiled out under NDEBUG, so Release refuses the dispatch quietly-but-logged
    // while debug/DebugFast still fail the run outright on a missing SetPipeline.
    assert(false && "Dispatch called without a compute pipeline bound (SetPipeline missing in this pass)");
    return false;
}

void VulkanCommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
    if (!RequireComputePipelineBound("Dispatch"))
    {
        return;
    }
    vkCmdDispatch(m_CommandBuffer, x, y, z);
}

void VulkanCommandList::DispatchIndirect(BufferHandle argsBuffer, size_t argsOffsetBytes)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
    if (!RequireComputePipelineBound("DispatchIndirect"))
    {
        return;
    }
    VkBuffer vkBuffer = m_Device->GetVkBuffer(argsBuffer);
    if (vkBuffer != VK_NULL_HANDLE)
    {
        // VUID-vkCmdDispatchIndirect-offset-02710: offset must be 4-aligned.
        // (Range vs buffer size is validation-layer territory — no size query
        // exists on the command-list surface.)
        assert(argsOffsetBytes % 4 == 0 &&
               "DispatchIndirect args offset must be a multiple of 4");
        vkCmdDispatchIndirect(m_CommandBuffer, vkBuffer, static_cast<VkDeviceSize>(argsOffsetBytes));
    }
}

VkAttachmentStoreOp VulkanCommandList::ResolveStoreOp(RenderPassDesc::StoreOp op, bool storeOpNoneUsable)
{
    switch (op)
    {
    case RenderPassDesc::StoreOp::Store: return VK_ATTACHMENT_STORE_OP_STORE;
    case RenderPassDesc::StoreOp::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case RenderPassDesc::StoreOp::None:
        return storeOpNoneUsable ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
    }
    return VK_ATTACHMENT_STORE_OP_STORE;
}

void VulkanCommandList::BeginRenderPass(const RenderPassDesc& desc)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
    // Reset current color target count for this rendering scope
    m_CurrentColorTargetCount = 0;

    if (GameEngine::Rendering::RenderingDebugFlags::Get().srgbDiag && m_LastMarkerName == "FinalSRGBEncode")
    {
        std::string colors;
        for (uint32_t i = 0; i < 8; ++i)
        {
            const TextureHandle th = desc.colorTargets[i];
            if (!th.IsValid())
                continue;
            if (!colors.empty())
                colors += ", ";
            colors += std::to_string(i) +
                      ":th=" + std::to_string(static_cast<uint64_t>(th)) +
                      " img=" + std::to_string(reinterpret_cast<uint64_t>(m_Device->GetVkImage(th))) +
                      " view=" + std::to_string(reinterpret_cast<uint64_t>(m_Device->GetVkImageView(th)));
        }
        Logger::Log::Info(
            "[SRGBEncodeCLDiag] descColorTargetCount={} descColors=[{}]",
            desc.colorTargetCount,
            colors.empty() ? "<none>" : colors);
    }

    // Toggle dynamic rendering per device setting and capability
    m_DynamicRenderingActive = m_Device->IsDynamicRenderingEnabled() && m_Device->SupportsDynamicRendering();
    m_DynamicRenderingBegun = false;
    m_LegacyRenderPassBegun = false;
    m_LegacySwapchainAttachment = INVALID_HANDLE;
    m_LegacyOneOffUsesRP2 = false;

    // StoreOp::None means "preserve, write nothing" — the only op a read-only
    // attachment may carry, and it preserves the contents only while nothing
    // writes the attachment. What suppresses the writes is
    // vkCmdSetDepthWriteEnable(VK_FALSE), which SetPipeline issues only inside a
    // dynamic render pass or a secondary inheriting one; a legacy render pass
    // leaves every pipeline's compiled depthWriteEnable in force, so NONE there
    // could leave the contents undefined. Falling back to STORE keeps them
    // (never DONT_CARE, which discards them): it costs the sync-hazard freedom,
    // not the pixels.
    const bool storeOpNoneUsable = m_Device->SupportsAttachmentStoreOpNone()
                                   && m_DynamicRenderingActive
                                   && m_Device->GetCmdSetDepthWriteEnable() != nullptr;
    const auto toStoreOp = [storeOpNoneUsable](RenderPassDesc::StoreOp op)
    { return ResolveStoreOp(op, storeOpNoneUsable); };

    // Build attachments from desc (ignore INVALID_HANDLE entries)
    std::vector<VkFormat> colorFormats;
    std::vector<VkAttachmentLoadOp> colorLoads;
    std::vector<VkAttachmentStoreOp> colorStores;
    std::vector<VkImageView> views;
    colorFormats.reserve(desc.colorTargetCount);
    colorLoads.reserve(desc.colorTargetCount);
    colorStores.reserve(desc.colorTargetCount);
    views.reserve(desc.colorTargetCount + 1);

    std::vector<uint32_t> colorSlots;
    colorSlots.reserve(desc.colorTargetCount);
    for (uint32_t i = 0; i < 8; ++i)
    {
        TextureHandle th = desc.colorTargets[i];
        if (th == INVALID_HANDLE)
            continue;
        colorSlots.push_back(i);
        VkFormat fmt = m_Device->GetVkImageFormat(th);
        // Swapchain images should always have a known format. In some swapchain recreation paths,
        // the per-handle format cache can be temporarily unset; fall back to the swapchain format
        // so dynamic rendering validation and pipeline compatibility remain correct.
        if (fmt == VK_FORMAT_UNDEFINED && m_Device->IsSwapchainTextureHandle(th))
        {
            fmt = m_Device->GetSwapchainFormat();
        }
        colorFormats.push_back(fmt);
        colorLoads.push_back(desc.clearColor[i] || desc.colorLoadOp[i] == RenderPassDesc::LoadOp::Clear
                                 ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                 : (desc.colorLoadOp[i] == RenderPassDesc::LoadOp::Load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE));
        colorStores.push_back(toStoreOp(desc.colorStoreOp[i]));
        // Use explicit view if provided for this color slot (for legacy swapchain fast-path only)
        if (desc.useColorView[i])
        {
            TextureViewHandle tvh = GetOrCreateAttachmentView(th, desc.colorViewDesc[i]);
            views.push_back(m_Device->GetVkImageView(tvh));
        }
        else
        {
            views.push_back(m_Device->GetVkImageView(th));
        }
    }

    const uint32_t colorCount = static_cast<uint32_t>(colorFormats.size());
    m_CurrentColorTargetCount = colorCount;
    const bool hasDepth = desc.depthTarget != INVALID_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkAttachmentLoadOp depthLoad = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp depthStore = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkAttachmentLoadOp stencilLoad = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkAttachmentStoreOp stencilStore = VK_ATTACHMENT_STORE_OP_DONT_CARE;

    if (hasDepth)
    {
        depthFormat = m_Device->GetVkImageFormat(desc.depthTarget);
        depthLoad = desc.clearDepth || desc.depthLoadOp == RenderPassDesc::LoadOp::Clear
                        ? VK_ATTACHMENT_LOAD_OP_CLEAR
                        : (desc.depthLoadOp == RenderPassDesc::LoadOp::Load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        depthStore = toStoreOp(desc.depthStoreOp);
        stencilLoad = desc.clearStencil || desc.stencilLoadOp == RenderPassDesc::LoadOp::Clear
                          ? VK_ATTACHMENT_LOAD_OP_CLEAR
                          : (desc.stencilLoadOp == RenderPassDesc::LoadOp::Load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        stencilStore = toStoreOp(desc.stencilStoreOp);
        if (desc.useDepthView)
        {
            TextureViewHandle tvh = GetOrCreateAttachmentView(desc.depthTarget, desc.depthViewDesc);
            views.push_back(m_Device->GetVkImageView(tvh));
        }
        else
        {
            views.push_back(m_Device->GetVkImageView(desc.depthTarget));
        }
    }

    // Early out if nothing to render
    if (colorCount == 0 && !hasDepth)
    {
        return;
    }

    // Fast path: if rendering directly to swapchain backbuffer with 1 color and no depth,
    // use the exact swapchain render pass/framebuffer to guarantee compatibility with pipelines.
    const bool isSwapchainPath = (desc.colorTargetCount == 1) &&
                                 (desc.colorTargets[0] != INVALID_HANDLE) &&
                                 m_Device->IsSwapchainTextureHandle(desc.colorTargets[0]) &&
                                 !hasDepth;
    if (isSwapchainPath && !m_DynamicRenderingActive && !desc.useColorView[0] && !desc.useDepthView)
    {
        VkRenderPass renderPass = m_Device->GetOrCreateSwapchainRenderPass();
        VkFramebuffer framebuffer = m_Device->GetCurrentSwapchainFramebuffer();
        if (renderPass == VK_NULL_HANDLE || framebuffer == VK_NULL_HANDLE)
        {
            // This can happen during swapchain recreation.
            Logger::Log::Warning(
                "BeginRenderPass: swapchain render pass or framebuffer is NULL (renderPass={}, framebuffer={}). "
                "This can happen during resize. Skipping render pass.",
                (void*)renderPass, (void*)framebuffer);
            return;
        }
        VkExtent2D sc = m_Device->GetSwapchainExtent();

        // Rely on RenderGraph for swapchain image layout transitions. No ad-hoc transitions here.

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass;
        renderPassInfo.framebuffer = framebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {sc.width, sc.height};

        VkClearValue clearValues[1]{};
        clearValues[0].color.float32[0] = desc.clearColorValue[0][0];
        clearValues[0].color.float32[1] = desc.clearColorValue[0][1];
        clearValues[0].color.float32[2] = desc.clearColorValue[0][2];
        clearValues[0].color.float32[3] = desc.clearColorValue[0][3];
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = clearValues;

        vkCmdBeginRenderPass(m_CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        m_LegacyRenderPassBegun = true;
        m_LegacySwapchainAttachment = desc.colorTargets[0];
        // Mark that backbuffer is being rendered this frame so Present() chooses correct wait path
        m_Device->NotifyBackbufferRenderedThisFrame();
        return;
    }

    // For non-swapchain paths, build a compatible render pass from the attachment description
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    if (colorCount > 0)
    {
        VkExtent3D e = m_Device->GetVkImageExtent(desc.colorTargets[0]);
        width = e.width;
        height = e.height;
    }
    else if (hasDepth)
    {
        VkExtent3D e = m_Device->GetVkImageExtent(desc.depthTarget);
        width = e.width;
        height = e.height;
    }
    if (width == 0 || height == 0)
    {
        // Fallback to swapchain extent when using backbuffer
        VkExtent2D sc = m_Device->GetSwapchainExtent();
        width = sc.width;
        height = sc.height;
    }

    bool usesTransientAttachmentViews = desc.useDepthView;
    for (uint32_t slot : colorSlots)
    {
        usesTransientAttachmentViews = usesTransientAttachmentViews ||
            desc.useColorView[slot] ||
            (desc.resolveColorTargets[slot].IsValid() && desc.useColorResolveView[slot]);
    }
    usesTransientAttachmentViews = usesTransientAttachmentViews ||
        (desc.resolveDepthTarget.IsValid() && desc.useDepthResolveView) ||
        (desc.resolveStencilTarget.IsValid() && desc.useStencilResolveView);

    auto createTransientFramebuffer = [&](VkRenderPass rp,
                                          uint32_t attachmentCount,
                                          const VkImageView* attachmentViews,
                                          uint32_t fbWidth,
                                          uint32_t fbHeight) -> VkFramebuffer
    {
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = rp;
        fbci.attachmentCount = attachmentCount;
        fbci.pAttachments = attachmentViews;
        fbci.width = fbWidth;
        fbci.height = fbHeight;
        fbci.layers = 1;

        VkFramebuffer fb = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(m_Device->GetVkDevice(), &fbci, nullptr, &fb) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        m_PendingFramebufferDestroys.push_back(fb);
        return fb;
    };

    uint32_t legacyExtraAttachments = 0; // number of resolve attachments added in legacy path

    // Only create legacy render pass/framebuffer when dynamic rendering is NOT active
    if (!m_DynamicRenderingActive)
    {
        // Determine sample count from first valid attachment
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        if (colorCount > 0 && desc.colorTargets[0] != INVALID_HANDLE)
        {
            samples = m_Device->GetVkImageSamples(desc.colorTargets[0]);
        }
        else if (hasDepth && desc.depthTarget != INVALID_HANDLE)
        {
            samples = m_Device->GetVkImageSamples(desc.depthTarget);
        }

        // Determine if any resolves are requested
        bool anyColorResolve = false;
        uint32_t colorResolveCount = 0;
        for (uint32_t ci = 0; ci < colorCount; ++ci)
        {
            uint32_t slot = colorSlots[ci];
            if (desc.resolveColorTargets[slot] != INVALID_HANDLE)
            {
                anyColorResolve = true;
                ++colorResolveCount;
            }
        }
        const bool anyDSResolve = (desc.resolveDepthTarget != INVALID_HANDLE) || (desc.resolveStencilTarget != INVALID_HANDLE);

        if (!anyColorResolve && !anyDSResolve)
        {
            // Fallback to cached legacy render pass without resolves
            renderPass = m_Device->GetOrCreateRenderPass(
                colorCount,
                colorFormats.data(),
                colorLoads.data(),
                colorStores.data(),
                hasDepth,
                depthFormat,
                depthLoad,
                depthStore,
                stencilLoad,
                stencilStore,
                false,
                static_cast<uint32_t>(samples));

            // Track final present for manual barrier after end (off-swapchain path uses cache, no need)
            m_LastRenderPassWasPresent = (desc.colorTargetCount == 1) &&
                                         (desc.colorTargets[0] != INVALID_HANDLE) &&
                                         m_Device->IsSwapchainTextureHandle(desc.colorTargets[0]) &&
                                         !hasDepth;
            m_LastRenderPassImage = m_LastRenderPassWasPresent ? m_Device->GetVkImage(desc.colorTargets[0]) : VK_NULL_HANDLE;
            if (m_LastRenderPassWasPresent)
            {
                // Let device know we drew to backbuffer this frame so Present() can choose proper wait path
                m_Device->NotifyBackbufferRenderedThisFrame();
            }
            if (renderPass == VK_NULL_HANDLE)
                return;

            if (usesTransientAttachmentViews)
            {
                framebuffer = createTransientFramebuffer(renderPass,
                                                         static_cast<uint32_t>(views.size()),
                                                         views.data(),
                                                         width,
                                                         height);
            }
            else
            {
                framebuffer = m_Device->GetOrCreateFramebuffer(renderPass,
                                                               static_cast<uint32_t>(views.size()),
                                                               views.data(),
                                                               width,
                                                               height);
            }
        }
        else
        {
            // Build a one-off VkRenderPass2 with color/depth resolve attachments.
            // Pushed to m_PendingRenderPassDestroys and destroyed in Begin() on the
            // next frame, after the GPU fence guarantees the command buffer has completed.
            std::vector<VkAttachmentDescription> attachments;
            attachments.reserve(colorCount + (hasDepth ? 1u : 0u) + colorResolveCount);
            std::vector<VkAttachmentReference> colorRefs(colorCount);
            std::vector<VkAttachmentReference> colorResolveRefs(colorCount);

            std::vector<VkImageView> fbViews;
            fbViews.reserve(colorCount + (hasDepth ? 1u : 0u) + colorResolveCount);

            // Query the tracked layout for each attachment so initialLayout matches
            // the post-RG-barrier state. The RG emits explicit barriers before the
            // pass regardless of whether dynamic rendering is active; using UNDEFINED
            // when the image is already COLOR_ATTACHMENT_OPTIMAL creates a mismatch
            // that confuses the validation layer's submit-time layout checks.
            auto queryInitialLayout = [&](TextureHandle th, VkImageLayout fallback) -> VkImageLayout
            {
                if (!th.IsValid() || !m_Device)
                    return fallback;
                VkImageLayout tracked = m_Device->GetTrackedImageLayout(th);
                return (tracked != VK_IMAGE_LAYOUT_UNDEFINED) ? tracked : fallback;
            };

            for (uint32_t ci = 0; ci < colorCount; ++ci)
            {
                uint32_t slot = colorSlots[ci];
                VkAttachmentDescription cad{};
                cad.format = colorFormats[ci];
                cad.samples = static_cast<VkSampleCountFlagBits>(samples);
                cad.loadOp = colorLoads[ci];
                cad.storeOp = colorStores[ci];
                cad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                cad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                if (cad.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
                    cad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                else
                    cad.initialLayout = queryInitialLayout(desc.colorTargets[slot], VK_IMAGE_LAYOUT_UNDEFINED);
                cad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                uint32_t colorIndex = static_cast<uint32_t>(attachments.size());
                attachments.push_back(cad);
                colorRefs[ci] = {colorIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                TextureHandle th = desc.colorTargets[slot];
                if (desc.useColorView[slot])
                {
                    TextureViewHandle tvh = GetOrCreateAttachmentView(th, desc.colorViewDesc[slot]);
                    fbViews.push_back(m_Device->GetVkImageView(tvh));
                }
                else
                {
                    fbViews.push_back(m_Device->GetVkImageView(th));
                }

                if (desc.resolveColorTargets[slot] != INVALID_HANDLE)
                {
                    VkAttachmentDescription rad{};
                    VkFormat resolveFmt = m_Device->GetVkImageFormat(desc.resolveColorTargets[slot]);
                    rad.format = resolveFmt;
                    rad.samples = VK_SAMPLE_COUNT_1_BIT;
                    rad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    rad.storeOp = colorStores[ci];
                    rad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    rad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    rad.initialLayout = queryInitialLayout(desc.resolveColorTargets[slot], VK_IMAGE_LAYOUT_UNDEFINED);
                    rad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    uint32_t resolveIndex = static_cast<uint32_t>(attachments.size());
                    attachments.push_back(rad);
                    colorResolveRefs[ci] = {resolveIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                    TextureHandle rth = desc.resolveColorTargets[slot];
                    if (desc.useColorResolveView[slot])
                    {
                        TextureViewHandle rtvh = GetOrCreateAttachmentView(rth, desc.colorResolveViewDesc[slot]);
                        fbViews.push_back(m_Device->GetVkImageView(rtvh));
                    }
                    else
                    {
                        fbViews.push_back(m_Device->GetVkImageView(rth));
                    }
                }
                else
                {
                    colorResolveRefs[ci] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
                }
            }

            int32_t depthAttachIndex = -1;
            int32_t depthResolveAttachIndex = -1;
            if (hasDepth)
            {
                VkAttachmentDescription dad{};
                dad.format = depthFormat;
                dad.samples = static_cast<VkSampleCountFlagBits>(samples);
                dad.loadOp = depthLoad;
                dad.storeOp = depthStore;
                dad.stencilLoadOp = stencilLoad;
                dad.stencilStoreOp = stencilStore;
                const VkImageLayout depthLayout = desc.depthReadOnly
                    ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                    : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                if (depthLoad == VK_ATTACHMENT_LOAD_OP_LOAD)
                    dad.initialLayout = depthLayout;
                else
                    dad.initialLayout = queryInitialLayout(desc.depthTarget, VK_IMAGE_LAYOUT_UNDEFINED);
                dad.finalLayout = depthLayout;
                depthAttachIndex = static_cast<int32_t>(attachments.size());
                attachments.push_back(dad);
                // Depth view
                if (desc.useDepthView)
                {
                    TextureViewHandle dtvh = GetOrCreateAttachmentView(desc.depthTarget, desc.depthViewDesc);
                    fbViews.push_back(m_Device->GetVkImageView(dtvh));
                }
                else
                {
                    fbViews.push_back(m_Device->GetVkImageView(desc.depthTarget));
                }

                // Depth/stencil resolve target if provided
                if (desc.resolveDepthTarget.IsValid() || desc.resolveStencilTarget.IsValid())
                {
                    TextureHandle rdst = desc.resolveDepthTarget.IsValid() ? desc.resolveDepthTarget : desc.resolveStencilTarget;
                    VkAttachmentDescription dsrad{};
                    dsrad.format = m_Device->GetVkImageFormat(rdst);
                    dsrad.samples = VK_SAMPLE_COUNT_1_BIT;
                    dsrad.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    dsrad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    dsrad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    dsrad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
                    dsrad.initialLayout = queryInitialLayout(rdst, VK_IMAGE_LAYOUT_UNDEFINED);
                    dsrad.finalLayout = depthLayout;
                    depthResolveAttachIndex = static_cast<int32_t>(attachments.size());
                    attachments.push_back(dsrad);
                    // Resolve view
                    if (desc.resolveDepthTarget.IsValid() && desc.useDepthResolveView)
                    {
                        TextureViewHandle rtvh = GetOrCreateAttachmentView(rdst, desc.depthResolveViewDesc);
                        fbViews.push_back(m_Device->GetVkImageView(rtvh));
                    }
                    else if (desc.resolveStencilTarget.IsValid() && desc.useStencilResolveView)
                    {
                        TextureViewHandle rtvh = GetOrCreateAttachmentView(rdst, desc.stencilResolveViewDesc);
                        fbViews.push_back(m_Device->GetVkImageView(rtvh));
                    }
                    else
                    {
                        fbViews.push_back(m_Device->GetVkImageView(rdst));
                    }
                }
            }

            // Build subpass2 + ds resolve chain
            // Convert color refs to VkAttachmentReference2
            std::vector<VkAttachmentReference2> colorRefs2(colorCount);
            std::vector<VkAttachmentReference2> colorResolveRefs2(colorCount);
            for (uint32_t ci = 0; ci < colorCount; ++ci)
            {
                colorRefs2[ci] = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, colorRefs[ci].attachment, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT};
                if (colorResolveRefs[ci].attachment != VK_ATTACHMENT_UNUSED)
                {
                    colorResolveRefs2[ci] = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, colorResolveRefs[ci].attachment, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT};
                }
                else
                {
                    colorResolveRefs2[ci] = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED, 0};
                }
            }

            const VkImageLayout depthRefLayout = (hasDepth && desc.depthReadOnly)
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkAttachmentReference2 depthRef2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, VK_ATTACHMENT_UNUSED, depthRefLayout, (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)};
            if (hasDepth && depthAttachIndex >= 0)
            {
                depthRef2.attachment = static_cast<uint32_t>(depthAttachIndex);
            }

            VkSubpassDescriptionDepthStencilResolve dsResolve{};
            dsResolve.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
            dsResolve.pNext = nullptr;
            dsResolve.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            dsResolve.stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            VkAttachmentReference2 depthResolveRef2{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, VK_ATTACHMENT_UNUSED, depthRefLayout, (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)};

            if (hasDepth && depthResolveAttachIndex >= 0)
            {
                // Map requested modes and clamp to capabilities
                const auto& caps = m_Device->GetCapabilities();

                // Helper to map Vulkan resolve flags to engine capability bits for verification (using shared mapping logic)
                auto hasDepthMode = [&](VkResolveModeFlagBits m)
                { return VulkanMappings::CheckDepthResolveSupport(caps, m); };
                auto hasStencilMode = [&](VkResolveModeFlagBits m)
                { return VulkanMappings::CheckStencilResolveSupport(caps, m); };

                VkResolveModeFlagBits reqDepth = dsResolve.depthResolveMode;
                VkResolveModeFlagBits reqStencil = dsResolve.stencilResolveMode;
                if (desc.useDepthResolveMode)
                    reqDepth = VulkanMappings::TranslateResolveModeToVulkan(desc.depthResolveMode);
                if (desc.useStencilResolveMode)
                    reqStencil = VulkanMappings::TranslateResolveModeToVulkan(desc.stencilResolveMode);
                if (!hasDepthMode(reqDepth))
                {
                    if (hasDepthMode(VK_RESOLVE_MODE_SAMPLE_ZERO_BIT))
                        reqDepth = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                    else if (hasDepthMode(VK_RESOLVE_MODE_AVERAGE_BIT))
                        reqDepth = VK_RESOLVE_MODE_AVERAGE_BIT;
                    else if (hasDepthMode(VK_RESOLVE_MODE_MIN_BIT))
                        reqDepth = VK_RESOLVE_MODE_MIN_BIT;
                    else if (hasDepthMode(VK_RESOLVE_MODE_MAX_BIT))
                        reqDepth = VK_RESOLVE_MODE_MAX_BIT;
                }
                if (!hasStencilMode(reqStencil))
                {
                    if (hasStencilMode(VK_RESOLVE_MODE_SAMPLE_ZERO_BIT))
                        reqStencil = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                    else if (hasStencilMode(VK_RESOLVE_MODE_AVERAGE_BIT))
                        reqStencil = VK_RESOLVE_MODE_AVERAGE_BIT;
                    else if (hasStencilMode(VK_RESOLVE_MODE_MIN_BIT))
                        reqStencil = VK_RESOLVE_MODE_MIN_BIT;
                    else if (hasStencilMode(VK_RESOLVE_MODE_MAX_BIT))
                        reqStencil = VK_RESOLVE_MODE_MAX_BIT;
                }
                dsResolve.depthResolveMode = reqDepth;
                dsResolve.stencilResolveMode = reqStencil;
                depthResolveRef2.attachment = static_cast<uint32_t>(depthResolveAttachIndex);
                dsResolve.pDepthStencilResolveAttachment = &depthResolveRef2;
            }
            else
            {
                dsResolve.pDepthStencilResolveAttachment = nullptr;
            }

            VkSubpassDescription2 subpass2{};
            subpass2.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
            subpass2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass2.colorAttachmentCount = colorCount;
            subpass2.pColorAttachments = colorRefs2.data();
            subpass2.pResolveAttachments = colorResolveCount ? colorResolveRefs2.data() : nullptr;
            subpass2.pDepthStencilAttachment = hasDepth ? &depthRef2 : nullptr;
            subpass2.pNext = (hasDepth && depthResolveAttachIndex >= 0) ? &dsResolve : nullptr;

            // Convert attachment descriptions to *_2
            std::vector<VkAttachmentDescription2> attachments2;
            attachments2.reserve(attachments.size());
            for (const auto& a : attachments)
            {
                VkAttachmentDescription2 a2{};
                a2.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
                a2.format = a.format;
                a2.samples = a.samples;
                a2.loadOp = a.loadOp;
                a2.storeOp = a.storeOp;
                a2.stencilLoadOp = a.stencilLoadOp;
                a2.stencilStoreOp = a.stencilStoreOp;
                a2.initialLayout = a.initialLayout;
                a2.finalLayout = a.finalLayout;
                attachments2.push_back(a2);
            }

            VkRenderPassCreateInfo2 rpci2{};
            rpci2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
            rpci2.attachmentCount = static_cast<uint32_t>(attachments2.size());
            rpci2.pAttachments = attachments2.data();
            rpci2.subpassCount = 1;
            rpci2.pSubpasses = &subpass2;

            if (vkCreateRenderPass2(m_Device->GetVkDevice(), &rpci2, nullptr, &renderPass) != VK_SUCCESS)
            {
                return;
            }
            m_PendingRenderPassDestroys.push_back(renderPass);
            m_LegacyOneOffUsesRP2 = true;

            legacyExtraAttachments = colorResolveCount + ((hasDepth && depthResolveAttachIndex >= 0) ? 1u : 0u);
            framebuffer = createTransientFramebuffer(renderPass,
                                                     static_cast<uint32_t>(fbViews.size()),
                                                     fbViews.data(),
                                                     width,
                                                     height);
        }
    }

    // Dynamic rendering path (preferred)
    if (m_DynamicRenderingActive)
    {
        m_CurrentRenderingColorFormats.clear();

        // Helper: ensure swapchain images are transitioned back to COLOR_ATTACHMENT_OPTIMAL
        // before they are used as dynamic rendering attachments.
        //
        // Why: swapchain images may start a frame in UNDEFINED (first frame) or PRESENT (after vkQueuePresentKHR).
        // If the render graph misses (or skips) the transition barrier, validation will trip at vkCmdDraw with
        // "expects COLOR_ATTACHMENT_OPTIMAL, current layout UNDEFINED/PRESENT".
        auto ensureSwapchainColorLayout = [&](TextureHandle th)
        {
            if (!th.IsValid() || !m_Device || !m_Device->IsSwapchainTextureHandle(th))
                return;
            const VkImageLayout tracked = m_Device->GetTrackedSwapchainImageLayout(th);
            if (tracked == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                return; // already known-good
            // Force a transition to RenderTarget (COLOR_ATTACHMENT_OPTIMAL). oldLayout is resolved in BarrierBatch
            // using the device's swapchain layout tracker when available (PRESENT), otherwise UNDEFINED.
            Barrier(ResourceBarrier::CreateTextureBarrier(th, ResourceState::Undefined, ResourceState::RenderTarget, 0u, 1u, 0u, 1u));
        };
        // Fallback for passes whose target layout the render graph did not already
        // transition. Checks and transitions the attachment's ACTUAL subresource:
        // the device tracker is whole-image, so for layered targets (cube faces,
        // array slices) it can misreport the attached cell — acting on (0,0) here
        // used to desync layer 0 from the graph's per-cell state (VUID-09600 on
        // sliced cube bakes). Per-command-list tracking is consulted first because
        // it already reflects this frame's hoisted import transitions.
        auto ensureAttachmentColorLayout = [&](TextureHandle th, uint32_t baseMip, uint32_t baseLayer)
        {
            if (!th.IsValid() || !m_Device)
                return;
            if (m_Device->IsSwapchainTextureHandle(th))
                return;
            VkImageLayout tracked = VK_IMAGE_LAYOUT_UNDEFINED;
            auto it = m_SubresourceLayouts.find(MakeSubKey(th, baseMip, baseLayer));
            if (it != m_SubresourceLayouts.end())
                tracked = it->second;
            else
                tracked = m_Device->GetTrackedImageLayout(th);
            if (tracked == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                return;
            ResourceState srcState = ResourceState::Undefined;
            if (tracked == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                srcState = ResourceState::ShaderResource;
            else if (tracked == VK_IMAGE_LAYOUT_GENERAL)
                srcState = ResourceState::UnorderedAccess;
            Barrier(ResourceBarrier::CreateTextureBarrier(
                th, srcState, ResourceState::RenderTarget,
                baseMip, 1u, baseLayer, 1u));
        };

        // Build VkRenderingAttachmentInfo for colors
        std::vector<VkRenderingAttachmentInfo> colorAttachments;
        colorAttachments.reserve(colorCount);
        // Track first valid color attachment and its samples for debug checks
        m_CurrentColorAttachment = INVALID_TEXTURE_HANDLE;
        m_CurrentColorSamples = VK_SAMPLE_COUNT_1_BIT;
        for (uint32_t i = 0; i < 8; ++i)
        {
            TextureHandle th = desc.colorTargets[i];
            if (th.IsValid())
            {
                // Track current color formats for runtime validation
                m_CurrentRenderingColorFormats.push_back(m_Device->GetVkImageFormat(th));
            }

            if (!th.IsValid())
                continue;

            ensureSwapchainColorLayout(th);
            ensureAttachmentColorLayout(th,
                                        desc.useColorView[i] ? desc.colorViewDesc[i].baseMip : 0u,
                                        desc.useColorView[i] ? desc.colorViewDesc[i].baseLayer : 0u);

            if (!m_CurrentColorAttachment.IsValid())
            {
                m_CurrentColorAttachment = th;
                m_CurrentColorSamples = m_Device->GetVkImageSamples(th);
            }
            VkRenderingAttachmentInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            VkImageView selectedView = VK_NULL_HANDLE;
            if (desc.useColorView[i])
            {
                TextureViewHandle tvh = GetOrCreateAttachmentView(th, desc.colorViewDesc[i]);
                selectedView = m_Device->GetVkImageView(tvh);
            }
            else
            {
                selectedView = m_Device->GetVkImageView(th);
            }
            ai.imageView = selectedView;

            ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            const bool doClear = (desc.clearColor[i] || desc.colorLoadOp[i] == RenderPassDesc::LoadOp::Clear);
            ai.loadOp = doClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                : (desc.colorLoadOp[i] == RenderPassDesc::LoadOp::Load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
            // Respect requested storeOp from RenderPassDesc
            ai.storeOp = toStoreOp(desc.colorStoreOp[i]);
            VkClearValue cv{};
            cv.color.float32[0] = desc.clearColorValue[i][0];
            cv.color.float32[1] = desc.clearColorValue[i][1];
            cv.color.float32[2] = desc.clearColorValue[i][2];
            cv.color.float32[3] = desc.clearColorValue[i][3];
            ai.clearValue = cv;
#if defined(DEBUG) || defined(_DEBUG)

#endif

            if (desc.resolveColorTargets[i].IsValid())
            {
                // Resolve target may also be the swapchain.
                ensureSwapchainColorLayout(desc.resolveColorTargets[i]);
                // Ensure resolve destination is in COLOR_ATTACHMENT_OPTIMAL.
                ensureAttachmentColorLayout(
                    desc.resolveColorTargets[i],
                    desc.useColorResolveView[i] ? desc.colorResolveViewDesc[i].baseMip : 0u,
                    desc.useColorResolveView[i] ? desc.colorResolveViewDesc[i].baseLayer : 0u);

                // MSAA resolve validation: src must be MSAA, dst must be single-sample
                VkSampleCountFlagBits srcSamples = m_Device->GetVkImageSamples(desc.colorTargets[i]);
                VkSampleCountFlagBits dstSamples = m_Device->GetVkImageSamples(desc.resolveColorTargets[i]);
                if (srcSamples > VK_SAMPLE_COUNT_1_BIT && dstSamples == VK_SAMPLE_COUNT_1_BIT)
                {
                    VkImageView resolveView = VK_NULL_HANDLE;
                    if (desc.useColorResolveView[i])
                    {
                        TextureViewHandle rtvh = GetOrCreateAttachmentView(desc.resolveColorTargets[i], desc.colorResolveViewDesc[i]);
                        resolveView = m_Device->GetVkImageView(rtvh);
                    }
                    else
                    {
                        resolveView = m_Device->GetVkImageView(desc.resolveColorTargets[i]);
                    }
                    ai.resolveImageView = resolveView;
                    ai.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    // Resolve mode: default based on format unless overridden by RenderPassDesc
                    VkFormat srcFmt = m_Device->GetVkImageFormat(desc.colorTargets[i]);
                    auto isIntegerColorFormat = [](VkFormat f) -> bool
                    {
                        switch (f)
                        {
                        case VK_FORMAT_R8_UINT:
                        case VK_FORMAT_R8_SINT:
                        case VK_FORMAT_R8G8_UINT:
                        case VK_FORMAT_R8G8_SINT:
                        case VK_FORMAT_R8G8B8_UINT:
                        case VK_FORMAT_R8G8B8_SINT:
                        case VK_FORMAT_R8G8B8A8_UINT:
                        case VK_FORMAT_R8G8B8A8_SINT:
                        case VK_FORMAT_A2R10G10B10_UINT_PACK32:
                        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
                        case VK_FORMAT_R16_UINT:
                        case VK_FORMAT_R16_SINT:
                        case VK_FORMAT_R16G16_UINT:
                        case VK_FORMAT_R16G16_SINT:
                        case VK_FORMAT_R16G16B16_UINT:
                        case VK_FORMAT_R16G16B16_SINT:
                        case VK_FORMAT_R16G16B16A16_UINT:
                        case VK_FORMAT_R16G16B16A16_SINT:
                        case VK_FORMAT_R32_UINT:
                        case VK_FORMAT_R32_SINT:
                        case VK_FORMAT_R32G32_UINT:
                        case VK_FORMAT_R32G32_SINT:
                        case VK_FORMAT_R32G32B32_UINT:
                        case VK_FORMAT_R32G32B32_SINT:
                        case VK_FORMAT_R32G32B32A32_UINT:
                        case VK_FORMAT_R32G32B32A32_SINT:
                        // 64-bit integer families (if supported as color attachments)
                        case VK_FORMAT_R64_UINT:
                        case VK_FORMAT_R64_SINT:
                        case VK_FORMAT_R64G64_UINT:
                        case VK_FORMAT_R64G64_SINT:
                        case VK_FORMAT_R64G64B64_UINT:
                        case VK_FORMAT_R64G64B64_SINT:
                        case VK_FORMAT_R64G64B64A64_UINT:
                        case VK_FORMAT_R64G64B64A64_SINT:
                            return true;
                        default:
                            return false;
                        }
                    };
                    ai.resolveMode = isIntegerColorFormat(srcFmt) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                                                  : VK_RESOLVE_MODE_AVERAGE_BIT;
                    if (desc.useColorResolveMode[i])
                    {
                        VkResolveModeFlagBits requested = VulkanMappings::TranslateResolveModeToVulkan(desc.colorResolveMode[i]);
                        if (isIntegerColorFormat(srcFmt) && requested == VK_RESOLVE_MODE_AVERAGE_BIT)
                        {
                            std::cerr << "RenderGraph Warning: Integer color format does not support Average resolve; clamping to SampleZero.\n";
                            requested = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                        }
                        ai.resolveMode = requested;
                    }
                }
            }
            colorAttachments.push_back(ai);
        }
        VkRenderingAttachmentInfo depthAttach{};
        depthAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        bool useDepth = hasDepth;
        // If depth is requested but sample count doesn't match color, prefer to skip binding depth (especially for read-only depth)
        if (useDepth && desc.colorTargetCount > 0 && desc.colorTargets[0].IsValid())
        {
            VkSampleCountFlagBits colorSamples = m_Device->GetVkImageSamples(desc.colorTargets[0]);
            VkSampleCountFlagBits depthSamples = m_Device->GetVkImageSamples(desc.depthTarget);
            if (depthSamples != colorSamples)
            {
                useDepth = false; // avoid invalid sample count mismatch; pipeline typically has depth test disabled in tests
            }
        }
        if (useDepth)
        {
            {
                VkImageView depthView = VK_NULL_HANDLE;
                if (desc.useDepthView)
                {
                    TextureViewHandle tvh = GetOrCreateAttachmentView(desc.depthTarget, desc.depthViewDesc);
                    depthView = m_Device->GetVkImageView(tvh);
                }
                else
                {
                    depthView = m_Device->GetVkImageView(desc.depthTarget);
                }
                depthAttach.imageView = depthView;
            }
            // Use conservative combined layouts for widest compatibility (avoid DEPTH_ATTACHMENT_OPTIMAL unless features are enabled)
            depthAttach.imageLayout = desc.depthReadOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                         : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            // If read-only is requested, do not clear; force LOAD to avoid invalid CLEAR with read-only layout.
            // The store op arrives as StoreOp::None for a read-only attachment
            // (RGPassBuilder::AttachDepth) — contents preserved, no write. STORE
            // here would be a DEPTH_STENCIL_ATTACHMENT_WRITE at LATE_FRAGMENT_TESTS
            // that no declared access covers; DONT_CARE would discard depth later
            // passes still sample.
            depthAttach.loadOp = desc.depthReadOnly ? VK_ATTACHMENT_LOAD_OP_LOAD : depthLoad;
            depthAttach.storeOp = depthStore;
            VkClearValue dv{};
            dv.depthStencil.depth = desc.clearDepthValue;
            dv.depthStencil.stencil = desc.clearStencilValue;
            depthAttach.clearValue = dv;
        }
        // Optional depth resolve setup (VK_KHR_depth_stencil_resolve via dynamic rendering)
        if (useDepth && desc.resolveDepthTarget.IsValid())
        {
            VkSampleCountFlagBits srcSamples = m_Device->GetVkImageSamples(desc.depthTarget);
            VkSampleCountFlagBits dstSamples = m_Device->GetVkImageSamples(desc.resolveDepthTarget);
            if (srcSamples > VK_SAMPLE_COUNT_1_BIT && dstSamples == VK_SAMPLE_COUNT_1_BIT)
            {
                VkImageView resolveDepthView = VK_NULL_HANDLE;
                if (desc.useDepthResolveView)
                {
                    TextureViewHandle drtvh = GetOrCreateAttachmentView(desc.resolveDepthTarget, desc.depthResolveViewDesc);
                    resolveDepthView = m_Device->GetVkImageView(drtvh);
                }
                else
                {
                    resolveDepthView = m_Device->GetVkImageView(desc.resolveDepthTarget);
                }
                depthAttach.resolveImageView = resolveDepthView;
                depthAttach.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depthAttach.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT; // widely supported for depth
                VkResolveModeFlagBits requestedDepth = depthAttach.resolveMode;
                if (desc.useDepthResolveMode)
                {
                    switch (desc.depthResolveMode)
                    {
                    case RenderPassDesc::ResolveMode::Average:
                        requestedDepth = VK_RESOLVE_MODE_AVERAGE_BIT;
                        break;
                    case RenderPassDesc::ResolveMode::SampleZero:
                        requestedDepth = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                        break;
                    case RenderPassDesc::ResolveMode::Min:
                        requestedDepth = VK_RESOLVE_MODE_MIN_BIT;
                        break;
                    case RenderPassDesc::ResolveMode::Max:
                        requestedDepth = VK_RESOLVE_MODE_MAX_BIT;
                        break;
                    }
                }
                // Clamp to supported depth resolve modes
                {
                    const auto& caps = m_Device->GetCapabilities();
                    auto hasMode = [&](VkResolveModeFlagBits m)
                    { return (caps.supportedDepthResolveModes & m) != 0; };
                    if (!hasMode(requestedDepth))
                    {
                        std::cerr << "RenderGraph Warning: Requested depth resolve mode not supported; clamping to a supported mode.\n";
                        if (hasMode(VK_RESOLVE_MODE_SAMPLE_ZERO_BIT))
                            requestedDepth = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                        else if (hasMode(VK_RESOLVE_MODE_AVERAGE_BIT))
                            requestedDepth = VK_RESOLVE_MODE_AVERAGE_BIT;
                        else if (hasMode(VK_RESOLVE_MODE_MIN_BIT))
                            requestedDepth = VK_RESOLVE_MODE_MIN_BIT;
                        else if (hasMode(VK_RESOLVE_MODE_MAX_BIT))
                            requestedDepth = VK_RESOLVE_MODE_MAX_BIT;
                    }
                    depthAttach.resolveMode = requestedDepth;
                }
            }
        }
        // Optional stencil attachment and resolve setup
        VkRenderingAttachmentInfo stencilAttach{};
        stencilAttach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        bool useStencil = false;
        if (useDepth)
        {
            VkFormat dfmt = m_Device->GetVkImageFormat(desc.depthTarget);
            bool hasStencilAspect = (dfmt == VK_FORMAT_D24_UNORM_S8_UINT || dfmt == VK_FORMAT_D32_SFLOAT_S8_UINT);
            // Only enable stencil attachment when the depth format actually has a stencil aspect
            useStencil = hasStencilAspect;
            if (useStencil)
            {
                stencilAttach.imageView = m_Device->GetVkImageView(desc.depthTarget);
                // When the format has a stencil aspect, prefer combined layout for widest compatibility
                stencilAttach.imageLayout = desc.depthReadOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                               : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                // If read-only, clearing is not valid; force LOAD
                if (desc.depthReadOnly)
                    stencilAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                else
                    stencilAttach.loadOp = stencilLoad;
                stencilAttach.storeOp = stencilStore;
                VkClearValue sv{};
                sv.depthStencil.stencil = desc.clearStencilValue;
                sv.depthStencil.depth = 0.0f;  // reverse-Z (value unused on stencil-only path; flip for consistency)
                stencilAttach.clearValue = sv;

                if (desc.resolveStencilTarget != INVALID_HANDLE)
                {
                    VkSampleCountFlagBits srcS = m_Device->GetVkImageSamples(desc.depthTarget);
                    VkSampleCountFlagBits dstS = m_Device->GetVkImageSamples(desc.resolveStencilTarget);
                    if (srcS > VK_SAMPLE_COUNT_1_BIT && dstS == VK_SAMPLE_COUNT_1_BIT)
                    {
                        VkImageView resolveStencilView = VK_NULL_HANDLE;
                        if (desc.useStencilResolveView)
                        {
                            TextureViewHandle srtvh = GetOrCreateAttachmentView(desc.resolveStencilTarget, desc.stencilResolveViewDesc);
                            resolveStencilView = m_Device->GetVkImageView(srtvh);
                        }
                        else
                        {
                            resolveStencilView = m_Device->GetVkImageView(desc.resolveStencilTarget);
                        }
                        stencilAttach.resolveImageView = resolveStencilView;
                        stencilAttach.resolveImageLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
                        stencilAttach.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                        VkResolveModeFlagBits requestedStencil = stencilAttach.resolveMode;
                        if (desc.useStencilResolveMode)
                        {
                            switch (desc.stencilResolveMode)
                            {
                            case RenderPassDesc::ResolveMode::Average:
                                requestedStencil = VK_RESOLVE_MODE_AVERAGE_BIT;
                                break;
                            case RenderPassDesc::ResolveMode::SampleZero:
                                requestedStencil = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                                break;
                            case RenderPassDesc::ResolveMode::Min:
                                requestedStencil = VK_RESOLVE_MODE_MIN_BIT;
                                break;
                            case RenderPassDesc::ResolveMode::Max:
                                requestedStencil = VK_RESOLVE_MODE_MAX_BIT;
                                break;
                            }
                        }
                        // Clamp to supported stencil resolve modes
                        {
                            const auto& caps = m_Device->GetCapabilities();
                            auto hasMode = [&](VkResolveModeFlagBits m)
                            { return (caps.supportedStencilResolveModes & m) != 0; };
                            if (!hasMode(requestedStencil))
                            {
                                std::cerr << "RenderGraph Warning: Requested stencil resolve mode not supported; clamping to a supported mode.\n";
                                if (hasMode(VK_RESOLVE_MODE_SAMPLE_ZERO_BIT))
                                    requestedStencil = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                                else if (hasMode(VK_RESOLVE_MODE_AVERAGE_BIT))
                                    requestedStencil = VK_RESOLVE_MODE_AVERAGE_BIT;
                                else if (hasMode(VK_RESOLVE_MODE_MIN_BIT))
                                    requestedStencil = VK_RESOLVE_MODE_MIN_BIT;
                                else if (hasMode(VK_RESOLVE_MODE_MAX_BIT))
                                    requestedStencil = VK_RESOLVE_MODE_MAX_BIT;
                            }
                            stencilAttach.resolveMode = requestedStencil;
                        }
                    }
                }
            }
        }

        // Compute render area extent respecting attachment view base mips.
        // NOTE: renderArea must be <= every attached imageView extent (color + depth/stencil).
        VkExtent2D extent{0, 0};
        auto applyMipShift = [](VkExtent3D e, uint32_t baseMip)
        { e.width = std::max(1u, e.width >> baseMip); e.height = std::max(1u, e.height >> baseMip); return e; };
        bool extentInitialized = false;
        for (uint32_t i = 0; i < 8; ++i)
        {
            TextureHandle th = desc.colorTargets[i];
            if (th == INVALID_HANDLE)
                continue;
            VkExtent3D e = m_Device->GetVkImageExtent(th);
            uint32_t baseMip = desc.useColorView[i] ? desc.colorViewDesc[i].baseMip : 0u;
            e = applyMipShift(e, baseMip);
            if (!extentInitialized)
            {
                extent.width = e.width;
                extent.height = e.height;
                extentInitialized = true;
            }
            else
            {
                extent.width = std::min(extent.width, e.width);
                extent.height = std::min(extent.height, e.height);
            }
        }
        // Include depth/stencil attachment in the renderArea constraint even when color is present.
        // Without this, a pass that (accidentally or intentionally) binds a smaller depth texture with a
        // larger color target will violate VUID-VkRenderingInfo-pNext-06079/06080 and can trigger device loss.
        if (useDepth && desc.depthTarget != INVALID_HANDLE)
        {
            VkExtent3D e = m_Device->GetVkImageExtent(desc.depthTarget);
            uint32_t baseMip = desc.useDepthView ? desc.depthViewDesc.baseMip : 0u;
            e = applyMipShift(e, baseMip);
            if (!extentInitialized)
            {
                extent.width = e.width;
                extent.height = e.height;
                extentInitialized = true;
            }
            else
            {
                extent.width = std::min(extent.width, e.width);
                extent.height = std::min(extent.height, e.height);
            }
        }
        // Defensive fallback: if extent is zero (bring-up), use swapchain extent to avoid validation errors.
        if (extent.width == 0 || extent.height == 0)
        {
            VkExtent2D sc = m_Device->GetSwapchainExtent();
            extent = sc;
        }
#if defined(DEBUG) || defined(_DEBUG)

#endif

        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.offset = {0, 0};
        ri.renderArea.extent = extent;
        ri.layerCount = 1;
        if (desc.useSecondaryCommandBuffers)
            ri.flags |= VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
        m_DynamicRenderingBegun = true;

        // Seed subresource layout tracking for attachments so subsequent copies use correct oldLayout
        for (uint32_t i = 0; i < 8; ++i)
        {
            TextureHandle th = desc.colorTargets[i];
            if (th != INVALID_HANDLE)
            {
                uint32_t baseMip = desc.useColorView[i] ? desc.colorViewDesc[i].baseMip : 0u;
                uint32_t baseLayer = desc.useColorView[i] ? desc.colorViewDesc[i].baseLayer : 0u;
                m_SubresourceLayouts[MakeSubKey(th, baseMip, baseLayer)] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
        if (useDepth && desc.depthTarget != INVALID_HANDLE)
        {
            VkImageLayout depthLayoutSeed = desc.depthReadOnly
                                                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            uint32_t baseMip = desc.useDepthView ? desc.depthViewDesc.baseMip : 0u;
            uint32_t baseLayer = desc.useDepthView ? desc.depthViewDesc.baseLayer : 0u;
            m_SubresourceLayouts[MakeSubKey(desc.depthTarget, baseMip, baseLayer)] = depthLayoutSeed;
        }

        ri.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
        ri.pColorAttachments = colorAttachments.empty() ? nullptr : colorAttachments.data();
        ri.pDepthAttachment = useDepth ? &depthAttach : nullptr;
        ri.pStencilAttachment = useStencil ? &stencilAttach : nullptr;
        if (GameEngine::Rendering::RenderingDebugFlags::Get().traceRendering)
        {
            if (useDepth)
            {
                printf("[BeginRendering] depthView=%p layout=%d readOnly=%d clearDepth=%.3f\n",
                       (void*)(uintptr_t)depthAttach.imageView,
                       (int)depthAttach.imageLayout,
                       desc.depthReadOnly ? 1 : 0,
                       depthAttach.clearValue.depthStencil.depth);
            }
            else
            {
                // Report whether a depth target exists and why we might be skipping it, but gate spam
                static bool sNoDepthPrinted = false;
                if (!sNoDepthPrinted)
                {
                    int hasDepthInt = (desc.depthTarget != INVALID_HANDLE) ? 1 : 0;
                    if (hasDepthInt)
                    {
                        VkSampleCountFlagBits colorSamples = VK_SAMPLE_COUNT_1_BIT;
                        if (desc.colorTargetCount > 0 && desc.colorTargets[0] != INVALID_HANDLE)
                        {
                            colorSamples = m_Device->GetVkImageSamples(desc.colorTargets[0]);
                        }
                        VkSampleCountFlagBits depthSamples = m_Device->GetVkImageSamples(desc.depthTarget);
                        printf("[BeginRendering] no-depth (hasDepth=1, colorSamples=%u, depthSamples=%u)\n",
                               (unsigned)colorSamples, (unsigned)depthSamples);
                    }
                    else
                    {
                        printf("[BeginRendering] no-depth (hasDepth=0)\n");
                    }
                    sNoDepthPrinted = true;
                }
            }
        }

        auto beginRendering = m_Device->GetCmdBeginRendering();
        if (!beginRendering)
        {
            Logger::Log::Error("VulkanCommandList: dynamic rendering requested but vkCmdBeginRendering entry point is unavailable");
            return;
        }
        beginRendering(m_CommandBuffer, &ri);
        m_DepthReadOnly = desc.depthReadOnly;
        // A2.4-P0-R: when this render pass will be filled by secondary command
        // buffers (VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT), the ONLY
        // commands legal inside the instance on THIS primary are vkCmdExecuteCommands
        // and vkCmdEndRendering — no state-setting. Each secondary sets its own
        // depth-write-enable (SetPipeline, m_IsSecondary) and viewport/scissor
        // (the pass exec). Recording them here would be dead state AND a VVL error.
        if (!desc.useSecondaryCommandBuffers)
        {
            // Set a safe default for dynamic depth write enable. The per-pipeline
            // value is applied in SetPipeline() based on each pipeline's depthWriteEnable.
            // Read-only passes always force FALSE; non-read-only passes default to TRUE
            // until the first SetPipeline call refines it.
            if (useDepth && m_Device->GetCmdSetDepthWriteEnable())
            {
                m_Device->GetCmdSetDepthWriteEnable()(m_CommandBuffer, desc.depthReadOnly ? VK_FALSE : VK_TRUE);
            }
            // Set a default viewport and scissor matching the render area so draws work without explicit calls
            VkViewport viewport{};
            viewport.x = 0.0f;
            if (m_Device->SupportsNegativeViewportHeight())
            {
                viewport.y = static_cast<float>(extent.height);
                viewport.height = -static_cast<float>(extent.height);
            }
            else
            {
                viewport.y = 0.0f;
                viewport.height = static_cast<float>(extent.height);
            }
            viewport.width = static_cast<float>(extent.width);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(m_CommandBuffer, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.offset.x = 0;
            scissor.offset.y = 0;
            scissor.extent = extent;
            vkCmdSetScissor(m_CommandBuffer, 0, 1, &scissor);
        }
        // If any color target is the swapchain backbuffer, mark for Present wait selection
        for (uint32_t i = 0; i < 8; ++i)
        {
            TextureHandle th = desc.colorTargets[i];
            if (th != INVALID_HANDLE && m_Device->IsSwapchainTextureHandle(th))
            {
                m_Device->NotifyBackbufferRenderedThisFrame();
                break;
            }
        }
        // Resource usage capture: color attachments are writes
        for (uint32_t i = 0; i < desc.colorTargetCount; ++i)
        {
            TextureHandle th = desc.colorTargets[i];
            if (th != INVALID_HANDLE)
            {
                UsedResource u{};
                u.ResourceType = UsedResource::Type::Texture;
                u.Id = th;
                u.Writes = true;
                u.StageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                u.AccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                u.Layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                // Honor subresource when using explicit view for attachment i
                u.BaseMip = desc.useColorView[i] ? desc.colorViewDesc[i].baseMip : 0;
                u.LevelCount = desc.useColorView[i] ? (desc.colorViewDesc[i].levelCount == 0 ? 1u : desc.colorViewDesc[i].levelCount) : 1u;
                u.BaseLayer = desc.useColorView[i] ? desc.colorViewDesc[i].baseLayer : 0;
                u.LayerCount = desc.useColorView[i] ? (desc.colorViewDesc[i].layerCount == 0 ? 1u : desc.colorViewDesc[i].layerCount) : 1u;
                u.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                m_UsedResources.push_back(u);
            }
        }
        if (desc.depthTarget != INVALID_HANDLE)
        {
            UsedResource u{};
            u.ResourceType = UsedResource::Type::Texture;
            u.Id = desc.depthTarget;
            u.Writes = true;
            u.StageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            u.AccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            u.Layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            u.BaseMip = desc.useDepthView ? desc.depthViewDesc.baseMip : 0;
            u.LevelCount = desc.useDepthView ? (desc.depthViewDesc.levelCount == 0 ? 1u : desc.depthViewDesc.levelCount) : 1u;
            u.BaseLayer = desc.useDepthView ? desc.depthViewDesc.baseLayer : 0;
            u.LayerCount = desc.useDepthView ? (desc.depthViewDesc.layerCount == 0 ? 1u : desc.depthViewDesc.layerCount) : 1u;
            u.AspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; // conservative
            m_UsedResources.push_back(u);
        }

        return;
    }

    // Track final present for manual barrier after end
    this->m_LastRenderPassWasPresent = (desc.colorTargetCount == 1) &&
                                       (desc.colorTargets[0] != INVALID_HANDLE) &&
                                       m_Device->IsSwapchainTextureHandle(desc.colorTargets[0]) &&
                                       !hasDepth;
    this->m_LastRenderPassImage = this->m_LastRenderPassWasPresent ? m_Device->GetVkImage(desc.colorTargets[0]) : VK_NULL_HANDLE;
    if (framebuffer == VK_NULL_HANDLE)
    {
        return;
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {width, height};

    // Build clear values in the same order as attachments
    std::vector<VkClearValue> clearValues;
    // Include placeholders for resolve attachments on legacy path to match attachment count
    clearValues.reserve(colorCount + (hasDepth ? 1u : 0u) + legacyExtraAttachments);
    for (uint32_t i = 0; i < colorCount; ++i)
    {
        VkClearValue cv{};
        cv.color.float32[0] = desc.clearColorValue[i][0];
        cv.color.float32[1] = desc.clearColorValue[i][1];
        cv.color.float32[2] = desc.clearColorValue[i][2];
        cv.color.float32[3] = desc.clearColorValue[i][3];
        clearValues.push_back(cv);
    }
    if (hasDepth)
    {
        VkClearValue dv{};
        dv.depthStencil.depth = desc.clearDepthValue;
        dv.depthStencil.stencil = desc.clearStencilValue;
        clearValues.push_back(dv);
    }
    for (uint32_t i = 0; i < legacyExtraAttachments; ++i)
    {
        VkClearValue dummy{};
        clearValues.push_back(dummy);
    }
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    if (m_LegacyOneOffUsesRP2)
    {
        VkSubpassBeginInfo subpassBegin{};
        subpassBegin.sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO;
        subpassBegin.contents = VK_SUBPASS_CONTENTS_INLINE;
        vkCmdBeginRenderPass2(m_CommandBuffer, &renderPassInfo, &subpassBegin);
    }
    else
    {
        vkCmdBeginRenderPass(m_CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    }
    m_LegacyRenderPassBegun = true;
}

void VulkanCommandList::EndRenderPass()
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    // End appropriate render context depending on what was actually begun
    if (m_DynamicRenderingBegun)
    {
        auto endRendering = m_Device->GetCmdEndRendering();
        if (!endRendering)
        {
            Logger::Log::Error("VulkanCommandList: dynamic rendering active but vkCmdEndRendering entry point is unavailable");
            return;
        }
        endRendering(m_CommandBuffer);
        m_DynamicRenderingBegun = false;
        m_DepthReadOnly = false;
    }
    if (m_LegacyRenderPassBegun)
    {
        if (m_LegacyOneOffUsesRP2)
        {
            VkSubpassEndInfo subpassEnd{};
            subpassEnd.sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO;
            vkCmdEndRenderPass2(m_CommandBuffer, &subpassEnd);
        }
        else
        {
            vkCmdEndRenderPass(m_CommandBuffer);
        }
        m_LegacyRenderPassBegun = false;
        if (m_LegacySwapchainAttachment.IsValid())
        {
            m_SubresourceLayouts[MakeSubKey(m_LegacySwapchainAttachment, 0, 0)] =
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            m_Device->SetTrackedSwapchainImageLayout(m_LegacySwapchainAttachment, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            m_LegacySwapchainAttachment = INVALID_HANDLE;
        }
    }

    m_LegacyOneOffUsesRP2 = false;

    m_LastMarkerName.clear();

    m_CurrentPipelineHandle = PipelineHandle{};
    m_CurrentPipelineLayout = VK_NULL_HANDLE;
    m_CurrentBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    m_CurrentRenderingColorFormats.clear();
    m_CurrentColorTargetCount = 0;

    // Present transition is handled at the device Present() stage
    m_LastRenderPassWasPresent = false;
    m_LastRenderPassImage = VK_NULL_HANDLE;
}

void VulkanCommandList::SetViewport(float x, float y, float width, float height)
{

    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    VkViewport viewport{};
    viewport.x = x;
    // Prefer +Y up when supported via negative viewport height
    if (m_Device->SupportsNegativeViewportHeight())
    {
        viewport.y = y + height;
        viewport.height = -height;
    }
    else
    {
        viewport.y = y;
        viewport.height = height;
    }
    viewport.width = width;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vkCmdSetViewport(m_CommandBuffer, 0, 1, &viewport);
}

void VulkanCommandList::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    // Guard against accidental negative coordinates flowing through signed->unsigned
    // conversions (e.g., callers passing int screen-space values). Vulkan requires
    // non-negative offsets, so clamp here as a last line of defense.
    int32_t sx = static_cast<int32_t>(x);
    int32_t sy = static_cast<int32_t>(y);
    if (sx < 0)
    {
        sx = 0;
    }
    if (sy < 0)
    {
        sy = 0;
    }

    VkRect2D scissor{};
    scissor.offset.x = sx;
    scissor.offset.y = sy;
    scissor.extent.width = width;
    scissor.extent.height = height;

    vkCmdSetScissor(m_CommandBuffer, 0, 1, &scissor);
}

void VulkanCommandList::SetDepthBounds(float minDepth, float maxDepth)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
    vkCmdSetDepthBounds(m_CommandBuffer, minDepth, maxDepth);
}

void VulkanCommandList::CopyBuffer(BufferHandle src, BufferHandle dst, size_t size, size_t srcOffset, size_t dstOffset)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    VkBuffer srcBuffer = m_Device->GetVkBuffer(src);
    VkBuffer dstBuffer = m_Device->GetVkBuffer(dst);

    if (srcBuffer != VK_NULL_HANDLE && dstBuffer != VK_NULL_HANDLE)
    {
        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = srcOffset;
        copyRegion.dstOffset = dstOffset;
        copyRegion.size = size;

        vkCmdCopyBuffer(m_CommandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
        // Track resource usage (coarse): src read, dst write
        if (src != INVALID_HANDLE)
        {
            UsedResource u{};
            u.ResourceType = UsedResource::Type::Buffer;
            u.Id = src;
            u.Writes = false;
            u.StageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            u.AccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            u.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
            m_UsedResources.push_back(u);
        }
        if (dst != INVALID_HANDLE)
        {
            UsedResource u{};
            u.ResourceType = UsedResource::Type::Buffer;
            u.Id = dst;
            u.Writes = true;
            u.StageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            u.AccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            u.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
            m_UsedResources.push_back(u);
            // Subresource-aware tracking is not applicable to buffers
        }
    }
}

void VulkanCommandList::FillBuffer(BufferHandle dst, size_t offset, size_t size, uint32_t value)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
        return;
    VkBuffer dstBuffer = m_Device->GetVkBuffer(dst);
    if (dstBuffer == VK_NULL_HANDLE)
        return;

    vkCmdFillBuffer(m_CommandBuffer, dstBuffer, static_cast<VkDeviceSize>(offset),
                    static_cast<VkDeviceSize>(size), value);

    if (dst != INVALID_HANDLE)
    {
        UsedResource u{};
        u.ResourceType = UsedResource::Type::Buffer;
        u.Id = dst;
        u.Writes = true;
        u.StageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        u.AccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        u.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_UsedResources.push_back(u);
    }
}

void VulkanCommandList::CopyTexture(TextureHandle src, TextureHandle dst)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    VkImage srcImage = m_Device->GetVkImage(src);
    VkImage dstImage = m_Device->GetVkImage(dst);

    if (srcImage != VK_NULL_HANDLE && dstImage != VK_NULL_HANDLE)
    {
        if (TextureUsagePolicy::IsAuditEnabled())
        {
            TextureUsagePolicy::RecordTransferUse(src, TextureUsage::TransferSrc, "CopyTexture(src)");
            TextureUsagePolicy::RecordTransferUse(dst, TextureUsage::TransferDst, "CopyTexture(dst)");
        }

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = GetVkImageAspectFlags(src);
        copyRegion.srcSubresource.mipLevel = 0;
        copyRegion.srcSubresource.baseArrayLayer = 0;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.srcOffset = {0, 0, 0};

        copyRegion.dstSubresource.aspectMask = GetVkImageAspectFlags(dst);
        copyRegion.dstSubresource.mipLevel = 0;
        copyRegion.dstSubresource.baseArrayLayer = 0;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.dstOffset = {0, 0, 0};

        // Use actual texture extent
        VkExtent3D e = m_Device->GetVkImageExtent(dst);
        if (e.width == 0 || e.height == 0)
        {
            e = m_Device->GetVkImageExtent(src);
        }
        copyRegion.extent = {e.width, e.height, 1};

        vkCmdCopyImage(m_CommandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    }
    // Track resource usage (coarse): src read, dst write
    if (src != INVALID_HANDLE)
    {
        UsedResource u{};
        u.ResourceType = UsedResource::Type::Texture;
        u.Id = src;
        u.Writes = false;
        u.StageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        u.AccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        u.Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        u.BaseMip = 0;
        u.LevelCount = 1;
        u.BaseLayer = 0;
        u.LayerCount = 1;
        u.AspectMask = GetVkImageAspectFlags(src);
        m_UsedResources.push_back(u);
    }
    if (dst != INVALID_HANDLE)
    {
        UsedResource u{};
        u.ResourceType = UsedResource::Type::Texture;
        u.Id = dst;
        u.Writes = true;
        u.StageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        u.AccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        u.Layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        u.BaseMip = 0;
        u.LevelCount = 1;
        u.BaseLayer = 0;
        u.LayerCount = 1;
        u.AspectMask = GetVkImageAspectFlags(dst);
        m_UsedResources.push_back(u);
    }
}

void VulkanCommandList::BeginEvent(const char* name)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    const auto& fns = m_Device->GetDebugUtilsLabelFns();
    if (!fns.Available())
    {
        return;
    }

    VkDebugUtilsLabelEXT labelInfo{};
    labelInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    labelInfo.pLabelName = name;
    labelInfo.color[0] = 1.0f;
    labelInfo.color[1] = 1.0f;
    labelInfo.color[2] = 0.0f;
    labelInfo.color[3] = 1.0f;

    fns.Begin(m_CommandBuffer, &labelInfo);
}

void VulkanCommandList::EndEvent()
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    const auto& fns = m_Device->GetDebugUtilsLabelFns();
    if (!fns.Available())
    {
        return;
    }

    fns.End(m_CommandBuffer);
}

namespace
{
// Global ring of the most recent GPU debug markers across all command lists.
// See VulkanCommandList.h for the semantics/caveats.
//
// Sized in MARKERS, but what matters is the PASSES it covers: the render graph
// brackets each pass with a start and an end marker, so the ring reaches back
// half as many passes as it holds entries.
constexpr size_t kDiagMarkerRingSize = 128;
std::mutex g_DiagMarkerMutex;
std::array<std::string, kDiagMarkerRingSize> g_DiagMarkerRing;
size_t g_DiagMarkerHead = 0;
uint64_t g_DiagMarkerCount = 0;
} // namespace

void RecordDiagnosticMarker(const char* name)
{
    if (!name || name[0] == '\0')
        return;
    std::lock_guard<std::mutex> lock(g_DiagMarkerMutex);
    g_DiagMarkerRing[g_DiagMarkerHead] = name;
    g_DiagMarkerHead = (g_DiagMarkerHead + 1) % kDiagMarkerRingSize;
    ++g_DiagMarkerCount;
}

std::string DumpRecentDiagnosticMarkers()
{
    std::lock_guard<std::mutex> lock(g_DiagMarkerMutex);
    const size_t count = static_cast<size_t>(
        g_DiagMarkerCount < kDiagMarkerRingSize ? g_DiagMarkerCount : kDiagMarkerRingSize);
    if (count == 0)
        return "<none recorded>";
    const size_t start = (g_DiagMarkerHead + kDiagMarkerRingSize - count) % kDiagMarkerRingSize;
    std::string out;
    for (size_t i = 0; i < count; ++i)
    {
        if (!out.empty())
            out += " -> ";
        out += g_DiagMarkerRing[(start + i) % kDiagMarkerRingSize];
    }
    return out;
}

void VulkanCommandList::SetMarker(const char* name)
{
    // Feed the global diagnostic ring even before the recording guard: a marker
    // is worth capturing for the device-lost dump regardless of this list's state.
    RecordDiagnosticMarker(name);

    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    // Track last marker for CPU-side diagnostics
    if (name)
    {
        m_LastMarkerName = name;
    }

    // Scoped rather than an early return: the checkpoint below is a separate
    // instrument on a separate extension, and it must still fire on a device where
    // debug_utils is unavailable.
    if (const auto& fns = m_Device->GetDebugUtilsLabelFns(); fns.Available())
    {
        VkDebugUtilsLabelEXT labelInfo{};
        labelInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        labelInfo.pLabelName = name;
        labelInfo.color[0] = 1.0f;
        labelInfo.color[1] = 0.0f;
        labelInfo.color[2] = 1.0f;
        labelInfo.color[3] = 1.0f;

        fns.Insert(m_CommandBuffer, &labelInfo);
    }

    // GPU-side counterpart of the ring above: the ring says how far RECORDING
    // got, a checkpoint says how far EXECUTION got, and the gap between them
    // brackets the work that faulted. Gated at the call site rather than inside
    // EmitGpuCheckpoint so a marker pays one bool load when checkpoints are off.
    if (m_Device->GpuCheckpointsEnabled())
    {
        m_Device->EmitGpuCheckpoint(m_CommandBuffer, name);
    }
}

void VulkanCommandList::Barrier(const ResourceBarrier& barrier)
{
    // Delegate to the batched implementation to avoid code duplication.
    // The overhead of creating a single-element vector is negligible compared to driver overhead,
    // and this ensures consistent behavior (synchronization logic, debug reporting, layout tracking)
    // between individual and batched barriers.
    BarrierBatch({barrier});
}

void VulkanCommandList::BarrierBatch(const std::vector<ResourceBarrier>& barriers)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    // Helper lambdas for translation (shared between legacy and Sync2 paths)
    auto resolveStage = [&](ResourceState s, uint64_t explicitMask) -> VkPipelineStageFlags
    {
        if (explicitMask == 0)
        {
            return VulkanMappings::TranslateResourceStateToStage(s);
        }
        return VulkanMappings::TranslatePipelineStageMask(explicitMask);
    };
    auto resolveAccess = [&](ResourceState s, uint64_t explicitMask) -> VkAccessFlags
    {
        if (explicitMask == 0)
        {
            return VulkanMappings::TranslateResourceStateToAccess(s);
        }
        return VulkanMappings::TranslateResourceAccessMask(explicitMask);
    };
    // Clamp a barrier's SOURCE stage mask to what this command buffer's queue
    // family can express. A src stage the queue lacks (e.g. FRAGMENT_SHADER on a
    // compute queue) can only have come from a producer on ANOTHER queue — a
    // same-queue producer's stages are queue-valid by construction — and that
    // cross-queue dependency is carried by the submission's timeline-semaphore
    // wait, not by this barrier. Masking the foreign stage out is therefore a
    // correctness-preserving floor that keeps an illegal stage off the queue
    // (VUID-vkCmdPipelineBarrier2-srcStageMask-09675), independent of render-graph
    // queue bookkeeping. The graphics (universal) queue supports every stage.
    auto clampSrcStageToQueue = [&](VkPipelineStageFlags stage) -> VkPipelineStageFlags
    {
        if (m_QueueType == IDevice::QueueType::Graphics)
            return stage;
        VkPipelineStageFlags allowed = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
                                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT |
                                       VK_PIPELINE_STAGE_HOST_BIT |
                                       VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (m_QueueType == IDevice::QueueType::Compute)
            allowed |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        return stage & allowed;
    };
    // Applies the floor and drops the source access scope with it when nothing
    // survives — a foreign producer's writes are made available by the timeline
    // wait, so keeping its access bits against an empty stage mask would name a
    // dependency this barrier does not express.
    //
    // On a RENDER-GRAPH barrier the floor must never be load-bearing: the graph
    // sanitizes the source scope at the producing submission's tail, so a clamp
    // here means it derived an edge this queue cannot name. Debug builds report
    // that rather than repairing it quietly. Barriers recorded outside the graph
    // (a transfer-queue upload naming ShaderResource, say) are clamped by design
    // and stay silent.
    auto applySrcQueueFloor = [&](const ResourceBarrier& barrier, VkPipelineStageFlags& srcStage,
                                  VkAccessFlags& srcAccess)
    {
        const VkPipelineStageFlags clamped = clampSrcStageToQueue(srcStage);
        if (clamped == srcStage)
            return;
#if !defined(NDEBUG)
        if (barrier.rgAuthoritativeLayout)
        {
            static thread_local uint64_t s_GraphClampCount = 0;
            if (s_GraphClampCount++ % kGraphSrcClampLogStride == 0)
            {
                Logger::Log::Warning(
                    "VulkanCommandList: render-graph barrier src stage 0x{:x} is not expressible on "
                    "queue type {} (floored to 0x{:x}) — the graph derived a source scope this queue "
                    "cannot name. Occurrence {}.",
                    static_cast<uint64_t>(srcStage), static_cast<int>(m_QueueType),
                    static_cast<uint64_t>(clamped), s_GraphClampCount);
            }
        }
#else
        (void)barrier;
#endif
        if (clamped == 0)
            srcAccess = 0;
        srcStage = clamped;
    };
    // The single oldLayout rule, shared by the sync2 and sync1 paths:
    //   graph-owned texture -> compiled stateBefore verbatim. The render graph's
    //       cell grid is the per-subresource layout authority (it compiled the
    //       producer's end layout even across submissions and queues); Undefined
    //       is a deliberate discard.
    //   swapchain image     -> this CB's journal, else the device's acquire/
    //       present tracker (acquire happens outside the graph).
    //   non-graph surface   -> this CB's journal, else the flat per-image
    //       tracker (skipped when the caller declared Undefined — discard
    //       intent). A surface neither tracker knows resolves UNDEFINED: the
    //       first-use transition discards, the only safe claim for an image
    //       whose actual layout nobody recorded. The caller's declared state
    //       is deliberately NOT a fallback here — trusting an unverifiable
    //       claim over a discard would preserve garbage as if it were content.
    auto resolveOldLayout = [&](const ResourceBarrier& barrier, bool isSwapchainImage) -> VkImageLayout
    {
        if (barrier.rgAuthoritativeLayout && !isSwapchainImage)
            return VulkanMappings::TranslateResourceStateToLayout(barrier.stateBefore);
        VkImageLayout resolved = VK_IMAGE_LAYOUT_UNDEFINED;
        {
            const uint64_t key = MakeSubKey(barrier.textureHandle, barrier.subresource.baseMip,
                                            barrier.subresource.baseLayer);
            auto it = m_SubresourceLayouts.find(key);
            if (it != m_SubresourceLayouts.end())
                resolved = it->second;
        }
        if (isSwapchainImage)
        {
            if (resolved == VK_IMAGE_LAYOUT_UNDEFINED)
                resolved = m_Device->GetTrackedSwapchainImageLayout(barrier.textureHandle);
            return resolved;
        }
        const bool wantsUndefined = (barrier.stateBefore == ResourceState::Undefined);
        if (resolved == VK_IMAGE_LAYOUT_UNDEFINED && !wantsUndefined)
        {
            if (const VulkanTexture* tex = GetVulkanTextureConst(barrier.textureHandle))
                resolved = tex->trackedLayout;
        }
        return resolved;
    };

    if (!m_Device->SupportsSynchronization2())
    {
        // Legacy path: batch into a single vkCmdPipelineBarrier call
        // PERF: static thread_local to retain capacity (see sync2 path comment).
        static thread_local std::vector<VkMemoryBarrier> s_MemBarriersLegacy;
        static thread_local std::vector<VkBufferMemoryBarrier> s_BufBarriersLegacy;
        static thread_local std::vector<VkImageMemoryBarrier> s_ImgBarriersLegacy;
        auto& memBarriers = s_MemBarriersLegacy;
        auto& bufBarriers = s_BufBarriersLegacy;
        auto& imgBarriers = s_ImgBarriersLegacy;
        memBarriers.clear();
        bufBarriers.clear();
        imgBarriers.clear();
        memBarriers.reserve(barriers.size());
        bufBarriers.reserve(barriers.size());
        imgBarriers.reserve(barriers.size());

        VkPipelineStageFlags srcStageMaskTotal = 0;
        VkPipelineStageFlags dstStageMaskTotal = 0;

        for (const auto& barrier : barriers)
        {
            VkPipelineStageFlags srcStage = resolveStage(barrier.stateBefore, barrier.srcStageMask);
            VkPipelineStageFlags dstStage = resolveStage(barrier.stateAfter, barrier.dstStageMask);
            VkAccessFlags srcAccess = resolveAccess(barrier.stateBefore, barrier.srcAccessMask);
            VkAccessFlags dstAccess = resolveAccess(barrier.stateAfter, barrier.dstAccessMask);
            applySrcQueueFloor(barrier, srcStage, srcAccess);

            // Accumulate stage masks for the global barrier call
            srcStageMaskTotal |= srcStage;
            dstStageMaskTotal |= dstStage;

            if (barrier.type == ResourceBarrier::Texture)
            {
                VkImage image = m_Device->GetVkImage(barrier.textureHandle);
                if (image == VK_NULL_HANDLE)
                    continue;

                // Swapchain fresh from acquire (Undefined): force safe
                // synchronization points (acquire happens outside the graph).
                const bool isSwapchainImage = m_Device->IsSwapchainTextureHandle(barrier.textureHandle);
                if (isSwapchainImage && barrier.stateBefore == ResourceState::Undefined)
                {
                    srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                    srcAccess = 0;
                }

                VkImageMemoryBarrier imgBarrier{};
                imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                imgBarrier.srcAccessMask = srcAccess;
                imgBarrier.dstAccessMask = dstAccess;
                imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                imgBarrier.image = image;
                imgBarrier.subresourceRange.aspectMask = GetVkImageAspectFlags(barrier.textureHandle);
                imgBarrier.subresourceRange.baseMipLevel = barrier.subresource.baseMip;
                imgBarrier.subresourceRange.levelCount = barrier.subresource.levelCount;
                imgBarrier.subresourceRange.baseArrayLayer = barrier.subresource.baseLayer;
                imgBarrier.subresourceRange.layerCount = barrier.subresource.layerCount;

                imgBarrier.oldLayout = resolveOldLayout(barrier, isSwapchainImage);
                imgBarrier.newLayout = VulkanMappings::TranslateResourceStateToLayout(barrier.stateAfter);

                WatchImageBarrierIfWatched(imgBarrier.image, imgBarrier.oldLayout,
                                           imgBarrier.newLayout, imgBarrier.subresourceRange,
                                           "BarrierBatch/legacy");
                imgBarriers.push_back(imgBarrier);

                // Journal the new layout: consulted by non-graph resolution and by
                // utility ops recorded on this CB (readback copies, mip-gen) that
                // interleave with graph work — never by graph-owned barriers.
                for (uint32_t mip = barrier.subresource.baseMip; mip < barrier.subresource.baseMip + barrier.subresource.levelCount; ++mip)
                {
                    for (uint32_t layer = barrier.subresource.baseLayer; layer < barrier.subresource.baseLayer + barrier.subresource.layerCount; ++layer)
                    {
                        uint64_t key = MakeSubKey(barrier.textureHandle, mip, layer);
                        m_SubresourceLayouts[key] = imgBarrier.newLayout;
                    }
                }
                if (isSwapchainImage)
                {
                    m_Device->SetTrackedSwapchainImageLayout(barrier.textureHandle, imgBarrier.newLayout);
                }
                else
                {
                    if (VulkanTexture* tex = GetVulkanTexture(barrier.textureHandle))
                    {
                        // Flat tracker: whole-image updates only — a per-layer/mip
                        // barrier must not clobber other subresources' state. Best-
                        // effort cross-frame seed for non-graph surfaces; graph-owned
                        // barriers never read it (per-subresource truth is the RG's).
                        const bool coversAllLayers = (barrier.subresource.baseLayer == 0 &&
                                                      barrier.subresource.layerCount >= tex->arrayLayers);
                        const bool coversAllMips   = (barrier.subresource.baseMip == 0 &&
                                                      barrier.subresource.levelCount >= tex->mipLevels);
                        if (coversAllLayers && coversAllMips)
                            tex->trackedLayout = imgBarrier.newLayout;
                    }
                }
            }
            else if (barrier.type == ResourceBarrier::Buffer)
            {
                VkBuffer vkBuffer = m_Device->GetVkBuffer(barrier.bufferHandle);
                if (vkBuffer == VK_NULL_HANDLE)
                    continue;

                VkBufferMemoryBarrier bufBarrier{};
                bufBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                bufBarrier.srcAccessMask = srcAccess;
                bufBarrier.dstAccessMask = dstAccess;
                bufBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bufBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bufBarrier.buffer = vkBuffer;
                bufBarrier.offset = 0;
                bufBarrier.size = VK_WHOLE_SIZE;
                bufBarriers.push_back(bufBarrier);
            }
            else if (barrier.type == ResourceBarrier::Memory)
            {
                VkMemoryBarrier memBarrier{};
                memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                memBarrier.srcAccessMask = srcAccess;
                memBarrier.dstAccessMask = dstAccess;
                memBarriers.push_back(memBarrier);
            }
        }

        if (srcStageMaskTotal == 0)
            srcStageMaskTotal = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        if (dstStageMaskTotal == 0)
            dstStageMaskTotal = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

        if (!imgBarriers.empty() || !bufBarriers.empty() || !memBarriers.empty())
        {
            vkCmdPipelineBarrier(
                m_CommandBuffer,
                srcStageMaskTotal, dstStageMaskTotal,
                0,
                static_cast<uint32_t>(memBarriers.size()), memBarriers.data(),
                static_cast<uint32_t>(bufBarriers.size()), bufBarriers.data(),
                static_cast<uint32_t>(imgBarriers.size()), imgBarriers.data());
        }
        return;
    }

    // Sync2 Path
    // PERF: static thread_local so vectors retain capacity across frames.
    // VulkanCommandList is recreated each frame (make_unique), so member
    // scratch vectors would start empty every time.
    static thread_local std::vector<VkImageMemoryBarrier2> s_ImgBarriers;
    static thread_local std::vector<VkBufferMemoryBarrier2> s_BufBarriers;
    static thread_local std::vector<VkMemoryBarrier2> s_MemBarriers;
    auto& imgBarriers = s_ImgBarriers;
    auto& bufBarriers = s_BufBarriers;
    auto& memBarriers = s_MemBarriers;
    imgBarriers.clear();
    bufBarriers.clear();
    memBarriers.clear();

    for (const auto& barrier : barriers)
    {
        if (barrier.type == ResourceBarrier::Texture)
        {
            VkImage image = m_Device->GetVkImage(barrier.textureHandle);
            if (image == VK_NULL_HANDLE)
                continue;
            VkPipelineStageFlags srcStage = resolveStage(barrier.stateBefore, barrier.srcStageMask);
            VkPipelineStageFlags dstStage = resolveStage(barrier.stateAfter, barrier.dstStageMask);
            VkAccessFlags srcAccess = resolveAccess(barrier.stateBefore, barrier.srcAccessMask);
            VkAccessFlags dstAccess = resolveAccess(barrier.stateAfter, barrier.dstAccessMask);
            applySrcQueueFloor(barrier, srcStage, srcAccess);
            // Swapchain fresh from acquire (Undefined): force safe
            // synchronization points (acquire happens outside the graph).
            const bool isSwapchainImage = m_Device->IsSwapchainTextureHandle(barrier.textureHandle);
            if (isSwapchainImage && barrier.stateBefore == ResourceState::Undefined)
            {
                srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                srcAccess = 0;
            }

            VkImageMemoryBarrier2 img2{};
            img2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            img2.srcStageMask = srcStage;
            img2.srcAccessMask = srcAccess;
            img2.dstStageMask = dstStage;
            img2.dstAccessMask = dstAccess;
            img2.oldLayout = resolveOldLayout(barrier, isSwapchainImage);
            img2.newLayout = VulkanMappings::TranslateResourceStateToLayout(barrier.stateAfter);
            img2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            img2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            img2.image = image;
            img2.subresourceRange.aspectMask = GetVkImageAspectFlags(barrier.textureHandle);
            img2.subresourceRange.baseMipLevel = barrier.subresource.baseMip;
            img2.subresourceRange.levelCount = barrier.subresource.levelCount;
            img2.subresourceRange.baseArrayLayer = barrier.subresource.baseLayer;
            img2.subresourceRange.layerCount = barrier.subresource.layerCount;

            WatchImageBarrierIfWatched(img2.image, img2.oldLayout, img2.newLayout,
                                       img2.subresourceRange, "BarrierBatch/sync2");
            imgBarriers.push_back(img2);

#if GE_ENABLE_DEBUG_BARRIERS
            // Debug capture per barrier
            VulkanDevice::DebugBarrierInfo dbg{};
            dbg.isImage = true;
            dbg.image2 = img2;
            m_Device->PushDebugBarrier(dbg);
            if (img2.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED || img2.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED)
            {
                m_Device->DebugIncOwnershipTransfers();
            }
#endif

            // Journal the new layout: consulted by non-graph resolution and by
            // utility ops recorded on this CB (readback copies, mip-gen) that
            // interleave with graph work — never by graph-owned barriers.
            for (uint32_t mip = barrier.subresource.baseMip; mip < barrier.subresource.baseMip + barrier.subresource.levelCount; ++mip)
            {
                for (uint32_t layer = barrier.subresource.baseLayer; layer < barrier.subresource.baseLayer + barrier.subresource.layerCount; ++layer)
                {
                    uint64_t key = MakeSubKey(barrier.textureHandle, mip, layer);
                    m_SubresourceLayouts[key] = img2.newLayout;
                }
            }
            if (isSwapchainImage)
            {
                m_Device->SetTrackedSwapchainImageLayout(barrier.textureHandle, img2.newLayout);
            }
            else
            {
                if (VulkanTexture* tex = GetVulkanTexture(barrier.textureHandle))
                {
                    // Flat tracker: whole-image updates only — a per-layer/mip
                    // barrier must not clobber other subresources' state. Best-
                    // effort cross-frame seed for non-graph surfaces; graph-owned
                    // barriers never read it (per-subresource truth is the RG's).
                    const bool coversAllLayers = (barrier.subresource.baseLayer == 0 &&
                                                  barrier.subresource.layerCount >= tex->arrayLayers);
                    const bool coversAllMips   = (barrier.subresource.baseMip == 0 &&
                                                  barrier.subresource.levelCount >= tex->mipLevels);
                    if (coversAllLayers && coversAllMips)
                        tex->trackedLayout = img2.newLayout;
                }
            }
        }
        else if (barrier.type == ResourceBarrier::Buffer)
        {
            VkBuffer vkBuffer = m_Device->GetVkBuffer(barrier.bufferHandle);
            if (vkBuffer == VK_NULL_HANDLE)
            {
                continue;
            }
            VkPipelineStageFlags srcStage = resolveStage(barrier.stateBefore, barrier.srcStageMask);
            VkPipelineStageFlags dstStage = resolveStage(barrier.stateAfter, barrier.dstStageMask);
            VkAccessFlags srcAccess = resolveAccess(barrier.stateBefore, barrier.srcAccessMask);
            VkAccessFlags dstAccess = resolveAccess(barrier.stateAfter, barrier.dstAccessMask);
            applySrcQueueFloor(barrier, srcStage, srcAccess);

            VkBufferMemoryBarrier2 buf2{};
            buf2.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            buf2.srcStageMask = srcStage;
            buf2.srcAccessMask = srcAccess;
            buf2.dstStageMask = dstStage;
            buf2.dstAccessMask = dstAccess;
            buf2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf2.buffer = vkBuffer;
            buf2.offset = 0;
            buf2.size = VK_WHOLE_SIZE;

            bufBarriers.push_back(buf2);

#if GE_ENABLE_DEBUG_BARRIERS
            VulkanDevice::DebugBarrierInfo dbg{};
            dbg.isImage = false;
            dbg.buffer2 = buf2;
            m_Device->PushDebugBarrier(dbg);
            if (buf2.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED || buf2.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED)
            {
                m_Device->DebugIncOwnershipTransfers();
            }
#endif
        }
        else if (barrier.type == ResourceBarrier::Memory)
        {
            VkPipelineStageFlags srcStage = resolveStage(barrier.stateBefore, barrier.srcStageMask);
            VkPipelineStageFlags dstStage = resolveStage(barrier.stateAfter, barrier.dstStageMask);
            VkAccessFlags srcAccess = resolveAccess(barrier.stateBefore, barrier.srcAccessMask);
            VkAccessFlags dstAccess = resolveAccess(barrier.stateAfter, barrier.dstAccessMask);
            applySrcQueueFloor(barrier, srcStage, srcAccess);

            VkMemoryBarrier2 mem2{};
            mem2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            mem2.srcStageMask = srcStage;
            mem2.srcAccessMask = srcAccess;
            mem2.dstStageMask = dstStage;
            mem2.dstAccessMask = dstAccess;
            memBarriers.push_back(mem2);
        }
    }

    // Optional coalescing: merge image barriers that differ only by aspectMask
    if (!imgBarriers.empty())
    {
        auto sameKey = [](const VkImageMemoryBarrier2& a, const VkImageMemoryBarrier2& b)
        {
            return a.image == b.image &&
                   a.oldLayout == b.oldLayout && a.newLayout == b.newLayout &&
                   a.srcStageMask == b.srcStageMask && a.dstStageMask == b.dstStageMask &&
                   a.srcAccessMask == b.srcAccessMask && a.dstAccessMask == b.dstAccessMask &&
                   a.srcQueueFamilyIndex == b.srcQueueFamilyIndex && a.dstQueueFamilyIndex == b.dstQueueFamilyIndex &&
                   a.subresourceRange.baseMipLevel == b.subresourceRange.baseMipLevel &&
                   a.subresourceRange.levelCount == b.subresourceRange.levelCount &&
                   a.subresourceRange.baseArrayLayer == b.subresourceRange.baseArrayLayer &&
                   a.subresourceRange.layerCount == b.subresourceRange.layerCount;
        };
        std::sort(imgBarriers.begin(), imgBarriers.end(), [&](const VkImageMemoryBarrier2& a, const VkImageMemoryBarrier2& b)
                  {
            if (a.image != b.image) return a.image < b.image;
            if (a.oldLayout != b.oldLayout) return a.oldLayout < b.oldLayout;
            if (a.newLayout != b.newLayout) return a.newLayout < b.newLayout;
            if (a.srcStageMask != b.srcStageMask) return a.srcStageMask < b.srcStageMask;
            if (a.dstStageMask != b.dstStageMask) return a.dstStageMask < b.dstStageMask;
            if (a.srcAccessMask != b.srcAccessMask) return a.srcAccessMask < b.srcAccessMask;
            if (a.dstAccessMask != b.dstAccessMask) return a.dstAccessMask < b.dstAccessMask;
            if (a.srcQueueFamilyIndex != b.srcQueueFamilyIndex) return a.srcQueueFamilyIndex < b.srcQueueFamilyIndex;
            if (a.dstQueueFamilyIndex != b.dstQueueFamilyIndex) return a.dstQueueFamilyIndex < b.dstQueueFamilyIndex;
            if (a.subresourceRange.baseMipLevel != b.subresourceRange.baseMipLevel) return a.subresourceRange.baseMipLevel < b.subresourceRange.baseMipLevel;
            if (a.subresourceRange.levelCount != b.subresourceRange.levelCount) return a.subresourceRange.levelCount < b.subresourceRange.levelCount;
            if (a.subresourceRange.baseArrayLayer != b.subresourceRange.baseArrayLayer) return a.subresourceRange.baseArrayLayer < b.subresourceRange.baseArrayLayer;
            if (a.subresourceRange.layerCount != b.subresourceRange.layerCount) return a.subresourceRange.layerCount < b.subresourceRange.layerCount;
            return a.subresourceRange.aspectMask < b.subresourceRange.aspectMask; });
        auto& merged = m_ScratchMergedImgBarriers2;
        merged.clear();
        merged.reserve(imgBarriers.size());
        for (const auto& ib : imgBarriers)
        {
            if (!merged.empty() && sameKey(merged.back(), ib))
            {
                merged.back().subresourceRange.aspectMask |= ib.subresourceRange.aspectMask;
            }
            else
            {
                merged.push_back(ib);
            }
        }
        imgBarriers.swap(merged);
    }

    // Deduplicate identical buffer barriers (same buffer and sync params)
    if (!bufBarriers.empty())
    {
        std::sort(bufBarriers.begin(), bufBarriers.end(), [&](const VkBufferMemoryBarrier2& a, const VkBufferMemoryBarrier2& b)
                  {
            if (a.buffer != b.buffer) return a.buffer < b.buffer;
            if (a.offset != b.offset) return a.offset < b.offset;
            if (a.size != b.size) return a.size < b.size;
            if (a.srcStageMask != b.srcStageMask) return a.srcStageMask < b.srcStageMask;
            if (a.dstStageMask != b.dstStageMask) return a.dstStageMask < b.dstStageMask;
            if (a.srcAccessMask != b.srcAccessMask) return a.srcAccessMask < b.srcAccessMask;
            if (a.dstAccessMask != b.dstAccessMask) return a.dstAccessMask < b.dstAccessMask;
            if (a.srcQueueFamilyIndex != b.srcQueueFamilyIndex) return a.srcQueueFamilyIndex < b.srcQueueFamilyIndex;
            return a.dstQueueFamilyIndex < b.dstQueueFamilyIndex; });
        bufBarriers.erase(std::unique(bufBarriers.begin(), bufBarriers.end(), [&](const VkBufferMemoryBarrier2& a, const VkBufferMemoryBarrier2& b)
                                      { return a.buffer == b.buffer && a.offset == b.offset && a.size == b.size &&
                                               a.srcStageMask == b.srcStageMask && a.dstStageMask == b.dstStageMask &&
                                               a.srcAccessMask == b.srcAccessMask && a.dstAccessMask == b.dstAccessMask &&
                                               a.srcQueueFamilyIndex == b.srcQueueFamilyIndex && a.dstQueueFamilyIndex == b.dstQueueFamilyIndex; }),
                          bufBarriers.end());
    }

    // Deduplicate identical memory barriers
    if (!memBarriers.empty())
    {
        std::sort(memBarriers.begin(), memBarriers.end(), [&](const VkMemoryBarrier2& a, const VkMemoryBarrier2& b)
                  {
            if (a.srcStageMask != b.srcStageMask) return a.srcStageMask < b.srcStageMask;
            if (a.dstStageMask != b.dstStageMask) return a.dstStageMask < b.dstStageMask;
            if (a.srcAccessMask != b.srcAccessMask) return a.srcAccessMask < b.srcAccessMask;
            return a.dstAccessMask < b.dstAccessMask; });
        memBarriers.erase(std::unique(memBarriers.begin(), memBarriers.end(), [&](const VkMemoryBarrier2& a, const VkMemoryBarrier2& b)
                                      { return a.srcStageMask == b.srcStageMask && a.dstStageMask == b.dstStageMask &&
                                               a.srcAccessMask == b.srcAccessMask && a.dstAccessMask == b.dstAccessMask; }),
                          memBarriers.end());
    }

    if (imgBarriers.empty() && bufBarriers.empty() && memBarriers.empty())
    {
        return;
    }

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(imgBarriers.size());
    dep.pImageMemoryBarriers = imgBarriers.empty() ? nullptr : imgBarriers.data();
    // Debug: record counts for last batch
    m_Device->DebugSetLastDependencyCounts((uint32_t)imgBarriers.size(), (uint32_t)bufBarriers.size(), (uint32_t)memBarriers.size());

    dep.bufferMemoryBarrierCount = static_cast<uint32_t>(bufBarriers.size());
    dep.pBufferMemoryBarriers = bufBarriers.empty() ? nullptr : bufBarriers.data();
    dep.memoryBarrierCount = static_cast<uint32_t>(memBarriers.size());
    dep.pMemoryBarriers = memBarriers.empty() ? nullptr : memBarriers.data();

    auto fp = m_Device->GetCmdPipelineBarrier2();
    if (fp)
    {
        fp(m_CommandBuffer, &dep);
    }
    else
    {
        Logger::Log::Error("VulkanCommandList: synchronization2 barrier requested but vkCmdPipelineBarrier2 entry point is unavailable");
        return;
    }

    // Increment per-call debug counter
    m_Device->DebugIncPipelineBarrier2Calls();
}

void VulkanCommandList::CopyTextureToBuffer(TextureHandle srcTexture, BufferHandle dstBuffer,
                                            uint32_t width, uint32_t height, uint32_t SrcX, uint32_t SrcY,
                                            size_t DstOffsetBytes, size_t dstRowPitchBytes)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }
    VkImage srcImage = m_Device->GetVkImage(srcTexture);
    VkBuffer dstVkBuffer = m_Device->GetVkBuffer(dstBuffer);
    if (srcImage == VK_NULL_HANDLE || dstVkBuffer == VK_NULL_HANDLE)
        return;

    if (TextureUsagePolicy::IsAuditEnabled())
    {
        TextureUsagePolicy::RecordTransferUse(srcTexture, TextureUsage::TransferSrc, "CopyTextureToBuffer(src)");
    }

#if defined(DEBUG) || defined(_DEBUG)
    {
        VkExtent3D e = m_Device->GetVkImageExtent(srcTexture);
        uint32_t expW = e.width;
        if (expW == 0)
            expW = 1;
        uint32_t expH = e.height;
        if (expH == 0)
            expH = 1;
        if (width != expW || height != expH)
        {
        }
    }
#endif

    // Assume RenderGraph compiled barriers transitioned srcTexture to TRANSFER_SRC_OPTIMAL and ensured visibility.
    // Avoid issuing another layout transition here to prevent conflicts with the tracked known layout.
    /* Duplicate barrier block begins - commenting out to avoid overriding proper visibility barrier
            // Transition image to TRANSFER_SRC from expected prior layout based on aspect
            VkImageMemoryBarrier toSrc{};
            toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toSrc.srcAccessMask = 0;
            toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            VkImageAspectFlags aspect = GetVkImageAspectFlags(srcTexture);
            toSrc.oldLayout = (aspect & VK_IMAGE_ASPECT_COLOR_BIT)
                ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toSrc.image = srcImage;
            toSrc.subresourceRange.aspectMask = GetVkImageAspectFlags(srcTexture);
            toSrc.subresourceRange.baseMipLevel = 0;
            toSrc.subresourceRange.levelCount = 1;
            toSrc.subresourceRange.baseArrayLayer = 0;
            toSrc.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(
                m_CommandBuffer,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &toSrc
    // Track resource usage (coarse): src texture read, dst buffer write

    // Capture subresource info for the transfer read/write
    if (srcTexture != INVALID_HANDLE) {
        UsedResource u{}; u.ResourceType = UsedResource::Type::Texture; u.Id = srcTexture; u.Writes = false; u.StageMask = VK_PIPELINE_STAGE_TRANSFER_BIT; u.AccessMask = VK_ACCESS_TRANSFER_READ_BIT; u.Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        u.BaseMip = 0; u.LevelCount = 1; u.BaseLayer = 0; u.LayerCount = 1; u.AspectMask = GetVkImageAspectFlags(srcTexture);
        m_UsedResources.push_back(u);
    }
    if (dstBuffer != INVALID_HANDLE) {
        UsedResource u{}; u.ResourceType = UsedResource::Type::Buffer; u.Id = dstBuffer; u.Writes = true; u.StageMask = VK_PIPELINE_STAGE_TRANSFER_BIT; u.AccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; u.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_UsedResources.push_back(u);
    }



            );
            */

    // Prepare copy region
    VkBufferImageCopy region{};
    region.bufferOffset = DstOffsetBytes;
    // Compute rowLength in texels from dstRowPitchBytes when specified
    VkFormat vkFmt = m_Device->GetVkImageFormat(srcTexture);
    uint32_t bpp = 0;
    switch (vkFmt)
    {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        bpp = 4;
        break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        bpp = 8;
        break;
    case VK_FORMAT_R32G32_SFLOAT:
        bpp = 8;
        break;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        bpp = 16;
        break;
    case VK_FORMAT_D32_SFLOAT:
        bpp = 4;
        break; // depth copy support is limited
    case VK_FORMAT_D24_UNORM_S8_UINT:
        bpp = 4;
        break; // packed
    default:
        bpp = 0;
        break;
    }
    region.bufferRowLength = (dstRowPitchBytes && bpp) ? static_cast<uint32_t>(dstRowPitchBytes / bpp) : 0; // 0 = tightly packed
    region.bufferImageHeight = 0;
    // Use only one valid aspect for copy (depth if depth/stencil, else color)
    VkImageAspectFlags aspectCopy = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (vkFmt)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        aspectCopy = VK_IMAGE_ASPECT_DEPTH_BIT;
        break;
    default:
        aspectCopy = VK_IMAGE_ASPECT_COLOR_BIT;
        break;
    }
    region.imageSubresource.aspectMask = aspectCopy;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
#if defined(DEBUG) || defined(_DEBUG)
#endif

    region.imageOffset = {static_cast<int32_t>(SrcX), static_cast<int32_t>(SrcY), 0};
    region.imageExtent = {width, height, 1};

    // Copy image to buffer
    vkCmdCopyImageToBuffer(m_CommandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstVkBuffer, 1, &region);

    // Make transfer writes visible to host reads when the queue is idle
    {
        VkBufferMemoryBarrier toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = dstVkBuffer;
        toHost.offset = region.bufferOffset;
        VkDeviceSize approxSize = (region.bufferRowLength ? static_cast<VkDeviceSize>(region.bufferRowLength) : static_cast<VkDeviceSize>(width)) * (region.bufferImageHeight ? region.bufferImageHeight : height) * (bpp ? bpp : 4);
        toHost.size = approxSize;
        vkCmdPipelineBarrier(
            m_CommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            0, nullptr,
            1, &toHost,
            0, nullptr);
    }

    // Keep image in TRANSFER_SRC_OPTIMAL; caller/graph will transition to next use. Avoid resetting to GENERAL.
}

void VulkanCommandList::CopyTextureSubresourceToBuffer(TextureHandle srcTexture, uint32_t mip, uint32_t layer,
                                                       BufferHandle dstBuffer,
                                                       uint32_t width, uint32_t height,
                                                       uint32_t SrcX, uint32_t SrcY,
                                                       size_t DstOffsetBytes, size_t dstRowPitchBytes,
                                                       uint32_t depth, size_t dstSlicePitchBytes)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
        return;
    VkImage srcImage = m_Device->GetVkImage(srcTexture);
    VkBuffer dstVkBuffer = m_Device->GetVkBuffer(dstBuffer);
    if (srcImage == VK_NULL_HANDLE || dstVkBuffer == VK_NULL_HANDLE)
        return;

    if (TextureUsagePolicy::IsAuditEnabled())
    {
        TextureUsagePolicy::RecordTransferUse(srcTexture, TextureUsage::TransferSrc,
                                             "CopyTextureSubresourceToBuffer(src)");
    }

    // Ensure the specific subresource is in TRANSFER_SRC_OPTIMAL (use tracked layout if available)
    // We will use a DEPTH-only aspect for the copy region, but for depth-stencil formats
    // the barrier aspect must include BOTH depth and stencil unless separateDepthStencilLayouts is enabled.
    VkFormat vkFmt = m_Device->GetVkImageFormat(srcTexture);
    const bool isDepthStencil = (vkFmt == VK_FORMAT_D16_UNORM_S8_UINT) || (vkFmt == VK_FORMAT_D24_UNORM_S8_UINT) || (vkFmt == VK_FORMAT_D32_SFLOAT_S8_UINT);

    // Copy region uses a single aspect (depth for depth-stencil formats)
    VkImageAspectFlags aspectSingle = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (vkFmt)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        aspectSingle = VK_IMAGE_ASPECT_DEPTH_BIT;
        break;
    default:
        aspectSingle = VK_IMAGE_ASPECT_COLOR_BIT;
        break;
    }
    // For 3D images, baseArrayLayer must be 0 and layerCount must be 1; `layer`
    // is reinterpreted as the Z offset in the copy region below.
    const VkExtent3D srcExtent = m_Device->GetVkImageExtent(srcTexture);
    const bool is3DTexture = srcExtent.depth > 1;
    const uint32_t copyDepth = is3DTexture ? (depth ? depth : 1u) : 1u;
    if (!is3DTexture && depth != 1u)
    {
        Logger::Log::Error("CopyTextureSubresourceToBuffer: depth must be 1 for non-3D images (got {})", depth);
        return;
    }

    VkImageSubresourceRange rangeCopy{};
    rangeCopy.aspectMask = aspectSingle;
    rangeCopy.baseMipLevel = mip;
    rangeCopy.levelCount = 1;
    rangeCopy.baseArrayLayer = is3DTexture ? 0 : layer;
    rangeCopy.layerCount = 1;

    // Barrier range must include both depth and stencil for combined formats
    VkImageSubresourceRange rangeBarrier = rangeCopy;
    if (isDepthStencil && aspectSingle == VK_IMAGE_ASPECT_DEPTH_BIT)
    {
        rangeBarrier.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    // Two-step barrier collapsed to single: oldLayout -> TRANSFER_SRC_OPTIMAL
    // Be conservatively correct for visibility: assume any prior producer and make writes visible
    VkAccessFlags srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        uint64_t key = MakeSubKey(srcTexture, mip, rangeCopy.baseArrayLayer);
        auto it = m_SubresourceLayouts.find(key);
        oldLayout = (it != m_SubresourceLayouts.end()) ? it->second : VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        // An UNDEFINED source layout lets the driver satisfy this transition by
        // DISCARDING the subresource, so the readback can legally return blank
        // data — intermittently, which reads as a flaky test rather than a bug.
        // Render-graph passes journal a layout via their Read/Write edges; a
        // caller recording straight onto a CommandList must barrier the
        // subresource into its real state before reading it back.
        Logger::Log::Warning(
            "CopyTextureSubresourceToBuffer: mip {} layer {} has no recorded layout on this "
            "command list; the UNDEFINED -> TRANSFER_SRC transition may discard its contents. "
            "Barrier the subresource into its actual state before the copy.",
            mip, rangeCopy.baseArrayLayer);
    }
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask = srcAccessMask; // make any prior writes visible
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = oldLayout;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = srcImage;
    toSrc.subresourceRange = rangeBarrier;
#if defined(DEBUG) || defined(_DEBUG)
    if (GameEngine::Rendering::RenderingDebugFlags::Get().traceRendering)
    {
        printf("[CopyDepth] oldLayout=%d -> TRANSFER_SRC_OPTIMAL aspect=%u size=%ux%u src=(%u,%u) rowPitch=%zu\n",
               (int)oldLayout, (unsigned)aspectSingle, width, height, SrcX, SrcY, dstRowPitchBytes);
    }
#endif
    vkCmdPipelineBarrier(
        m_CommandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &toSrc);
    m_SubresourceLayouts[MakeSubKey(srcTexture, mip, rangeCopy.baseArrayLayer)] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    // Optional diagnostic: clear depth to 0.5 right before readback to validate copy path
    const char* dbgClr = std::getenv("GE_DEBUG_CLEAR_DEPTH_BEFORE_READBACK");
    if (dbgClr && aspectSingle == VK_IMAGE_ASPECT_DEPTH_BIT)
    {
        // Transition to TRANSFER_DST_OPTIMAL (barrier must include both aspects for combined formats)
        VkImageMemoryBarrier toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = srcImage;
        toDst.subresourceRange = rangeBarrier;
        vkCmdPipelineBarrier(
            m_CommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &toDst);
        m_SubresourceLayouts[MakeSubKey(srcTexture, mip, rangeCopy.baseArrayLayer)] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        if (TextureUsagePolicy::IsAuditEnabled())
        {
            TextureUsagePolicy::RecordTransferUse(srcTexture, TextureUsage::TransferDst,
                                                 "ClearDepthStencilBeforeReadback(dst)");
        }

        // Clear to 0.5 (clear uses the copy aspect range)
        VkClearDepthStencilValue dsv{};
        dsv.depth = 0.5f;
        dsv.stencil = 0;
        vkCmdClearDepthStencilImage(
            m_CommandBuffer,
            srcImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &dsv,
            1, &rangeCopy);

        // Transition back to TRANSFER_SRC_OPTIMAL
        VkImageMemoryBarrier backToSrc{};
        backToSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        backToSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        backToSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        backToSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        backToSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        backToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        backToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        backToSrc.image = srcImage;
        backToSrc.subresourceRange = rangeBarrier;
        vkCmdPipelineBarrier(
            m_CommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &backToSrc);
        m_SubresourceLayouts[MakeSubKey(srcTexture, mip, rangeCopy.baseArrayLayer)] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    // Track resource usage (coarse)
    if (srcTexture != INVALID_HANDLE)
    {
        UsedResource u{};
        u.ResourceType = UsedResource::Type::Texture;
        u.Id = srcTexture;
        u.Writes = false;
        u.StageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        u.AccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        u.Layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        u.BaseMip = mip;
        u.LevelCount = 1;
        u.BaseLayer = layer;
        u.LayerCount = 1;
        u.AspectMask = GetVkImageAspectFlags(srcTexture);
        m_UsedResources.push_back(u);
    }
    if (dstBuffer != INVALID_HANDLE)
    {
        UsedResource u{};
        u.ResourceType = UsedResource::Type::Buffer;
        u.Id = dstBuffer;
        u.Writes = true;
        u.StageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        u.AccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        u.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
        m_UsedResources.push_back(u);
    }

    // If requested via env var, perform a diagnostic blit of the source mip into a temporary 1-mip image,
    // then use the known-good CopyTextureToBuffer path on that temp. This is to isolate readback behavior.
    const char* dbgBlit = std::getenv("GE_DEBUG_BLIT_READBACK");
    vkFmt = m_Device->GetVkImageFormat(srcTexture);
    // Single source of truth for texel size — see BytesPerPixel(TextureFormat) in
    // Device.h. bpp sizes the trailing transfer→host buffer barrier: a table that
    // misses a format would claim a range past the readback buffer's end
    // (VUID-VkBufferMemoryBarrier-size-01189) for every format narrower than the
    // 4-byte fallback. Returns 0 only for block-compressed sources.
    uint32_t bpp = BytesPerPixel(m_Device->GetTextureFormat(srcTexture));

    if (dbgBlit && dbgBlit[0] == '1' && mip != 0)
    {
        // Create a transient temp image (1 mip) with transfer src/dst usage
        TextureDesc td{};
        td.width = width;
        td.height = height;
        td.depth = 1;
        td.mipLevels = 1;
        td.arrayLayers = 1;
        // Map a few common formats for diagnostics; default to RGBA8 if unknown
        uint32_t fmtEnum = 0;
        switch (vkFmt)
        {
        case VK_FORMAT_R8G8B8A8_UNORM:
            fmtEnum = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
            break;
        case VK_FORMAT_R8G8B8A8_SRGB:
            fmtEnum = static_cast<uint32_t>(TextureFormat::RGBA8_SRGB);
            break;
        case VK_FORMAT_B8G8R8A8_UNORM:
            fmtEnum = static_cast<uint32_t>(TextureFormat::BGRA8_UNORM);
            break;
        case VK_FORMAT_B8G8R8A8_SRGB:
            fmtEnum = static_cast<uint32_t>(TextureFormat::BGRA8_SRGB);
            break;
        default:
            fmtEnum = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
            break;
        }
        td.format = fmtEnum;
        td.usage = static_cast<uint32_t>(TextureUsage::TransferSrc) | static_cast<uint32_t>(TextureUsage::TransferDst);
        td.sampleCount = 1;
        td.persistent = false;
        td.debugName = "ReadbackTemp";
        TextureHandle temp = m_Device->CreateTexture(td);
        VkImage dstImage = m_Device->GetVkImage(temp);

        // Transition temp to TRANSFER_DST_OPTIMAL
        VkImageSubresourceRange tempRange{};
        tempRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        tempRange.baseMipLevel = 0;
        tempRange.levelCount = 1;
        tempRange.baseArrayLayer = 0;
        tempRange.layerCount = 1;
        VkImageMemoryBarrier tempToDst{};
        tempToDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        tempToDst.srcAccessMask = 0;
        tempToDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        tempToDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        tempToDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        tempToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tempToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tempToDst.image = dstImage;
        tempToDst.subresourceRange = tempRange;
        vkCmdPipelineBarrier(
            m_CommandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &tempToDst);

        // Blit from src mip to temp mip 0
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mip;
        blit.srcSubresource.baseArrayLayer = layer;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = {static_cast<int32_t>(SrcX), static_cast<int32_t>(SrcY), 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(SrcX + width), static_cast<int32_t>(SrcY + height), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = 0;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
#if defined(DEBUG) || defined(_DEBUG)

#endif
        // Prefer exact format-preserving copy to avoid filter/format issues
        VkImageCopy ic{};
        ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.mipLevel = mip;
        ic.srcSubresource.baseArrayLayer = layer;
        ic.srcSubresource.layerCount = 1;
        ic.srcOffset = {static_cast<int32_t>(SrcX), static_cast<int32_t>(SrcY), 0};
        ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.dstSubresource.mipLevel = 0;
        ic.dstSubresource.baseArrayLayer = 0;
        ic.dstSubresource.layerCount = 1;
        ic.dstOffset = {0, 0, 0};
        ic.extent = {width, height, 1};
        if (TextureUsagePolicy::IsAuditEnabled())
        {
            TextureUsagePolicy::RecordTransferUse(temp, TextureUsage::TransferDst, "ReadbackTempCopy(dst)");
        }
        vkCmdCopyImage(m_CommandBuffer,
                       srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &ic);

        // Transition temp from TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL for copy-to-buffer
        VkImageMemoryBarrier tempToSrc{};
        tempToSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        tempToSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        tempToSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        tempToSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        tempToSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        tempToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tempToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tempToSrc.image = dstImage;
        tempToSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        tempToSrc.subresourceRange.baseMipLevel = 0;
        tempToSrc.subresourceRange.levelCount = 1;
        tempToSrc.subresourceRange.baseArrayLayer = 0;
        tempToSrc.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(
            m_CommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &tempToSrc);

        // Now copy temp mip0 to buffer using the simple path (tightly packed)
        // Reuse this function's bpp for row pitch calculation
        CopyTextureToBuffer(temp, dstBuffer, width, height, 0, 0, DstOffsetBytes, dstRowPitchBytes);
        return;
    }

    VkBufferImageCopy region{};
    region.bufferOffset = DstOffsetBytes;
    // bufferRowLength / bufferImageHeight are in TEXELS. 0 = tightly packed from imageExtent.
    region.bufferRowLength = (dstRowPitchBytes && bpp) ? static_cast<uint32_t>(dstRowPitchBytes / bpp) : 0;
    region.bufferImageHeight = (dstSlicePitchBytes && dstRowPitchBytes)
                                   ? static_cast<uint32_t>(dstSlicePitchBytes / dstRowPitchBytes)
                                   : 0;
    // Use only one valid aspect for copy (depth if depth/stencil, else color)
    VkImageAspectFlags aspectCopy = aspectSingle;
    region.imageSubresource.aspectMask = aspectCopy;
    region.imageSubresource.mipLevel = mip;
    region.imageSubresource.baseArrayLayer = is3DTexture ? 0 : layer;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {static_cast<int32_t>(SrcX), static_cast<int32_t>(SrcY),
                          is3DTexture ? static_cast<int32_t>(layer) : 0};
    region.imageExtent = {width, height, is3DTexture ? copyDepth : 1u};
    vkCmdCopyImageToBuffer(m_CommandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstVkBuffer, 1, &region);

    if (oldLayout != VK_IMAGE_LAYOUT_UNDEFINED && oldLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
    {
        auto dstForLayout = [](VkImageLayout layout) -> SrcStageAccess
        {
            switch (layout)
            {
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT};
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
            default:
                return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
            }
        };

        const SrcStageAccess dst = dstForLayout(oldLayout);
        VkImageMemoryBarrier restore{};
        restore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        restore.dstAccessMask = dst.Access;
        restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        restore.newLayout = oldLayout;
        restore.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        restore.image = srcImage;
        restore.subresourceRange = rangeBarrier;
        vkCmdPipelineBarrier(
            m_CommandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            dst.Stage,
            0,
            0, nullptr,
            0, nullptr,
            1, &restore);
        m_SubresourceLayouts[MakeSubKey(srcTexture, mip, rangeCopy.baseArrayLayer)] = oldLayout;
    }

    // Make transfer writes visible to host reads when the queue is idle
    VkBufferMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = dstVkBuffer;
    toHost.offset = region.bufferOffset;
    const VkDeviceSize rowsPerSlice = region.bufferImageHeight ? region.bufferImageHeight : height;
    const VkDeviceSize widthTexels = region.bufferRowLength ? region.bufferRowLength : width;
    const VkDeviceSize approxSize = widthTexels * rowsPerSlice * (bpp ? bpp : 4) * (is3DTexture ? copyDepth : 1u);
    toHost.size = approxSize;
    vkCmdPipelineBarrier(
        m_CommandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0, nullptr,
        1, &toHost,
        0, nullptr);
}

void VulkanCommandList::ClearColorImageSubresource(TextureHandle texture, uint32_t mip, uint32_t layer, const float rgba[4])
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
        return;
    VkImage img = m_Device->GetVkImage(texture);
    if (img == VK_NULL_HANDLE)
        return;

    if (TextureUsagePolicy::IsAuditEnabled())
    {
        TextureUsagePolicy::RecordTransferUse(texture, TextureUsage::TransferDst,
                                             "ClearColorImageSubresource(dst)");
    }

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = mip;
    range.levelCount = 1;
    range.baseArrayLayer = layer;
    range.layerCount = 1;

    // Transition to TRANSFER_DST_OPTIMAL for clear. Source stage/access are derived
    // from the tracked previous layout so a prior shader read or color write is
    // correctly flushed before the clear begins.
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        uint64_t key = MakeSubKey(texture, mip, layer);
        auto it = m_SubresourceLayouts.find(key);
        if (it != m_SubresourceLayouts.end())
            oldLayout = it->second;
    }
    const SrcStageAccess src = SrcMasksForOldLayout(oldLayout);

    VkImageMemoryBarrier toClear{};
    toClear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toClear.srcAccessMask = src.Access;
    toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toClear.oldLayout = oldLayout;
    toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toClear.image = img;
    toClear.subresourceRange = range;
    vkCmdPipelineBarrier(
        m_CommandBuffer,
        src.Stage,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &toClear);
    // Track layout after transition
    m_SubresourceLayouts[MakeSubKey(texture, mip, layer)] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    VkClearColorValue color{};
    color.float32[0] = rgba[0];
    color.float32[1] = rgba[1];
    color.float32[2] = rgba[2];
    color.float32[3] = rgba[3];
    vkCmdClearColorImage(m_CommandBuffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

    // Leave subresource in TRANSFER_DST_OPTIMAL; caller/graph will transition to TRANSFER_SRC_OPTIMAL if needed

#if defined(DEBUG) || defined(_DEBUG)

#endif
}

void VulkanCommandList::CopyBufferToTextureSubresource(BufferHandle srcBuffer, TextureHandle dstTexture,
                                                       uint32_t mip, uint32_t layer,
                                                       uint32_t width, uint32_t height,
                                                       size_t srcOffsetBytes, size_t srcRowPitchBytes,
                                                       uint32_t depth, size_t srcSlicePitchBytes,
                                                       uint32_t dstX, uint32_t dstY,
                                                       ResourceState currentState)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
        return;
    VkImage dstImage = m_Device->GetVkImage(dstTexture);
    VkBuffer srcVkBuffer = m_Device->GetVkBuffer(srcBuffer);
    if (dstImage == VK_NULL_HANDLE || srcVkBuffer == VK_NULL_HANDLE)
        return;

    if (TextureUsagePolicy::IsAuditEnabled())
    {
        TextureUsagePolicy::RecordTransferUse(dstTexture, TextureUsage::TransferDst,
                                             "CopyBufferToTextureSubresource(dst)");
    }

    // Single source of truth for texel size — see BytesPerPixel(TextureFormat) in Device.h.
    // Returns 0 for block-compressed formats; row-pitch validation skips when bpp is 0.
    const uint32_t bpp = BytesPerPixel(m_Device->GetTextureFormat(dstTexture));

    VkExtent3D dstExtent = m_Device->GetVkImageExtent(dstTexture);
    const bool is3DTexture = dstExtent.depth > 1;

    const VulkanTexture* vkTex = GetVulkanTextureConst(dstTexture);
    const auto dimensionAtMip = [](uint32_t base, uint32_t level) -> uint32_t {
        uint32_t d = base;
        for (uint32_t l = 0; l < level; ++l)
            d = d > 1u ? d / 2u : 1u;
        return d > 0u ? d : 1u;
    };

    const uint32_t mipLevelCount = vkTex ? vkTex->mipLevels : 1u;
    if (mip >= mipLevelCount)
    {
        Logger::Log::Error("CopyBufferToTextureSubresource: mip {} out of range (mipLevels={})",
                           mip, mipLevelCount);
        return;
    }

    const uint32_t mipW = dimensionAtMip(dstExtent.width, mip);
    const uint32_t mipH = dimensionAtMip(dstExtent.height, mip);
    const uint32_t mipD = dimensionAtMip(dstExtent.depth, mip);

    if (width == 0u || height == 0u || depth == 0u)
    {
        Logger::Log::Error("CopyBufferToTextureSubresource: zero copy extent ({}x{}x{})",
                           width, height, depth);
        return;
    }

    if (!is3DTexture)
    {
        if (depth != 1u)
        {
            Logger::Log::Error("CopyBufferToTextureSubresource: depth must be 1 for non-3D images (got {})",
                               depth);
            return;
        }
        if (vkTex && layer >= vkTex->arrayLayers)
        {
            Logger::Log::Error("CopyBufferToTextureSubresource: layer {} out of range (arrayLayers={})",
                               layer, vkTex->arrayLayers);
            return;
        }
    }
    else
    {
        const uint64_t zEnd = static_cast<uint64_t>(layer) + static_cast<uint64_t>(depth);
        if (zEnd > mipD)
        {
            Logger::Log::Error(
                "CopyBufferToTextureSubresource: Z range [{}, {}) exceeds mip depth {} (mip {})",
                layer, zEnd, mipD, mip);
            return;
        }
    }

    const uint64_t xEnd = static_cast<uint64_t>(dstX) + static_cast<uint64_t>(width);
    const uint64_t yEnd = static_cast<uint64_t>(dstY) + static_cast<uint64_t>(height);
    if (xEnd > mipW || yEnd > mipH)
    {
        Logger::Log::Error(
            "CopyBufferToTextureSubresource: dst rect [{},{})x[{},{}) exceeds mip {}x{} at mip {}",
            dstX, xEnd, dstY, yEnd, mipW, mipH, mip);
        return;
    }

    if (srcSlicePitchBytes && srcRowPitchBytes && (srcSlicePitchBytes % srcRowPitchBytes) != 0u)
    {
        Logger::Log::Error(
            "CopyBufferToTextureSubresource: srcSlicePitchBytes ({}) must be a multiple of srcRowPitchBytes ({})",
            srcSlicePitchBytes, srcRowPitchBytes);
        return;
    }

    if (srcSlicePitchBytes && srcRowPitchBytes)
    {
        const uint32_t rowsPerSlice = static_cast<uint32_t>(srcSlicePitchBytes / srcRowPitchBytes);
        if (rowsPerSlice < height)
        {
            Logger::Log::Error(
                "CopyBufferToTextureSubresource: slice pitch implies {} row(s) but copy height is {}",
                rowsPerSlice, height);
            return;
        }
    }

    if (srcRowPitchBytes && bpp && (srcRowPitchBytes % bpp) != 0u)
    {
        Logger::Log::Error(
            "CopyBufferToTextureSubresource: srcRowPitchBytes ({}) is not a multiple of texel size ({} bytes)",
            srcRowPitchBytes, bpp);
        return;
    }

    const bool isSubRegion = dstX != 0u || dstY != 0u || width < mipW || height < mipH;

    // Transition destination subresource to TRANSFER_DST_OPTIMAL. When the caller
    // supplies currentState, the transition moves FROM that layout so contents are
    // preserved and the barrier picks up the matching source stage/access (a whole-
    // subresource discard otherwise). Without a caller state the old layout comes from
    // command-list-local tracking, which for a fresh list means UNDEFINED — correct only
    // for whole-subresource rewrites.
    // The barrier covers every aspect of the format (depth and stencil together for a
    // combined format); a buffer copy writes exactly one, the depth for a depth format.
    VkImageSubresourceRange range{};
    range.aspectMask = GetVkImageAspectFlags(dstTexture);
    range.baseMipLevel = mip;
    range.levelCount = 1;
    range.baseArrayLayer = is3DTexture ? 0 : layer;
    range.layerCount = 1;
    const VkImageAspectFlags copyAspect =
        (range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (currentState != ResourceState::Undefined)
    {
        oldLayout = VulkanMappings::TranslateResourceStateToLayout(currentState);
    }
    else
    {
        uint64_t key = MakeSubKey(dstTexture, mip, range.baseArrayLayer);
        auto it = m_SubresourceLayouts.find(key);
        if (it != m_SubresourceLayouts.end())
            oldLayout = it->second;
    }

    if (isSubRegion && oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        // A partial copy transitions the WHOLE subresource; from UNDEFINED that is a
        // spec-legal discard of everything outside the rect. Callers doing region
        // uploads must pass the resting state so the transition preserves contents.
        // Only warn when the resolved layout is genuinely unknown — row-by-row upload
        // loops within one list are covered by list-local tracking after the first
        // copy and would otherwise spam false positives.
        Logger::Log::Warning(
            "CopyBufferToTextureSubresource: sub-region copy with unknown current layout "
            "may discard texels outside the rect (dst {}x{} into mip {}x{})",
            width, height, mipW, mipH);
    }

    const SrcStageAccess src = SrcMasksForOldLayout(oldLayout);

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.srcAccessMask = src.Access;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = oldLayout;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = dstImage;
    toDst.subresourceRange = range;
    vkCmdPipelineBarrier(
        m_CommandBuffer,
        src.Stage,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &toDst);
    m_SubresourceLayouts[MakeSubKey(dstTexture, mip, range.baseArrayLayer)] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    // Issue copy. bufferRowLength/bufferImageHeight are in texels (Vulkan spec).
    // 0 means "tightly packed" — let the driver compute from imageExtent.
    VkBufferImageCopy region{};
    region.bufferOffset = srcOffsetBytes;
    region.bufferRowLength = (srcRowPitchBytes && bpp) ? static_cast<uint32_t>(srcRowPitchBytes / bpp) : 0;
    region.bufferImageHeight = (srcSlicePitchBytes && srcRowPitchBytes) ? static_cast<uint32_t>(srcSlicePitchBytes / srcRowPitchBytes) : 0;
    region.imageSubresource.aspectMask = copyAspect;
    region.imageSubresource.mipLevel = mip;
    region.imageSubresource.baseArrayLayer = is3DTexture ? 0 : layer;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {static_cast<int32_t>(dstX), static_cast<int32_t>(dstY),
                          is3DTexture ? static_cast<int32_t>(layer) : 0};
    region.imageExtent = {width, height, is3DTexture ? depth : 1u};
    vkCmdCopyBufferToImage(m_CommandBuffer, srcVkBuffer, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void VulkanCommandList::BlitImageMip(TextureHandle texture, uint32_t srcMip, uint32_t dstMip, uint32_t layer)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
        return;
    VkImage img = m_Device->GetVkImage(texture);
    if (img == VK_NULL_HANDLE)
        return;

    if (TextureUsagePolicy::IsAuditEnabled())
    {
        TextureUsagePolicy::RecordTransferUse(texture, TextureUsage::TransferSrc, "BlitImageMip(src)");
        TextureUsagePolicy::RecordTransferUse(texture, TextureUsage::TransferDst, "BlitImageMip(dst)");
    }

    uint32_t baseW = 0, baseH = 0;
    GetTextureSize(texture, baseW, baseH);
    auto mipDim = [](uint32_t v, uint32_t m)
    { uint32_t r = v >> m; return r ? r : 1u; };
    uint32_t srcW = mipDim(baseW, srcMip);
    uint32_t srcH = mipDim(baseH, srcMip);
    uint32_t dstW = mipDim(baseW, dstMip);
    uint32_t dstH = mipDim(baseH, dstMip);
    VkImageBlit blit{};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = srcMip;
    blit.srcSubresource.baseArrayLayer = layer;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(srcW), static_cast<int32_t>(srcH), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel = dstMip;
    blit.dstSubresource.baseArrayLayer = layer;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<int32_t>(dstW), static_cast<int32_t>(dstH), 1};
#if defined(DEBUG) || defined(_DEBUG)
#endif
    // Assumes caller has transitioned src to TRANSFER_SRC_OPTIMAL and dst to TRANSFER_DST_OPTIMAL
    vkCmdBlitImage(m_CommandBuffer,
                   img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
}

void VulkanCommandList::ClearRenderTarget(TextureHandle target, float r, float g, float b, float a)
{
    if (m_CommandBuffer == VK_NULL_HANDLE || !m_IsRecording)
    {
        return;
    }

    VkImage image = m_Device->GetVkImage(target);
    if (image != VK_NULL_HANDLE)
    {
        VkImageAspectFlags aspectFlags = GetVkImageAspectFlags(target);

        // Only clear color images with vkCmdClearColorImage
        if (aspectFlags == VK_IMAGE_ASPECT_COLOR_BIT)
        {
            if (TextureUsagePolicy::IsAuditEnabled())
            {
                TextureUsagePolicy::RecordTransferUse(target, TextureUsage::TransferDst, "ClearRenderTarget(dst)");
            }

            VkClearColorValue color = {{r, g, b, a}};
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel = 0;
            range.levelCount = 1;
            range.baseArrayLayer = 0;
            range.layerCount = 1;
            vkCmdClearColorImage(m_CommandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        }
        // Depth/stencil clearing should use vkCmdClearDepthStencilImage; out of scope here
    }
}

// Resource state conversion helpers
VkImageAspectFlags VulkanCommandList::GetVkImageAspectFlags(TextureHandle textureHandle)
{
    // Get the texture from the global texture manager
    const VulkanTexture* vulkanTexture = GetVulkanTextureConst(textureHandle);
    if (!vulkanTexture)
    {
        return VK_IMAGE_ASPECT_COLOR_BIT; // Default fallback
    }

    // Determine aspect flags based on format
    switch (vulkanTexture->format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;

    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

} // namespace Rendering
} // namespace GameEngine
