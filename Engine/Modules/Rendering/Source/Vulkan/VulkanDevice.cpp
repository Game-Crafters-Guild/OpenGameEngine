/**
 * @file VulkanDevice.cpp
 * @brief Vulkan implementation of the IDevice interface
 */

#include "VulkanDevice.h"
#include "Rendering/Core/TextureFormatSupportGate.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/RendererProfile.h"
#include "Rendering/Core/BufferManager.h"
#include "Rendering/Core/PipelineManager.h"
#include "Rendering/Core/SamplerManager.h"
#include "Rendering/Core/SpecializationConstants.h"
#include "Rendering/Core/TextureManager.h"
#include "TextureUsagePolicy.h"
#include "ValidationStats.h"
#include "VulkanAccelerationStructures.h"
#include "VulkanCommandList.h"
#include "VulkanDeviceFault.h"
#include "VulkanGpuHangShader.h"
#include "VulkanMappings.h"
#include "VulkanPipelineCache.h"
#include "VulkanQueueFamilySelection.h"
#include "VulkanSharedInstance.h"
#include "VulkanToolingInfo.h"
#include "VulkanValidationLayerSettings.h"
#include "VulkanVuidSuppressions.h"
#include "Rendering/Core/PipelineCache.h"
#if RENDERING_ENABLE_SPIRV_REFLECTION
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderReflection.h"
#endif
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ReflectionCache.h"

#include "Core/EngineVersion.h"
#include "Logger/Logger.h"
#include "VulkanPipelineCacheAdapter.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iterator>

// macOS bundle helpers (MoltenVK ICD discovery)
#if defined(PLATFORM_MACOS)
#include <mach-o/dyld.h>
#include <filesystem>
#include <cstdlib>
#include <limits>
#endif

// Verbose Vulkan device logging gate
#if !defined(GE_VERBOSE_RENDERING_LOGS)
#define VK_DBG(expr) \
    do               \
    {                \
    } while (0)
#else
#define VK_DBG(expr) \
    do               \
    {                \
        expr;        \
    } while (0)
#endif

#include <algorithm>
#include <csignal>
#include <initializer_list>
#include <set>
#include <vector>
#define GLFW_INCLUDE_VULKAN
#include "VulkanHandleHelpers.h"

#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
// Ensure Win32 surface types and PFNs are available
#include <vulkan/vulkan_win32.h>
#endif
#include <cctype>
#include <cstring>

// VMA (Vulkan Memory Allocator)
#ifdef RENDERING_HAS_VMA
// Helper to safely cast the opaque allocator handle
static inline VmaAllocator AsVmaAllocator(void* ptr)
{
    return reinterpret_cast<VmaAllocator>(ptr);
}

#ifdef _MSC_VER
#pragma warning(push)
// Suppress common 3rd-party warnings from VMA under MSVC (including /analyze noise).
#pragma warning(disable : 4189 4127 4324 4505 6326 6386 6387)
#endif
#define VMA_IMPLEMENTATION
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif
#include <vk_mem_alloc.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif
#include "VulkanBufferResidency.h"
#include "VulkanMemoryTopology.h"

#include <cstdint>

#include <cstdlib>

namespace
{
inline bool IsEnvEnabled(const char* name)
{
    const char* v = std::getenv(name);
    if (!v)
        return false;
    // Treat unset/empty/"0" as false; anything else as true
    return v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

/// GE_VK_CAPTURE_COMPAT: drop the descriptor-buffer and acceleration-structure
/// device features for this session (see the device-creation site for why).
/// Instance creation reads it too — it is the one thing knowable that early which
/// rules VK_EXT_descriptor_buffer out — so the two sites share one definition
/// rather than two copies that could drift apart.
inline bool IsCaptureCompatRequested()
{
    const char* env = std::getenv("GE_VK_CAPTURE_COMPAT");
    return env && env[0] != '\0' && env[0] != '0' && env[0] != 'f' && env[0] != 'F';
}

/// GE_VK_NO_DEBUG_UTILS: drop VK_EXT_debug_utils for this process.
///
/// Read once and memoized, so every device in the process sees the same answer.
/// Instance configuration that differs device to device has to be keyed into
/// SharedInstanceKey or two devices disagree about the instance they share (see
/// its doc comment); a value that cannot differ needs no key entry.
inline bool IsDebugUtilsDisabled()
{
    static const bool s_Disabled =
        GameEngine::Rendering::ParseDebugUtilsDisabled(std::getenv("GE_VK_NO_DEBUG_UTILS"));
    return s_Disabled;
}

inline bool IsResourcePoolDiagnosticsEnabled()
{
#if defined(DEBUG) || defined(_DEBUG)
    return true;
#else
    static const bool s_Enabled = IsEnvEnabled("GE_VK_LOG_POOL_HIGH_WATER") || IsEnvEnabled("GE_VK_LOG_RESOURCE_POOL_HIGH_WATER");
    return s_Enabled;
#endif
}

inline bool ShouldSuppressUnusedVertexInputPerformanceWarning(const char* messageId, const char* message)
{
    // Keep validation logs actionable by default: this specific performance warning
    // is expected when a mesh provides optional vertex streams (e.g. tangent) that
    // a given shader variant does not consume.
    static const bool s_LogAllPerformanceWarnings = IsEnvEnabled("GE_VK_LOG_ALL_PERF_WARNINGS");
    if (s_LogAllPerformanceWarnings)
        return false;
    if (!messageId || !message)
        return false;
    if (std::strcmp(messageId, "WARNING-Shader-OutputNotConsumed") != 0)
        return false;
    return std::strstr(message, "Vertex attribute at location") != nullptr &&
           std::strstr(message, "not consumed by vertex shader") != nullptr;
}

inline int StrCaseCmp(const char* a, const char* b)
{
    if (a == b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    while (*a && *b)
    {
        unsigned char ca = static_cast<unsigned char>(*a);
        unsigned char cb = static_cast<unsigned char>(*b);
        int da = std::tolower(ca);
        int db = std::tolower(cb);
        if (da != db)
        {
            return da - db;
        }
        ++a;
        ++b;
    }
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
}

#if defined(PLATFORM_MACOS)
// macOS: Help GLFW locate the Vulkan loader (MoltenVK) reliably.
// If GLFW was built without a hard-coded Vulkan loader path, it may fail to
// dlopen the loader from non-standard locations (e.g. vcpkg install trees),
// causing glfwGetRequiredInstanceExtensions() to return 0 and surface creation
// to fail with VK_ERROR_EXTENSION_NOT_PRESENT.
//
// GLFW requires this to be called BEFORE glfwInit(). We do it here at static
// initialization time so every app that links the Vulkan backend benefits.
struct GlfwVulkanLoaderBootstrap
{
    GlfwVulkanLoaderBootstrap()
    {
#if defined(GLFW_VERSION_MAJOR) && defined(GLFW_VERSION_MINOR)
#if (GLFW_VERSION_MAJOR > 3) || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
        glfwInitVulkanLoader(vkGetInstanceProcAddr);
#endif
#endif
    }
};
static GlfwVulkanLoaderBootstrap s_GlfwVulkanLoaderBootstrap;
#endif
} // namespace

// Ensure portability-related macros are available even when building against
// older Vulkan SDK headers. These are required for portability implementations
// such as MoltenVK on macOS, where failing to enable
// VK_KHR_portability_enumeration and set
// VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR can cause
// vkCreateInstance to return VK_ERROR_INCOMPATIBLE_DRIVER.
#ifndef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
#define VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME "VK_KHR_portability_enumeration"
#endif
#ifndef VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 0x00000001
#endif

#if defined(PLATFORM_MACOS)
// Some Vulkan SDK headers only expose these extension-name macros when a corresponding
// VK_USE_PLATFORM_* macro is defined. Provide fallbacks so we can still query support
// at runtime via vkEnumerateInstanceExtensionProperties without relying on those macros.
#ifndef VK_EXT_METAL_SURFACE_EXTENSION_NAME
#define VK_EXT_METAL_SURFACE_EXTENSION_NAME "VK_EXT_metal_surface"
#endif
#ifndef VK_MVK_MACOS_SURFACE_EXTENSION_NAME
#define VK_MVK_MACOS_SURFACE_EXTENSION_NAME "VK_MVK_macos_surface"
#endif
#endif

namespace GameEngine
{
namespace Rendering
{

// Remove legacy handle generation - now using proper GenerationalVector

// Forward declaration for VulkanResourceUtils
namespace VulkanResourceUtils
{
VkBufferUsageFlags GetVulkanBufferUsage(BufferUsage usage);
VkFormat GetVulkanFormat(Format format);
VkFormat GetVulkanFormat(TextureFormat format);
} // namespace VulkanResourceUtils

// Validation layers
const std::vector<const char*> g_validationLayers = {
    "VK_LAYER_KHRONOS_validation"};

// Required device extensions
#if RENDERING_ENABLE_SPIRV_REFLECTION
struct ShaderKey
{
    uint64_t vsHash = 0, fsHash = 0, csHash = 0, msHash = 0, gsHash = 0;
    uint64_t specHash = 0; // specialization constants variant hash
    bool operator==(const ShaderKey& o) const noexcept
    {
        return vsHash == o.vsHash && fsHash == o.fsHash && csHash == o.csHash && msHash == o.msHash && gsHash == o.gsHash && specHash == o.specHash;
    }
};
struct ShaderKeyHasher
{
    size_t operator()(const ShaderKey& k) const noexcept
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t x)
        { h ^= x; h *= 1099511628211ull; };
        mix(k.vsHash);
        mix(k.fsHash);
        mix(k.csHash);
        mix(k.msHash);
        mix(k.gsHash);
        mix(k.specHash);
        return static_cast<size_t>(h);
    }
};
static std::unordered_map<ShaderKey, ShaderMeta, ShaderKeyHasher> g_shaderMetaCache;
#endif

// Build device extension list dynamically based on actual support
// Swapchain and maintenance1 are widely supported but still guard them; dynamic rendering KHR is only enabled on < 1.3 when available
// Note: See CreateLogicalDevice() for the actual assembly of the list.

// Track sampler ownership across devices. Sampler handles are stored in a global manager,
// but VkSampler objects are device-bound. Guard against cross-device bind/destroy.
static std::mutex g_samplerOwnerMutex;
static std::unordered_map<uint64_t, VkDevice> g_samplerOwnerByHandleId;
static void RegisterSamplerOwner(SamplerHandle handle, VkDevice owner);
static void UnregisterSamplerOwner(SamplerHandle handle);
static void UnregisterSamplerOwnersForDevice(VkDevice owner);
static VkSampler GetVkSampler(SamplerHandle handle, VkDevice expectedOwner);
// Track descriptor-set ownership across devices. Descriptor sets are device-bound,
// but DescriptorSetHandle is opaque and can be accidentally reused cross-device.
static std::mutex g_descriptorSetOwnerMutex;
static std::unordered_map<uint64_t, VkDevice> g_descriptorSetOwnerByHandleId;
static void RegisterDescriptorSetOwner(DescriptorSetHandle handle, VkDevice owner);
static void UnregisterDescriptorSetOwner(DescriptorSetHandle handle);
static void UnregisterDescriptorSetOwnersForDevice(VkDevice owner);
static bool DescriptorSetOwnerMatches(DescriptorSetHandle handle, VkDevice expectedOwner);

// GE_VK_WATCH_IMAGE=<debugName substring>: diagnostic watch that attributes
// descriptor-image-layout VUIDs (which name only raw VkImage handles) to the
// engine code that wrote the descriptor. Every image view created on the
// watched image is remembered, and every descriptor write referencing one of
// those views is logged with set/binding/type. Zero cost unless the env var
// names a texture.
static const char* WatchedImageDebugName()
{
    static const char* name = std::getenv("GE_VK_WATCH_IMAGE");
    return (name && name[0] != '\0') ? name : nullptr;
}
static std::mutex g_watchedImageMutex;
static VkImage g_watchedImage = VK_NULL_HANDLE;
static std::unordered_set<VkImageView> g_watchedImageViews;

static void WatchImageIfNamed(const char* debugName, VkImage image)
{
    const char* watch = WatchedImageDebugName();
    if (!watch || !debugName || strstr(debugName, watch) == nullptr)
        return;
    std::scoped_lock lk(g_watchedImageMutex);
    g_watchedImage = image;
    Logger::Log::Warning("[WatchImage] watching '{}' VkImage={}", debugName, (void*)image);
}

static void WatchViewIfOnWatchedImage(VkImage image, VkImageView view,
                                      const VkImageSubresourceRange& range)
{
    if (!WatchedImageDebugName())
        return;
    std::scoped_lock lk(g_watchedImageMutex);
    if (g_watchedImage == VK_NULL_HANDLE || image != g_watchedImage)
        return;
    g_watchedImageViews.insert(view);
    Logger::Log::Warning("[WatchImage] view={} mips[{}+{}] layers[{}+{}]", (void*)view,
                         range.baseMipLevel, range.levelCount, range.baseArrayLayer,
                         range.layerCount);
}

static void WatchCheckDescriptorWrites(const VkWriteDescriptorSet* writes, uint32_t count)
{
    if (!WatchedImageDebugName())
        return;
    std::scoped_lock lk(g_watchedImageMutex);
    if (g_watchedImageViews.empty())
        return;
    for (uint32_t w = 0; w < count; ++w)
    {
        if (!writes[w].pImageInfo)
            continue;
        for (uint32_t i = 0; i < writes[w].descriptorCount; ++i)
        {
            const VkImageView v = writes[w].pImageInfo[i].imageView;
            if (v != VK_NULL_HANDLE && g_watchedImageViews.count(v))
                Logger::Log::Error(
                    "[WatchImage] descriptor write: set={} binding={} elem={} type={} "
                    "layout={} view={}",
                    (void*)writes[w].dstSet, writes[w].dstBinding,
                    writes[w].dstArrayElement + i, (int)writes[w].descriptorType,
                    (int)writes[w].pImageInfo[i].imageLayout, (void*)v);
        }
    }
}

void WatchImageBarrierIfWatched(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                const VkImageSubresourceRange& range, const char* site)
{
    if (!WatchedImageDebugName())
        return;
    std::scoped_lock lk(g_watchedImageMutex);
    if (g_watchedImage == VK_NULL_HANDLE || image != g_watchedImage)
        return;
    Logger::Log::Warning("[WatchImage] barrier ({}): layout {}->{} mips[{}+{}] layers[{}+{}]",
                         site, (int)oldLayout, (int)newLayout, range.baseMipLevel,
                         range.levelCount, range.baseArrayLayer, range.layerCount);
}

// Descriptor-buffer twin of WatchCheckDescriptorWrites: vkGetDescriptorEXT writes bypass
// vkUpdateDescriptorSets, so DB-backed sets need their own hook at the getInfo level.
static void WatchCheckDescriptorGetInfo(const VkDescriptorGetInfoEXT& getInfo, uint32_t binding,
                                        uint32_t element, const char* site)
{
    if (!WatchedImageDebugName())
        return;
    const VkDescriptorImageInfo* info = nullptr;
    switch (getInfo.type)
    {
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: info = getInfo.data.pCombinedImageSampler; break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:          info = getInfo.data.pSampledImage; break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:          info = getInfo.data.pStorageImage; break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:       info = getInfo.data.pInputAttachmentImage; break;
        default: return;
    }
    if (!info || info->imageView == VK_NULL_HANDLE)
        return;
    std::scoped_lock lk(g_watchedImageMutex);
    if (g_watchedImageViews.count(info->imageView))
        Logger::Log::Error(
            "[WatchImage] descriptor-buffer write ({}): binding={} elem={} type={} layout={} view={}",
            site, binding, element, (int)getInfo.type, (int)info->imageLayout,
            (void*)info->imageView);
}

// Optional sync2 extension for Vulkan 1.2 fallback
const char* kSync2Ext = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;

// How long the validation assert waits for the log trail before breaking. The
// break is a death path, so the wait is bounded rather than a plain Log::Flush.
constexpr std::chrono::milliseconds kValidationAssertFlushTimeout{2000};

// Phase-3b validation assert (design: validation-observability-2026-07-23).
// Default ON whenever validation runs: both descriptor configs measure zero
// errors and zero warnings on the reference scene, so an unsuppressed ERROR
// is always a regression worth breaking on. GE_VK_VALIDATION_ASSERT=0 is the
// triage escape hatch; suppression-table hits never assert but stay counted.
static bool ValidationAssertEnabledFromEnv()
{
    static const bool kEnabled = []
    {
        const char* v = std::getenv("GE_VK_VALIDATION_ASSERT");
        if (!v || v[0] == '\0')
            return true; // unset => on with validation
        return !(v[0] == '0' && v[1] == '\0');
    }();
    return kEnabled;
}

#if defined(_WIN32)
extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent(void);
#endif

static void ValidationAssertBreak()
{
#if defined(_MSC_VER)
    if (IsDebuggerPresent())
        __debugbreak();
    else
        std::abort();
#else
    raise(SIGTRAP);
#endif
}

#if defined(VK_EXT_layer_settings)
// True when the validation layer advertises VK_EXT_layer_settings, i.e. it will
// consume a VkLayerSettingsCreateInfoEXT chained into instance creation
// (SDK >= 1.3.268). Older loaders fall back to stock layer defaults.
static bool ValidationLayerAdvertisesLayerSettings(const char* layerName)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(layerName, &count, nullptr) != VK_SUCCESS || count == 0)
        return false;
    std::vector<VkExtensionProperties> props(count);
    if (vkEnumerateInstanceExtensionProperties(layerName, &count, props.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties& p : props)
    {
        if (std::strcmp(p.extensionName, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME) == 0)
            return true;
    }
    return false;
}
#endif
// Debug callback
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{

    const char* severityStr = "UNKNOWN";
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        severityStr = "ERROR";
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        severityStr = "WARNING";
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        severityStr = "INFO";
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
        severityStr = "VERBOSE";

    const char* typeStr = "UNKNOWN";
    if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)
        typeStr = "GENERAL";
    else if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
        typeStr = "VALIDATION";
    else if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
        typeStr = "PERFORMANCE";

    const char* msg = pCallbackData && pCallbackData->pMessage ? pCallbackData->pMessage : "";

    // Record + log validation warnings and errors through the central store/logger
    if (messageSeverity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
    {
        const char* messageId = (pCallbackData && pCallbackData->pMessageIdName) ? pCallbackData->pMessageIdName : "";
        if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) &&
            (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) &&
            ShouldSuppressUnusedVertexInputPerformanceWarning(messageId, msg))
        {
            return VK_FALSE;
        }
        // The layer hands us the involved objects (with any debug names) and the
        // command-buffer label stack (our pass markers) — without them a layout
        // VUID names a raw VkImage handle and nothing else, which is undebuggable.
        std::string objects;
        std::string labels;
        if (pCallbackData)
        {
            for (uint32_t i = 0; i < pCallbackData->objectCount; ++i)
            {
                const auto& obj = pCallbackData->pObjects[i];
                if (obj.pObjectName && obj.pObjectName[0] != '\0')
                {
                    if (!objects.empty())
                        objects += ", ";
                    objects += obj.pObjectName;
                }
            }
            for (uint32_t i = 0; i < pCallbackData->cmdBufLabelCount; ++i)
            {
                const auto& label = pCallbackData->pCmdBufLabels[i];
                if (label.pLabelName && label.pLabelName[0] != '\0')
                {
                    if (!labels.empty())
                        labels += " > ";
                    labels += label.pLabelName;
                }
            }
        }

        // 1. Record (always, before any assert path) — counts stay exact even
        // when the log below is thinned.
        const bool isError = (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
        ValidationMessageInfo record{};
        record.IsError = isError;
        record.Vuid = messageId;
        record.Message = msg;
        record.Objects = objects;
        record.Labels = labels;
        record.Suppressed = FindVuidSuppression(messageId) != nullptr;
        const ValidationRecordOutcome outcome = ValidationStatsStore::Get().Record(record);

        // 2. Log, deduped per ShouldLogValidationOccurrence: full text on first
        // occurrence per VUID, then a one-liner every kValidationLogRepeatStride
        // repeats. This messenger is the process's only validation log sink, so
        // that policy alone bounds the log for a VUID that fires per-draw.
        const bool hasId = messageId[0] != '\0';
        if (outcome.FirstOccurrence)
        {
            std::string context;
            if (!objects.empty())
                context += " objects: " + objects;
            if (!labels.empty())
                context += " | labels: " + labels;
            if (isError)
            {
                if (hasId)
                    Logger::Log::Error("[Vulkan][{}][{}] {}: {}{}", severityStr, typeStr, messageId, msg, context);
                else
                    Logger::Log::Error("[Vulkan][{}][{}]: {}{}", severityStr, typeStr, msg, context);
            }
            else
            {
                if (hasId)
                    Logger::Log::Warning("[Vulkan][{}][{}] {}: {}{}", severityStr, typeStr, messageId, msg, context);
                else
                    Logger::Log::Warning("[Vulkan][{}][{}]: {}{}", severityStr, typeStr, msg, context);
            }
        }
        else if (ShouldLogValidationOccurrence(outcome))
        {
            const char* what = outcome.Overflowed ? "untracked validation messages" : (hasId ? messageId : "(id-less validation message)");
            if (isError)
                Logger::Log::Error("[Vulkan] {} repeated x{} (see get_validation_stats)", what, outcome.Count);
            else
                Logger::Log::Warning("[Vulkan] {} repeated x{} (see get_validation_stats)", what, outcome.Count);
        }

        // 3. Phase-3 assert, strictly after record + log so the stats and
        // first-occurrence capture survive the break. ERROR only; suppression
        // table hits skip the assert but were counted above.
        if (isError && !record.Suppressed && ValidationAssertEnabledFromEnv())
        {
            Logger::Log::Critical("[Vulkan] validation assert (GE_VK_VALIDATION_ASSERT): {}: {} objects:[{}] labels:[{}]",
                                  hasId ? messageId : "(no VUID)", msg, objects, labels);
            // Bounded: a break/abort without a flush loses the reason, but an
            // unbounded flush would hang here instead of breaking whenever the
            // drain thread is blocked behind the sink mutex.
            Logger::Log::FlushForCrash(kValidationAssertFlushTimeout);
            ValidationAssertBreak();
        }
    }

    return VK_FALSE;
}

VulkanDevice::VulkanDevice()
{
    // Every role resolves to the graphics context until BindQueueSubmitContexts runs, so a read
    // before bringup sees an unbound context (no queue, no timeline, counter 0) rather than a
    // null pointer.
    m_SubmitContextByRole.fill(&m_SubmitContextStorage[0]);
}

VulkanDevice::~VulkanDevice()
{
    if (!m_IsShutdown)
    {
        Shutdown();
    }
}

#if GE_ENABLE_DEBUG_BARRIERS
void VulkanDevice::ClearDebugBarriers()
{
    m_DebugBarriers.clear();
    const size_t target = std::max<size_t>(m_DebugBarriersHighWater, 256);
    if (m_DebugBarriers.capacity() < target)
        m_DebugBarriers.reserve(target);
}
#endif

// GE_DEVICE_RECOVERY kill switch, read once per process (design §12). See
// ParseDeviceRecoveryEnabled for the semantics.
static bool DeviceRecoveryEnabledFromEnv()
{
    static const bool kEnabled = ParseDeviceRecoveryEnabled(std::getenv("GE_DEVICE_RECOVERY"));
    return kEnabled;
}

bool VulkanDevice::Initialize(const DeviceDesc& desc)
{

    m_Health.Reset();
    m_RecoveryEnabled = DeviceRecoveryEnabledFromEnv();
    m_FaultInjection = FaultInjectionFromEnv();
    m_FaultFrameCounter = 0;
    m_FaultSubmitCounter = 0;
    m_InitDesc = desc; // stashed for RebuildDevice (Q6 slice 2)
    if (m_FaultInjection.Active())
    {
        Logger::Log::Warning("VulkanDevice: [fault-injection] active — GE_VK_FORCE_DEVICE_LOST / GE_VK_FORCE_BEGINFRAME_TIMEOUT set");
    }
    m_DebugLayerEnabled = desc.enableDebugLayer;
    m_ApplicationName = desc.applicationName;

    m_EnableDescriptorValidation = desc.enableDescriptorValidation;
    // Respect config toggle for bindless capability gating
    m_ForceDisableBindlessResources = desc.forceDisableBindlessResources;
    // Record whether swapchain/WSI should be enabled for this device
    m_UseSwapchain = desc.enableSwapchain;
    m_Vsync = desc.vsync;
    m_RequestedDescriptorBuffers = desc.descriptorBuffers;
    m_HdrState.enabled = desc.hdrEnabled;
    m_HdrState.requestedMode = desc.hdrEnabled ? desc.hdrMode : HdrOutputMode::Off;
    m_HdrState.activeMode = HdrOutputMode::Off;
    m_HdrState.swapchainBitDepth = desc.hdrSwapchainBitDepth;
    m_HdrState.targetDisplay = desc.hdrTargetDisplay;
    m_HdrState.staticMetadata = desc.hdrStaticMetadata;

    // Test lever for the 8-bit _SRGB presented-swapchain arm (#767): with the
    // 10-bit preference off, ChooseSwapSurfaceFormat falls through to the
    // B8G8R8A8_SRGB / R8G8B8A8_SRGB fallbacks, so SDR frames exercise the
    // Finalize D(c) arm (encode_srgb.frag outEncoding 6) against a real
    // hardware-sRGB ROP instead of the raw requantize into RGB10A2.
    m_FillNewTargetsWithNaN = IsEnvEnabled("GE_VK_FILL_NEW_TARGETS_NAN");
    if (m_FillNewTargetsWithNaN)
        Logger::Log::Warning("VulkanDevice: GE_VK_FILL_NEW_TARGETS_NAN — new float color targets and storage buffers start "
                             "as NaN and render-graph transients are not recycled, so a read before the first write "
                             "shows NaN (diagnostic; every such creation costs a fill)");
    if (IsEnvEnabled("GE_FORCE_8BIT_SRGB_SWAPCHAIN"))
    {
        SetPreferTenBitSwapchain(false);
        Logger::Log::Info(
            "VulkanDevice: GE_FORCE_8BIT_SRGB_SWAPCHAIN set — 10-bit swapchain preference off, "
            "expecting an 8-bit _SRGB surface format");
    }

    // Policy: clamp max push constants to device limit
    // Note: compute after selecting physical device so m_DeviceProperties is valid
    // (assigned below after SelectPhysicalDevice())

    if (!CreateInstance(desc))
    {
        Logger::Log::Error("Failed to create Vulkan instance");
        return false;
    }

    if (!SelectPhysicalDevice())
    {
        Logger::Log::Error("Failed to select physical device");
        return false;
    }

    return CreateDeviceAndResources(desc);
}

// VK_EXT_debug_utils is an INSTANCE extension, but every entry point below is
// DEVICE-level: enabling it once on the instance does not resolve them, and each
// path that produces a VkDevice — initial creation, the minimal fallback inside
// CreateLogicalDevice, and the post-loss rebuild — has to come back through here.
// Cleared both here and at device destruction (CleanupVulkan), so neither a
// rebuild nor the teardown-to-resolve window can see the previous device's
// pointers.
void VulkanDevice::ResolveDebugUtilsEntryPoints()
{
    m_SetVkObjectNameFn = nullptr;
    m_DebugUtilsLabelFns = {};

    if (!m_DebugUtilsExtEnabled)
    {
        return;
    }

    // vkGetDeviceProcAddr first: it yields this device's own dispatch and skips the
    // loader trampoline vkGetInstanceProcAddr hands back. Not every loader resolves
    // instance-extension commands that way, so the instance lookup stays as the
    // fallback rather than the primary.
    bool allViaDevice = true;
    auto resolve = [&](const char* name) -> PFN_vkVoidFunction
    {
        if (PFN_vkVoidFunction fn = vkGetDeviceProcAddr(m_Device, name))
        {
            return fn;
        }
        allViaDevice = false;
        return vkGetInstanceProcAddr(m_Instance, name);
    };

    m_SetVkObjectNameFn =
        reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(resolve("vkSetDebugUtilsObjectNameEXT"));

    DebugUtilsLabelFns fns{};
    fns.Begin = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(resolve("vkCmdBeginDebugUtilsLabelEXT"));
    fns.End = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(resolve("vkCmdEndDebugUtilsLabelEXT"));
    fns.Insert = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(resolve("vkCmdInsertDebugUtilsLabelEXT"));

    // All or none. A set missing only End opens labels it can never close, and an
    // unbalanced label tree misreports pass nesting in every tool that reads it —
    // worse than no labels at all, because it reads as data rather than as absence.
    if (fns.Begin == nullptr || fns.End == nullptr || fns.Insert == nullptr)
    {
        Logger::Log::Warning(
            "VulkanDevice: " VK_EXT_DEBUG_UTILS_EXTENSION_NAME " is enabled but its label entry points did "
            "not all resolve (begin={} end={} insert={}); GPU debug labels are off for this device",
            fns.Begin != nullptr,
            fns.End != nullptr,
            fns.Insert != nullptr);
        return;
    }

    m_DebugUtilsLabelFns = fns;
    Logger::Log::Info("VulkanDevice: " VK_EXT_DEBUG_UTILS_EXTENSION_NAME " labels active, resolved {}",
                      allViaDevice ? "via vkGetDeviceProcAddr"
                                   : "with a vkGetInstanceProcAddr fallback for at least one entry point");
}

// Everything from logical-device creation through the disk pipeline cache. Shared
// by Initialize (after instance + physical-device acquisition) and RebuildDevice
// (which re-runs this on the surviving instance/physical device — design F8).
bool VulkanDevice::CreateDeviceAndResources(const DeviceDesc& desc)
{
    // Bringup runs on the thread that owns this device's frames and teardown
    // (initial Initialize, and the rebuild path). The immediate destroy helpers
    // assert against this id; BeginFrame refreshes it each frame.
    m_DeviceOwnerThread.store(std::this_thread::get_id(), std::memory_order_relaxed);

    // Initialize policy: clamp to device limit now that m_DeviceProperties is valid
    m_MaxPushConstantBytes = std::min(desc.maxPushConstantBytes, GetMaxPushConstantsSize());

    if (!CreateLogicalDevice())
    {
        Logger::Log::Error("Failed to create logical device");
        return false;
    }

    // The resolved half of the GPU-AV report. Only here are both facts in hand:
    // what was asked of the layer (instance creation) and whether
    // VK_EXT_descriptor_buffer was actually enabled (just now). Re-stated on every
    // device rebuild because recovery re-runs this path and coverage after a
    // rebuild is a separate fact from coverage before it.
    if (m_GpuAvChained)
    {
        const GpuAvShaderInstrumentationState state = ClassifyGpuAvShaderInstrumentation(
            m_GpuAvShaderInstrumentationRequested, m_DescriptorBufferExtensionEnabledOnDevice);
        if (state == GpuAvShaderInstrumentationState::Active)
            Logger::Log::Info("VulkanDevice: {}", DescribeGpuAvShaderInstrumentation(state));
        else
            Logger::Log::Warning("VulkanDevice: {}", DescribeGpuAvShaderInstrumentation(state));
    }

    ResolveDebugUtilsEntryPoints();

    if (!CreateCommandPool())
    {
        Logger::Log::Error("Failed to create command pool");
        return false;
    }

    // Initialize descriptor set allocator with defaults
    {
        // One TLAS bind per ray-query pass per frame, across a handful of
        // consumers (RT shadow mask, DDGI classify + trace) — generous, and
        // only requested at all once CreateLogicalDevice actually enabled
        // acceleration structures.
        constexpr uint32_t kAccelerationStructureDescriptors = 64;
        DescriptorSetAllocator::Config cfg{};
        cfg.AccelerationStructures = m_RayQueryEnabledAtInit ? kAccelerationStructureDescriptors : 0;
        m_DsAllocator.Initialize(m_Device, cfg);
    }

    static_assert(MAX_FRAMES_IN_FLIGHT <= kMaxSupportedFramesInFlight,
                  "VulkanDevice MAX_FRAMES_IN_FLIGHT exceeds IDevice::kMaxSupportedFramesInFlight");

    // Initialize per-frame command buffer frames
    m_Frames.resize(MAX_FRAMES_IN_FLIGHT);
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        auto& fr = m_Frames[i];
        auto initPerQueue = [&](IDevice::QueueType qt, VkCommandPool pool, VkFence& fence)
        {
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fi.flags = 0; // Do not start signaled to avoid accidental signaled-fence submits
            vkCreateFence(m_Device, &fi, nullptr, &fence);
            // Use per-queue pools if available; fall back to main
            (void)qt;
            (void)pool;
        };
        initPerQueue(IDevice::QueueType::Graphics, m_CommandPool, fr.graphics.fence);
        if (m_ComputeCommandPool)
            initPerQueue(IDevice::QueueType::Compute, m_ComputeCommandPool, fr.compute.fence);
        if (m_TransferCommandPool)
            initPerQueue(IDevice::QueueType::Transfer, m_TransferCommandPool, fr.transfer.fence);
    }
    // Create per-queue timeline semaphores for cross-queue ordering (value 0 initially)
    m_GraphicsTimeline = CreateTimelineSemaphore(0);
    m_ComputeTimeline = CreateTimelineSemaphore(0);
    m_TransferTimeline = CreateTimelineSemaphore(0);

    // Queues and their timelines both exist now, so every submit path has its chokepoint. The
    // contexts also cache each timeline's VkSemaphore, which is what lets worker threads submit
    // without touching the m_TimelineSemaphores map.
    BindQueueSubmitContexts();

    QueryDeviceCapabilities();

    // Descriptor-buffer allocator: eager init when the feature is available AND the
    // runtime env flag enabled the path. The capability query above populated
    // m_Capabilities.supportsDescriptorBuffer from the init-time stash, so
    // IsDescriptorBufferEnabled() is authoritative here.
    if (IsDescriptorBufferEnabled())
    {
        m_DescriptorBufferPool = std::make_unique<VulkanDescriptorBufferPool>();
        VulkanDescriptorBufferPool::Config cfg;
        cfg.BlockSize      = 256 * 1024; // 256 KiB; spill-grow handles bursty frames
        cfg.FramesInFlight = MAX_FRAMES_IN_FLIGHT;
        if (!m_DescriptorBufferPool->Initialize(*this, cfg))
        {
            Logger::Log::Error("VulkanDevice: failed to initialize descriptor-buffer pool; falling back to legacy path for this frame");
            m_DescriptorBufferPool.reset();
            // Drop the cap flag so every downstream path treats DB as unavailable.
            m_Capabilities.supportsDescriptorBuffer = false;
            m_DescriptorBufferEnabledAtInit = false;
        }
    }

    // Create synchronization objects
    if (!CreateSyncObjects())
    {
        Logger::Log::Error("Failed to create synchronization objects");
        return false;
    }

    // Initialize QueryPool for GPU profiling and performance analysis

    IQueryPool::Config queryConfig;

    queryConfig.MaxTimestampQueries = 512;
    queryConfig.MaxOcclusionQueries = 256;
    queryConfig.MaxPipelineStatsQueries = 64;

    // Query pool slots are 1:1 with the device frame ring. Each slot is reused
    // only after its per-frame fence signals, matching the FrameEnd timestamp readback.
    queryConfig.FramesInFlight = MAX_FRAMES_IN_FLIGHT;
    queryConfig.EnableValidation = true;
    queryConfig.SupportsHostQueryReset = m_HostQueryResetEnabledAtInit;

    // Enable dynamic rendering only if requested and supported.
    m_EnableDynamicRendering = desc.enableDynamicRendering && m_SupportsDynamicRendering;

    m_QueryPool = std::make_unique<VulkanQueryPool>(m_Device, m_PhysicalDevice, queryConfig);
    if (!m_QueryPool->Initialize())
    {
        Logger::Log::Error("Failed to initialize QueryPool");
        return false;
    }

    // Initialize VMA (Vulkan Memory Allocator)

    if (!InitializeVMA())
    {
        Logger::Log::Error("Failed to initialize VMA (Vulkan Memory Allocator)");
        return false;
    }

    // Initialize pipeline cache for performance optimization

    // Configure pipeline cache with platform-specific directory
    VulkanPipelineCache::Config cacheConfig;
    cacheConfig.CacheDirectory = VulkanPipelineCache::GetPlatformCacheDirectory();

    m_VkDiskPipelineCache = std::make_unique<VulkanPipelineCache>(m_Device, cacheConfig);
    if (!m_VkDiskPipelineCache->Initialize())
    {
        Logger::Log::Error("Failed to initialize pipeline cache");
        return false;
    }
    // Wrap native Vulkan cache behind backend-agnostic interface
    m_PipelineCacheIface = std::make_unique<VulkanPipelineCacheAdapter>(m_VkDiskPipelineCache.get());

    return true;
}

void VulkanDevice::Shutdown()
{
    if (m_IsShutdown)
        return;
    m_IsShutdown = true;
    TextureUsagePolicy::Report();
    if (IsResourcePoolDiagnosticsEnabled())
    {
        UpdateResourcePoolHighWater();
        LogResourcePoolHighWater("pre-shutdown");
        constexpr size_t kSuspiciousPoolHighWater = 50000;
        if (m_ResourcePoolHighWater.deferredBuffers > kSuspiciousPoolHighWater ||
            m_ResourcePoolHighWater.deferredTextures > kSuspiciousPoolHighWater ||
            m_ResourcePoolHighWater.deferredStagingBuffers > kSuspiciousPoolHighWater ||
            m_ResourcePoolHighWater.liveBuffers > kSuspiciousPoolHighWater ||
            m_ResourcePoolHighWater.liveTextures > kSuspiciousPoolHighWater)
        {
            Logger::Log::Warning(
                "[VulkanPools] Suspicious high-water detected: deferred(B/T/V/S)={}/{}/{}/{} staging={} live(B/T/V)={}/{}/{}",
                m_ResourcePoolHighWater.deferredBuffers,
                m_ResourcePoolHighWater.deferredTextures,
                m_ResourcePoolHighWater.deferredTextureViews,
                m_ResourcePoolHighWater.deferredSamplers,
                m_ResourcePoolHighWater.deferredStagingBuffers,
                m_ResourcePoolHighWater.liveBuffers,
                m_ResourcePoolHighWater.liveTextures,
                m_ResourcePoolHighWater.liveTextureViews);
        }
    }
    // Shutdown is an explicit global teardown path; a global idle is expected.
    if (!EnsureGlobalGpuIdle())
    {
        Logger::Log::Warning("VulkanDevice::Shutdown proceeding after failed global idle ensure");
    }
    ProcessPendingWindowTargetRetirements(true);
    TeardownAllWindowTargetsForShutdown();

    // Let subsystems drop their per-device caches while the objects those handles
    // name are still alive, before the backend tears down Vulkan objects.
    InvokePerDeviceCacheCleanups();

    // Flush deferred destructions now that all queues are idle.
    FlushDeferredResourcesAndCompactLiveTracking();

    // AS backend teardown: after the global idle (its destructor destroys
    // VkAccelerationStructureKHR objects immediately) and before VMA teardown
    // (its storage/scratch buffers release through DestroyBuffer).
    m_AccelerationStructures.reset();

    // Ensure descriptor pools/sets are destroyed before any images/buffers they reference
    // This avoids validation errors when destroying image views still referenced by descriptor sets.
    // Descriptor-buffer pool must be destroyed before the VMA allocator (VMA is torn
    // down later in Shutdown); the pool owns VkBuffers + VmaAllocations.
    m_DescriptorBufferPool.reset();
    m_DsAllocator.Destroy();

    // Proactively destroy pipelines that belong to THIS VkDevice only
    {
        auto handles = GameEngine::Rendering::PipelineManager::Instance().GetAllHandlesCopy();
        for (auto h : handles)
        {
            if (auto* p = GetVulkanPipeline(h))
            {
                if (p->device == m_Device)
                {
                    DestroyPipeline(h);
                }
            }
        }
        // NOTE: Do NOT clear the global PipelineManager here; other devices/windows may still be alive.
    }
    {
        // Destroy any samplers created by this device
        auto liveSamplers = m_LiveSamplers;
        for (auto h : liveSamplers)
        {
            DestroySamplerImmediate(h);
        }
        m_LiveSamplers.clear();
    }

    // Clean up resources before shutting down VMA
    // IMPORTANT: Destroy GPU resources via device so VMA allocations are freed

    {
        // Destroy any textures created by this device
        // Use a copy because DestroyTexture modifies m_LiveTextures
        auto liveTextures = m_LiveTextures;
        for (auto h : liveTextures)
        {
            DestroyTexture(h);
        }
        m_LiveTextures.clear();
    }
    {
        // Destroy any buffers created by this device
        // Use a copy because DestroyBuffer modifies m_LiveBuffers
        auto liveBuffers = m_LiveBuffers;
        for (auto h : liveBuffers)
        {
            DestroyBuffer(h);
        }
        m_LiveBuffers.clear();
        if (m_PipelineCacheIface)
        {
            m_PipelineCacheIface.reset();
        }
    }

    // Shutdown pipeline cache (saves cache to disk)
    if (m_VkDiskPipelineCache)
    {
        m_VkDiskPipelineCache->Shutdown();
        m_VkDiskPipelineCache.reset();
    }

    // Shutdown query pool (cleanup Vulkan resources)
    if (m_QueryPool)
    {
        m_QueryPool->Shutdown();
        m_QueryPool.reset();
    }

    // Shutdown VMA before destroying the device
    ShutdownVMA();

    // Cleanup remaining Vulkan objects (descriptor allocator already destroyed)
    CleanupVulkan();
}

// Bounded retry budget for loss-during-rebuild (design M3). A double-fault that
// re-hits the wall within this many attempts is a persistently dying GPU; give up
// and surface save-and-restart rather than looping forever. The cap counts full
// rebuild attempts (each is one vkCreateDevice try). Raised 3 -> 4 with the
// timed-backoff schedule below: a real Windows TDR resets the adapter and the
// driver returns INITIALIZATION_FAILED to a vkCreateDevice issued before the reset
// settles, so the attempts are SPACED (0/1s/2s/4s = ~7s span) rather than fired
// immediately. Four backed-off attempts cover a real TDR without unbounded looping.
static constexpr uint32_t kMaxRebuildAttempts = 4;

// A rebuilt device that keeps rendering this long has recovered; a loss sooner
// than this means the rebuild replaced the device but not the fault, and the
// engine is looping rather than recovering. Sized well above one rebuild plus
// re-provision so a genuinely unrelated later loss starts a fresh episode.
static constexpr auto kRecoveryStickWindow = std::chrono::seconds(60);

// Non-holding recoveries tolerated before the loop is declared unrecoverable.
// kMaxRebuildAttempts cannot bound this: a successful re-provision zeroes the
// attempt counter, so a recover-relose loop spends no retry budget at all.
static constexpr uint32_t kMaxRecoveryCycles = 3;

// Settle delay BEFORE the attempt whose index is attemptsSoFar (0-based count of
// attempts already made). First attempt is immediate (fast path for a transient /
// injected loss on a still-live adapter); subsequent attempts back off so a real
// TDR reset can complete before the next vkCreateDevice.
static std::chrono::milliseconds RebuildBackoffFor(uint32_t attemptsSoFar)
{
    switch (attemptsSoFar)
    {
    case 0: return std::chrono::milliseconds(0);
    case 1: return std::chrono::milliseconds(1000);
    case 2: return std::chrono::milliseconds(2000);
    default: return std::chrono::milliseconds(4000);
    }
}

// Which device's rebuild mutex this thread already holds SHARED, via an outer
// guarded entry (UpdateBuffer -> CreateBuffer, SubmitTextureUploads ->
// CreateUploadBuffer/DestroyBuffer). Recursive shared acquisition of
// std::shared_mutex deadlocks once a writer queues between the two
// acquisitions, so nested guarded entries must skip.
static thread_local const VulkanDevice* tl_DeviceRebuildSharedHeld = nullptr;

VulkanDevice::DeviceRebuildSharedGuard::DeviceRebuildSharedGuard(VulkanDevice& device, Kind kind)
{
    (void)kind;
    if (device.m_RebuildExclusiveThread.load(std::memory_order_acquire) == std::this_thread::get_id())
    {
        // Rebuild thread: bringup re-enters create paths under the exclusive
        // lock (legal); the teardown window forbids create/upload by the
        // RegisterPerDeviceCacheCleanup contract — make a violation loud.
        assert(!(kind == Kind::Create && device.m_RebuildTeardownPhase.load(std::memory_order_relaxed)) &&
               "GPU resource create/upload during rebuild teardown — per-device cache cleanups must only "
               "destroy handles and drop state (RegisterPerDeviceCacheCleanup contract)");
        return;
    }
    if (tl_DeviceRebuildSharedHeld == &device)
    {
        return; // an outer guarded entry on this thread already holds it shared
    }
    device.m_DeviceRebuildMutex.lock_shared();
    m_PreviousHeld = tl_DeviceRebuildSharedHeld;
    tl_DeviceRebuildSharedHeld = &device;
    m_LockedDevice = &device;
}

VulkanDevice::DeviceRebuildSharedGuard::~DeviceRebuildSharedGuard()
{
    if (!m_LockedDevice)
    {
        return;
    }
    tl_DeviceRebuildSharedHeld = m_PreviousHeld;
    m_LockedDevice->m_DeviceRebuildMutex.unlock_shared();
}

bool VulkanDevice::RebuildDevice()
{
    if (!m_RecoveryEnabled)
    {
        return false;
    }
    if (m_Health.IsFailed())
    {
        return false; // terminal — only a fresh Initialize leaves Failed
    }
    // Enforce the recover-relose loop cap before the per-episode retry cap. A
    // rebuild that completes and re-provisions cleanly, only for the same fault to
    // kill the fresh device again, is not recovery — it is a loop that will run
    // until the process dies. Stop it here and surface save-and-restart, so the
    // device-lost site and GPU checkpoints logged above stay the last word.
    if (m_Health.RecoveryCycles() >= kMaxRecoveryCycles)
    {
        Logger::Log::Error(
            "VulkanDevice: device recovered and was lost again {} times, each within {} s of the previous "
            "recovery — the fault reproduces on every rebuilt device, so rebuilding cannot fix it. "
            "Transitioning to Failed (save-and-restart) instead of looping. See the device-lost site and "
            "GPU-executed checkpoints logged above for the faulting pass; re-run with "
            "GE_VK_GPU_CHECKPOINTS=1 if they were not recording.",
            (unsigned)m_Health.RecoveryCycles(),
            (long long)std::chrono::duration_cast<std::chrono::seconds>(kRecoveryStickWindow).count());
        m_Health.TransitionToFailed();
        return false;
    }
    // Enforce the bounded-retry cap before starting another attempt (M3).
    if (m_Health.RebuildAttempts() >= kMaxRebuildAttempts)
    {
        Logger::Log::Error(
            "VulkanDevice: device rebuild exhausted its {}-attempt budget — transitioning to Failed (save-and-restart)",
            (unsigned)kMaxRebuildAttempts);
        m_Health.TransitionToFailed();
        return false;
    }
    // Only ever entered from a latched loss, and never re-entered while already
    // Rebuilding (the state machine rejects a second BeginRebuild).
    if (!m_Health.BeginRebuild())
    {
        Logger::Log::Warning("VulkanDevice: RebuildDevice ignored — health is {} (a rebuild is only started from Lost)",
                             DeviceHealthToString(m_Health.Load()));
        return false;
    }

    m_LastRebuildAttemptTime = std::chrono::steady_clock::now();
    Logger::Log::Warning("VulkanDevice: device rebuild starting (attempt {}/{})",
                         (unsigned)m_Health.RebuildAttempts(), (unsigned)kMaxRebuildAttempts);

    // Step 2 (design §7): quiesce async device users. Take the rebuild lock
    // EXCLUSIVE for the whole teardown+bringup window (MUST-FIX 1): every
    // guarded device-touching entry (DeviceRebuildSharedGuard) and job-thread
    // SubmitTextureUploads hold it SHARED for their entire duration (VMA prep
    // included), so acquiring it exclusive here blocks until every in-flight
    // use has finished and prevents any new one from touching the device
    // mid-rebuild — closing the prep-phase TOCTOU against ShutdownVMA. Worker
    // thread-local command pools additionally self-heal via the
    // m_DeviceGeneration bump in DestroyAllThreadPools (design F4). The lock is
    // released before the re-provision seam so a slice-3a consumer can never
    // deadlock re-entering an upload path from the render thread.
    // A thread already holding the rebuild lock SHARED (inside a guarded device
    // entry) cannot upgrade: the exclusive acquisition below would self-deadlock
    // silently. Make the misuse loud instead.
    assert(tl_DeviceRebuildSharedHeld != this &&
           "RebuildDevice entered while this thread holds the rebuild lock shared (guarded device entry)");
    {
        std::unique_lock<std::shared_mutex> rebuildLock(m_DeviceRebuildMutex);
        // Declared after rebuildLock, so its destructor runs BEFORE the mutex
        // releases on every exit path: the flags below are only ever true while
        // the exclusive lock is held. A shared-lock holder therefore never
        // observes a mid-rebuild InTeardown() — worker Destroy* always routes
        // to the deferred queues, never into the immediate helpers — and the
        // guarded entries' compare-and-skip against m_RebuildExclusiveThread is
        // exact.
        struct RebuildExclusiveScope
        {
            VulkanDevice& Device;
            explicit RebuildExclusiveScope(VulkanDevice& device) : Device(device)
            {
                Device.m_RebuildExclusiveThread.store(std::this_thread::get_id(), std::memory_order_release);
                Device.m_RebuildInProgress = true;
                Device.m_RebuildTeardownPhase.store(true, std::memory_order_release);
            }
            ~RebuildExclusiveScope()
            {
                Device.m_RebuildTeardownPhase.store(false, std::memory_order_release);
                Device.m_RebuildInProgress = false;
                Device.m_RebuildExclusiveThread.store(std::thread::id{}, std::memory_order_release);
            }
        } rebuildScope(*this);

        // Bounded drain (MUST-FIX 2): vkDeviceWaitIdle is unbounded and would wedge
        // forever on a hung-escalated-to-lost GPU (design §6 requires teardown waits
        // to be bounded/skippable when the device is not healthy). Wait on the
        // tracked per-queue timelines under a single deadline instead: fast when
        // truly lost (returns DEVICE_LOST) or when the loss was injected (device
        // alive → a real drain), and a bounded give-up on a genuine wedge. We
        // proceed with teardown regardless — the device is being replaced wholesale.
        if (m_Device != VK_NULL_HANDLE)
        {
            const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            auto remainingNs = [&]() -> uint64_t
            {
                const auto left = drainDeadline - std::chrono::steady_clock::now();
                return left <= std::chrono::steady_clock::duration::zero()
                           ? 0ull
                           : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count());
            };
            auto drainTimeline = [&](SemaphoreHandle timeline, uint64_t value, const char* name)
            {
                if (value == 0)
                    return;
                const TimelineWaitOutcome outcome = WaitTimelineSemaphore(timeline, value, remainingNs());
                switch (outcome)
                {
                case TimelineWaitOutcome::Completed:
                    return;
                case TimelineWaitOutcome::DeviceLost:
                    // Nothing to drain and no budget was consumed: the work is gone
                    // with the device. Reporting a timeout here sends the reader
                    // hunting a hang that never happened.
                    Logger::Log::Warning("VulkanDevice: rebuild {} drain skipped — device is lost, its work is already gone", name);
                    return;
                case TimelineWaitOutcome::TimedOut:
                    Logger::Log::Warning("VulkanDevice: rebuild {} drain did not complete within budget; proceeding with teardown", name);
                    return;
                case TimelineWaitOutcome::NotATimeline:
                case TimelineWaitOutcome::Failed:
                    Logger::Log::Warning("VulkanDevice: rebuild {} drain could not run ({}); proceeding with teardown",
                                         name,
                                         TimelineWaitOutcomeToString(outcome));
                    return;
                }
            };
            // Drain each context once: roles that alias one physical queue share a context, so
            // draining per role would wait on the same timeline twice and burn the budget.
            static constexpr const char* kContextNames[] = {"graphics", "compute", "transfer"};
            for (size_t i = 0; i < m_SubmitContextStorage.size(); ++i)
            {
                const QueueSubmitContext& context = m_SubmitContextStorage[i];
                drainTimeline(context.Timeline(), context.LastSignalled(), kContextNames[i]);
            }
        }
        m_DeviceKnownIdle = true;

        // Step 3 (design §7): unconditionally purge the timeline-keyed deferred lists.
        // Their completedTimeline >= value predicates never fire after the counters
        // reset to 0, so waiting would hang; the device is dead, so drain-then-destroy
        // without waiting (mirrors the shutdown path).
        FlushDeferredResourcesAndCompactLiveTracking();

        // Step 4 (design §7): tear down device-scoped objects, keeping the shared
        // VkInstance + per-window VkSurfaceKHR (both instance-scoped, design fold).
        std::vector<RetainedWindowTarget> retainedTargets;
        TeardownDeviceScopedForRebuild(retainedTargets);

        // Step 5 (design §7): reset per-frame + timeline counters to fresh-device
        // semantics — AFTER the purge, BEFORE bringup.
        ResetFrameAndTimelineCountersForRebuild();

        // Teardown is complete: bringup may create GPU resources again (still on
        // this thread, still under the exclusive lock — the guarded create
        // entries skip via m_RebuildExclusiveThread).
        m_RebuildTeardownPhase.store(false, std::memory_order_release);

        // Step 6 (design §7): bring up a fresh device on the surviving instance +
        // physical device (F8: CreateInstance/SelectPhysicalDevice are NOT re-run, so
        // the instance refcount is not re-incremented). CreateDeviceAndResources
        // reconstructs the disk pipeline cache, whose Initialize() reloads the on-disk
        // bytes — warm recompiles, no frame-2 compile storm (design M2).
        if (!CreateDeviceAndResources(m_InitDesc))
        {
            Logger::Log::Error("VulkanDevice: device rebuild FAILED during bringup");
            m_Health.NoteRebuildFailed(); // -> Lost; the next BeginFrame retries until the cap
            return false;
        }

        // Step 7 (design §7): recreate each window target's swapchain on its kept surface.
        if (!RecreateWindowTargetSwapchainsAfterRebuild(retainedTargets))
        {
            Logger::Log::Error("VulkanDevice: device rebuild FAILED during swapchain recreation");
            m_Health.NoteRebuildFailed();
            return false;
        }
    } // release the exclusive rebuild lock: device is fully rebuilt and usable

    // Step 9 BEFORE step 8 (deliberate): move to AwaitingReprovision FIRST so the
    // re-provision callbacks below run on a device that reports IsDeviceUsable()
    // (Healthy || AwaitingReprovision). Buffer/texture creation and, crucially,
    // SubmitTextureUploads gate on IsDeviceUsable — a consumer recreating GPU
    // resources inside the callback would be silently rejected while the health
    // still reads Rebuilding. Rendering stays suppressed (AwaitingReprovision is
    // not Healthy) until a consumer calls NotifyReprovisionComplete(). Setting the
    // state before the (fallible) callbacks also means a throwing consumer leaves
    // the device in the correct suppressed-but-usable state, not mid-Rebuilding.
    m_Health.NoteRebuildSucceeded();
    // Bump BEFORE the re-provision callbacks so any consumer that polls inside a
    // callback observes the new generation. Poll-based recovery consumers (slice
    // 3b ECS pass, EZTree, HLOD) compare this on their own thread to run once.
    m_DeviceRebuildGeneration.fetch_add(1, std::memory_order_relaxed);
    Logger::Log::Warning(
        "VulkanDevice: device rebuild SUCCEEDED — device functional, health=AwaitingReprovision "
        "(rendering suppressed until re-provision completes)");

    // Step 8 (design §7): re-provision seam. Slice-3a consumers (RenderServices)
    // recreate their GPU resources here; a consumer that fully restores a
    // renderable picture then calls NotifyReprovisionComplete() to resume. With
    // no consumer registered (slice 2 alone) the device stays AwaitingReprovision.
    InvokeDeviceRebuiltCallbacks();
    return true;
}

void VulkanDevice::TickDeviceRecovery()
{
    // Drive the rebuild retry from the render-loop tick + health state, NOT from
    // submit/BeginFrame — a failed rebuild suppresses rendering, so the render path
    // stops issuing frames/submits and a submit-driven retry starves (the recorded
    // M3 gap). The caller invokes this every tick UNCONDITIONALLY, before the
    // (possibly suppressed) per-window render.
    if (!m_RecoveryEnabled || !m_Health.IsLost())
        return;

    // Timed backoff: a real Windows TDR resets the adapter, and vkCreateDevice
    // returns INITIALIZATION_FAILED until the reset settles. Space the attempts
    // (0/1s/2s/4s) so a later attempt lands on a settled adapter. The first attempt
    // is immediate (a transient/injected loss on a still-live adapter recovers at
    // once). RebuildAttempts() is the count already made; it is 0 before the first
    // and only resets on a successful re-provision.
    const uint32_t attemptsSoFar = m_Health.RebuildAttempts();
    if (attemptsSoFar > 0)
    {
        const auto sinceLast = std::chrono::steady_clock::now() - m_LastRebuildAttemptTime;
        if (sinceLast < RebuildBackoffFor(attemptsSoFar))
            return; // still settling — retry on a later tick
    }
    RebuildDevice();
}

void VulkanDevice::NotifyReprovisionComplete()
{
    // Slice-3a seam consumer calls this after re-provisioning the upper layers.
    if (m_Health.NoteReprovisioned(std::chrono::steady_clock::now()))
    {
        Logger::Log::Info("VulkanDevice: re-provision complete — health=Healthy, rendering resumed");
    }
}

void VulkanDevice::TeardownDeviceScopedForRebuild(std::vector<RetainedWindowTarget>& outTargets)
{
    // Same first step as Shutdown, and for the same reason: a consumer cache must
    // drop its handles while they still resolve. Everything below frees the
    // generational slots those handles name, and m_RebuildInProgress already routes
    // Destroy* straight to the immediate helpers, so a cleanup running here destroys
    // its own objects exactly once instead of queueing a deferred destroy of a slot
    // this function is about to free.
    InvokePerDeviceCacheCleanups();

    // Fold the active target back into the map so every live target is walked
    // uniformly, then capture each surface + extent to reuse after bringup and
    // DETACH the surface (instance-scoped: it survives the device, and destroying
    // it here would strand the window).
    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0)
    {
        SaveActiveWindowTargetState();
    }
    outTargets.clear();
    outTargets.reserve(m_WindowTargets.size());
    for (auto& kv : m_WindowTargets)
    {
        RetainedWindowTarget rt;
        rt.id = kv.first;
        rt.surface = kv.second.surface;
        rt.width = kv.second.swapchainExtent.width;
        rt.height = kv.second.swapchainExtent.height;
        rt.hdrState = kv.second.hdrState;
        rt.wasActive = (kv.first == m_ActiveWindowTargetId);
        outTargets.push_back(rt);
        kv.second.surface = VK_NULL_HANDLE; // keep the surface across the rebuild
    }
    // The member mirror also holds the active surface; null it so CleanupVulkan's
    // surface teardown (guarded on VK_NULL_HANDLE) skips it.
    m_Surface = VK_NULL_HANDLE;

    // Device-scoped resource destruction — mirrors the body of Shutdown(), but keeps
    // the instance + surfaces (CleanupVulkan skips them while m_RebuildInProgress).
    // Deferred lists were already purged by the caller.
    //
    // Acceleration structures die with this device, so they are destroyed against it
    // here, before VMA and the device go. The backend object itself is kept (bringup
    // rebinds it) so consumers' backend pointers and TLAS slot handles stay valid.
    if (m_AccelerationStructures)
        m_AccelerationStructures->ReleaseForDeviceRebuild();
    m_DescriptorBufferPool.reset();
    m_DsAllocator.Destroy();

    // Concrete VkPipelines for THIS device. The intern tables + SPIR-V live in the
    // base IDevice pipeline cache and survive, so PSOs recompile lazily afterward.
    {
        auto handles = GameEngine::Rendering::PipelineManager::Instance().GetAllHandlesCopy();
        for (auto h : handles)
        {
            if (auto* p = GetVulkanPipeline(h))
            {
                if (p->device == m_Device)
                {
                    DestroyPipeline(h);
                }
            }
        }
    }
    // These must destroy now, not queue: the caller already purged the deferred
    // lists, and ShutdownVMA below would run with live allocations. m_RebuildInProgress
    // puts the device in teardown, so the public entry points route straight through
    // to the immediate helpers.
    {
        auto liveSamplers = m_LiveSamplers;
        for (auto h : liveSamplers)
        {
            DestroySamplerImmediate(h);
        }
        m_LiveSamplers.clear();
    }
    {
        auto liveTextures = m_LiveTextures;
        for (auto h : liveTextures)
        {
            DestroyTexture(h);
        }
        m_LiveTextures.clear();
    }
    {
        auto liveBuffers = m_LiveBuffers;
        for (auto h : liveBuffers)
        {
            DestroyBuffer(h);
        }
        m_LiveBuffers.clear();
        m_PipelineCacheIface.reset();
    }

    // Drop the dead disk-pipeline-cache object WITHOUT re-persisting it: bringup
    // reconstructs a fresh cache that reloads the same on-disk bytes, so saving
    // here is redundant and (per the slice-2 smoke) dominates recovery wall-time.
    // The last-good on-disk cache from startup / the previous clean shutdown is
    // preserved, and the physical-device UUID is unchanged post-TDR so the reload
    // is warm (design M2). The normal Shutdown() path still saves (default arg).
    if (m_VkDiskPipelineCache)
    {
        m_VkDiskPipelineCache->Shutdown(/*saveToDisk=*/false);
        m_VkDiskPipelineCache.reset();
    }
    if (m_QueryPool)
    {
        m_QueryPool->Shutdown();
        m_QueryPool.reset();
    }
    ShutdownVMA();

    // Destroys the remaining device objects and the VkDevice itself; skips the
    // instance + surface teardown / refcount decrement while m_RebuildInProgress.
    CleanupVulkan();
}

void VulkanDevice::ResetFrameAndTimelineCountersForRebuild()
{
    m_CurrentFrame = 0;
    // The fences that could have held slot 0 died with the old device, so the
    // fresh one starts in the same state as a device that has never presented.
    m_CurrentFrameSlotFenced.store(true, std::memory_order_relaxed);
    m_CurrentSwapchainImage = 0;
    // Unbind rather than merely zero the counters: the queues and semaphores these contexts hold
    // belong to the device being torn down. Bringup rebinds them against the fresh handles, whose
    // timelines start at 0 again.
    for (QueueSubmitContext& context : m_SubmitContextStorage)
        context.Unbind();
    m_SubmitContextByRole.fill(&m_SubmitContextStorage[0]);
    m_GraphicsTimeline = {};
    m_ComputeTimeline = {};
    m_TransferTimeline = {};
    m_DeviceKnownIdle = true;   // a fresh device has no in-flight work
    m_AnyGraphicsSubmitThisFrame = false;
    m_DidBackbufferRenderThisFrame = false;
    m_HaveReadAnyFrameEndTimestamp = false;
    m_LastReadFrameEndTimestamp = 0;
    for (uint32_t& q : m_FrameEndQuery)
    {
        q = UINT32_MAX;
    }
    // m_Frames hold stale (destroyed) fences + command buffers from the old device;
    // clear so CreateDeviceAndResources rebuilds them from scratch.
    m_Frames.clear();
}

bool VulkanDevice::RecreateWindowTargetSwapchainsAfterRebuild(const std::vector<RetainedWindowTarget>& targets)
{
    if (targets.empty())
    {
        return true; // headless / no windows attached
    }

    // Keep m_NextWindowTargetId ahead of every reused id up front, so the failure
    // path below (which re-seeds m_WindowTargets) can never hand out a colliding id.
    for (const auto& rt : targets)
    {
        if (rt.id >= m_NextWindowTargetId)
        {
            m_NextWindowTargetId = rt.id + 1;
        }
    }

    uint64_t activeId = 0;
    for (size_t i = 0; i < targets.size(); ++i)
    {
        const auto& rt = targets[i];
        if (rt.surface == VK_NULL_HANDLE)
        {
            continue;
        }
        ResetWindowTargetMembers();
        m_Surface = rt.surface;
        m_HdrState = rt.hdrState;
        const uint32_t w = rt.width > 0 ? rt.width : 1;
        const uint32_t h = rt.height > 0 ? rt.height : 1;
        if (!CreateSwapchain(w, h) || !CreateActiveWindowTargetPresentFence())
        {
            Logger::Log::Error("VulkanDevice: rebuild failed to recreate swapchain for window target {}",
                               (unsigned long long)rt.id);
            // SHOULD-FIX 4: do NOT drop the remaining windows or leak their surfaces.
            // Preserve EVERY not-yet-recreated surface back into m_WindowTargets so the
            // next teardown re-captures all of them and the retry re-attempts every
            // window. The failed target keeps whatever partial objects CreateSwapchain
            // left (captured here, destroyed on the next teardown); the unprocessed
            // targets get a swapchain-less carrier holding just the surface + extent.
            m_WindowTargets[rt.id] = CaptureWindowTargetState();
            for (size_t j = i + 1; j < targets.size(); ++j)
            {
                const RetainedWindowTarget& rem = targets[j];
                if (rem.surface == VK_NULL_HANDLE)
                {
                    continue;
                }
                WindowTargetState carrier{};
                carrier.surface = rem.surface;
                carrier.swapchainExtent = {rem.width, rem.height};
                carrier.hdrState = rem.hdrState;
                m_WindowTargets[rem.id] = std::move(carrier);
            }
            // Ownership of the failed target's objects is now in the map entry;
            // clear the member mirror so nothing double-frees on the next teardown.
            ResetWindowTargetMembers();
            return false;
        }
        m_WindowTargets[rt.id] = CaptureWindowTargetState();
        if (rt.wasActive)
        {
            activeId = rt.id;
        }
    }

    // Restore the active target's members (fall back to any target if the previously
    // active one was not recreated).
    if (activeId == 0 && !m_WindowTargets.empty())
    {
        activeId = m_WindowTargets.begin()->first;
    }
    if (activeId != 0)
    {
        auto it = m_WindowTargets.find(activeId);
        if (it != m_WindowTargets.end())
        {
            ApplyWindowTargetState(it->second);
            m_ActiveWindowTargetId = activeId;
            m_HasActiveWindowTarget = true;
        }
    }
    return true;
}

bool VulkanDevice::CreateInstance(const DeviceDesc& desc)
{
#if defined(PLATFORM_MACOS)
    // If the app is launched from Finder on a machine without the full Vulkan SDK installed,
    // the Vulkan loader may not be able to locate MoltenVK's ICD JSON automatically.
    //
    // We stage `MoltenVK_icd.json` into the app bundle at:
    //   Editor.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json
    // and prefer that VK_ICD_FILENAMES at runtime.
    // This is a temporary bridge for local MoltenVK patches and can be relaxed
    // once official MoltenVK releases expose the required support.
    // Upstream PR: https://github.com/KhronosGroup/MoltenVK/pull/2740
    //
    // This keeps Vulkan startup self-contained for the Editor app bundle.
    auto bootstrapMoltenVkIcd = []()
    {
        auto envListHasExistingPath = [](const char* value) -> bool
        {
            if (!value || value[0] == '\0')
                return false;
            try
            {
                std::string v(value);
                // Vulkan loader uses ':' on Unix-like platforms for multiple ICD jsons.
                size_t start = 0;
                while (start <= v.size())
                {
                    size_t end = v.find(':', start);
                    if (end == std::string::npos)
                        end = v.size();
                    std::string token = v.substr(start, end - start);
                    if (!token.empty())
                    {
                        std::error_code ec;
                        if (std::filesystem::exists(std::filesystem::path(token), ec))
                            return true;
                    }
                    if (end == v.size())
                        break;
                    start = end + 1;
                }
            }
            catch (...)
            {
                return false;
            }
            return false;
        };

        const char* existing = std::getenv("VK_ICD_FILENAMES");
        const char* existingDriverFiles = std::getenv("VK_DRIVER_FILES");
        const bool preferExistingIcd =
            (std::getenv("GE_USE_SYSTEM_MOLTENVK") && std::getenv("GE_USE_SYSTEM_MOLTENVK")[0] != '\0');

        auto trySetIcd = [existing, existingDriverFiles](const std::filesystem::path& p, bool preferBundle) -> bool
        {
            std::error_code ec;
            std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
            if (ec || canon.empty() || !std::filesystem::exists(canon))
                return false;

            const std::string path = canon.string();
            ::setenv("VK_ICD_FILENAMES", path.c_str(), preferBundle ? 1 : 0);
            ::setenv("VK_DRIVER_FILES", path.c_str(), preferBundle ? 1 : 0);
            const bool overrodeIcd =
                preferBundle && existing && existing[0] != '\0' && path != existing;
            const bool overrodeDriver =
                preferBundle && existingDriverFiles && existingDriverFiles[0] != '\0' &&
                path != existingDriverFiles;
            if (overrodeIcd)
                Logger::Log::Info(std::string("macOS: overriding VK_ICD_FILENAMES '") + existing + "' with bundled MoltenVK ICD: " + path);
            if (overrodeDriver)
                Logger::Log::Info(std::string("macOS: overriding VK_DRIVER_FILES '") + existingDriverFiles + "' with bundled MoltenVK ICD: " + path);
            if (!overrodeIcd && !overrodeDriver)
                Logger::Log::Info(std::string("macOS: using MoltenVK ICD: ") + path);
            return true;
        };

        // Resolve executable directory via _NSGetExecutablePath (avoid Engine::PathUtils dependency).
        std::filesystem::path exeDir;
        {
            uint32_t size = 0;
            // Probe required size (call returns non-zero and sets size when buffer is too small).
            (void)_NSGetExecutablePath(nullptr, &size);
            if (size == 0)
                return;

            std::string buf;
            buf.resize(size + 1, '\0');
            uint32_t outSize = size;
            if (_NSGetExecutablePath(buf.data(), &outSize) != 0)
                return;

            // Canonicalize when possible (symlinks, .. components).
            std::error_code ec;
            std::filesystem::path full = std::filesystem::path(buf.c_str());
            std::filesystem::path canon = std::filesystem::canonical(full, ec);
            exeDir = ec ? full.parent_path() : canon.parent_path();
        }

        if (exeDir.empty())
            return;

        if (preferExistingIcd && (envListHasExistingPath(existing) || envListHasExistingPath(existingDriverFiles)))
        {
            Logger::Log::Info("macOS: GE_USE_SYSTEM_MOLTENVK set; using existing Vulkan ICD/driver environment.");
            return;
        }

        // 1) Prefer the app bundle-staged ICD (Editor.app).
        if (trySetIcd(exeDir / ".." / "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json", true))
            return;

        if (envListHasExistingPath(existing))
        {
            Logger::Log::Info(std::string("macOS: VK_ICD_FILENAMES already set: ") + existing);
            return;
        }

        if (existing && existing[0] != '\0')
            Logger::Log::Warning(std::string("macOS: VK_ICD_FILENAMES was set but did not point to an existing ICD json; overriding. Value was: ") + existing);

        // 2) Fall back to Vulkan SDK if present (useful for terminal launches).
        if (const char* sdk = std::getenv("VULKAN_SDK"); sdk && sdk[0] != '\0')
        {
            if (trySetIcd(std::filesystem::path(sdk) / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json", true))
                return;
            if (trySetIcd(std::filesystem::path(sdk) / "MoltenVK" / "icd" / "MoltenVK_icd.json", true))
                return;
        }

        // 3) Common Homebrew/system install locations.
        if (trySetIcd("/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json", true))
            return;
        if (trySetIcd("/usr/local/share/vulkan/icd.d/MoltenVK_icd.json", true))
            return;
        if (trySetIcd("/etc/vulkan/icd.d/MoltenVK_icd.json", true))
            return;

        // 4) Default LunarG Vulkan SDK user install: ~/VulkanSDK/<version>/macOS/...
        if (const char* home = std::getenv("HOME"); home && home[0] != '\0')
        {
            std::error_code ec;
            const std::filesystem::path root = std::filesystem::path(home) / "VulkanSDK";
            if (std::filesystem::exists(root, ec) && std::filesystem::is_directory(root, ec))
            {
                // Pick the lexicographically largest version directory (good enough for typical "X.Y.Z" names).
                std::filesystem::path best;
                for (const auto& e : std::filesystem::directory_iterator(root, ec))
                {
                    if (ec)
                        break;
                    if (!e.is_directory(ec))
                        continue;
                    const auto name = e.path().filename().string();
                    if (name.empty())
                        continue;
                    if (best.empty() || name > best.filename().string())
                        best = e.path();
                }
                if (!best.empty())
                {
                    if (trySetIcd(best / "macOS" / "share" / "vulkan" / "icd.d" / "MoltenVK_icd.json", true))
                        return;
                    if (trySetIcd(best / "macOS" / "MoltenVK" / "icd" / "MoltenVK_icd.json", true))
                        return;
                }
            }
        }

        Logger::Log::Warning(
            "macOS: VK_ICD_FILENAMES is not set and MoltenVK_icd.json was not found in the app bundle or common install locations. "
            "Vulkan may fail to initialize when launching from Finder/Xcode. Ensure MoltenVK is bundled into Editor.app or install the Vulkan SDK/Homebrew MoltenVK.");
    };

    bootstrapMoltenVkIcd();
#endif

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = desc.applicationName.c_str();
    appInfo.applicationVersion = desc.applicationVersion;
    appInfo.pEngineName = "GameEngine";
    // The calendar version packed as (year - 2000, month, patch): the packed major has 7 bits,
    // too few for the year itself. The prerelease suffix has no field and is dropped.
    static_assert(GameEngine::kEngineVersionYear >= 2000 && GameEngine::kEngineVersionYear - 2000 < 128,
                  "the engine version's year no longer fits VkApplicationInfo::engineVersion's 7-bit major");
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, GameEngine::kEngineVersionYear - 2000u,
                                                GameEngine::kEngineVersionMonth, GameEngine::kEngineVersionPatch);
    // Request the highest Vulkan API version supported by the loader/driver.
    // On platforms where vkEnumerateInstanceVersion is not available, fall back
    // to Vulkan 1.0 (per spec). This avoids VK_ERROR_INCOMPATIBLE_DRIVER when
    // the application requests a higher version than the loader supports.
    uint32_t loaderVersion = VK_API_VERSION_1_0;
#if defined(VK_API_VERSION_1_1)
    if (auto* pfnEnumerateInstanceVersion =
            reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion")))
    {
        uint32_t reported = 0;
        if (pfnEnumerateInstanceVersion(&reported) == VK_SUCCESS && reported != 0)
        {
            loaderVersion = reported;
        }
    }
#endif
    // Clamp to at most 1.3, but do not exceed what the loader reports.
    const uint32_t kMaxRequestedApi = VK_API_VERSION_1_3;
    uint32_t requestedApi = loaderVersion;
    if (requestedApi > kMaxRequestedApi)
    {
        requestedApi = kMaxRequestedApi;
    }
    appInfo.apiVersion = requestedApi;
    // Retained: spec-legality of core physical-device queries (e.g. the tooling
    // query) is judged against the version the instance was CREATED with, not
    // against what the loader could have offered.
    m_InstanceApiVersion = requestedApi;

    // Layer settings for VK_LAYER_KHRONOS_validation: always the dedup-cap removal
    // (ValidationStatsStore owns dedup and must count exactly), plus synchronization
    // validation when GE_VK_SYNC_VALIDATION asks for it. Resolved ahead of the
    // sharing lookup because these are instance-level: they are part of which
    // instance this device is asking for, not something it can add to one later.
    ValidationLayerSettingsDesc validationDesc{};
    validationDesc.SyncValidation = ParseSyncValidationEnabled(std::getenv("GE_VK_SYNC_VALIDATION"));
    validationDesc.GpuAssistedValidation = ParseGpuAssistedValidationEnabled(std::getenv("GE_VK_GPU_AV"));

    // Shader instrumentation has to be asked for here, at instance creation, but
    // whether VK_EXT_descriptor_buffer ends up enabled is settled at device
    // creation — the physical device is not even selected yet. What IS knowable
    // now is when the extension CANNOT be enabled: capture-compat drops it
    // outright, and so does a caller that asked for no descriptor buffers. Auto
    // asks for instrumentation exactly then, which is the only direction that is
    // sound this early. GE_VK_GPU_AV_SHADER_INSTRUMENTATION overrides in both
    // directions so the two control arms — instrumentation asked for WITH the
    // extension, and withheld WITHOUT it — are reachable at all; they are what
    // proves which gate a given run was stopped by.
    const bool descriptorBufferPossible =
        (desc.descriptorBuffers != DescriptorBufferMode::Disabled) && !IsCaptureCompatRequested();
    validationDesc.GpuAvShaderInstrumentation = ShouldRequestGpuAvShaderInstrumentation(
        validationDesc.GpuAssistedValidation,
        descriptorBufferPossible,
        ParseGpuAvShaderInstrumentationMode(std::getenv("GE_VK_GPU_AV_SHADER_INSTRUMENTATION")));

    // Read here rather than at the messenger below: the messenger is created once
    // per instance and inherited as it stands, so asking for verbose output is part
    // of which instance this device is asking for, not something it can widen later.
    const bool verboseMessenger = IsEnvEnabled("GE_VK_VALIDATION_VERBOSE");

    // Share an existing instance only with a device that is asking for the same
    // one — see SharedInstanceKey for what that means. Built from the members the
    // creation path below actually consumes, and captured before the availability
    // downgrade at the layer check, so the key states the request.
    SharedInstanceKey instanceKey{};
    instanceKey.ApplicationName = desc.applicationName;
    instanceKey.ApplicationVersion = desc.applicationVersion;
    instanceKey.DebugLayer = m_DebugLayerEnabled;
    instanceKey.VerboseMessenger = verboseMessenger;
    instanceKey.SurfaceExtensions = m_UseSwapchain;
    instanceKey.SyncValidation = validationDesc.SyncValidation;
    instanceKey.GpuAssistedValidation = validationDesc.GpuAssistedValidation;
    instanceKey.GpuAvShaderInstrumentation = validationDesc.GpuAvShaderInstrumentation;

    if (const auto shared = SharedInstanceRegistry::Get().Acquire(instanceKey))
    {
        m_Instance = shared->Instance;
        m_DebugMessenger = shared->DebugMessenger;
        // Instance-level results are the creating device's, not re-derived here: a
        // sharer that assumed its own request had been granted would report a
        // diagnostic as armed while nothing was validating.
        m_DebugLayerEnabled = shared->DebugLayerActive;
        m_DebugUtilsExtEnabled = shared->DebugUtilsExtEnabled;
        m_InstanceApiVersion = shared->InstanceApiVersion;
        m_SwapchainColorSpaceExtEnabled = shared->SwapchainColorSpaceExtEnabled;
        m_GpuAvChained = shared->GpuAvChained;
        m_GpuAvShaderInstrumentationRequested = shared->GpuAvShaderInstrumentationRequested;
        return true;
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    // Get required extensions
    std::vector<const char*> extensions;

    // Get GLFW required extensions (may be null/zero in headless/offscreen tests)
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions && glfwExtensionCount > 0)
    {
        for (uint32_t i = 0; i < glfwExtensionCount; i++)
        {
            extensions.push_back(glfwExtensions[i]);
        }
    }
    else if (m_UseSwapchain)
    {
        // When swapchain/WSI is enabled, GLFW is expected to provide platform WSI instance extensions.
        // A 0 count commonly means GLFW could not locate a Vulkan loader (e.g. vcpkg-installed loader
        // not on default search paths). On macOS, GLFW 3.4+ supports glfwInitVulkanLoader() which can
        // be used to point GLFW at a non-standard loader; GameEngine's Vulkan backend wires this up
        // at process startup when available.
        Logger::Log::Warning(
            "GLFW did not report required Vulkan instance extensions (count=0). "
            "Window surface creation may fail with VK_ERROR_EXTENSION_NOT_PRESENT. "
            "Ensure a Vulkan loader is available to GLFW and that platform WSI extensions are enabled.");
    }

    // Ensure VK_KHR_surface is enabled if device will enable VK_KHR_swapchain
    auto hasExt = [&](const char* name)
    {
        for (const char* e : extensions)
        {
            if (std::strcmp(e, name) == 0)
                return true;
        }
        return false;
    };
    if (m_UseSwapchain)
    {
        if (!hasExt(VK_KHR_SURFACE_EXTENSION_NAME))
        {
            extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        }
#if defined(_WIN32)
        // On Windows, also include the Win32 surface extension when available
        if (!hasExt("VK_KHR_win32_surface"))
        {
            extensions.push_back("VK_KHR_win32_surface");
        }
#endif
    }

#ifdef VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME
    m_SwapchainColorSpaceExtEnabled = false;
    if (m_UseSwapchain)
    {
        uint32_t instExtCount = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr) == VK_SUCCESS && instExtCount > 0)
        {
            std::vector<VkExtensionProperties> instExts(instExtCount);
            if (vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, instExts.data()) == VK_SUCCESS)
            {
                for (const auto& ep : instExts)
                {
                    if (std::strcmp(ep.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0)
                    {
                        if (!hasExt(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
                            extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
                        m_SwapchainColorSpaceExtEnabled = true;
                        break;
                    }
                }
            }
        }
    }
#endif

#if defined(PLATFORM_MACOS)
    // On macOS (MoltenVK), ensure we enable a platform-specific surface extension
    // in addition to VK_KHR_surface so glfwCreateWindowSurface can succeed.
    if (m_UseSwapchain)
    {
        uint32_t instExtCount = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr) == VK_SUCCESS && instExtCount > 0)
        {
            std::vector<VkExtensionProperties> instExts(instExtCount);
            if (vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, instExts.data()) == VK_SUCCESS)
            {
                bool supportedMetal = false;
                bool supportedMvkMac = false;
                for (const auto& ep : instExts)
                {
                    if (std::strcmp(ep.extensionName, VK_EXT_METAL_SURFACE_EXTENSION_NAME) == 0)
                        supportedMetal = true;
                    else if (std::strcmp(ep.extensionName, VK_MVK_MACOS_SURFACE_EXTENSION_NAME) == 0)
                        supportedMvkMac = true;
                }

                auto addIfAvailable = [&](const char* name)
                {
                    if (hasExt(name))
                        return;
                    for (const auto& ep : instExts)
                    {
                        if (std::strcmp(ep.extensionName, name) == 0)
                        {
                            extensions.push_back(name);
                            break;
                        }
                    }
                };

                // Prefer VK_EXT_metal_surface when present; also accept the legacy
                // VK_MVK_macos_surface for compatibility with older MoltenVK stacks.
                addIfAvailable(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
                addIfAvailable(VK_MVK_MACOS_SURFACE_EXTENSION_NAME);

                const bool enabledMetal = hasExt(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
                const bool enabledMvkMac = hasExt(VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
                const bool anySupported = supportedMetal || supportedMvkMac;
                const bool anyEnabled = enabledMetal || enabledMvkMac;

                if (!anySupported)
                {
                    Logger::Log::Warning(
                        "Vulkan loader on macOS does not expose any WSI surface extensions (VK_EXT_metal_surface, VK_MVK_macos_surface). "
                        "Window surface creation will likely fail with VK_ERROR_EXTENSION_NOT_PRESENT. Ensure MoltenVK is installed and visible to the Vulkan loader.");
                }
                else if (!anyEnabled)
                {
                    Logger::Log::Warning(
                        "Vulkan loader on macOS reports WSI surface extensions but none are enabled on the Vulkan instance. "
                        "Window surface creation may fail with VK_ERROR_EXTENSION_NOT_PRESENT.");
                }
            }
        }
    }
#endif

#if defined(PLATFORM_LINUX)
    // Some Vulkan SDK headers only expose these extension-name macros when a corresponding
    // VK_USE_PLATFORM_* macro is defined. Provide fallbacks so we can still query support
    // at runtime via vkEnumerateInstanceExtensionProperties without relying on those macros.
#ifndef VK_KHR_XCB_SURFACE_EXTENSION_NAME
#define VK_KHR_XCB_SURFACE_EXTENSION_NAME "VK_KHR_xcb_surface"
#endif
#ifndef VK_KHR_XLIB_SURFACE_EXTENSION_NAME
#define VK_KHR_XLIB_SURFACE_EXTENSION_NAME "VK_KHR_xlib_surface"
#endif
#ifndef VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
#define VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME "VK_KHR_wayland_surface"
#endif

    // On Linux, ensure we enable at least one platform-specific surface extension
    // in addition to VK_KHR_surface so glfwCreateWindowSurface can succeed.
    if (m_UseSwapchain)
    {
        uint32_t instExtCount = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr) == VK_SUCCESS && instExtCount > 0)
        {
            std::vector<VkExtensionProperties> instExts(instExtCount);
            if (vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, instExts.data()) == VK_SUCCESS)
            {
                bool supportedXcb = false;
                bool supportedXlib = false;
                bool supportedWayland = false;
                for (const auto& ep : instExts)
                {
                    if (std::strcmp(ep.extensionName, VK_KHR_XCB_SURFACE_EXTENSION_NAME) == 0)
                        supportedXcb = true;
                    else if (std::strcmp(ep.extensionName, VK_KHR_XLIB_SURFACE_EXTENSION_NAME) == 0)
                        supportedXlib = true;
                    else if (std::strcmp(ep.extensionName, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0)
                        supportedWayland = true;
                }

                auto addIfAvailable = [&](const char* name)
                {
                    if (hasExt(name))
                        return;
                    for (const auto& ep : instExts)
                    {
                        if (std::strcmp(ep.extensionName, name) == 0)
                        {
                            extensions.push_back(name);
                            break;
                        }
                    }
                };

                // Try XCB, then Xlib, then Wayland surfaces; whichever are supported
                addIfAvailable(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
                addIfAvailable(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
                addIfAvailable(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

                bool enabledXcb = hasExt(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
                bool enabledXlib = hasExt(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
                bool enabledWayland = hasExt(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
                bool anySupported = supportedXcb || supportedXlib || supportedWayland;
                bool anyEnabled = enabledXcb || enabledXlib || enabledWayland;

                if (!anySupported)
                {
                    Logger::Log::Warning(
                        "Vulkan loader on Linux does not expose any WSI surface extensions (VK_KHR_xcb_surface, VK_KHR_xlib_surface, VK_KHR_wayland_surface). "
                        "Window surface creation will likely fail with VK_ERROR_EXTENSION_NOT_PRESENT. Ensure your Vulkan loader was built with X11/Wayland WSI support (e.g., vcpkg vulkan-loader[wayland,xcb,xlib]).");
                }
                else if (!anyEnabled)
                {
                    Logger::Log::Warning(
                        "Vulkan loader on Linux reports WSI surface extensions but none are enabled on the Vulkan instance. "
                        "Window surface creation may fail with VK_ERROR_EXTENSION_NOT_PRESENT.");
                }
            }
        }
    }
#endif

    // Handle Vulkan Portability (e.g. MoltenVK on macOS). If the loader exposes
    // VK_KHR_portability_enumeration, we must both enable the extension and set
    // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR on the instance flags,
    // otherwise vkCreateInstance may return VK_ERROR_INCOMPATIBLE_DRIVER even
    // when the requested API version is supported.
    //
    // VK_EXT_debug_utils is probed in the same enumeration rather than assumed:
    // asking for an extension the loader does not advertise fails vkCreateInstance
    // outright with VK_ERROR_EXTENSION_NOT_PRESENT.
    {
        bool instExtsEnumerated = false;
        uint32_t instExtCount = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr) == VK_SUCCESS && instExtCount > 0)
        {
            std::vector<VkExtensionProperties> instExts(instExtCount);
            if (vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, instExts.data()) == VK_SUCCESS)
            {
                instExtsEnumerated = true;
                bool supportsPortabilityEnum = false;
                bool supportsDebugUtils = false;
                for (const auto& ep : instExts)
                {
                    if (std::strcmp(ep.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
                    {
                        supportsPortabilityEnum = true;
                    }
                    else if (std::strcmp(ep.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                    {
                        supportsDebugUtils = true;
                    }
                }

                if (supportsPortabilityEnum)
                {
                    if (!hasExt(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
                    {
                        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                    }
                    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
                }

                // Enabled in every config, not only under the validation layer:
                // labels and object names are what let a RenderDoc capture or a
                // driver crash report name the pass that faulted, and the perf
                // configs are exactly where those investigations happen.
                if (!supportsDebugUtils)
                {
                    if (m_DebugLayerEnabled)
                    {
                        Logger::Log::Warning(
                            "VulkanDevice: the Vulkan loader does not advertise " VK_EXT_DEBUG_UTILS_EXTENSION_NAME
                            "; the validation debug messenger cannot be created and no validation messages will be "
                            "reported. Install the Vulkan SDK / a loader that provides it");
                    }
                }
                // The escape hatch cannot drop the extension out from under a
                // debug-layer instance: the messenger created below is itself a
                // VK_EXT_debug_utils object, so honouring it there would silence
                // validation rather than the labels.
                else if (m_DebugLayerEnabled || !IsDebugUtilsDisabled())
                {
                    if (!hasExt(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                    {
                        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                    }
                    m_DebugUtilsExtEnabled = true;
                }
                else
                {
                    // Say so rather than going quiet: an instrument that leaves no
                    // trace when it is off cannot be told apart from one that is on
                    // and finding nothing.
                    Logger::Log::Info(
                        "VulkanDevice: " VK_EXT_DEBUG_UTILS_EXTENSION_NAME " dropped by GE_VK_NO_DEBUG_UTILS — "
                        "GPU debug labels and object names are off for devices on this instance");
                }
            }
        }
        if (!instExtsEnumerated)
        {
            // Not silent: on this path nothing above ran, VK_EXT_debug_utils stays
            // unrequested, and the messenger below is skipped via
            // m_DebugUtilsExtEnabled — the reader deserves to know why.
            Logger::Log::Warning(
                "VulkanDevice: vkEnumerateInstanceExtensionProperties failed; " VK_EXT_DEBUG_UTILS_EXTENSION_NAME
                " and portability stay unrequested — GPU debug labels are off and no validation debug messenger "
                "will be created");
        }
    }

    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    // Validation layers
    std::vector<const char*> enabledLayers;
    if (m_DebugLayerEnabled)
    {
        // Enumerate available instance layers and enable only those that are present.
        // This prevents vkCreateInstance from failing with VK_ERROR_LAYER_NOT_PRESENT
        // on systems where validation layers (e.g. VK_LAYER_KHRONOS_validation) are
        // not installed or not visible to the Vulkan loader (common on fresh macOS
        // installs without the full Vulkan SDK).
        uint32_t layerCount = 0;
        VkResult layerResult = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        if (layerResult != VK_SUCCESS)
        {
            Logger::Log::Warning(
                "vkEnumerateInstanceLayerProperties failed ({}); disabling validation layers",
                (int)layerResult);
        }
        else
        {
            std::vector<VkLayerProperties> availableLayers(layerCount);
            if (layerCount > 0)
            {
                layerResult = vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
                if (layerResult != VK_SUCCESS)
                {
                    Logger::Log::Warning(
                        "vkEnumerateInstanceLayerProperties (2) failed ({}); disabling validation layers",
                        (int)layerResult);
                    availableLayers.clear();
                }
            }

            auto hasLayer = [&availableLayers](const char* name)
            {
                for (const auto& lp : availableLayers)
                {
                    if (std::strcmp(lp.layerName, name) == 0)
                        return true;
                }
                return false;
            };

            for (const char* layerName : g_validationLayers)
            {
                if (hasLayer(layerName))
                {
                    enabledLayers.push_back(layerName);
                }
                else
                {
                    Logger::Log::Warning(
                        "Requested Vulkan validation layer '{}' is not available; it will be disabled",
                        layerName);
                }
            }
        }

        if (!enabledLayers.empty())
        {
            createInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
            createInfo.ppEnabledLayerNames = enabledLayers.data();
        }
        else
        {
            createInfo.enabledLayerCount = 0;
            // Ensure we also skip validation layers when creating the logical device.
            m_DebugLayerEnabled = false;
            Logger::Log::Warning(
                "No requested Vulkan validation layers are available; continuing without validation layers");
        }
    }
    else
    {
        createInfo.enabledLayerCount = 0;
    }

    // The settings object lives in this scope so its values outlive
    // vkCreateInstance, and is chained only when the layer advertises
    // VK_EXT_layer_settings.
    bool syncValidationChained = false;
    bool gpuAvChained = false;
#if defined(VK_EXT_layer_settings)
    ValidationLayerSettings layerSettings(g_validationLayers[0], validationDesc);
    VkLayerSettingsCreateInfoEXT layerSettingsInfo{};
    if (m_DebugLayerEnabled && !enabledLayers.empty() &&
        ValidationLayerAdvertisesLayerSettings(g_validationLayers[0]))
    {
        layerSettingsInfo.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
        layerSettingsInfo.settingCount = layerSettings.Count();
        layerSettingsInfo.pSettings = layerSettings.Data();
        layerSettingsInfo.pNext = createInfo.pNext;
        createInfo.pNext = &layerSettingsInfo;
        syncValidationChained = layerSettings.SyncValidationEnabled();
        gpuAvChained = layerSettings.GpuAssistedValidationEnabled();
        m_GpuAvChained = gpuAvChained;
        m_GpuAvShaderInstrumentationRequested = layerSettings.GpuAvShaderInstrumentationRequested();
    }
#endif

    VkResult result = vkCreateInstance(&createInfo, nullptr, &m_Instance);
    if (result != VK_SUCCESS)
    {
        Logger::Log::Error("Failed to create Vulkan instance: {}", (int)result);
        return false;
    }

    // State whether the instrument is live: a run that reports no sync hazards
    // means nothing unless the log says sync-val was actually validating. Once per
    // instance — a device that shares one returns before here.
    if (syncValidationChained)
    {
        Logger::Log::Info(
            "VulkanDevice: Vulkan synchronization validation ENABLED (GE_VK_SYNC_VALIDATION) — "
            "diagnostic mode; expect a large frame-time cost and do not use it for measurement");
    }
    else if (validationDesc.SyncValidation)
    {
        Logger::Log::Warning(
            "VulkanDevice: GE_VK_SYNC_VALIDATION is set but synchronization validation is INACTIVE — no "
            "sync hazards will be reported. It needs a debug-layer device, VK_LAYER_KHRONOS_validation "
            "installed, and a Vulkan SDK new enough to advertise VK_EXT_layer_settings (1.3.268+)");
    }

    // Same rule as sync-val: a run that reports no GPU-AV errors means nothing
    // unless the log says GPU-AV was actually validating. WHICH HALF was
    // validating cannot be settled here — the shader-instrumentation half depends
    // on VK_EXT_descriptor_buffer, which device creation decides — so that line is
    // emitted after the device exists and this one states only the request.
    if (gpuAvChained)
    {
        Logger::Log::Info(
            "VulkanDevice: GPU-assisted validation ENABLED (GE_VK_GPU_AV) — buffer-content checks "
            "(indirect draw/dispatch arguments, index buffers, buffer copies) are armed. Shader "
            "instrumentation was {}; its resolved state is logged after device creation",
            m_GpuAvShaderInstrumentationRequested ? "REQUESTED" : "not requested");
        Logger::Log::Warning(
            "VulkanDevice: GPU-AV inserts a validation dispatch ahead of each indirect draw/dispatch and "
            "does not save or restore descriptor-buffer bindings, so it perturbs the very state under "
            "test — corroborate anything it reports against a GE_VK_GPU_AV=0 run before acting on it");
    }
    else if (validationDesc.GpuAssistedValidation)
    {
        Logger::Log::Warning(
            "VulkanDevice: GE_VK_GPU_AV is set but GPU-assisted validation is INACTIVE — no GPU-AV errors "
            "will be reported. It needs the validation layer actually loaded, which DebugFast does NOT do by "
            "default: set GE_VK_VALIDATION=1 as well. It also needs VK_LAYER_KHRONOS_validation installed and "
            "a Vulkan SDK new enough to advertise VK_EXT_layer_settings (1.3.268+)");
    }

    // Setup debug messenger. Gated on the extension actually being enabled, not
    // just the layer request: on an enumeration failure or a loader without
    // VK_EXT_debug_utils, creating the messenger would call into an extension
    // this instance never enabled.
    if (m_DebugLayerEnabled && m_DebugUtilsExtEnabled)
    {
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        // VERBOSE + INFO fire many times per Vulkan call (string format + mutex in
        // the callback). Debug keeps all four severities; DebugFast narrows to
        // WARNING+ERROR. GE_VK_VALIDATION_VERBOSE forces all four regardless.
        VkDebugUtilsMessageSeverityFlagsEXT severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
#if !defined(GE_DEBUGFAST) && (defined(_DEBUG) || defined(DEBUG))
        severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
#endif
        if (verboseMessenger)
        {
            severity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        }
        debugCreateInfo.messageSeverity = severity;
        debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

        debugCreateInfo.pfnUserCallback = DebugCallback;
        debugCreateInfo.pUserData = nullptr; // callback records into ValidationStatsStore::Get()

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_Instance, "vkCreateDebugUtilsMessengerEXT");
        if (func != nullptr)
        {
            func(m_Instance, &debugCreateInfo, nullptr, &m_DebugMessenger);
        }
    }

    // Register for sharing, with this device's own reference counted. Last, so the
    // entry a later device adopts is complete — the messenger above is part of the
    // instance, not of the device that happened to create it. Unconditional by
    // necessity: every device releases one reference at teardown, so a publish the
    // creating device skipped would leave that release with no entry to find and
    // the instance would never be destroyed.
    SharedInstanceState state{};
    state.Instance = m_Instance;
    state.DebugMessenger = m_DebugMessenger;
    state.DebugLayerActive = m_DebugLayerEnabled;
    state.DebugUtilsExtEnabled = m_DebugUtilsExtEnabled;
    state.InstanceApiVersion = m_InstanceApiVersion;
    state.SwapchainColorSpaceExtEnabled = m_SwapchainColorSpaceExtEnabled;
    state.GpuAvChained = m_GpuAvChained;
    state.GpuAvShaderInstrumentationRequested = m_GpuAvShaderInstrumentationRequested;
    SharedInstanceRegistry::Get().Publish(instanceKey, state);

    return true;
}

bool VulkanDevice::SelectPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);

    if (deviceCount == 0)
    {
        Logger::Log::Error("No Vulkan-capable devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

    // Select the first discrete GPU, or fallback to any device
    VkPhysicalDevice selectedDevice = VK_NULL_HANDLE;
    for (const auto& device : devices)
    {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);

        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            selectedDevice = device;
            break;
        }
    }

    if (selectedDevice == VK_NULL_HANDLE)
    {
        selectedDevice = devices[0]; // Fallback to first device
    }

    m_PhysicalDevice = selectedDevice;
    vkGetPhysicalDeviceProperties(m_PhysicalDevice, &m_DeviceProperties);
    vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &m_DeviceFeatures);
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &m_MemoryProperties);

    // Derived here, beside its only inputs, rather than in QueryDeviceCapabilities:
    // that function is re-run by RebuildDevice and zeroes m_Capabilities first, and
    // CreateBuffer reads the topology from job threads without the rebuild lock. A
    // rebuild keeps this physical device, so the value cannot change and must not
    // be seen absent. No extra Vulkan call, no extension.
    m_MemoryTopology = DeriveMemoryTopology(m_MemoryProperties, m_DeviceProperties.deviceType);

    // Find queue families
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

    // Sole resolution point for all four family indices — CreateLogicalDevice, the
    // command pools and the CONCURRENT sharing lists all read the members set here.
    // RebuildDevice keeps them: it re-runs on the surviving physical device, whose
    // families cannot have changed.
    static_assert(QueueFamilySelection::kInvalidQueueFamily == kInvalidQueueFamilyIndex,
                  "Queue-family sentinels must agree");
    const QueueFamilySelection selection = SelectQueueFamilies(queueFamilies);
    m_GraphicsQueueFamily = selection.Graphics;
    m_PresentQueueFamily = selection.Graphics; // Presentation is assumed to follow graphics.
    m_ComputeQueueFamily = selection.Compute;
    m_TransferQueueFamily = selection.Transfer;

    Logger::Log::Info("Vulkan queue families: {}, present={}",
                      DescribeQueueFamilySelection(selection, queueFamilies), m_PresentQueueFamily);

    return m_GraphicsQueueFamily != kInvalidQueueFamilyIndex;
}

bool VulkanDevice::CreateLogicalDevice()
{
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {m_GraphicsQueueFamily, m_PresentQueueFamily};

    // Compute and transfer families were resolved in SelectPhysicalDevice; a dedicated
    // family that exists gets its own VkDeviceQueueCreateInfo here.
    if (m_ComputeQueueFamily != kInvalidQueueFamilyIndex)
        uniqueQueueFamilies.insert(m_ComputeQueueFamily);
    if (m_TransferQueueFamily != kInvalidQueueFamilyIndex)
        uniqueQueueFamilies.insert(m_TransferQueueFamily);

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies)
    {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    // Query supported features and enable only what we need
    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supportedFeatures);

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = supportedFeatures.samplerAnisotropy;
    // Enable depth-bounds test feature if supported (allows vkCmdSetDepthBounds and pipeline depthBoundsTestEnable)
    deviceFeatures.depthBounds = supportedFeatures.depthBounds;

    deviceFeatures.pipelineStatisticsQuery = supportedFeatures.pipelineStatisticsQuery; // needed for pipeline stats query pool
    deviceFeatures.occlusionQueryPrecise = supportedFeatures.occlusionQueryPrecise;     // precise occlusion if available
    // R8 (and other non-mandatory) storage-image formats — the RT shadow-mask
    // pass writes its full-res mask as an r8 storage image. Guarded by
    // support; the mask lane itself is additionally gated on ray query, and
    // every ray-query desktop device also reports extended storage formats.
    deviceFeatures.shaderStorageImageExtendedFormats = supportedFeatures.shaderStorageImageExtendedFormats;
    // Keep multi-draw available where supported; counted-indirect fallback can
    // still operate without it by issuing one indirect draw per command record.
    deviceFeatures.multiDrawIndirect = supportedFeatures.multiDrawIndirect;
    // GE_INSTANCED shaders declare uint64_t push-constant addresses (see
    // instance_io.glsl InstancedPC). The shader SPIR-V therefore carries the
    // Int64 capability, which requires VkPhysicalDeviceFeatures::shaderInt64 —
    // otherwise VUID-VkShaderModuleCreateInfo-pCode-08740 fires on every
    // shader load. (BDA itself uses PhysicalStorageBuffer64, a separate
    // capability; we enable Int64 purely because our shaders use the uint64_t
    // scalar type.)
    deviceFeatures.shaderInt64 = supportedFeatures.shaderInt64;
    // BC (DXT/BPTC) sampled-texture support. Universal on desktop GPUs;
    // MoltenVK reports it per Metal GPU family. Loaders check the capability
    // flag and fall back to uncompressed RGBA when absent.
    deviceFeatures.textureCompressionBC = supportedFeatures.textureCompressionBC;
    // Dual-source blending (near-universal on desktop). Enabled whenever
    // supported so capability reporting can assume support == enabled; gates
    // pipelines whose blend factors reference SRC1 (subpixel RGB text AA).
    deviceFeatures.dualSrcBlend = supportedFeatures.dualSrcBlend;
    // Depth clamp (core VK 1.0, universal on desktop). Backs
    // RasterizationState::depthClampEnable, which the directional shadow cascades
    // use for pancaking: a caster nearer the light than the cascade's near plane
    // rasterizes clamped instead of being clipped, so the near plane bounds the
    // depth encode without deciding which occluders exist.
    deviceFeatures.depthClamp = supportedFeatures.depthClamp;
    // Indirect records select their instance range through firstInstance: the GPU draw stream
    // (draw_command_scatter.comp) and the grass LOD pool both write it, and without this
    // feature any non-zero firstInstance is VUID-VkDrawIndexedIndirectCommand-firstInstance-00554.
    deviceFeatures.drawIndirectFirstInstance = supportedFeatures.drawIndirectFirstInstance;
    // The parallax colour variants take their relief footprint through interpolateAtOffset, whose
    // SPIR-V InterpolationFunction capability requires sampleRateShading (otherwise
    // VUID-VkShaderModuleCreateInfo-pCode-08740 on every such module). Enabling the feature turns no
    // pipeline to per-sample shading; that stays each pipeline's sampleShadingEnable. Enabled
    // whenever supported, so capability reporting can assume support == enabled.
    deviceFeatures.sampleRateShading = supportedFeatures.sampleRateShading;

    // Query Vulkan 1.3 feature support (synchronization2)
    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    // Query descriptor indexing features (bindless)
    VkPhysicalDeviceDescriptorIndexingFeatures indexingSupported{};
    indexingSupported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &supported13;
    supported13.pNext = &indexingSupported;
    vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &features2);

    // Query Vulkan 1.2 feature support (timelineSemaphore, hostQueryReset, etc.)
    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 tmpQry{};
    tmpQry.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    tmpQry.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &tmpQry);

    // Enable required Vulkan 1.2/1.3 features (guarded by support)
    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled12.timelineSemaphore = supported12.timelineSemaphore;
    enabled12.hostQueryReset = supported12.hostQueryReset; // Promote hostQueryReset via v1.2 struct
    enabled12.bufferDeviceAddress = supported12.bufferDeviceAddress;
    m_HostQueryResetEnabledAtInit = (supported12.hostQueryReset == VK_TRUE);
    // Stash for QueryDeviceCapabilities, which zeroes m_Capabilities and rehydrates
    // selectively. Without the stash the cap would be lost, breaking the VMA
    // BUFFER_DEVICE_ADDRESS gate in InitializeVMA (fires after QueryDeviceCapabilities).
    m_BufferDeviceAddressEnabledAtInit = (supported12.bufferDeviceAddress == VK_TRUE);
    // Descriptor indexing features are promoted in Vulkan 1.2; enable via VkPhysicalDeviceVulkan12Features
    enabled12.descriptorIndexing = supported12.descriptorIndexing;
    enabled12.runtimeDescriptorArray = supported12.runtimeDescriptorArray;
    enabled12.descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound;
    enabled12.descriptorBindingVariableDescriptorCount = supported12.descriptorBindingVariableDescriptorCount;
    enabled12.shaderSampledImageArrayNonUniformIndexing = supported12.shaderSampledImageArrayNonUniformIndexing;
    enabled12.shaderStorageBufferArrayNonUniformIndexing = supported12.shaderStorageBufferArrayNonUniformIndexing;
    enabled12.shaderUniformBufferArrayNonUniformIndexing = supported12.shaderUniformBufferArrayNonUniformIndexing;
    enabled12.shaderStorageImageArrayNonUniformIndexing = supported12.shaderStorageImageArrayNonUniformIndexing;
    enabled12.descriptorBindingSampledImageUpdateAfterBind = supported12.descriptorBindingSampledImageUpdateAfterBind;
    m_DescriptorIndexingEnabledAtInit =
        enabled12.descriptorIndexing == VK_TRUE &&
        enabled12.runtimeDescriptorArray == VK_TRUE &&
        enabled12.descriptorBindingPartiallyBound == VK_TRUE &&
        enabled12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
        enabled12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    // GPU-driven indirect rendering: vkCmdDrawIndexedIndirectCount requires this feature
    // (VUID-vkCmdDrawIndexedIndirectCount-None-04445). Promoted into v1.2 core from
    // VK_KHR_draw_indirect_count; some MoltenVK builds expose it only with local patches.
    enabled12.drawIndirectCount = supported12.drawIndirectCount;
    m_SupportsDrawIndirectCount = (enabled12.drawIndirectCount == VK_TRUE);
    m_SupportsMultiDrawIndirect = (deviceFeatures.multiDrawIndirect == VK_TRUE);
    Logger::Log::Info("VulkanDevice: indirect features supported: drawIndirectCount={}, multiDrawIndirect={}, "
                      "drawIndirectFirstInstance={}; enabled: drawIndirectCount={}, multiDrawIndirect={}, "
                      "drawIndirectFirstInstance={}",
                      supported12.drawIndirectCount == VK_TRUE,
                      supportedFeatures.multiDrawIndirect == VK_TRUE,
                      supportedFeatures.drawIndirectFirstInstance == VK_TRUE,
                      enabled12.drawIndirectCount == VK_TRUE,
                      deviceFeatures.multiDrawIndirect == VK_TRUE,
                      deviceFeatures.drawIndirectFirstInstance == VK_TRUE);
    // Both resolve once from the environment; print them so a run's mode is read
    // from the log rather than assumed from the config.
    Logger::Log::Info("VulkanDevice: texture usage policy: audit={}, strict={} (images get {})",
                      TextureUsagePolicy::IsAuditEnabled() ? "ON" : "OFF",
                      TextureUsagePolicy::IsStrict() ? "ON" : "OFF",
                      TextureUsagePolicy::IsStrict() ? "exactly their declared usage"
                                                     : "both transfer bits regardless");
    VkPhysicalDeviceVulkan13Features enabled13{};
    enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    // Chain mesh shader features only when extension is supported
    VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{};
    if (CheckExtensionSupport(VK_EXT_MESH_SHADER_EXTENSION_NAME))
    {
        meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        meshFeatures.meshShader = VK_TRUE;
        meshFeatures.taskShader = VK_FALSE;
        enabled13.pNext = &meshFeatures;
    }

    enabled13.synchronization2 = supported13.synchronization2;
    enabled13.dynamicRendering = supported13.dynamicRendering;
    // We compile shaders to SPIR-V 1.6, where glslang lowers `discard` to
    // OpDemoteToHelperInvocation (not OpKill) — that capability requires this feature, so any
    // shader with a discard (alpha test, transmission occlusion) fails vkCreateShaderModule
    // without it. Promoted to core in 1.3; enable when the device reports support.
    enabled13.shaderDemoteToHelperInvocation = supported13.shaderDemoteToHelperInvocation;

    // If the device reports < 1.3, enable KHR feature structs explicitly when extensions are present
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynRenderingKHR{};
    dynRenderingKHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    dynRenderingKHR.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2KHR{};
    sync2KHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
    sync2KHR.synchronization2 = VK_TRUE;
    // We compute wantSync2 below; defer installing KHR feature nodes until then

    // Track dynamic rendering support: either Vulkan 1.3 feature or KHR extension presence
    bool hasDynRenderingKHR = false;
    {
        uint32_t extCountDyn = 0;
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCountDyn, nullptr);
        std::vector<VkExtensionProperties> extPropsDyn(extCountDyn);
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCountDyn, extPropsDyn.data());
        for (const auto& ep : extPropsDyn)
        {
            if (strcmp(ep.extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0)
            {
                hasDynRenderingKHR = true;
                break;
            }
        }
    }
    m_SupportsDynamicRendering = (enabled13.dynamicRendering == VK_TRUE) || hasDynRenderingKHR;

    // Build pNext chain: vulkan12 -> vulkan13
    // Add sync2 extension if needed for Vulkan 1.2 devices
    bool wantSync2 = false;
    if (m_DeviceProperties.apiVersion < VK_API_VERSION_1_3)
    {
        // Query device extension support for sync2
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> extProps(extCount);
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, extProps.data());
        for (const auto& ep : extProps)
        {
            if (std::strcmp(ep.extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0)
            {
                wantSync2 = true;
                break;
            }
        }
    }
    // Now that wantSync2 is known, install KHR nodes for pre-1.3 devices
    if (m_DeviceProperties.apiVersion < VK_API_VERSION_1_3)
    {
        dynRenderingKHR.pNext = enabled13.pNext;
        enabled13.pNext = &dynRenderingKHR;
        if (wantSync2)
        {
            sync2KHR.pNext = enabled13.pNext;
            enabled13.pNext = &sync2KHR;
        }
    }
    // On MoltenVK / portability subset devices, enable mutableComparisonSamplers so
    // shadow-map comparison samplers can be written to descriptor sets.
    // The struct lives in vulkan_beta.h behind VK_ENABLE_BETA_EXTENSIONS; define
    // the sType constant and struct layout inline to avoid pulling in all beta APIs.
#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR static_cast<VkStructureType>(1000163000)
    struct VkPhysicalDevicePortabilitySubsetFeaturesKHR
    {
        VkStructureType sType;
        void*           pNext;
        VkBool32        constantAlphaColorBlendFactors;
        VkBool32        events;
        VkBool32        imageViewFormatReinterpretation;
        VkBool32        imageViewFormatSwizzle;
        VkBool32        imageView2DOn3DImage;
        VkBool32        multisampleArrayImage;
        VkBool32        mutableComparisonSamplers;
        VkBool32        pointPolygons;
        VkBool32        samplerMipLodBias;
        VkBool32        separateStencilMaskRef;
        VkBool32        shaderSampleRateInterpolationFunctions;
        VkBool32        tessellationIsolines;
        VkBool32        tessellationPointMode;
        VkBool32        triangleFans;
        VkBool32        vertexAttributeAccessBeyondStride;
    };
#define GE_LOCAL_PORTABILITY_SUBSET_DEFINED 1
#endif
    VkPhysicalDevicePortabilitySubsetFeaturesKHR portabilityFeatures{};
    portabilityFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
    if (CheckExtensionSupport("VK_KHR_portability_subset"))
    {
        // Query which portability features the device actually supports
        VkPhysicalDevicePortabilitySubsetFeaturesKHR supportedPortability{};
        supportedPortability.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
        VkPhysicalDeviceFeatures2 portabilityQuery{};
        portabilityQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        portabilityQuery.pNext = &supportedPortability;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &portabilityQuery);

        portabilityFeatures.mutableComparisonSamplers = supportedPortability.mutableComparisonSamplers;
        portabilityFeatures.pNext = enabled13.pNext;
        enabled13.pNext = &portabilityFeatures;
    }
#ifdef GE_LOCAL_PORTABILITY_SUBSET_DEFINED
#undef GE_LOCAL_PORTABILITY_SUBSET_DEFINED
#undef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR
#endif

    // GE_VK_CAPTURE_COMPAT=1: skip the descriptor-buffer and acceleration-
    // structure device features for this session. RenderDoc marks ALL device
    // memory as buffer-device-address when either feature is enabled, and a
    // BDA-everything capture only replays if every allocation lands at its
    // captured VA — which routinely fails (VK_ERROR_INVALID_OPAQUE_CAPTURE_
    // ADDRESS on open). Both features degrade gracefully by design (legacy
    // descriptor-pool path, RT-dependent features report unavailable), so a
    // capture session trades them for a replayable .rdc. Same bisection-env
    // idiom as GE_VK_USE_DESCRIPTOR_BUFFER below.
    //
    // Diagnostic-only: HZB occlusion culling has been observed reporting zero
    // culled draws under this env. The cause is unidentified — every
    // descriptor-buffer branch keys on the runtime gate this env shares with
    // GE_VK_USE_DESCRIPTOR_BUFFER=0, and no HZB path reads a descriptor-buffer
    // capability, so do not assume that mechanism when root-causing it.
    const bool captureCompatDevice = IsCaptureCompatRequested();
    if (captureCompatDevice)
        Logger::Log::Warning("VulkanDevice: GE_VK_CAPTURE_COMPAT — descriptor-buffer + "
                             "acceleration-structure features disabled for replayable captures. "
                             "HZB occlusion culling has been observed inert (0 culled) under this "
                             "env; cause unidentified. Do NOT use compat captures for occlusion "
                             "or perf analysis.");

    // VK_EXT_descriptor_buffer: query support, chain feature struct, record in capabilities.
    // The feature is consumed by the descriptor-buffer migration.
    // Default OFF at this point — actual runtime enablement (and extension-name enqueue below)
    // happens only when both the extension is available AND the feature bit is VK_TRUE.
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descBufferFeatures{};
    descBufferFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    const bool descBufferExtAvailable = CheckExtensionSupport(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    const bool descBufferRequested =
        (m_RequestedDescriptorBuffers != DescriptorBufferMode::Disabled) && !captureCompatDevice;
    if (descBufferExtAvailable && descBufferRequested)
    {
        VkPhysicalDeviceDescriptorBufferFeaturesEXT supportedDescBuffer{};
        supportedDescBuffer.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
        VkPhysicalDeviceFeatures2 dbQry{};
        dbQry.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        dbQry.pNext = &supportedDescBuffer;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &dbQry);
        if (supportedDescBuffer.descriptorBuffer == VK_TRUE)
        {
            // Enable only the core feature bit; capture-replay / layout-ignored / push are
            // opt-in by device and can be toggled on in later phases if needed.
            descBufferFeatures.descriptorBuffer = VK_TRUE;
            descBufferFeatures.pNext = enabled13.pNext;
            enabled13.pNext = &descBufferFeatures;
            m_Capabilities.supportsDescriptorBuffer = true;
            m_DescriptorBufferEnabledAtInit = true;
        }
    }

    // VK_KHR_acceleration_structure + VK_KHR_ray_query: enable when the device
    // supports the full set (RT shadow-mask lane, DirectionalShadowMode::RayTraced).
    // VK_KHR_deferred_host_operations is a hard extension dependency of
    // acceleration_structure even for GPU-timeline-only builds, and AS builds
    // consume buffer device addresses, so bufferDeviceAddress is a
    // prerequisite. Core feature bits only — capture-replay, host commands and
    // indirect builds stay off. When anything is missing the capability bit
    // stays false and RT-dependent features are silently unavailable.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{};
    accelFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
    rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    const bool rayQueryExtsAvailable =
        CheckExtensionSupport(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
        CheckExtensionSupport(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
        CheckExtensionSupport(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    if (rayQueryExtsAvailable && supported12.bufferDeviceAddress == VK_TRUE &&
        !captureCompatDevice)
    {
        VkPhysicalDeviceAccelerationStructureFeaturesKHR supportedAccel{};
        supportedAccel.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        VkPhysicalDeviceRayQueryFeaturesKHR supportedRayQuery{};
        supportedRayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        supportedAccel.pNext = &supportedRayQuery;
        VkPhysicalDeviceFeatures2 rtQry{};
        rtQry.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        rtQry.pNext = &supportedAccel;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &rtQry);
        if (supportedAccel.accelerationStructure == VK_TRUE &&
            supportedRayQuery.rayQuery == VK_TRUE)
        {
            accelFeatures.accelerationStructure = VK_TRUE;
            rayQueryFeatures.rayQuery = VK_TRUE;
            rayQueryFeatures.pNext = enabled13.pNext;
            enabled13.pNext = &rayQueryFeatures;
            accelFeatures.pNext = enabled13.pNext;
            enabled13.pNext = &accelFeatures;
            m_RayQueryEnabledAtInit = true;
        }
    }
    // VK_EXT_device_fault: post-mortem fault records after VK_ERROR_DEVICE_LOST.
    // Default-on in the developer configs and opt-in in Release (GE_VK_DEVICE_FAULT)
    // — retrieval costs nothing until a loss happens, and a loss is exactly when it
    // is too late to go and enable it, but a shipped build should not silently take
    // on an extension nothing in it reads. Orthogonal to descriptor buffers and to
    // checkpoint stage granularity, so it stays the one instrument that survives
    // both. Only the deviceFault feature is asked for; deviceFaultVendorBinary stays
    // zero-initialised, because that blob needs a vendor offline tool to read while
    // the address and vendor-info records are readable in the log.
    const bool deviceFaultRequested =
        ParseDeviceFaultEnabled(std::getenv("GE_VK_DEVICE_FAULT"), kDeviceFaultDefaultEnabled);
    VkPhysicalDeviceFaultFeaturesEXT faultFeatures{};
    faultFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
    m_DeviceFaultEnabledAtInit = false;
    if (deviceFaultRequested && CheckExtensionSupport(VK_EXT_DEVICE_FAULT_EXTENSION_NAME))
    {
        VkPhysicalDeviceFaultFeaturesEXT supportedFault{};
        supportedFault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
        VkPhysicalDeviceFeatures2 faultQry{};
        faultQry.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        faultQry.pNext = &supportedFault;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &faultQry);
        if (supportedFault.deviceFault == VK_TRUE)
        {
            faultFeatures.deviceFault = VK_TRUE;
            faultFeatures.pNext = enabled13.pNext;
            enabled13.pNext = &faultFeatures;
            m_DeviceFaultEnabledAtInit = true;
        }
    }

    Logger::Log::Info("VulkanDevice: ray query {} (extensions {}, bufferDeviceAddress {})",
                      m_RayQueryEnabledAtInit ? "enabled" : "unavailable",
                      rayQueryExtsAvailable ? "present" : "missing",
                      supported12.bufferDeviceAddress == VK_TRUE ? "supported" : "unsupported");

    enabled12.pNext = &enabled13;

    VkPhysicalDeviceFeatures2 enabledFeatures2{};
    enabledFeatures2.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabledFeatures2.features = deviceFeatures;
    enabledFeatures2.pNext    = &enabled12;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &enabledFeatures2; // Chain required features
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = nullptr;
    // Enable device extensions. VK_KHR_swapchain requires VK_KHR_surface at the instance level;
    // we ensured that in CreateInstance even for offscreen contexts.
    // Cache sync2 support
    m_SupportsSync2 = (m_DeviceProperties.apiVersion >= VK_API_VERSION_1_3) || wantSync2;
    std::vector<const char*> deviceExtensions;
    // Guard core/common extensions by availability
    if (m_UseSwapchain && CheckExtensionSupport(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
    {
        deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
#ifdef VK_EXT_HDR_METADATA_EXTENSION_NAME
    m_HdrMetadataExtEnabled = false;
    if (m_UseSwapchain && CheckExtensionSupport(VK_EXT_HDR_METADATA_EXTENSION_NAME))
    {
        deviceExtensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
        m_HdrMetadataExtEnabled = true;
    }
#endif
    if (CheckExtensionSupport(VK_KHR_MAINTENANCE1_EXTENSION_NAME))
    {
        deviceExtensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
    }
    // On Vulkan Portability implementations (e.g. MoltenVK on macOS), the
    // VK_KHR_portability_subset device extension must be enabled whenever it
    // is reported. Guard it behind CheckExtensionSupport so we only request
    // it when available.
    if (CheckExtensionSupport("VK_KHR_portability_subset"))
    {
        deviceExtensions.push_back("VK_KHR_portability_subset");
        m_IsPortabilitySubsetDevice = true;
    }
    // Dynamic rendering KHR only needed on < 1.3; add only if supported
    if (m_DeviceProperties.apiVersion < VK_API_VERSION_1_3 && CheckExtensionSupport(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
    {
        deviceExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    }
    // Optionally enable separate depth/stencil layouts if supported (safer read-only transitions)
    if (CheckExtensionSupport(VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME))
    {
        deviceExtensions.push_back(VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
    }
    if (m_Capabilities.supportsMeshShaders)
    {
        deviceExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    }
    if (m_Capabilities.supportsDescriptorBuffer)
    {
        deviceExtensions.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        // What the validation layer keys its GPU-AV shader-instrumentation gate
        // on is the extension being ENABLED, not this engine using it. Tracked
        // separately from m_DescriptorBufferEnabledAtInit, which later drops to
        // false when the engine declines the path (null entry points below) while
        // the device still has the extension on.
        m_DescriptorBufferExtensionEnabledOnDevice = true;
    }
    if (m_RayQueryEnabledAtInit)
    {
        deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }
    // GPU execution breadcrumbs. Enabled whenever present, even though emission
    // is opt-in: enabling the extension costs nothing until a checkpoint is
    // actually recorded, and having it enabled is what lets a device-lost
    // investigation be armed with an env var instead of a rebuild. Vendor-gated
    // in practice (NVIDIA); absent support leaves emission and retrieval inert.
    m_DeviceDiagnosticCheckpointsEnabledAtInit = false;
    if (CheckExtensionSupport(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME))
    {
        deviceExtensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        m_DeviceDiagnosticCheckpointsEnabledAtInit = true;
    }
    // Paired with the deviceFault feature chained above: the flag is only set when
    // both the extension and the feature bit are present.
    if (m_DeviceFaultEnabledAtInit)
    {
        deviceExtensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    }
    // If descriptor indexing is available and running on < 1.2, enable the extension for bindless
    if (m_DeviceProperties.apiVersion < VK_API_VERSION_1_2 && CheckExtensionSupport(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
    {
        deviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    }
    // If running on Vulkan 1.2 but sync2 is present, enable the extension for best compatibility
    if (m_DeviceProperties.apiVersion < VK_API_VERSION_1_3 && wantSync2)
    {
        deviceExtensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    }
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    // Chain feature structs (Vulkan 1.2/1.3). Descriptor indexing is enabled via enabled12.* fields.
    enabled12.pNext = &enabled13;
    enabledFeatures2.pNext = &enabled12;
    createInfo.pNext = &enabledFeatures2;

    if (m_DebugLayerEnabled)
    {
        // If VK_KHR_synchronization2 is available on 1.2, enable support flag; extension is optional for core usage
        m_SupportsSync2 = (m_DeviceProperties.apiVersion >= VK_API_VERSION_1_3) || wantSync2;
    }
    // Validation layers are instance-level in modern Vulkan. Passing them to
    // vkCreateDevice is deprecated (device layers "have never worked since Vulkan
    // 1.0") and triggers VUID-VkDeviceCreateInfo-enabledLayerCount-12384. Leave
    // device layers empty regardless of debug state — instance-level validation
    // (set at vkCreateInstance) still applies. Device-level function pointers are
    // resolved after vkCreateDevice.
    createInfo.enabledLayerCount = 0;
    createInfo.ppEnabledLayerNames = nullptr;

    VkResult result = vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device);
    if (result != VK_SUCCESS)
    {
        Logger::Log::Error("Failed to create logical device: {} ({})", VkResultToString(result), (int)result);
        Logger::Log::Error("API version: {}.{}.{}",
                           (int)VK_VERSION_MAJOR(m_DeviceProperties.apiVersion),
                           (int)VK_VERSION_MINOR(m_DeviceProperties.apiVersion),
                           (int)VK_VERSION_PATCH(m_DeviceProperties.apiVersion));
        Logger::Log::Error("Requested device extensions ({}):", deviceExtensions.size());
        for (size_t i = 0; i < deviceExtensions.size(); ++i)
        {
            Logger::Log::Error("  {}", deviceExtensions[i]);
        }
        // Fallback: retry with a minimal feature/ext set to tolerate picky drivers/VMs.
        // Keep swapchain enabled when rendering to a window; otherwise the device
        // can be created but presentation entry points are unavailable.
        Logger::Log::Warning("Retrying vkCreateDevice with minimal device features and required presentation extensions...");
        std::vector<const char*> fallbackExtensions;
        if (m_UseSwapchain && CheckExtensionSupport(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        {
            fallbackExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }
        VkPhysicalDeviceFeatures minimalFeatures{};
        minimalFeatures.samplerAnisotropy = supportedFeatures.samplerAnisotropy;
        // Keep Int64 enabled in the fallback path too — buffer_reference shaders
        // on the instanced path declare it regardless of extension availability.
        minimalFeatures.shaderInt64 = supportedFeatures.shaderInt64;
        // Keep BC textures enabled in the fallback path too — capability
        // reporting assumes support == enabled (see caps fill).
        minimalFeatures.textureCompressionBC = supportedFeatures.textureCompressionBC;
        // Same contract for dual-source blending.
        minimalFeatures.dualSrcBlend = supportedFeatures.dualSrcBlend;
        // Keep depth clamp in the fallback path too — the cascade depth PSOs are
        // built with depthClampEnable regardless of which device path created the
        // device, and a PSO requesting it without the feature is invalid usage.
        minimalFeatures.depthClamp = supportedFeatures.depthClamp;
        minimalFeatures.drawIndirectFirstInstance = supportedFeatures.drawIndirectFirstInstance;
        // Same support == enabled contract for sampleRateShading (the parallax footprint).
        minimalFeatures.sampleRateShading = supportedFeatures.sampleRateShading;
        VkPhysicalDeviceVulkan12Features fallback12{};
        fallback12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        fallback12.timelineSemaphore = supported12.timelineSemaphore;
        fallback12.hostQueryReset = supported12.hostQueryReset;
        fallback12.bufferDeviceAddress = supported12.bufferDeviceAddress;
        fallback12.descriptorIndexing = supported12.descriptorIndexing;
        fallback12.runtimeDescriptorArray = supported12.runtimeDescriptorArray;
        fallback12.descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound;
        fallback12.descriptorBindingVariableDescriptorCount = supported12.descriptorBindingVariableDescriptorCount;
        fallback12.shaderSampledImageArrayNonUniformIndexing = supported12.shaderSampledImageArrayNonUniformIndexing;
        fallback12.shaderStorageBufferArrayNonUniformIndexing = supported12.shaderStorageBufferArrayNonUniformIndexing;
        fallback12.shaderUniformBufferArrayNonUniformIndexing = supported12.shaderUniformBufferArrayNonUniformIndexing;
        fallback12.shaderStorageImageArrayNonUniformIndexing = supported12.shaderStorageImageArrayNonUniformIndexing;
        fallback12.descriptorBindingSampledImageUpdateAfterBind = supported12.descriptorBindingSampledImageUpdateAfterBind;
        fallback12.drawIndirectCount = supported12.drawIndirectCount;
        VkPhysicalDeviceFeatures2 fallbackFeatures2{};
        fallbackFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        fallbackFeatures2.features = minimalFeatures;
        fallbackFeatures2.pNext = &fallback12;
        VkDeviceCreateInfo fallbackCreateInfo = createInfo;
        fallbackCreateInfo.pNext = &fallbackFeatures2; // no optional extension feature structs
        fallbackCreateInfo.pEnabledFeatures = nullptr;
        fallbackCreateInfo.enabledExtensionCount = static_cast<uint32_t>(fallbackExtensions.size());
        fallbackCreateInfo.ppEnabledExtensionNames = fallbackExtensions.data();
        result = vkCreateDevice(m_PhysicalDevice, &fallbackCreateInfo, nullptr, &m_Device);
        if (result != VK_SUCCESS)
        {
            Logger::Log::Warning(
                "Feature-preserving fallback vkCreateDevice failed: {} ({}); retrying minimal fallback",
                VkResultToString(result),
                (int)result);

            VkPhysicalDeviceVulkan12Features minimal12{};
            minimal12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            minimal12.timelineSemaphore = supported12.timelineSemaphore;
            minimal12.hostQueryReset = supported12.hostQueryReset;
            minimal12.bufferDeviceAddress = supported12.bufferDeviceAddress;

            VkPhysicalDeviceFeatures2 minimalFeatures2{};
            minimalFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            minimalFeatures2.features = minimalFeatures;
            minimalFeatures2.pNext = &minimal12;

            VkDeviceCreateInfo minimalCreateInfo = createInfo;
            minimalCreateInfo.pNext = &minimalFeatures2;
            minimalCreateInfo.pEnabledFeatures = nullptr;
            minimalCreateInfo.enabledExtensionCount = static_cast<uint32_t>(fallbackExtensions.size());
            minimalCreateInfo.ppEnabledExtensionNames = fallbackExtensions.data();
            result = vkCreateDevice(m_PhysicalDevice, &minimalCreateInfo, nullptr, &m_Device);
            if (result != VK_SUCCESS)
            {
                Logger::Log::Error("Minimal fallback vkCreateDevice also failed: {} ({})", VkResultToString(result), (int)result);
                return false;
            }

            fallback12 = minimal12;
        }
        // The fallback path intentionally drops optional promoted/KHR feature
        // structs, but keeps the Vulkan 1.2 feature bits required by bindless
        // material shaders when the driver accepts them.
        m_SupportsDrawIndirectCount = (fallback12.drawIndirectCount == VK_TRUE);
        m_SupportsDynamicRendering = false;
        m_SupportsSync2 = false;
        m_HostQueryResetEnabledAtInit = (fallback12.hostQueryReset == VK_TRUE);
        m_BufferDeviceAddressEnabledAtInit = (fallback12.bufferDeviceAddress == VK_TRUE);
        m_DescriptorIndexingEnabledAtInit =
            fallback12.descriptorIndexing == VK_TRUE &&
            fallback12.runtimeDescriptorArray == VK_TRUE &&
            fallback12.descriptorBindingPartiallyBound == VK_TRUE &&
            fallback12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
            fallback12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
        m_DescriptorBufferEnabledAtInit = false;
        m_Capabilities.supportsDescriptorBuffer = false;
        // The fallback ext list carries presentation only, so the device this path
        // creates genuinely does not enable descriptor buffer.
        m_DescriptorBufferExtensionEnabledOnDevice = false;
        m_RayQueryEnabledAtInit = false;
        m_SwapchainColorSpaceExtEnabled = false;
        m_HdrMetadataExtEnabled = false;
        // The fallback ext list carries presentation only, so neither the
        // checkpoints nor the device-fault extension is enabled on this device
        // however their probes went. Leaving either flag set would resolve an
        // entry point for an extension this device never enabled, and report the
        // instrument as armed right up until the loss it cannot service.
        m_DeviceDiagnosticCheckpointsEnabledAtInit = false;
        m_DeviceFaultEnabledAtInit = false;
        // m_DebugUtilsExtEnabled is deliberately NOT cleared with them:
        // VK_EXT_debug_utils is enabled on the INSTANCE, which this fallback does
        // not rebuild, so it survives intact. What the fallback does invalidate is
        // the DEVICE-level entry points its commands resolve to —
        // ResolveDebugUtilsEntryPoints re-resolves them against whichever device
        // this path ended up creating.
    }

    vkGetDeviceQueue(m_Device, m_GraphicsQueueFamily, 0, &m_GraphicsQueue);
    vkGetDeviceQueue(m_Device, m_PresentQueueFamily, 0, &m_PresentQueue);
    if (m_ComputeQueueFamily != kInvalidQueueFamilyIndex)
    {
        vkGetDeviceQueue(m_Device, m_ComputeQueueFamily, 0, &m_ComputeQueue);
    }
    if (m_TransferQueueFamily != kInvalidQueueFamilyIndex)
    {
        vkGetDeviceQueue(m_Device, m_TransferQueueFamily, 0, &m_TransferQueue);
    }

    // Resolve device-level function pointers based on enabled features/extensions (post-vkCreateDevice)
    if (m_SupportsSync2)
    {
        if (m_DeviceProperties.apiVersion < VK_API_VERSION_1_3)
        {
            m_FpCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(m_Device, "vkCmdPipelineBarrier2KHR"));
            m_FpQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(m_Device, "vkQueueSubmit2KHR"));
        }
        else
        {
            m_FpCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(m_Device, "vkCmdPipelineBarrier2"));
            m_FpQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(m_Device, "vkQueueSubmit2"));
        }
    }
    else
    {
        m_FpCmdPipelineBarrier2 = nullptr;
        m_FpQueueSubmit2 = nullptr;
    }

    if (m_SupportsDynamicRendering)
    {
        if (m_DeviceProperties.apiVersion < VK_API_VERSION_1_3)
        {
            m_FpCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(m_Device, "vkCmdBeginRenderingKHR"));
            m_FpCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(m_Device, "vkCmdEndRenderingKHR"));
        }
        else
        {
            m_FpCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(m_Device, "vkCmdBeginRendering"));
            m_FpCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(m_Device, "vkCmdEndRendering"));
        }
    }
    else
    {
        m_FpCmdBeginRendering = nullptr;
        m_FpCmdEndRendering = nullptr;
    }

    m_FpCmdSetDepthWriteEnable = nullptr;
    if (m_DeviceProperties.apiVersion >= VK_API_VERSION_1_3)
    {
        m_FpCmdSetDepthWriteEnable = reinterpret_cast<PFN_vkCmdSetDepthWriteEnable>(vkGetDeviceProcAddr(m_Device, "vkCmdSetDepthWriteEnable"));
    }

    if (m_Capabilities.supportsMeshShaders)
    {
        m_FpCmdDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(m_Device, "vkCmdDrawMeshTasksEXT"));
        m_SupportsMeshShaderEXT = (m_FpCmdDrawMeshTasksEXT != nullptr);
    }
#ifdef VK_EXT_HDR_METADATA_EXTENSION_NAME
    m_FpSetHdrMetadataEXT = m_HdrMetadataExtEnabled
        ? reinterpret_cast<PFN_vkSetHdrMetadataEXT>(vkGetDeviceProcAddr(m_Device, "vkSetHdrMetadataEXT"))
        : nullptr;
#endif

    if (m_Capabilities.supportsDescriptorBuffer)
    {
        m_FpGetDescriptorSetLayoutSizeEXT          = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSizeEXT>(vkGetDeviceProcAddr(m_Device, "vkGetDescriptorSetLayoutSizeEXT"));
        m_FpGetDescriptorSetLayoutBindingOffsetEXT = reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(vkGetDeviceProcAddr(m_Device, "vkGetDescriptorSetLayoutBindingOffsetEXT"));
        m_FpGetDescriptorEXT                       = reinterpret_cast<PFN_vkGetDescriptorEXT>(vkGetDeviceProcAddr(m_Device, "vkGetDescriptorEXT"));
        m_FpCmdBindDescriptorBuffersEXT            = reinterpret_cast<PFN_vkCmdBindDescriptorBuffersEXT>(vkGetDeviceProcAddr(m_Device, "vkCmdBindDescriptorBuffersEXT"));
        m_FpCmdSetDescriptorBufferOffsetsEXT       = reinterpret_cast<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(vkGetDeviceProcAddr(m_Device, "vkCmdSetDescriptorBufferOffsetsEXT"));

        // Guard: if any resolution returned null the driver is lying about ext support.
        // Drop the capability flag rather than crash later. Reset the stashed
        // init-time bit too, otherwise QueryDeviceCapabilities will rehydrate the cap.
        if (m_FpGetDescriptorSetLayoutSizeEXT == nullptr ||
            m_FpGetDescriptorEXT == nullptr ||
            m_FpCmdBindDescriptorBuffersEXT == nullptr ||
            m_FpCmdSetDescriptorBufferOffsetsEXT == nullptr)
        {
            Logger::Log::Warning("VK_EXT_descriptor_buffer enabled but function resolution failed — disabling feature flag");
            m_Capabilities.supportsDescriptorBuffer = false;
            m_DescriptorBufferEnabledAtInit = false;
        }
    }

    if (m_DeviceDiagnosticCheckpointsEnabledAtInit)
    {
        m_FpCmdSetCheckpointNV = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(vkGetDeviceProcAddr(m_Device, "vkCmdSetCheckpointNV"));
        m_FpGetQueueCheckpointDataNV = reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(vkGetDeviceProcAddr(m_Device, "vkGetQueueCheckpointDataNV"));
        // Same driver-lying guard as descriptor buffers: emission without
        // retrieval (or vice versa) is a half-instrument, so drop both.
        if (m_FpCmdSetCheckpointNV == nullptr || m_FpGetQueueCheckpointDataNV == nullptr)
        {
            Logger::Log::Warning("VK_NV_device_diagnostic_checkpoints enabled but function resolution failed — disabling GPU checkpoints");
            m_FpCmdSetCheckpointNV = nullptr;
            m_FpGetQueueCheckpointDataNV = nullptr;
            m_DeviceDiagnosticCheckpointsEnabledAtInit = false;
        }
    }

    // Per-family checkpoint support. The extension being present does NOT mean
    // every queue family can report checkpoints: a family whose
    // checkpointExecutionStageMask is 0 records nothing and returns nothing, and
    // in the device-lost dump that is indistinguishable from a family that simply
    // ran no marked work. Probe it once so the report can name which it is.
    m_QueueFamilyCheckpointStages.clear();
    if (m_DeviceDiagnosticCheckpointsEnabledAtInit)
    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties2(m_PhysicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties2> familyProps(familyCount);
        std::vector<VkQueueFamilyCheckpointPropertiesNV> checkpointProps(familyCount);
        for (uint32_t f = 0; f < familyCount; ++f)
        {
            checkpointProps[f].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_CHECKPOINT_PROPERTIES_NV;
            checkpointProps[f].pNext = nullptr;
            familyProps[f].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
            familyProps[f].pNext = &checkpointProps[f];
        }
        vkGetPhysicalDeviceQueueFamilyProperties2(m_PhysicalDevice, &familyCount, familyProps.data());

        m_QueueFamilyCheckpointStages.resize(familyCount);
        std::string summary;
        for (uint32_t f = 0; f < familyCount; ++f)
        {
            m_QueueFamilyCheckpointStages[f] = checkpointProps[f].checkpointExecutionStageMask;
            char maskHex[16];
            std::snprintf(maskHex, sizeof(maskHex), "0x%08x",
                          static_cast<unsigned>(m_QueueFamilyCheckpointStages[f]));
            if (!summary.empty())
                summary += ", ";
            summary += std::to_string(f) + "=" + maskHex;
        }
        // Loud on purpose: which families are blind decides whether an empty
        // checkpoint result in a loss dump is a finding or an instrument limit.
        Logger::Log::Info("VulkanDevice: checkpoint stage mask per queue family: {} (graphics={}, compute={}, transfer={})",
                          summary, m_GraphicsQueueFamily, GetComputeQueueFamilyIndex(), GetTransferQueueFamilyIndex());
        for (uint32_t f = 0; f < familyCount; ++f)
        {
            if (m_QueueFamilyCheckpointStages[f] == 0)
                Logger::Log::Warning("VulkanDevice: queue family {} reports NO checkpoint stages — GPU breadcrumbs are blind on it",
                                     f);
        }
    }
    // Emission is opt-in; retrieval is always armed when the extension resolved,
    // so an unlucky TDR still reports whatever the driver has (nothing, if no
    // checkpoint was ever recorded). The name table is created once and kept
    // across device rebuilds so recorded ordinals stay comparable.
    m_GpuCheckpointsEnabled = m_DeviceDiagnosticCheckpointsEnabledAtInit &&
                              ParseGpuCheckpointsEnabled(std::getenv("GE_VK_GPU_CHECKPOINTS"));
    if (m_GpuCheckpointsEnabled && !m_GpuCheckpointNames)
    {
        m_GpuCheckpointNames = std::make_unique<GpuCheckpointNameTable>();
    }
    if (m_DeviceFaultEnabledAtInit)
    {
        m_FpGetDeviceFaultInfoEXT = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
            vkGetDeviceProcAddr(m_Device, "vkGetDeviceFaultInfoEXT"));
        if (m_FpGetDeviceFaultInfoEXT == nullptr)
        {
            Logger::Log::Warning("VK_EXT_device_fault enabled but vkGetDeviceFaultInfoEXT failed to resolve — disabling fault reporting");
            m_DeviceFaultEnabledAtInit = false;
        }
    }
    // Stated at startup in every case: after a loss it is too late to learn that
    // the instrument was never armed, an absent report must never read as "clean",
    // and "we did not ask" is a different fact from "the device cannot".
    if (m_FpGetDeviceFaultInfoEXT != nullptr)
    {
        Logger::Log::Info("VulkanDevice: VK_EXT_device_fault ARMED — a device loss will report fault address/vendor records automatically");
    }
    else if (!deviceFaultRequested)
    {
        Logger::Log::Info("VulkanDevice: VK_EXT_device_fault NOT REQUESTED (opt-in in this config) — device-lost reports will carry no faulting-address records; set GE_VK_DEVICE_FAULT=1 to arm it");
    }
    else
    {
        Logger::Log::Info("VulkanDevice: VK_EXT_device_fault unavailable — device-lost reports will carry no faulting-address records");
    }

    if (!m_DeviceDiagnosticCheckpointsEnabledAtInit)
    {
        Logger::Log::Info("VulkanDevice: GPU device-loss checkpoints unavailable (VK_NV_device_diagnostic_checkpoints not supported) — device-lost reports stay CPU-marker only");
    }
    else
    {
        Logger::Log::Info("VulkanDevice: GPU device-loss checkpoints available; emission {} (GE_VK_GPU_CHECKPOINTS)",
                          m_GpuCheckpointsEnabled ? "ENABLED" : "off by default — set to 1 to localize a device loss");
    }

    // An in-place rebuild keeps the backend (TeardownDeviceScopedForRebuild
    // emptied it against the old device) and rebinds it to this one.
    const bool rebinding = m_AccelerationStructures != nullptr;
    if (rebinding)
        m_AccelerationStructures->BindRebuiltDevice();
    else if (m_RayQueryEnabledAtInit)
        m_AccelerationStructures = std::make_unique<VulkanAccelerationStructures>(*this);
    if (m_AccelerationStructures && !m_AccelerationStructures->IsFunctional())
    {
        // Entry points failed to resolve (extensions enabled but the driver
        // lied, or the rebuilt device no longer enables ray query) — same
        // guard as descriptor buffers: drop the stash (the source
        // QueryDeviceCapabilities rehydrates supportsRayQuery from) so the RT
        // lane stays silently unavailable. A rebound backend is kept, empty
        // and refusing every call, because consumers from before the rebuild
        // still hold its pointer; GetAccelerationStructureBackend hides it
        // from new ones.
        if (!rebinding)
            m_AccelerationStructures.reset();
        m_RayQueryEnabledAtInit = false;
    }

    // Default ON when the device supports descriptor buffers: every migrated pass
    // (RenderGraphOverlay, FullscreenShaderNode, GPUCulling, Sky, BRDF LUT,
    // DepthReduce, Selection-Outline, Gizmo-Composite, UI Overlay) takes the DB
    // path and the measured CPU recovery applies by default. GE_VK_USE_DESCRIPTOR
    // _BUFFER=0 forces the legacy pool path for bisection / regression testing.
    if (m_Capabilities.supportsDescriptorBuffer)
    {
        m_UseDescriptorBuffer = true;
        if (const char* env = std::getenv("GE_VK_USE_DESCRIPTOR_BUFFER"))
        {
            m_UseDescriptorBuffer = (env[0] != '\0' && env[0] != '0' && env[0] != 'f' && env[0] != 'F');
        }
        if (m_UseDescriptorBuffer)
        {
            Logger::Log::Info("VK_EXT_descriptor_buffer runtime path ENABLED (default on)");
        }
        else
        {
            Logger::Log::Info("VK_EXT_descriptor_buffer runtime path DISABLED (GE_VK_USE_DESCRIPTOR_BUFFER=0)");
        }
    }

    return true;
}

bool VulkanDevice::CreateCommandPool()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_GraphicsQueueFamily;

    VkResult result = vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool);
    if (result != VK_SUCCESS)
    {
        Logger::Log::Error("Failed to create command pool: {}", (int)result);
        return false;
    }
    // Create optional compute/transfer pools if families exist
    if (m_ComputeQueueFamily != kInvalidQueueFamilyIndex)
    {
        poolInfo.queueFamilyIndex = m_ComputeQueueFamily;
        if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_ComputeCommandPool) != VK_SUCCESS)
        {
            Logger::Log::Error("Failed to create compute command pool");
            return false;
        }
    }
    if (m_TransferQueueFamily != kInvalidQueueFamilyIndex)
    {
        poolInfo.queueFamilyIndex = m_TransferQueueFamily;
        if (vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_TransferCommandPool) != VK_SUCCESS)
        {
            Logger::Log::Error("Failed to create transfer command pool");
            return false;
        }
    }

    return true;
}

VkResult VulkanDevice::CreateSemaphoreTracked(const VkSemaphoreCreateInfo& createInfo, VkSemaphore& outSemaphore)
{
    const VkResult result = vkCreateSemaphore(m_Device, &createInfo, nullptr, &outSemaphore);
    if (result == VK_SUCCESS)
    {
        m_SemaphoresCreated.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

void VulkanDevice::DestroySemaphoreTracked(VkSemaphore semaphore)
{
    if (semaphore == VK_NULL_HANDLE)
    {
        return;
    }
    vkDestroySemaphore(m_Device, semaphore, nullptr);
    m_SemaphoresDestroyed.fetch_add(1, std::memory_order_relaxed);
}

void VulkanDevice::CloseSemaphoreLedgerForDeviceGeneration()
{
    const uint64_t created = m_SemaphoresCreated.exchange(0, std::memory_order_relaxed);
    const uint64_t destroyed = m_SemaphoresDestroyed.exchange(0, std::memory_order_relaxed);
    if (destroyed > created)
    {
        // Unsigned subtraction would report this as a colossal residual and read as
        // the opposite defect, so name it instead: a handle was destroyed twice.
        m_LastDeviceGenerationSemaphoreResidual = 0;
        Logger::Log::Error(
            "VulkanDevice: VkSemaphore ledger destroyed {} for {} created — a handle was destroyed more than once.",
            (unsigned long long)destroyed,
            (unsigned long long)created);
        return;
    }
    m_LastDeviceGenerationSemaphoreResidual = created - destroyed;
    if (m_LastDeviceGenerationSemaphoreResidual != 0)
    {
        Logger::Log::Error(
            "VulkanDevice: {} VkSemaphore(s) outlive the device being destroyed ({} created, {} destroyed). "
            "Vulkan requires every child object to be destroyed first; find the owner that dropped its handles.",
            (unsigned long long)m_LastDeviceGenerationSemaphoreResidual,
            (unsigned long long)created,
            (unsigned long long)destroyed);
        return;
    }
    // Reported on the clean path too: device destruction is rare, and a reader
    // checking parity needs a line that is PRESENT when the invariant holds. An
    // absent error line cannot distinguish "clean" from "this never ran".
    Logger::Log::Info("VulkanDevice: VkSemaphore ledger clean at device destruction ({} created, {} destroyed)",
                      (unsigned long long)created,
                      (unsigned long long)destroyed);
}

bool VulkanDevice::CreateSyncObjects()
{
    // Present-ready semaphores are per-swapchain-image and belong to CreateSwapchain.
    // Creating any here would strand them: the first window target resets the member
    // vector, and nothing owns what the reset drops.
    m_ImageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_FenceReg.Clear();

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (CreateSemaphoreTracked(semaphoreInfo, m_ImageAvailableSemaphores[i]) != VK_SUCCESS)
        {
            Logger::Log::Error("Failed to create synchronization objects for frame {}", (int)i);
            return false;
        }
    }
    return true;
}

bool VulkanDevice::CheckExtensionSupport(const char* extensionName)
{
    // Query available device extensions
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extensionCount, availableExtensions.data());

    // Check if the requested extension is available
    for (const auto& extension : availableExtensions)
    {
        if (strcmp(extension.extensionName, extensionName) == 0)
        {

            return true;
        }
    }

    return false;
}

void VulkanDevice::QueryDeviceCapabilities()
{
    m_Capabilities = {};
    // Rehydrate descriptor-buffer enablement stash-set by CreateLogicalDevice — it runs
    // before this function and sets m_Capabilities.supportsDescriptorBuffer, which the
    // above reset clobbers. Keep the runtime gate consistent.
    m_Capabilities.supportsDescriptorBuffer = m_DescriptorBufferEnabledAtInit;
    // Same stash pattern for Vulkan 1.2 bufferDeviceAddress — InitializeVMA reads this
    // to gate VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT.
    m_Capabilities.supportsBufferDeviceAddress = m_BufferDeviceAddressEnabledAtInit;
    // Same stash pattern for the 1.2 drawIndirectCount feature — set during
    // logical-device creation before this reset runs. When false, the command
    // list emulates DrawIndexedIndirectCount by iterating maxDrawCount records
    // and producers must zero-fill unused trailing draw records.
    m_Capabilities.supportsDrawIndirectCountNative = m_SupportsDrawIndirectCount;

    // shaderInt64 is enabled by CreateLogicalDevice whenever the device supports
    // it (main + minimal fallback paths both set deviceFeatures.shaderInt64), so
    // support == enabled here. Expose the bit for CBT terrain, which stores its
    // per-bisector HeapID as a plain (non-atomic) u64 and gates on this.
    {
        VkPhysicalDeviceFeatures baseFeatures{};
        vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &baseFeatures);
        m_Capabilities.supportsShaderInt64 = (baseFeatures.shaderInt64 == VK_TRUE);
        // Same support == enabled contract as shaderInt64: CreateLogicalDevice
        // enables textureCompressionBC whenever the device reports it.
        m_Capabilities.supportsTextureCompressionBC = (baseFeatures.textureCompressionBC == VK_TRUE);
        // Same contract for dualSrcBlend; the limit is checked as well because
        // dual-source pipelines consume one dual-src attachment slot.
        m_Capabilities.supportsDualSourceBlending =
            (baseFeatures.dualSrcBlend == VK_TRUE) &&
            m_DeviceProperties.limits.maxFragmentDualSrcAttachments >= 1;
        // Same contract for samplerAnisotropy: all three vkCreateDevice paths enable
        // it whenever supported (the main features, and minimalFeatures, which both
        // fallbacks pass). Without the feature a sampler must not enable anisotropy
        // at all, which a limit of 1 expresses.
        m_Capabilities.maxSamplerAnisotropy = (baseFeatures.samplerAnisotropy == VK_TRUE)
                                                  ? m_DeviceProperties.limits.maxSamplerAnisotropy
                                                  : 1.0f;
        // Same contract: CreateLogicalDevice enables sampleRateShading whenever supported.
        m_Capabilities.supportsSampleRateShading = (baseFeatures.sampleRateShading == VK_TRUE);
    }

    // Basic capabilities - check for extensions
    m_Capabilities.supportsMeshShaders = CheckExtensionSupport("VK_EXT_mesh_shader");
    m_Capabilities.supportsRayTracing = CheckExtensionSupport("VK_KHR_ray_tracing_pipeline");
    // Enabled-at-init stash, not detection: true only when acceleration
    // structures + ray query were actually enabled on the logical device
    // (see CreateLogicalDevice; false on the fallback creation paths).
    m_Capabilities.supportsRayQuery = m_RayQueryEnabledAtInit;

    // Publish the max SPIR-V version this device can consume to the (backend-
    // agnostic) shader compile service: Vulkan 1.3 devices accept SPIR-V 1.6,
    // 1.2 devices cap at 1.5 so their packages remain loadable.
    ShaderCompileService::SetMaxSupportedSpirv(
        m_DeviceProperties.apiVersion >= VK_API_VERSION_1_3
            ? ShaderCompileService::SpirvTarget::Spirv_1_6
            : ShaderCompileService::SpirvTarget::Spirv_1_5);
    m_Capabilities.maxPerStageSamplers = m_DeviceProperties.limits.maxPerStageDescriptorSamplers;
    m_Capabilities.maxPerStageSampledImages = m_DeviceProperties.limits.maxPerStageDescriptorSampledImages;
    m_Capabilities.maxPerStageResources = m_DeviceProperties.limits.maxPerStageResources;
    m_Capabilities.maxPerStageStorageBuffers =
        m_DeviceProperties.limits.maxPerStageDescriptorStorageBuffers;
    m_Capabilities.maxComputeWorkGroupSize = m_DeviceProperties.limits.maxComputeWorkGroupInvocations;
    m_Capabilities.maxComputeSharedMemorySize = m_DeviceProperties.limits.maxComputeSharedMemorySize;
    {
        m_Capabilities.vendorId = m_DeviceProperties.vendorID;
        m_Capabilities.vendor   = ClassifyGpuVendor(m_DeviceProperties.vendorID);
        const bool isAmd = m_Capabilities.vendor == GpuVendor::Amd;
        const char* deviceName = m_DeviceProperties.deviceName;
        const bool isRadeon = std::strstr(deviceName, "Radeon") != nullptr;
        auto hasThreeDigitRxSeries = [deviceName](char series) -> bool
        {
            const char* p = deviceName;
            while ((p = std::strstr(p, "RX ")) != nullptr)
            {
                const char* model = p + 3;
                const bool isDigit1 = model[1] >= '0' && model[1] <= '9';
                const bool isDigit2 = model[2] >= '0' && model[2] <= '9';
                const bool isDigit3 = model[3] >= '0' && model[3] <= '9';
                if (model[0] == series && isDigit1 && isDigit2 && !isDigit3)
                    return true;
                ++p;
            }
            return false;
        };
        const bool isLegacyRadeon =
            isAmd && isRadeon &&
            (std::strstr(deviceName, "Radeon Pro 4") != nullptr ||
             std::strstr(deviceName, "Radeon Pro 5") != nullptr ||
             hasThreeDigitRxSeries('4') ||
             hasThreeDigitRxSeries('5') ||
             std::strstr(deviceName, "Radeon R5") != nullptr ||
             std::strstr(deviceName, "Radeon R7") != nullptr ||
             std::strstr(deviceName, "Radeon R9") != nullptr ||
             std::strstr(deviceName, "Radeon HD") != nullptr);
        m_Capabilities.prefersNoDefaultMSAA = isLegacyRadeon;
        m_Capabilities.prefersStableShadowFiltering = isLegacyRadeon;
    }
    {
        const VkSampleCountFlags sampleMask = m_DeviceProperties.limits.framebufferColorSampleCounts &
                                              m_DeviceProperties.limits.framebufferDepthSampleCounts;
        m_Capabilities.maxMSAASamples = 1;
        for (uint32_t samples : {2u, 4u, 8u})
        {
            if ((sampleMask & static_cast<VkSampleCountFlags>(samples)) != 0u)
            {
                m_Capabilities.maxMSAASamples = samples;
            }
        }
    }
    // Probe descriptor indexing features for bindless support
    {
        VkPhysicalDeviceDescriptorIndexingFeatures idx{};
        idx.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &idx;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &f2);
        const bool hasExt = CheckExtensionSupport(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        m_Capabilities.supportsBindlessResources = m_DescriptorIndexingEnabledAtInit &&
                                                   hasExt &&
                                                   idx.runtimeDescriptorArray && idx.descriptorBindingPartiallyBound;
    }
    // Query the actual device limit for bindless texture (SAMPLED_IMAGE)
    // descriptors. The bindless set's large array (binding 0) is a SAMPLED_IMAGE
    // array (the companion SAMPLER array, binding 1, holds only
    // SamplerPreset::kCount entries and is trivially within any sampler limit, so
    // it no longer constrains the texture-array size). The array is visible to
    // vertex and fragment shaders (terrain samples height in VS, material textures
    // in FS), so the cross-stage total is divided across those two stages.
    {
        VkPhysicalDeviceVulkan12Properties props12{};
        props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 devProps2{};
        devProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        devProps2.pNext = &props12;
        vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &devProps2);

        uint32_t perStageImageLimit = props12.maxPerStageDescriptorUpdateAfterBindSampledImages;
        if (perStageImageLimit == 0)
            perStageImageLimit = m_DeviceProperties.limits.maxPerStageDescriptorSampledImages;
        uint32_t totalImageLimit = props12.maxDescriptorSetUpdateAfterBindSampledImages;
        if (totalImageLimit == 0)
            totalImageLimit = m_DeviceProperties.limits.maxDescriptorSetSampledImages;
        // Reserve headroom for non-bindless sampled-image bindings in other sets.
        constexpr uint32_t kReservedImageSlots = 8;
        constexpr uint32_t kBindlessVisibleStages = 2;
        const uint32_t perStageBudget = (perStageImageLimit > kReservedImageSlots)
                                            ? (perStageImageLimit - kReservedImageSlots)
                                            : perStageImageLimit;
        const uint32_t totalBudget = (totalImageLimit > kReservedImageSlots)
                                         ? (totalImageLimit - kReservedImageSlots)
                                         : totalImageLimit;
        const uint32_t bindlessLimit = std::min(perStageBudget, totalBudget / kBindlessVisibleStages);
        m_Capabilities.maxBindlessTextures = std::min(bindlessLimit, 1000000u);

        // The bindless texture set is a single persistent UPDATE_AFTER_BIND
        // descriptor set, allocated from the legacy descriptor pool on every
        // platform (UAB layouts can't be descriptor buffers, so it's pool-backed
        // even on descriptor-buffer-capable GPUs). A descriptor pool must reserve
        // at least as many descriptors as any set layout it allocates declares,
        // so the bindless SAMPLED_IMAGE array cannot exceed what that pool reserves
        // (DescriptorSetAllocator::Config::UpdateAfterBindSampledImages).
        //
        // Historically the array was sized from the device limit (~1M on desktop,
        // large on MoltenVK) while the pool reserved only 16384. Lenient desktop
        // drivers over-allocate past the declared pool size and hid the violation;
        // MoltenVK enforces it and returns VK_ERROR_OUT_OF_POOL_MEMORY — the
        // bindless set never allocates, descriptor set 1 is left unbound, and the
        // GPU-driven world draw faults. Clamp to the pool's reserved budget on ALL
        // platforms so layout and pool always agree (single source of truth = the
        // pool Config). Raising both in lockstep, or a VARIABLE_DESCRIPTOR_COUNT
        // growable heap, is the path to a larger capacity.
        const uint32_t uabPoolSampledImages =
            DescriptorSetAllocator::Config{}.UpdateAfterBindSampledImages;
        m_Capabilities.maxBindlessTextures =
            std::min(m_Capabilities.maxBindlessTextures, uabPoolSampledImages);
    }
    if (m_ForceDisableBindlessResources)
    {
        m_Capabilities.supportsBindlessResources = false;
        m_Capabilities.maxBindlessTextures = 0;
        m_Capabilities.maxBindlessBuffers = 0;
    }

    // Phase-0 diagnostics for the planned VK_EXT_descriptor_buffer migration.
    // Read-only; no code path consumes these bits yet.
    {
        m_DescBufferDiag = {};
        m_DescBufferDiag.extensionAvailable = CheckExtensionSupport(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        const bool hasPushDesc = CheckExtensionSupport(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) ||
                                 m_DeviceProperties.apiVersion >= VK_API_VERSION_1_4;

        VkPhysicalDevicePushDescriptorPropertiesKHR pushProps{};
        pushProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR;

        VkPhysicalDeviceDescriptorBufferPropertiesEXT dbProps{};
        dbProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;

        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        void** tail = &props2.pNext;
        if (m_DescBufferDiag.extensionAvailable)
        {
            *tail = &dbProps;
            tail = &dbProps.pNext;
        }
        if (hasPushDesc)
        {
            *tail = &pushProps;
            tail = &pushProps.pNext;
        }
        if (props2.pNext != nullptr)
        {
            vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &props2);
        }

        if (hasPushDesc)
        {
            m_DescBufferDiag.maxPushDescriptors = pushProps.maxPushDescriptors;
        }

        if (m_DescBufferDiag.extensionAvailable)
        {
            m_DescBufferDiag.descriptorBufferOffsetAlignment = dbProps.descriptorBufferOffsetAlignment;
            m_DescBufferDiag.samplerDescriptorSize = static_cast<uint32_t>(dbProps.samplerDescriptorSize);
            m_DescBufferDiag.combinedImageSamplerDescriptorSize = static_cast<uint32_t>(dbProps.combinedImageSamplerDescriptorSize);
            m_DescBufferDiag.sampledImageDescriptorSize = static_cast<uint32_t>(dbProps.sampledImageDescriptorSize);
            m_DescBufferDiag.storageImageDescriptorSize = static_cast<uint32_t>(dbProps.storageImageDescriptorSize);
            m_DescBufferDiag.uniformBufferDescriptorSize = static_cast<uint32_t>(dbProps.uniformBufferDescriptorSize);
            m_DescBufferDiag.storageBufferDescriptorSize = static_cast<uint32_t>(dbProps.storageBufferDescriptorSize);
            m_DescBufferDiag.inputAttachmentDescriptorSize = static_cast<uint32_t>(dbProps.inputAttachmentDescriptorSize);
            m_DescBufferDiag.accelerationStructureDescriptorSize =
                static_cast<uint32_t>(dbProps.accelerationStructureDescriptorSize);
            m_DescBufferDiag.combinedImageSamplerDescriptorSingleArray = (dbProps.combinedImageSamplerDescriptorSingleArray == VK_TRUE);
            m_DescBufferDiag.bufferlessPushDescriptors = (dbProps.bufferlessPushDescriptors == VK_TRUE);
            m_DescBufferDiag.maxResourceDescriptorBufferRange = dbProps.maxResourceDescriptorBufferRange;
            m_DescBufferDiag.maxSamplerDescriptorBufferRange = dbProps.maxSamplerDescriptorBufferRange;
            m_DescBufferDiag.maxResourceDescriptorBufferBindings = dbProps.maxResourceDescriptorBufferBindings;

            VkPhysicalDeviceDescriptorBufferFeaturesEXT dbFeatures{};
            dbFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 fq{};
            fq.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            fq.pNext = &dbFeatures;
            vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &fq);
            m_DescBufferDiag.descriptorBuffer = (dbFeatures.descriptorBuffer == VK_TRUE);
            m_DescBufferDiag.descriptorBufferCaptureReplay = (dbFeatures.descriptorBufferCaptureReplay == VK_TRUE);
            m_DescBufferDiag.descriptorBufferImageLayoutIgnored = (dbFeatures.descriptorBufferImageLayoutIgnored == VK_TRUE);
            m_DescBufferDiag.descriptorBufferPushDescriptors = (dbFeatures.descriptorBufferPushDescriptors == VK_TRUE);
        }
    }

    // Storage buffer offset alignment (used for buffer slice bindings in GPU culling, etc.).
    // This is the Vulkan device limit minStorageBufferOffsetAlignment exposed through our
    // backend-agnostic capabilities.
    m_Capabilities.minStorageBufferOffsetAlignment =
        static_cast<size_t>(m_DeviceProperties.limits.minStorageBufferOffsetAlignment);
    m_Capabilities.maxStorageBufferBindingSize =
        static_cast<size_t>(m_DeviceProperties.limits.maxStorageBufferRange);

    // Negative viewport height is available via VK_KHR_maintenance1 and promoted to core in Vulkan 1.1+
    // Honor either the extension or a core API version >= 1.1 so +Y up works broadly
    m_SupportsNegativeViewportHeight =
        (m_DeviceProperties.apiVersion >= VK_API_VERSION_1_1) ||
        CheckExtensionSupport(VK_KHR_MAINTENANCE1_EXTENSION_NAME);

    // Query depth/stencil resolve properties (VK_KHR_depth_stencil_resolve)
    {
        VkPhysicalDeviceDepthStencilResolveProperties dsProps{};
        dsProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &dsProps;
        vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &props2);
        m_Capabilities.supportedDepthResolveModes = VulkanMappings::TranslateVulkanResolveModesToEngine(dsProps.supportedDepthResolveModes);
        m_Capabilities.supportedStencilResolveModes = VulkanMappings::TranslateVulkanResolveModesToEngine(dsProps.supportedStencilResolveModes);
        // Consider either independentResolve or independentResolveNone as signal for some level of independence
        m_Capabilities.supportsIndependentStencilResolve = (dsProps.independentResolve == VK_TRUE) || (dsProps.independentResolveNone == VK_TRUE);

        if (std::getenv("GE_DEBUG_PRINT_RESOLVE_CAPS"))
        {
            auto printModes = [](uint32_t flags)
            {
                printf("%s%s%s%s",
                       (flags & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) ? " SampleZero" : "",
                       (flags & VK_RESOLVE_MODE_AVERAGE_BIT) ? " Average" : "",
                       (flags & VK_RESOLVE_MODE_MIN_BIT) ? " Min" : "",
                       (flags & VK_RESOLVE_MODE_MAX_BIT) ? " Max" : "");
            };
            printf("[Caps] Depth resolve modes:");
            printModes(m_Capabilities.supportedDepthResolveModes);
            printf("\n");
            printf("[Caps] Stencil resolve modes:");
            printModes(m_Capabilities.supportedStencilResolveModes);
            printf("\n");
        }
    }

    // Choose a preferred depth (and optional stencil) attachment format based on device support.
    {
        auto pickFirstSupported = [this](std::initializer_list<TextureFormat> candidates)
            -> TextureFormat
        {
            const uint32_t usage =
                static_cast<uint32_t>(TextureUsage::DepthStencil) |
                static_cast<uint32_t>(TextureUsage::ShaderResource);
            for (TextureFormat tf : candidates)
            {
                if (IsTextureFormatSupported(tf, usage))
                    return tf;
            }
            return TextureFormat::Unknown;
        };

        // Reverse-Z requires float depth (D24_UNORM is uniform-distributed and
        // discards the precision win). Priority: float-depth combined first,
        // then float-depth-only, then unorm fallbacks for last-resort hardware
        // that doesn't expose float depth at all.
        // 1) Float depth + stencil (preferred).
        TextureFormat chosen = pickFirstSupported({
            TextureFormat::D32_SFLOAT_S8_UINT,
        });

        // 2) Float depth-only.
        if (chosen == TextureFormat::Unknown)
        {
            chosen = pickFirstSupported({
                TextureFormat::D32_FLOAT,
            });
        }

        // 3) UNORM fallbacks (degraded reverse-Z precision).
        if (chosen == TextureFormat::Unknown)
        {
            chosen = pickFirstSupported({
                TextureFormat::D24_UNORM_S8_UINT,
                TextureFormat::X8_D24_UNORM_PACK32,
                TextureFormat::D16_UNORM,
            });
        }

        if (chosen == TextureFormat::Unknown)
        {
            // 3) Nothing usable: log loudly and mark as Unknown instead of guessing.
            Logger::Log::Error("[Vulkan] No supported depth/stencil format for optimal tiling; depth testing will be unavailable on this device.");
            m_Capabilities.preferredDepthAndStencilFormat = TextureFormat::Unknown;
        }
        else
        {
            m_Capabilities.preferredDepthAndStencilFormat = chosen;
        }
    }

    // Query mesh shader properties if supported
    if (m_Capabilities.supportsMeshShaders)
    {
        VkPhysicalDeviceMeshShaderPropertiesEXT meshShaderProps{};
        meshShaderProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;

        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &meshShaderProps;

        vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &props2);
    }

    // Published, not derived: SelectPhysicalDevice owns the derivation so the
    // value survives this function's m_Capabilities reset unchanged.
    m_Capabilities.memoryTopology = m_MemoryTopology;
    m_Capabilities.dedicatedVideoMemory =
        m_MemoryTopology.has_value() ? static_cast<size_t>(m_MemoryTopology->deviceLocalHeapBytesTotal) : 0u;

    LogMemoryTopology();
    LogGpuToolingState();

    // GE_FORCE_COMPAT=1: clamp to the WebGPU-class subset so the whole
    // compatibility renderer is testable on desktop Vulkan (native tooling,
    // RenderDoc) without a WebGPU device in the loop.
    ApplyForceCompatOverride(m_Capabilities);
}

void VulkanDevice::LogGpuToolingState() const
{
    bool toolingQueryAvailable = false;
    const std::vector<AttachedGraphicsTool> tools =
        QueryAttachedTools(m_Instance, m_PhysicalDevice, m_InstanceApiVersion, toolingQueryAvailable);

    // "LIVE" is about this device, not this build: the label entry points either
    // resolved here or they did not, and a build compiled with label calls in it
    // proves nothing either way.
    Logger::Log::Info(
        "VulkanDevice GPU tooling: vendor {} (0x{:04X}); debug labels {}; validation layer {}; tools {}",
        GpuVendorName(m_Capabilities.vendor),
        m_Capabilities.vendorId,
        m_DebugUtilsLabelFns.Available() ? "LIVE" : "unavailable",
        m_DebugLayerEnabled ? "ON" : "off",
        FormatAttachedTools(tools, toolingQueryAvailable));
}

void VulkanDevice::LogMemoryTopology() const
{
    const auto& topology = m_MemoryTopology;
    if (!topology.has_value())
        return;

    constexpr double kBytesPerMiB = 1024.0 * 1024.0;
    Logger::Log::Info(
        "VulkanDevice memory topology: unified={} deviceLocalTotal={:.0f} MiB "
        "largestHostVisibleDeviceLocalHeap={:.0f} MiB ({:.1f}% of device-local)",
        topology->isUnifiedMemory,
        static_cast<double>(topology->deviceLocalHeapBytesTotal) / kBytesPerMiB,
        static_cast<double>(topology->largestHostVisibleDeviceLocalHeapBytes) / kBytesPerMiB,
        topology->deviceLocalHeapBytesTotal > 0
            ? 100.0 * static_cast<double>(topology->largestHostVisibleDeviceLocalHeapBytes) /
                  static_cast<double>(topology->deviceLocalHeapBytesTotal)
            : 0.0);

    // The residency gate's decision, stated rather than left to be re-derived
    // from the numbers above. Where a pool actually lands is a separate fact and
    // belongs to the allocation, not the device — MeshGPURegistry logs the
    // resolved heap of the pools themselves.
    const bool fullyHostWritable = DeviceLocalMemoryIsFullyHostWritable(topology);
    Logger::Log::Info(
        "VulkanDevice device-local host-write gate: {} - UploadDeviceLocalPreferred buffers "
        "(mesh pools) {} on this device. Reason: {}",
        fullyHostWritable ? "ELIGIBLE" : "NOT ELIGIBLE",
        fullyHostWritable ? "prefer the device heap" : "keep the host preference, as before",
        DescribeResidencyGateDecision(topology));
}

IDevice::BufferMemoryResidency VulkanDevice::GetBufferMemoryResidency(BufferHandle handle) const
{
    const VulkanBuffer* vulkanBuffer = GetVulkanBufferConst(handle);
    if (vulkanBuffer == nullptr)
        return {};

    BufferMemoryResidency residency{};
    residency.reported = true;
    const VkMemoryPropertyFlags flags = vulkanBuffer->resolvedMemoryProperties;
    residency.deviceLocal = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    residency.hostVisible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    residency.hostCoherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

#ifdef RENDERING_HAS_VMA
    // The property flags alone cannot say WHICH heap absorbed the allocation,
    // and on a device with both a small BAR heap and a large device-local heap
    // that is the whole question.
    if (vulkanBuffer->allocation != nullptr)
    {
        VmaAllocationInfo allocInfo{};
        vmaGetAllocationInfo(AsVmaAllocator(m_VmaAllocator), vulkanBuffer->allocation, &allocInfo);
        if (allocInfo.memoryType < m_MemoryProperties.memoryTypeCount)
        {
            residency.heapIndex = m_MemoryProperties.memoryTypes[allocInfo.memoryType].heapIndex;
            if (residency.heapIndex < m_MemoryProperties.memoryHeapCount)
                residency.heapSizeBytes = m_MemoryProperties.memoryHeaps[residency.heapIndex].size;
        }
    }
#endif
    return residency;
}

VkMemoryPropertyFlags VulkanDevice::GetBufferResolvedMemoryProperties(BufferHandle handle) const
{
    const VulkanBuffer* vulkanBuffer = GetVulkanBufferConst(handle);
    return vulkanBuffer ? vulkanBuffer->resolvedMemoryProperties : 0;
}

// Clean resource creation implementation
BufferHandle VulkanDevice::CreateBuffer(const BufferDesc& desc)
{
    // Minimal validation (no excessive logging)
    if (desc.size == 0)
    {
        return INVALID_BUFFER_HANDLE;
    }

    // Reachable from job threads (upload workers, UpdateBuffer's staging path);
    // vmaCreateBuffer must not overlap an in-place rebuild's ShutdownVMA.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Create);

#ifdef RENDERING_HAS_VMA
    // A FAILED rebuild releases the exclusive lock with no allocator (ShutdownVMA
    // ran; bringup never reached InitializeVMA) and health Lost. Workers keep
    // arriving during the retry backoff, and vmaCreateBuffer(nullptr) is a
    // crash, not an error return.
    if (m_VmaAllocator == nullptr)
    {
        return INVALID_BUFFER_HANDLE;
    }
#endif

    VulkanBuffer* vulkanBuffer = new VulkanBuffer();
    vulkanBuffer->device = m_Device;
    vulkanBuffer->size = desc.size;
    vulkanBuffer->usage = static_cast<BufferUsage>(desc.usage);
    vulkanBuffer->createFlags = desc.flags;
    vulkanBuffer->debugName = desc.debugName ? desc.debugName : "";

    // Set memory properties based on memoryUsage
    switch (desc.memoryUsage)
    {
    case BufferMemoryUsage::Upload:
    // Same recorded properties as Upload: the device preference changes which
    // heap VMA resolves to, never the host-visibility the engine maps through.
    // What the allocation actually resolved to is answered by
    // GetBufferMemoryResidency, not by this field.
    case BufferMemoryUsage::UploadDeviceLocalPreferred:
        vulkanBuffer->memoryProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
    case BufferMemoryUsage::Readback:
        vulkanBuffer->memoryProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    case BufferMemoryUsage::DeviceLocal:
        vulkanBuffer->memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case BufferMemoryUsage::Auto:
    default:
        vulkanBuffer->memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    }

    // Convert usage flags to Vulkan usage
    VkBufferUsageFlags vkUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    if (desc.usage & static_cast<uint32_t>(BufferUsage::Vertex))
    {
        vkUsage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(BufferUsage::Index))
    {
        vkUsage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(BufferUsage::Uniform))
    {
        vkUsage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(BufferUsage::Storage))
    {
        vkUsage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(BufferUsage::Indirect))
    {
        vkUsage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress))
    {
        vkUsage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(BufferUsage::AccelerationStructureBuildInput))
    {
        // Callers gate on supportsRayQuery (Device.h contract); requesting this
        // on a device without VK_KHR_acceleration_structure enabled is a
        // programming error that validation reports at buffer creation.
        vkUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }
    if (desc.usage & static_cast<uint32_t>(BufferUsage::AccelerationStructureStorage))
    {
        // The omission of this branch shipped AS objects on buffers without
        // the storage usage bit (VUID 03614) — builds then wrote through BDA
        // into improperly-created storage, which the 21k-instance TLAS turned
        // into a reproducible VK_ERROR_DEVICE_LOST. Same supportsRayQuery
        // gate contract as the build-input bit above.
        vkUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    }

    // Default to storage buffer if no specific usage
    if (desc.usage == 0 || desc.usage == static_cast<uint32_t>(BufferUsage::None))
    {
        vkUsage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }

    // Descriptor-buffer SSBO/UBO descriptors are packed via vkGetBufferDeviceAddress,
    // which requires VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT on the source buffer
    // (VUID-VkBufferDeviceAddressInfo-buffer-02601). Add the bit on every buffer when
    // the runtime DB path is active — feature + VMA flag are already enabled in Phase 1d.
    if (IsDescriptorBufferEnabled())
    {
        vkUsage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        vulkanBuffer->usage = vulkanBuffer->usage | BufferUsage::ShaderDeviceAddress;
    }

    // Create VkBuffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = vulkanBuffer->size;
    bufferInfo.usage = vkUsage;
    // Prefer CONCURRENT sharing across available families to avoid ownership transfers in multi-queue
    uint32_t unique[3];
    uint32_t uniqueCount = 0;
    GetUniqueQueueFamilies(unique, uniqueCount);

    if (uniqueCount > 1)
    {
        bufferInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bufferInfo.queueFamilyIndexCount = uniqueCount;
        bufferInfo.pQueueFamilyIndices = unique;
    }
    else
    {
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

#ifdef RENDERING_HAS_VMA
    // Both residency inputs are write-once-at-init members of the surviving
    // physical device, so this runs unlocked on job threads (UploadMesh) and an
    // in-place rebuild cannot make it observe a half-populated device.
    const BufferResidencyPolicy residency = ResolveBufferResidencyPolicy(
        desc.memoryUsage, desc.usage,
        m_DeviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
        m_MemoryTopology);
    VmaAllocationCreateInfo vmaAllocInfo = {};
    vmaAllocInfo.usage = residency.Usage;
    vmaAllocInfo.flags = residency.Flags;
    if ((desc.flags & BufferCreateFlags::PersistentlyMapped) == BufferCreateFlags::PersistentlyMapped)
    {
        // Only allow persistent mapping on host-visible usages.
        // UploadDeviceLocalPreferred belongs here and not in the else: its
        // policy keeps HOST_VISIBLE required, so it is exactly as mappable as
        // Upload — dropping it through would silently un-map every mesh pool.
        if (desc.memoryUsage == BufferMemoryUsage::Upload ||
            desc.memoryUsage == BufferMemoryUsage::UploadDeviceLocalPreferred ||
            desc.memoryUsage == BufferMemoryUsage::Readback)
        {
            vmaAllocInfo.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }
        else
        {
            // Device-local or AUTO prefer-device: ignore persistent map and warn in debug builds
#if defined(DEBUG) || defined(_DEBUG)
            Logger::Log::Warning(
                "VulkanDevice: PersistentlyMapped ignored for non-host-visible buffer (memoryUsage={})",
                (int)desc.memoryUsage);
#endif
        }
    }

    VmaAllocation allocation;
    VmaAllocationInfo allocInfo{};
    VkResult result = vmaCreateBuffer(AsVmaAllocator(m_VmaAllocator), &bufferInfo, &vmaAllocInfo, &vulkanBuffer->buffer, &allocation, &allocInfo);
    vulkanBuffer->allocation = allocation;
    vulkanBuffer->memory = VK_NULL_HANDLE; // VMA handles memory

    if (result != VK_SUCCESS)
    {
        delete vulkanBuffer;
        return INVALID_BUFFER_HANDLE;
    }

    // Capture persistent mapped pointer if allocation was created with MAPPED flag.
    // VMA clears MAPPED itself when the resolved type is not host-visible, so
    // pMappedData is null in that case rather than a stale pointer.
    if ((vmaAllocInfo.flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) != 0)
    {
        vulkanBuffer->persistentMappedData = allocInfo.pMappedData;
    }
    else
    {
        vulkanBuffer->persistentMappedData = nullptr;
    }

    vmaGetAllocationMemoryProperties(AsVmaAllocator(m_VmaAllocator), allocation,
                                     &vulkanBuffer->resolvedMemoryProperties);
#else
    // Fallback to direct Vulkan buffer creation
    VkResult result = vkCreateBuffer(m_Device, &bufferInfo, nullptr, &vulkanBuffer->buffer);
    if (result == VK_SUCCESS)
    {
        // Allocate memory for the buffer
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(m_Device, vulkanBuffer->buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, vulkanBuffer->memoryProperties);

        result = vkAllocateMemory(m_Device, &allocInfo, nullptr, &vulkanBuffer->memory);
        if (result == VK_SUCCESS)
        {
            vkBindBufferMemory(m_Device, vulkanBuffer->buffer, vulkanBuffer->memory, 0);
            vulkanBuffer->allocation = nullptr; // No VMA allocation

            // Track this direct memory allocation
            TrackDirectMemoryAllocation(vulkanBuffer->memory);
        }
        else
        {
            vkDestroyBuffer(m_Device, vulkanBuffer->buffer, nullptr);
            delete vulkanBuffer;
            return INVALID_BUFFER_HANDLE;
        }
    }
    else
    {
        delete vulkanBuffer;
        return INVALID_BUFFER_HANDLE;
    }
#endif

    SetVkObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(vulkanBuffer->buffer),
                    vulkanBuffer->debugName.c_str());

    // Use global buffer manager and track live handle for safe shutdown (thread-safe)
    BufferHandle h = GameEngine::Rendering::CreateBuffer(vulkanBuffer);
    if (h.IsValid())
    {
        std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
        if (vulkanBuffer->allocation)
            m_VmaBufferAllocs[vulkanBuffer->allocation] = vulkanBuffer->buffer;
        m_LiveBuffers.push_back(h);
    }
    else
    {
        // Cleanup if handle creation failed
        // (Assume Vulkan resources are destroyed by caller if CreateBuffer returns invalid, but here we allocated the struct)
        // Actually we need to destroy the buffer if handle creation failed to avoid leak
        // But CreateBuffer currently just pushes to vector. If it returns Invalid, it means Manager logic failed (rare).
        // For now, leak prevention:
        // delete vulkanBuffer; // Careful, we need to destroy VK resources too.
        // Call helper to destroy buffer resources?
        // For now, assume valid handle.
    }
    if (h.IsValid() && m_FillNewTargetsWithNaN && (vkUsage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0)
        FillNewStorageBufferWithNaN(h, vulkanBuffer->size);
    return h;
}

uint32_t VulkanDevice::ResolveSupportedSampleCount(uint32_t requestedSamples, uint32_t usageFlags) const
{
    static constexpr uint32_t kMaxVulkanSampleCount = 64u;
    uint32_t normalizedRequested = requestedSamples > 0 ? requestedSamples : 1u;
    if (normalizedRequested > kMaxVulkanSampleCount)
        normalizedRequested = kMaxVulkanSampleCount;

    const VkSampleCountFlags colorSampleCounts = m_DeviceProperties.limits.framebufferColorSampleCounts;
    const VkSampleCountFlags depthSampleCounts = m_DeviceProperties.limits.framebufferDepthSampleCounts;

    VkSampleCountFlags supportedMask = colorSampleCounts & depthSampleCounts;
    const bool wantsColor = (usageFlags & static_cast<uint32_t>(TextureUsage::RenderTarget)) != 0u;
    const bool wantsDepth = (usageFlags & static_cast<uint32_t>(TextureUsage::DepthStencil)) != 0u;
    if (wantsColor && wantsDepth)
        supportedMask = colorSampleCounts & depthSampleCounts;
    else if (wantsColor)
        supportedMask = colorSampleCounts;
    else if (wantsDepth)
        supportedMask = depthSampleCounts;

    static constexpr uint32_t kSampleCandidates[] = {64u, 32u, 16u, 8u, 4u, 2u, 1u};
    for (const uint32_t candidate : kSampleCandidates)
    {
        if (candidate > normalizedRequested)
            continue;
        if ((supportedMask & static_cast<VkSampleCountFlags>(candidate)) != 0u)
            return candidate;
    }

    return 1u;
}

void VulkanDevice::LogSampleCountFallbackIfNeeded(uint32_t requestedSamples, uint32_t resolvedSamples, uint32_t usageFlags) const
{
    if (resolvedSamples == requestedSamples)
        return;

    const uint64_t key = (static_cast<uint64_t>(requestedSamples) << 32u)
        | (static_cast<uint64_t>(resolvedSamples) << 16u)
        | static_cast<uint64_t>(usageFlags & 0xFFFFu);

    {
        std::lock_guard<std::mutex> lock(m_SampleCountWarningMutex);
        if (!m_LoggedSampleCountFallbacks.insert(key).second)
            return;
    }

    Logger::Log::Warning(
        "VulkanDevice: unsupported sample count {} requested (usage=0x{:X}), falling back to {}.",
        requestedSamples, usageFlags, resolvedSamples);
}

void VulkanDevice::FillNewTargetWithNaN(TextureHandle texture, const TextureDesc& desc)
{
    const auto format = static_cast<TextureFormat>(desc.format);
    const bool floatColor = format == TextureFormat::R32G32B32A32_FLOAT || format == TextureFormat::R16G16B16A16_FLOAT ||
                            format == TextureFormat::R11G11B10_FLOAT || format == TextureFormat::R16_FLOAT ||
                            format == TextureFormat::R16G16_FLOAT || format == TextureFormat::R32_FLOAT ||
                            format == TextureFormat::R32G32_FLOAT;
    const uint32_t targetUsage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                                 static_cast<uint32_t>(TextureUsage::UnorderedAccess);
    if (!floatColor || (desc.usage & targetUsage) == 0)
        return;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float rgba[4] = {nan, nan, nan, nan};
    auto cl = CreateCommandList(QueueType::Graphics);
    cl->Begin();
    const uint32_t mips = desc.mipLevels > 0 ? desc.mipLevels : 1;
    const uint32_t layers = desc.arrayLayers > 0 ? desc.arrayLayers : 1;
    for (uint32_t mip = 0; mip < mips; ++mip)
        for (uint32_t layer = 0; layer < layers; ++layer)
            cl->ClearColorImageSubresource(texture, mip, layer, rgba);
    cl->End();
    CommandList* raw = cl.get();
    ExecuteCommandLists({raw});
}

void VulkanDevice::FillNewStorageBufferWithNaN(BufferHandle buffer, uint64_t sizeBytes)
{
    // 0x7FC0 is a quiet NaN as a half, so the pattern reads NaN as float32 and
    // as either float16 half of the word.
    constexpr uint32_t kNaNPattern = 0x7FC07FC0u;
    const uint64_t fillBytes = sizeBytes & ~uint64_t{3};
    if (fillBytes == 0)
        return;
    auto cl = CreateCommandList(QueueType::Graphics);
    cl->Begin();
    cl->FillBuffer(buffer, 0, static_cast<size_t>(fillBytes), kNaNPattern);
    cl->End();
    CommandList* raw = cl.get();
    // Waited, not just submitted: the caller's next write may come from the
    // host through a mapping or from another queue, and must land after the fill.
    WaitGpuSyncToken(ExecuteCommandListsTracked({raw}).graphics);
}

TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc)
{
    // Minimal validation (no excessive logging)
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0)
    {
        return INVALID_TEXTURE_HANDLE;
    }
    if (TextureFormatSupportGateRefuses(*this, desc))
    {
        return INVALID_TEXTURE_HANDLE;
    }

    // Reachable from streaming/upload job threads; vmaCreateImage must not
    // overlap an in-place rebuild's ShutdownVMA.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Create);

#ifdef RENDERING_HAS_VMA
    // No allocator after a FAILED rebuild (see CreateBuffer): fail the create
    // instead of handing vmaCreateImage a null allocator.
    if (m_VmaAllocator == nullptr)
    {
        return INVALID_TEXTURE_HANDLE;
    }
#endif

    const bool is3DTexture = desc.depth > 1;
    if (is3DTexture)
    {
        const char* name = desc.debugName ? desc.debugName : "<unnamed>";
        if (desc.arrayLayers > 1)
        {
            Logger::Log::Error("CreateTexture invalid 3D texture '{}': arrayLayers must be 1, got {}", name, desc.arrayLayers);
            return INVALID_TEXTURE_HANDLE;
        }
        if (desc.sampleCount > 1)
        {
            Logger::Log::Error("CreateTexture invalid 3D texture '{}': multisampling is not supported for 3D images", name);
            return INVALID_TEXTURE_HANDLE;
        }
        if ((desc.flags & TextureCreateFlags::CubeCompatible) != TextureCreateFlags::None)
        {
            Logger::Log::Error("CreateTexture invalid 3D texture '{}': cube-compatible flag only applies to 2D arrays", name);
            return INVALID_TEXTURE_HANDLE;
        }
        if (desc.usage & static_cast<uint32_t>(TextureUsage::DepthStencil))
        {
            Logger::Log::Error("CreateTexture invalid 3D texture '{}': DepthStencil usage is not valid for 3D images", name);
            return INVALID_TEXTURE_HANDLE;
        }
    }
    // Defensive: reject absurd sizes early to surface the culprit cleanly
    const uint32_t kMaxTexDim = 32768u;
    if (desc.width > kMaxTexDim || desc.height > kMaxTexDim || desc.depth > kMaxTexDim)
    {
        const char* name = desc.debugName ? desc.debugName : "<unnamed>";
        Logger::Log::Error("CreateTexture invalid size: '{}' {}x{}x{}, format={} ",
                           name,
                           desc.width,
                           desc.height,
                           desc.depth,
                           (int)desc.format);
        return INVALID_TEXTURE_HANDLE;
    }

    VulkanTexture* vulkanTexture = new VulkanTexture();
    vulkanTexture->device = m_Device;
    vulkanTexture->extent = {desc.width, desc.height, desc.depth > 0 ? desc.depth : 1};
    vulkanTexture->usage = static_cast<TextureUsage>(desc.usage);
    vulkanTexture->debugName = desc.debugName ? desc.debugName : "";
    vulkanTexture->trackedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vulkanTexture->arrayLayers = is3DTexture ? 1 : (desc.arrayLayers > 0 ? desc.arrayLayers : 1);
    vulkanTexture->mipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;
    vulkanTexture->sampledInGeneralLayout = desc.sampledInGeneralLayout;

    // Convert format using the shared VulkanResourceUtils mapping so that all
    // TextureFormat integer/float variants (including R32_UINT for star
    // accumulators) are handled consistently. desc.format is encoded using
    // the TextureFormat/Format enum layout.
    TextureFormat texFormat = static_cast<TextureFormat>(desc.format);
    VkFormat vkFormat = VulkanResourceUtils::GetVulkanFormat(texFormat);
    // Debug-time guard: if a new TextureFormat value has not been wired up in
    // VulkanResourceUtils::GetVulkanFormat we would previously have silently
    // fallen back to RGBA8. Assert here so developers fix the mapping
    // instead of getting a mismatched VkFormat at runtime.
    assert(vkFormat != VK_FORMAT_UNDEFINED &&
           "VulkanDevice::CreateTexture: unmapped TextureFormat; update VulkanResourceUtils::GetVulkanFormat");
    if (vkFormat == VK_FORMAT_UNDEFINED)
    {
        // Fallback for legacy/unknown formats: preserve previous behaviour
        // and avoid creating textures with VK_FORMAT_UNDEFINED.
        vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    }
    vulkanTexture->format = vkFormat;

    // Convert usage flags
    VkImageUsageFlags vkUsage = 0;
    if (desc.usage & static_cast<uint32_t>(TextureUsage::RenderTarget))
    {
        vkUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(TextureUsage::DepthStencil))
    {
        vkUsage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(TextureUsage::ShaderResource))
    {
        vkUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(TextureUsage::UnorderedAccess))
    {
        vkUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    // Explicit transfer usage flags
    if (desc.usage & static_cast<uint32_t>(TextureUsage::TransferSrc))
    {
        vkUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (desc.usage & static_cast<uint32_t>(TextureUsage::TransferDst))
    {
        vkUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    // Every image gets both transfer bits regardless of what it declared, which is
    // why TransferSrc/TransferDst are the only usage bits this backend does not
    // enforce. Strict mode maps exactly the declaration instead, turning an
    // undeclared copy into a validation error at the command. Not yet the default:
    // paths no platform has exercised would fail here first.
    if (!TextureUsagePolicy::IsStrict())
    {
        vkUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    // Default usage if none specified
    if (vkUsage == 0)
    {
        vkUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    const uint32_t requestedSampleCount = desc.sampleCount > 0 ? desc.sampleCount : 1u;
    const uint32_t resolvedSampleCount = ResolveSupportedSampleCount(requestedSampleCount, desc.usage);
    LogSampleCountFallbackIfNeeded(requestedSampleCount, resolvedSampleCount, desc.usage);

    // Create VkImage
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = is3DTexture ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = vulkanTexture->extent.width;
    imageInfo.extent.height = vulkanTexture->extent.height;
    imageInfo.extent.depth = vulkanTexture->extent.depth;
    imageInfo.mipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;
    imageInfo.arrayLayers = is3DTexture ? 1 : (desc.arrayLayers > 0 ? desc.arrayLayers : 1);
    imageInfo.format = vulkanTexture->format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = vkUsage;
    imageInfo.samples = static_cast<VkSampleCountFlagBits>(resolvedSampleCount);
    imageInfo.flags = 0;
    if ((desc.flags & TextureCreateFlags::CubeCompatible) != TextureCreateFlags::None)
    {
        imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    vulkanTexture->sampleCount = resolvedSampleCount;
    // Prefer CONCURRENT sharing across available families for images used across queues
    uint32_t uniqueImg[3];
    uint32_t uniqueImgCount = 0;
    GetUniqueQueueFamilies(uniqueImg, uniqueImgCount);

    if (uniqueImgCount > 1)
    {
        imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = uniqueImgCount;
        imageInfo.pQueueFamilyIndices = uniqueImg;
    }
    else
    {
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    // Track sample count on resource for later queries
    vulkanTexture->sampleCount = resolvedSampleCount;

    VkResult result;
    bool imageCreated = false;

#ifdef RENDERING_HAS_VMA
    // Use VMA for image creation
    VmaAllocationCreateInfo vmaAllocInfo = {};
    vmaAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; // Prefer device-local memory for images
    vmaAllocInfo.flags = 0;                                   // No special flags needed for images

    VmaAllocation allocation;
    result = vmaCreateImage(AsVmaAllocator(m_VmaAllocator), &imageInfo, &vmaAllocInfo, &vulkanTexture->image, &allocation, nullptr);
    if (result == VK_SUCCESS)
    {
        vulkanTexture->allocation = allocation;
        vulkanTexture->memory = VK_NULL_HANDLE; // VMA handles memory
        // Register raw VMA allocation defensively for shutdown sweep (thread-safe)
        {
            std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
            m_VmaImageAllocs[allocation] = vulkanTexture->image;
        }
        imageCreated = true;
    }
    else
    {
        Logger::Log::Error("VMA image creation failed! Error: {}", (int)result);
    }
#else
    // Fallback to direct Vulkan image creation
    result = vkCreateImage(m_Device, &imageInfo, nullptr, &vulkanTexture->image);
    if (result == VK_SUCCESS)
    {
        // Allocate memory for the image
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(m_Device, vulkanTexture->image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        result = vkAllocateMemory(m_Device, &allocInfo, nullptr, &vulkanTexture->memory);
        if (result == VK_SUCCESS)
        {
            vkBindImageMemory(m_Device, vulkanTexture->image, vulkanTexture->memory, 0);
            vulkanTexture->allocation = nullptr; // No VMA allocation

            // Track this direct memory allocation
            TrackDirectMemoryAllocation(vulkanTexture->memory);

            imageCreated = true;
        }
        else
        {
            Logger::Log::Error("Failed to allocate image memory! Error: {}", (int)result);
            vkDestroyImage(m_Device, vulkanTexture->image, nullptr);
            vulkanTexture->image = VK_NULL_HANDLE;
        }
    }
    else
    {
        Logger::Log::Error("Failed to create image! Error: {}", (int)result);
    }
#endif

    if (imageCreated)
    {
        SetVkObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(vulkanTexture->image),
                        vulkanTexture->debugName.c_str());
    }

    // Create image view if image was created successfully
    if (imageCreated && vulkanTexture->image != VK_NULL_HANDLE)
    {
        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = vulkanTexture->image;

        // Choose a sensible default view type based on layer count and cube compatibility.
        const bool isCubeCompatible = (imageInfo.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0;
        const bool isExactCube = isCubeCompatible && imageInfo.arrayLayers == 6;
        const bool isCubeArray = isCubeCompatible && imageInfo.arrayLayers > 6 && (imageInfo.arrayLayers % 6) == 0;

        if (imageInfo.imageType == VK_IMAGE_TYPE_3D)
        {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        }
        else if (isExactCube)
        {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        }
        else if (isCubeArray)
        {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        }
        else if (imageInfo.arrayLayers > 1 ||
                 (desc.flags & TextureCreateFlags::ForceArrayView) != TextureCreateFlags::None)
        {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        }
        else
        {
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        }

        viewInfo.format = vulkanTexture->format;

        // Set correct aspect mask based on format.
        //
        // IMPORTANT:
        // For depth-stencil formats, using a view with BOTH depth+stencil aspects is not valid
        // for descriptor sampling on Vulkan (it triggers VUID-VkDescriptorImageInfo-imageView-01976).
        //
        // We default to a DEPTH-only view for depth-stencil images. If/when we need stencil sampling
        // or stencil attachments explicitly, we should create a separate view with STENCIL-only
        // aspectMask (and/or a depth-stencil attachment view as needed).
        switch (vulkanTexture->format)
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        default:
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            break;
        }

        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = imageInfo.mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        // For cube views, Vulkan requires layerCount to be exactly 6 per cube.
        if (isExactCube)
        {
            viewInfo.subresourceRange.layerCount = 6;
        }
        else
        {
            viewInfo.subresourceRange.layerCount = imageInfo.arrayLayers;
        }

        result = vkCreateImageView(m_Device, &viewInfo, nullptr, &vulkanTexture->view);
        if (result == VK_SUCCESS)
        {
            if (!vulkanTexture->debugName.empty())
            {
                const std::string viewName = vulkanTexture->debugName + "_view";
                SetVkObjectName(VK_OBJECT_TYPE_IMAGE_VIEW,
                                reinterpret_cast<uint64_t>(vulkanTexture->view), viewName.c_str());
            }
            WatchImageIfNamed(vulkanTexture->debugName.c_str(), vulkanTexture->image);
            WatchViewIfOnWatchedImage(vulkanTexture->image, vulkanTexture->view,
                                      viewInfo.subresourceRange);
        }
        else
        {
            Logger::Log::Error("Failed to create image view! Error: {}", (int)result);
#ifdef RENDERING_HAS_VMA
            if (vulkanTexture->allocation != nullptr)
            {
                vmaDestroyImage(AsVmaAllocator(m_VmaAllocator), vulkanTexture->image, vulkanTexture->allocation);
            }
            else
            {
                vkDestroyImage(m_Device, vulkanTexture->image, nullptr);
            }
#else
            if (vulkanTexture->memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_Device, vulkanTexture->memory, nullptr);
            }
            vkDestroyImage(m_Device, vulkanTexture->image, nullptr);
#endif
            vulkanTexture->image = VK_NULL_HANDLE;
        }
    }

    if (vulkanTexture->image == VK_NULL_HANDLE)
    {
        if (std::getenv("GE_DEBUG_TRACE_RENDERING"))
        {
            const char* name = desc.debugName ? desc.debugName : "<unnamed>";
            Logger::Log::Debug(
                "[VK] CreateTexture FAILED (no image) name='{}' {}x{} fmt={} usageBits={}",
                name,
                desc.width,
                desc.height,
                (int)desc.format,
                (uint32_t)desc.usage);
        }
        delete vulkanTexture;
        return INVALID_TEXTURE_HANDLE;
    }

    // Use global texture manager and track live handle for safe shutdown
    TextureHandle h = GameEngine::Rendering::CreateTexture(vulkanTexture);
    if (std::getenv("GE_DEBUG_TRACE_RENDERING"))
    {
        const char* name = desc.debugName ? desc.debugName : "<unnamed>";
        Logger::Log::Debug(
            "[VK] CreateTexture {} name='{}' {}x{} fmt={} usageBits={} handle={} image={}",
            h.IsValid() ? "OK" : "FAIL",
            name,
            desc.width,
            desc.height,
            (int)desc.format,
            (uint32_t)desc.usage,
            (uint64_t)h,
            (const void*)vulkanTexture->image);
    }
    if (h.IsValid())
    {
        {
            std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
            m_LiveTextures.push_back(h);
        }

        if (m_FillNewTargetsWithNaN)
            FillNewTargetWithNaN(h, desc);

        // If caller requested a non-Undefined initial state, transition now via a
        // one-shot command list (same pattern as TextureUploadHelpers).
        if (desc.initialState != ResourceState::Undefined)
        {
            auto cl = CreateCommandList(QueueType::Graphics);
            cl->Begin();
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(
                h,
                m_FillNewTargetsWithNaN ? ResourceState::CopyDest : ResourceState::Undefined,
                desc.initialState,
                0, desc.mipLevels > 0 ? desc.mipLevels : 1,
                0, desc.arrayLayers > 0 ? desc.arrayLayers : 1));
            cl->End();
            CommandList* raw = cl.get();
            ExecuteCommandLists({raw});
        }
    }
    else
    {
        delete vulkanTexture;
    }
    return h;
}

SamplerHandle VulkanDevice::CreateSampler(const SamplerDesc& desc)
{
    // Reachable from ECS extraction workers (TextureService bindless
    // registration); vkCreateSampler must not overlap a rebuild's teardown.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Create);

    // Create VkSampler with provided parameters
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

    // Convert filter modes
    samplerInfo.magFilter = (desc.magFilter == 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerInfo.minFilter = (desc.minFilter == 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = (desc.mipFilter == 1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

    // Convert address modes
    auto convertAddressMode = [](uint32_t mode) -> VkSamplerAddressMode
    {
        switch (mode)
        {
        case 0:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    };

    samplerInfo.addressModeU = convertAddressMode(desc.addressModeU);
    samplerInfo.addressModeV = convertAddressMode(desc.addressModeV);
    samplerInfo.addressModeW = convertAddressMode(desc.addressModeW);

    // Anisotropy. The request must already sit inside the device limit
    // (ResolveSamplerPreset clamps through RendererProfile); past it the sampler
    // is invalid, and with the feature absent the limit is 1.
    assert(desc.maxAnisotropy >= 1.0f && desc.maxAnisotropy <= m_Capabilities.maxSamplerAnisotropy &&
           "SamplerDesc::maxAnisotropy outside [1, maxSamplerAnisotropy]");
    samplerInfo.anisotropyEnable = (desc.maxAnisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = desc.maxAnisotropy;

    // LOD settings
    samplerInfo.mipLodBias = desc.mipLodBias;
    samplerInfo.minLod = desc.minLod;
    samplerInfo.maxLod = desc.maxLod;

    // Border color and compare sampling
    samplerInfo.borderColor = (desc.borderColor == 1) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE;
    samplerInfo.compareOp = desc.compareEnable ? VulkanMappings::TranslateCompareOp(desc.compareOp) : VK_COMPARE_OP_ALWAYS;

    VkSampler sampler;
    VkResult result = vkCreateSampler(m_Device, &samplerInfo, nullptr, &sampler);
    if (result != VK_SUCCESS)
    {
        return INVALID_SAMPLER_HANDLE;
    }
    SetVkObjectName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(sampler), desc.debugName);

    // Store in global sampler manager and record owning VkDevice (thread-safe tracking).
    SamplerHandle h = GameEngine::Rendering::CreateSampler(std::move(sampler));
    RegisterSamplerOwner(h, m_Device);
    {
        std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
        m_LiveSamplers.push_back(h);
    }
    return h;
}

PipelineHandle VulkanDevice::CreateConcreteGraphicsPipeline(
    const GraphicsPipelineDesc& gd, const PipelineFormatKey& fk)
{
    // The existing creation core consumes a `PipelineDesc`. Translate via
    // the cache's helper, then forward. Future cleanup can rewrite the
    // creation core to consume `GraphicsPipelineDesc` directly; until then,
    // this translation lives here as a Vulkan-backend implementation
    // detail and stays out of the cross-cutting IDevice surface.
    PipelineDesc backendDesc = PipelineCache::BuildPipelineDescForGraphics(gd, fk, GetPipelineCache());
    return CreatePipelineInternal(backendDesc);
}

PipelineHandle VulkanDevice::CreateConcreteComputePipeline(const ComputePipelineDesc& cd)
{
    PipelineDesc backendDesc = PipelineCache::BuildPipelineDescForCompute(cd, GetPipelineCache());
    return CreatePipelineInternal(backendDesc);
}

PipelineHandle VulkanDevice::CreatePipelineInternal(const PipelineDesc& desc)
{

    VulkanPipeline vulkanPipeline{};
    vulkanPipeline.device = m_Device; // Track device for proper cleanup
    if (desc.debugName && desc.debugName[0] != '\0')
    {
        vulkanPipeline.debugName = desc.debugName;
    }
    else
    {
        vulkanPipeline.debugName = "PipelineAnon";
    }

    // Handle different pipeline types
    if (desc.type == PipelineType::Compute)
    {
        // Check if we have compute shader data
        if (desc.computeShader.empty())
        {
            return INVALID_PIPELINE_HANDLE;
        }

        PipelineDesc reflected = desc;
        // Higher layers should have applied ShaderMeta; if not provided, we proceed with what we have
        // Enforce push constant policy
        if (reflected.pushConstantSize && reflected.pushConstantSize > m_MaxPushConstantBytes)
        {
            return INVALID_PIPELINE_HANDLE;
        }
        auto computePipelineResult = CreateComputePipelineFromDesc(reflected);

        if (computePipelineResult.pipeline != VK_NULL_HANDLE)
        {
            vulkanPipeline.pipeline = computePipelineResult.pipeline;
            vulkanPipeline.layout = computePipelineResult.layout;
            vulkanPipeline.renderPass = VK_NULL_HANDLE;  // Compute pipelines don't use render passes
            vulkanPipeline.type = PipelineType::Compute; // Store pipeline type
            vulkanPipeline.descriptorSetLayout = computePipelineResult.descriptorSetLayout;
            // Store push constant layout for typed push constants (use effective size/mask).
            // Compute pipelines can only carry VK_SHADER_STAGE_COMPUTE_BIT in any push range;
            // default the mask accordingly so the metadata matches the actual layout built
            // above in CreateComputePipelineFromDesc.
            uint32_t effectivePcSize = reflected.pushConstantSize ? reflected.pushConstantSize : m_MaxPushConstantBytes;
            uint32_t effectivePcStages = reflected.pushConstantStagesMask
                                             ? reflected.pushConstantStagesMask
                                             : static_cast<uint32_t>(VK_SHADER_STAGE_COMPUTE_BIT);
            vulkanPipeline.pushConstantSize = effectivePcSize;
            vulkanPipeline.pushConstantStagesMask = effectivePcStages;
            // The compute layout carries a COMPUTE_BIT range iff any push
            // constants were declared (enforceComputeStage clamps the flags).
            vulkanPipeline.layoutPushStageFlags =
                (!reflected.pushConstantRanges.empty() || reflected.pushConstantSize)
                    ? static_cast<uint32_t>(VK_SHADER_STAGE_COMPUTE_BIT)
                    : 0u;

            // Preserve per-range metadata for by-name API
            vulkanPipeline.pushRanges.clear();
            if (!reflected.pushConstantRanges.empty())
            {
                uint32_t runningOffset = 0;
                for (const auto& pr : reflected.pushConstantRanges)
                {
                    VulkanPipeline::PushRange vr{};
                    runningOffset = (runningOffset + 3u) & ~3u; // align 4
                    vr.name = pr.name;
                    vr.size = pr.size;
                    vr.stagesMask = pr.stagesMask;
                    vr.offset = runningOffset;
                    runningOffset += ((pr.size + 3u) & ~3u);
                    vulkanPipeline.pushRanges.push_back(std::move(vr));
                }
            }
            else if (effectivePcSize)
            {
                VulkanPipeline::PushRange vr{};
                vr.name = "__default";
                vr.offset = 0;
                vr.size = effectivePcSize;
                vr.stagesMask = effectivePcStages;
                vulkanPipeline.pushRanges.push_back(vr);
            }

            // Use global pipeline manager (much cleaner)
            VulkanPipeline* pipelinePtr = new VulkanPipeline(std::move(vulkanPipeline));
            PipelineHandle h = GameEngine::Rendering::CreatePipeline(pipelinePtr);
#ifndef NDEBUG
            if (std::getenv("GE_DEBUG_TRACE_RENDERING"))
            {
                fprintf(stderr, "[VK] Created pipeline (compute) name='%s' handle=%llu\n",
                        pipelinePtr->debugName.c_str(), (unsigned long long)h);
            }
#endif
            if (!h.IsValid())
            {
                delete pipelinePtr;
            }
            return h;
        }
        else
        {
            return INVALID_PIPELINE_HANDLE;
        }
    }

    // Mesh pipeline creation (VK_EXT_mesh_shader)
    if (desc.type == PipelineType::Mesh)
    {
        if (!m_Capabilities.supportsMeshShaders || !m_SupportsMeshShaderEXT || desc.meshShader.empty())
        {
            return INVALID_PIPELINE_HANDLE;
        }

        // Prefer .shaderdesc to fill descriptor layouts and push constants if provided
        PipelineDesc reflected = desc;
        // Higher layers should have applied ShaderMeta; if not provided, we proceed with what we have
        if (reflected.pushConstantSize && reflected.pushConstantSize > m_MaxPushConstantBytes)
        {
            return INVALID_PIPELINE_HANDLE;
        }

        // Create shader modules
        VkShaderModule meshModule = CreateShaderModuleFromBytes(reflected.meshShader);
        VkShaderModule fragModule = reflected.pixelShader.empty() ? VK_NULL_HANDLE : CreateShaderModuleFromBytes(reflected.pixelShader);
        if (meshModule == VK_NULL_HANDLE)
        {
            if (fragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(m_Device, fragModule, nullptr);
            return INVALID_PIPELINE_HANDLE;
        }

        // Descriptor set layouts
        std::vector<VkDescriptorSetLayout> vkLayouts;
        for (const auto& layoutDesc : reflected.descriptorSetLayouts)
        {
            uint64_t key = 0;
            VkDescriptorSetLayout vkLayout = GetOrCreateDescriptorSetLayoutCached(layoutDesc, key);
            if (vkLayout == VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(m_Device, meshModule, nullptr);
                if (fragModule != VK_NULL_HANDLE)
                    vkDestroyShaderModule(m_Device, fragModule, nullptr);
                return INVALID_PIPELINE_HANDLE;
            }
            vkLayouts.push_back(vkLayout);
        }

        // Push constants
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = reflected.pushConstantStagesMask
                                           ? static_cast<VkShaderStageFlags>(reflected.pushConstantStagesMask)
                                           : static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_ALL);
        pushConstantRange.offset = 0;
        pushConstantRange.size = reflected.pushConstantSize ? reflected.pushConstantSize : m_MaxPushConstantBytes;

        uint64_t plKey = 0;
        std::vector<VkPushConstantRange> ranges;
        if (pushConstantRange.size)
            ranges.push_back(pushConstantRange);
        VkPipelineLayout pipelineLayout = GetOrCreatePipelineLayoutCached(vkLayouts, ranges, plKey);
        if (pipelineLayout == VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(m_Device, meshModule, nullptr);
            if (fragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(m_Device, fragModule, nullptr);
            return INVALID_PIPELINE_HANDLE;
        }

        // Stages
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        VkPipelineShaderStageCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ms.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        ms.module = meshModule;
        ms.pName = "main";
        stages.push_back(ms);
        if (fragModule)
        {
            VkPipelineShaderStageCreateInfo fs{};
            fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fs.module = fragModule;
            fs.pName = "main";
            stages.push_back(fs);
        }

        // Fixed function (most states still apply)
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = static_cast<VkPrimitiveTopology>(desc.topology);
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = nullptr;
        viewportState.scissorCount = 1;
        viewportState.pScissors = nullptr;
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VulkanMappings::TranslatePolygonMode(desc.rasterizationState.polygonMode);
        rasterizer.cullMode = VulkanMappings::TranslateCullMode(desc.rasterizationState.cullMode);
        rasterizer.frontFace = VulkanMappings::TranslateFrontFace(desc.rasterizationState.frontFace);
        rasterizer.lineWidth = desc.rasterizationState.lineWidth;
        const uint32_t requestedRasterSamples = desc.rasterizationSamples ? desc.rasterizationSamples : 1u;
        const uint32_t resolvedRasterSamples =
            ResolveSupportedSampleCount(requestedRasterSamples, 0u);
        LogSampleCountFallbackIfNeeded(requestedRasterSamples, resolvedRasterSamples, 0u);
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = static_cast<VkSampleCountFlagBits>(resolvedRasterSamples);
        multisampling.alphaToCoverageEnable =
            desc.colorBlendState.alphaToCoverageEnable ? VK_TRUE : VK_FALSE;
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = desc.depthStencilState.depthTestEnable ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = desc.depthStencilState.depthWriteEnable ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = VulkanMappings::TranslateCompareOp(desc.depthStencilState.depthCompareOp);
        depthStencil.depthBoundsTestEnable = desc.depthStencilState.depthBoundsTestEnable ? VK_TRUE : VK_FALSE;
        depthStencil.stencilTestEnable = desc.depthStencilState.stencilTestEnable ? VK_TRUE : VK_FALSE;
        depthStencil.minDepthBounds = desc.depthStencilState.minDepthBounds;
        depthStencil.maxDepthBounds = desc.depthStencilState.maxDepthBounds;
        // Color blending - build attachments from structured state
        std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
        if (!desc.colorBlendState.attachments.empty())
        {
            for (const auto& attachment : desc.colorBlendState.attachments)
            {
                VkPipelineColorBlendAttachmentState a{};
                a.colorWriteMask = attachment.colorWriteMask;
                a.blendEnable = attachment.blendEnable ? VK_TRUE : VK_FALSE;
                if (attachment.blendEnable)
                {
                    a.srcColorBlendFactor = VulkanMappings::TranslateBlendFactor(attachment.srcColorBlendFactor);
                    a.dstColorBlendFactor = VulkanMappings::TranslateBlendFactor(attachment.dstColorBlendFactor);
                    a.colorBlendOp = VulkanMappings::TranslateBlendOp(attachment.colorBlendOp);
                    a.srcAlphaBlendFactor = VulkanMappings::TranslateBlendFactor(attachment.srcAlphaBlendFactor);
                    a.dstAlphaBlendFactor = VulkanMappings::TranslateBlendFactor(attachment.dstAlphaBlendFactor);
                    a.alphaBlendOp = VulkanMappings::TranslateBlendOp(attachment.alphaBlendOp);
                }
                colorBlendAttachments.push_back(a);
            }
        }
        if (colorBlendAttachments.empty())
        {
            uint32_t colorCountHint = desc.colorAttachmentFormats.empty() ? 1u : static_cast<uint32_t>(desc.colorAttachmentFormats.size());
            for (uint32_t i = 0; i < colorCountHint; ++i)
            {
                VkPipelineColorBlendAttachmentState a{};
                a.colorWriteMask = 0xF;
                a.blendEnable = VK_FALSE;
                colorBlendAttachments.push_back(a);
            }
        }
        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = (uint32_t)colorBlendAttachments.size();
        colorBlending.pAttachments = colorBlendAttachments.data();
        std::vector<VkDynamicState> dynamicStates;
        for (auto ds : desc.dynamicState.states)
            dynamicStates.push_back(static_cast<VkDynamicState>(ds));
        // Ensure viewport/scissor are dynamic to avoid hard-coded states
        auto ensureDyn = [&](VkDynamicState s)
        { if (std::find(dynamicStates.begin(), dynamicStates.end(), s) == dynamicStates.end()) dynamicStates.push_back(s); };
        ensureDyn(VK_DYNAMIC_STATE_VIEWPORT);
        ensureDyn(VK_DYNAMIC_STATE_SCISSOR);
        // Allow depth write to be toggled dynamically so passes with read-only depth
        // attachments can disable writes without needing separate pipeline variants.
        if (m_DeviceProperties.apiVersion >= VK_API_VERSION_1_3)
            ensureDyn(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE);
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = (uint32_t)dynamicStates.size();
        dynamicState.pDynamicStates = dynamicStates.empty() ? nullptr : dynamicStates.data();

        // Dynamic rendering
        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        std::vector<VkFormat> safeColorFormats; // Temp storage to avoid strict aliasing violation
        if (!desc.colorAttachmentFormats.empty())
        {
            safeColorFormats.reserve(desc.colorAttachmentFormats.size());
            for (uint32_t fmt : desc.colorAttachmentFormats)
                safeColorFormats.push_back(static_cast<VkFormat>(fmt));
            renderingInfo.colorAttachmentCount = static_cast<uint32_t>(safeColorFormats.size());
            renderingInfo.pColorAttachmentFormats = safeColorFormats.data();
        }
        if (desc.depthAttachmentFormat)
            renderingInfo.depthAttachmentFormat = static_cast<VkFormat>(desc.depthAttachmentFormat);

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = (uint32_t)stages.size();
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInputInfo; // unused by mesh
        pipelineInfo.pInputAssemblyState = &inputAssembly; // safe default
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = (dynamicState.dynamicStateCount ? &dynamicState : nullptr);
        pipelineInfo.layout = pipelineLayout;
        // Ensure dynamic rendering is actually activated for this pipeline
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
        pipelineInfo.subpass = 0;
        if (m_EnableDynamicRendering && m_SupportsDynamicRendering)
        {
            pipelineInfo.pNext = &renderingInfo;
        }
        // VK_EXT_descriptor_buffer: if any bound set layout is DB-eligible, the pipeline
        // must be created with the DB flag to accept vkCmdSetDescriptorBufferOffsetsEXT
        // draws (VUID-08115/08117). No-op when the runtime path is off.
        if (AnySetLayoutIsDescriptorBufferEligible(vkLayouts))
            pipelineInfo.flags |= VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;

        VkPipeline pipeline{};
        PIPELINE_CREATION_TIMER(m_VkDiskPipelineCache.get(), false);
        VkResult res;
        {
            auto cacheLock = m_VkDiskPipelineCache ? m_VkDiskPipelineCache->LockForCreation() : std::unique_lock<std::mutex>();
            VkPipelineCache pc = m_VkDiskPipelineCache ? m_VkDiskPipelineCache->GetVkPipelineCache() : VK_NULL_HANDLE;
            res = vkCreateGraphicsPipelines(m_Device, pc, 1, &pipelineInfo, nullptr, &pipeline);
        }
        // Cleanup shader modules
        vkDestroyShaderModule(m_Device, meshModule, nullptr);
        if (fragModule)
            vkDestroyShaderModule(m_Device, fragModule, nullptr);
        if (res != VK_SUCCESS)
        {
            return INVALID_PIPELINE_HANDLE;
        }
        SetVkObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline),
                        vulkanPipeline.debugName.c_str());

        vulkanPipeline.pipeline = pipeline;
        vulkanPipeline.layout = pipelineLayout;
        vulkanPipeline.renderPass = m_SwapchainRenderPass; // may be null when dynamic rendering used
        vulkanPipeline.type = PipelineType::Mesh;
        vulkanPipeline.depthWriteEnable = desc.depthStencilState.depthWriteEnable;
        vulkanPipeline.pushConstantSize = desc.pushConstantSize;
        vulkanPipeline.pushConstantStagesMask = desc.pushConstantStagesMask
                                                    ? desc.pushConstantStagesMask
                                                    : static_cast<uint32_t>(VK_SHADER_STAGE_ALL);
        // Mirrors the mesh layout's range construction above (reflected mask or
        // ALL; the range exists whenever its size resolved non-zero).
        vulkanPipeline.layoutPushStageFlags =
            (desc.pushConstantSize || m_MaxPushConstantBytes)
                ? vulkanPipeline.pushConstantStagesMask
                : 0u;
        VulkanPipeline* pipelinePtr = new VulkanPipeline(std::move(vulkanPipeline));
        return GameEngine::Rendering::CreatePipeline(pipelinePtr);
    }

    // Graphics pipeline creation (existing logic)
    // Decide if we can use dynamic rendering (explicit formats or feature enabled)
    const bool canUseDynamicPath = (!desc.colorAttachmentFormats.empty()) || (m_SupportsDynamicRendering && m_EnableDynamicRendering);
    // Only require/create a swapchain render pass when we cannot use the dynamic rendering path

    if (!canUseDynamicPath)
    {
        if (m_SwapchainRenderPass == VK_NULL_HANDLE)
        {
            GetOrCreateSwapchainRenderPass();
            if (m_SwapchainRenderPass == VK_NULL_HANDLE)
            {
                return INVALID_PIPELINE_HANDLE;
            }
        }
    }
    // Conservative dev warning for dynamic rendering without explicit color formats
    // Suppress this when the pipeline is depth-only (depthAttachmentFormat set)
    if (m_EnableDynamicRendering && m_SupportsDynamicRendering && desc.colorAttachmentFormats.empty() && desc.depthAttachmentFormat == 0)
    {
        VK_DBG(Logger::Log::Warning(
            "Dynamic rendering is enabled but PipelineDesc.colorAttachmentFormats is empty. Provide explicit formats to ensure correct validation (especially for offscreen targets)."));
    }

    // Check if we have shader data for graphics pipeline
    // Allow depth-only pipelines (no pixel shader) when there are no color attachments and a valid depth format is provided
    const bool depthOnlyPipeline = (!desc.vertexShader.empty()) && desc.pixelShader.empty() && desc.colorAttachmentFormats.empty() && desc.depthAttachmentFormat != 0;
    if (desc.vertexShader.empty() || (!depthOnlyPipeline && desc.pixelShader.empty()))
    {
        // Fail fast: incomplete shader stages are not allowed (except for valid depth-only pipelines)
        return INVALID_PIPELINE_HANDLE;
    }

// If reflection is enabled and descriptor layouts are missing, build them from shaders via MaterialBuilder
#if RENDERING_ENABLE_SPIRV_REFLECTION
    if (desc.descriptorSetLayouts.empty())
    {
        // Prefer metadata already applied by higher layers; fallback to on-the-fly reflection only
        PipelineDesc reflected = desc;
        ShaderMeta meta{};
        bool haveMeta = false;
        // If higher layers passed computed push constant size or set layouts we use them; otherwise reflect
        if (reflected.descriptorSetLayouts.empty())
        {
            std::vector<StageReflectionResult> stages;
            if (!desc.vertexShader.empty())
            {
                stages.emplace_back();
                auto& vs = stages.back();
                ReflectSpirv(ShaderStageKind::Vertex, reinterpret_cast<const uint32_t*>(desc.vertexShader.data()), desc.vertexShader.size() / 4, {}, vs, nullptr);
            }
            if (!desc.pixelShader.empty())
            {
                stages.emplace_back();
                auto& fs = stages.back();
                ReflectSpirv(ShaderStageKind::Fragment, reinterpret_cast<const uint32_t*>(desc.pixelShader.data()), desc.pixelShader.size() / 4, {}, fs, nullptr);
            }
            meta = MergeStages(stages);
            haveMeta = true;
        }

        uint32_t pcSizeFromMeta = 0, pcStagesFromMeta = 0;
        if (haveMeta)
        {
            std::vector<DescriptorSetLayoutDesc> setLayouts;
            MaterialBuilder::BuildPipelineLayoutInputs(meta, setLayouts, pcSizeFromMeta, pcStagesFromMeta);
            reflected.descriptorSetLayouts = std::move(setLayouts);
            reflected.pushConstantSize = desc.pushConstantSize ? desc.pushConstantSize : pcSizeFromMeta;
            reflected.pushConstantStagesMask = desc.pushConstantStagesMask ? desc.pushConstantStagesMask : pcStagesFromMeta;
        }
        // Enforce push constants policy: prefer explicit; otherwise from meta
        reflected.pushConstantSize = desc.pushConstantSize ? desc.pushConstantSize : (haveMeta ? pcSizeFromMeta : reflected.pushConstantSize);
        reflected.pushConstantStagesMask = desc.pushConstantStagesMask ? desc.pushConstantStagesMask : (haveMeta ? pcStagesFromMeta : reflected.pushConstantStagesMask);
        // Policy: enforce DeviceDesc-configured limit; fail fast before creating modules
        if (desc.pushConstantSize && reflected.pushConstantSize > m_MaxPushConstantBytes)
        {
            return INVALID_PIPELINE_HANDLE;
        }
        // Use reflected desc for creation
        VkShaderModule vsm = CreateShaderModuleFromBytes(reflected.vertexShader);
        VkShaderModule fsm = CreateShaderModuleFromBytes(reflected.pixelShader);
        auto pipelineResult = CreateVulkanPipelineFromDesc(reflected, vsm, fsm);
        if (vsm != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_Device, vsm, nullptr);
        if (fsm != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_Device, fsm, nullptr);
        if (pipelineResult.pipeline != VK_NULL_HANDLE)
        {
            vulkanPipeline.pipeline = pipelineResult.pipeline;
            vulkanPipeline.layout = pipelineResult.layout;
            vulkanPipeline.descriptorSetLayout = pipelineResult.descriptorSetLayout;
            vulkanPipeline.renderPass = m_SwapchainRenderPass;
            vulkanPipeline.type = PipelineType::Graphics;
            vulkanPipeline.depthWriteEnable = reflected.depthStencilState.depthWriteEnable;
            vulkanPipeline.rasterizationSamples = ResolveSupportedSampleCount(
                reflected.rasterizationSamples ? reflected.rasterizationSamples : 1u, 0u);
            uint32_t declaredPcSize = reflected.pushConstantSize;
            vulkanPipeline.pushConstantSize = declaredPcSize;
            // Stage metadata is canonical for classic graphics pipelines: the
            // layout's single range is kGraphicsPushConstantStages, and
            // vkCmdPushConstants must use exactly those flags (sizes/names
            // stay reflected for policy checks and the by-name API).
            vulkanPipeline.pushConstantStagesMask =
                static_cast<uint32_t>(kGraphicsPushConstantStages);
            vulkanPipeline.layoutPushStageFlags =
                (m_MaxPushConstantBytes & ~3u)
                    ? static_cast<uint32_t>(kGraphicsPushConstantStages)
                    : 0u;
            // Preserve per-range metadata for by-name API (graphics)
            vulkanPipeline.pushRanges.clear();
            if (!reflected.pushConstantRanges.empty())
            {
                uint32_t runningOffset = 0;
                for (const auto& pr : reflected.pushConstantRanges)
                {
                    VulkanPipeline::PushRange vr{};
                    runningOffset = (runningOffset + 3u) & ~3u;
                    vr.name = pr.name;
                    vr.size = pr.size;
                    vr.stagesMask = static_cast<uint32_t>(kGraphicsPushConstantStages);
                    vr.offset = runningOffset;
                    runningOffset += ((pr.size + 3u) & ~3u);
                    vulkanPipeline.pushRanges.push_back(std::move(vr));
                }
            }
            else if (declaredPcSize)
            {
                VulkanPipeline::PushRange vr{};
                vr.name = "__default";
                vr.offset = 0;
                vr.size = declaredPcSize;
                vr.stagesMask = static_cast<uint32_t>(kGraphicsPushConstantStages);
                vulkanPipeline.pushRanges.push_back(vr);
            }

            // Propagate expected color attachment formats from PipelineDesc for runtime validation
            vulkanPipeline.colorAttachmentFormats.clear();
            if (!desc.colorAttachmentFormats.empty())
            {
                vulkanPipeline.colorAttachmentFormats.reserve(desc.colorAttachmentFormats.size());
                for (auto v : desc.colorAttachmentFormats)
                {
                    VkFormat f = VulkanResourceUtils::GetVulkanFormat(static_cast<TextureFormat>(v));
                    vulkanPipeline.colorAttachmentFormats.push_back(f == VK_FORMAT_UNDEFINED ? static_cast<VkFormat>(v) : f);
                }
            }
            VulkanPipeline* pipelinePtr = new VulkanPipeline(std::move(vulkanPipeline));
            PipelineHandle h = GameEngine::Rendering::CreatePipeline(pipelinePtr);
#ifndef NDEBUG
            // Print color formats for diagnostics
            if (std::getenv("GE_DEBUG_TRACE_RENDERING"))
            {
                fprintf(stderr, "[VK] Created pipeline (gfx) name='%s' handle=%llu formats=[",
                        pipelinePtr->debugName.c_str(), (unsigned long long)h);
                for (size_t i = 0; i < vulkanPipeline.colorAttachmentFormats.size(); ++i)
                {
                    if (i)
                        fputs(", ", stderr);
                    fprintf(stderr, "%u", (unsigned)vulkanPipeline.colorAttachmentFormats[i]);
                }
                fputs("]\n", stderr);
            }
#endif
            return h;
        }
    }
#endif

    // Propagate expected color attachment formats from PipelineDesc for runtime validation
    vulkanPipeline.colorAttachmentFormats.clear();
    if (!desc.colorAttachmentFormats.empty())
    {
        vulkanPipeline.colorAttachmentFormats.reserve(desc.colorAttachmentFormats.size());
        for (auto v : desc.colorAttachmentFormats)
        {
            VkFormat f = VulkanResourceUtils::GetVulkanFormat(static_cast<TextureFormat>(v));
            vulkanPipeline.colorAttachmentFormats.push_back(f == VK_FORMAT_UNDEFINED ? static_cast<VkFormat>(v) : f);
        }
    }

    // Create shader modules from provided SPIR-V data
    VkShaderModule vertShaderModule = CreateShaderModuleFromBytes(desc.vertexShader);
    VkShaderModule fragShaderModule = VK_NULL_HANDLE;
    if (!desc.pixelShader.empty())
    {
        fragShaderModule = CreateShaderModuleFromBytes(desc.pixelShader);
    }

    if (vertShaderModule == VK_NULL_HANDLE || (!desc.pixelShader.empty() && fragShaderModule == VK_NULL_HANDLE))
    {
        Logger::Log::Error("CreatePipeline: Shader module creation failed (VS={}, FS={})",
                           vertShaderModule != VK_NULL_HANDLE,
                           fragShaderModule != VK_NULL_HANDLE);
        if (vertShaderModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_Device, vertShaderModule, nullptr);
        if (fragShaderModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_Device, fragShaderModule, nullptr);
        return INVALID_PIPELINE_HANDLE;
    }

    // Developer hint: when using dynamic rendering, provide colorAttachmentFormats to match actual render targets
    // Suppress this when the pipeline is depth-only (depthAttachmentFormat set)
    if (m_EnableDynamicRendering && m_SupportsDynamicRendering && desc.colorAttachmentFormats.empty() && desc.depthAttachmentFormat == 0)
    {
        VK_DBG(Logger::Log::Warning(
            "Dynamic rendering is enabled but PipelineDesc.colorAttachmentFormats is empty. Provide explicit formats to ensure correct validation when rendering offscreen."));
    }

    // Create the actual Vulkan pipeline using structured description
    auto pipelineResult = CreateVulkanPipelineFromDesc(desc, vertShaderModule, fragShaderModule);

    // Cleanup shader modules
    vkDestroyShaderModule(m_Device, vertShaderModule, nullptr);
    vkDestroyShaderModule(m_Device, fragShaderModule, nullptr);

    // Store debug info
    vulkanPipeline.rasterizationSamples = ResolveSupportedSampleCount(
        desc.rasterizationSamples ? desc.rasterizationSamples : 1u, 0u);

    if (pipelineResult.pipeline != VK_NULL_HANDLE)
    {
        vulkanPipeline.pipeline = pipelineResult.pipeline;
        vulkanPipeline.layout = pipelineResult.layout;
        vulkanPipeline.descriptorSetLayout = pipelineResult.descriptorSetLayout;
        vulkanPipeline.renderPass = m_SwapchainRenderPass;
        vulkanPipeline.type = PipelineType::Graphics;
        vulkanPipeline.depthWriteEnable = desc.depthStencilState.depthWriteEnable;
        // Store push constant layout for typed push constants. Stage metadata
        // is canonical for classic graphics pipelines (see
        // kGraphicsPushConstantStages); sizes/names stay as declared for
        // policy checks and the by-name API.
        uint32_t declaredPcSize = desc.pushConstantSize; // do not fallback here; reflect actual layout
        // Preserve per-range metadata for introspection and by-name/by-id writes
        vulkanPipeline.pushRanges.clear();
        if (!desc.pushConstantRanges.empty())
        {
            uint32_t runningOffset = 0;
            for (const auto& pr : desc.pushConstantRanges)
            {
                VulkanPipeline::PushRange vr{};
                runningOffset = (runningOffset + 3u) & ~3u;
                vr.name = pr.name;
                vr.size = pr.size;
                vr.stagesMask = static_cast<uint32_t>(kGraphicsPushConstantStages);
                vr.offset = runningOffset;
                runningOffset += ((pr.size + 3u) & ~3u);
                vulkanPipeline.pushRanges.push_back(std::move(vr));
            }
        }
        else if (declaredPcSize)
        {
            VulkanPipeline::PushRange vr{};
            vr.name = "__default";
            vr.offset = 0;
            vr.size = declaredPcSize;
            vr.stagesMask = static_cast<uint32_t>(kGraphicsPushConstantStages);
            vulkanPipeline.pushRanges.push_back(vr);
        }

        vulkanPipeline.pushConstantSize = declaredPcSize;
        vulkanPipeline.pushConstantStagesMask =
            static_cast<uint32_t>(kGraphicsPushConstantStages);
        vulkanPipeline.layoutPushStageFlags =
            (m_MaxPushConstantBytes & ~3u)
                ? static_cast<uint32_t>(kGraphicsPushConstantStages)
                : 0u;

        // Use global pipeline manager (much cleaner)
        VulkanPipeline* pipelinePtr = new VulkanPipeline(std::move(vulkanPipeline));
        return GameEngine::Rendering::CreatePipeline(pipelinePtr);
    }
    else
    {
        return INVALID_PIPELINE_HANDLE;
    }
}

// Resource destruction
void VulkanDevice::DestroyBuffer(BufferHandle handle)
{
    // Reachable from job threads (upload-worker failure paths, UpdateBuffer's
    // staging retire). Held so the InTeardown() read below is coherent: the
    // rebuild arm only flips under the exclusive lock, so a worker inside this
    // guard always routes deferred — never into the immediate helper against a
    // device mid-teardown.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Access);

    // Untrack from ResourceManager for leak detection
    if (handle.IsValid() && m_ResourceManager)
    {
        m_ResourceManager->UntrackBuffer(handle);
    }

    if (!InTeardown() && handle.IsValid())
    {
        QueueDeferredBufferDestroy(handle);
        return;
    }
    DestroyBufferImmediate(handle);
}

void VulkanDevice::DestroyTexture(TextureHandle handle)
{
    // Same InTeardown() coherence as DestroyBuffer (see there).
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Access);

    // Untrack from ResourceManager for leak detection
    if (handle.IsValid() && m_ResourceManager)
    {
        m_ResourceManager->UntrackTexture(handle);
    }

    if (!InTeardown() && handle.IsValid())
    {
        QueueDeferredTextureDestroy(handle);
        return;
    }
    DestroyTextureImmediate(handle);
}

// Deferred-destroy queueing. Every runtime destruction routes through these:
// resources retire against the value the NEXT submit on EACH queue will signal,
// or against a frame slot's fence on a device with no timeline semaphore.
//
// Per queue, not graphics alone: async compute is on by default in both hosts,
// the render graph submits compute-typed command lists to the compute queue on
// its own timeline, and the HZB build passes ride it. The graphics timeline
// carries no information about that work, so a graphics-only tag cannot prove a
// resource is unreferenced. Which queues actually touch a given resource is not
// knowable at destroy time, so every queue is tagged and all three must pass.
//
// NEXT, not last-signaled: a destroy issued while a frame is being RECORDED is
// ordered before that frame's submits, so the last-signaled value belongs to the
// PREVIOUS frame. Retiring against it frees the object while the frame that
// recorded it — and wrote it into a descriptor set — is still executing, which
// the layer reports as VUID-vkDestroyImageView-imageView-01026 and which is a
// silent use-after-free wherever descriptor buffers put no VkDescriptorSet in
// front of it. NEXT also covers any submit already made this frame, since it is
// strictly greater. The retire sweep in BeginFrame frees entries early once a
// queue has drained, so tagging forward can never strand them.
//
// ONE submit ahead is sufficient only because recording and submission are
// interleaved: RGFrame::RecordAndSubmit walks one submission at a time, ending
// and submitting each command list before recording the next (RGRecord.cpp).
// So at any destroy, every command buffer already recorded has already been
// submitted at a value <= that queue's context counter, and the single open one
// lands in the +1 submit. A recorder that buffered several submissions and
// issued them together would break that: the tag would cover the first of them
// and not the rest. GE_PARALLEL_RECORD is the one path that pre-records ahead
// (pass interiors into secondaries, before the submission walk); it is
// off by default and its secondaries are graphics-queue render passes.
//
// Tags are derived here rather than mirrored in members: the next submit on a queue signals
// that queue's last signalled value + 1, so a cached "next" would only add a second place for
// that invariant to drift — and drift the wrong way is an already-signaled tag, i.e. exactly
// the early free above, silently.
//
// Thread-safe by contract — see m_DeferredDestroyMutex.
VulkanDevice::PerQueueFrame* VulkanDevice::DeferralFrameSlotLocked()
{
    const uint32_t slot = m_DeferralFrameSlot.load(std::memory_order_relaxed);
    return slot < m_Frames.size() ? &m_Frames[slot].graphics : nullptr;
}

QueueRetireTags VulkanDevice::NextSubmitTagsLocked() const
{
    // What the queue lock delivers is that each value is allocated to exactly one submit, so
    // "+1" names a single future submit rather than an ambiguous one. It does NOT reserve that
    // value for this destroy: the read here is unlocked, so any submit on that queue between
    // now and the destroy's own referencing submit takes it, and the tag then names work that
    // does not reference the resource. The paragraph above is the argument that no such submit
    // interposes — it holds only for the interleaved record-then-submit walk described there.
    return {SubmitContextFor(QueueType::Graphics).LastSignalled() + 1,
            SubmitContextFor(QueueType::Compute).LastSignalled() + 1,
            SubmitContextFor(QueueType::Transfer).LastSignalled() + 1};
}

void VulkanDevice::QueueDeferredBufferDestroy(BufferHandle handle)
{
    std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
    // Avoid unbounded duplicate queueing of the same handle.
    if (!m_PendingBufferDestroyIds.insert(handle.id).second)
        return;
    if (HasGraphicsTimelineSemaphore())
        m_DeferredBuffersTimeline.push_back({handle, NextSubmitTagsLocked()});
    else if (PerQueueFrame* slot = DeferralFrameSlotLocked())
        slot->deferredBuffers.push_back(handle);
}

void VulkanDevice::QueueDeferredTextureDestroy(TextureHandle handle)
{
    std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
    if (!m_PendingTextureDestroyIds.insert(handle.id).second)
        return;
    if (HasGraphicsTimelineSemaphore())
        m_DeferredTexturesTimeline.push_back({handle, NextSubmitTagsLocked()});
    else if (PerQueueFrame* slot = DeferralFrameSlotLocked())
        slot->deferredTextures.push_back(handle);
}

void VulkanDevice::QueueDeferredTextureViewDestroy(TextureViewHandle handle)
{
    std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
    if (!m_PendingTextureViewDestroyIds.insert(handle.id).second)
        return;
    if (HasGraphicsTimelineSemaphore())
        m_DeferredTextureViewsTimeline.push_back({handle, NextSubmitTagsLocked()});
    else if (PerQueueFrame* slot = DeferralFrameSlotLocked())
        slot->deferredTextureViews.push_back(handle);
}

void VulkanDevice::QueueDeferredSamplerDestroy(SamplerHandle handle)
{
    std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
    if (!m_PendingSamplerDestroyIds.insert(handle.id).second)
        return;
    if (HasGraphicsTimelineSemaphore())
        m_DeferredSamplersTimeline.push_back({handle, NextSubmitTagsLocked()});
    else if (PerQueueFrame* slot = DeferralFrameSlotLocked())
        slot->deferredSamplers.push_back(handle);
}

// Immediate destruction helpers. Reached only once GPU completion is confirmed
// (the deferred drains) or during teardown — never from the public Destroy* entry
// points while the device is running. They touch VkDevice/VMA with no deferral,
// so each asserts owner-thread affinity: a worker reaching one is racing device
// teardown.
void VulkanDevice::DestroyBufferImmediate(BufferHandle handle)
{
    assert((std::this_thread::get_id() == m_DeviceOwnerThread.load(std::memory_order_relaxed) ||
            std::this_thread::get_id() == m_RebuildExclusiveThread.load(std::memory_order_relaxed)) &&
           "immediate buffer destroy reached from a non-owner thread");
    if (handle.IsValid())
    {
        std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
        m_PendingBufferDestroyIds.erase(handle.id);
    }
    if (VulkanBuffer* buffer = GetVulkanBuffer(handle))
    {
#ifdef RENDERING_HAS_VMA
        if (buffer->allocation != nullptr && m_VmaAllocator != nullptr)
        {
            {
                std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
                m_VmaBufferAllocs.erase(buffer->allocation);
            }
            vmaDestroyBuffer(AsVmaAllocator(m_VmaAllocator), buffer->buffer, buffer->allocation);
        }
        else
        {
            if (buffer->device != VK_NULL_HANDLE)
            {
                if (buffer->buffer != VK_NULL_HANDLE)
                    vkDestroyBuffer(buffer->device, buffer->buffer, nullptr);
                if (buffer->memory != VK_NULL_HANDLE)
                {
                    UntrackDirectMemoryAllocation(buffer->memory);
                    vkFreeMemory(buffer->device, buffer->memory, nullptr);
                }
            }
        }
#else
        if (buffer->device != VK_NULL_HANDLE)
        {
            if (buffer->buffer != VK_NULL_HANDLE)
                vkDestroyBuffer(buffer->device, buffer->buffer, nullptr);
            if (buffer->memory != VK_NULL_HANDLE)
            {
                UntrackDirectMemoryAllocation(buffer->memory);
                vkFreeMemory(buffer->device, buffer->memory, nullptr);
            }
        }
#endif
        GameEngine::Rendering::DestroyBuffer(handle);
        if (!m_IsShutdown && !m_BulkDestroyInProgress)
        {
            std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
            auto it2 = std::remove(m_LiveBuffers.begin(), m_LiveBuffers.end(), handle);
            if (it2 != m_LiveBuffers.end())
                m_LiveBuffers.erase(it2, m_LiveBuffers.end());
        }
        delete buffer;
    }
}

void VulkanDevice::DestroyTextureImmediate(TextureHandle handle)
{
    assert((std::this_thread::get_id() == m_DeviceOwnerThread.load(std::memory_order_relaxed) ||
            std::this_thread::get_id() == m_RebuildExclusiveThread.load(std::memory_order_relaxed)) &&
           "immediate texture destroy reached from a non-owner thread");
    if (handle.IsValid())
    {
        std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
        m_PendingTextureDestroyIds.erase(handle.id);
    }
    if (VulkanTexture* texture = GetVulkanTexture(handle))
    {
        if (texture->device != VK_NULL_HANDLE)
        {
            if (texture->view != VK_NULL_HANDLE)
                vkDestroyImageView(texture->device, texture->view, nullptr);
#ifdef RENDERING_HAS_VMA
            if (texture->allocation != nullptr && m_VmaAllocator != nullptr)
            {
                {
                    std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
                    m_VmaImageAllocs.erase(texture->allocation);
                }
                vmaDestroyImage(AsVmaAllocator(m_VmaAllocator), texture->image, texture->allocation);
            }
            else
            {
                if (texture->image != VK_NULL_HANDLE)
                    vkDestroyImage(texture->device, texture->image, nullptr);
                if (texture->memory != VK_NULL_HANDLE)
                {
                    UntrackDirectMemoryAllocation(texture->memory);
                    vkFreeMemory(texture->device, texture->memory, nullptr);
                }
            }
#else
            if (texture->image != VK_NULL_HANDLE)
                vkDestroyImage(texture->device, texture->image, nullptr);
            if (texture->memory != VK_NULL_HANDLE)
            {
                UntrackDirectMemoryAllocation(texture->memory);
                vkFreeMemory(texture->device, texture->memory, nullptr);
            }
#endif
        }
        GameEngine::Rendering::DestroyTexture(handle);
        if (!m_IsShutdown && !m_BulkDestroyInProgress)
        {
            std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
            auto it2 = std::remove(m_LiveTextures.begin(), m_LiveTextures.end(), handle);
            if (it2 != m_LiveTextures.end())
                m_LiveTextures.erase(it2, m_LiveTextures.end());
        }
        delete texture;
    }
}

static void RegisterSamplerOwner(SamplerHandle handle, VkDevice owner)
{
    if (!handle.IsValid() || owner == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::mutex> lock(g_samplerOwnerMutex);
    g_samplerOwnerByHandleId[handle.id] = owner;
}

static void UnregisterSamplerOwner(SamplerHandle handle)
{
    if (!handle.IsValid())
        return;
    std::lock_guard<std::mutex> lock(g_samplerOwnerMutex);
    g_samplerOwnerByHandleId.erase(handle.id);
}

static void UnregisterSamplerOwnersForDevice(VkDevice owner)
{
    if (owner == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::mutex> lock(g_samplerOwnerMutex);
    for (auto it = g_samplerOwnerByHandleId.begin(); it != g_samplerOwnerByHandleId.end();)
    {
        if (it->second == owner)
            it = g_samplerOwnerByHandleId.erase(it);
        else
            ++it;
    }
}

static void RegisterDescriptorSetOwner(DescriptorSetHandle handle, VkDevice owner)
{
    if (!handle.IsValid() || owner == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::mutex> lock(g_descriptorSetOwnerMutex);
    g_descriptorSetOwnerByHandleId[handle.id] = owner;
}

static void UnregisterDescriptorSetOwner(DescriptorSetHandle handle)
{
    if (!handle.IsValid())
        return;
    std::lock_guard<std::mutex> lock(g_descriptorSetOwnerMutex);
    g_descriptorSetOwnerByHandleId.erase(handle.id);
}

static void UnregisterDescriptorSetOwnersForDevice(VkDevice owner)
{
    if (owner == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::mutex> lock(g_descriptorSetOwnerMutex);
    for (auto it = g_descriptorSetOwnerByHandleId.begin(); it != g_descriptorSetOwnerByHandleId.end();)
    {
        if (it->second == owner)
            it = g_descriptorSetOwnerByHandleId.erase(it);
        else
            ++it;
    }
}

static bool DescriptorSetOwnerMatches(DescriptorSetHandle handle, VkDevice expectedOwner)
{
    if (!handle.IsValid() || expectedOwner == VK_NULL_HANDLE)
        return true;
    std::lock_guard<std::mutex> lock(g_descriptorSetOwnerMutex);
    auto it = g_descriptorSetOwnerByHandleId.find(handle.id);
    if (it == g_descriptorSetOwnerByHandleId.end())
    {
        // Unknown ownership (legacy/untracked path): allow and rely on existing Vulkan validation.
        return true;
    }
    return it->second == expectedOwner;
}

// Helper to safely access VkSampler from SamplerHandle, with optional ownership check.
static VkSampler GetVkSampler(SamplerHandle handle, VkDevice expectedOwner)
{
    if (expectedOwner != VK_NULL_HANDLE && handle.IsValid())
    {
        std::lock_guard<std::mutex> lock(g_samplerOwnerMutex);
        auto it = g_samplerOwnerByHandleId.find(handle.id);
        if (it != g_samplerOwnerByHandleId.end() && it->second != expectedOwner)
        {
            return VK_NULL_HANDLE;
        }
    }
    void* raw = GameEngine::Rendering::GetSampler(handle);
    return raw ? reinterpret_cast<VkSampler>(raw) : VK_NULL_HANDLE;
}

void VulkanDevice::DestroySamplerImmediate(SamplerHandle handle)
{
    assert((std::this_thread::get_id() == m_DeviceOwnerThread.load(std::memory_order_relaxed) ||
            std::this_thread::get_id() == m_RebuildExclusiveThread.load(std::memory_order_relaxed)) &&
           "immediate sampler destroy reached from a non-owner thread");
    if (handle.IsValid())
    {
        std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
        m_PendingSamplerDestroyIds.erase(handle.id);
    }
    if (VkSampler sampler = GetVkSampler(handle, m_Device))
    {
        if (sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_Device, sampler, nullptr);
        }
        GameEngine::Rendering::DestroySampler(handle);
        UnregisterSamplerOwner(handle);

        if (!m_IsShutdown && !m_BulkDestroyInProgress)
        {
            std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
            auto it = std::remove(m_LiveSamplers.begin(), m_LiveSamplers.end(), handle);
            if (it != m_LiveSamplers.end())
                m_LiveSamplers.erase(it, m_LiveSamplers.end());
        }
    }
}

void VulkanDevice::DestroySampler(SamplerHandle handle)
{
    // Same InTeardown() coherence as DestroyBuffer (see there).
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Access);

    // Deferred like textures/buffers/views: vkDestroySampler() must not run while
    // descriptor sets that reference the sampler may still be in-flight on the GPU.
    if (!InTeardown() && handle.IsValid())
    {
        QueueDeferredSamplerDestroy(handle);
        return;
    }
    DestroySamplerImmediate(handle);
}

VulkanDevice::PipelineCreationResult VulkanDevice::CreateComputePipeline(const std::vector<uint8_t>& computeShaderCode,
                                                                         const std::vector<DescriptorSetLayoutDesc>& descriptorSetLayouts)
{
    // Create shader module from compute shader code
    VkShaderModule computeShaderModule = CreateShaderModuleFromBytes(computeShaderCode);
    if (computeShaderModule == VK_NULL_HANDLE)
    {
        Logger::Log::Error("Failed to create compute shader module (CreateComputePipeline)");
        return {VK_NULL_HANDLE, VK_NULL_HANDLE};
    }

    // Convert descriptor set layouts to Vulkan format
    std::vector<VkDescriptorSetLayout> vkDescriptorSetLayouts;
    for (const auto& layoutDesc : descriptorSetLayouts)
    {
        uint64_t key = 0;
        VkDescriptorSetLayout vkLayout = GetOrCreateDescriptorSetLayoutCached(layoutDesc, key);
        if (vkLayout == VK_NULL_HANDLE)
        {
            Logger::Log::Error("Failed to create descriptor set layout for compute pipeline");
            vkDestroyShaderModule(m_Device, computeShaderModule, nullptr);
            return {VK_NULL_HANDLE, VK_NULL_HANDLE};
        }
        vkDescriptorSetLayouts.push_back(vkLayout);
    }

    // Create pipeline layout with push constant support (compute path uses policy defaults).
    // Compute pipelines can only carry VK_SHADER_STAGE_COMPUTE_BIT in a push range
    // (VUID-VkPushConstantRange-stageFlags-01304).
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = 128;

    // Create or reuse pipeline layout via cache
    uint64_t pipelineKey = 0;
    VkPipelineLayout pipelineLayout = GetOrCreatePipelineLayoutCached(
        vkDescriptorSetLayouts,
        pushConstantRange.size,
        pushConstantRange.stageFlags,
        pipelineKey);
    if (pipelineLayout == VK_NULL_HANDLE)
    {
        Logger::Log::Error("Failed to create/reuse compute pipeline layout");
        for (auto layout : vkDescriptorSetLayouts)
        {
            ReleaseDescriptorSetLayoutCached(layout);
        }
        vkDestroyShaderModule(m_Device, computeShaderModule, nullptr);
        return {VK_NULL_HANDLE, VK_NULL_HANDLE};
    }

    // Create compute pipeline
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = computeShaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout;
    if (AnySetLayoutIsDescriptorBufferEligible(vkDescriptorSetLayouts))
        pipelineInfo.flags |= VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;

    VkPipeline pipeline;
    VkPipelineCache pipelineCache = m_VkDiskPipelineCache ? m_VkDiskPipelineCache->GetVkPipelineCache() : VK_NULL_HANDLE;

    PIPELINE_CREATION_TIMER(m_VkDiskPipelineCache.get(), false); // Assume cache miss for timing
    VkResult result;
    {
        auto cacheLock = m_VkDiskPipelineCache ? m_VkDiskPipelineCache->LockForCreation() : std::unique_lock<std::mutex>();
        result = vkCreateComputePipelines(m_Device, pipelineCache, 1, &pipelineInfo, nullptr, &pipeline);
    }

    // Cleanup shader module (no longer needed after pipeline creation)
    vkDestroyShaderModule(m_Device, computeShaderModule, nullptr);

    if (result == VK_SUCCESS)
    {
        // For now, return the first descriptor set layout (we'll improve this later)
        VkDescriptorSetLayout firstLayout = vkDescriptorSetLayouts.empty() ? VK_NULL_HANDLE : vkDescriptorSetLayouts[0];
        return {pipeline, pipelineLayout, firstLayout};
    }
    else
    {
        Logger::Log::Error("Failed to create compute pipeline! Error: {}", (int)result);
        ReleasePipelineLayoutCached(pipelineLayout);
        for (auto layout : vkDescriptorSetLayouts)
        {
            ReleaseDescriptorSetLayoutCached(layout);
        }
        return {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    }
}
VulkanDevice::PipelineCreationResult VulkanDevice::CreateComputePipelineFromDesc(const PipelineDesc& desc)
{
    // Create shader module from compute shader code
    VkShaderModule computeShaderModule = CreateShaderModuleFromBytes(desc.computeShader);
    if (computeShaderModule == VK_NULL_HANDLE)
    {
        Logger::Log::Error("Failed to create compute shader module (CreateComputePipelineFromDesc)");
        return {VK_NULL_HANDLE, VK_NULL_HANDLE};
    }

    // Get specialization constants if provided
    const VkSpecializationInfo* specializationInfo = nullptr;
    std::vector<VkSpecializationMapEntry> specializationEntries;
    VkSpecializationInfo specializationInfoStorage{};
    if (desc.specializationConstants && !desc.specializationConstants->IsEmpty())
    {
        const auto& entries = desc.specializationConstants->GetEntries();
        specializationEntries.reserve(entries.size());
        for (const auto& kv : entries)
        {
            const auto& entry = kv.second;
            VkSpecializationMapEntry vkEntry{};
            vkEntry.constantID = entry.ConstantId;
            vkEntry.offset = entry.Offset;
            vkEntry.size = entry.Size;
            specializationEntries.push_back(vkEntry);
        }
        specializationInfoStorage.mapEntryCount = static_cast<uint32_t>(specializationEntries.size());
        specializationInfoStorage.pMapEntries = specializationEntries.data();
        specializationInfoStorage.dataSize = desc.specializationConstants->GetDataSize();
        specializationInfoStorage.pData = desc.specializationConstants->GetDataPtr();
        specializationInfo = &specializationInfoStorage;
    }

    // Create descriptor set layouts from desc.descriptorSetLayouts
    std::vector<VkDescriptorSetLayout> vkLayouts;
    for (const auto& layoutDesc : desc.descriptorSetLayouts)
    {
        uint64_t key = 0;
        VkDescriptorSetLayout vkLayout = GetOrCreateDescriptorSetLayoutCached(layoutDesc, key);
        if (vkLayout == VK_NULL_HANDLE)
        {
            Logger::Log::Error("Failed to create descriptor set layout from PipelineDesc for compute pipeline");
            vkDestroyShaderModule(m_Device, computeShaderModule, nullptr);
            return {VK_NULL_HANDLE, VK_NULL_HANDLE};
        }
        vkLayouts.push_back(vkLayout);
    }

    // Create pipeline layout with optional push constants (multi-range).
    // Compute pipelines can only have VK_SHADER_STAGE_COMPUTE_BIT on any push
    // range (Vulkan spec: VUID-VkPushConstantRange-stageFlags-01304). When a
    // caller omits stagesMask we default to COMPUTE rather than the
    // historical VK_SHADER_STAGE_ALL, which would make vkCmdPushConstants
    // trip VUID-01795 if the command list pushed with any non-compute stage.
    std::vector<VkPushConstantRange> pushRanges;
    auto enforceComputeStage = [](VkShaderStageFlags flags, const char* context) -> VkShaderStageFlags
    {
        if ((flags & VK_SHADER_STAGE_COMPUTE_BIT) != VK_SHADER_STAGE_COMPUTE_BIT)
        {
            Logger::Log::Error(
                "Compute pipeline push-constant stagesMask 0x{:x} lacks COMPUTE (context={}). "
                "Auto-correcting to VK_SHADER_STAGE_COMPUTE_BIT to avoid a VUID-01304 violation.",
                static_cast<uint32_t>(flags), context);
            assert(false && "compute pipeline push-constant stagesMask must include COMPUTE");
            return static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_COMPUTE_BIT);
        }
        return flags;
    };

    if (!desc.pushConstantRanges.empty())
    {
        uint32_t runningOffset = 0;
        for (const auto& r : desc.pushConstantRanges)
        {
            VkPushConstantRange vkR{};
            VkShaderStageFlags rawStages = r.stagesMask
                                               ? static_cast<VkShaderStageFlags>(r.stagesMask)
                                               : static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_COMPUTE_BIT);
            vkR.stageFlags = enforceComputeStage(rawStages, "compute multi-range");
            runningOffset = (runningOffset + 3u) & ~3u;
            vkR.offset = runningOffset;
            vkR.size = (r.size + 3u) & ~3u;
            runningOffset += vkR.size;
            pushRanges.push_back(vkR);
        }
        if (runningOffset > m_MaxPushConstantBytes)
        {
            Logger::Log::Error("PushConstantsEnforcement: total size exceeds policy (compute)");
            vkDestroyShaderModule(m_Device, computeShaderModule, nullptr);
            return {VK_NULL_HANDLE, VK_NULL_HANDLE};
        }
    }
    else if (desc.pushConstantSize)
    {
        VkPushConstantRange vkR{};
        VkShaderStageFlags rawStages = desc.pushConstantStagesMask
                                           ? static_cast<VkShaderStageFlags>(desc.pushConstantStagesMask)
                                           : static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_COMPUTE_BIT);
        vkR.stageFlags = enforceComputeStage(rawStages, "compute single-range");
        vkR.offset = 0;
        vkR.size = desc.pushConstantSize;
        if (vkR.size > m_MaxPushConstantBytes)
        {
            Logger::Log::Error("PushConstantsEnforcement: requested size exceeds policy (compute)");
            vkDestroyShaderModule(m_Device, computeShaderModule, nullptr);
            return {VK_NULL_HANDLE, VK_NULL_HANDLE};
        }
        pushRanges.push_back(vkR);
    }

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    uint64_t plKey = 0;
    pipelineLayout = GetOrCreatePipelineLayoutCached(vkLayouts, pushRanges, plKey);
    if (pipelineLayout == VK_NULL_HANDLE)
    {
        Logger::Log::Error("Failed to get/create compute pipeline layout from PipelineDesc (compute)");
        vkDestroyShaderModule(m_Device, computeShaderModule, nullptr);
        return {VK_NULL_HANDLE, VK_NULL_HANDLE};
    }

    // Create compute pipeline
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = computeShaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.stage.pSpecializationInfo = specializationInfo;
    pipelineInfo.layout = pipelineLayout;
    if (AnySetLayoutIsDescriptorBufferEligible(vkLayouts))
        pipelineInfo.flags |= VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;

    VkPipeline pipeline;
    VkPipelineCache pipelineCache = m_VkDiskPipelineCache ? m_VkDiskPipelineCache->GetVkPipelineCache() : VK_NULL_HANDLE;

    PIPELINE_CREATION_TIMER(m_VkDiskPipelineCache.get(), false);
    VkResult result;
    {
        auto cacheLock = m_VkDiskPipelineCache ? m_VkDiskPipelineCache->LockForCreation() : std::unique_lock<std::mutex>();
        result = vkCreateComputePipelines(m_Device, pipelineCache, 1, &pipelineInfo, nullptr, &pipeline);
    }

    vkDestroyShaderModule(m_Device, computeShaderModule, nullptr);

    if (result != VK_SUCCESS)
    {
        Logger::Log::Error("Failed to create compute pipeline! Error: {}", (int)result);
        // These handles are CACHED (GetOrCreate*Cached): destroying them directly
        // would invalidate every other pipeline sharing them and corrupt the cache
        // refcounts — release through the cache like CreateComputePipeline does.
        ReleasePipelineLayoutCached(pipelineLayout);
        for (auto l : vkLayouts)
        {
            ReleaseDescriptorSetLayoutCached(l);
        }
        return {VK_NULL_HANDLE, VK_NULL_HANDLE};
    }
    SetVkObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(pipeline), desc.debugName);

    // Return first set layout for convenience
    VkDescriptorSetLayout firstLayout = vkLayouts.empty() ? VK_NULL_HANDLE : vkLayouts[0];
    return {pipeline, pipelineLayout, firstLayout};
}

void VulkanDevice::DestroyPipeline(PipelineHandle handle)
{
    if (VulkanPipeline* pipeline = GetVulkanPipeline(handle))
    {
        if (pipeline->device != VK_NULL_HANDLE)
        {
            if (pipeline->pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(pipeline->device, pipeline->pipeline, nullptr);
            }
            // Release pipeline layout and descriptor set layout via cache-aware helpers
            // Guard: only release via this device's caches if the pipeline belongs to this VkDevice
            if (pipeline->device == m_Device)
            {
                if (pipeline->layout != VK_NULL_HANDLE)
                {
                    ReleasePipelineLayoutCached(pipeline->layout);
                }
                if (pipeline->descriptorSetLayout != VK_NULL_HANDLE)
                {
                    ReleaseDescriptorSetLayoutCached(pipeline->descriptorSetLayout);
                }
            }
        }
        // Destroy using global manager
        GameEngine::Rendering::DestroyPipeline(handle);
        delete pipeline;
    }
}

void VulkanDevice::DiagnoseFrameSlottedHostWrite(const VulkanBuffer& buffer) const
{
#if defined(GE_DEV_DIAG)
    if ((buffer.createFlags & BufferCreateFlags::FrameSlotted) != BufferCreateFlags::FrameSlotted)
        return;
    if (m_CurrentFrameSlotFenced.load(std::memory_order_relaxed))
        return;
    Logger::Log::Error(
        "Host write to frame-slotted buffer '{}' outside an acquired frame: slot {} has not had "
        "its fence waited, so a frame still in flight can be reading these bytes. The write "
        "belongs between BeginFrame and Present.",
        buffer.debugName, m_CurrentFrame.load(std::memory_order_relaxed));
    assert(false && "host write to a frame-slotted buffer before its slot fence was waited");
#else
    (void)buffer;
#endif
}

// Resource access - REAL implementation
void* VulkanDevice::MapBuffer(BufferHandle handle)
{
    // vmaMapMemory/vmaInvalidateAllocation must not overlap a rebuild's
    // ShutdownVMA.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Access);

#ifdef RENDERING_HAS_VMA
    // No allocator after a FAILED rebuild (see CreateBuffer): every mappable
    // allocation died with it, so fail the map rather than hand the vma calls
    // below a null allocator.
    if (m_VmaAllocator == nullptr)
    {
        return nullptr;
    }
#endif

    VulkanBuffer* vulkanBuffer = GetVulkanBuffer(handle);
    if (!vulkanBuffer)
    {
        return nullptr;
    }
    if (vulkanBuffer->buffer == VK_NULL_HANDLE)
    {
        return nullptr;
    }
    DiagnoseFrameSlottedHostWrite(*vulkanBuffer);

    // Use VMA for memory mapping (allocation may be non-null when using VMA)
#ifdef RENDERING_HAS_VMA
    if (vulkanBuffer->allocation != nullptr)
    {
        if (vulkanBuffer->persistentMappedData != nullptr)
        {
            // Use persistent mapping - invalidate to ensure latest data
            vmaInvalidateAllocation(AsVmaAllocator(m_VmaAllocator), vulkanBuffer->allocation, 0, VK_WHOLE_SIZE);
            return vulkanBuffer->persistentMappedData;
        }
        else
        {
            // Guard: require host-visible memory for temporary mapping
            if ((vulkanBuffer->memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
            {
                Logger::Log::Error(
                    "MapBuffer called on non host-visible buffer (handle={}). Use UpdateBuffer/staging instead.",
                    (uint64_t)handle);
                return nullptr;
            }
            // Use temporary VMA mapping
            void* mappedData = nullptr;
            VkResult result = vmaMapMemory(AsVmaAllocator(m_VmaAllocator), vulkanBuffer->allocation, &mappedData);
            if (result == VK_SUCCESS && mappedData)
            {
                // Invalidate memory to ensure we read latest data
                vmaInvalidateAllocation(AsVmaAllocator(m_VmaAllocator), vulkanBuffer->allocation, 0, VK_WHOLE_SIZE);
                return mappedData;
            }
            else
            {
                Logger::Log::Error("Failed to map buffer memory using VMA! Error: {}", (int)result);
                return nullptr;
            }
        }
    }
    else
    {
        Logger::Log::Error("Buffer {} has no VMA allocation!", (uint64_t)handle);
        return nullptr;
    }
#else
    // Fallback to direct Vulkan memory mapping
    void* mappedData = nullptr;
    VkResult result = vkMapMemory(m_Device, vulkanBuffer.memory, 0, vulkanBuffer.size, 0, &mappedData);
    if (result == VK_SUCCESS)
    {
        // Invalidate memory range to ensure GPU writes are visible to CPU
        VkMappedMemoryRange memoryRange{};
        memoryRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        memoryRange.memory = vulkanBuffer.memory;
        memoryRange.offset = 0;
        memoryRange.size = vulkanBuffer.size;

        VkResult invalidateResult = vkInvalidateMappedMemoryRanges(m_Device, 1, &memoryRange);
        if (invalidateResult == VK_SUCCESS)
        {
        }
        else
        {
            Logger::Log::Warning(
                "Buffer {} mapped successfully, but memory invalidation failed: {}",
                (uint64_t)handle,
                (int)invalidateResult);
        }

        return mappedData;
    }
    else
    {
        Logger::Log::Error("Failed to map buffer memory! Error: {}", (int)result);
        return nullptr;
    }
#endif
}

void VulkanDevice::UnmapBuffer(BufferHandle handle)
{
    // vmaUnmapMemory must not overlap a rebuild's ShutdownVMA.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Access);

    VulkanBuffer* vulkanBuffer = GetVulkanBuffer(handle);
    if (!vulkanBuffer)
    {
        return;
    }

#ifdef RENDERING_HAS_VMA
    if (vulkanBuffer->allocation != nullptr)
    {
        if (vulkanBuffer->persistentMappedData != nullptr)
        {
            // Persistent mapping - no need to unmap
        }
        else
        {
            // Unmap temporary VMA mapping
            vmaUnmapMemory(AsVmaAllocator(m_VmaAllocator), vulkanBuffer->allocation);
        }
    }
#else
    // Fallback to direct Vulkan memory unmapping
    if (vulkanBuffer->memory != VK_NULL_HANDLE)
    {
        vkUnmapMemory(m_Device, vulkanBuffer->memory);
    }
#endif
}

uint64_t VulkanDevice::GetBufferDeviceAddress(BufferHandle handle)
{
    // Address 0 is the documented "no BDA" answer consumers bail on; querying
    // vkGetBufferDeviceAddress without the feature enabled (and on buffers
    // created without the usage bit) is a VUID violation with an undefined
    // return, so answer 0 when either the device or this allocation lacks BDA.
    if (!m_BufferDeviceAddressEnabledAtInit)
    {
        return 0;
    }

    VulkanBuffer* vulkanBuffer = GetVulkanBuffer(handle);
    if (!vulkanBuffer || vulkanBuffer->buffer == VK_NULL_HANDLE ||
        (static_cast<uint32_t>(vulkanBuffer->usage) &
         static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress)) == 0)
    {
        return 0;
    }

    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = vulkanBuffer->buffer;
    return static_cast<uint64_t>(vkGetBufferDeviceAddress(m_Device, &info));
}

IAccelerationStructureBackend* VulkanDevice::GetAccelerationStructureBackend()
{
    // A backend that failed to rebind after a rebuild stays alive for the
    // consumers holding it, but is never handed out again.
    if (!m_AccelerationStructures || !m_AccelerationStructures->IsFunctional())
        return nullptr;
    return m_AccelerationStructures.get();
}

void VulkanDevice::UpdateBufferRanges(BufferHandle handle, std::span<const BufferUpdateRange> ranges)
{
    if (ranges.empty())
        return;
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Create);
    VulkanBuffer* buffer = GetVulkanBuffer(handle);
    if (!buffer || buffer->buffer == VK_NULL_HANDLE)
        return;
    DiagnoseFrameSlottedHostWrite(*buffer);

    // Validate the entire batch before writing anything, including overflow.
    size_t first = buffer->size;
    size_t end = 0;
    for (const auto& range : ranges)
    {
        if (range.size == 0)
            continue;
        if (!range.data || range.size > buffer->size || range.offset > buffer->size - range.size)
        {
            Logger::Log::Error("UpdateBufferRanges: invalid write of {} bytes at offset {} to '{}' ({} bytes); batch rejected",
                               range.size, range.offset, buffer->debugName, buffer->size);
            return;
        }
        first = std::min(first, range.offset);
        end = std::max(end, range.offset + range.size);
    }
    if (end == 0)
        return;

#ifdef RENDERING_HAS_VMA
    if (buffer->allocation && (buffer->memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
    {
        void* mapped = buffer->persistentMappedData;
        const bool temporary = mapped == nullptr;
        if (temporary && vmaMapMemory(AsVmaAllocator(m_VmaAllocator), buffer->allocation, &mapped) != VK_SUCCESS)
            return;
        if (!mapped)
            return;
        for (const auto& range : ranges)
            if (range.size != 0)
                std::memcpy(static_cast<char*>(mapped) + range.offset, range.data, range.size);
        // VMA handles non-coherent atom alignment; coherent heaps need no flush.
        // One map/flush and rebuild guard cover all sparse instance writes.
        vmaFlushAllocation(AsVmaAllocator(m_VmaAllocator), buffer->allocation, first, end - first);
        if (temporary)
            vmaUnmapMemory(AsVmaAllocator(m_VmaAllocator), buffer->allocation);
        return;
    }
#endif
    // Preserve the existing staging and non-VMA paths for other allocations.
    IDevice::UpdateBufferRanges(handle, ranges);
}

void VulkanDevice::UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data)
{
    if (!data)
    {
        Logger::Log::Error("UpdateBuffer: data is null");
        return;
    }

    // A zero-byte update is a no-op request, not an error. The staging branch
    // below sizes its buffer from `size`, and CreateBuffer rejects size 0.
    if (size == 0)
    {
        return;
    }

    // Reachable from job threads. Every branch below touches the allocator
    // (vmaMap/vmaFlush, or CreateBuffer + a queue submit on the staging path),
    // none of which may overlap an in-place rebuild's teardown. Held across the
    // whole entry; the nested CreateBuffer/DestroyBuffer/self-recursion skip
    // re-acquisition via the guard's thread-local.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Create);

    VulkanBuffer* vulkanBuffer = GetVulkanBuffer(handle);
    if (!vulkanBuffer)
    {
        return;
    }

    // Valid buffer handle is required; memory may be null when using VMA (allocation-driven)
    if (vulkanBuffer->buffer == VK_NULL_HANDLE)
    {
        return;
    }
    DiagnoseFrameSlottedHostWrite(*vulkanBuffer);

    // Reject writes that would run past the allocation. Every path below
    // (persistent map, temp map, staging copy) trusts offset/size blindly, so
    // an oversized request would corrupt whatever lives after the buffer's
    // memory. (Overflow-safe form: offset + size can wrap.)
    if (size > vulkanBuffer->size || offset > vulkanBuffer->size - size)
    {
        Logger::Log::Error(
            "UpdateBuffer: write of {} bytes at offset {} exceeds buffer '{}' ({} bytes); rejected",
            size, offset, vulkanBuffer->debugName, vulkanBuffer->size);
        return;
    }

    // Map the buffer memory and copy data using VMA
#ifdef RENDERING_HAS_VMA
    if (vulkanBuffer->allocation != nullptr)
    {
        if (vulkanBuffer->persistentMappedData != nullptr)
        {
            // Use persistent mapping - no need to map/unmap
            memcpy(static_cast<char*>(vulkanBuffer->persistentMappedData) + offset, data, size);
            vmaFlushAllocation(AsVmaAllocator(m_VmaAllocator), vulkanBuffer->allocation, offset, size);
        }
        else if ((vulkanBuffer->memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            // Host-visible allocation: map temporarily
            void* mappedData = nullptr;
            VkResult result = vmaMapMemory(AsVmaAllocator(m_VmaAllocator), vulkanBuffer->allocation, &mappedData);
            if (result == VK_SUCCESS && mappedData)
            {
                memcpy(static_cast<char*>(mappedData) + offset, data, size);
                vmaFlushAllocation(AsVmaAllocator(m_VmaAllocator), vulkanBuffer->allocation, offset, size);
                vmaUnmapMemory(AsVmaAllocator(m_VmaAllocator), vulkanBuffer->allocation);
            }
        }
        else
        {
            // Non-host-visible: staging upload with deferred staging buffer destruction via timeline semaphore.
            // The staging buffer holds the payload only — the copy below reads
            // it from offset 0 and writes the destination at `offset`.
            BufferDesc stagingDesc{};
            stagingDesc.size = size;
            stagingDesc.usage = static_cast<uint32_t>(BufferUsage::TransferSrc);
            stagingDesc.memoryUsage = BufferMemoryUsage::Upload;
            stagingDesc.flags = BufferCreateFlags::PersistentlyMapped;
            stagingDesc.debugName = "UpdateBuffer_Staging";
            BufferHandle staging = CreateBuffer(stagingDesc);
            if (staging != INVALID_HANDLE)
            {
                // Upload to staging (host-visible, persistently mapped)
                UpdateBuffer(staging, 0, size, data);
                // Record copy from staging to destination
                auto cmd = CreateCommandList(QueueType::Transfer);
                cmd->Begin();
                cmd->CopyBuffer(staging, handle, size, 0, offset);
                cmd->End();
                // Defer the staging buffer against THIS submit's value. Re-reading the queue
                // counter here would pick up whatever the latest submit on that queue reached —
                // another thread's, on a worker, already past this copy.
                const GpuSyncToken copyToken =
                    ExecuteCommandListsTracked(std::vector<CommandList*>{cmd.get()}).transfer;
                if (copyToken.value != 0)
                {
                    // Store the timeline the token names — the one the copy actually signalled.
                    // It is the graphics timeline when no dedicated transfer queue exists, and
                    // the compute one when compute and transfer share a family, so recording
                    // anything role-derived would retire this buffer against a counter that
                    // never tracks the copy.
                    std::lock_guard<std::mutex> stagingLock(m_DeferredStagingMutex);
                    m_DeferredStagingBuffers.push_back({staging, copyToken.sem, copyToken.value});
                }
                else
                {
                    // Nothing was signalled, so no timeline will ever release this buffer.
                    DestroyBuffer(staging);
                }
            }
            else
            {
                Logger::Log::Error("UpdateBuffer: failed to create staging buffer for device-local upload");
            }
        }
    }
    else
    {
        Logger::Log::Error("Buffer {} has no VMA allocation!", (uint64_t)handle);
    }

#else
    // Fallback to direct Vulkan memory mapping
    void* mappedData = nullptr;
    VkResult result = vkMapMemory(m_Device, vulkanBuffer->memory, offset, size, 0, &mappedData);
    if (result == VK_SUCCESS)
    {
        memcpy(static_cast<char*>(mappedData), data, size);
        vkUnmapMemory(m_Device, vulkanBuffer->memory);
    }
    else
    {
        Logger::Log::Error("Failed to map buffer memory! Error: {}", (int)result);
    }
#endif
}

SemaphoreHandle VulkanDevice::CreateTimelineSemaphore(uint64_t initialValue)
{
    // Try to create a timeline semaphore; if unavailable, fall back to binary and mark type accordingly
    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = initialValue;
    VkSemaphoreCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    createInfo.pNext = &typeInfo;
    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult r = CreateSemaphoreTracked(createInfo, sem);
    Handle h = m_HandleManager.Create();
    if (r != VK_SUCCESS)
    {
        // Fallback: create binary semaphore so we can still submit via legacy path
        VkSemaphoreCreateInfo binCI{};
        binCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (CreateSemaphoreTracked(binCI, sem) != VK_SUCCESS)
        {
            return INVALID_HANDLE;
        }
        m_TimelineSemaphores[h.id] = {sem, /*isTimeline*/ false};
#if defined(DEBUG) || defined(_DEBUG)
        Logger::Log::Warning("Timeline semaphore not supported, using binary fallback");
#endif
        return SemaphoreHandle(h.id);
    }
    m_TimelineSemaphores[h.id] = {sem, /*isTimeline*/ true};
    return SemaphoreHandle(h.id);
}

void VulkanDevice::DestroySemaphore(SemaphoreHandle h)
{
    auto it = m_TimelineSemaphores.find(h.id);
    if (it != m_TimelineSemaphores.end())
    {
        DestroySemaphoreTracked(it->second.sem);
        m_TimelineSemaphores.erase(it);
    }
}

static int QueueOrdinal(IDevice::QueueType qt)
{
    switch (qt)
    {
    case IDevice::QueueType::Graphics: return 0;
    case IDevice::QueueType::Compute:  return 1;
    case IDevice::QueueType::Transfer: return 2;
    }
    return 0;
}

void VulkanDevice::BindQueueSubmitContexts()
{
    auto semaphoreFor = [this](SemaphoreHandle timeline) -> VkSemaphore
    {
        auto it = m_TimelineSemaphores.find(timeline.id);
        return (it != m_TimelineSemaphores.end() && it->second.isTimeline) ? it->second.sem
                                                                          : VK_NULL_HANDLE;
    };

    // Graphics always exists and is the queue every other role falls back to. Its context is
    // bound to m_GraphicsTimeline unconditionally, which is the invariant the graphics-only
    // drains and polls rely on when they pair that timeline against the context's counter.
    // Compute and transfer carry no such invariant: whether their timelines are the ones their
    // work signals depends on the family selection, so nothing may pair them by role.
    QueueSubmitContext& graphics = m_SubmitContextStorage[0];
    graphics.Bind(m_GraphicsQueue, m_GraphicsTimeline, semaphoreFor(m_GraphicsTimeline));
    m_SubmitContextByRole.fill(&graphics);

    size_t nextFreeContext = 1;
    auto bindRole = [&](QueueType role, VkQueue queue, SemaphoreHandle timeline)
    {
        // No dedicated queue: the role runs on the graphics queue, so it must take the graphics
        // lock and signal the graphics timeline — which is what it already points at.
        if (queue == VK_NULL_HANDLE)
            return;

        // One physical queue gets one context whatever the roles call it. The compute and
        // transfer roles resolve independently and can land on the same family, and two
        // contexts over one VkQueue would be two locks over one externally-synchronised
        // handle — exactly what this type exists to prevent.
        for (size_t i = 0; i < nextFreeContext; ++i)
        {
            if (m_SubmitContextStorage[i].Queue() == queue)
            {
                m_SubmitContextByRole[QueueOrdinal(role)] = &m_SubmitContextStorage[i];
                return;
            }
        }

        m_SubmitContextStorage[nextFreeContext].Bind(queue, timeline, semaphoreFor(timeline));
        m_SubmitContextByRole[QueueOrdinal(role)] = &m_SubmitContextStorage[nextFreeContext];
        ++nextFreeContext;
    };

    bindRole(QueueType::Compute, m_ComputeQueue, m_ComputeTimeline);
    bindRole(QueueType::Transfer, m_TransferQueue, m_TransferTimeline);

    // Contexts the roles no longer point at (a rebuild that resolved fewer dedicated queues
    // than the previous device) must not keep a stale queue handle.
    for (size_t i = nextFreeContext; i < m_SubmitContextStorage.size(); ++i)
        m_SubmitContextStorage[i].Unbind();
}

QueueSubmitContext& VulkanDevice::SubmitContextFor(QueueType queue)
{
    return *m_SubmitContextByRole[QueueOrdinal(queue)];
}

const QueueSubmitContext& VulkanDevice::SubmitContextFor(QueueType queue) const
{
    return *m_SubmitContextByRole[QueueOrdinal(queue)];
}

void VulkanDevice::RouteSubmittedCommandList(VulkanCommandList* list, SemaphoreHandle timeline,
                                             uint64_t timelineValue)
{
    if (!list)
        return;

    VkCommandBuffer cb = list->GetVkCommandBuffer();
    if (cb != VK_NULL_HANDLE)
    {
        auto& fr = m_Frames[m_CurrentFrame];
        // The slot's compute fence must cover every compute submission made under it, however
        // its buffer retires: render-graph passes on that queue write the slot's timestamp
        // queries, which BeginFrame resets on the host once the fence is waited.
        if (list->GetQueueType() == QueueType::Compute)
            fr.compute.submittedSinceArm = true;
        if (auto* threadPool = static_cast<ThreadCommandPoolEntry*>(list->GetThreadPoolOwner()))
        {
            RouteThreadPoolCmdBufferForRecycle(threadPool, cb, QueueOrdinal(list->GetQueueType()),
                                               timeline, timelineValue);
        }
        else
        {
            // Legacy per-frame pool: covered by the frame fence rather than the timeline.
            auto& pq = (list->GetQueueType() == QueueType::Graphics)  ? fr.graphics
                       : (list->GetQueueType() == QueueType::Compute) ? fr.compute
                                                                      : fr.transfer;
            pq.usedCmd.push_back(cb);
        }
    }

    // Nulls the list's buffer, so the destructor can no longer recycle a buffer the GPU is
    // still executing. Pairing this with the routing above is what makes "submitted" and
    // "tagged for retirement" one step rather than two a mid-frame return can separate.
    list->OnSubmitted();
}

bool VulkanDevice::QueueSubmit(QueueType queue,
                               const std::vector<CommandList*>& cmdLists,
                               const std::vector<std::pair<SemaphoreHandle, uint64_t>>& waitSemaphores,
                               const std::vector<std::pair<SemaphoreHandle, uint64_t>>& signalSemaphores)
{
    if (cmdLists.empty())
        return true;

    // Q6 mid-frame short-circuit (design F10): a latched device loss means no
    // submit may reach the dead device.
    if (m_RecoveryEnabled && m_Health.IsLost())
        return false;

    // Q6 device-loss classification + forced-loss injection for the render-graph
    // per-frame submit. This is the editor's real graphics submit path
    // (RGFrame::RecordAndSubmit -> QueueSubmit). ExecuteCommandLists is the other
    // one, and it is not confined to bring-up: TextureService::GetOrUpload reaches
    // it from ECS extraction workers (and, under GE_PARALLEL_RECORD, render-graph
    // record workers) via UploadTexturesBatched. Both route through
    // OnDeviceLostObserved. Forced loss targets the graphics queue only (matches
    // the design's "graphics submit").
    auto classifyQueueSubmit = [&](VkResult& res)
    {
        bool synthesized = false;
        if (m_FaultInjection.Active() && queue == QueueType::Graphics)
        {
            ++m_FaultSubmitCounter;
            if (m_FaultInjection.ShouldForceLostAtFrame(m_FaultFrameCounter) ||
                m_FaultInjection.ShouldForceLostAtSubmit(m_FaultSubmitCounter))
            {
                Logger::Log::Warning("VulkanDevice: [fault-injection] forcing DEVICE_LOST at RG graphics submit (frame {}, submit {})",
                                     m_FaultFrameCounter, m_FaultSubmitCounter);
                res = VK_ERROR_DEVICE_LOST;
                synthesized = true;
                // Consume this target so the loss is not re-triggered every frame
                // post-rebuild; if extra frame targets were queued
                // (GE_VK_FORCE_DEVICE_LOST=N1,N2,...) the next one arms for a
                // repeat-loss / re-entry test, otherwise the hook disarms.
                m_FaultInjection.ConsumeLostTarget();
            }
        }
        if (res == VK_ERROR_DEVICE_LOST && m_RecoveryEnabled)
        {
            OnDeviceLostObserved("render-graph queue submit", res, /*deviceActuallyLost=*/!synthesized);
        }
    };

    // The chokepoint for the queue this role resolves to: it owns the VkQueue, the timeline
    // that queue signals, and the lock covering both. A role with no dedicated queue resolves
    // to graphics here, so its work is submitted and tagged against the queue it truly runs on.
    QueueSubmitContext& submitContext = SubmitContextFor(queue);
    const VkQueue vkQueue = submitContext.Queue();

    // Keep the lists alongside their buffers: only the lists whose buffers actually reach
    // vkQueueSubmit may be routed for retirement, and they must be routed against that submit's
    // value. Collecting them together is what keeps the two sets from drifting apart.
    std::vector<VkCommandBuffer> cbs;
    std::vector<VulkanCommandList*> submittedLists;
    cbs.reserve(cmdLists.size());
    submittedLists.reserve(cmdLists.size());
    for (auto* cl : cmdLists)
    {
        auto* vcl = dynamic_cast<VulkanCommandList*>(cl);
        if (vcl && vcl->IsReadyToSubmit())
        {
            cbs.push_back(vcl->GetVkCommandBuffer());
            submittedLists.push_back(vcl);
        }
    }
    if (cbs.empty())
        return true;

    bool touchesSwapchainPath = false;
    if (queue == QueueType::Graphics && m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0)
    {
        for (auto* cl : cmdLists)
        {
            auto* vcl = dynamic_cast<VulkanCommandList*>(cl);
            if (!vcl)
                continue;
            for (const auto& use : vcl->GetUsedResources())
            {
                if (use.ResourceType != VulkanCommandList::UsedResource::Type::Texture)
                    continue;
                if (IsSwapchainTextureHandle(TextureHandle(use.Id.id)))
                {
                    touchesSwapchainPath = true;
                    break;
                }
            }
            if (touchesSwapchainPath)
                break;
        }
    }

    // Prefer vkQueueSubmit2 with timeline semaphores and synchronization2 when enabled; fallback to vkQueueSubmit
    std::vector<VkSemaphoreSubmitInfo> waitInfos;
    std::vector<VkSemaphore> binWaitSems;
    std::vector<VkPipelineStageFlags> binWaitStages;
    bool timelinesAvailable = true;
    for (auto& w : waitSemaphores)
    {
        auto it = m_TimelineSemaphores.find(w.first.id);
        if (it == m_TimelineSemaphores.end() || !it->second.isTimeline)
        {
            timelinesAvailable = false;
            break;
        }
    }
    for (auto& s : signalSemaphores)
    {
        auto it = m_TimelineSemaphores.find(s.first.id);
        if (it == m_TimelineSemaphores.end() || !it->second.isTimeline)
        {
            timelinesAvailable = false;
            break;
        }
    }

    // Compute whether this is the first graphics submit after AcquireNextImage (needs binary imageAvailable wait)
    bool shouldConsumeAcquire = (queue == QueueType::Graphics) && m_Swapchain != VK_NULL_HANDLE && m_AcquiredThisFrame && !m_WaitedOnImageAvailableThisFrame;
    const bool shouldRecordTargetProgress = (queue == QueueType::Graphics) &&
                                            (touchesSwapchainPath || m_DidBackbufferRenderThisFrame || shouldConsumeAcquire);
    // Decide if we can use vkQueueSubmit2: requires both timeline semaphores and synchronization2 feature,
    // and we must not include a binary acquire wait (imageAvailable) in a submit2 batch
    // because validation treats WSI signaled binaries differently.
    PFN_vkQueueSubmit2 queueSubmit2 = GetQueueSubmit2();
    bool useSubmit2 = timelinesAvailable && SupportsSynchronization2() && queueSubmit2 && !shouldConsumeAcquire;

    if (useSubmit2)
    {
        waitInfos.reserve(waitSemaphores.size() + 8);
        // Do NOT consume imageAvailable in submit2 path (handled in legacy submit path when needed)
        for (auto& w : waitSemaphores)
        {
            VkSemaphore sem = m_TimelineSemaphores[w.first.id].sem;
            VkSemaphoreSubmitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            info.semaphore = sem;
            info.value = w.second;
            info.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            waitInfos.push_back(info);
        }
        std::vector<VkCommandBufferSubmitInfo> cbInfos;
        cbInfos.reserve(cbs.size());
        for (auto cb : cbs)
        {
            VkCommandBufferSubmitInfo cbsi{};
            cbsi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
            cbsi.commandBuffer = cb;
            cbInfos.push_back(cbsi);
        }
        std::vector<VkSemaphoreSubmitInfo> signalInfos;
        signalInfos.reserve(signalSemaphores.size() + 2);
        for (auto& s : signalSemaphores)
        {
            VkSemaphore sem = m_TimelineSemaphores[s.first.id].sem;
            VkSemaphoreSubmitInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            info.semaphore = sem;
            info.value = s.second;
            info.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            signalInfos.push_back(info);
        }
        // Slot for this queue's own timeline signal, so completion tracks these command buffers.
        // The value goes in once the chokepoint allocates it; the slot is dropped on a device
        // whose queue has no timeline semaphore.
        const size_t ownSignalSlot = signalInfos.size();
        {
            VkSemaphoreSubmitInfo sig{};
            sig.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            sig.semaphore = submitContext.TimelineSemaphore();
            sig.stageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            signalInfos.push_back(sig);
        }

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = (uint32_t)waitInfos.size();
        submit.pWaitSemaphoreInfos = waitInfos.data();
        submit.pSignalSemaphoreInfos = signalInfos.data();
        submit.commandBufferInfoCount = (uint32_t)cbInfos.size();
        submit.pCommandBufferInfos = cbInfos.data();

        // No per-frame fence here; fence is signaled in Present() barrier submit once per frame.
        // Classification runs inside the chokepoint but does not decide publication: forced
        // device loss overrides a submit the GPU really accepted, and refusing to publish a
        // value the GPU will signal would hand the same value to the next submit.
        VkResult submitRes = VK_SUCCESS;
        const QueueSubmitContext::SubmitResult submitted =
            SubmitBatchToQueue(submitContext, submittedLists, [&](uint64_t signalValue)
        {
            signalInfos[ownSignalSlot].value = signalValue;
            submit.signalSemaphoreInfoCount =
                (uint32_t)(signalValue != 0 ? signalInfos.size() : ownSignalSlot);

            submitRes = queueSubmit2(vkQueue, 1, &submit, VK_NULL_HANDLE);
            // Answer taken before classification, which rewrites submitRes to
            // VK_ERROR_DEVICE_LOST under fault injection.
            const bool submitSucceeded = submitRes == VK_SUCCESS;
            classifyQueueSubmit(submitRes);
            return submitSucceeded;
        });

        if (submitted.Submitted)
        {
            m_DeviceKnownIdle = false;
            if (shouldRecordTargetProgress)
            {
                RecordActiveWindowTargetGraphicsProgress(submitted.SignalledValue);
            }
        }
        return submitRes == VK_SUCCESS;
    }
    else
    {
        // Legacy submit path (vkQueueSubmit) or special-case first-graphics-after-acquire.
        // IMPORTANT: we still need correct timeline waits/signals via VkTimelineSemaphoreSubmitInfo.

        std::vector<VkSemaphore> waitSems;
        std::vector<uint64_t> waitValues;
        std::vector<VkPipelineStageFlags> waitStagesLocal;
        waitSems.reserve(waitSemaphores.size() + 4);
        waitValues.reserve(waitSemaphores.size() + 4);
        waitStagesLocal.reserve(waitSemaphores.size() + 4);

        // Caller-provided timeline waits
        for (auto& w : waitSemaphores)
        {
            auto it = m_TimelineSemaphores.find(w.first.id);
            if (it == m_TimelineSemaphores.end() || it->second.sem == VK_NULL_HANDLE || !it->second.isTimeline)
                continue;
            waitSems.push_back(it->second.sem);
            waitValues.push_back(w.second);
            waitStagesLocal.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        // First-graphics-after-acquire: add imageAvailable binary wait (value=0)
        if (shouldConsumeAcquire && m_CurrentFrame < m_ImageAvailableSemaphores.size())
        {
            VkSemaphore acquireSem = m_ImageAvailableSemaphores[m_CurrentFrame];
            if (acquireSem != VK_NULL_HANDLE)
            {
                waitSems.push_back(acquireSem);
                waitValues.push_back(0ull);
                waitStagesLocal.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                m_WaitedOnImageAvailableThisFrame = true;
            }
        }

        std::vector<VkSemaphore> signalSems;
        std::vector<uint64_t> signalValues;
        signalSems.reserve(signalSemaphores.size() + 2);
        signalValues.reserve(signalSemaphores.size() + 2);

        // Caller-provided timeline signals
        for (auto& s : signalSemaphores)
        {
            auto it = m_TimelineSemaphores.find(s.first.id);
            if (it == m_TimelineSemaphores.end() || it->second.sem == VK_NULL_HANDLE || !it->second.isTimeline)
                continue;
            signalSems.push_back(it->second.sem);
            signalValues.push_back(s.second);
        }

        // Slot for this queue's own timeline signal, so completion tracks these command buffers
        // (matches the submit2 path). The value goes in once the chokepoint allocates it; the
        // slot is dropped on a device whose queue has no timeline semaphore.
        const size_t ownSignalSlot = signalSems.size();
        signalSems.push_back(submitContext.TimelineSemaphore());
        signalValues.push_back(0ull);

        VkTimelineSemaphoreSubmitInfo tsi{};
        tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tsi.waitSemaphoreValueCount = (uint32_t)waitValues.size();
        tsi.pWaitSemaphoreValues = waitValues.empty() ? nullptr : waitValues.data();
        tsi.pSignalSemaphoreValues = signalValues.data();

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &tsi;
        submit.waitSemaphoreCount = (uint32_t)waitSems.size();
        submit.pWaitSemaphores = waitSems.empty() ? nullptr : waitSems.data();
        submit.pWaitDstStageMask = waitStagesLocal.empty() ? nullptr : waitStagesLocal.data();
        submit.commandBufferCount = (uint32_t)cbs.size();
        submit.pCommandBuffers = cbs.data();
        submit.pSignalSemaphores = signalSems.data();

        // Classification runs inside the chokepoint but does not decide publication — see the
        // submit2 path above for why a synthesized loss must not un-publish a real signal.
        VkResult submitRes = VK_SUCCESS;
        const QueueSubmitContext::SubmitResult submitted =
            SubmitBatchToQueue(submitContext, submittedLists, [&](uint64_t signalValue)
        {
            signalValues[ownSignalSlot] = signalValue;
            const uint32_t signalCount = (uint32_t)(signalValue != 0 ? signalSems.size() : ownSignalSlot);
            tsi.signalSemaphoreValueCount = signalCount;
            submit.signalSemaphoreCount = signalCount;

            submitRes = vkQueueSubmit(vkQueue, 1, &submit, VK_NULL_HANDLE);
            // Answer taken before classification, which rewrites submitRes to
            // VK_ERROR_DEVICE_LOST under fault injection.
            const bool submitSucceeded = submitRes == VK_SUCCESS;
            classifyQueueSubmit(submitRes);
            return submitSucceeded;
        });

        if (submitted.Submitted)
        {
            m_DeviceKnownIdle = false;
            if (shouldRecordTargetProgress)
            {
                RecordActiveWindowTargetGraphicsProgress(submitted.SignalledValue);
            }
        }

        return (submitRes == VK_SUCCESS);
    }
}

IDevice::GpuSyncToken VulkanDevice::SubmitTextureUploads(const TextureUploadRequest* requests, uint32_t count)
{
    if (!requests || count == 0)
        return {};

    // Q6 slice 2 upload-worker quiesce (runs on JOB threads). Hold the rebuild lock
    // SHARED for the WHOLE upload — staging-buffer/VMA prep through submit — so an
    // in-place rebuild's EXCLUSIVE lock cannot tear down VMA/queues while this worker
    // is mid-flight. The health re-check below runs UNDER the lock, so a worker that
    // passed a now-stale check then descheduled across a loss observes the fresh
    // state (and a rebuild in progress blocks here until it finishes). Uncontended
    // shared acquisition is ~free, so the healthy steady state pays nothing. Taken
    // via the guard so the nested CreateUploadBuffer/UpdateBuffer/DestroyBuffer
    // entries skip re-acquisition through its thread-local.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Create);
    if (m_RecoveryEnabled && !m_Health.IsDeviceUsable())
        return {};

    // Require a dedicated transfer queue and its timeline. The context caches the VkSemaphore,
    // so this worker never touches the m_TimelineSemaphores map.
    QueueSubmitContext& transferContext = SubmitContextFor(QueueType::Transfer);
    if (m_TransferQueue == VK_NULL_HANDLE || !transferContext.HasTimeline())
    {
        Logger::Log::Warning("SubmitTextureUploads: no dedicated transfer queue available");
        return {};
    }

    // 1. Create staging buffers and copy pixel data (thread-safe via VMA).
    constexpr uint32_t kStackCapacity = 4;
    BufferHandle stackBuf[kStackCapacity]{};
    std::vector<BufferHandle> heapBuf;
    const bool useHeap = count > kStackCapacity;
    if (useHeap)
        heapBuf.resize(count);
    auto* staging = useHeap ? heapBuf.data() : stackBuf;

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& req = requests[i];
        const size_t uploadSize = static_cast<size_t>(req.Height) * req.RowPitchBytes;
        staging[i] = CreateUploadBuffer(uploadSize, "TexUploadStaging");
        if (staging[i].IsValid())
            UpdateBuffer(staging[i], 0, uploadSize, req.Pixels);
    }

    // 2. Record transfer commands using per-thread command pool.
    // CopyBufferToTextureSubresource handles Undefined → CopyDest internally,
    // so only the post-copy CopyDest → ShaderResource barrier is needed.
    auto cl = CreateCommandList(QueueType::Transfer);
    cl->Begin();
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!staging[i].IsValid())
            continue;
        const auto& req = requests[i];
        cl->CopyBufferToTextureSubresource(
            staging[i], req.Texture, req.MipLevel, req.ArrayLayer,
            req.Width, req.Height, 0, req.RowPitchBytes);
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(
            req.Texture, ResourceState::CopyDest, ResourceState::ShaderResource,
            req.MipLevel, 1, req.ArrayLayer, 1));
    }
    cl->End();

    auto* vcl = static_cast<VulkanCommandList*>(cl.get());
    VkCommandBuffer cb = vcl->GetVkCommandBuffer();
    if (cb == VK_NULL_HANDLE || !vcl->IsReadyToSubmit())
    {
        for (uint32_t i = 0; i < count; ++i)
            if (staging[i].IsValid())
                DestroyBuffer(staging[i]);
        return {};
    }

    // 3. Submit to transfer queue with timeline signal.
    //    Uses the context's cached VkSemaphore to avoid a m_TimelineSemaphores map race.
    //    Uses legacy vkQueueSubmit path (compatible with Vulkan 1.2 without sync2).
    //    The routing below is this path's own (it defers staging buffers alongside the CB),
    //    so it takes the value the chokepoint returns rather than going through
    //    SubmitBatchToQueue.
    const VkSemaphore transferTimelineSem = transferContext.TimelineSemaphore();
    const QueueSubmitContext::SubmitResult submitted =
        transferContext.SubmitAndSignal([&](uint64_t signalValue)
    {
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;

        VkTimelineSemaphoreSubmitInfo tsInfo{};
        tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        uint64_t signalValues[1] = {signalValue};
        tsInfo.signalSemaphoreValueCount = 1;
        tsInfo.pSignalSemaphoreValues = signalValues;
        si.pNext = &tsInfo;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &transferTimelineSem;

        const VkResult result = vkQueueSubmit(m_TransferQueue, 1, &si, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
        {
            Logger::Log::Error("SubmitTextureUploads: vkQueueSubmit failed ({})", (int)result);
            if (result == VK_ERROR_DEVICE_LOST && m_RecoveryEnabled)
            {
                // Job thread: hand off rather than observe here (see the thread
                // contract on OnDeviceLostObserved). Two relaxed/release atomics, so
                // holding the submit mutex across this cannot invert any lock order.
                NoteDeviceLostOnWorkerThread();
            }
            return false;
        }
        m_DeviceKnownIdle = false;
        return true;
    });
    const uint64_t signaledValue = submitted.SignalledValue;

    // Submit failed — clean up staging outside the lock. Nothing was signalled, so nothing may
    // be deferred against a value the GPU will never reach.
    if (signaledValue == 0)
    {
        for (uint32_t i = 0; i < count; ++i)
            if (staging[i].IsValid())
                DestroyBuffer(staging[i]);
        return {};
    }

    // 4. Under one lock, defer everything keyed to this submit's transfer-timeline value:
    //    the command buffer for recycling and the staging buffers for destruction. The CB was
    //    submitted with a timeline signal and a NULL fence, so the per-frame transfer fence
    //    does not cover it — recycling it by frame slot (as RecycleThreadPoolsForFrame does,
    //    unconditionally) could reset a CB still pending on the GPU. RetireDeferredRecycle-
    //    CommandBuffers returns it to its owning pool's freelist once the GPU passes the
    //    signaled value. Runs on worker threads, so it deliberately never reads m_CurrentFrame
    //    (the transfer timeline is guaranteed valid here, so the frame-slot fallback in
    //    RouteThreadPoolCmdBufferForRecycle is not needed and not used).
    const SemaphoreHandle transferTimeline = transferContext.Timeline();
    {
        auto* threadPool = static_cast<ThreadCommandPoolEntry*>(vcl->GetThreadPoolOwner());
        std::lock_guard<std::mutex> lock(m_DeferredStagingMutex);
        if (threadPool && cb != VK_NULL_HANDLE)
        {
            m_DeferredRecycleCmdBuffers.push_back(
                {threadPool, cb, QueueOrdinal(QueueType::Transfer), transferTimeline, signaledValue});
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            if (staging[i].IsValid())
                m_DeferredStagingBuffers.push_back({staging[i], transferTimeline, signaledValue});
        }
    }
    vcl->OnSubmitted();

    return GpuSyncToken{transferTimeline, signaledValue};
}

std::unique_ptr<CommandList> VulkanDevice::CreateCommandList(QueueType queue)
{
    // Use per-thread command pool for thread-safe allocation
    VkCommandBuffer cmd = AcquireThreadCmdBuffer(queue);
    auto* entry = t_CachedPool.entry; // guaranteed set by AcquireThreadCmdBuffer
    VkCommandPool pool = entry->graphicsPool;
    if (queue == QueueType::Compute && entry->computePool)
        pool = entry->computePool;
    else if (queue == QueueType::Transfer && entry->transferPool)
        pool = entry->transferPool;
    auto cl = std::make_unique<VulkanCommandList>(this, queue, cmd, pool);
    cl->SetThreadPoolOwner(entry);
    return cl;
}

std::unique_ptr<CommandList> VulkanDevice::CreateSecondaryCommandList(QueueType queue)
{
    VkCommandBuffer cmd = AcquireThreadSecondaryCmdBuffer(queue);
    if (cmd == VK_NULL_HANDLE) return nullptr;
    auto* entry = t_CachedPool.entry;
    VkCommandPool pool = entry->graphicsPool;
    if (queue == QueueType::Compute && entry->computePool)
        pool = entry->computePool;
    else if (queue == QueueType::Transfer && entry->transferPool)
        pool = entry->transferPool;
    auto cl = std::make_unique<VulkanCommandList>(this, queue, cmd, pool);
    cl->SetThreadPoolOwner(entry);
    return cl;
}

void VulkanDevice::RetireSecondaryCommandLists(CommandList* const* lists, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        auto* vcl = static_cast<VulkanCommandList*>(lists[i]);
        if (!vcl) continue;
        VkCommandBuffer cb = vcl->GetVkCommandBuffer();
        auto* threadPool = static_cast<ThreadCommandPoolEntry*>(vcl->GetThreadPoolOwner());
        if (threadPool && cb != VK_NULL_HANDLE)
        {
            int ord = QueueOrdinal(vcl->GetQueueType());
            std::lock_guard<std::mutex> lock(threadPool->recycleMutex);
            threadPool->usedCmdPerFrame[m_CurrentFrame][ThreadCommandPoolEntry::kLevelSecondary][ord]
                .push_back(cb);
        }
        vcl->OnSubmitted(); // Null out CB so destructor doesn't double-recycle
    }
}

// Number of VkCommandBuffers to batch-allocate when the per-frame pool runs dry.
// Larger values front-load driver overhead and reduce per-frame allocation spikes.
static constexpr uint32_t kCmdBufferBatchAllocCount = 32;

VkCommandBuffer VulkanDevice::AcquireCmdBuffer(IDevice::QueueType queue)
{
    // Try to reuse a free command buffer for this frame and queue
    auto& fr = m_Frames[m_CurrentFrame];
    auto& pq = (queue == IDevice::QueueType::Graphics) ? fr.graphics : (queue == IDevice::QueueType::Compute ? fr.compute : fr.transfer);
    if (!pq.freeCmd.empty())
    {
        VkCommandBuffer cmd = pq.freeCmd.back();
        pq.freeCmd.pop_back();
        return cmd;
    }
    // Batch-allocate to front-load driver overhead and reduce per-frame allocation spikes.
    VkCommandPool pool = GetCommandPool(queue);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kCmdBufferBatchAllocCount;
    std::array<VkCommandBuffer, kCmdBufferBatchAllocCount> cmds{};
    VkResult result = vkAllocateCommandBuffers(m_Device, &ai, cmds.data());
    if (result != VK_SUCCESS)
    {
        // Fallback: try allocating a single buffer
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_Device, &ai, &cmd);
        return cmd;
    }
    // Return the first, put the rest in the free pool
    pq.freeCmd.reserve(pq.freeCmd.size() + kCmdBufferBatchAllocCount - 1);
    for (uint32_t i = 1; i < kCmdBufferBatchAllocCount; ++i)
    {
        pq.freeCmd.push_back(cmds[i]);
    }
    return cmds[0];
}

void VulkanDevice::RecycleCmdBuffer(IDevice::QueueType queue, VkCommandBuffer cmd)
{
    auto& fr = m_Frames[m_CurrentFrame];
    auto& pq = (queue == IDevice::QueueType::Graphics) ? fr.graphics : (queue == IDevice::QueueType::Compute ? fr.compute : fr.transfer);
    pq.freeCmd.push_back(cmd);
}

// ---------------------------------------------------------------------------
// Per-thread command pool management (Phase 0.1 multi-threading support)
// ---------------------------------------------------------------------------
thread_local VulkanDevice::ThreadPoolCache VulkanDevice::t_CachedPool = {};

uint64_t VulkanDevice::NextDeviceId()
{
    // 0 is the empty-cache sentinel, so ids start at 1.
    static std::atomic<uint64_t> s_NextDeviceId{1};
    return s_NextDeviceId.fetch_add(1, std::memory_order_relaxed);
}

VulkanDevice::ThreadCommandPoolEntry* VulkanDevice::AcquireThreadPoolEntry(IDevice::QueueType queue)
{
    // Fast path: the cache is this thread's, belongs to THIS device, and predates
    // no rebuild of it.
    const uint32_t currentGeneration = m_DeviceGeneration.load(std::memory_order_relaxed);
    if (t_CachedPool.entry && t_CachedPool.deviceId == m_DeviceId &&
        t_CachedPool.generation == currentGeneration)
        return t_CachedPool.entry;

    // Cache belongs to another live device, or to this one before a rebuild.
    t_CachedPool = {};

    auto tid = std::this_thread::get_id();

    std::lock_guard<std::mutex> lock(m_ThreadPoolsMutex);

    // Search existing entries
    for (auto& entry : m_ThreadPools)
    {
        if (entry->threadId == tid)
        {
            t_CachedPool = {entry.get(), m_DeviceId, currentGeneration};
            return t_CachedPool.entry;
        }
    }

    // Create new entry for this thread
    auto entry = std::make_unique<ThreadCommandPoolEntry>();
    entry->threadId = tid;

    // Create command pools for each queue family this thread might use
    auto createPool = [&](uint32_t queueFamily) -> VkCommandPool
    {
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = queueFamily;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkResult result = vkCreateCommandPool(m_Device, &ci, nullptr, &pool);
        if (result != VK_SUCCESS)
            Logger::Log::Error("VulkanDevice: Failed to create per-thread command pool for queue family {}", queueFamily);
        return pool;
    };

    entry->graphicsPool = createPool(m_GraphicsQueueFamily);
    if (entry->graphicsPool == VK_NULL_HANDLE)
    {
        Logger::Log::Error("VulkanDevice: Failed to create per-thread graphics command pool — cannot record");
        return nullptr;
    }
    // Must match CreateCommandPool(): kInvalidQueueFamilyIndex means no dedicated queue was found.
    // That value equals VK_QUEUE_FAMILY_IGNORED; do not pass it to vkCreateCommandPool.
    if (m_ComputeQueueFamily != kInvalidQueueFamilyIndex && m_ComputeQueueFamily != m_GraphicsQueueFamily)
        entry->computePool = createPool(m_ComputeQueueFamily);
    if (m_TransferQueueFamily != kInvalidQueueFamilyIndex && m_TransferQueueFamily != m_GraphicsQueueFamily
        && m_TransferQueueFamily != m_ComputeQueueFamily)
        entry->transferPool = createPool(m_TransferQueueFamily);

    t_CachedPool = {entry.get(), m_DeviceId, currentGeneration};
    m_ThreadPools.push_back(std::move(entry));
    return t_CachedPool.entry;
}

VkCommandBuffer VulkanDevice::AcquireThreadCmdBuffer(IDevice::QueueType queue)
{
    auto* entry = AcquireThreadPoolEntry(queue);
    if (!entry)
        return VK_NULL_HANDLE;
    int ord = QueueOrdinal(queue);

    // Try reuse from the free list. The recycleMutex protects against
    // concurrent push_back from CL destructors running on other threads.
    // In practice, cross-thread recycling is rare: submitted CLs have their
    // CB nulled by OnSubmitted(), so their destructors don't recycle.
    // The lock is uncontended in the common case (no cross-thread destroyers).
    {
        std::lock_guard<std::mutex> lock(entry->recycleMutex);
        auto& primaryFree = entry->freeCmd[ThreadCommandPoolEntry::kLevelPrimary][ord];
        if (!primaryFree.empty())
        {
            VkCommandBuffer cmd = primaryFree.back();
            primaryFree.pop_back();
            return cmd;
        }
    }

    // Determine which pool to use for this queue type.
    // When transfer and compute share the same queue family, transferPool is NULL and
    // we must fall back to computePool (not graphicsPool) to avoid a queue-family mismatch.
    VkCommandPool pool = entry->graphicsPool;
    if (queue == IDevice::QueueType::Compute && entry->computePool)
        pool = entry->computePool;
    else if (queue == IDevice::QueueType::Transfer)
        pool = entry->transferPool ? entry->transferPool : (entry->computePool ? entry->computePool : entry->graphicsPool);

    // Batch-allocate from this thread's pool (no lock — only the owning thread allocates)
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kCmdBufferBatchAllocCount;
    std::array<VkCommandBuffer, kCmdBufferBatchAllocCount> cmds{};
    VkResult result = vkAllocateCommandBuffers(m_Device, &ai, cmds.data());
    if (result != VK_SUCCESS)
    {
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(m_Device, &ai, &cmd);
        return cmd;
    }

    {
        std::lock_guard<std::mutex> lock(entry->recycleMutex);
        auto& primaryFree = entry->freeCmd[ThreadCommandPoolEntry::kLevelPrimary][ord];
        primaryFree.reserve(primaryFree.size() + kCmdBufferBatchAllocCount - 1);
        for (uint32_t i = 1; i < kCmdBufferBatchAllocCount; ++i)
            primaryFree.push_back(cmds[i]);
    }

    return cmds[0];
}

VkCommandBuffer VulkanDevice::AcquireThreadSecondaryCmdBuffer(IDevice::QueueType queue)
{
    auto* entry = AcquireThreadPoolEntry(queue);
    if (!entry)
        return VK_NULL_HANDLE;
    int ord = QueueOrdinal(queue);

    // Reuse a recycled secondary from this thread's pool if one is available.
    // BeginSecondary resets the buffer before recording, so a returned-but-unreset
    // CB is fine. The recycleMutex guards a CL destructor pushing from another thread.
    {
        std::lock_guard<std::mutex> lock(entry->recycleMutex);
        auto& secondaryFree = entry->freeCmd[ThreadCommandPoolEntry::kLevelSecondary][ord];
        if (!secondaryFree.empty())
        {
            VkCommandBuffer cmd = secondaryFree.back();
            secondaryFree.pop_back();
            return cmd;
        }
    }

    // Determine which pool to use.
    // When transfer and compute share the same queue family, transferPool is NULL and
    // we must fall back to computePool (not graphicsPool) to avoid a queue-family mismatch.
    VkCommandPool pool = entry->graphicsPool;
    if (queue == IDevice::QueueType::Compute && entry->computePool)
        pool = entry->computePool;
    else if (queue == IDevice::QueueType::Transfer)
        pool = entry->transferPool ? entry->transferPool : (entry->computePool ? entry->computePool : entry->graphicsPool);

    // Miss: allocate a fresh secondary. Recycled secondaries re-enter
    // freeCmd[secondary] via RetireSecondaryCommandLists + RecycleThreadPoolsForFrame,
    // so after warm-up the freelist above absorbs steady-state demand.
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(m_Device, &ai, &cmd);
    if (result != VK_SUCCESS)
    {
        Logger::Log::Error("VulkanDevice: Failed to allocate secondary command buffer");
        return VK_NULL_HANDLE;
    }
    return cmd;
}

void VulkanDevice::RecycleThreadPoolsForFrame(uint32_t frameSlot)
{
    std::lock_guard<std::mutex> lock(m_ThreadPoolsMutex);
    for (auto& entry : m_ThreadPools)
    {
        std::lock_guard<std::mutex> rlock(entry->recycleMutex);
        for (int lvl = 0; lvl < ThreadCommandPoolEntry::kLevelCount; ++lvl)
        {
            for (int q = 0; q < 3; ++q)
            {
                auto& used = entry->usedCmdPerFrame[frameSlot][lvl][q];
                if (used.empty())
                    continue;
                auto& free = entry->freeCmd[lvl][q];
                free.reserve(free.size() + used.size());
                free.insert(free.end(), used.begin(), used.end());
                used.clear();
            }
        }
    }
}

void VulkanDevice::DestroyAllThreadPools()
{
    std::lock_guard<std::mutex> lock(m_ThreadPoolsMutex);
    for (auto& entry : m_ThreadPools)
    {
        if (entry->graphicsPool) vkDestroyCommandPool(m_Device, entry->graphicsPool, nullptr);
        if (entry->computePool)  vkDestroyCommandPool(m_Device, entry->computePool, nullptr);
        if (entry->transferPool) vkDestroyCommandPool(m_Device, entry->transferPool, nullptr);
    }
    m_ThreadPools.clear();
    // Bump this device's generation so caches other threads took from it before
    // the teardown miss. Caches belonging to a DIFFERENT device are rejected by
    // the id, not by this counter.
    m_DeviceGeneration.fetch_add(1, std::memory_order_relaxed);
    // Clear this thread's cache
    t_CachedPool = {};
}

void VulkanDevice::ExecuteCommandLists(const std::vector<CommandList*>& commandLists)
{
    ExecuteCommandListsTracked(commandLists);
}

VulkanDevice::SubmittedBatchTokens
VulkanDevice::ExecuteCommandListsTracked(const std::vector<CommandList*>& commandLists)
{
    SubmittedBatchTokens tokens{};

    if (commandLists.empty())
    {
        return tokens;
    }

    // Q6 mid-frame short-circuit (design F10): a latched device loss means no
    // submit may reach the dead device.
    if (m_RecoveryEnabled && m_Health.IsLost())
    {
        return tokens;
    }

    // Partition by intended queue, keeping each list beside its buffer. Each queue's submit
    // routes exactly the lists it carried, against the value that submit signalled — a list
    // tagged with another queue's (or another submit's) value is one whose buffer gets recycled
    // while the GPU is still executing it.
    std::vector<VkCommandBuffer> gfxCBs, cmpCBs, xferCBs;
    std::vector<VulkanCommandList*> gfxLists, cmpLists, xferLists;
    gfxCBs.reserve(commandLists.size());
    cmpCBs.reserve(commandLists.size());
    xferCBs.reserve(commandLists.size());
    gfxLists.reserve(commandLists.size());
    cmpLists.reserve(commandLists.size());
    xferLists.reserve(commandLists.size());

    for (CommandList* cmdList : commandLists)
    {
        VulkanCommandList* vulkanCmdList = static_cast<VulkanCommandList*>(cmdList);
        if (vulkanCmdList && vulkanCmdList->IsReadyToSubmit())
        {
            VkCommandBuffer vkCmdBuffer = vulkanCmdList->GetVkCommandBuffer();
            if (vkCmdBuffer != VK_NULL_HANDLE)
            {
                switch (vulkanCmdList->GetQueueType())
                {
                case IDevice::QueueType::Compute:
                    cmpCBs.push_back(vkCmdBuffer);
                    cmpLists.push_back(vulkanCmdList);
                    break;
                case IDevice::QueueType::Transfer:
                    xferCBs.push_back(vkCmdBuffer);
                    xferLists.push_back(vulkanCmdList);
                    break;
                case IDevice::QueueType::Graphics:
                default:
                    gfxCBs.push_back(vkCmdBuffer);
                    gfxLists.push_back(vulkanCmdList);
                    break;
                }
            }
        }
    }

    bool graphicsTouchesSwapchainPath = false;
    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0)
    {
        for (CommandList* cmdList : commandLists)
        {
            VulkanCommandList* vulkanCmdList = static_cast<VulkanCommandList*>(cmdList);
            if (!vulkanCmdList || vulkanCmdList->GetQueueType() != IDevice::QueueType::Graphics)
                continue;
            for (const auto& use : vulkanCmdList->GetUsedResources())
            {
                if (use.ResourceType != VulkanCommandList::UsedResource::Type::Texture)
                    continue;
                if (IsSwapchainTextureHandle(TextureHandle(use.Id.id)))
                {
                    graphicsTouchesSwapchainPath = true;
                    break;
                }
            }
            if (graphicsTouchesSwapchainPath)
                break;
        }
    }

    // Submits one queue's batch through that queue's chokepoint, signaling its timeline so
    // deferred retirement can track when the GPU finishes, and routing the batch's lists
    // against the value this submit signalled.
    auto submitBatch = [&](QueueType role, const std::vector<VkCommandBuffer>& cbs,
                           const std::vector<VulkanCommandList*>& lists) -> GpuSyncToken
    {
        if (cbs.empty())
            return {};

        QueueSubmitContext& context = SubmitContextFor(role);
        if (context.Queue() == VK_NULL_HANDLE)
            return {};

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = static_cast<uint32_t>(cbs.size());
        si.pCommandBuffers = cbs.data();

        VkSemaphore signalSem[1] = {context.TimelineSemaphore()};
        uint64_t signalVal[1] = {};
        VkTimelineSemaphoreSubmitInfo tsInfo{};
        tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tsInfo.signalSemaphoreValueCount = 1;
        tsInfo.pSignalSemaphoreValues = signalVal;

        const QueueSubmitContext::SubmitResult submitted =
            SubmitBatchToQueue(context, lists, [&](uint64_t signalValue)
        {
            if (signalValue != 0)
            {
                signalVal[0] = signalValue;
                si.pNext = &tsInfo;
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = signalSem;
            }
            return vkQueueSubmit(context.Queue(), 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
        });

        if (!submitted.Submitted)
            return {};
        m_DeviceKnownIdle = false;
        return GpuSyncToken{context.Timeline(), submitted.SignalledValue};
    };

    // Graphics submit retains existing swapchain wait behavior
    if (!gfxCBs.empty())
    {
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        // Arrays used for waiting on the swapchain image-available semaphore when needed.
        // These must outlive the vkQueueSubmit call, so they are declared in the same
        // scope as submitInfo (not inside the inner conditional block).
        VkSemaphore waitSemaphores[1] = {};
        VkPipelineStageFlags waitStages[1] = {};

        if (m_Swapchain != VK_NULL_HANDLE && m_AcquiredThisFrame && !m_WaitedOnImageAvailableThisFrame &&
            m_CurrentFrame < m_ImageAvailableSemaphores.size())
        {
            VkSemaphore waitSemaphore = m_ImageAvailableSemaphores[m_CurrentFrame];
            if (waitSemaphore != VK_NULL_HANDLE && waitSemaphore == m_AcquireSemaphoreThisFrame)
            {
                waitSemaphores[0] = waitSemaphore;
                // IMPORTANT: The first graphics submission in a frame may be a non-render pass
                // (e.g., texture uploads). If we wait only at COLOR_ATTACHMENT_OUTPUT, that
                // wait may not block anything in such submissions. Use ALL_COMMANDS so the
                // acquire wait is effective regardless of the submission contents.
                waitStages[0] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

                submitInfo.waitSemaphoreCount = 1;
                submitInfo.pWaitSemaphores = waitSemaphores;
                submitInfo.pWaitDstStageMask = waitStages;
                m_WaitedOnImageAvailableThisFrame = true;
                m_AcquireSemaphoreThisFrame = VK_NULL_HANDLE;
            }
        }

        submitInfo.commandBufferCount = static_cast<uint32_t>(gfxCBs.size());
        submitInfo.pCommandBuffers = gfxCBs.data();

        // Signal the graphics timeline from this submit when a real timeline semaphore
        // exists. This keeps timeline-deferred resource retirement working even on
        // drivers where synchronization2 is unavailable.
        QueueSubmitContext& graphicsContext = SubmitContextFor(QueueType::Graphics);
        VkSemaphore signalSemaphores[1] = {graphicsContext.TimelineSemaphore()};
        uint64_t signalValues[1] = {};
        VkTimelineSemaphoreSubmitInfo timelineInfo{};
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = signalValues;

        // Fault injection overrides the result AFTER the chokepoint has published: the real
        // submit succeeded, so the GPU will signal that value, and withholding it would hand
        // the same value to the next submit.
        VkResult result = VK_SUCCESS;
        const QueueSubmitContext::SubmitResult submitted =
            SubmitBatchToQueue(graphicsContext, gfxLists, [&](uint64_t signalValue)
        {
            if (signalValue != 0)
            {
                signalValues[0] = signalValue;
                submitInfo.pNext = &timelineInfo;
                submitInfo.signalSemaphoreCount = 1;
                submitInfo.pSignalSemaphores = signalSemaphores;
            }
            result = vkQueueSubmit(graphicsContext.Queue(), 1, &submitInfo, VK_NULL_HANDLE);
            return result == VK_SUCCESS;
        });

        const uint64_t graphicsTimelineSignalValue = submitted.SignalledValue;
        bool lossSynthesized = false;
        if (m_FaultInjection.Active())
        {
            ++m_FaultSubmitCounter;
            if (m_FaultInjection.ShouldForceLostAtFrame(m_FaultFrameCounter) ||
                m_FaultInjection.ShouldForceLostAtSubmit(m_FaultSubmitCounter))
            {
                // The real submit succeeded; we override the result so the loss
                // path runs against a live device (the queued work simply drains).
                Logger::Log::Warning("VulkanDevice: [fault-injection] forcing DEVICE_LOST at graphics submit (frame {}, submit {})",
                                     m_FaultFrameCounter, m_FaultSubmitCounter);
                result = VK_ERROR_DEVICE_LOST;
                lossSynthesized = true;
                // Consume so the rebuilt device is not immediately re-lost every
                // frame; a queued target (GE_VK_FORCE_DEVICE_LOST=N1,N2,...) arms
                // next, which is what makes repeat-loss reachable on this path.
                m_FaultInjection.ConsumeLostTarget();
            }
        }
        if (result != VK_SUCCESS)
        {
            Logger::Log::Error("Failed to submit graphics command buffers! Error: {}", (int)result);
            if (result == VK_ERROR_DEVICE_LOST)
            {
                if (m_RecoveryEnabled)
                {
                    OnDeviceLostObserved("graphics queue submit", result, /*deviceActuallyLost=*/!lossSynthesized);
                }
                else
                {
                    // Legacy (GE_DEVICE_RECOVERY=0): lightweight quiesce + swapchain
                    // refresh, no latch — the pre-Q6 behavior.
                    if (m_Device)
                        vkDeviceWaitIdle(m_Device);
                    if (m_SwapchainExtent.width > 0 && m_SwapchainExtent.height > 0)
                    {
                        const WindowTargetHandle activeTarget = GetActiveWindowTarget();
                        if (activeTarget.IsValid())
                        {
                            RecreateWindowTargetSwapchain(activeTarget, m_SwapchainExtent.width, m_SwapchainExtent.height);
                        }
                    }
                }
            }
        }
        else
        {
            m_AnyGraphicsSubmitThisFrame = true;
            m_DeviceKnownIdle = false;
            tokens.graphics = GpuSyncToken{graphicsContext.Timeline(), graphicsTimelineSignalValue};
            if (graphicsTimelineSignalValue != 0 &&
                (graphicsTouchesSwapchainPath || m_DidBackbufferRenderThisFrame))
            {
                RecordActiveWindowTargetGraphicsProgress(graphicsTimelineSignalValue);
            }
        }
    }

    // Q6 mid-frame short-circuit (design F10): if the graphics submit above
    // latched a device loss, issue no further submits — nothing may touch the dead
    // device for the remainder of this frame walk. The graphics batch above was
    // already routed for retirement inside its own submit, so returning here can no
    // longer strand a submitted buffer for the destructor to recycle mid-flight.
    if (m_RecoveryEnabled && m_Health.IsLost())
    {
        return tokens;
    }

    // Compute/Transfer: submit through their own queues' chokepoints. A role with no dedicated
    // queue resolves to the graphics context, so it takes the graphics lock and signals the
    // graphics timeline — the queue its work actually runs on. Kept after the graphics submit:
    // on such a device these batches share the graphics queue, where submission order is
    // execution order.
    tokens.compute = submitBatch(QueueType::Compute, cmpCBs, cmpLists);
    tokens.transfer = submitBatch(QueueType::Transfer, xferCBs, xferLists);
    return tokens;
}

// A hung GPU that stays hung this long is treated as a device loss rather than
// waited on forever (design §7 / open-question #2 — Linux/macOS only; Windows
// reaches Lost via TDR). Generous so a legitimate cold-compile stall (~1.6 s) is
// never mistaken for a wedge.
static constexpr auto kHungEscalationCap = std::chrono::seconds(30);
// Perceptible-stall threshold after which the "GPU busy" surfacing kicks in
// while holding the last-good frame (design M4).
static constexpr double kHungSurfaceThresholdMs = 250.0;
// Minimum spacing between "GPU busy" surfacing logs so a long hang doesn't spam.
static constexpr auto kHungSurfaceLogInterval = std::chrono::milliseconds(1000);

void VulkanDevice::TriggerGpuHang()
{
    if (m_Device == VK_NULL_HANDLE || m_GraphicsQueue == VK_NULL_HANDLE)
        return;

    // Lazy-build the push-constant-gated infinite-loop compute pipeline (no
    // descriptor sets). SPIR-V is embedded so this needs no shader staging.
    if (m_GpuHangPipeline == VK_NULL_HANDLE)
    {
        const auto& spirv = Detail::kGpuHangComputeSpirv;
        std::vector<uint8_t> bytes(spirv.size() * sizeof(uint32_t));
        std::memcpy(bytes.data(), spirv.data(), bytes.size());
        PipelineCreationResult r = CreateComputePipeline(bytes, {});
        if (r.pipeline == VK_NULL_HANDLE)
        {
            Logger::Log::Error("VulkanDevice: [fault-injection] GPU-hang pipeline creation failed");
            return;
        }
        m_GpuHangPipeline = r.pipeline;
        m_GpuHangPipelineLayout = r.layout;
    }

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_CommandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_Device, &ai, &cb) != VK_SUCCESS)
        return;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_GpuHangPipeline);
    const uint32_t sentinel = 1u; // shader loops while this != 0; nothing clears it
    vkCmdPushConstants(cb, m_GpuHangPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(sentinel), &sentinel);
    vkCmdDispatch(cb, 1024, 1, 1); // 1024 * 64 = 65536 invocations spinning forever
    vkEndCommandBuffer(cb);

    Logger::Log::Warning(
        "VulkanDevice: [fault-injection] submitting non-terminating compute dispatch on frame {} "
        "— expect a REAL GPU TDR + DEVICE_LOST (adapter will flash/stall ~2s)",
        m_FaultFrameCounter);

    // Fire-and-forget on the render thread's graphics queue: the GPU never completes
    // this, so the next fence wait sees the OS TDR reset as VK_ERROR_DEVICE_LOST. The
    // command buffer is intentionally not freed — the ensuing rebuild resets the pool.
    // Signals no timeline value, so it goes through the queue lock without consuming one.
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    SubmitContextFor(QueueType::Graphics).WithQueueLocked([&] {
        return vkQueueSubmit(m_GraphicsQueue, 1, &si, VK_NULL_HANDLE);
    });
}

void VulkanDevice::EmitGpuCheckpoint(VkCommandBuffer commandBuffer, const char* name)
{
    // Gated by GpuCheckpointsEnabled() at the call site, which is what keeps a
    // disabled build down to one bool load; both members are non-null when set.
    assert(m_GpuCheckpointsEnabled && m_FpCmdSetCheckpointNV && m_GpuCheckpointNames &&
           "EmitGpuCheckpoint requires the GpuCheckpointsEnabled() gate at the call site");
    if (const void* payload = m_GpuCheckpointNames->Intern(name))
    {
        m_FpCmdSetCheckpointNV(commandBuffer, payload);
    }
}

void VulkanDevice::OnDeviceLostObserved(const char* site, VkResult result, bool deviceActuallyLost)
{
    // Only the first edge into Lost emits the diagnostic; later observations
    // (other sites this frame, or repeated frames) are silent.
    if (m_Health.TransitionToLost(std::chrono::steady_clock::now(), kRecoveryStickWindow))
    {
        Logger::Log::Error(
            "VulkanDevice: device lost observed at {} (VkResult={}{}); rendering suppressed while recovery rebuilds the device in place",
            site, (int)result, deviceActuallyLost ? "" : ", synthesized");
        // Recorded (CPU) markers run ahead of GPU execution, so this is a
        // locality hint (newest last), not a precise executed-culprit.
        Logger::Log::Error("VulkanDevice: recent GPU markers (recorded, newest last): {}",
                           DumpRecentDiagnosticMarkers());
        // Checkpoint retrieval is valid at any time, so it runs either way.
        ReportGpuCheckpointsOnLoss();
        // Runs on the same edge, after the checkpoints: checkpoints bracket WHERE
        // execution stopped, these say WHAT the hardware faulted on. Must happen
        // before any rebuild disposes the lost device.
        ReportDeviceFaultOnLoss(deviceActuallyLost);
    }
}

void VulkanDevice::ReportDeviceFaultOnLoss(bool deviceActuallyLost)
{
    // Every branch here writes m_LastDeviceFaultReport and logs, because an
    // absent fault report is a missing instrument rather than a clean bill of
    // health, and the two have been confused before.
    if (!deviceActuallyLost)
    {
        // VUID-vkGetDeviceFaultInfoEXT-device-07336: the device must be in the
        // _lost_ state. A synthesized loss runs against a live device, so the
        // query is undefined here and anything it returned would describe a fault
        // that never happened.
        m_LastDeviceFaultReport = "<not queried: loss was synthesized, device is not in the lost state>";
        Logger::Log::Error("VulkanDevice: device-fault records NOT QUERIED — this loss was synthesized (fault "
                           "injection or hung-escalation cap), so the device is still alive and the query would "
                           "be invalid");
        return;
    }
    if (m_FpGetDeviceFaultInfoEXT == nullptr)
    {
        m_LastDeviceFaultReport = "<device-fault retrieval unavailable>";
        Logger::Log::Error("VulkanDevice: device-fault records UNAVAILABLE (VK_EXT_device_fault not enabled on this device) — "
                           "no faulting address can be reported for this loss");
        return;
    }
    m_LastDeviceFaultReport = QueryAndFormatDeviceFault(m_FpGetDeviceFaultInfoEXT, m_Device);
    Logger::Log::Error("VulkanDevice: device fault records: {}", m_LastDeviceFaultReport);
}

void VulkanDevice::NoteDeviceLostOnWorkerThread()
{
    m_PendingWorkerDeviceLoss.Note(m_DeviceGeneration.load(std::memory_order_relaxed));
}

void VulkanDevice::DrainWorkerDeviceLossObservation()
{
    // Claim() drops a record the rebuild already overtook: CleanupVulkan ->
    // DestroyAllThreadPools bumps the generation, and the device that died no longer
    // exists, so observing it would latch Lost onto the healthy replacement.
    if (m_PendingWorkerDeviceLoss.Claim(m_DeviceGeneration.load(std::memory_order_relaxed)))
    {
        // A real VK_ERROR_DEVICE_LOST off the worker's own vkQueueSubmit, and the
        // matching generation means that device is still the current one, so the
        // fault query is valid against it.
        OnDeviceLostObserved("texture upload submit", VK_ERROR_DEVICE_LOST, /*deviceActuallyLost=*/true);
    }
}

void VulkanDevice::ReportGpuCheckpointsOnLoss()
{
    // The executed side of the same story: the ring above says how far RECORDING
    // got, these say how far the GPU actually EXECUTED, and the region between
    // them is where the fault lives. Retrieval is defined to be valid after
    // VK_ERROR_DEVICE_LOST — that is the extension's entire purpose — and the
    // payloads stay resolvable because the name table outlives the loss.
    if (m_FpGetQueueCheckpointDataNV != nullptr && m_GpuCheckpointNames)
    {
        // Present commonly aliases graphics; the formatter drops duplicates.
        const VkQueue queues[] = {m_GraphicsQueue, m_ComputeQueue, m_TransferQueue, m_PresentQueue};
        static constexpr const char* kQueueLabels[] = {"graphics", "compute", "transfer", "present"};
        // Each queue's family decides whether it can report checkpoints at all.
        const VkPipelineStageFlags queueStages[] = {
            CheckpointStagesForFamily(m_GraphicsQueueFamily),
            CheckpointStagesForFamily(GetComputeQueueFamilyIndex()),
            CheckpointStagesForFamily(GetTransferQueueFamilyIndex()),
            CheckpointStagesForFamily(m_PresentQueueFamily)};
        constexpr uint32_t kQueueCount = static_cast<uint32_t>(std::size(queues));
        static_assert(std::size(queues) == std::size(kQueueLabels),
                      "queue handles and their labels must stay parallel");
        static_assert(std::size(queues) == std::size(queueStages),
                      "queue handles and their family stage masks must stay parallel");
        m_LastGpuCheckpointReport =
            FormatExecutedGpuCheckpoints(m_FpGetQueueCheckpointDataNV, queues, kQueueLabels, queueStages,
                                         kQueueCount, *m_GpuCheckpointNames);
        Logger::Log::Error("VulkanDevice: GPU-executed checkpoints (last per pipeline stage, newest last): {}",
                           m_LastGpuCheckpointReport);
    }
    else if (GpuCheckpointsAvailable())
    {
        Logger::Log::Error("VulkanDevice: GPU checkpoints are supported but were not recording — "
                           "re-run with GE_VK_GPU_CHECKPOINTS=1 to get the executed side of this gap");
    }
}

void VulkanDevice::OnFenceTimeout(const char* queueName)
{
    // Healthy -> Hung. A finite-wait fence timeout means the GPU is alive but
    // behind; hold the last-good frame and let the caller poll to resume.
    if (m_Health.TransitionToHung(std::chrono::steady_clock::now()))
    {
        m_LastHungSurfaceLog = {}; // fresh episode: allow immediate surfacing once past threshold
        Logger::Log::Warning(
            "VulkanDevice: BeginFrame {} fence wait timed out; entering Hung (GPU alive but behind) — holding last-good frame",
            queueName);
    }
}

void VulkanDevice::MaybeSurfaceHungFrame(const char* queueName, std::chrono::steady_clock::time_point now)
{
    const double elapsedMs = m_Health.HungElapsedMs(now);
    if (elapsedMs < kHungSurfaceThresholdMs)
    {
        return;
    }
    // The last-good frame is already on screen (we simply skip BeginFrame, so the
    // compositor holds the prior image). Log-based surfacing is the smallest
    // honest v1 signal; the health accessor lets the editor raise a real toast
    // (slice 6). Throttled so a long hang does not flood the log.
    if (m_LastHungSurfaceLog == std::chrono::steady_clock::time_point{} ||
        (now - m_LastHungSurfaceLog) >= kHungSurfaceLogInterval)
    {
        m_LastHungSurfaceLog = now;
        Logger::Log::Warning("VulkanDevice: GPU busy ({} fence still pending after {:.0f} ms) — holding last-good frame",
                             queueName, elapsedMs);
    }
}

// Begin a new frame: wait/reset fence for this frame index, recycle command buffers, and acquire swapchain image once
// Reset graphics timeline value consumption marker
// Note: We do not reset the actual timeline semaphore; we only reset the per-frame usage flags
// so Present can decide whether to wait on it.
// Each queue's last-signalled counter persists as it is monotonically increasing.

bool VulkanDevice::BeginFrame()
{
    // The frame-driving thread owns this device (see m_DeviceOwnerThread).
    // Refreshed here rather than trusted from bringup alone, so a host that
    // initializes on one thread and drives frames on another still asserts
    // against the thread that actually runs the deferred-drain sweeps.
    m_DeviceOwnerThread.store(std::this_thread::get_id(), std::memory_order_relaxed);

    // Perform any loss a job thread handed off, before the health gate below can
    // consume the frame — otherwise a worker-observed loss would never be latched,
    // logged, or checkpoint-dumped by anyone.
    if (m_RecoveryEnabled)
    {
        DrainWorkerDeviceLossObservation();
    }

    // Q6 Tier-2 (design §7 / scope item 6). Single relaxed health load per frame
    // (steady-state cost unchanged from slices 0/1). BeginFrame only SUPPRESSES
    // rendering for any non-Healthy state; the rebuild RETRY is driven separately by
    // TickDeviceRecovery on the unconditional render-loop tick (a failed rebuild
    // suppresses rendering, so a BeginFrame-driven retry would starve — the M3 gap).
    {
        const DeviceHealth h = m_Health.Load();
        if (m_RecoveryEnabled &&
            (h == DeviceHealth::Lost || h == DeviceHealth::Rebuilding ||
             h == DeviceHealth::AwaitingReprovision || h == DeviceHealth::Failed))
        {
            return false;
        }
    }

    // Frame serial for validation-stats first/last-occurrence stamps (relaxed atomic).
    ValidationStatsStore::Get().NoteFrameBegin();

    // Fault-injection frame ordinal (zero cost when inert).
    if (m_FaultInjection.Active())
    {
        ++m_FaultFrameCounter;
        if (m_FaultInjection.ShouldForceGpuHangAtFrame(m_FaultFrameCounter))
        {
            m_FaultInjection.hangEnabled = false; // one-shot
            TriggerGpuHang();
        }
    }

    static const bool s_DeferLogEnabled = []() -> bool
    {
        const char* v = std::getenv("GE_VK_DEFER_DESTROY_LOG");
        return (v && v[0] == '1');
    }();
    static int s_DeferLogBudget = 128;

    // Reset per-frame sync timings (best-effort stall diagnosis).
    m_LastFrameSyncTimings = {};
    m_LastFrameSyncTimings.frameIndex = m_CurrentFrame;

    // Clear debug barrier capture each frame to prevent unbounded growth.
    ClearDebugBarriers();

    if (m_DeviceKnownIdle)
    {
        FlushRetiredWindowTargetSemaphores();
    }
    ++m_WindowTargetRetirementFrameCounter;
    ProcessPendingWindowTargetRetirements(false);
    MaybeWarnOrForceWindowTargetRetirement();

    // Reset retirement counters for this BeginFrame
    m_LastRetiredTexturesCount = 0;
    m_LastRetiredBuffersCount = 0;
    // Track high-water before we recycle/retire resources this frame.
    UpdateResourcePoolHighWater();
    MaybeLogLiveBufferBreakdown();

    // Wait for previous GPU work on this frame index to finish so we can safely reuse semaphores/CBs
    auto& frSyncG = m_Frames[m_CurrentFrame].graphics;
    auto& frSyncC = m_Frames[m_CurrentFrame].compute;
    auto& frSyncT = m_Frames[m_CurrentFrame].transfer;
    // Only wait on per-frame fences that were actually armed by a submission last time this frame index was active
    bool waitedG = false, waitedC = false, waitedT = false;
    auto waitFrameFence = [&](PerQueueFrame& sync, const char* queueName, bool& waited) -> bool
    {
        if (!sync.fenceArmed)
        {
            // Nothing was submitted under this slot's fence since it was last
            // waited, so the slot has no GPU work to outlive: its command buffers
            // and deferred resources are already free to retire. Reporting "not
            // waited" here would strand them until the next full device drain,
            // and runtime code no longer performs one.
            waited = true;
            return true;
        }
        if (sync.fence == VK_NULL_HANDLE)
        {
            // Armed but fenceless: GPU completion is unprovable, so retire nothing.
            return true;
        }

        // Hung poll-resume (design §7 Tier 1 / F11): once Hung, never block or
        // sleep on the render/input pump. Poll the fence non-blocking so the
        // outer loop keeps draining events, and resume the instant the GPU
        // catches up. A genuine wedge is escalated to Lost by the cap below.
        if (m_RecoveryEnabled && m_Health.IsHung())
        {
            const VkResult status = vkGetFenceStatus(m_Device, sync.fence);
            if (status == VK_SUCCESS)
            {
                vkResetFences(m_Device, 1, &sync.fence);
                sync.fenceArmed = false;
                waited = true;
                m_Health.NoteFenceSignaled(); // Hung -> Healthy
                return true;
            }
            if (status == VK_ERROR_DEVICE_LOST)
            {
                OnDeviceLostObserved("BeginFrame fence poll", status, /*deviceActuallyLost=*/true);
                return false;
            }
            // VK_NOT_READY: still hung.
            const auto pollNow = std::chrono::steady_clock::now();
            if (m_Health.ShouldEscalate(pollNow, kHungEscalationCap))
            {
                Logger::Log::Error("VulkanDevice: GPU hung > {} s on {} fence; escalating to device-lost",
                                   (long long)std::chrono::duration_cast<std::chrono::seconds>(kHungEscalationCap).count(),
                                   queueName);
                // Our own policy decision, not the driver's verdict: the fence is
                // still VK_NOT_READY, so the device is wedged but not lost.
                OnDeviceLostObserved("hung escalation cap", VK_ERROR_DEVICE_LOST, /*deviceActuallyLost=*/false);
                return false;
            }
            MaybeSurfaceHungFrame(queueName, pollNow);
            return false;
        }

        // Per-platform fence-wait position (design §7):
        //  - Windows: keep the wait effectively infinite. TDR (~2 s) converts a
        //    genuine wedge to DEVICE_LOST; a sub-TDR cap would false-Hung a
        //    legitimately slow (cold-compile) frame.
        //  - macOS: existing 2 s cap -> Hung.
        //  - Linux: no TDR, so a finite cap is the only wedge detector -> Hung,
        //    which the poll-resume above then drives.
        static constexpr uint64_t kMacDisplayMoveFenceWaitNs = 2ull * 1000ull * 1000ull * 1000ull;
        static constexpr uint64_t kLinuxFenceWaitCapNs = 5ull * 1000ull * 1000ull * 1000ull;
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
        const uint64_t waitTimeoutNs = kMacDisplayMoveFenceWaitNs;
#elif defined(_WIN32)
        const uint64_t waitTimeoutNs = UINT64_MAX;
#else
        const uint64_t waitTimeoutNs = m_RecoveryEnabled ? kLinuxFenceWaitCapNs : UINT64_MAX;
#endif
        VkResult waitResult;
        if (m_FaultInjection.Active() && m_FaultInjection.ShouldForceTimeoutAtFrame(m_FaultFrameCounter))
        {
            // Test-only: synthesize a fence timeout without blocking so the Hung
            // path is exercised deterministically (works on Windows' infinite
            // wait too). The next frame's non-blocking poll finds the fence
            // signaled and resumes.
            Logger::Log::Warning("VulkanDevice: [fault-injection] forcing {} fence timeout on frame {}", queueName, m_FaultFrameCounter);
            waitResult = VK_TIMEOUT;
        }
        else
        {
            const auto t0 = std::chrono::high_resolution_clock::now();
            waitResult = vkWaitForFences(m_Device, 1, &sync.fence, VK_TRUE, waitTimeoutNs);
            const auto t1 = std::chrono::high_resolution_clock::now();
            m_LastFrameSyncTimings.beginFrameWaitMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        }

        if (waitResult == VK_SUCCESS)
        {
            vkResetFences(m_Device, 1, &sync.fence);
            sync.fenceArmed = false;
            waited = true;
            return true;
        }

        if (waitResult == VK_TIMEOUT)
        {
            if (m_RecoveryEnabled)
            {
                // Finite-wait platforms (macOS / Linux). Enter Hung and skip this
                // frame; the poll-resume path above takes over next frame.
                OnFenceTimeout(queueName);
            }
            else
            {
                Logger::Log::Warning("VulkanDevice: BeginFrame {} fence wait timed out on frame {}; skipping render so window events can drain",
                                     queueName,
                                     (unsigned)m_CurrentFrame);
            }
        }
        else if (waitResult == VK_ERROR_DEVICE_LOST)
        {
            OnDeviceLostObserved("BeginFrame fence wait", waitResult, /*deviceActuallyLost=*/true);
        }
        else
        {
            Logger::Log::Warning("VulkanDevice: BeginFrame {} fence wait failed (res={}, frame={})",
                                 queueName,
                                 (int)waitResult,
                                 (unsigned)m_CurrentFrame);
        }
        return false;
    };
    if (!waitFrameFence(frSyncG, "graphics", waitedG))
    {
        return false;
    }
    if (!waitFrameFence(frSyncC, "compute", waitedC))
    {
        return false;
    }
    if (!waitFrameFence(frSyncT, "transfer", waitedT))
    {
        return false;
    }

    // Host writes into frame-slotted buffers are legal from here until
    // AdvanceFrameIndex, but only where every queue actually PROVED this slot's
    // previous user finished. `waited` is false only for the armed-but-fenceless
    // queue above, which today cannot occur — fenceArmed is set solely after a
    // successful submit carrying a real fence. Defensive rather than a live gap:
    // an unprovable slot must not report as fenced, or the diagnostic goes silent
    // exactly where it has the least evidence.
    m_CurrentFrameSlotFenced.store(waitedG && waitedC && waitedT, std::memory_order_relaxed);

    m_LastFrameSyncTimings.waitedGraphicsFence = waitedG;
    m_LastFrameSyncTimings.waitedComputeFence = waitedC;
    m_LastFrameSyncTimings.waitedTransferFence = waitedT;

    // Sync the query pool to this frame slot after the fence wait above. This
    // reads completed timestamps into the slot cache before host/device resets
    // make the query indices reusable.
    if (m_QueryPool)
        m_QueryPool->BeginFrame(m_CurrentFrame);

    // Frame-wide GPU bracket: read back the FrameEnd timestamp from this
    // slot's previous use out of the freshly collected query-pool cache.
    // Compute the per-frame GPU wall-clock period as the delta from the most
    // recent previously-read value. The slot we read is from N-3 frames ago;
    // the last value we read was from N-1 frames ago (a different slot at the
    // previous BeginFrame). The deltas are in GPU submission order, so
    // (this read) - (previous read) is the wall time spanning ~one frame.
    if (m_QueryPool)
    {
        const uint32_t pendingQuery = m_FrameEndQuery[m_CurrentFrame];
        if (pendingQuery != UINT32_MAX)
        {
            QueryResult result{};
            if (m_QueryPool->GetTimestampResult(pendingQuery, result) && result.Available)
            {
                if (m_HaveReadAnyFrameEndTimestamp && result.Value > m_LastReadFrameEndTimestamp)
                {
                    const uint64_t deltaTicks = result.Value - m_LastReadFrameEndTimestamp;
                    m_LastFrameSyncTimings.frameGpuPeriodMs =
                        m_QueryPool->TimestampToMs(deltaTicks);
                }
                m_LastReadFrameEndTimestamp = result.Value;
                m_HaveReadAnyFrameEndTimestamp = true;
            }
            m_FrameEndQuery[m_CurrentFrame] = UINT32_MAX;
        }
    }

    // Recycle used command buffers from last use of this frame index, but only for queues where we awaited the fence
    auto recycleFor = [&](IDevice::QueueType qt)
    {
        auto& fr = m_Frames[m_CurrentFrame];
        auto& pq = (qt == IDevice::QueueType::Graphics) ? fr.graphics : (qt == IDevice::QueueType::Compute ? fr.compute : fr.transfer);
        bool canRecycle = (qt == IDevice::QueueType::Graphics) ? waitedG : (qt == IDevice::QueueType::Compute ? waitedC : waitedT);
        if (canRecycle && !pq.usedCmd.empty())
        {
            // Recycle command buffers instead of freeing to avoid driver allocations.
            pq.freeCmd.reserve(pq.freeCmd.size() + pq.usedCmd.size());
            pq.freeCmd.insert(pq.freeCmd.end(), pq.usedCmd.begin(), pq.usedCmd.end());
            pq.usedCmd.clear();
        }
        // Destroy resources deferred to this frame fence (used before the device's
        // first graphics submit, and on backends without timeline semaphores).
        // Producers are concurrent, so swap the lists out under the deferral lock
        // and destroy outside it — the Destroy*Immediate helpers take other locks.
        if (canRecycle)
        {
            std::vector<BufferHandle> buffers;
            std::vector<TextureViewHandle> views;
            std::vector<TextureHandle> textures;
            std::vector<SamplerHandle> samplers;
            {
                std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
                buffers.swap(pq.deferredBuffers);
                views.swap(pq.deferredTextureViews);
                textures.swap(pq.deferredTextures);
                samplers.swap(pq.deferredSamplers);
            }
            if (s_DeferLogEnabled && !(buffers.empty() && textures.empty()) && s_DeferLogBudget-- > 0)
            {
                Logger::Log::Info("[VulkanDefer] drain frame={} buffers={} textures={}",
                                  (unsigned)m_CurrentFrame,
                                  (unsigned)buffers.size(),
                                  (unsigned)textures.size());
            }
            // Views before images: a live view keeps a refcount on its owner.
            for (auto h : views)
            {
                if (h.IsValid())
                    DestroyTextureViewImmediate(h);
            }
            for (auto h : samplers)
            {
                if (h.IsValid())
                    DestroySamplerImmediate(h);
            }
            for (auto h : textures)
            {
                if (h.IsValid())
                    DestroyTextureImmediate(h);
            }
            for (auto h : buffers)
            {
                if (h.IsValid())
                    DestroyBufferImmediate(h);
            }
        }
    };

    // Retire deferred resources once EVERY queue that could reference them has
    // passed its tag. Producers are concurrent: sample the timelines outside the
    // deferral lock, partition each queue under it, then destroy after releasing
    // it. Sound only because this runs at BeginFrame, with no frame mid-recording,
    // so every recorded submit has already been issued.
    if (HasGraphicsTimelineSemaphore())
    {
        const QueueTimelineProgress progress = SampleQueueTimelines();

        std::vector<TextureViewHandle> views;
        std::vector<TextureHandle> textures;
        std::vector<SamplerHandle> samplers;
        std::vector<BufferHandle> buffers;
        {
            std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
            auto partition = [&progress](auto& deferred, auto& retired)
            {
                size_t write = 0;
                for (size_t read = 0; read < deferred.size(); ++read)
                {
                    const auto& entry = deferred[read];
                    if (progress.Retires(entry.tags))
                    {
                        retired.push_back(entry.handle);
                    }
                    else
                    {
                        if (write != read)
                            deferred[write] = entry;
                        ++write;
                    }
                }
                deferred.resize(write);
            };
            partition(m_DeferredTextureViewsTimeline, views);
            partition(m_DeferredTexturesTimeline, textures);
            partition(m_DeferredSamplersTimeline, samplers);
            partition(m_DeferredBuffersTimeline, buffers);
        }
        // Views first: a live view holds a refcount on its owning image.
        for (auto h : views)
            DestroyTextureViewImmediate(h);
        for (auto h : samplers)
            DestroySamplerImmediate(h);
        m_LastRetiredTexturesCount += textures.size();
        for (auto h : textures)
            DestroyTextureImmediate(h);
        m_LastRetiredBuffersCount += buffers.size();
        for (auto h : buffers)
            DestroyBufferImmediate(h);
    }

    RetireDeferredStagingBuffers();
    RetireDeferredRecycleCommandBuffers();

    recycleFor(IDevice::QueueType::Graphics);
    recycleFor(IDevice::QueueType::Compute);
    recycleFor(IDevice::QueueType::Transfer);

    // Publish the slot that pre-first-submit deferrals attach to, after this
    // frame's slot has been drained. This frame's submit arms its fence, so
    // anything parked here retires when the slot next cycles round.
    m_DeferralFrameSlot.store(m_CurrentFrame, std::memory_order_relaxed);

    // Recycle per-thread command pool buffers for this frame slot (GPU fence waited above)
    RecycleThreadPoolsForFrame(m_CurrentFrame);

    // Acquire swapchain image exactly once per frame if we have a swapchain

    // Reset per-frame sync aggregator state
    m_EndOfFrameWaits.clear();
    m_DidBackbufferRenderThisFrame = false;
    m_AnyGraphicsSubmitThisFrame = false;

    // Reset debug ownership-transfer counter per frame
    m_DebugOwnershipTransfers = 0;
    // Reset debug pipeline bind counters per frame
    m_DebugPipelineBindGraphics = 0;
    m_DebugPipelineBindCompute = 0;

    // Reset debug pipeline barrier call counter per frame
    m_DebugPipelineBarrier2Calls = 0;

    // Reset descriptor bind counters per frame
    for (int i = 0; i < 8; ++i)
    {
        m_DebugDescBindGraphics[i] = 0;
        m_DebugDescBindCompute[i] = 0;
    }

    m_AcquireSemaphoreThisFrame = VK_NULL_HANDLE;

    if (m_Swapchain != VK_NULL_HANDLE)
    {
        uint32_t imageIndex = 0;
        if (!AcquireNextImage(imageIndex))
        {
            // Expected during resize/move (VK_TIMEOUT/OUT_OF_DATE); skip this frame quietly.
            m_CurrentSwapchainImage = UINT32_MAX;
            m_AcquiredThisFrame = false;
            return false;
        }
        m_AcquiredThisFrame = true;
    }
    else
    {
        m_AcquiredThisFrame = false;
    }
    // Reset transient descriptor pool for this frame (only the pools tied to this frame index)
    m_DsAllocator.SetCurrentFrame(m_CurrentFrame, MAX_FRAMES_IN_FLIGHT);
    m_DsAllocator.BeginFrameReset(m_CurrentFrame);
    // The slot's transient sets were just recycled — drop their validation metadata
    // so a recycled VkDescriptorSet pointer can't resolve a stale layout desc.
    if (DescriptorValidationActive())
    {
        std::lock_guard<std::mutex> lock(m_DescriptorSetLayoutsMutex);
        m_TransientDescriptorSetLayoutDescs[m_CurrentFrame].clear();
    }
    // Rewind the descriptor-buffer pool for this frame — safe because the frame fence
    // was already waited on above, so any GPU use of this frame's blocks has completed.
    if (m_DescriptorBufferPool)
    {
        m_DescriptorBufferPool->SetCurrentFrame(m_CurrentFrame);
        m_DescriptorBufferPool->BeginFrameReset(m_CurrentFrame);
        // Purge DB handles that were allocated into this slot — their backing memory is now reused.
        {
            std::unique_lock lock(m_DescriptorBufferSetsMutex);
            for (auto it = m_DescriptorBufferSets.begin(); it != m_DescriptorBufferSets.end(); )
            {
                if (it->second.frameSlot == m_CurrentFrame)
                    it = m_DescriptorBufferSets.erase(it);
                else
                    ++it;
            }
        }
    }
    return true;
}

void VulkanDevice::WaitForIdle()
{
    ++m_IdleDrainCount;
    if (!EnsureGlobalGpuIdle())
    {
        return;
    }
    ProcessPendingWindowTargetRetirements(true);
    FlushDeferredResourcesAndCompactLiveTracking();
}

const VulkanPipelineCache::Statistics& VulkanDevice::GetVkDiskPipelineCacheStatistics() const
{
    static VulkanPipelineCache::Statistics emptyStats{};
    return m_VkDiskPipelineCache ? m_VkDiskPipelineCache->GetStatistics() : emptyStats;
}

const IQueryPool::ProfilingStats& VulkanDevice::GetProfilingStats() const
{
    static IQueryPool::ProfilingStats emptyStats{};
    return m_QueryPool ? m_QueryPool->GetProfilingStats() : emptyStats;
}
// Centralized present-time transition recording. Uses tracked swapchain image
// layout instead of a render-flag heuristic so Present is robust across mixed
// rendering paths (draw/no-draw, UI-only, etc.).
void VulkanDevice::RecordPresentTransition(VkCommandBuffer cmd, VkImage image)
{
    if (m_CurrentSwapchainImage >= m_SwapchainImageLayouts.size())
    {
        return;
    }

    const VkImageLayout tracked = m_SwapchainImageLayouts[m_CurrentSwapchainImage];
    if (tracked == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || tracked == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR)
    {
        return;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = tracked;
    switch (tracked)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
#ifdef VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
#endif
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_GENERAL:
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_UNDEFINED:
    default:
        barrier.srcAccessMask = 0;
        break;
    }
    barrier.dstAccessMask = 0;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    const VkPipelineStageFlags srcStage =
        (tracked == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
#ifdef VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL
         || tracked == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL
#endif
         )
            ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    vkCmdPipelineBarrier(cmd,
                         srcStage,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0,
                         0, nullptr,
                         0, nullptr,
                         1, &barrier);
}

bool VulkanDevice::GetTimelineSemaphoreValue(SemaphoreHandle sem, uint64_t& outValue) const
{
    outValue = 0;
    // Timeline semaphores are a Vulkan 1.2 feature and do not require synchronization2.
    if (sem.id == 0)
        return false;
    auto it = m_TimelineSemaphores.find(sem.id);
    if (it == m_TimelineSemaphores.end() || it->second.sem == VK_NULL_HANDLE || !it->second.isTimeline)
        return false;
    uint64_t value = 0;
    VkResult r = vkGetSemaphoreCounterValue(m_Device, it->second.sem, &value);
    if (r != VK_SUCCESS)
        return false;
    outValue = value;
    return true;
}

const char* VulkanDevice::TimelineWaitOutcomeToString(TimelineWaitOutcome outcome)
{
    switch (outcome)
    {
    case TimelineWaitOutcome::Completed: return "completed";
    case TimelineWaitOutcome::TimedOut: return "timed out";
    case TimelineWaitOutcome::DeviceLost: return "device lost";
    case TimelineWaitOutcome::NotATimeline: return "not a timeline semaphore";
    case TimelineWaitOutcome::Failed: return "failed";
    }
    return "unknown";
}

VulkanDevice::TimelineWaitOutcome VulkanDevice::WaitTimelineSemaphore(SemaphoreHandle sem,
                                                                     uint64_t value,
                                                                     uint64_t timeoutNs)
{
    // Timeline semaphores are a Vulkan 1.2 feature and do not require synchronization2.
    if (sem.id == 0)
        return TimelineWaitOutcome::NotATimeline;
    auto it = m_TimelineSemaphores.find(sem.id);
    if (it == m_TimelineSemaphores.end() || it->second.sem == VK_NULL_HANDLE || !it->second.isTimeline)
        return TimelineWaitOutcome::NotATimeline;

    VkSemaphoreWaitInfo wi{};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.flags = 0;
    VkSemaphore vkSem = it->second.sem;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &vkSem;
    wi.pValues = &value;

    switch (vkWaitSemaphores(m_Device, &wi, timeoutNs))
    {
    case VK_SUCCESS: return TimelineWaitOutcome::Completed;
    case VK_TIMEOUT: return TimelineWaitOutcome::TimedOut;
    case VK_ERROR_DEVICE_LOST: return TimelineWaitOutcome::DeviceLost;
    default: return TimelineWaitOutcome::Failed;
    }
}

bool VulkanDevice::WaitTimelineSemaphoreValue(SemaphoreHandle sem, uint64_t value, uint64_t timeoutNs)
{
    return WaitTimelineSemaphore(sem, value, timeoutNs) == TimelineWaitOutcome::Completed;
}

bool VulkanDevice::HasGraphicsTimelineSemaphore() const
{
    if (m_GraphicsTimeline.id == 0)
        return false;
    auto it = m_TimelineSemaphores.find(m_GraphicsTimeline.id);
    return (it != m_TimelineSemaphores.end()) && (it->second.sem != VK_NULL_HANDLE) && it->second.isTimeline;
}

IDevice::GpuSyncToken VulkanDevice::LastGraphicsSubmissionToken() const
{
    if (!HasGraphicsTimelineSemaphore())
        return {};
    // The graphics context's last signalled value is what the most recent graphics submit
    // signalled, so waiting it retires that submit and every earlier one on this queue. Zero
    // (no submit yet) yields an invalid token, which reads as GpuSyncStatus::Unknown — the
    // caller decides what "no submit yet" means for it.
    const QueueSubmitContext& graphics = SubmitContextFor(QueueType::Graphics);
    return GpuSyncToken{graphics.Timeline(), graphics.LastSignalled()};
}

uint64_t VulkanDevice::GetGraphicsTimelineCompletedValue() const
{
    if (!HasGraphicsTimelineSemaphore())
        return 0;
    auto it = m_TimelineSemaphores.find(m_GraphicsTimeline.id);
    if (it == m_TimelineSemaphores.end() || it->second.sem == VK_NULL_HANDLE || !it->second.isTimeline)
        return 0;
    uint64_t value = 0;
    VkResult r = vkGetSemaphoreCounterValue(m_Device, it->second.sem, &value);
    return (r == VK_SUCCESS) ? value : 0;
}

QueueTimelineCompletions VulkanDevice::SampleQueueTimelineCompletions() const
{
    // Keyed by the timeline each CONTEXT signals, so contexts that alias one physical queue
    // contribute one entry and every deferred entry's recorded handle is covered.
    QueueTimelineCompletions completions;
    for (const QueueSubmitContext& context : m_SubmitContextStorage)
    {
        uint64_t completed = 0;
        if (GetTimelineSemaphoreValue(context.Timeline(), completed))
            completions.Add(context.Timeline(), completed);
    }
    return completions;
}

uint64_t VulkanDevice::CompletedValueFor(SemaphoreHandle timeline,
                                         const QueueTimelineCompletions& completions) const
{
    uint64_t completed = 0;
    if (completions.TryGet(timeline, completed))
        return completed;
    // A timeline this sweep did not sample. Defensive: every deferred entry records a context's
    // timeline and every context is sampled, and a rebuild flushes both deferred lists before it
    // resets the handles. Query it directly rather than substituting another queue's progress —
    // one that no longer resolves yields 0, leaving the entry for a flush to drain rather than
    // freeing it early.
    GetTimelineSemaphoreValue(timeline, completed);
    return completed;
}

QueueTimelineProgress VulkanDevice::SampleQueueTimelines() const
{
    // Each role's completed/submitted pair comes from the context that role resolves to, so a
    // role aliased onto another queue is measured against the timeline its work truly signals.
    // (The transfer context's counter is also advanced by worker-thread SubmitTextureUploads.)
    const QueueTimelineCompletions completions = SampleQueueTimelineCompletions();
    const std::array<const QueueSubmitContext*, kQueueRoleCount> byRole = {
        &SubmitContextFor(QueueType::Graphics), &SubmitContextFor(QueueType::Compute),
        &SubmitContextFor(QueueType::Transfer)};
    return SampleQueueProgress(byRole, completions);
}

void VulkanDevice::RetireDeferredStagingBuffers()
{
    // Collect retired handles under lock, destroy outside to avoid nesting with m_ResourceTrackingMutex.
    constexpr size_t kStackCapacity = 8;
    BufferHandle stackDestroy[kStackCapacity]{};
    std::vector<BufferHandle> heapDestroy;
    size_t destroyCount = 0;

    {
        std::lock_guard<std::mutex> lock(m_DeferredStagingMutex);
        if (m_DeferredStagingBuffers.empty())
            return;

        // Resolve completion by the timeline each entry recorded. The copy runs on whichever
        // physical queue the transfer role resolved to, and that queue's timeline is the
        // graphics one with no dedicated transfer family, or the compute one when compute and
        // transfer share a family — neither of which is m_TransferTimeline.
        const QueueTimelineCompletions completions = SampleQueueTimelineCompletions();
        size_t write = 0;
        for (size_t read = 0; read < m_DeferredStagingBuffers.size(); ++read)
        {
            const auto& entry = m_DeferredStagingBuffers[read];
            const uint64_t completed = CompletedValueFor(entry.timeline, completions);
            if (entry.timelineValue <= completed)
            {
                if (destroyCount < kStackCapacity)
                    stackDestroy[destroyCount] = entry.handle;
                else
                    heapDestroy.push_back(entry.handle);
                ++destroyCount;
            }
            else
            {
                if (write != read)
                    m_DeferredStagingBuffers[write] = entry;
                ++write;
            }
        }
        m_DeferredStagingBuffers.resize(write);
    }

    for (size_t i = 0; i < destroyCount; ++i)
    {
        BufferHandle h = (i < kStackCapacity) ? stackDestroy[i] : heapDestroy[i - kStackCapacity];
        DestroyBufferImmediate(h);
    }
}

void VulkanDevice::RouteThreadPoolCmdBufferForRecycle(ThreadCommandPoolEntry* pool, VkCommandBuffer cb,
                                                      int freeListOrdinal, SemaphoreHandle timeline,
                                                      uint64_t timelineValue)
{
    if (!pool || cb == VK_NULL_HANDLE)
        return;

    // Prefer timeline-based retirement: the submit signaled `timeline` to `timelineValue`
    // with a NULL fence, so nothing else guards the CB. Deferring it against that value
    // guarantees it is never reset while still pending on the GPU. Only when the device has
    // no usable timeline semaphore do we fall back to frame-slot recycling (the pre-existing
    // behavior for such devices), which is main-thread only — it reads m_CurrentFrame.
    //
    // A non-zero value IS the timeline's validity: QueueSubmitContext hands out a value only
    // when its queue has a real timeline semaphore, and only for a submit that succeeded.
    // Re-deriving that from m_TimelineSemaphores would mean reading an unsynchronized map from
    // the worker threads that reach here through ExecuteCommandLists.
    if (timelineValue != 0)
    {
        std::lock_guard<std::mutex> lock(m_DeferredStagingMutex);
        m_DeferredRecycleCmdBuffers.push_back({pool, cb, freeListOrdinal, timeline, timelineValue});
        return;
    }

    // Submitted thread-pool CBs are always primary-level — secondaries are executed
    // via vkCmdExecuteCommands, never queue-submitted — so they recycle to the
    // primary bucket (both here and in the timeline-deferred path above).
    std::lock_guard<std::mutex> rlock(pool->recycleMutex);
    pool->usedCmdPerFrame[m_CurrentFrame][ThreadCommandPoolEntry::kLevelPrimary][freeListOrdinal]
        .push_back(cb);
}

void VulkanDevice::RetireDeferredRecycleCommandBuffers()
{
    // Collect the CBs the GPU has finished with under the deferred lock, then return them to
    // their pool freelists outside it. Collecting-then-acting keeps the per-pool recycleMutex
    // from nesting under m_DeferredStagingMutex, matching the documented lock ordering.
    constexpr size_t kStackCapacity = 8;
    DeferredRecycleCommandBuffer stackReady[kStackCapacity]{};
    std::vector<DeferredRecycleCommandBuffer> heapReady;
    size_t readyCount = 0;

    {
        std::lock_guard<std::mutex> lock(m_DeferredStagingMutex);
        if (m_DeferredRecycleCmdBuffers.empty())
            return;

        // Cache each queue timeline's completed value once; entries store the timeline their
        // submit signaled, so a CB is safe to recycle when that value has been reached.
        const QueueTimelineCompletions completions = SampleQueueTimelineCompletions();

        size_t write = 0;
        for (size_t read = 0; read < m_DeferredRecycleCmdBuffers.size(); ++read)
        {
            const auto& entry = m_DeferredRecycleCmdBuffers[read];
            const uint64_t completed = CompletedValueFor(entry.timeline, completions);
            if (entry.timelineValue <= completed)
            {
                if (readyCount < kStackCapacity)
                    stackReady[readyCount] = entry;
                else
                    heapReady.push_back(entry);
                ++readyCount;
            }
            else
            {
                if (write != read)
                    m_DeferredRecycleCmdBuffers[write] = entry;
                ++write;
            }
        }
        m_DeferredRecycleCmdBuffers.resize(write);
    }

    for (size_t i = 0; i < readyCount; ++i)
    {
        const DeferredRecycleCommandBuffer& entry =
            (i < kStackCapacity) ? stackReady[i] : heapReady[i - kStackCapacity];
        if (!entry.pool || entry.commandBuffer == VK_NULL_HANDLE)
            continue;
        std::lock_guard<std::mutex> rlock(entry.pool->recycleMutex);
        entry.pool->freeCmd[ThreadCommandPoolEntry::kLevelPrimary][entry.freeListOrdinal]
            .push_back(entry.commandBuffer);
    }
}

VkResult VulkanDevice::ArmGraphicsFrameFence(PerQueueFrame& queueFrame)
{
    // Empty submit whose only job is to signal the frame fence. Signals no timeline value, so
    // it takes the graphics queue lock without consuming one.
    QueueSubmitContext& context = SubmitContextFor(QueueType::Graphics);
    if (context.Queue() == VK_NULL_HANDLE)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    const VkResult result = context.WithQueueLocked([&] {
        return vkQueueSubmit(context.Queue(), 1, &submit, queueFrame.fence);
    });
    if (result == VK_SUCCESS)
        queueFrame.fenceArmed = true;
    return result;
}

void VulkanDevice::ArmComputeTransferFences()
{
    // Fence-only submits: they serialise against the queue's other work but signal no timeline
    // value, so they take the lock without consuming one. Each role resolves to the context of
    // the queue it runs on, so a role with no dedicated queue arms its fence under the graphics
    // lock rather than racing the graphics submits.
    // A fence is armed when the slot recycles legacy command buffers or, for compute, when any
    // list was routed on that queue in the slot (submittedSinceArm): render-graph passes run on
    // compute and write the slot's queries, so the slot's proof must cover them. No render-graph
    // pass runs on the transfer queue; its thread-pool submissions are worker uploads, retired by
    // timeline, and arming for them would make BeginFrame wait on an upload backlog. A pass that
    // moves to the transfer queue needs that queue marked the same way.
    // Yields the submit's result so the loss is observed OUTSIDE the queue lock:
    // OnDeviceLostObserved reads every queue's checkpoints, and this context's lock may not be
    // held across work that touches another queue.
    auto armFence = [this](QueueType role, PerQueueFrame& queueFrame) -> VkResult
    {
        if (!queueFrame.fence || queueFrame.fenceArmed)
            return VK_SUCCESS;
        QueueSubmitContext& context = SubmitContextFor(role);
        if (context.Queue() == VK_NULL_HANDLE)
            return VK_SUCCESS;

        VkSubmitInfo emptySubmit{};
        emptySubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        bool submitted = false;
        const VkResult armResult = context.WithQueueLocked([&] {
            if (queueFrame.usedCmd.empty() && !queueFrame.submittedSinceArm)
                return VK_SUCCESS;
            queueFrame.submittedSinceArm = false;
            submitted = true;
            return vkQueueSubmit(context.Queue(), 1, &emptySubmit, queueFrame.fence);
        });
        if (submitted && armResult == VK_SUCCESS)
            queueFrame.fenceArmed = true;
        return armResult;
    };

    auto& fr = m_Frames[m_CurrentFrame];
    const VkResult computeRes = armFence(QueueType::Compute, fr.compute);
    if (computeRes == VK_ERROR_DEVICE_LOST && m_RecoveryEnabled)
        OnDeviceLostObserved("compute fence arm", computeRes, /*deviceActuallyLost=*/true);
    const VkResult transferRes = armFence(QueueType::Transfer, fr.transfer);
    if (transferRes == VK_ERROR_DEVICE_LOST && m_RecoveryEnabled)
        OnDeviceLostObserved("transfer fence arm", transferRes, /*deviceActuallyLost=*/true);
}

void VulkanDevice::AdvanceFrameIndex()
{
    // Cleared before the rotation, not after: the moment the index moves, the
    // slot it names belongs to a frame that may still be executing, and only
    // BeginFrame's wait proves otherwise.
    m_CurrentFrameSlotFenced.store(false, std::memory_order_relaxed);
    m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanDevice::Present()
{
    // Q6 mid-frame short-circuit (design F10): once a device loss has latched,
    // present nothing against the dead device.
    if (m_RecoveryEnabled && m_Health.IsLost())
    {
        return;
    }

    if (m_Swapchain == VK_NULL_HANDLE)
    {
        // Offscreen frame: we still need to advance the per-frame fence/index so the next BeginFrame() doesn't block.
        // We submit an empty batch to the graphics queue that only signals the per-frame fence; this is equivalent to
        // "frame end" and guarantees that any previously submitted graphics work completes-before the fence signals
        // (queue order), without introducing spurious GPU work. Alternatively, higher layers can call FinalizeFrame().
        {
            auto& fr = m_Frames[m_CurrentFrame];
            if (!fr.graphics.fenceArmed && fr.graphics.fence)
            {
                if (ArmGraphicsFrameFence(fr.graphics) == VK_SUCCESS)
                {
                    m_DeviceKnownIdle = false;
                }
            }
            ArmComputeTransferFences();
            AdvanceFrameIndex();
        }
        return; // No swapchain to present to
    }

    // Present only if an image was acquired this frame and the image index is valid
    // for BOTH arrays it indexes: a CreateSwapchain that fails between sizing the
    // images and sizing the semaphores leaves a live swapchain whose image count the
    // semaphore vector does not match, and SetVsync discards that failure.
    if (!m_AcquiredThisFrame || m_CurrentSwapchainImage >= m_SwapchainImages.size() ||
        m_CurrentSwapchainImage >= m_PresentReadySemaphores.size())
    {
        auto& fr = m_Frames[m_CurrentFrame];
        if (!fr.graphics.fenceArmed && fr.graphics.fence)
        {
            if (ArmGraphicsFrameFence(fr.graphics) == VK_SUCCESS)
            {
                m_DeviceKnownIdle = false;
            }
        }
        ArmComputeTransferFences();
        AdvanceFrameIndex();
        return;
    }
    {
        // Ensure the backbuffer image is transitioned to PRESENT at present time
        // We perform a short command buffer submission that inserts the COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR barrier.
        VkCommandBuffer cmd = AcquireCmdBuffer(IDevice::QueueType::Graphics);
        if (cmd != VK_NULL_HANDLE)
        {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cmd, &begin);
            // Transition swapchain image for this frame
            VkImage image = m_SwapchainImages[m_CurrentSwapchainImage];
            // Centralized helper decides whether to insert a barrier based on whether
            // the backbuffer was rendered this frame.
            RecordPresentTransition(cmd, image);
            // Track layout across frames so next frame's first transition can start from PRESENT
            if (m_CurrentSwapchainImage < m_SwapchainImageLayouts.size())
            {
                m_SwapchainImageLayouts[m_CurrentSwapchainImage] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            }
            // Frame-wide GPU bracket: write one timestamp at the END of this
            // frame's last graphics-queue cmd buffer. The next time this
            // frame slot's BeginFrame runs (N-3 frames later when slot
            // cycles), we read this value back — the delta vs the previous
            // read is the GPU's wall-clock per-frame period, capturing
            // everything per-pass timestamps miss (submit overhead, layout
            // transitions outside passes, cross-queue waits).
            if (m_QueryPool)
            {
                auto* vkPool = dynamic_cast<VulkanQueryPool*>(m_QueryPool.get());
                if (vkPool)
                    m_FrameEndQuery[m_CurrentFrame] = vkPool->WriteTimestampRaw(cmd, "_FrameEnd");
            }
            vkEndCommandBuffer(cmd);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            // Choose appropriate waits for this barrier-only submit
            VkSemaphore waitSems[16];
            VkPipelineStageFlags waitStages[16];
            uint32_t waitCount = 0;

            // If we acquired an image but never consumed imageAvailable this frame (no graphics submit),
            // consume it here so validation stays clean. This does not make Present wait on imageAvailable;
            // it only gates the barrier submission which touches the swapchain image.
            if (m_Swapchain != VK_NULL_HANDLE && m_AcquiredThisFrame && !m_WaitedOnImageAvailableThisFrame && m_CurrentFrame < m_ImageAvailableSemaphores.size())
            {
                VkSemaphore acquireSem = m_ImageAvailableSemaphores[m_CurrentFrame];
                if (acquireSem != VK_NULL_HANDLE && acquireSem == m_AcquireSemaphoreThisFrame)
                {
                    waitSems[waitCount] = acquireSem;
                    waitStages[waitCount] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                    ++waitCount;
                    m_WaitedOnImageAvailableThisFrame = true;
                    m_AcquireSemaphoreThisFrame = VK_NULL_HANDLE;
                }
            }

            // Merge user-registered per-frame waits (binary only here)
            for (const auto& w : m_EndOfFrameWaits)
            {
                if (w.value == 0)
                {
                    auto it = m_TimelineSemaphores.find(w.sem.id);
                    if (it != m_TimelineSemaphores.end())
                    {
                        waitSems[waitCount] = it->second.sem;
                        VkPipelineStageFlags stageFlags = w.stageMask
                                                              ? static_cast<VkPipelineStageFlags>(w.stageMask)
                                                              : static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                        waitStages[waitCount] = stageFlags;
                        ++waitCount;
                        if (waitCount >= 16)
                            break;
                    }
                }
            }
            submit.waitSemaphoreCount = waitCount;
            submit.pWaitSemaphores = waitCount ? waitSems : nullptr;
            submit.pWaitDstStageMask = waitCount ? waitStages : nullptr;
            // Signal presentReady so vkQueuePresentKHR can wait on it. Also signal the
            // graphics timeline for target-scoped drain tracking when available.
            QueueSubmitContext& graphicsContext = SubmitContextFor(QueueType::Graphics);
            VkSemaphore signalSems[2] = {m_PresentReadySemaphores[m_CurrentSwapchainImage],
                                         graphicsContext.TimelineSemaphore()};
            uint64_t signalValues[2] = {0ull, 0ull};
            VkTimelineSemaphoreSubmitInfo timelineInfo{};
            timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineInfo.signalSemaphoreValueCount = 2;
            timelineInfo.pSignalSemaphoreValues = signalValues;
            submit.pSignalSemaphores = signalSems;

            // Submit with the per-frame graphics fence so we know when it's safe to recycle
            // resources. This submit carries no command lists of its own — the barrier CB is
            // covered by that fence — so it goes straight through the queue context rather than
            // SubmitBatchToQueue.
            auto& fr = m_Frames[m_CurrentFrame];
            const auto t0 = std::chrono::high_resolution_clock::now();
            VkResult submitRes = VK_SUCCESS;
            const QueueSubmitContext::SubmitResult submitted =
                graphicsContext.SubmitAndSignal([&](uint64_t signalValue)
            {
                if (signalValue != 0)
                {
                    signalValues[1] = signalValue;
                    submit.signalSemaphoreCount = 2;
                    submit.pNext = &timelineInfo;
                }
                else
                {
                    submit.signalSemaphoreCount = 1;
                }
                submitRes = vkQueueSubmit(graphicsContext.Queue(), 1, &submit, fr.graphics.fence);
                return submitRes == VK_SUCCESS;
            });
            const auto t1 = std::chrono::high_resolution_clock::now();
            m_LastFrameSyncTimings.presentTransitionSubmitMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            const uint64_t graphicsTimelineSignalValue = submitted.SignalledValue;
            if (submitRes == VK_SUCCESS)
            {
                fr.graphics.fenceArmed = true;
                RecordActiveWindowTargetFenceArmed(m_CurrentFrame);
                m_DeviceKnownIdle = false;
                if (graphicsTimelineSignalValue > 0)
                {
                    RecordActiveWindowTargetGraphicsProgress(graphicsTimelineSignalValue);
                }
            }
            else
            {
                Logger::Log::Error("Failed to submit present-transition command buffer! Error: {}", (int)submitRes);
            }
            // Track command buffer for deferred free next frame
            fr.graphics.usedCmd.push_back(cmd);
        }
        ArmComputeTransferFences();
        PresentImage(m_CurrentSwapchainImage);
    }
}

// Frame-end waits forwarding overload (preserve timeline values)
void VulkanDevice::Present(const std::vector<IDevice::SemaphoreWait>& extraWaits)
{
    // Append detailed waits so Present() can merge them with WSI waits properly
    for (const auto& w : extraWaits)
    {
        m_EndOfFrameWaits.push_back(w);
    }
    Present();
}

void VulkanDevice::RegisterEndOfFrameWait(const IDevice::SemaphoreWait& wait)
{
    m_EndOfFrameWaits.push_back(wait);
}
void VulkanDevice::ClearEndOfFrameWaits()
{
    m_EndOfFrameWaits.clear();
}

// FinalizeFrame: signal per-frame fence for offscreen frames and rotate frame index.
// Command buffers in usedCmd are left for BeginFrame() to recycle after the fence is waited.
void VulkanDevice::FinalizeFrame()
{
    if (m_Swapchain == VK_NULL_HANDLE)
    {
        auto& fr = m_Frames[m_CurrentFrame];

        if (!fr.graphics.fenceArmed && fr.graphics.fence)
        {
            const VkResult armResult = ArmGraphicsFrameFence(fr.graphics);
            if (armResult == VK_SUCCESS)
            {
                m_DeviceKnownIdle = false;
            }
            else
            {
                Logger::Log::Error("FinalizeFrame: failed to submit fence signal! Error: {}",
                                   (int)armResult);
            }
        }

        ArmComputeTransferFences();

        // Advance frame index
        AdvanceFrameIndex();
    }
}

VulkanDevice::WindowTargetState VulkanDevice::CaptureWindowTargetState() const
{
    WindowTargetState state{};
    state.surface = m_Surface;
    state.swapchain = m_Swapchain;
    state.swapchainImages = m_SwapchainImages;
    state.swapchainImageViews = m_SwapchainImageViews;
    state.swapchainImageLayouts = m_SwapchainImageLayouts;
    state.swapchainImageFormat = m_SwapchainImageFormat;
    state.swapchainColorSpace = m_SwapchainColorSpace;
    state.swapchainExtent = m_SwapchainExtent;
    state.swapchainSupportsReadback = m_SwapchainSupportsReadback;
    state.hdrState = m_HdrState;
    state.swapchainRenderPass = m_SwapchainRenderPass;
    state.swapchainFramebuffers = m_SwapchainFramebuffers;
    state.presentReadySemaphores = m_PresentReadySemaphores;
    state.presentCompleteFence = m_ActiveWindowTargetPresentCompleteFence;
    state.presentFenceArmed = m_ActiveWindowTargetPresentFenceArmed;
    state.swapchainTextures = m_SwapchainTextures;
    state.frameIndex = m_ActiveWindowTargetFrameIndex;
    state.lastGraphicsTimelineValue = m_ActiveWindowTargetLastGraphicsTimelineValue;
    state.lastArmedFrameIndex = m_ActiveWindowTargetLastArmedFrameIndex;
    return state;
}

void VulkanDevice::ApplyWindowTargetState(const WindowTargetState& state)
{
    m_Surface = state.surface;
    m_Swapchain = state.swapchain;
    m_SwapchainImages = state.swapchainImages;
    m_SwapchainImageViews = state.swapchainImageViews;
    m_SwapchainImageLayouts = state.swapchainImageLayouts;
    m_SwapchainImageFormat = state.swapchainImageFormat;
    m_SwapchainColorSpace = state.swapchainColorSpace;
    m_SwapchainExtent = state.swapchainExtent;
    m_SwapchainSupportsReadback = state.swapchainSupportsReadback;
    m_HdrState = state.hdrState;
    m_SwapchainRenderPass = state.swapchainRenderPass;
    m_SwapchainFramebuffers = state.swapchainFramebuffers;
    m_PresentReadySemaphores = state.presentReadySemaphores;
    m_ActiveWindowTargetPresentCompleteFence = state.presentCompleteFence;
    m_ActiveWindowTargetPresentFenceArmed = state.presentFenceArmed;
    m_SwapchainTextures = state.swapchainTextures;
    m_ActiveWindowTargetFrameIndex = state.frameIndex;
    m_ActiveWindowTargetLastGraphicsTimelineValue = state.lastGraphicsTimelineValue;
    m_ActiveWindowTargetLastArmedFrameIndex = state.lastArmedFrameIndex;
}

// Drops the member mirror WITHOUT destroying anything: the caller must already have
// moved ownership of the active target's objects into m_WindowTargets / a captured
// WindowTargetState, or destroyed them. Nothing creates window-target objects outside
// CreateSwapchain, so on a device with no window target these members are empty and
// this is a no-op.
void VulkanDevice::ResetWindowTargetMembers()
{
    m_Surface = VK_NULL_HANDLE;
    m_Swapchain = VK_NULL_HANDLE;
    m_SwapchainImages.clear();
    m_SwapchainImageViews.clear();
    m_SwapchainImageLayouts.clear();
    m_SwapchainImageFormat = VK_FORMAT_UNDEFINED;
    m_SwapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    m_SwapchainSupportsReadback = false;
    m_HdrState.activeMode = HdrOutputMode::Off;
    m_SwapchainExtent = {0, 0};
    m_SwapchainRenderPass = VK_NULL_HANDLE;
    m_SwapchainFramebuffers.clear();
    m_PresentReadySemaphores.clear();
    m_ActiveWindowTargetPresentCompleteFence = VK_NULL_HANDLE;
    m_ActiveWindowTargetPresentFenceArmed = false;
    m_SwapchainTextures.clear();
    m_CurrentSwapchainImage = 0;
    m_ActiveWindowTargetFrameIndex = 0;
    m_ActiveWindowTargetLastGraphicsTimelineValue = 0;
    m_ActiveWindowTargetLastArmedFrameIndex = UINT32_MAX;
}

bool VulkanDevice::CreateActiveWindowTargetPresentFence()
{
    if (m_Device == VK_NULL_HANDLE)
    {
        return false;
    }
    if (m_ActiveWindowTargetPresentCompleteFence != VK_NULL_HANDLE)
    {
        return true;
    }
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence = VK_NULL_HANDLE;
    const VkResult r = vkCreateFence(m_Device, &fi, nullptr, &fence);
    if (r != VK_SUCCESS)
    {
        Logger::Log::Error("Failed to create window-target present-complete fence (res={})", (int)r);
        return false;
    }
    m_ActiveWindowTargetPresentCompleteFence = fence;
    m_ActiveWindowTargetPresentFenceArmed = false;
    return true;
}

bool VulkanDevice::IsFenceSignaled(VkFence fence) const
{
    if (m_Device == VK_NULL_HANDLE || fence == VK_NULL_HANDLE)
    {
        return true;
    }
    return vkGetFenceStatus(m_Device, fence) == VK_SUCCESS;
}

bool VulkanDevice::IsPreviousFrameGraphicsComplete() const
{
    // Non-blocking poll of the graphics timeline (vkGetSemaphoreCounterValue via
    // GetTimelineSemaphoreValue). The graphics context's last signalled value is
    // what the most recent graphics submission signals; when the GPU has reached
    // it, that submission — and any per-frame GPU->CPU copy it carried (e.g. the
    // scatter cursor readback) — is complete. Nothing submitted yet ⇒ no stale
    // mirror to guard. Never waits.
    const uint64_t submitted = SubmitContextFor(QueueType::Graphics).LastSignalled();
    if (!HasGraphicsTimelineSemaphore() || submitted == 0)
        return true;
    uint64_t completed = 0;
    if (!GetTimelineSemaphoreValue(m_GraphicsTimeline, completed))
        return false;
    return completed >= submitted;
}

bool VulkanDevice::WaitForFence(VkFence fence, uint64_t timeoutNs)
{
    if (m_Device == VK_NULL_HANDLE || fence == VK_NULL_HANDLE)
    {
        return true;
    }
    const VkResult r = vkWaitForFences(m_Device, 1, &fence, VK_TRUE, timeoutNs);
    return r == VK_SUCCESS;
}

void VulkanDevice::RecordActiveWindowTargetGraphicsProgress(uint64_t graphicsTimelineValue)
{
    if (!m_HasActiveWindowTarget || m_ActiveWindowTargetId == 0 || graphicsTimelineValue == 0)
        return;
    m_ActiveWindowTargetLastGraphicsTimelineValue = graphicsTimelineValue;
    auto it = m_WindowTargets.find(m_ActiveWindowTargetId);
    if (it != m_WindowTargets.end())
    {
        it->second.lastGraphicsTimelineValue = graphicsTimelineValue;
    }
}

void VulkanDevice::RecordActiveWindowTargetFenceArmed(uint32_t frameIndex)
{
    if (!m_HasActiveWindowTarget || m_ActiveWindowTargetId == 0)
        return;
    m_ActiveWindowTargetLastArmedFrameIndex = frameIndex;
    auto it = m_WindowTargets.find(m_ActiveWindowTargetId);
    if (it != m_WindowTargets.end())
    {
        it->second.lastArmedFrameIndex = frameIndex;
    }
}

void VulkanDevice::EnqueueWindowTargetRetirement(uint64_t targetId, WindowTargetState&& state, uint64_t retireTimelineValue)
{
    PendingWindowTargetRetirement pending{};
    pending.targetId = targetId;
    pending.state = std::move(state);
    pending.retireTimelineValue = retireTimelineValue;
    pending.enqueueFrameCounter = m_WindowTargetRetirementFrameCounter;
    m_PendingWindowTargetRetirements.push_back(std::move(pending));
    MaybeWarnOrForceWindowTargetRetirement();
}

void VulkanDevice::ProcessPendingWindowTargetRetirements(bool allowBlockingFallback)
{
    if (m_PendingWindowTargetRetirements.empty())
    {
        return;
    }

    const size_t pendingCount = m_PendingWindowTargetRetirements.size();
    bool needsBlockingFallback = false;
    if (allowBlockingFallback)
    {
        static constexpr size_t kForceRetireCount = 4;
        static constexpr uint64_t kForceRetireAgeFrames = 600;
        if (pendingCount >= kForceRetireCount)
        {
            needsBlockingFallback = true;
        }
        else
        {
            const uint64_t newestCounter = m_WindowTargetRetirementFrameCounter;
            for (const auto& pending : m_PendingWindowTargetRetirements)
            {
                const uint64_t age = (newestCounter >= pending.enqueueFrameCounter)
                                         ? (newestCounter - pending.enqueueFrameCounter)
                                         : 0;
                if (age >= kForceRetireAgeFrames)
                {
                    needsBlockingFallback = true;
                    break;
                }
            }
        }
    }

    if (!m_DeviceKnownIdle)
    {
        if (allowBlockingFallback && needsBlockingFallback)
        {
            Logger::Log::Warning("[VulkanWindowTarget] forcing global idle to retire {} pending window targets",
                                 pendingCount);
            const auto t0 = std::chrono::high_resolution_clock::now();
            if (!EnsureGlobalGpuIdle())
            {
                return;
            }
            const auto t1 = std::chrono::high_resolution_clock::now();
            const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            ++m_WindowTargetRetirementForcedIdleCount;
            m_WindowTargetRetirementForcedIdleTotalMs += elapsedMs;
            if (elapsedMs > m_WindowTargetRetirementForcedIdleMaxMs)
            {
                m_WindowTargetRetirementForcedIdleMaxMs = elapsedMs;
            }
            Logger::Log::Warning("[VulkanWindowTarget] forced retirement idle took {:.3f} ms (count={}, maxMs={:.3f})",
                                 elapsedMs,
                                 (unsigned long long)m_WindowTargetRetirementForcedIdleCount,
                                 m_WindowTargetRetirementForcedIdleMaxMs);
        }
    }

    const bool hasTimeline = HasGraphicsTimelineSemaphore();
    const uint64_t completedTimeline = hasTimeline ? GetGraphicsTimelineCompletedValue() : 0;
    std::vector<PendingWindowTargetRetirement> remaining;
    remaining.reserve(m_PendingWindowTargetRetirements.size());
    for (auto& pending : m_PendingWindowTargetRetirements)
    {
        bool canFinalize = m_DeviceKnownIdle;
        if (!canFinalize)
        {
            bool presentComplete = true;
            if (pending.state.presentFenceArmed && pending.state.presentCompleteFence != VK_NULL_HANDLE)
            {
                presentComplete = IsFenceSignaled(pending.state.presentCompleteFence);
                if (presentComplete)
                {
                    pending.state.presentFenceArmed = false;
                }
            }

            bool graphicsComplete = false;
            if (hasTimeline && pending.retireTimelineValue > 0)
            {
                graphicsComplete = completedTimeline >= pending.retireTimelineValue;
            }
            else if (pending.state.lastArmedFrameIndex != UINT32_MAX)
            {
                graphicsComplete = WaitForSpecificGraphicsFence(pending.state.lastArmedFrameIndex, 0);
            }
            else
            {
                graphicsComplete = true;
            }

            if (presentComplete && graphicsComplete)
            {
                canFinalize = true;
            }
            else if (allowBlockingFallback && needsBlockingFallback)
            {
                // Fallback path was requested and threshold exceeded; perform a
                // controlled blocking wait for this pending target.
                if (pending.state.presentFenceArmed && pending.state.presentCompleteFence != VK_NULL_HANDLE)
                {
                    if (WaitForFence(pending.state.presentCompleteFence, UINT64_MAX))
                    {
                        pending.state.presentFenceArmed = false;
                    }
                }
                if (hasTimeline && pending.retireTimelineValue > 0)
                {
                    (void)WaitTimelineSemaphoreValue(m_GraphicsTimeline, pending.retireTimelineValue, UINT64_MAX);
                }
                else if (pending.state.lastArmedFrameIndex != UINT32_MAX)
                {
                    (void)WaitForSpecificGraphicsFence(pending.state.lastArmedFrameIndex, UINT64_MAX);
                }
                else
                {
                    (void)WaitForArmedGraphicsFences(UINT64_MAX);
                }

                if (pending.state.presentFenceArmed &&
                    pending.state.presentCompleteFence != VK_NULL_HANDLE)
                {
                    pending.state.presentFenceArmed = IsFenceSignaled(pending.state.presentCompleteFence) ? false : true;
                }
                if (pending.state.presentFenceArmed == false ||
                    pending.state.presentCompleteFence == VK_NULL_HANDLE)
                {
                    pending.state.presentFenceArmed = false;
                    canFinalize = true;
                }
                else if (!pending.state.presentReadySemaphores.empty() &&
                         m_PresentQueue != VK_NULL_HANDLE &&
                         m_PresentQueue != m_GraphicsQueue)
                {
                    vkQueueWaitIdle(m_PresentQueue);
                    pending.state.presentFenceArmed = false;
                    canFinalize = true;
                }
            }
        }

        if (canFinalize)
        {
            DestroyWindowTargetState(pending.state);
        }
        else
        {
            remaining.push_back(std::move(pending));
        }
    }
    m_PendingWindowTargetRetirements = std::move(remaining);
}

void VulkanDevice::TeardownAllWindowTargetsForShutdown()
{
    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0)
    {
        SaveActiveWindowTargetState();
    }

    if (!m_WindowTargets.empty())
    {
        for (auto& kv : m_WindowTargets)
        {
            DestroyWindowTargetState(kv.second);
        }
        m_WindowTargets.clear();
    }
    else
    {
        // Legacy/no-map path: current members may still own swapchain resources
        // and present-ready semaphores. Destroy them before resetting members.
        WindowTargetState state = CaptureWindowTargetState();
        DestroyWindowTargetState(state);
    }

    for (auto& pending : m_PendingWindowTargetRetirements)
    {
        DestroyWindowTargetState(pending.state);
    }
    m_PendingWindowTargetRetirements.clear();

    ResetWindowTargetMembers();
    m_ActiveWindowTargetId = 0;
    m_HasActiveWindowTarget = false;
}

bool VulkanDevice::EnsureGlobalGpuIdle(uint64_t timeoutNs)
{
    if (m_Device == VK_NULL_HANDLE)
    {
        return true;
    }
    // Q6 shutdown-wait gating (design §6): never block teardown forever on a
    // hung/lost device. On Lost the queue waits return DEVICE_LOST fast anyway,
    // but a genuinely hung GPU would wedge the unbounded vkQueueWaitIdle calls
    // below. Proceed with teardown — the OS reclaims VRAM. AwaitingReprovision is
    // a functional device (post-rebuild), so it waits normally via IsDeviceUsable.
    if (m_RecoveryEnabled && !m_Health.IsDeviceUsable())
    {
        Logger::Log::Warning("VulkanDevice: skipping global GPU idle wait — device health is {}",
                             DeviceHealthToString(m_Health.Load()));
        return true;
    }
    if (m_DeviceKnownIdle)
    {
        FlushRetiredWindowTargetSemaphores();
        return true;
    }

    const uint64_t graphicsSubmitted = SubmitContextFor(QueueType::Graphics).LastSignalled();
    if (HasGraphicsTimelineSemaphore() && graphicsSubmitted > 0)
    {
        if (!WaitTimelineSemaphoreValue(m_GraphicsTimeline, graphicsSubmitted, timeoutNs))
        {
            if (timeoutNs == 0)
            {
                return false;
            }
        }
    }

    if (m_GraphicsQueue && vkQueueWaitIdle(m_GraphicsQueue) != VK_SUCCESS)
    {
        return false;
    }
    if (m_ComputeQueue && vkQueueWaitIdle(m_ComputeQueue) != VK_SUCCESS)
    {
        return false;
    }
    if (m_TransferQueue && vkQueueWaitIdle(m_TransferQueue) != VK_SUCCESS)
    {
        return false;
    }
    if (m_PresentQueue && m_PresentQueue != m_GraphicsQueue && vkQueueWaitIdle(m_PresentQueue) != VK_SUCCESS)
    {
        return false;
    }

    m_DeviceKnownIdle = true;
    FlushRetiredWindowTargetSemaphores();
    return true;
}

void VulkanDevice::FlushPerFrameDeferredResourcesAfterIdle()
{
    for (auto& fr : m_Frames)
    {
        auto flushQueue = [&](PerQueueFrame& q)
        {
            // Concurrent producers: swap out under the deferral lock, destroy after.
            std::vector<BufferHandle> buffers;
            std::vector<TextureViewHandle> views;
            std::vector<SamplerHandle> samplers;
            std::vector<TextureHandle> textures;
            {
                std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
                buffers.swap(q.deferredBuffers);
                views.swap(q.deferredTextureViews);
                samplers.swap(q.deferredSamplers);
                textures.swap(q.deferredTextures);
            }
            for (auto h : views)
                if (h.IsValid())
                    DestroyTextureViewImmediate(h);
            for (auto h : samplers)
                if (h.IsValid())
                    DestroySamplerImmediate(h);
            for (auto h : textures)
                if (h.IsValid())
                    DestroyTextureImmediate(h);
            for (auto h : buffers)
                if (h.IsValid())
                    DestroyBufferImmediate(h);
        };
        flushQueue(fr.graphics);
        flushQueue(fr.compute);
        flushQueue(fr.transfer);
    }
}

void VulkanDevice::FlushTimelineDeferredResourcesAfterIdle()
{
    // The caller has fully idled the GPU, so every entry is retired regardless of
    // its timeline value. Take the queues under their lock, destroy outside it.
    std::vector<DeferredTextureView> views;
    std::vector<DeferredSampler> samplers;
    std::vector<DeferredTexture> textures;
    std::vector<DeferredBuffer> buffers;
    {
        std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
        views.swap(m_DeferredTextureViewsTimeline);
        samplers.swap(m_DeferredSamplersTimeline);
        textures.swap(m_DeferredTexturesTimeline);
        buffers.swap(m_DeferredBuffersTimeline);
    }
    for (const auto& dv : views)
        if (dv.handle.IsValid())
            DestroyTextureViewImmediate(dv.handle);
    for (const auto& ds : samplers)
        if (ds.handle.IsValid())
            DestroySamplerImmediate(ds.handle);
    for (const auto& dt : textures)
        if (dt.handle.IsValid())
            DestroyTextureImmediate(dt.handle);
    for (const auto& db : buffers)
        if (db.handle.IsValid())
            DestroyBufferImmediate(db.handle);
    std::vector<DeferredRecycleCommandBuffer> pendingRecycleCmdBuffers;
    {
        std::lock_guard<std::mutex> lock(m_DeferredStagingMutex);
        for (const auto& entry : m_DeferredStagingBuffers)
            if (entry.handle.IsValid())
                DestroyBufferImmediate(entry.handle);
        m_DeferredStagingBuffers.clear();
        pendingRecycleCmdBuffers.swap(m_DeferredRecycleCmdBuffers);
    }
    // The caller has fully idled the GPU, so every deferred CB has completed; return them to
    // their pools unconditionally (a timeline check would be redundant, and at teardown the
    // timeline semaphores may already be destroyed). Draining here also clears the dangling
    // pool pointers before DestroyAllThreadPools runs. At teardown the pools are destroyed
    // next — which frees these CBs anyway — so recycling first is harmless; mid-run
    // (WaitForIdle) it returns them for reuse.
    for (const auto& entry : pendingRecycleCmdBuffers)
    {
        if (!entry.pool || entry.commandBuffer == VK_NULL_HANDLE)
            continue;
        std::lock_guard<std::mutex> rlock(entry.pool->recycleMutex);
        entry.pool->freeCmd[ThreadCommandPoolEntry::kLevelPrimary][entry.freeListOrdinal]
            .push_back(entry.commandBuffer);
    }
}

void VulkanDevice::CompactLiveTrackingVectors()
{
    if (!m_LiveBuffers.empty())
    {
        auto it = std::remove_if(m_LiveBuffers.begin(), m_LiveBuffers.end(), [&](BufferHandle h)
                                 { return !h.IsValid() || GetVulkanBuffer(h) == nullptr; });
        m_LiveBuffers.erase(it, m_LiveBuffers.end());
    }
    if (!m_LiveTextures.empty())
    {
        auto it = std::remove_if(m_LiveTextures.begin(), m_LiveTextures.end(), [&](TextureHandle h)
                                 { return !h.IsValid() || GetVulkanTexture(h) == nullptr; });
        m_LiveTextures.erase(it, m_LiveTextures.end());
    }
    if (!m_LiveTextureViews.empty())
    {
        std::shared_lock viewLock(m_TextureViewMutex);
        auto it = std::remove_if(m_LiveTextureViews.begin(), m_LiveTextureViews.end(), [&](TextureViewHandle h)
                                 {
                                     if (!h.IsValid())
                                         return true;
                                     return m_TextureViews.Get(h) == nullptr;
                                 });
        m_LiveTextureViews.erase(it, m_LiveTextureViews.end());
    }
    if (!m_LiveSamplers.empty())
    {
        auto it = std::remove_if(m_LiveSamplers.begin(), m_LiveSamplers.end(), [&](SamplerHandle h)
                                 { return !h.IsValid() || GetVkSampler(h, m_Device) == VK_NULL_HANDLE; });
        m_LiveSamplers.erase(it, m_LiveSamplers.end());
    }
}

void VulkanDevice::FlushDeferredResourcesAndCompactLiveTracking()
{
    m_BulkDestroyInProgress = true;
    FlushPerFrameDeferredResourcesAfterIdle();
    FlushTimelineDeferredResourcesAfterIdle();
    CompactLiveTrackingVectors();
    {
        std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
        m_PendingBufferDestroyIds.clear();
        m_PendingTextureDestroyIds.clear();
        m_PendingTextureViewDestroyIds.clear();
        m_PendingSamplerDestroyIds.clear();
    }
    m_BulkDestroyInProgress = false;
}

void VulkanDevice::MaybeWarnOrForceWindowTargetRetirement()
{
    if (m_PendingWindowTargetRetirements.size() >= m_NextPendingRetirementWarnThreshold)
    {
        Logger::Log::Warning("[VulkanWindowTarget] pending retirements={} (nextWarn={})",
                             m_PendingWindowTargetRetirements.size(),
                             m_NextPendingRetirementWarnThreshold);
        m_NextPendingRetirementWarnThreshold *= 2;
    }
    if (m_RetiredWindowTargetSemaphores.size() >= m_NextRetiredWindowSemaphoreWarnThreshold)
    {
        Logger::Log::Warning("[VulkanWindowTarget] retired present semaphores={} (nextWarn={})",
                             m_RetiredWindowTargetSemaphores.size(),
                             m_NextRetiredWindowSemaphoreWarnThreshold);
        m_NextRetiredWindowSemaphoreWarnThreshold *= 2;
    }
}

bool VulkanDevice::WaitForSpecificGraphicsFence(uint32_t frameIndex, uint64_t timeoutNs)
{
    if (m_Device == VK_NULL_HANDLE)
    {
        return true;
    }
    if (frameIndex >= m_Frames.size())
    {
        return false;
    }
    auto& g = m_Frames[frameIndex].graphics;
    if (!g.fenceArmed || g.fence == VK_NULL_HANDLE)
    {
        return true;
    }
    const VkResult waitRes = vkWaitForFences(m_Device, 1, &g.fence, VK_TRUE, timeoutNs);
    if (waitRes == VK_SUCCESS)
    {
        return true;
    }
    if (waitRes != VK_TIMEOUT)
    {
        Logger::Log::Warning("WaitForSpecificGraphicsFence failed (res={}, frame={}, timeoutNs={})",
                             (int)waitRes,
                             frameIndex,
                             (unsigned long long)timeoutNs);
    }
    return false;
}

bool VulkanDevice::WaitForArmedGraphicsFences(uint64_t timeoutNs)
{
    if (m_Device == VK_NULL_HANDLE)
    {
        return true;
    }

    for (const auto& fr : m_Frames)
    {
        const auto& g = fr.graphics;
        if (!g.fenceArmed || g.fence == VK_NULL_HANDLE)
        {
            continue;
        }

        const VkResult waitRes = vkWaitForFences(m_Device, 1, &g.fence, VK_TRUE, timeoutNs);
        if (waitRes != VK_SUCCESS)
        {
            Logger::Log::Warning("WaitForArmedGraphicsFences failed (res={}, timeoutNs={})",
                                 (int)waitRes,
                                 (unsigned long long)timeoutNs);
            return false;
        }
    }
    return true;
}

void VulkanDevice::RetireOrDestroyWindowTargetSemaphore(VkSemaphore sem, bool safeToDestroyNow)
{
    if (sem == VK_NULL_HANDLE || m_Device == VK_NULL_HANDLE)
    {
        return;
    }
    if (safeToDestroyNow)
    {
        DestroySemaphoreTracked(sem);
        return;
    }
    m_RetiredWindowTargetSemaphores.push_back(sem);
    MaybeWarnOrForceWindowTargetRetirement();
}

void VulkanDevice::FlushRetiredWindowTargetSemaphores()
{
    if (m_Device == VK_NULL_HANDLE || m_RetiredWindowTargetSemaphores.empty())
    {
        return;
    }
    for (VkSemaphore sem : m_RetiredWindowTargetSemaphores)
    {
        DestroySemaphoreTracked(sem);
    }
    m_RetiredWindowTargetSemaphores.clear();
}

void VulkanDevice::DestroyWindowTargetState(WindowTargetState& state)
{
    const bool hasSwapchainObjects = (state.swapchain != VK_NULL_HANDLE) ||
                                     !state.swapchainFramebuffers.empty() ||
                                     !state.swapchainImageViews.empty();
    const bool hasPresentSemaphores = !state.presentReadySemaphores.empty();

    // Targeted drain: wait tracked graphics fences and only wait present queue when
    // present is on a separate queue. This avoids unconditional queue-wide stalls.
    if ((hasSwapchainObjects || hasPresentSemaphores) && !m_DeviceKnownIdle)
    {
        if (HasGraphicsTimelineSemaphore() && state.lastGraphicsTimelineValue > 0)
        {
            WaitTimelineSemaphoreValue(m_GraphicsTimeline, state.lastGraphicsTimelineValue, UINT64_MAX);
        }
        else if (state.lastArmedFrameIndex != UINT32_MAX)
        {
            WaitForSpecificGraphicsFence(state.lastArmedFrameIndex, UINT64_MAX);
        }
        else
        {
            WaitForArmedGraphicsFences(UINT64_MAX);
        }
    }
    bool presentFenceComplete = m_DeviceKnownIdle || !state.presentFenceArmed;
    if (!presentFenceComplete && state.presentCompleteFence != VK_NULL_HANDLE)
    {
        if (WaitForFence(state.presentCompleteFence, UINT64_MAX))
        {
            presentFenceComplete = true;
            state.presentFenceArmed = false;
        }
    }
    const bool waitedPresentQueueIdle = !presentFenceComplete &&
                                        hasPresentSemaphores &&
                                        (m_PresentQueue != VK_NULL_HANDLE) &&
                                        (m_PresentQueue != m_GraphicsQueue);
    if (waitedPresentQueueIdle)
    {
        vkQueueWaitIdle(m_PresentQueue);
        presentFenceComplete = true;
        state.presentFenceArmed = false;
    }

    for (auto framebuffer : state.swapchainFramebuffers)
    {
        if (framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(m_Device, framebuffer, nullptr);
        }
    }
    state.swapchainFramebuffers.clear();

    if (state.swapchainRenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_Device, state.swapchainRenderPass, nullptr);
        state.swapchainRenderPass = VK_NULL_HANDLE;
    }

    for (VkImageView view : state.swapchainImageViews)
    {
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_Device, view, nullptr);
        }
    }
    state.swapchainImageViews.clear();
    state.swapchainImages.clear();
    state.swapchainImageLayouts.clear();

    if (state.swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_Device, state.swapchain, nullptr);
        state.swapchain = VK_NULL_HANDLE;
    }

    if (state.surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_Instance, state.surface, nullptr);
        state.surface = VK_NULL_HANDLE;
    }

    const bool canDestroyPresentSemaphoresNow = m_DeviceKnownIdle ||
                                                presentFenceComplete ||
                                                waitedPresentQueueIdle ||
                                                (m_PresentQueue == VK_NULL_HANDLE);
    for (VkSemaphore sem : state.presentReadySemaphores)
    {
        RetireOrDestroyWindowTargetSemaphore(sem, canDestroyPresentSemaphoresNow);
    }
    state.presentReadySemaphores.clear();
    if (state.presentCompleteFence != VK_NULL_HANDLE)
    {
        if (state.presentFenceArmed && !m_DeviceKnownIdle)
        {
            WaitForFence(state.presentCompleteFence, UINT64_MAX);
            state.presentFenceArmed = false;
        }
        vkDestroyFence(m_Device, state.presentCompleteFence, nullptr);
        state.presentCompleteFence = VK_NULL_HANDLE;
    }
    state.swapchainTextures.clear();
    state.swapchainImageFormat = VK_FORMAT_UNDEFINED;
    state.swapchainExtent = {0, 0};
}

void VulkanDevice::SaveActiveWindowTargetState()
{
    if (!m_HasActiveWindowTarget || m_ActiveWindowTargetId == 0)
    {
        return;
    }
    m_WindowTargets[m_ActiveWindowTargetId] = CaptureWindowTargetState();
}

WindowTargetHandle VulkanDevice::CreateWindowTarget(void* windowHandle, uint32_t width, uint32_t height)
{
    const bool hadActive = m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0;
    const uint64_t previousActiveId = m_ActiveWindowTargetId;

    if (hadActive)
    {
        SaveActiveWindowTargetState();
    }

    ResetWindowTargetMembers();

    if (!CreateSurface(windowHandle) || !CreateSwapchain(width, height) || !CreateActiveWindowTargetPresentFence())
    {
        WindowTargetState failedState = CaptureWindowTargetState();
        DestroyWindowTargetState(failedState);
        ResetWindowTargetMembers();

        if (hadActive)
        {
            auto itPrev = m_WindowTargets.find(previousActiveId);
            if (itPrev != m_WindowTargets.end())
            {
                ApplyWindowTargetState(itPrev->second);
                m_ActiveWindowTargetId = previousActiveId;
                m_HasActiveWindowTarget = true;
            }
            else
            {
                m_ActiveWindowTargetId = 0;
                m_HasActiveWindowTarget = false;
                m_ActiveWindowTargetFrameIndex = 0;
            }
        }
        return WindowTargetHandle{};
    }

    const uint64_t newId = m_NextWindowTargetId++;
    m_WindowTargets[newId] = CaptureWindowTargetState();

    if (hadActive)
    {
        auto itPrev = m_WindowTargets.find(previousActiveId);
        if (itPrev != m_WindowTargets.end())
        {
            ApplyWindowTargetState(itPrev->second);
            m_ActiveWindowTargetId = previousActiveId;
            m_HasActiveWindowTarget = true;
        }
        else
        {
            m_ActiveWindowTargetId = newId;
            m_HasActiveWindowTarget = true;
        }
    }
    else
    {
        m_ActiveWindowTargetId = newId;
        m_HasActiveWindowTarget = true;
    }

    return WindowTargetHandle(newId);
}

bool VulkanDevice::DrainWindowTarget(WindowTargetHandle target, uint64_t timeoutNs)
{
    if (!target.IsValid())
    {
        return false;
    }

    const uint64_t id = target.id;
    WindowTargetState activeSnapshot{};
    const WindowTargetState* state = nullptr;
    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId == id)
    {
        activeSnapshot = CaptureWindowTargetState();
        state = &activeSnapshot;
    }
    else
    {
        auto it = m_WindowTargets.find(id);
        if (it == m_WindowTargets.end())
        {
            return false;
        }
        state = &it->second;
    }

    if (m_Device == VK_NULL_HANDLE)
    {
        return true;
    }

    if (m_DeviceKnownIdle)
    {
        return true;
    }

    bool graphicsDrained = true;
    if (HasGraphicsTimelineSemaphore() && state->lastGraphicsTimelineValue > 0)
    {
        graphicsDrained = WaitTimelineSemaphoreValue(m_GraphicsTimeline, state->lastGraphicsTimelineValue, timeoutNs);
    }
    else if (state->lastArmedFrameIndex != UINT32_MAX)
    {
        graphicsDrained = WaitForSpecificGraphicsFence(state->lastArmedFrameIndex, timeoutNs);
    }
    else if (timeoutNs != 0)
    {
        // Conservative fallback for legacy targets that predate per-target tracking.
        graphicsDrained = WaitForArmedGraphicsFences(timeoutNs);
    }
    else
    {
        graphicsDrained = false;
    }

    if (!graphicsDrained)
    {
        return false;
    }

    bool presentDrained = true;
    if (state->presentFenceArmed && state->presentCompleteFence != VK_NULL_HANDLE)
    {
        if (timeoutNs == 0)
        {
            presentDrained = IsFenceSignaled(state->presentCompleteFence);
        }
        else
        {
            presentDrained = WaitForFence(state->presentCompleteFence, timeoutNs);
        }
    }
    else if (!state->presentReadySemaphores.empty() &&
             m_PresentQueue != VK_NULL_HANDLE &&
             m_PresentQueue != m_GraphicsQueue)
    {
        if (timeoutNs == 0)
        {
            presentDrained = false;
        }
        else
        {
            const VkResult waitRes = vkQueueWaitIdle(m_PresentQueue);
            if (waitRes != VK_SUCCESS)
            {
                Logger::Log::Warning("DrainWindowTarget present queue wait failed (res={})", (int)waitRes);
                return false;
            }
        }
    }

    if (!presentDrained)
    {
        return false;
    }

    return true;
}

bool VulkanDevice::DestroyWindowTarget(WindowTargetHandle target)
{
    if (!target.IsValid())
    {
        return false;
    }

    const uint64_t id = target.id;
    auto it = m_WindowTargets.find(id);
    if (it == m_WindowTargets.end())
    {
        return false;
    }

    // Best-effort non-blocking drain; if not ready we retire asynchronously.
    const bool drainedNow = DrainWindowTarget(target, 0);
    const bool isActive = m_HasActiveWindowTarget && (m_ActiveWindowTargetId == id);
    WindowTargetState stateToRetire{};
    if (isActive)
    {
        stateToRetire = CaptureWindowTargetState();
        m_WindowTargets.erase(it);
        ResetWindowTargetMembers();
        m_ActiveWindowTargetId = 0;
        m_HasActiveWindowTarget = false;

        if (!m_WindowTargets.empty())
        {
            auto itNew = m_WindowTargets.begin();
            ApplyWindowTargetState(itNew->second);
            m_ActiveWindowTargetId = itNew->first;
            m_HasActiveWindowTarget = true;
        }
    }
    else
    {
        stateToRetire = std::move(it->second);
        m_WindowTargets.erase(it);
    }

    const uint64_t retireTimelineValue =
        (stateToRetire.lastGraphicsTimelineValue > 0)
            ? stateToRetire.lastGraphicsTimelineValue
            : SubmitContextFor(QueueType::Graphics).LastSignalled();
    if (drainedNow && m_DeviceKnownIdle)
    {
        DestroyWindowTargetState(stateToRetire);
    }
    else
    {
        EnqueueWindowTargetRetirement(id, std::move(stateToRetire), retireTimelineValue);
    }
    return true;
}

bool VulkanDevice::SetActiveWindowTarget(WindowTargetHandle target)
{
    if (!target.IsValid())
    {
        return false;
    }

    const uint64_t id = target.id;
    auto it = m_WindowTargets.find(id);
    if (it == m_WindowTargets.end())
    {
        return false;
    }

    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId == id)
    {
        return true;
    }

    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0)
    {
        SaveActiveWindowTargetState();
    }

    ApplyWindowTargetState(it->second);
    m_ActiveWindowTargetId = id;
    m_HasActiveWindowTarget = true;
    return true;
}

WindowTargetHandle VulkanDevice::GetActiveWindowTarget() const
{
    if (!m_HasActiveWindowTarget || m_ActiveWindowTargetId == 0)
    {
        return WindowTargetHandle{};
    }
    return WindowTargetHandle(m_ActiveWindowTargetId);
}

uint32_t VulkanDevice::GetFrameIndexForWindowTarget(WindowTargetHandle target) const
{
    if (!target.IsValid())
        return m_CurrentFrame;
    const uint64_t id = target.id;
    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId == id)
        return m_ActiveWindowTargetFrameIndex;
    auto it = m_WindowTargets.find(id);
    if (it != m_WindowTargets.end())
        return it->second.frameIndex;
    return m_CurrentFrame;
}

uint32_t VulkanDevice::GetActiveWindowTargetFrameIndex() const
{
    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0)
        return m_ActiveWindowTargetFrameIndex;
    return m_CurrentFrame;
}

bool VulkanDevice::RecreateWindowTargetSwapchain(WindowTargetHandle target, uint32_t width, uint32_t height)
{
    if (!SetActiveWindowTarget(target))
    {
        return false;
    }
    const bool ok = RecreateActiveSwapchain(width, height);
    if (ok)
    {
        SaveActiveWindowTargetState();
    }
    return ok;
}

void VulkanDevice::SetVsync(bool vsync)
{
    if (m_Vsync == vsync)
        return;
    m_Vsync = vsync;
    if (m_HasActiveWindowTarget)
    {
        const WindowTargetHandle activeTarget = GetActiveWindowTarget();
        RecreateWindowTargetSwapchain(activeTarget, m_SwapchainExtent.width, m_SwapchainExtent.height);
    }
}

bool VulkanDevice::SetHdrOutputMode(HdrOutputMode mode, const HdrStaticMetadata* metadata, HdrSwapchainBitDepth bitDepth)
{
    const bool enabled = mode != HdrOutputMode::Off;
    const HdrOutputState previousState = m_HdrState;
    const HdrStaticMetadata requestedMetadata = metadata ? *metadata : m_HdrState.staticMetadata;
    const bool metadataChanged = metadata && m_HdrState.staticMetadata != requestedMetadata;
    const bool changed = m_HdrState.enabled != enabled ||
                         m_HdrState.requestedMode != mode ||
                         m_HdrState.swapchainBitDepth != bitDepth ||
                         metadataChanged;
    m_HdrState.enabled = enabled;
    m_HdrState.requestedMode = mode;
    m_HdrState.swapchainBitDepth = bitDepth;
    m_HdrState.staticMetadata = requestedMetadata;
    if (!changed)
        return true;
    if (m_HasActiveWindowTarget)
    {
        const WindowTargetHandle activeTarget = GetActiveWindowTarget();
        const bool recreated = RecreateWindowTargetSwapchain(activeTarget, m_SwapchainExtent.width, m_SwapchainExtent.height);
        if (!recreated)
        {
            m_HdrState = previousState;
            Logger::Log::Warning("VulkanDevice: HDR output mode change to {} was deferred because swapchain recreation failed",
                                 HdrOutputModeToString(mode));
        }
        return recreated;
    }
    return true;
}

bool VulkanDevice::GetWindowTargetSize(WindowTargetHandle target, uint32_t& outWidth, uint32_t& outHeight) const
{
    if (!target.IsValid())
    {
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    const uint64_t id = target.id;
    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId == id)
    {
        return GetSwapchainSize(outWidth, outHeight);
    }
    auto it = m_WindowTargets.find(id);
    if (it == m_WindowTargets.end())
    {
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    outWidth = it->second.swapchainExtent.width;
    outHeight = it->second.swapchainExtent.height;
    return (outWidth > 0 && outHeight > 0);
}

bool VulkanDevice::IsTextureHandleLive(TextureHandle texture) const
{
    if (!texture.IsValid())
        return false;
    return GetVulkanTextureConst(texture) != nullptr;
}

bool VulkanDevice::RecreateActiveSwapchain(uint32_t width, uint32_t height)
{
    // Drain set. Below this point the function destroys the swapchain, its image
    // views, the swapchain render pass and the framebuffers over them, so every
    // queue that could still hold work referencing them has to be idle first.
    // Render passes and framebuffers are graphics-only object classes, and the
    // swapchain image is graphics-only by contract (RGFrame::Execute asserts that
    // no compute/transfer pass declares the backbuffer) -- but compute and transfer
    // are separately submitted queues on this device, so they are drained too
    // rather than resting this destroy on an invariant enforced in another module.
    // Queue waits are acceptable here and only here on a runtime path: recreation
    // is an event (resize, vsync/HDR toggle, monitor move), never per-frame.
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
    // Graphics drains through its armed fences with a deadline instead: a display
    // move can leave work that never retires, and deferring the recreate beats an
    // unbounded wait inside the platform's move handler.
    static constexpr uint64_t kMacDisplayMoveRecreateWaitNs = 2ull * 1000ull * 1000ull * 1000ull;
    if (!WaitForArmedGraphicsFences(kMacDisplayMoveRecreateWaitNs))
    {
        Logger::Log::Warning("VulkanDevice: deferred swapchain recreation because graphics work did not drain within {:.1f} ms",
                             static_cast<double>(kMacDisplayMoveRecreateWaitNs) / 1000000.0);
        return false;
    }
#else
    if (m_GraphicsQueue)
        vkQueueWaitIdle(m_GraphicsQueue);
#endif
    // vkQueuePresentKHR work is not tracked by the graphics fences, so fence-waiting
    // (the macOS path) or even draining the graphics queue does not guarantee a pending
    // present has finished with the swapchain/semaphores we are about to destroy. Drain
    // the present queue unconditionally -- including when it aliases the graphics queue
    // (the common case on MoltenVK), which is exactly when the old guard skipped it and
    // left a present in flight (VUID-vkDestroySwapchainKHR-swapchain-01282 / -semaphore-05149).
    if (m_PresentQueue)
        vkQueueWaitIdle(m_PresentQueue);
    // Compute and transfer are non-null only when the family selection found a
    // dedicated non-graphics family, so a live handle here is always a queue the
    // waits above did not cover.
    if (m_ComputeQueue)
        vkQueueWaitIdle(m_ComputeQueue);
    if (m_TransferQueue)
        vkQueueWaitIdle(m_TransferQueue);

    uint32_t reqW = width, reqH = height;
    VkSurfaceCapabilitiesKHR caps{};
    if (m_PhysicalDevice != VK_NULL_HANDLE && m_Surface != VK_NULL_HANDLE)
    {
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &caps);
        if (caps.currentExtent.width != UINT32_MAX)
        {
            reqW = caps.currentExtent.width;
            reqH = caps.currentExtent.height;
        }
    }
    if (reqW == 0 || reqH == 0)
    {
        Logger::Log::Warning("VulkanDevice: deferred swapchain recreation because the active surface size is {}x{}", reqW, reqH);
        return false;
    }

    // Clear per-frame DB set ring entries. After queue-idle above, all GPU work
    // referencing the current frame's DB offsets has completed, so the entries are
    // safe to drop. Without this, frameSlot-tagged entries outlive the swapchain-
    // recreate boundary and get mis-purged when the old slot index wraps around
    // (e.g., alt-tab / resize storms). Persistent entries (kPersistentFrameSlot
    // sentinel) are retained — they live in the DB pool's app-lifetime region.
    if (m_DescriptorBufferPool)
    {
        std::unique_lock lock(m_DescriptorBufferSetsMutex);
        for (auto it = m_DescriptorBufferSets.begin(); it != m_DescriptorBufferSets.end(); )
        {
            if (it->second.frameSlot != kPersistentFrameSlot)
                it = m_DescriptorBufferSets.erase(it);
            else
                ++it;
        }
    }

    // Destroy swapchain-dependent resources
    for (auto framebuffer : m_SwapchainFramebuffers)
    {
        if (framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(m_Device, framebuffer, nullptr);
        }
    }
    m_SwapchainFramebuffers.clear();
    for (size_t i = 0; i < m_SwapchainImageViews.size(); ++i)
    {
        if (m_SwapchainImageViews[i] != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_Device, m_SwapchainImageViews[i], nullptr);
            m_SwapchainImageViews[i] = VK_NULL_HANDLE;
        }
    }
    m_SwapchainImageViews.clear();
    if (m_SwapchainRenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_Device, m_SwapchainRenderPass, nullptr);
        m_SwapchainRenderPass = VK_NULL_HANDLE;
    }
    m_SwapchainImages.clear();
    m_SwapchainImageLayouts.clear();
    if (m_Swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }
    // Clear registered swapchain textures map so GetCurrentSwapchainImageHandle rebuilds entries
    m_SwapchainTextures.clear();
    m_CurrentSwapchainImage = UINT32_MAX;
    m_AcquiredThisFrame = false;
    m_AcquireSemaphoreThisFrame = VK_NULL_HANDLE;
    m_WaitedOnImageAvailableThisFrame = false;
    m_AnyGraphicsSubmitThisFrame = false;
    m_DidBackbufferRenderThisFrame = false;
    // Create new swapchain with the most accurate extent we know
    return CreateSwapchain(reqW, reqH);
}

// Note: AcquireNextImage and PresentImage are implemented below as Vulkan-specific methods
// They satisfy the abstract interface requirements with override keyword

namespace
{
// Encode swapchain image handles so they are unique per window target:
// [55] marker bit, [54:16] target id, [15:0] image index
static constexpr uint64_t kSwapchainHandleMarkerBit = (1ull << 55);
static constexpr uint64_t kSwapchainHandleImageBits = 16ull;
static constexpr uint64_t kSwapchainHandleImageMask = (1ull << kSwapchainHandleImageBits) - 1ull;
static constexpr uint64_t kSwapchainHandleTargetMask = (1ull << (55ull - kSwapchainHandleImageBits)) - 1ull;

static inline TextureHandle MakeSwapchainTextureHandle(uint64_t targetId, uint32_t imageIndex)
{
    const uint64_t idx = kSwapchainHandleMarkerBit |
                         ((targetId & kSwapchainHandleTargetMask) << kSwapchainHandleImageBits) |
                         (static_cast<uint64_t>(imageIndex) & kSwapchainHandleImageMask);
    return TextureHandle(idx, 1);
}

static inline bool DecodeSwapchainTextureHandle(TextureHandle handle, uint64_t& outTargetId, uint32_t& outImageIndex)
{
    const uint64_t idx = handle.Index();
    if ((idx & kSwapchainHandleMarkerBit) == 0)
        return false;
    outTargetId = (idx >> kSwapchainHandleImageBits) & kSwapchainHandleTargetMask;
    outImageIndex = static_cast<uint32_t>(idx & kSwapchainHandleImageMask);
    return outTargetId != 0;
}
} // namespace

TextureHandle VulkanDevice::GetSwapchainImage(uint32_t index) const
{
    if (index < m_SwapchainImages.size())
    {
        // Return handle to swapchain image.
        // IMPORTANT: keep this consistent with GetCurrentSwapchainImageHandle() encoding.
        const uint64_t targetId = (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0) ? m_ActiveWindowTargetId : 1ull;
        return MakeSwapchainTextureHandle(targetId, index);
    }
    return TextureHandle{}; // Invalid handle
}

// VMA (Vulkan Memory Allocator) methods
bool VulkanDevice::InitializeVMA()
{
#ifdef RENDERING_HAS_VMA
    VmaAllocatorCreateInfo allocatorInfo = {};
    // Clamp VMA's view of the Vulkan API to what the physical device reports,
    // and to at most 1.3. This keeps VMA from assuming features beyond what
    // the loader/driver combination actually supports (especially on macOS
    // configurations where only a subset of newer core versions are exposed).
    uint32_t apiForVma = m_DeviceProperties.apiVersion;
    if (apiForVma == 0)
    {
        apiForVma = VK_API_VERSION_1_0;
    }
    if (apiForVma > VK_API_VERSION_1_3)
    {
        apiForVma = VK_API_VERSION_1_3;
    }
    allocatorInfo.vulkanApiVersion = apiForVma;
    allocatorInfo.physicalDevice = m_PhysicalDevice;
    allocatorInfo.device = m_Device;

    allocatorInfo.instance = m_Instance;
    VmaVulkanFunctions vmaFunctions = {};
    vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    allocatorInfo.pVulkanFunctions = &vmaFunctions;

    // Enable buffer-device-address in VMA only when the device actually supports
    // bufferDeviceAddress. Setting the flag without the feature enabled would be
    // a VMA spec violation on any hypothetical driver that reports no BDA
    // support. The feature itself is enabled in CreateLogicalDevice under the
    // same gate (see supported12.bufferDeviceAddress / supportsBufferDeviceAddress).
    if (m_Capabilities.supportsBufferDeviceAddress)
    {
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    VkResult result = vmaCreateAllocator(&allocatorInfo, reinterpret_cast<VmaAllocator*>(&m_VmaAllocator));
    if (result != VK_SUCCESS)
    {
        Logger::Log::Error("Failed to create VMA allocator: {}", (int)result);
        return false;
    }
    return true;
#else

    return true;
#endif
}

void VulkanDevice::ShutdownVMA()
{
#ifdef RENDERING_HAS_VMA
    if (m_VmaAllocator)
    {
        // DEBUG guards: By this point, all tracked resources should be destroyed.
        // If registries still contain entries, warn (we will still force-destroy for safety).
#if defined(DEBUG) || defined(_DEBUG)
        if (!m_LiveBuffers.empty() || !m_LiveTextures.empty() || !m_LiveSamplers.empty())
        {
            Logger::Log::Warning(
                "[VMA Shutdown] Live resource handles remain (buffers={}, textures={}, samplers={}) at allocator shutdown. Forcing cleanup.",
                m_LiveBuffers.size(),
                m_LiveTextures.size(),
                m_LiveSamplers.size());
        }
        if (!m_VmaBufferAllocs.empty() || !m_VmaImageAllocs.empty())
        {
            Logger::Log::Warning(
                "[VMA Shutdown] Raw VMA allocation registries not empty (buffers={}, images={}). Forcing cleanup.",
                m_VmaBufferAllocs.size(),
                m_VmaImageAllocs.size());
        }
        if (!m_DirectMemoryAllocations.empty())
        {
            Logger::Log::Warning(
                "[VMA Shutdown] Direct Vulkan allocations still tracked (count={}).", m_DirectMemoryAllocations.size());
        }
#endif
        // Defensive sweep: destroy any VMA-backed buffers/images that escaped normal tracking
        if (!m_VmaBufferAllocs.empty() || !m_VmaImageAllocs.empty())
        {
            for (auto& kv : m_VmaBufferAllocs)
            {
                VmaAllocation alloc = static_cast<VmaAllocation>(kv.first);
                VkBuffer buf = kv.second;
                if (buf != VK_NULL_HANDLE && alloc)
                {
                    vmaDestroyBuffer(AsVmaAllocator(m_VmaAllocator), buf, alloc);
                }
            }
            m_VmaBufferAllocs.clear();
            for (auto& kv : m_VmaImageAllocs)
            {
                VmaAllocation alloc = static_cast<VmaAllocation>(kv.first);
                VkImage img = kv.second;
                if (img != VK_NULL_HANDLE && alloc)
                {
                    vmaDestroyImage(AsVmaAllocator(m_VmaAllocator), img, alloc);
                }
            }
            m_VmaImageAllocs.clear();
        }

        // Debug: print live allocation stats before shutdown to catch leaks in tests
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(AsVmaAllocator(m_VmaAllocator), &stats);
        // Detailed leak diagnostics (gated behind env var to avoid JSON spam)
        if (IsEnvEnabled("GE_DUMP_VMA_STATS") || IsEnvEnabled("GE_VMA_DUMP"))
        {
            char* statsStr = nullptr;
            vmaBuildStatsString(AsVmaAllocator(m_VmaAllocator), &statsStr, VK_TRUE);
            if (statsStr)
            {
                vmaFreeStatsString(AsVmaAllocator(m_VmaAllocator), statsStr);
            }
        }
        vmaDestroyAllocator(AsVmaAllocator(m_VmaAllocator));
        m_VmaAllocator = nullptr;
    }
#endif
}

// Debug/testing stats implementation
size_t VulkanDevice::DebugGetAllocationCount() const
{
#ifdef RENDERING_HAS_VMA
    if (m_VmaAllocator)
    {
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(AsVmaAllocator(m_VmaAllocator), &stats);
        return static_cast<size_t>(stats.total.statistics.allocationCount);
    }
#endif
    return 0;
}

size_t VulkanDevice::DebugGetBufferRegistryCount() const
{
#ifdef RENDERING_HAS_VMA
    return m_VmaBufferAllocs.size();
#else
    return 0;
#endif
}

size_t VulkanDevice::DebugGetImageRegistryCount() const
{
#ifdef RENDERING_HAS_VMA
    return m_VmaImageAllocs.size();
#else
    return 0;
#endif
}

size_t VulkanDevice::DebugGetAllocatedBytes() const
{
#ifdef RENDERING_HAS_VMA
    if (m_VmaAllocator)
    {
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(AsVmaAllocator(m_VmaAllocator), &stats);
        return static_cast<size_t>(stats.total.statistics.allocationBytes);
    }
#endif
    return 0;
}

size_t VulkanDevice::DebugGetTextureBytes() const
{
#ifdef RENDERING_HAS_VMA
    if (!m_VmaAllocator)
        return 0;
    size_t total = 0;
    for (const auto& kv : m_VmaImageAllocs)
    {
        VmaAllocation alloc = static_cast<VmaAllocation>(kv.first);
        if (!alloc)
            continue;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(AsVmaAllocator(m_VmaAllocator), alloc, &info);
        total += static_cast<size_t>(info.size);
    }
    return total;
#else
    return 0;
#endif
}

size_t VulkanDevice::DebugGetBufferBytes() const
{
#ifdef RENDERING_HAS_VMA
    if (!m_VmaAllocator)
        return 0;
    size_t total = 0;
    for (const auto& kv : m_VmaBufferAllocs)
    {
        VmaAllocation alloc = static_cast<VmaAllocation>(kv.first);
        if (!alloc)
            continue;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(AsVmaAllocator(m_VmaAllocator), alloc, &info);
        total += static_cast<size_t>(info.size);
    }
    return total;
#else
    return 0;
#endif
}

static TextureFormat FromVkFormat(VkFormat f); // defined below; used here for the VRAM list

void VulkanDevice::DebugEnumerateResources(
    const std::function<void(const DebugResourceInfo&)>& fn) const
{
    if (!fn)
        return;
    std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);

    auto allocBytes = [this](VmaAllocation alloc, VkDeviceSize fallback) -> uint64_t {
#ifdef RENDERING_HAS_VMA
        if (m_VmaAllocator && alloc)
        {
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(AsVmaAllocator(m_VmaAllocator), alloc, &info);
            return static_cast<uint64_t>(info.size);
        }
#endif
        return static_cast<uint64_t>(fallback);
    };

    for (BufferHandle h : m_LiveBuffers)
    {
        const VulkanBuffer* b = GetVulkanBuffer(h);
        if (!b || b->buffer == VK_NULL_HANDLE)
            continue;
        DebugResourceInfo info;
        info.Type = DebugResourceInfo::Kind::Buffer;
        info.Name = b->debugName.empty() ? "(unnamed buffer)" : b->debugName;
        info.Bytes = allocBytes(b->allocation, b->size);
        fn(info);
    }
    for (TextureHandle h : m_LiveTextures)
    {
        const VulkanTexture* t = GetVulkanTexture(h);
        if (!t || t->image == VK_NULL_HANDLE)
            continue;
        DebugResourceInfo info;
        info.Type = DebugResourceInfo::Kind::Texture;
        info.Name = t->debugName.empty() ? "(unnamed texture)" : t->debugName;
        info.Bytes = allocBytes(t->allocation, 0);
        info.Width = t->extent.width;
        info.Height = t->extent.height;
        info.Format = FromVkFormat(t->format);
        fn(info);
    }
}

void VulkanDevice::UpdateResourcePoolHighWater()
{
    if (!IsResourcePoolDiagnosticsEnabled())
        return;
    const ResourcePoolStats s = GetResourcePoolStats();
    auto bump = [](size_t& dst, size_t value)
    {
        if (value > dst)
            dst = value;
    };
    bump(m_ResourcePoolHighWater.cmdBuffersFree, s.cmdBuffersFree);
    bump(m_ResourcePoolHighWater.cmdBuffersUsed, s.cmdBuffersUsed);
    bump(m_ResourcePoolHighWater.deferredBuffers, s.deferredBuffers);
    bump(m_ResourcePoolHighWater.deferredTextures, s.deferredTextures);
    bump(m_ResourcePoolHighWater.deferredTextureViews, s.deferredTextureViews);
    bump(m_ResourcePoolHighWater.deferredSamplers, s.deferredSamplers);
    bump(m_ResourcePoolHighWater.deferredStagingBuffers, s.deferredStagingBuffers);
    bump(m_ResourcePoolHighWater.liveBuffers, s.liveBuffers);
    bump(m_ResourcePoolHighWater.liveTextures, s.liveTextures);
    bump(m_ResourcePoolHighWater.liveTextureViews, s.liveTextureViews);
    bump(m_ResourcePoolHighWater.descriptorTransientPools, s.descriptorTransientPools);
    bump(m_ResourcePoolHighWater.descriptorPersistentPools, s.descriptorPersistentPools);
}

void VulkanDevice::MaybeLogLiveBufferBreakdown()
{
    if (!IsResourcePoolDiagnosticsEnabled())
        return;

    const size_t liveCount = m_LiveBuffers.size();
    if (liveCount < m_NextLiveBufferBreakdownThreshold)
        return;

    const size_t sampleBudget = 200000;
    const size_t step = std::max<size_t>(1, liveCount / sampleBudget);
    std::unordered_map<std::string, size_t> counts;
    counts.reserve(std::min<size_t>(liveCount / step, 4096));

    for (size_t i = 0; i < liveCount; i += step)
    {
        const BufferHandle h = m_LiveBuffers[i];
        const VulkanBuffer* buffer = GetVulkanBufferConst(h);
        std::string key;
        if (!buffer)
        {
            key = "<invalid>";
        }
        else if (!buffer->debugName.empty())
        {
            key = buffer->debugName;
        }
        else
        {
            key = "<unnamed>";
        }
        counts[key] += 1;
    }

    std::vector<std::pair<std::string, size_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
              { return a.second > b.second; });

    const size_t topN = std::min<size_t>(6, sorted.size());
    std::string summary;
    for (size_t i = 0; i < topN; ++i)
    {
        if (!summary.empty())
            summary += ", ";
        summary += sorted[i].first;
        summary += ":~";
        summary += std::to_string(sorted[i].second * step);
    }

    Logger::Log::Warning("[VulkanPools][live-buffer-breakdown] live={} sampleStep={} approxTop=[{}]",
                         liveCount,
                         step,
                         summary);

    size_t nextThreshold = liveCount * 2;
    if (nextThreshold <= m_NextLiveBufferBreakdownThreshold)
        nextThreshold = m_NextLiveBufferBreakdownThreshold + 1;
    m_NextLiveBufferBreakdownThreshold = nextThreshold;
}

void VulkanDevice::LogResourcePoolHighWater(const char* reason) const
{
    if (!IsResourcePoolDiagnosticsEnabled())
        return;
    const ResourcePoolStats current = GetResourcePoolStats();
    Logger::Log::Info(
        "[VulkanPools][{}] current cmd(free={},used={}) deferred(B/T/V/S)={}/{}/{}/{} staging={} live(B/T/V)={}/{}/{} dsPools(T/P)={}/{} | high-water cmd(free={},used={}) deferred(B/T/V/S)={}/{}/{}/{} staging={} live(B/T/V)={}/{}/{} dsPools(T/P)={}/{}",
        reason ? reason : "snapshot",
        current.cmdBuffersFree,
        current.cmdBuffersUsed,
        current.deferredBuffers,
        current.deferredTextures,
        current.deferredTextureViews,
        current.deferredSamplers,
        current.deferredStagingBuffers,
        current.liveBuffers,
        current.liveTextures,
        current.liveTextureViews,
        current.descriptorTransientPools,
        current.descriptorPersistentPools,
        m_ResourcePoolHighWater.cmdBuffersFree,
        m_ResourcePoolHighWater.cmdBuffersUsed,
        m_ResourcePoolHighWater.deferredBuffers,
        m_ResourcePoolHighWater.deferredTextures,
        m_ResourcePoolHighWater.deferredTextureViews,
        m_ResourcePoolHighWater.deferredSamplers,
        m_ResourcePoolHighWater.deferredStagingBuffers,
        m_ResourcePoolHighWater.liveBuffers,
        m_ResourcePoolHighWater.liveTextures,
        m_ResourcePoolHighWater.liveTextureViews,
        m_ResourcePoolHighWater.descriptorTransientPools,
        m_ResourcePoolHighWater.descriptorPersistentPools);
}

IDevice::ResourcePoolStats VulkanDevice::GetResourcePoolStats() const
{
    ResourcePoolStats s{};
    // Deferred queues have concurrent producers; sample them under their lock so a
    // caller never observes a torn count.
    {
        std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
        for (const auto& fr : m_Frames)
        {
            auto addQueue = [&](const PerQueueFrame& pq)
            {
                s.cmdBuffersFree += pq.freeCmd.size();
                s.cmdBuffersUsed += pq.usedCmd.size();
                s.deferredBuffers += pq.deferredBuffers.size();
                s.deferredTextures += pq.deferredTextures.size();
                s.deferredTextureViews += pq.deferredTextureViews.size();
                s.deferredSamplers += pq.deferredSamplers.size();
            };
            addQueue(fr.graphics);
            addQueue(fr.compute);
            addQueue(fr.transfer);
        }
        s.deferredBuffers += m_DeferredBuffersTimeline.size();
        s.deferredTextures += m_DeferredTexturesTimeline.size();
        s.deferredTextureViews += m_DeferredTextureViewsTimeline.size();
        s.deferredSamplers += m_DeferredSamplersTimeline.size();
    }
    // Staging buffers have worker-thread producers (SubmitTextureUploads, UpdateBuffer),
    // so the size read takes their guard rather than racing the push. The scope closes
    // before the view/tracking locks below, so this adds no lock-ordering edge.
    {
        std::lock_guard<std::mutex> stagingLock(m_DeferredStagingMutex);
        s.deferredStagingBuffers = m_DeferredStagingBuffers.size();
    }
    // Live counts and per-view metadata are sampled together so the documented
    // "3 metadata entries per live view" invariant is checkable at any time.
    // Lock ordering: m_TextureViewMutex < m_ResourceTrackingMutex.
    {
        std::shared_lock viewLock(m_TextureViewMutex);
        std::lock_guard<std::mutex> trackingLock(m_ResourceTrackingMutex);
        s.liveBuffers = m_LiveBuffers.size();
        s.liveTextures = m_LiveTextures.size();
        s.liveTextureViews = m_LiveTextureViews.size();
        s.liveSamplers = m_LiveSamplers.size();
        s.textureViewMetadataEntries =
            m_TextureViewAspects.size() + m_TextureViewFormats.size() + m_TextureViewOwners.size();
    }
    auto dsStats = m_DsAllocator.GetStats();
    s.descriptorTransientPools = dsStats.TransientPools;
    s.descriptorPersistentPools = dsStats.PersistentPools;
    return s;
}

// Direct memory allocation tracking methods
void VulkanDevice::TrackDirectMemoryAllocation(VkDeviceMemory memory)
{
    if (memory != VK_NULL_HANDLE)
    {
        m_DirectMemoryAllocations.push_back(memory);
    }
}

void VulkanDevice::UntrackDirectMemoryAllocation(VkDeviceMemory memory)
{
    if (memory != VK_NULL_HANDLE)
    {
        auto it = std::find(m_DirectMemoryAllocations.begin(), m_DirectMemoryAllocations.end(), memory);
        if (it != m_DirectMemoryAllocations.end())
        {
            m_DirectMemoryAllocations.erase(it);
        }
        else
        {
            Logger::Log::Warning("Attempted to untrack memory that wasn't tracked: {}", (const void*)memory);
        }
    }
}

void VulkanDevice::CleanupDirectMemoryAllocations()
{
    for (VkDeviceMemory memory : m_DirectMemoryAllocations)
    {
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_Device, memory, nullptr);
        }
    }
    m_DirectMemoryAllocations.clear();
}

ResourceManager* VulkanDevice::GetResourceManager()
{
    // Create ResourceManager instance if not already created
    if (!m_ResourceManager)
    {
        m_ResourceManager = std::make_unique<ResourceManager>(this);
    }
    return m_ResourceManager.get();
}

// Debug events are now inline no-ops in the header

// GPU timeline profiling removed - use external profiling tools instead

// EndTimestamp method removed

// GetTimestamp method removed

// ResetTimestamps method removed

// Indirect command support removed - moved to CommandList interface

// Surface and swapchain implementation
bool VulkanDevice::CreateSurface(void* windowHandle)
{
    if (!windowHandle)
    {
        Logger::Log::Error("Invalid window handle provided");
        return false;
    }

    // Create surface from GLFW window
    GLFWwindow* glfwWindow = static_cast<GLFWwindow*>(windowHandle);

    // Prefer GLFW path; if it fails, attempt direct Win32 surface creation as a fallback
    VkResult result = glfwCreateWindowSurface(m_Instance, glfwWindow, nullptr, &m_Surface);
#if defined(_WIN32)
    if (result != VK_SUCCESS)
    {
        HWND hwnd = glfwGetWin32Window(glfwWindow);
        HINSTANCE hinstance = GetModuleHandle(NULL);
        VkWin32SurfaceCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        sci.hinstance = hinstance;
        sci.hwnd = hwnd;
        auto fpCreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(m_Instance, "vkCreateWin32SurfaceKHR"));
        if (fpCreateWin32SurfaceKHR)
        {
            result = fpCreateWin32SurfaceKHR(m_Instance, &sci, nullptr, &m_Surface);
        }
    }
#endif

    if (result != VK_SUCCESS)
    {
        if (result == VK_ERROR_EXTENSION_NOT_PRESENT)
        {
#if defined(PLATFORM_MACOS)
            Logger::Log::Error(
                "glfwCreateWindowSurface failed with VK_ERROR_EXTENSION_NOT_PRESENT. "
                "On macOS this usually means the Vulkan instance is missing VK_EXT_metal_surface (or VK_MVK_macos_surface) "
                "and/or GLFW could not use a Vulkan loader. Ensure MoltenVK is installed and visible to the Vulkan loader. "
                "If your app initializes GLFW manually with a non-standard Vulkan loader location, call glfwInitVulkanLoader(vkGetInstanceProcAddr) before glfwInit (GLFW 3.4+).");
#endif
        }
        Logger::Log::Error("Failed to create window surface! Error: {}", (int)result);
        return false;
    }

    // Check if surface is supported by our physical device
    if (!CheckSurfaceSupport())
    {
        Logger::Log::Error("Surface not supported by physical device");
        return false;
    }

    return true;
}

// Forward declaration for VkFormat -> TextureFormat mapper used below.
// Defined later in this file near other format helpers.
static TextureFormat FromVkFormat(VkFormat f);

bool VulkanDevice::CreateSwapchain(uint32_t width, uint32_t height)
{

    if (m_Surface == VK_NULL_HANDLE)
    {
        Logger::Log::Error("Cannot create swapchain without surface");
        return false;
    }

    // Query surface capabilities
    VkSurfaceCapabilitiesKHR surfaceCapabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_PhysicalDevice, m_Surface, &surfaceCapabilities);

    // Query surface formats
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_PhysicalDevice, m_Surface, &formatCount, surfaceFormats.data());
    if (m_HdrState.enabled)
    {
        Logger::Log::Info("VulkanDevice: HDR requested mode={} swapchainColorSpaceExt={} surfaceFormatCount={}",
                          HdrOutputModeToString(m_HdrState.requestedMode),
                          m_SwapchainColorSpaceExtEnabled ? "true" : "false",
                          surfaceFormats.size());
        for (size_t i = 0; i < surfaceFormats.size(); ++i)
        {
            Logger::Log::Info("VulkanDevice: HDR surface format[{}] VkFormat={} ColorSpace={}",
                              i,
                              static_cast<int>(surfaceFormats[i].format),
                              static_cast<int>(surfaceFormats[i].colorSpace));
        }
    }
    HdrDisplayInfo hdrDisplay{};
    hdrDisplay.requestedMode = m_HdrState.requestedMode;
    hdrDisplay.paperWhiteNits = m_HdrState.staticMetadata.paperWhiteNits;
    hdrDisplay.swapchainBitDepth = m_HdrState.swapchainBitDepth;
    for (const VkSurfaceFormatKHR& format : surfaceFormats)
    {
        hdrDisplay.colorSpaces.push_back(std::to_string(static_cast<int>(format.colorSpace)));
        // Portability drivers (MoltenVK) enumerate every colorspace as a static formats x colorspaces
        // product regardless of real display capability, so ST2084/HLG aren't a trustworthy signal
        // there — only scRGB/EDR is a real, honored HDR path on macOS. (See m_IsPortabilitySubsetDevice;
        // cf. VK_EXT_swapchain_colorspace not tying enumeration to capability, dolphin-emu/dolphin#13207.)
        // On conformant WSI (Windows/Linux) the enumeration does track real capability.
        if (format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && !m_IsPortabilitySubsetDevice)
        {
            hdrDisplay.supportsHDR10_PQ = true;
            hdrDisplay.hdrAvailable = true;
        }
        if (format.colorSpace == VK_COLOR_SPACE_HDR10_HLG_EXT && !m_IsPortabilitySubsetDevice)
        {
            hdrDisplay.supportsHLG = true;
            hdrDisplay.hdrAvailable = true;
        }
        if (format.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
        {
            hdrDisplay.supportsScRGB = true;
            hdrDisplay.hdrAvailable = true;
        }
    }
    hdrDisplay.diagnosticHints.push_back(m_SwapchainColorSpaceExtEnabled
                                             ? "VK_EXT_swapchain_colorspace enabled."
                                             : "VK_EXT_swapchain_colorspace not exposed; HDR10/HLG Vulkan color spaces are unavailable.");
    if (m_HdrState.enabled && !hdrDisplay.hdrAvailable)
        hdrDisplay.diagnosticHints.push_back("No HDR Vulkan surface formats exposed for the active monitor/compositor.");
    m_HdrState.display = hdrDisplay;

    // Query present modes
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_PhysicalDevice, m_Surface, &presentModeCount, presentModes.data());

    // Choose swapchain settings
    VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(surfaceFormats);
    VkPresentModeKHR presentMode = ChooseSwapPresentMode(presentModes);
    VkExtent2D extent = ChooseSwapExtent(surfaceCapabilities, width, height);

    // Determine number of swapchain images.
    //
    // NOTE: This is not required to match our CPU "frames-in-flight" count (`MAX_FRAMES_IN_FLIGHT`).
    // - Swapchain image count controls how many presentable images the WSI/presentation engine can rotate through.
    // - Frames-in-flight controls how many frames of CPU-side per-frame resources (fences, command buffers, transient allocators)
    //   we maintain and can have submitted concurrently.
    //
    // In practice, preferring 3 swapchain images (triple buffering) often reduces acquire/present stalls, even if we only keep
    // 2 frames worth of CPU-side resources in flight for latency reasons.
    uint32_t imageCount = std::max(3u, surfaceCapabilities.minImageCount + 1);
    if (surfaceCapabilities.maxImageCount > 0 && imageCount > surfaceCapabilities.maxImageCount)
    {
        imageCount = surfaceCapabilities.maxImageCount;
    }

    // Create swapchain
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_Surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Allow swapchain readback (screenshots, frame capture) when the surface
    // permits it. Pure hint — no GPU cost; drivers that don't support it leave
    // the bit off and the screenshot path falls back to FinalLinear or viewport.
    if (surfaceCapabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (surfaceCapabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Record whether the backbuffer is transfer-source capable: the screenshot
    // path reads it directly for full-window capture when there is no manual-encode
    // FinalLinear intermediate (native-sRGB swapchain, HDR off).
    m_SwapchainSupportsReadback = (createInfo.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

    // Handle queue families
    uint32_t queueFamilyIndices[] = {m_GraphicsQueueFamily, m_PresentQueueFamily};
    if (m_GraphicsQueueFamily != m_PresentQueueFamily)
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = surfaceCapabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    VkResult result = vkCreateSwapchainKHR(m_Device, &createInfo, nullptr, &m_Swapchain);
    if (result != VK_SUCCESS)
    {
        Logger::Log::Error("Failed to create swapchain! Error: {}", (int)result);
        return false;
    }

    // Store swapchain properties
    m_SwapchainImageFormat = surfaceFormat.format;
    m_SwapchainColorSpace = surfaceFormat.colorSpace;
    m_SwapchainExtent = extent;

    // Debug: log the chosen swapchain format and color space in engine terms
    TextureFormat swapchainFmt = FromVkFormat(m_SwapchainImageFormat);
    // An unmapped swapchain format degrades silently and wrongly: GetSwapchainTextureFormat()
    // reports Unknown, SwapchainNeedsManualSRGBEncode() takes its default arm and answers false,
    // and the SDR path then presents linear values with no OETF (a much too dark frame). Two
    // routes reach here — ChooseSwapSurfaceFormat's A2R10G10B10 ten-bit fallback, which has no
    // FromVkFormat row, and its last-resort "first available format", which is whatever the
    // driver enumerated. Warn once rather than refuse: the last-resort route has no mapped
    // alternative to fall back to, so refusing there would mean no swapchain at all.
    if (swapchainFmt == TextureFormat::Unknown)
    {
        static std::atomic<bool> warnedUnmappedSwapchainFormat{false};
        if (!warnedUnmappedSwapchainFormat.exchange(true))
        {
            Logger::Log::Warning(
                "VulkanDevice: swapchain VkFormat {} has no FromVkFormat mapping, so the engine sees "
                "TextureFormat::Unknown. SwapchainNeedsManualSRGBEncode() therefore returns false and the "
                "SDR path emits no sRGB encode — expect a too-dark image. Fix: add a FromVkFormat row for "
                "this format (and a TextureFormat enumerator if none describes its channel order).",
                static_cast<int32_t>(m_SwapchainImageFormat));
        }
    }
    m_HdrState.display.swapchainFormat = swapchainFmt;
    m_HdrState.display.nativeFormat = static_cast<uint32_t>(m_SwapchainImageFormat);
    m_HdrState.display.nativeColorSpace = static_cast<uint32_t>(m_SwapchainColorSpace);
    m_HdrState.display.swapchainBitDepth = m_HdrState.swapchainBitDepth;
    m_HdrState.display.width = m_SwapchainExtent.width;
    m_HdrState.display.height = m_SwapchainExtent.height;
    m_HdrState.display.resolvedMode = HdrOutputMode::Off;
    m_HdrState.activeMode = HdrOutputMode::Off;
    if (surfaceFormat.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
        m_HdrState.activeMode = HdrOutputMode::HDR10_PQ;
    if (surfaceFormat.colorSpace == VK_COLOR_SPACE_HDR10_HLG_EXT)
        m_HdrState.activeMode = HdrOutputMode::HLG;
    if (surfaceFormat.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
        m_HdrState.activeMode = HdrOutputMode::ScRGB;
    if (!m_HdrState.enabled)
        m_HdrState.activeMode = HdrOutputMode::Off;
    m_HdrState.display.hdrActive = IsHdrOutputModeActive(m_HdrState.activeMode);
    m_HdrState.display.resolvedMode = m_HdrState.activeMode;
    m_HdrState.display.outputMaxLinearValue = GetHdrOutputMaxLinearValue(m_HdrState);

#ifdef VK_EXT_HDR_METADATA_EXTENSION_NAME
    if (m_FpSetHdrMetadataEXT && m_Swapchain != VK_NULL_HANDLE && m_HdrState.activeMode == HdrOutputMode::HDR10_PQ)
    {
        const HdrStaticMetadata& src = m_HdrState.staticMetadata;
        VkHdrMetadataEXT metadata{};
        metadata.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
        metadata.displayPrimaryRed = {src.redPrimary[0], src.redPrimary[1]};
        metadata.displayPrimaryGreen = {src.greenPrimary[0], src.greenPrimary[1]};
        metadata.displayPrimaryBlue = {src.bluePrimary[0], src.bluePrimary[1]};
        metadata.whitePoint = {src.whitePoint[0], src.whitePoint[1]};
        metadata.maxLuminance = src.maxMasteringLuminance;
        metadata.minLuminance = src.minMasteringLuminance;
        metadata.maxContentLightLevel = src.maxContentLightLevel;
        metadata.maxFrameAverageLightLevel = src.maxFrameAverageLightLevel;
        m_FpSetHdrMetadataEXT(m_Device, 1, &m_Swapchain, &metadata);
        m_HdrState.display.diagnosticHints.push_back("Static HDR10 metadata applied with vkSetHdrMetadataEXT.");
    }
#endif
    bool swapchainIsSRGB =
        (swapchainFmt == TextureFormat::RGBA8_SRGB) ||
        (swapchainFmt == TextureFormat::BGRA8_SRGB);
    Logger::Log::Info(
        "VulkanDevice: Created window swapchain {}x{}, VkFormat={} ColorSpace={} TextureFormat={} (sRGB={}) HDR={} outputMaxLinear={:.2f}",
        m_SwapchainExtent.width,
        m_SwapchainExtent.height,
        static_cast<int>(m_SwapchainImageFormat),
        static_cast<int>(surfaceFormat.colorSpace),
        static_cast<int>(swapchainFmt),
        swapchainIsSRGB ? "true" : "false",
        HdrOutputModeToString(m_HdrState.activeMode),
        m_HdrState.display.outputMaxLinearValue);

    Logger::Log::Info(
        "VulkanDevice: Swapchain images={} (surface min={}, max={})",
        imageCount,
        surfaceCapabilities.minImageCount,
        surfaceCapabilities.maxImageCount);

    // Get swapchain images
    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &imageCount, nullptr);
    m_SwapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_Device, m_Swapchain, &imageCount, m_SwapchainImages.data());
    for (size_t i = 0; i < imageCount; i++)
    {
        const std::string name = "swapchain_image_" + std::to_string(i);
        SetVkObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(m_SwapchainImages[i]),
                        name.c_str());
    }
    // Initialize per-image layout tracking (best-effort). Swapchain images are treated as UNDEFINED
    // until we explicitly transition them for rendering and then to PRESENT in Present().
    m_SwapchainImageLayouts.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);

    // Create image views
    m_SwapchainImageViews.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_SwapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_SwapchainImageFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_Device, &viewInfo, nullptr, &m_SwapchainImageViews[i]) != VK_SUCCESS)
        {
            Logger::Log::Error("Failed to create image view {}", (int)i);
            return false;
        }
        const std::string viewName = "swapchain_image_" + std::to_string(i) + "_view";
        SetVkObjectName(VK_OBJECT_TYPE_IMAGE_VIEW,
                        reinterpret_cast<uint64_t>(m_SwapchainImageViews[i]), viewName.c_str());
    }
    // Swapchain images are left tracked as UNDEFINED (above). We deliberately do
    // NOT pre-transition them to PRESENT_SRC here: a swapchain image may only be
    // transitioned after it has been acquired, so a transition at creation trips
    // UNASSIGNED-non-acquired-swapchain-image-used. It is also unnecessary — the
    // command list's barrier resolver explicitly keeps oldLayout=UNDEFINED for a
    // freshly-created image (deviceTrackedOld==UNDEFINED; see VulkanCommandList.cpp
    // "keep resolvedOld = UNDEFINED so the barrier discards and transitions
    // correctly"), so the first per-frame barrier transitions the just-acquired
    // image UNDEFINED->COLOR_ATTACHMENT and RecordPresentTransition then drives it
    // to PRESENT_SRC, with the tracked layout following each barrier. The same
    // path runs after a swapchain recreate, since the layouts are reset to
    // UNDEFINED above.

    // Rebuild presentReady semaphores per-swapchain-image to avoid reuse hazards across images
    for (VkSemaphore s : m_PresentReadySemaphores)
    {
        DestroySemaphoreTracked(s);
    }
    m_PresentReadySemaphores.clear();
    m_PresentReadySemaphores.resize(imageCount, VK_NULL_HANDLE);
    {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < imageCount; ++i)
        {
            VkResult r = CreateSemaphoreTracked(sci, m_PresentReadySemaphores[i]);
            if (r != VK_SUCCESS)
            {
                Logger::Log::Error("Failed to create presentReady semaphore for image {}", (int)i);
                return false;
            }
        }
    }

    // Recreate render pass and framebuffers to match the real swapchain (replace any dummy ones)
    // Destroy existing framebuffers (dummy or old swapchain)
    for (VkFramebuffer fb : m_SwapchainFramebuffers)
    {
        if (fb != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(m_Device, fb, nullptr);
        }
    }
    m_SwapchainFramebuffers.clear();

    // Destroy any offscreen dummy resources
    if (m_OffscreenImageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_Device, m_OffscreenImageView, nullptr);
        m_OffscreenImageView = VK_NULL_HANDLE;
    }
    if (m_OffscreenImage != VK_NULL_HANDLE)
    {
        if (m_OffscreenImageMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_Device, m_OffscreenImageMemory, nullptr);
            m_OffscreenImageMemory = VK_NULL_HANDLE;
        }
        vkDestroyImage(m_Device, m_OffscreenImage, nullptr);
        m_OffscreenImage = VK_NULL_HANDLE;
    }

    // If an old render pass exists (possibly with different format), destroy it
    if (m_SwapchainRenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_Device, m_SwapchainRenderPass, nullptr);
        m_SwapchainRenderPass = VK_NULL_HANDLE;
    }

    // Create a render pass compatible with the actual swapchain format and framebuffers for each image
    GetOrCreateSwapchainRenderPass();
    return true;
}

bool VulkanDevice::CheckSurfaceSupport()
{
    VkBool32 presentSupport = false;
    vkGetPhysicalDeviceSurfaceSupportKHR(m_PhysicalDevice, m_PresentQueueFamily, m_Surface, &presentSupport);
    return presentSupport;
}

VkSurfaceFormatKHR VulkanDevice::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats)
{
    auto matches = [&](VkFormat fmt, VkColorSpaceKHR cs)
    {
        for (const auto& f : availableFormats)
        {
            if (f.format == fmt && f.colorSpace == cs)
                return f;
        }
        return VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    };

    auto pickTenBit = [&](VkColorSpaceKHR cs)
    {
        VkSurfaceFormatKHR pick = matches(VK_FORMAT_A2B10G10R10_UNORM_PACK32, cs);
        if (pick.format != VK_FORMAT_UNDEFINED)
            return pick;
        return matches(VK_FORMAT_A2R10G10B10_UNORM_PACK32, cs);
    };
    auto pickFloat16 = [&](VkColorSpaceKHR cs)
    {
        return matches(VK_FORMAT_R16G16B16A16_SFLOAT, cs);
    };
    auto pickHdrDepth = [&](VkColorSpaceKHR cs)
    {
        if (m_HdrState.swapchainBitDepth == HdrSwapchainBitDepth::Float16)
        {
            VkSurfaceFormatKHR pick = pickFloat16(cs);
            if (pick.format != VK_FORMAT_UNDEFINED)
                return pick;
            return pickTenBit(cs);
        }
        VkSurfaceFormatKHR pick = pickTenBit(cs);
        if (pick.format != VK_FORMAT_UNDEFINED)
            return pick;
        return pickFloat16(cs);
    };
    auto pickScRgb = [&]()
    {
        return pickFloat16(VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
    };

    if (m_HdrState.enabled)
    {
        // Resolve the requested mode against ACTUAL capability. macOS/MoltenVK advertises the
        // ST2084 (PQ) and HLG colorspaces for every surface format but does not honor absolute-nit
        // output (it reads washed-out), so supportsHDR10_PQ/supportsHLG are left false there. Any
        // unsupported HDR request falls back to scRGB — the working EDR path — with a log, rather
        // than building a washed-out PQ swapchain or silently degrading to SDR. The chosen
        // colorspace then drives activeMode below, so the encode follows automatically.
        HdrOutputMode effectiveMode = m_HdrState.requestedMode;
        if ((effectiveMode == HdrOutputMode::HDR10_PQ || effectiveMode == HdrOutputMode::HDR10Plus) &&
            !m_HdrState.display.supportsHDR10_PQ)
        {
            Logger::Log::Warning("VulkanDevice: HDR10 PQ/HDR10+ not supported on this display/driver; using scRGB (EDR) instead.");
            effectiveMode = HdrOutputMode::ScRGB;
        }
        else if (effectiveMode == HdrOutputMode::HLG && !m_HdrState.display.supportsHLG)
        {
            Logger::Log::Warning("VulkanDevice: HLG not supported on this display/driver; using scRGB (EDR) instead.");
            effectiveMode = HdrOutputMode::ScRGB;
        }
        else if (effectiveMode == HdrOutputMode::Auto && m_IsPortabilitySubsetDevice)
        {
            // On a portability driver (MoltenVK) the only real, honored HDR path is scRGB/EDR; steer
            // Auto there regardless of requested bit depth, so it never selects a spuriously-
            // enumerated PQ/HLG format. Conformant WSI keeps Auto and may resolve to PQ below.
            effectiveMode = HdrOutputMode::ScRGB;
        }

        const bool preferScRgbFirst =
            effectiveMode == HdrOutputMode::ScRGB ||
            (effectiveMode == HdrOutputMode::Auto &&
             m_HdrState.swapchainBitDepth == HdrSwapchainBitDepth::Float16);
        if (preferScRgbFirst)
        {
            VkSurfaceFormatKHR pick = pickScRgb();
            if (pick.format != VK_FORMAT_UNDEFINED)
                return pick;
        }
        if (effectiveMode == HdrOutputMode::HDR10_PQ ||
            effectiveMode == HdrOutputMode::HDR10Plus ||
            effectiveMode == HdrOutputMode::Auto)
        {
            VkSurfaceFormatKHR pick = pickHdrDepth(VK_COLOR_SPACE_HDR10_ST2084_EXT);
            if (pick.format != VK_FORMAT_UNDEFINED)
                return pick;
        }
        if (effectiveMode == HdrOutputMode::HLG ||
            effectiveMode == HdrOutputMode::Auto)
        {
            VkSurfaceFormatKHR pick = pickHdrDepth(VK_COLOR_SPACE_HDR10_HLG_EXT);
            if (pick.format != VK_FORMAT_UNDEFINED)
                return pick;
        }
        if (effectiveMode == HdrOutputMode::ScRGB ||
            effectiveMode == HdrOutputMode::Auto)
        {
            VkSurfaceFormatKHR pick = pickScRgb();
            if (pick.format != VK_FORMAT_UNDEFINED)
                return pick;
        }
    }

    const VkColorSpaceKHR kCS = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkSurfaceFormatKHR pick{VK_FORMAT_UNDEFINED, kCS};
    if (m_PreferTenBitSwapchain)
    {
        pick = pickTenBit(kCS);
        if (pick.format != VK_FORMAT_UNDEFINED)
            return pick;
    }
    // 8-bit UNORM ahead of 8-bit _SRGB (#767 P6a) — OFF by default, so this is
    // a no-op on every host today. Turning it on makes SwapchainNeedsManualSRGBEncode()
    // true for every SDR frame, which is what gives a host the terminal encode
    // pass: deband, dither, and an always-available capture composite. Windows
    // never reaches here (the 10-bit preference above already wins); the host
    // this matters for is macOS, where m_PreferTenBitSwapchain is false and the
    // _SRGB fallbacks below are what gets picked today.
    //
    // Defaulting it ON for macOS is the user's call, and it needs a macOS
    // measurement first — the design's cost gate (#767 §5 measurement 6),
    // quoted: "Gate: > 0.25 ms GPU or any measurable editor frame-time
    // regression at the 95th percentile ⇒ P6a does not land as a default;
    // macOS keeps `_SRGB` swapchains and routes through the P6b Finalize arm
    // instead." Both routes are supported, so nothing is blocked on that
    // measurement — only which one macOS takes by default.
    if (m_PreferUnormSdrSwapchain)
    {
        pick = matches(VK_FORMAT_B8G8R8A8_UNORM, kCS);
        if (pick.format != VK_FORMAT_UNDEFINED)
            return pick;
        pick = matches(VK_FORMAT_R8G8B8A8_UNORM, kCS);
        if (pick.format != VK_FORMAT_UNDEFINED)
            return pick;
    }
    // 8-bit sRGB fallbacks
    pick = matches(VK_FORMAT_B8G8R8A8_SRGB, kCS);
    if (pick.format != VK_FORMAT_UNDEFINED)
        return pick;
    pick = matches(VK_FORMAT_R8G8B8A8_SRGB, kCS);
    if (pick.format != VK_FORMAT_UNDEFINED)
        return pick;
    // If 10-bit not preferred but available, still consider as late fallback
    if (!m_PreferTenBitSwapchain)
    {
        pick = pickTenBit(kCS);
        if (pick.format != VK_FORMAT_UNDEFINED)
            return pick;
    }
    // As a last resort, return the first available
    return availableFormats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, kCS} : availableFormats[0];
}

VkPresentModeKHR VulkanDevice::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes)
{
    // Optional override via environment for profiling (IMMEDIATE|MAILBOX|FIFO)
    if (const char* pm = std::getenv("GE_PREFERRED_PRESENT_MODE"))
    {
        auto match = [&](VkPresentModeKHR want)
        {
            for (auto m : availablePresentModes)
                if (m == want)
                    return true;
            return false;
        };
        if (StrCaseCmp(pm, "IMMEDIATE") == 0 && match(VK_PRESENT_MODE_IMMEDIATE_KHR))
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        if (StrCaseCmp(pm, "MAILBOX") == 0 && match(VK_PRESENT_MODE_MAILBOX_KHR))
            return VK_PRESENT_MODE_MAILBOX_KHR;
        if (StrCaseCmp(pm, "FIFO") == 0 && match(VK_PRESENT_MODE_FIFO_KHR))
            return VK_PRESENT_MODE_FIFO_KHR;
    }
    // VSync: use FIFO (hard vsync, always available per spec)
    if (m_Vsync)
        return VK_PRESENT_MODE_FIFO_KHR;

    // Default: prefer mailbox (low latency, no tearing), then immediate (uncapped;
    // the only non-vsync mode MoltenVK exposes), then FIFO as guaranteed fallback.
    for (const auto& availablePresentMode : availablePresentModes)
    {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            return availablePresentMode;
        }
    }
    for (const auto& availablePresentMode : availablePresentModes)
    {
        if (availablePresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
        {
            return availablePresentMode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR; // Always available
}

VkExtent2D VulkanDevice::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height)
{
    if (capabilities.currentExtent.width != UINT32_MAX)
    {
        return capabilities.currentExtent;
    }
    else
    {
        VkExtent2D actualExtent = {width, height};
        actualExtent.width = std::max(capabilities.minImageExtent.width,
                                      std::min(capabilities.maxImageExtent.width, actualExtent.width));
        actualExtent.height = std::max(capabilities.minImageExtent.height,
                                       std::min(capabilities.maxImageExtent.height, actualExtent.height));
        return actualExtent;
    }
}

// Pipeline and buffer access methods
VkPipelineLayout VulkanDevice::GetVkPipelineLayout(PipelineHandle handle)
{
    // First try to get the specific pipeline
    if (const VulkanPipeline* pipeline = GetVulkanPipelineConst(handle))
    {
        if (pipeline->device != VK_NULL_HANDLE && pipeline->device != m_Device)
            return VK_NULL_HANDLE;
        if (pipeline->layout != VK_NULL_HANDLE)
        {
            return pipeline->layout;
        }
    }
    return VK_NULL_HANDLE;
}

VkPipeline VulkanDevice::GetVkPipeline(PipelineHandle handle)
{
    // First try to get the specific pipeline
    if (const VulkanPipeline* pipeline = GetVulkanPipelineConst(handle))
    {
        if (pipeline->device != VK_NULL_HANDLE && pipeline->device != m_Device)
            return VK_NULL_HANDLE;
        if (pipeline->pipeline != VK_NULL_HANDLE)
        {
            return pipeline->pipeline;
        }
    }

    // No fallback. Caller must supply a valid pipeline.
    return VK_NULL_HANDLE;
}

VkBuffer VulkanDevice::GetVkBuffer(BufferHandle handle)
{
    if (const VulkanBuffer* buffer = GetVulkanBufferConst(handle))
    {
        return buffer->buffer;
    }
    return VK_NULL_HANDLE;
}

VkImage VulkanDevice::GetVkImage(TextureHandle handle)
{
    // Check regular textures first
    if (const VulkanTexture* texture = GetVulkanTexture(handle))
    {
        return texture->image;
    }

    // Check swapchain textures (registered with target-scoped synthetic handles).
    // Note: backing map can be cleared
    // during swapchain recreation. To keep rendering robust during resize/bring-up transitions, lazily
    // re-register swapchain handles when queried.
    auto ensureSwapchainHandle = [&](TextureHandle h) -> VulkanTexture*
    {
        auto it = m_SwapchainTextures.find(h);
        if (it != m_SwapchainTextures.end())
            return &it->second;

        uint64_t handleTargetId = 0;
        uint32_t imageIndex = 0;
        if (!DecodeSwapchainTextureHandle(h, handleTargetId, imageIndex))
            return nullptr;
        const uint64_t activeTargetId = (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0) ? m_ActiveWindowTargetId : 1ull;
        if (handleTargetId != activeTargetId)
        {
            // Fail-fast instead of mapping to a wrong swapchain image from another window target.
            static int s_CrossTargetHandleWarnBudget = 16;
            if (s_CrossTargetHandleWarnBudget-- > 0)
            {
                Logger::Log::Warning(
                    "VulkanDevice: cross-target swapchain handle resolve blocked (handleTarget={}, activeTarget={}, imageIndex={})",
                    handleTargetId,
                    activeTargetId,
                    imageIndex);
            }
            return nullptr;
        }
        if (m_Swapchain == VK_NULL_HANDLE || imageIndex >= m_SwapchainImages.size() || imageIndex >= m_SwapchainImageViews.size())
            return nullptr;

        VulkanTexture swapchainTexture{};
        swapchainTexture.device = m_Device;
        swapchainTexture.image = m_SwapchainImages[imageIndex];
        swapchainTexture.view = m_SwapchainImageViews[imageIndex];
        swapchainTexture.memory = VK_NULL_HANDLE;
        swapchainTexture.allocation = nullptr;
        swapchainTexture.format = m_SwapchainImageFormat;
        swapchainTexture.extent = {m_SwapchainExtent.width, m_SwapchainExtent.height, 1};
        swapchainTexture.usage = TextureUsage::RenderTarget;
        swapchainTexture.debugName = "swapchain_t" + std::to_string(handleTargetId) + "_image_" + std::to_string(imageIndex);
        auto [insIt, ok] = m_SwapchainTextures.emplace(h, std::move(swapchainTexture));
        return ok ? &insIt->second : nullptr;
    };

    if (VulkanTexture* sw = ensureSwapchainHandle(handle))
        return sw->image;

    return VK_NULL_HANDLE;
}

VkFormat VulkanDevice::GetVkImageFormat(TextureHandle handle)
{
    if (const VulkanTexture* texture = GetVulkanTexture(handle))
    {
        return texture->format;
    }
    // Also check swapchain textures; lazily re-register on demand (see GetVkImage()).
    auto it = m_SwapchainTextures.find(handle);
    if (it != m_SwapchainTextures.end())
        return it->second.format;
    // Trigger lazy registration by calling GetVkImage (which will populate m_SwapchainTextures if possible).
    (void)GetVkImage(handle);
    it = m_SwapchainTextures.find(handle);
    if (it != m_SwapchainTextures.end())
        return it->second.format;
    return VK_FORMAT_UNDEFINED;
}

// Map VkFormat to engine TextureFormat (partial, extend as needed)
static TextureFormat FromVkFormat(VkFormat f)
{
    switch (f)
    {
    // 8-bit UNORM/SRGB
    case VK_FORMAT_R8_UNORM:
        return TextureFormat::R8_UNORM;
    case VK_FORMAT_R8G8_UNORM:
        return TextureFormat::R8G8_UNORM;
    case VK_FORMAT_R8_UINT:
        return TextureFormat::R8_UINT;
    case VK_FORMAT_R8_SINT:
        return TextureFormat::R8_SINT;
    case VK_FORMAT_R8G8_UINT:
        return TextureFormat::R8G8_UINT;
    case VK_FORMAT_R8G8_SINT:
        return TextureFormat::R8G8_SINT;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return TextureFormat::RGBA8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return TextureFormat::RGBA8_SRGB;
    case VK_FORMAT_R8G8B8A8_UINT:
        return TextureFormat::RGBA8_UINT;
    case VK_FORMAT_R8G8B8A8_SINT:
        return TextureFormat::RGBA8_SINT;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return TextureFormat::BGRA8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return TextureFormat::BGRA8_SRGB;
    // 16-bit float/integer
    case VK_FORMAT_R16_SFLOAT:
        return TextureFormat::R16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT:
        return TextureFormat::R16G16_FLOAT;
    case VK_FORMAT_R16_UINT:
        return TextureFormat::R16_UINT;
    case VK_FORMAT_R16_SINT:
        return TextureFormat::R16_SINT;
    case VK_FORMAT_R16G16_UINT:
        return TextureFormat::R16G16_UINT;
    case VK_FORMAT_R16G16_SINT:
        return TextureFormat::R16G16_SINT;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return TextureFormat::R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16B16A16_UNORM:
        return TextureFormat::R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16A16_UINT:
        return TextureFormat::RGBA16_UINT;
    case VK_FORMAT_R16G16B16A16_SINT:
        return TextureFormat::RGBA16_SINT;
    // 32-bit float/integer
    case VK_FORMAT_R32_SFLOAT:
        return TextureFormat::R32_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT:
        return TextureFormat::R32G32_FLOAT;
    case VK_FORMAT_R32_UINT:
        return TextureFormat::R32_UINT;
    case VK_FORMAT_R32_SINT:
        return TextureFormat::R32_SINT;
    case VK_FORMAT_R32G32_UINT:
        return TextureFormat::R32G32_UINT;
    case VK_FORMAT_R32G32_SINT:
        return TextureFormat::R32G32_SINT;
    case VK_FORMAT_R32G32B32_UINT:
        return TextureFormat::R32G32B32_UINT;
    case VK_FORMAT_R32G32B32_SINT:
        return TextureFormat::R32G32B32_SINT;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return TextureFormat::R32G32B32A32_FLOAT;
    case VK_FORMAT_R32G32B32A32_UINT:
        return TextureFormat::RGBA32_UINT;
    case VK_FORMAT_R32G32B32A32_SINT:
        return TextureFormat::RGBA32_SINT;
    // Packed floats
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        return TextureFormat::R11G11B10_FLOAT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return TextureFormat::RGB10A2_UNORM;
    // Depth/stencil
    case VK_FORMAT_D32_SFLOAT:
        return TextureFormat::D32_FLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return TextureFormat::D24_UNORM_S8_UINT;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return TextureFormat::D32_SFLOAT_S8_UINT;
    case VK_FORMAT_D16_UNORM:
        return TextureFormat::D16_UNORM;
    case VK_FORMAT_S8_UINT:
        return TextureFormat::S8_UINT;
    case VK_FORMAT_X8_D24_UNORM_PACK32:
        return TextureFormat::X8_D24_UNORM_PACK32;
    // Block compressed
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        return TextureFormat::BC1_UNORM;
    case VK_FORMAT_BC3_UNORM_BLOCK:
        return TextureFormat::BC3_UNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK:
        return TextureFormat::BC5_UNORM;
    case VK_FORMAT_BC7_UNORM_BLOCK:
        return TextureFormat::BC7_UNORM;
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return TextureFormat::BC7_SRGB;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        return TextureFormat::BC1_SRGB;
    case VK_FORMAT_BC4_UNORM_BLOCK:
        return TextureFormat::BC4_UNORM;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        return TextureFormat::BC6H_UF16;
    default:
        return TextureFormat::Unknown;
    }
}

TextureFormat VulkanDevice::GetTextureFormat(TextureHandle texture) const
{
    VkFormat vkf = VK_FORMAT_UNDEFINED;
    if (const VulkanTexture* t = GetVulkanTexture(texture))
    {
        vkf = t->format;
    }
    else
    {
        auto it = m_SwapchainTextures.find(texture);
        if (it != m_SwapchainTextures.end())
        {
            vkf = it->second.format;
        }
    }
    return FromVkFormat(vkf);
}

uint32_t VulkanDevice::GetTextureSampleCount(TextureHandle texture) const
{
    if (const VulkanTexture* t = GetVulkanTexture(texture))
        return t->sampleCount > 0u ? t->sampleCount : 1u;
    // Swapchain images are always single-sampled.
    return 1u;
}

uint32_t VulkanDevice::GetTextureArrayLayers(TextureHandle texture) const
{
    if (const VulkanTexture* t = GetVulkanTexture(texture))
        return t->arrayLayers > 0u ? t->arrayLayers : 1u;
    // Swapchain images are single-layer.
    return 1u;
}

bool VulkanDevice::GetTextureSampledInGeneralLayout(TextureHandle texture) const
{
    if (const VulkanTexture* t = GetVulkanTexture(texture))
        return t->sampledInGeneralLayout;
    return false;
}

void VulkanDevice::GetTextureSize(TextureHandle texture, uint32_t& outWidth,
                                  uint32_t& outHeight) const
{
    outWidth = 0;
    outHeight = 0;
    if (const VulkanTexture* t = GetVulkanTexture(texture))
    {
        outWidth = t->extent.width;
        outHeight = t->extent.height;
        return;
    }
    auto it = m_SwapchainTextures.find(texture);
    if (it != m_SwapchainTextures.end())
    {
        outWidth = m_SwapchainExtent.width;
        outHeight = m_SwapchainExtent.height;
    }
}

bool VulkanDevice::IsTextureAlive(TextureHandle texture) const
{
    // Queued-for-deferred-destroy counts as dead: the handle still resolves
    // until the timeline drain runs, but no new consumer may take it.
    if (GetVulkanTexture(texture) == nullptr)
        return false;
    return m_PendingTextureDestroyIds.find(texture.id) == m_PendingTextureDestroyIds.end();
}

bool VulkanDevice::IsPipelineAlive(PipelineHandle pipeline) const
{
    // Executes in the device's home module by virtue of the virtual call —
    // the module-local handle manager here is the one CreatePipeline used.
    return GetVulkanPipeline(pipeline) != nullptr;
}

TextureFormat VulkanDevice::GetSwapchainTextureFormat() const
{
    return FromVkFormat(m_SwapchainImageFormat);
}

TextureFormat VulkanDevice::GetWindowTargetSwapchainFormat(WindowTargetHandle target) const
{
    // The active target's format lives in the live member; every other target's
    // in its banked state (Capture/ApplyWindowTargetState). A handle the map
    // does not know — stale after DestroyWindowTarget, or fabricated — reports
    // Unknown rather than the active neighbour's format.
    if (!target.IsValid())
        return TextureFormat::Unknown;
    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId == target.id)
        return FromVkFormat(m_SwapchainImageFormat);
    auto it = m_WindowTargets.find(target.id);
    if (it != m_WindowTargets.end())
        return FromVkFormat(it->second.swapchainImageFormat);
    return TextureFormat::Unknown;
}

bool VulkanDevice::IsTextureFormatSupported(TextureFormat format, uint32_t usageFlags) const
{
    VkFormat vkFormat = VulkanResourceUtils::GetVulkanFormat(format);
    if (vkFormat == VK_FORMAT_UNDEFINED)
    {
        return false;
    }

    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, vkFormat, &props);

    VkFormatFeatureFlags required = 0;

    if (usageFlags & static_cast<uint32_t>(TextureUsage::RenderTarget))
    {
        required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }
    if (usageFlags & static_cast<uint32_t>(TextureUsage::DepthStencil))
    {
        required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (usageFlags & static_cast<uint32_t>(TextureUsage::ShaderResource))
    {
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    }
    if (usageFlags & static_cast<uint32_t>(TextureUsage::UnorderedAccess))
    {
        required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    }
    if (usageFlags & static_cast<uint32_t>(TextureUsage::TransferSrc))
    {
        required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    }
    if (usageFlags & static_cast<uint32_t>(TextureUsage::TransferDst))
    {
        required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }

    // If no specific usage was requested, treat the format as supported.
    if (required == 0)
    {
        return true;
    }

    return (props.optimalTilingFeatures & required) == required;
}

bool VulkanDevice::GetSwapchainSize(uint32_t& outWidth, uint32_t& outHeight) const
{
    if (m_Swapchain != VK_NULL_HANDLE)
    {
        outWidth = m_SwapchainExtent.width;
        outHeight = m_SwapchainExtent.height;
        return true;
    }
    outWidth = 0;
    outHeight = 0;
    return false;
}

// Simple 64-bit hash helpers for caches (file-scope)
static uint64_t Hash64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
static uint64_t CombineHash(uint64_t a, uint64_t b)
{
    return Hash64(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
}

VkRenderPass VulkanDevice::GetOrCreateRenderPass(
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
    uint32_t samples)
{
    const uint32_t requestedSamples = samples > 0 ? samples : 1u;
    uint32_t usageFlags = 0u;
    if (colorCount > 0)
        usageFlags |= static_cast<uint32_t>(TextureUsage::RenderTarget);
    if (hasDepth)
        usageFlags |= static_cast<uint32_t>(TextureUsage::DepthStencil);
    const uint32_t resolvedSamples = ResolveSupportedSampleCount(requestedSamples, usageFlags);
    LogSampleCountFallbackIfNeeded(requestedSamples, resolvedSamples, usageFlags);

    // Build a compact hash key
    uint64_t key = colorCount;
    for (uint32_t i = 0; i < colorCount; ++i)
    {
        uint64_t f = static_cast<uint64_t>(colorFormats[i]);
        uint64_t l = static_cast<uint64_t>(colorLoadOps[i]);
        uint64_t s = static_cast<uint64_t>(colorStoreOps[i]);
        key = CombineHash(key, (f << 32) ^ (l << 8) ^ s);
    }
    key = CombineHash(key, hasDepth ? 1 : 0);
    if (hasDepth)
    {
        key = CombineHash(key, static_cast<uint64_t>(depthFormat));
        key = CombineHash(key, (static_cast<uint64_t>(depthLoadOp) << 8) ^ static_cast<uint64_t>(depthStoreOp));
        key = CombineHash(key, (static_cast<uint64_t>(stencilLoadOp) << 8) ^ static_cast<uint64_t>(stencilStoreOp));
    }
    key = CombineHash(key, resolvedSamples);

    auto it = m_RenderPassCache.find(key);
    if (it != m_RenderPassCache.end())
        return it->second;

    std::vector<VkAttachmentDescription> attachments;
    attachments.reserve(colorCount + (hasDepth ? 1u : 0u));

    for (uint32_t i = 0; i < colorCount; ++i)
    {
        VkAttachmentDescription ad{};
        ad.format = colorFormats[i];
        ad.samples = static_cast<VkSampleCountFlagBits>(resolvedSamples);
        ad.loadOp = colorLoadOps[i];
        ad.storeOp = colorStoreOps[i];
        ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Spec: initialLayout must not be UNDEFINED when loadOp == LOAD
        ad.initialLayout = (colorLoadOps[i] == VK_ATTACHMENT_LOAD_OP_LOAD)
                               ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                               : VK_IMAGE_LAYOUT_UNDEFINED;
        ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments.push_back(ad);
    }

    uint32_t depthIndex = VK_ATTACHMENT_UNUSED;
    if (hasDepth)
    {
        depthIndex = static_cast<uint32_t>(attachments.size());
        VkAttachmentDescription ad{};
        ad.format = depthFormat;
        ad.samples = static_cast<VkSampleCountFlagBits>(resolvedSamples);
        ad.loadOp = depthLoadOp;
        ad.storeOp = depthStoreOp;
        ad.stencilLoadOp = stencilLoadOp;
        ad.stencilStoreOp = stencilStoreOp;
        // Spec: initialLayout must not be UNDEFINED when loadOp == LOAD
        ad.initialLayout = (depthLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
                               ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                               : VK_IMAGE_LAYOUT_UNDEFINED;
        ad.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(ad);
    }

    std::vector<VkAttachmentReference> colorRefs(colorCount);
    for (uint32_t i = 0; i < colorCount; ++i)
    {
        colorRefs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkAttachmentReference depthRef{depthIndex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = colorCount;
    subpass.pColorAttachments = colorRefs.data();
    subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

    // Add a default subpass dependency to match pipeline compatibility expectations
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkSubpassDependency depOut{};
    depOut.srcSubpass = 0;
    depOut.dstSubpass = VK_SUBPASS_EXTERNAL;
    depOut.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    depOut.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    depOut.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    depOut.dstAccessMask = 0;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpci.pAttachments = attachments.data();
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    if (colorFinalPresent && colorCount == 1 && !hasDepth)
    {
        VkSubpassDependency deps[2] = {dep, depOut};
        rpci.dependencyCount = 2;
        rpci.pDependencies = deps;
    }

    VkRenderPass renderPass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(m_Device, &rpci, nullptr, &renderPass) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    m_RenderPassCache[key] = renderPass;
    return renderPass;
}

VkFramebuffer VulkanDevice::GetOrCreateFramebuffer(
    VkRenderPass renderPass,
    uint32_t attachmentCount,
    const VkImageView* attachmentViews,
    uint32_t width,
    uint32_t height)
{

    // Simple hash combining renderPass pointer and views + dims
    uint64_t key = reinterpret_cast<uint64_t>(renderPass);
    key = CombineHash(key, width);
    key = CombineHash(key, height);
    for (uint32_t i = 0; i < attachmentCount; ++i)
    {
        key = CombineHash(key, reinterpret_cast<uint64_t>(attachmentViews[i]));
    }

    auto it = m_FramebufferCache.find(key);
    if (it != m_FramebufferCache.end())
        return it->second;

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = renderPass;
    fbci.attachmentCount = attachmentCount;
    fbci.pAttachments = attachmentViews;
    fbci.width = width;
    fbci.height = height;
    fbci.layers = 1;

    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(m_Device, &fbci, nullptr, &fb) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    m_FramebufferCache[key] = fb;
    return fb;
}

bool VulkanDevice::IsSwapchainTextureHandle(TextureHandle handle) const
{
    uint64_t handleTargetId = 0;
    uint32_t imageIndex = 0;
    if (!DecodeSwapchainTextureHandle(handle, handleTargetId, imageIndex))
    {
        return false;
    }
    const uint64_t activeTargetId = (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0) ? m_ActiveWindowTargetId : 1ull;
    if (handleTargetId != activeTargetId)
    {
        return false;
    }
    return imageIndex < m_SwapchainImages.size();
}

VkImageLayout VulkanDevice::GetTrackedSwapchainImageLayout(TextureHandle handle) const
{
    // Only meaningful for swapchain handles owned by the active window target.
    if (!IsSwapchainTextureHandle(handle))
    {
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }
    uint64_t handleTargetId = 0;
    uint32_t imageIndex = 0;
    if (!DecodeSwapchainTextureHandle(handle, handleTargetId, imageIndex))
    {
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (imageIndex >= m_SwapchainImageLayouts.size())
    {
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return m_SwapchainImageLayouts[static_cast<size_t>(imageIndex)];
}

VkImageLayout VulkanDevice::GetTrackedImageLayout(TextureHandle handle) const
{
    if (!handle.IsValid())
        return VK_IMAGE_LAYOUT_UNDEFINED;
    if (IsSwapchainTextureHandle(handle))
        return GetTrackedSwapchainImageLayout(handle);
    const VulkanTexture* tex = GetVulkanTextureConst(handle);
    return tex ? tex->trackedLayout : VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanDevice::SetTrackedSwapchainImageLayout(TextureHandle handle, VkImageLayout layout)
{
    if (!IsSwapchainTextureHandle(handle))
    {
        return;
    }
    uint64_t handleTargetId = 0;
    uint32_t imageIndex = 0;
    if (!DecodeSwapchainTextureHandle(handle, handleTargetId, imageIndex))
    {
        return;
    }
    if (imageIndex >= m_SwapchainImageLayouts.size())
    {
        return;
    }
    m_SwapchainImageLayouts[static_cast<size_t>(imageIndex)] = layout;
}

VkImageView VulkanDevice::GetVkImageView(TextureHandle handle)
{
    if (const VulkanTexture* texture = GetVulkanTexture(handle))
    {
        return texture->view;
    }
    auto it = m_SwapchainTextures.find(handle);
    if (it != m_SwapchainTextures.end())
    {
        return it->second.view;
    }
    // Lazily re-register swapchain handles if needed (see GetVkImage()).
    (void)GetVkImage(handle);
    it = m_SwapchainTextures.find(handle);
    if (it != m_SwapchainTextures.end())
        return it->second.view;
    return VK_NULL_HANDLE;
}

VkExtent3D VulkanDevice::GetVkImageExtent(TextureHandle handle)
{
    if (const VulkanTexture* texture = GetVulkanTexture(handle))
    {
        return texture->extent;
    }
    auto it = m_SwapchainTextures.find(handle);
    if (it != m_SwapchainTextures.end())
    {
        return it->second.extent;
    }
    // Lazily re-register swapchain handles if needed (see GetVkImage()).
    (void)GetVkImage(handle);
    it = m_SwapchainTextures.find(handle);
    if (it != m_SwapchainTextures.end())
        return it->second.extent;
    return VkExtent3D{0, 0, 1};
}

VkSampleCountFlagBits VulkanDevice::GetVkImageSamples(TextureHandle handle)
{
    if (const VulkanTexture* texture = GetVulkanTexture(handle))
    {
        return static_cast<VkSampleCountFlagBits>(texture->sampleCount > 0 ? texture->sampleCount : 1);
    }
    auto it = m_SwapchainTextures.find(handle);
    if (it != m_SwapchainTextures.end())
    {
        return VK_SAMPLE_COUNT_1_BIT; // swapchain images are single-sample
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

uint32_t VulkanDevice::GetPipelinePushConstantRangeCount(PipelineHandle pipeline) const
{
    if (const VulkanPipeline* p = GetVulkanPipelineConst(pipeline))
    {
        return static_cast<uint32_t>(p->pushRanges.size());
    }
    return 0;
}

bool VulkanDevice::GetPipelinePushConstantRangeInfo(PipelineHandle pipeline, uint32_t id, PushConstantRangeInfo& outInfo) const
{
    if (const VulkanPipeline* p = GetVulkanPipelineConst(pipeline))
    {
        if (id < p->pushRanges.size())
        {
            const auto& r = p->pushRanges[id];
            outInfo.id = id;
            outInfo.name = r.name.c_str();
            outInfo.offset = r.offset;
            outInfo.size = r.size;
            outInfo.stagesMask = r.stagesMask
                                     ? r.stagesMask
                                     : static_cast<uint32_t>(VK_SHADER_STAGE_ALL);
            return true;
        }
    }
    outInfo = {};
    return false;
}

bool VulkanDevice::FindPipelinePushConstantRangeId(PipelineHandle pipeline, const char* name, uint32_t& outId) const
{
    if (!name || !*name)
        return false;
    if (const VulkanPipeline* p = GetVulkanPipelineConst(pipeline))
    {
        for (uint32_t i = 0; i < p->pushRanges.size(); ++i)
        {
            if (p->pushRanges[i].name == name)
            {
                outId = i;
                return true;
            }
        }
    }
    return false;
}

VkPipelineLayout VulkanDevice::GetPipelineLayout(PipelineHandle handle) const
{
    if (const VulkanPipeline* pipeline = GetVulkanPipelineConst(handle))
    {
        if (pipeline->device != VK_NULL_HANDLE && pipeline->device != m_Device)
            return VK_NULL_HANDLE;
        return pipeline->layout;
    }
    return VK_NULL_HANDLE;
}

bool VulkanDevice::GetPipelinePushConstantInfo(PipelineHandle handle, PipelinePushConstantInfo& outInfo) const
{
    if (const VulkanPipeline* p = GetVulkanPipelineConst(handle))
    {
        if (p->device != VK_NULL_HANDLE && p->device != m_Device)
        {
            outInfo = {};
            return false;
        }
        outInfo.size = p->pushConstantSize;
        outInfo.stagesMask = p->pushConstantStagesMask;
        return true;
    }
    outInfo = {};
    return false;
}

VkDescriptorSetLayout VulkanDevice::GetFirstDescriptorSetLayoutForPipeline(PipelineHandle handle) const
{
    if (const VulkanPipeline* pipeline = GetVulkanPipelineConst(handle))
    {
        if (pipeline->device != VK_NULL_HANDLE && pipeline->device != m_Device)
            return VK_NULL_HANDLE;
        return pipeline->descriptorSetLayout;
    }
    return VK_NULL_HANDLE;
}

PipelineType VulkanDevice::GetPipelineType(PipelineHandle handle) const
{
    if (const VulkanPipeline* pipeline = GetVulkanPipelineConst(handle))
    {
        if (pipeline->device != VK_NULL_HANDLE && pipeline->device != m_Device)
        {
            assert(false && "GetPipelineType: cross-device pipeline handle used");
            return PipelineType::Graphics;
        }
        return pipeline->type;
    }
    // T17: Do not silently default. This indicates a misuse (stale/invalid handle or missing registration)
    assert(false && "GetPipelineType: pipeline handle not found");
    return PipelineType::Graphics; // fallback to keep release builds from crashing
}

bool VulkanDevice::AcquireNextImage(uint32_t& imageIndex)
{
    if (m_Swapchain == VK_NULL_HANDLE)
    {
        return false;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();

    // Use Vulkan swapchain image acquisition with a small timeout to keep UI responsive during window moves/resizes
    const uint64_t kAcquireTimeoutNanoseconds = 16ull * 1000ull * 1000ull; // ~16 ms
    VkResult result = vkAcquireNextImageKHR(
        m_Device,
        m_Swapchain,
        kAcquireTimeoutNanoseconds,
        m_ImageAvailableSemaphores[m_CurrentFrame], // Signal when image is available
        VK_NULL_HANDLE,                             // No fence
        &imageIndex);

    const auto t1 = std::chrono::high_resolution_clock::now();
    m_LastFrameSyncTimings.acquireMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Mark that we must wait on the imageAvailable semaphore once this frame
    m_WaitedOnImageAvailableThisFrame = false;

    if (result == VK_SUCCESS)
    {
        m_CurrentSwapchainImage = imageIndex;
        m_AcquireSemaphoreThisFrame = (m_CurrentFrame < m_ImageAvailableSemaphores.size()) ? m_ImageAvailableSemaphores[m_CurrentFrame] : VK_NULL_HANDLE;

        // Preserve the tracked swapchain image layout across acquire. After the
        // first present, Present() leaves rendered images in PRESENT_SRC_KHR, so
        // the next render pass must transition from that tracked PRESENT layout
        // back to COLOR_ATTACHMENT_OPTIMAL. (Freshly created images start tracked
        // as UNDEFINED — see CreateSwapchain, which no longer pre-transitions to
        // PRESENT_SRC — and the barrier resolver keeps UNDEFINED until the first
        // present.) Resetting this to UNDEFINED here would record the wrong
        // oldLayout once an image has been presented.

        return true;
    }
    else if (result == VK_TIMEOUT)
    {
        // Non-fatal: skip this frame to keep the UI responsive (common during window moves/resizes)
        m_LastFrameSyncTimings.acquireTimedOut = true;
        return false;
    }
    else if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        if (m_SwapchainExtent.width > 0 && m_SwapchainExtent.height > 0)
        {
            const WindowTargetHandle activeTarget = GetActiveWindowTarget();
            if (activeTarget.IsValid())
            {
                RecreateWindowTargetSwapchain(activeTarget, m_SwapchainExtent.width, m_SwapchainExtent.height);
            }
        }
        // Skip this frame; the loop will continue and try acquire again next frame
        m_AcquiredThisFrame = false;
        m_AcquireSemaphoreThisFrame = VK_NULL_HANDLE;
        return false;
    }
    else if (result == VK_SUBOPTIMAL_KHR)
    {
        // Suboptimal means the swapchain can still be used but should be recreated.
        // We must NOT use the old imageIndex after recreation because it's from the
        // old swapchain. Skip this frame and let the next frame acquire from the new swapchain.
        if (m_SwapchainExtent.width > 0 && m_SwapchainExtent.height > 0)
        {
            const WindowTargetHandle activeTarget = GetActiveWindowTarget();
            if (activeTarget.IsValid())
            {
                RecreateWindowTargetSwapchain(activeTarget, m_SwapchainExtent.width, m_SwapchainExtent.height);
            }
        }
        m_AcquiredThisFrame = false;
        m_AcquireSemaphoreThisFrame = VK_NULL_HANDLE;
        return false;
    }
    else if (result == VK_ERROR_DEVICE_LOST)
    {
        // Previously fell into the generic else and was swallowed (Q6 fix).
        if (m_RecoveryEnabled)
        {
            OnDeviceLostObserved("swapchain acquire", result, /*deviceActuallyLost=*/true);
        }
        else
        {
            Logger::Log::Error("Failed to acquire swapchain image! Error: {}", (int)result);
        }
        return false;
    }
    else
    {
        Logger::Log::Error("Failed to acquire swapchain image! Error: {}", (int)result);
        return false;
    }
}

bool VulkanDevice::PresentImage(uint32_t imageIndex)
{
    if (m_Swapchain == VK_NULL_HANDLE)
    {
        Logger::Log::Error("Cannot present: no swapchain");
        return false;
    }

    if (m_PresentQueue == VK_NULL_HANDLE)
    {
        Logger::Log::Error("Cannot present: no present queue");
        return false;
    }

    if (imageIndex >= m_PresentReadySemaphores.size())
    {
        Logger::Log::Error("Cannot present: image {} has no present-ready semaphore ({} exist)",
                           imageIndex,
                           (unsigned)m_PresentReadySemaphores.size());
        return false;
    }

    // Present the image to the surface using Vulkan API with proper synchronization

    VkSemaphore waitSemaphores[] = {m_PresentReadySemaphores[imageIndex]};

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = waitSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_Swapchain;
    presentInfo.pImageIndices = &imageIndex;
    presentInfo.pResults = nullptr;

    // The present queue always resolves to the graphics family (see SelectPhysicalDevice), so
    // m_PresentQueue IS the graphics VkQueue. vkQueuePresentKHR takes that externally-
    // synchronised handle exactly as vkQueueSubmit does, so presentation and the marker submit
    // that follows it both run under the graphics queue lock — and under one acquisition, so
    // no other thread's submit can interleave between the present and its completion marker.
    QueueSubmitContext& presentContext = SubmitContextFor(QueueType::Graphics);
    const auto t0 = std::chrono::high_resolution_clock::now();
    VkResult result = presentContext.WithQueueLocked([&] {
        const VkResult presentRes = vkQueuePresentKHR(m_PresentQueue, &presentInfo);
        if (presentRes != VK_SUCCESS)
            return presentRes;

        // Arm a per-target completion fence with an empty submit on the present
        // queue, ordered after this present operation. This gives us a target-
        // scoped completion token and avoids present-queue-wide idle in drains.
        if (m_ActiveWindowTargetPresentCompleteFence != VK_NULL_HANDLE && m_PresentQueue != VK_NULL_HANDLE)
        {
            bool canArmMarker = true;
            if (m_ActiveWindowTargetPresentFenceArmed)
            {
                canArmMarker = IsFenceSignaled(m_ActiveWindowTargetPresentCompleteFence);
            }
            if (canArmMarker)
            {
                vkResetFences(m_Device, 1, &m_ActiveWindowTargetPresentCompleteFence);
                VkSubmitInfo marker{};
                marker.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                const VkResult markerRes = vkQueueSubmit(m_PresentQueue, 1, &marker, m_ActiveWindowTargetPresentCompleteFence);
                if (markerRes == VK_SUCCESS)
                {
                    m_ActiveWindowTargetPresentFenceArmed = true;
                    if (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0)
                    {
                        auto it = m_WindowTargets.find(m_ActiveWindowTargetId);
                        if (it != m_WindowTargets.end())
                        {
                            it->second.presentFenceArmed = true;
                            it->second.presentCompleteFence = m_ActiveWindowTargetPresentCompleteFence;
                        }
                    }
                }
                else
                {
                    Logger::Log::Warning("Failed to submit present completion marker (res={})", (int)markerRes);
                }
            }
        }
        return presentRes;
    });
    const auto t1 = std::chrono::high_resolution_clock::now();
    m_LastFrameSyncTimings.presentMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (result == VK_SUCCESS)
    {

        // Advance global device frame pacing.
        AdvanceFrameIndex();
        // Advance logical frame progression for the active window target.
        m_ActiveWindowTargetFrameIndex = (m_ActiveWindowTargetFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
        if (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0)
        {
            auto it = m_WindowTargets.find(m_ActiveWindowTargetId);
            if (it != m_WindowTargets.end())
                it->second.frameIndex = m_ActiveWindowTargetFrameIndex;
        }
        return true;
    }
    else if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        // Keep global frame pacing moving even on resize/out-of-date presents.
        AdvanceFrameIndex();
        if (m_SwapchainExtent.width > 0 && m_SwapchainExtent.height > 0)
        {
            const WindowTargetHandle activeTarget = GetActiveWindowTarget();
            if (activeTarget.IsValid())
            {
                RecreateWindowTargetSwapchain(activeTarget, m_SwapchainExtent.width, m_SwapchainExtent.height);
            }
        }
        return false;
    }
    else
    {
        // Keep global frame pacing moving even on failed presents.
        AdvanceFrameIndex();
        if (result == VK_ERROR_DEVICE_LOST && m_RecoveryEnabled)
        {
            // Previously swallowed here (Q6 fix).
            OnDeviceLostObserved("present", result, /*deviceActuallyLost=*/true);
        }
        else
        {
            Logger::Log::Error("Failed to present swapchain image! Error: {}", (int)result);
        }
        return false;
    }
}

TextureHandle VulkanDevice::GetCurrentSwapchainImageHandle()
{
    if (m_Swapchain == VK_NULL_HANDLE || m_CurrentSwapchainImage >= m_SwapchainImages.size())
    {
        Logger::Log::Error("VulkanDevice: Invalid swapchain or image index for GetCurrentSwapchainImageHandle");
        return INVALID_HANDLE;
    }

    // Use a target-scoped synthetic handle to avoid cross-window collisions on shared-device.
    const uint64_t targetId = (m_HasActiveWindowTarget && m_ActiveWindowTargetId != 0) ? m_ActiveWindowTargetId : 1ull;
    TextureHandle swapchainHandle = MakeSwapchainTextureHandle(targetId, m_CurrentSwapchainImage);

    // Lazily register this swapchain image in the swapchain texture map so that helper
    // queries (GetVkImage, GetVkImageView, etc.) can resolve it by handle.
    auto it = m_SwapchainTextures.find(swapchainHandle);
    if (it == m_SwapchainTextures.end())
    {
        // Create a VulkanTexture entry for the swapchain image
        VulkanTexture swapchainTexture{};
        swapchainTexture.device = m_Device; // Track device for proper cleanup
        swapchainTexture.image = m_SwapchainImages[m_CurrentSwapchainImage];
        swapchainTexture.view = m_SwapchainImageViews[m_CurrentSwapchainImage];
        swapchainTexture.memory = VK_NULL_HANDLE; // Swapchain images don't have separate memory
        swapchainTexture.allocation = nullptr;    // No VMA allocation for swapchain images
        swapchainTexture.format = m_SwapchainImageFormat;
        swapchainTexture.extent = {m_SwapchainExtent.width, m_SwapchainExtent.height, 1};
        swapchainTexture.usage = TextureUsage::RenderTarget;
        swapchainTexture.debugName = "swapchain_t" + std::to_string(targetId) + "_image_" + std::to_string(m_CurrentSwapchainImage);

        // Store the swapchain texture in a dedicated map keyed by its synthetic handle.
        // This map is cleared whenever the swapchain is recreated or destroyed.
        m_SwapchainTextures.emplace(swapchainHandle, std::move(swapchainTexture));
    }

    return swapchainHandle;
}

VkRenderPass VulkanDevice::GetOrCreateSwapchainRenderPass()
{
    // Strict: only valid when a real swapchain exists
    if (m_SwapchainRenderPass != VK_NULL_HANDLE)
    {
        return m_SwapchainRenderPass;
    }
    if (m_Swapchain == VK_NULL_HANDLE)
    {
        Logger::Log::Error("GetOrCreateSwapchainRenderPass called without a valid swapchain");
        return VK_NULL_HANDLE;
    }

    // Create a simple render pass for swapchain rendering
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_SwapchainImageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // Keep it simple and correct: let the render pass perform the transition
    // from UNDEFINED to COLOR_ATTACHMENT_OPTIMAL. This avoids initialLayout mismatch
    // and is valid for swapchain images.
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    // Add subpass dependencies for proper synchronization
    VkSubpassDependency dependencies[2] = {};

    // Dependency for transition into the render pass
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    // Dependency for transition out of the render pass (for presentation)
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].dstAccessMask = 0;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 2;

    renderPassInfo.pDependencies = dependencies;

    VkResult result = vkCreateRenderPass(m_Device, &renderPassInfo, nullptr, &m_SwapchainRenderPass);
    if (result == VK_SUCCESS)
    {
        // Create framebuffers for all swapchain images
        CreateSwapchainFramebuffers();
    }
    else
    {
        Logger::Log::Error("Failed to create swapchain render pass! Error: {}", (int)result);
    }

    return m_SwapchainRenderPass;
}

VkFramebuffer VulkanDevice::GetCurrentSwapchainFramebuffer()
{
    // For dummy swapchain (offscreen rendering), return VK_NULL_HANDLE
    // This will cause BeginRenderPass to fail gracefully, which is expected for offscreen rendering
    if (m_SwapchainFramebuffers.empty())
    {
        return VK_NULL_HANDLE;
    }

    if (m_CurrentSwapchainImage < m_SwapchainFramebuffers.size())
    {
        return m_SwapchainFramebuffers[m_CurrentSwapchainImage];
    }
    return VK_NULL_HANDLE;
}

void VulkanDevice::CreateSwapchainFramebuffers()
{
    // Requires a real swapchain with valid image views
    if (m_SwapchainImageViews.empty())
    {
        Logger::Log::Error("CreateSwapchainFramebuffers called without swapchain image views");
        return;
    }

    // Destroy existing framebuffers
    for (VkFramebuffer fb : m_SwapchainFramebuffers)
    {
        if (fb != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(m_Device, fb, nullptr);
        }
    }
    m_SwapchainFramebuffers.clear();

    m_SwapchainFramebuffers.resize(m_SwapchainImageViews.size());

    for (size_t i = 0; i < m_SwapchainImageViews.size(); ++i)
    {
        VkImageView attachments[] = {m_SwapchainImageViews[i]};

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_SwapchainRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = m_SwapchainExtent.width;
        framebufferInfo.height = m_SwapchainExtent.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(m_Device, &framebufferInfo, nullptr, &m_SwapchainFramebuffers[i]) != VK_SUCCESS)
        {
            Logger::Log::Error("Failed to create framebuffer for swapchain image {}", (int)i);
            m_SwapchainFramebuffers[i] = VK_NULL_HANDLE;
        }
    }
}

uint32_t VulkanDevice::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
    {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }

    Logger::Log::Error("Failed to find suitable memory type");
    return 0;
}

VkShaderModule VulkanDevice::CreateShaderModule(const std::vector<char>& code)
{
    // Ensure 4-byte alignment of SPIR-V words per Vulkan spec
    const size_t byteCount = code.size();
    const size_t wordCount = (byteCount + 3) / 4;
    std::vector<uint32_t> words(wordCount, 0u);
    if (byteCount > 0)
    {
        std::memcpy(words.data(), code.data(), byteCount);
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = byteCount;
    createInfo.pCode = words.data();

    VkShaderModule shaderModule;
    VkResult result = vkCreateShaderModule(m_Device, &createInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

VkShaderModule VulkanDevice::CreateShaderModuleFromBytes(const std::vector<uint8_t>& code)
{
    // Ensure 4-byte alignment of SPIR-V words per Vulkan spec
    const size_t byteCount = code.size();
    const size_t wordCount = (byteCount + 3) / 4;
    std::vector<uint32_t> words(wordCount, 0u);
    if (byteCount > 0)
    {
        std::memcpy(words.data(), code.data(), byteCount);
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    // Use padded size to satisfy multiple-of-4 requirement and match pCode length
    createInfo.codeSize = static_cast<uint32_t>(wordCount * sizeof(uint32_t));
    createInfo.pCode = words.data();

    VkShaderModule shaderModule;
    VkResult result = vkCreateShaderModule(m_Device, &createInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS)
    {
        Logger::Log::Error("vkCreateShaderModule failed (FromBytes) codeSize={} VkResult={}",
                           (uint32_t)createInfo.codeSize,
                           (int)result);
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

VulkanDevice::PipelineCreationResult VulkanDevice::CreateVulkanPipelineFromDesc(const PipelineDesc& desc, VkShaderModule vertShader, VkShaderModule fragShader)
{
    auto failPipelineCreate = [this, &desc](std::string reason) -> PipelineCreationResult
    {
        const char* name = desc.debugName ? desc.debugName : "";
        SetLastGraphicsPipelineFailure(
            "Vulkan CreateVulkanPipelineFromDesc pipeline '" + std::string(name) + "': " + std::move(reason));
        return {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    };

    // Get specialization constants if provided
    const VkSpecializationInfo* specializationInfo = nullptr;
    std::vector<VkSpecializationMapEntry> specializationEntries;
    VkSpecializationInfo specializationInfoStorage{};
    if (desc.specializationConstants && !desc.specializationConstants->IsEmpty())
    {
        const auto& entries = desc.specializationConstants->GetEntries();
        specializationEntries.reserve(entries.size());
        for (const auto& kv : entries)
        {
            const auto& entry = kv.second;
            VkSpecializationMapEntry vkEntry{};
            vkEntry.constantID = entry.ConstantId;
            vkEntry.offset = entry.Offset;
            vkEntry.size = entry.Size;
            specializationEntries.push_back(vkEntry);
        }
        specializationInfoStorage.mapEntryCount = static_cast<uint32_t>(specializationEntries.size());
        specializationInfoStorage.pMapEntries = specializationEntries.data();
        specializationInfoStorage.dataSize = desc.specializationConstants->GetDataSize();
        specializationInfoStorage.pData = desc.specializationConstants->GetDataPtr();
        specializationInfo = &specializationInfoStorage;
    }

    // Create shader stages (allow depth-only pipelines without a fragment shader)
    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    uint32_t stageCount = 0;

    shaderStages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[stageCount].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[stageCount].module = vertShader;
    shaderStages[stageCount].pName = "main";
    shaderStages[stageCount].pSpecializationInfo = specializationInfo;
    ++stageCount;

    if (fragShader != VK_NULL_HANDLE)
    {
        shaderStages[stageCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[stageCount].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[stageCount].module = fragShader;
        shaderStages[stageCount].pName = "main";
        shaderStages[stageCount].pSpecializationInfo = specializationInfo;
        ++stageCount;
    }

    // Vertex input state - configure based on vertex bindings/attributes
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    std::vector<VkVertexInputBindingDescription> bindingDescriptions;
    std::vector<VkVertexInputAttributeDescription> attributeDescriptions;

    // Convert vertex bindings
    for (const auto& binding : desc.vertexBindings)
    {
        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding = binding.binding;
        bindingDesc.stride = binding.stride;
        bindingDesc.inputRate = static_cast<VkVertexInputRate>(binding.inputRate);
        bindingDescriptions.push_back(bindingDesc);
    }

    // Convert vertex attributes
    for (const auto& attribute : desc.vertexAttributes)
    {
        VkVertexInputAttributeDescription attributeDesc{};
        attributeDesc.binding = attribute.binding;
        attributeDesc.location = attribute.location;
        attributeDesc.format = VulkanResourceUtils::GetVulkanFormat(attribute.format);
        attributeDesc.offset = attribute.offset;
        attributeDescriptions.push_back(attributeDesc);
    }

    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescriptions.size());
    vertexInputInfo.pVertexBindingDescriptions = bindingDescriptions.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = static_cast<VkPrimitiveTopology>(desc.topology);
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport state (will be dynamic)
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = 1280.0f; // Default size
    viewport.height = 720.0f;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {1280, 720};

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    // Rasterizer - USE STRUCTURED STATE
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = desc.rasterizationState.depthClampEnable ? VK_TRUE : VK_FALSE;
    rasterizer.rasterizerDiscardEnable = desc.rasterizationState.rasterizerDiscardEnable ? VK_TRUE : VK_FALSE;
    rasterizer.polygonMode = VulkanMappings::TranslatePolygonMode(desc.rasterizationState.polygonMode);
    rasterizer.lineWidth = desc.rasterizationState.lineWidth;
    rasterizer.cullMode = VulkanMappings::TranslateCullMode(desc.rasterizationState.cullMode);
    rasterizer.frontFace = VulkanMappings::TranslateFrontFace(desc.rasterizationState.frontFace);
    rasterizer.depthBiasEnable = desc.rasterizationState.depthBiasEnable ? VK_TRUE : VK_FALSE;
    rasterizer.depthBiasConstantFactor = desc.rasterizationState.depthBiasConstantFactor;
    rasterizer.depthBiasClamp = desc.rasterizationState.depthBiasClamp;
    rasterizer.depthBiasSlopeFactor = desc.rasterizationState.depthBiasSlopeFactor;

    // Multisampling
    const uint32_t requestedRasterSamples = desc.rasterizationSamples ? desc.rasterizationSamples : 1u;
    const uint32_t resolvedRasterSamples = ResolveSupportedSampleCount(requestedRasterSamples, 0u);
    LogSampleCountFallbackIfNeeded(requestedRasterSamples, resolvedRasterSamples, 0u);
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = static_cast<VkSampleCountFlagBits>(resolvedRasterSamples);
    multisampling.alphaToCoverageEnable =
        desc.colorBlendState.alphaToCoverageEnable ? VK_TRUE : VK_FALSE;

    // Depth stencil - USE STRUCTURED STATE
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = desc.depthStencilState.depthTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthStencilState.depthWriteEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VulkanMappings::TranslateCompareOp(desc.depthStencilState.depthCompareOp);
    depthStencil.depthBoundsTestEnable = desc.depthStencilState.depthBoundsTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.stencilTestEnable = desc.depthStencilState.stencilTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.minDepthBounds = desc.depthStencilState.minDepthBounds;
    depthStencil.maxDepthBounds = desc.depthStencilState.maxDepthBounds;

    // Color blending - USE STRUCTURED STATE (fill full blend attachment when enabled)
    std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
    for (const auto& attachment : desc.colorBlendState.attachments)
    {
        VkPipelineColorBlendAttachmentState a{};
        a.colorWriteMask = attachment.colorWriteMask;
        a.blendEnable = attachment.blendEnable ? VK_TRUE : VK_FALSE;
        if (attachment.blendEnable)
        {
            a.srcColorBlendFactor = VulkanMappings::TranslateBlendFactor(attachment.srcColorBlendFactor);
            a.dstColorBlendFactor = VulkanMappings::TranslateBlendFactor(attachment.dstColorBlendFactor);
            a.colorBlendOp = VulkanMappings::TranslateBlendOp(attachment.colorBlendOp);
            a.srcAlphaBlendFactor = VulkanMappings::TranslateBlendFactor(attachment.srcAlphaBlendFactor);
            a.dstAlphaBlendFactor = VulkanMappings::TranslateBlendFactor(attachment.dstAlphaBlendFactor);
            a.alphaBlendOp = VulkanMappings::TranslateBlendOp(attachment.alphaBlendOp);
        }
        colorBlendAttachments.push_back(a);
    }

    // Default attachments if none specified: one per declared color format
    // For depth-only pipelines (no color attachments), do not add any color blend attachments
    if (colorBlendAttachments.empty())
    {
        const uint32_t colorCountHint = static_cast<uint32_t>(desc.colorAttachmentFormats.size());
        for (uint32_t i = 0; i < colorCountHint; ++i)
        {
            VkPipelineColorBlendAttachmentState s{};
            s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            s.blendEnable = VK_FALSE;
            colorBlendAttachments.push_back(s);
        }
    }
#ifndef NDEBUG

#endif

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = desc.colorBlendState.logicOpEnable ? VK_TRUE : VK_FALSE;
    colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
    colorBlending.pAttachments = colorBlendAttachments.data();
    colorBlending.blendConstants[0] = desc.colorBlendState.blendConstants[0];
    colorBlending.blendConstants[1] = desc.colorBlendState.blendConstants[1];
    colorBlending.blendConstants[2] = desc.colorBlendState.blendConstants[2];
    colorBlending.blendConstants[3] = desc.colorBlendState.blendConstants[3];
    auto syncColorBlendAttachmentCount = [&](uint32_t colorCount)
    {
        while (colorBlendAttachments.size() < colorCount)
        {
            VkPipelineColorBlendAttachmentState s{};
            s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            s.blendEnable = VK_FALSE;
            colorBlendAttachments.push_back(s);
        }
        if (colorBlendAttachments.size() > colorCount)
        {
            colorBlendAttachments.resize(colorCount);
        }
        colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
        colorBlending.pAttachments = colorBlendAttachments.empty() ? nullptr : colorBlendAttachments.data();
    };

    // Dynamic state
    std::vector<VkDynamicState> dynamicStates;
    for (const auto& state : desc.dynamicState.states)
    {
        dynamicStates.push_back(static_cast<VkDynamicState>(state));
    }

    // Always include viewport and scissor as dynamic
    if (std::find(dynamicStates.begin(), dynamicStates.end(), VK_DYNAMIC_STATE_VIEWPORT) == dynamicStates.end())
    {
        dynamicStates.push_back(VK_DYNAMIC_STATE_VIEWPORT);
    }
    if (std::find(dynamicStates.begin(), dynamicStates.end(), VK_DYNAMIC_STATE_SCISSOR) == dynamicStates.end())
    {
        dynamicStates.push_back(VK_DYNAMIC_STATE_SCISSOR);
    }
    if (m_DeviceProperties.apiVersion >= VK_API_VERSION_1_3 &&
        std::find(dynamicStates.begin(), dynamicStates.end(), VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE) == dynamicStates.end())
    {
        dynamicStates.push_back(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE);
    }

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.empty() ? nullptr : dynamicStates.data();

    // Create descriptor set layouts from desc.descriptorSetLayouts
    std::vector<VkDescriptorSetLayout> vkLayouts;

    for (const auto& layoutDesc : desc.descriptorSetLayouts)
    {
        uint64_t key = 0;
        VkDescriptorSetLayout vkLayout = GetOrCreateDescriptorSetLayoutCached(layoutDesc, key);
        if (vkLayout == VK_NULL_HANDLE)
        {
            Logger::Log::Error("Failed to create descriptor set layout from PipelineDesc (graphics)");
            return {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        }
        vkLayouts.push_back(vkLayout);
    }

    // Push-constant policy checks (fail fast on oversize requests), then the
    // canonical graphics range. The layout always carries exactly ONE range —
    // kGraphicsPushConstantStages, [0, m_MaxPushConstantBytes) — independent of
    // the reflected stages/size, so graphics pipeline layouts that agree on
    // descriptor set layouts dedup to the same VkPipelineLayout (see the
    // constant's comment in VulkanDevice.h for the compatibility rationale).
    // The declared/reflected sizes still gate policy: shaders that want more
    // than the policy budget fail creation here.
    if (!desc.pushConstantRanges.empty())
    {
        uint32_t runningOffset = 0;
        for (const auto& r : desc.pushConstantRanges)
        {
            runningOffset = (runningOffset + 3u) & ~3u; // align 4
            runningOffset += (r.size + 3u) & ~3u;
        }
        if (runningOffset > m_MaxPushConstantBytes)
        {
            Logger::Log::Error("PushConstantsEnforcement: total size exceeds policy (graphics)");
            return {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        }
    }
    else if (desc.pushConstantSize > m_MaxPushConstantBytes)
    {
        Logger::Log::Error("PushConstantsEnforcement: requested size exceeds policy (graphics)");
        return {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    }

    std::vector<VkPushConstantRange> pushRanges;
    const uint32_t canonicalPcSize = m_MaxPushConstantBytes & ~3u;
    if (canonicalPcSize)
    {
        VkPushConstantRange canonical{};
        canonical.stageFlags = kGraphicsPushConstantStages;
        canonical.offset = 0;
        canonical.size = canonicalPcSize;
        pushRanges.push_back(canonical);
    }

    VkPipelineLayout pipelineLayout;

    // Dynamic rendering support: attach VkPipelineRenderingCreateInfo when enabled
    // Note: defer attaching until after pipelineInfo is declared

    uint64_t plKey = 0;
    pipelineLayout = GetOrCreatePipelineLayoutCached(vkLayouts, pushRanges, plKey);
    VK_DBG(Logger::Log::Debug("[VK] pipelineLayout={}, pushRangesCount={}",
                              (const void*)pipelineLayout,
                              pushRanges.size()));
    if (pipelineLayout == VK_NULL_HANDLE)
    {
        Logger::Log::Error("CreateVulkanPipelineFromDesc: Failed to get/create pipeline layout (vkLayouts={})",
                           vkLayouts.size());
        return {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    }

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};

    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = stageCount;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = m_SwapchainRenderPass;
    pipelineInfo.subpass = 0;
#ifndef NDEBUG

#endif

    // Conservative dev warning for dynamic rendering without explicit color formats
    // Suppress this when the pipeline is depth-only (depthAttachmentFormat set)
    if (m_EnableDynamicRendering && m_SupportsDynamicRendering && desc.colorAttachmentFormats.empty() && desc.depthAttachmentFormat == 0)
    {
        // T19: explicit formats required for dynamic rendering; do not guess or default
        Logger::Log::Error("CreateVulkanPipelineFromDesc: No colorAttachmentFormats provided under dynamic rendering; failing pipeline creation");
        return failPipelineCreate("no colorAttachmentFormats under dynamic rendering");
    }

    // Persist dynamic rendering structs until vkCreateGraphicsPipelines call
    VkPipelineRenderingCreateInfo renderingInfo2{};
    renderingInfo2.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    std::vector<VkFormat> dynColorFormats; // keep alive across the create call

    const bool dynEnabled = (m_EnableDynamicRendering && m_SupportsDynamicRendering);
    if (dynEnabled)
    {
        // Prefer formats from PipelineDesc if provided; depth-only is allowed (zero color attachments)
        if (!desc.colorAttachmentFormats.empty())
        {
            dynColorFormats.reserve(desc.colorAttachmentFormats.size());
            auto mapFmt = [&](uint32_t n)
            {
                VkFormat f = VulkanResourceUtils::GetVulkanFormat(static_cast<TextureFormat>(n));
#ifndef NDEBUG
                // Debug guard: in engine code, colorAttachmentFormats should normally
                // be engine TextureFormat values cast to uint32_t. If the mapping
                // yields VK_FORMAT_UNDEFINED, we will treat the value as a raw
                // VkFormat for flexibility, but emit a one-time warning so that
                // accidental garbage (e.g. uninitialized values) is caught early.
                if (f == VK_FORMAT_UNDEFINED)
                {
                    const char* name = desc.debugName ? desc.debugName : "";
                    Logger::Log::Warning(
                        "CreateVulkanPipelineFromDesc: colorAttachmentFormats entry {} for pipeline '{}' does not map to a known TextureFormat; "
                        "treating it as a raw VkFormat. Ensure this is intentional.",
                        (int)n, name);
                }
#endif
                return f == VK_FORMAT_UNDEFINED ? static_cast<VkFormat>(n) : f;
            };
            for (auto v : desc.colorAttachmentFormats)
            {
                dynColorFormats.push_back(mapFmt(v));
            }
        }
        else
        {
            // depth-only path: leave dynColorFormats empty and proceed
        }
        // Ensure formats are supported as color attachments; fail fast if not.
        // Callers must provide supported formats; we intentionally do not guess here.
        for (const auto& fCheck : dynColorFormats)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, fCheck, &props);
            bool supported = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
            if (supported)
            {
                VkImageFormatProperties imgProps{};
                VkResult q = vkGetPhysicalDeviceImageFormatProperties(
                    m_PhysicalDevice, fCheck, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &imgProps);
                supported = (q == VK_SUCCESS);
            }
            if (!supported)
            {
                const char* name = desc.debugName ? desc.debugName : "";
                Logger::Log::Error(
                    "CreateVulkanPipelineFromDesc: color attachment format {} is not supported for pipeline '{}' (VkFormat={}); failing pipeline creation",
                    (int)fCheck,
                    name,
                    (unsigned)fCheck);
                // Do not silently substitute; require caller to choose supported formats
                return failPipelineCreate(
                    "unsupported color attachment VkFormat=" + std::to_string(static_cast<unsigned>(fCheck)));
            }
        }
        // Use core dynamic rendering struct (no KHR variant)
        VkPipelineRenderingCreateInfo renderingInfo2Local{};
        renderingInfo2Local.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo2Local.colorAttachmentCount = static_cast<uint32_t>(dynColorFormats.size());
        renderingInfo2Local.pColorAttachmentFormats = dynColorFormats.data();
        renderingInfo2Local.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        renderingInfo2Local.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        // Attach to pNext when not overridden by forced path later
        pipelineInfo.pNext = &renderingInfo2Local;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
        // Keep color blend attachments count consistent with dynamic rendering color attachments
        syncColorBlendAttachmentCount(renderingInfo2Local.colorAttachmentCount);
        // Map numeric TextureFormat or VkFormat values to VkFormat for depth/stencil
        auto mapFmt = [&](uint32_t n)
        {
            VkFormat f = VulkanResourceUtils::GetVulkanFormat(static_cast<TextureFormat>(n));
            return f == VK_FORMAT_UNDEFINED ? static_cast<VkFormat>(n) : f;
        };
        VkFormat depthFmt = mapFmt(desc.depthAttachmentFormat);
        VkFormat stencilFmt = mapFmt(desc.stencilAttachmentFormat);
        auto hasStencil = [&](VkFormat f)
        {
            switch (f)
            {
            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return true;
            default:
                return false;
            }
        };
        // Validate depth/stencil support for dynamic rendering when formats are provided.
        // This helps catch cases where a format (e.g. D24_UNORM_S8_UINT on some drivers)
        // advertises no DEPTH_STENCIL_ATTACHMENT usage and would otherwise only surface
        // as a validation-layer VUID during vkCreateGraphicsPipelines.
        auto checkDepthStencilSupport = [&](VkFormat fmt, const char* which) -> bool
        {
            if (fmt == VK_FORMAT_UNDEFINED)
                return true;

            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, fmt, &props);
            if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
            {
                const char* name = desc.debugName ? desc.debugName : "";
                Logger::Log::Error(
                    "CreateVulkanPipelineFromDesc: {} format {} (VkFormat={}) does not support VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT for pipeline '{}' under dynamic rendering; failing pipeline creation",
                    which,
                    (int)fmt,
                    (unsigned)fmt,
                    name);
                SetLastGraphicsPipelineFailure(
                    "Vulkan CreateVulkanPipelineFromDesc pipeline '" + std::string(name) + "': unsupported " +
                    which + " attachment VkFormat=" + std::to_string(static_cast<unsigned>(fmt)) +
                    " under dynamic rendering");
                return false;
            }
            return true;
        };

        if (!checkDepthStencilSupport(depthFmt, "depth") || !checkDepthStencilSupport(stencilFmt, "stencil"))
        {
            return {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        }

        // Also copy color attachments into the persistent struct
        renderingInfo2.colorAttachmentCount = static_cast<uint32_t>(dynColorFormats.size());
        renderingInfo2.pColorAttachmentFormats = dynColorFormats.data();

        renderingInfo2.depthAttachmentFormat = depthFmt;
        if (stencilFmt != VK_FORMAT_UNDEFINED)
        {
            renderingInfo2.stencilAttachmentFormat = stencilFmt;
        }
        else if (hasStencil(depthFmt))
        {
            renderingInfo2.stencilAttachmentFormat = depthFmt; // mirror depth when combined D+S format is used
        }
        else
        {
            renderingInfo2.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        }
        pipelineInfo.pNext = &renderingInfo2;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
        pipelineInfo.subpass = 0;
    }
    else
    {
        auto mapLegacyFmt = [&](uint32_t n)
        {
            VkFormat f = VulkanResourceUtils::GetVulkanFormat(static_cast<TextureFormat>(n));
            return f == VK_FORMAT_UNDEFINED ? static_cast<VkFormat>(n) : f;
        };

        std::vector<VkFormat> legacyColorFormats;
        legacyColorFormats.reserve(desc.colorAttachmentFormats.size());
        for (auto v : desc.colorAttachmentFormats)
        {
            legacyColorFormats.push_back(mapLegacyFmt(v));
        }

        VkFormat depthFmt = mapLegacyFmt(desc.depthAttachmentFormat);
        VkFormat stencilFmt = mapLegacyFmt(desc.stencilAttachmentFormat);
        auto hasStencil = [](VkFormat f)
        {
            switch (f)
            {
            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return true;
            default:
                return false;
            }
        };
        if (depthFmt == VK_FORMAT_UNDEFINED && stencilFmt != VK_FORMAT_UNDEFINED)
            depthFmt = stencilFmt;
        if (stencilFmt == VK_FORMAT_UNDEFINED && hasStencil(depthFmt))
            stencilFmt = depthFmt;

        const bool hasExplicitLegacyAttachments = !legacyColorFormats.empty() || depthFmt != VK_FORMAT_UNDEFINED;
        if (hasExplicitLegacyAttachments)
        {
            for (const auto& fCheck : legacyColorFormats)
            {
                VkFormatProperties props{};
                vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, fCheck, &props);
                if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0)
                {
                    const char* name = desc.debugName ? desc.debugName : "";
                    Logger::Log::Error(
                        "CreateVulkanPipelineFromDesc: color attachment format {} is not supported for legacy pipeline '{}' (VkFormat={}); failing pipeline creation",
                        (int)fCheck,
                        name,
                        (unsigned)fCheck);
                    return failPipelineCreate(
                        "unsupported legacy color attachment VkFormat=" + std::to_string(static_cast<unsigned>(fCheck)));
                }
            }

            auto checkDepthStencilSupportLegacy = [&](VkFormat fmt, const char* which) -> bool
            {
                if (fmt == VK_FORMAT_UNDEFINED)
                    return true;

                VkFormatProperties props{};
                vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, fmt, &props);
                if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
                {
                    const char* name = desc.debugName ? desc.debugName : "";
                    Logger::Log::Error(
                        "CreateVulkanPipelineFromDesc: {} format {} (VkFormat={}) does not support VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT for legacy pipeline '{}'; failing pipeline creation",
                        which,
                        (int)fmt,
                        (unsigned)fmt,
                        name);
                    SetLastGraphicsPipelineFailure(
                        "Vulkan CreateVulkanPipelineFromDesc pipeline '" + std::string(name) + "': unsupported legacy " +
                        which + " attachment VkFormat=" + std::to_string(static_cast<unsigned>(fmt)));
                    return false;
                }
                return true;
            };

            if (!checkDepthStencilSupportLegacy(depthFmt, "depth") || !checkDepthStencilSupportLegacy(stencilFmt, "stencil"))
            {
                return {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
            }

            std::vector<VkAttachmentLoadOp> colorLoadOps(legacyColorFormats.size(), VK_ATTACHMENT_LOAD_OP_LOAD);
            std::vector<VkAttachmentStoreOp> colorStoreOps(legacyColorFormats.size(), VK_ATTACHMENT_STORE_OP_STORE);
            VkRenderPass legacyRenderPass = GetOrCreateRenderPass(
                static_cast<uint32_t>(legacyColorFormats.size()),
                legacyColorFormats.empty() ? nullptr : legacyColorFormats.data(),
                colorLoadOps.empty() ? nullptr : colorLoadOps.data(),
                colorStoreOps.empty() ? nullptr : colorStoreOps.data(),
                depthFmt != VK_FORMAT_UNDEFINED,
                depthFmt,
                VK_ATTACHMENT_LOAD_OP_LOAD,
                VK_ATTACHMENT_STORE_OP_STORE,
                stencilFmt != VK_FORMAT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                stencilFmt != VK_FORMAT_UNDEFINED ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                false,
                resolvedRasterSamples);
            if (legacyRenderPass == VK_NULL_HANDLE)
            {
                const char* name = desc.debugName ? desc.debugName : "";
                Logger::Log::Error("CreateVulkanPipelineFromDesc: failed to create legacy render pass for pipeline '{}'", name);
                return failPipelineCreate("failed to create legacy render pass");
            }
            syncColorBlendAttachmentCount(static_cast<uint32_t>(legacyColorFormats.size()));
            pipelineInfo.pNext = nullptr;
            pipelineInfo.renderPass = legacyRenderPass;
            pipelineInfo.subpass = 0;
        }
        else
        {
            if (m_SwapchainRenderPass == VK_NULL_HANDLE)
                GetOrCreateSwapchainRenderPass();
            if (m_SwapchainRenderPass == VK_NULL_HANDLE)
            {
                return failPipelineCreate("failed to create swapchain render pass");
            }
            syncColorBlendAttachmentCount(1);
            pipelineInfo.pNext = nullptr;
            pipelineInfo.renderPass = m_SwapchainRenderPass;
            pipelineInfo.subpass = 0;
        }
    }

    VkResult result = VK_SUCCESS;

    VkPipeline graphicsPipeline;

    // Use dynamic rendering info prepared above; do not override with forced format path

    if (AnySetLayoutIsDescriptorBufferEligible(vkLayouts))
        pipelineInfo.flags |= VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;

    VkPipelineCache pipelineCache = m_VkDiskPipelineCache ? m_VkDiskPipelineCache->GetVkPipelineCache() : VK_NULL_HANDLE;
#ifndef NDEBUG
    if (std::getenv("GE_DEBUG_TRACE_RENDERING"))
    {
        const char* name = desc.debugName ? desc.debugName : "";
        Logger::Log::Debug("[VK][CreateGfx-DR] name='{}' pcSize={} pcStages=0x{:X} layout={}",
                           name,
                           (uint32_t)desc.pushConstantSize,
                           (uint32_t)desc.pushConstantStagesMask,
                           (const void*)pipelineLayout);
    }
#endif
    {
        // Lock the VkPipelineCache for thread safety. The Vulkan spec requires
        // external synchronization when multiple threads call vkCreate*Pipelines
        // with the same VkPipelineCache handle.
        auto cacheLock = m_VkDiskPipelineCache ? m_VkDiskPipelineCache->LockForCreation() : std::unique_lock<std::mutex>();
        auto t0 = std::chrono::high_resolution_clock::now();
        result = vkCreateGraphicsPipelines(m_Device, pipelineCache, 1, &pipelineInfo, nullptr, &graphicsPipeline);
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        if (ms > 10.0)
            Logger::Log::Info("[PSOCreate] '{}': {:.1f}ms", desc.debugName ? desc.debugName : "?", ms);
    }

    if (result != VK_SUCCESS)
    {
        const char* name = desc.debugName ? desc.debugName : "";
        const bool usingDynamicRendering = (pipelineInfo.renderPass == VK_NULL_HANDLE);
        VkFormat loggedDepthFmt = VK_FORMAT_UNDEFINED;
        VkFormat loggedStencilFmt = VK_FORMAT_UNDEFINED;
        uint32_t loggedColorCount = 0;

        if (usingDynamicRendering)
        {
            const VkPipelineRenderingCreateInfo* ri =
                static_cast<const VkPipelineRenderingCreateInfo*>(pipelineInfo.pNext);
            if (ri)
            {
                loggedColorCount = ri->colorAttachmentCount;
                loggedDepthFmt = ri->depthAttachmentFormat;
                loggedStencilFmt = ri->stencilAttachmentFormat;
            }
        }

        Logger::Log::Error(
            "vkCreateGraphicsPipelines failed for pipeline '{}' (dynamicRendering={}, colorAttachmentCount={}, depthFormat=0x{:X}, stencilFormat=0x{:X}) with error: {}",
            name,
            usingDynamicRendering,
            loggedColorCount,
            (unsigned)loggedDepthFmt,
            (unsigned)loggedStencilFmt,
            (int)result);
        SetLastGraphicsPipelineFailure(
            "Vulkan vkCreateGraphicsPipelines failed for pipeline '" + std::string(name) +
            "' result=" + std::to_string(static_cast<int>(result)) +
            " dynamicRendering=" + std::to_string(usingDynamicRendering ? 1 : 0) +
            " colorAttachmentCount=" + std::to_string(loggedColorCount) +
            " depthVkFormat=" + std::to_string(static_cast<unsigned>(loggedDepthFmt)) +
            " stencilVkFormat=" + std::to_string(static_cast<unsigned>(loggedStencilFmt)));
        ReleasePipelineLayoutCached(pipelineLayout);
        for (auto l : vkLayouts)
        {
            ReleaseDescriptorSetLayoutCached(l);
        }
        return {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    }

    SetVkObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(graphicsPipeline), desc.debugName);

    // Keep first layout (or VK_NULL_HANDLE if none) to honor existing API contract
    VkDescriptorSetLayout firstLayout = vkLayouts.empty() ? VK_NULL_HANDLE : vkLayouts[0];
    return {graphicsPipeline, pipelineLayout, firstLayout};
}

void VulkanDevice::BindDescriptorSet(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout, VkDescriptorSet descriptorSet)
{
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
}

void VulkanDevice::BindComputeDescriptorSet(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout, VkDescriptorSet descriptorSet)
{
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
}

VkPipelineLayout VulkanDevice::GetPipelineLayout(PipelineHandle pipeline)
{
    if (const VulkanPipeline* pipelinePtr = GetVulkanPipeline(pipeline))
    {
        if (pipelinePtr->device != VK_NULL_HANDLE && pipelinePtr->device != m_Device)
            return VK_NULL_HANDLE;
        return pipelinePtr->layout;
    }
    return VK_NULL_HANDLE;
}

// Helper method to create Vulkan descriptor set layout from our descriptor
VkDescriptorSetLayout VulkanDevice::CreateVulkanDescriptorSetLayout(const DescriptorSetLayoutDesc& layoutDesc)
{
    std::vector<VkDescriptorSetLayoutBinding> vkBindings;

    for (const auto& bindingIn : layoutDesc.bindings)
    {
        // Make a local copy so we can safely adjust stage flags for special cases
        DescriptorBinding binding = bindingIn;

        // Debug instrumentation + safety fixup: track BonesUBO-like bindings for stage mask
        if (binding.binding == 7 && binding.type == DescriptorType::UniformBuffer)
        {
            uint32_t originalStages = binding.shaderStages;
            const uint32_t vertexBit = VK_SHADER_STAGE_VERTEX_BIT;
            const uint32_t fragmentBit = VK_SHADER_STAGE_FRAGMENT_BIT;
            // If this UBO is fragment-only but used from VS (BonesUBO case), widen to VS|FS
            if ((binding.shaderStages & vertexBit) == 0 && (binding.shaderStages & fragmentBit) != 0)
            {
                binding.shaderStages |= vertexBit;
                Logger::Log::Debug(
                    "[DescriptorLayoutDebug] FIXUP BonesUBO-like binding: setLayout='{}' binding={} stages 0x{:X} -> 0x{:X}",
                    layoutDesc.debugName ? layoutDesc.debugName : "<null>",
                    binding.binding,
                    originalStages,
                    binding.shaderStages);
            }
            else
            {
                Logger::Log::Debug(
                    "[DescriptorLayoutDebug] setLayout='{}' binding={} type=UniformBuffer stages=0x{:X}",
                    layoutDesc.debugName ? layoutDesc.debugName : "<null>",
                    binding.binding,
                    binding.shaderStages);
            }
        }

        VkDescriptorSetLayoutBinding vkBinding{};
        vkBinding.binding = binding.binding;
        vkBinding.descriptorCount = binding.count;
        vkBinding.stageFlags = binding.shaderStages; // Treat as backend-native flags

        // Convert descriptor type
        switch (binding.type)
        {
        case DescriptorType::UniformBuffer:
            vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            break;
        case DescriptorType::StorageBuffer:
            vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            break;
        case DescriptorType::Texture:
            vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            break;
        case DescriptorType::Sampler:
            vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            break;
        case DescriptorType::CombinedImageSampler:
            vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            break;
        case DescriptorType::StorageImage:
            vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            break;
        case DescriptorType::AccelerationStructure:
            // The enum value only exists with VK_KHR_acceleration_structure
            // enabled, which is exactly what supportsRayQuery gates.
            if (!m_Capabilities.supportsRayQuery)
            {
                Logger::Log::Error(
                    "CreateVkDescriptorSetLayout: layout '{}' binding {} is an acceleration structure, "
                    "but this device has no ray-query support",
                    layoutDesc.debugName ? layoutDesc.debugName : "<null>", binding.binding);
                return VK_NULL_HANDLE;
            }
            vkBinding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            break;
        default:
            Logger::Log::Error("Unsupported descriptor type in CreateVkDescriptorSetLayout");
            return VK_NULL_HANDLE;
        }

        vkBindings.push_back(vkBinding);
    }

    // Translate per-binding flags. UAB is incompatible with the DB layout bit
    // (VUID-08003); on a DB-enabled device, UAB requests are silently stripped
    // here so the bindless texture array can still be authored with UAB intent.
    // PARTIALLY_BOUND is always kept — it is legal alongside DB.
    //
    // KNOWN ISSUE (MoltenVK-with-DB): stripping UAB makes the large bindless
    // sampler array validate against the small non-bindless sampler limit on
    // MoltenVK. The proper fix is pipeline-context-aware routing — when ANY
    // layout in a pipeline has UAB, force all its layouts to pool. Attempted
    // and reverted in this branch because the bindless descriptor pool isn't
    // sized for ~500k pool-allocated samplers (triggers OOPM on Windows). Both
    // a routing change and a pool-capacity bump are needed; tracked as a
    // follow-up.
    const bool buildingDbLayout = IsDescriptorBufferEnabled();
    bool hasBindingFlags = false;
    bool hasUpdateAfterBind = false;
    std::vector<VkDescriptorBindingFlags> bindingFlags(vkBindings.size(), 0);
    for (size_t i = 0; i < layoutDesc.bindings.size(); ++i)
    {
        const auto& src = layoutDesc.bindings[i];
        if ((src.flags & kDescriptorBindingUpdateAfterBind) && !buildingDbLayout)
        {
            bindingFlags[i] |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            hasUpdateAfterBind = true;
        }
        if (src.flags & kDescriptorBindingPartiallyBound)
            bindingFlags[i] |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        if (bindingFlags[i] != 0)
            hasBindingFlags = true;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
    flagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    createInfo.bindingCount = static_cast<uint32_t>(vkBindings.size());
    createInfo.pBindings = vkBindings.empty() ? nullptr : vkBindings.data();
    if (hasBindingFlags)
        createInfo.pNext = &flagsInfo;
    if (hasUpdateAfterBind)
        createInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    // Descriptor-buffer migration: layouts without UAB are eligible for the DB
    // path and carry DESCRIPTOR_BUFFER_BIT_EXT so they pair with DB-enabled
    // pipeline layouts. UAB layouts stay plain — they live on the legacy pool.
    if (buildingDbLayout)
    {
        createInfo.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    }

    VkDescriptorSetLayout layout;
    VkResult result = vkCreateDescriptorSetLayout(m_Device, &createInfo, nullptr, &layout);
    if (result != VK_SUCCESS)
    {
        Logger::Log::Error("Failed to create descriptor set layout! Error: {}", (int)result);
        return VK_NULL_HANDLE;
    }

    return layout;
}

// Proper descriptor set management methods

DescriptorSetHandle VulkanDevice::CreateDescriptorSet(const DescriptorSetDesc& desc)
{
    // Validate: empty layouts produce sets with 0 descriptors, which causes VUIDs on bind
    if (desc.layout.bindings.empty())
    {
        Logger::Log::Warning(
            "VulkanDevice: Refusing to create descriptor set with empty layout (0 bindings). dsName='{}', layoutName='{}'",
            desc.debugName ? desc.debugName : "<unnamed>",
            desc.layout.debugName ? desc.layout.debugName : "<unnamed>");
        return INVALID_HANDLE;
    }
    // Create descriptor set layout from description
    uint64_t layoutKey = 0;
    VkDescriptorSetLayout layout = GetOrCreateDescriptorSetLayoutCached(desc.layout, layoutKey);
    if (layout == VK_NULL_HANDLE)
    {
        Logger::Log::Error("VulkanDevice: Failed to create descriptor set layout");
        return INVALID_HANDLE;
    }

    // Look up the per-layout DB eligibility recorded at layout-creation time.
    // UAB layouts (e.g. the bindless texture array) are forced onto the pool
    // path even when the device is DB-enabled; the cache mirrors that decision.
    bool layoutDescriptorBufferEligible = false;
    {
        std::lock_guard<std::mutex> lock(m_LayoutCacheMutex);
        auto itEntry = m_SetLayoutCache.find(layoutKey);
        if (itEntry != m_SetLayoutCache.end())
            layoutDescriptorBufferEligible = itEntry->second.descriptorBufferEligible;
    }

    // Descriptor-buffer fast path: if the layout is DB-eligible and the runtime flag
    // is on, allocate bytes from the device's DB pool and return a tagged handle
    // (high bit set so downstream routing can distinguish DB from pool handles).
    // All callers are unchanged — the unified DescriptorSetHandle looks the same.
    // Persistent (transient=false) sets use the DB pool's app-lifetime region so
    // they're not rewound by BeginFrameReset; their entries carry a sentinel
    // frameSlot (UINT32_MAX) that never matches a real current-frame slot.
    if (layoutDescriptorBufferEligible && m_DescriptorBufferPool)
    {
        const bool persistent = !desc.transient;
        DescriptorBufferAllocation alloc = AllocateDescriptorSetBytes(layout, persistent);
        if (!alloc.IsValid())
        {
            Logger::Log::Error("VulkanDevice: failed to allocate descriptor-buffer storage; falling back");
            ReleaseDescriptorSetLayoutCached(layout);
            return INVALID_HANDLE;
        }
        const uint64_t id = kDescriptorBufferHandleTag | m_NextDescriptorBufferSetId.fetch_add(1);
        VulkanDescriptorBufferSetEntry entry;
        entry.alloc = alloc;
        entry.layout = layout;
        entry.frameSlot = persistent ? kPersistentFrameSlot : m_CurrentFrame.load(std::memory_order_relaxed);
        if (desc.debugName) entry.debugName = desc.debugName;
        if (DescriptorValidationActive())
            entry.layoutDesc = desc.layout;
        {
            std::unique_lock lock(m_DescriptorBufferSetsMutex);
            m_DescriptorBufferSets.emplace(id, std::move(entry));
        }
        return DescriptorSetHandle(id);
    }

    // Allocate via DescriptorSetAllocator (transient, persistent, or persistent+UpdateAfterBind)
    const bool needsUpdateAfterBind = (desc.poolFlags & DescriptorPoolFlags::UpdateAfterBind) != DescriptorPoolFlags::None;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (desc.transient)
        descriptorSet = m_DsAllocator.AllocateTransient(layout, desc.debugName);
    else if (needsUpdateAfterBind)
        descriptorSet = m_DsAllocator.AllocatePersistentUpdateAfterBind(layout, desc.debugName);
    else
        descriptorSet = m_DsAllocator.AllocatePersistent(layout, desc.debugName);
    if (descriptorSet == VK_NULL_HANDLE)
    {
        Logger::Log::Error("VulkanDevice: Failed to allocate descriptor set (allocator)");
        ReleaseDescriptorSetLayoutCached(layout);
        return INVALID_HANDLE;
    }

    // Track layout and live set for cleanup and double-free prevention.
    // Transient descriptor sets must not enter these maps: they're bulk-reset each
    // frame via vkResetDescriptorPool and never individually destroyed, so the
    // entries would grow by N per frame. Their validation metadata instead goes
    // into the per-frame-slot map that BeginFrame purges when it rewinds the
    // slot's transient pools.
    if (!desc.transient)
    {
        {
            std::lock_guard<std::mutex> lock(m_DescriptorSetLayoutsMutex);
            m_DescriptorSetLayouts[descriptorSet] = layout;
            m_DescriptorSetLayoutDescs[descriptorSet] = desc.layout;
        }
        {
            std::lock_guard<std::mutex> lock(m_LiveDescriptorSetsMutex);
            m_LiveDescriptorSets.insert(descriptorSet);
        }
    }
    else if (DescriptorValidationActive())
    {
        std::lock_guard<std::mutex> lock(m_DescriptorSetLayoutsMutex);
        m_TransientDescriptorSetLayoutDescs[m_CurrentFrame][descriptorSet] = desc.layout;
    }

    // Return descriptor set as handle (pool frees sets; we clean up layouts we created)
    DescriptorSetHandle outHandle(reinterpret_cast<uint64_t>(descriptorSet));
    // Transient sets are pool-reset in bulk and not individually destroyed.
    // Do not owner-track them, otherwise the global owner map grows unbounded.
    if (!desc.transient)
        RegisterDescriptorSetOwner(outHandle, m_Device);
    return outHandle;
}

void VulkanDevice::DestroyDescriptorSet(DescriptorSetHandle descriptorSet)
{
    if (!DescriptorSetOwnerMatches(descriptorSet, m_Device))
        return;
    // Descriptor-buffer handle: release the m_DescriptorBufferSets entry and
    // return. The underlying bump-allocated persistent region doesn't get
    // reclaimed (audit §7.1.3), but dropping the entry stops the map from
    // growing unboundedly across view churn / shutdown cycles. Without this
    // branch, the raw reinterpret_cast below on a tagged id (high bit set)
    // produces a garbage VkDescriptorSet pointer that gets handed to
    // vkFreeDescriptorSets on the legacy pool — silent leak + undefined
    // behaviour.
    if (IsDescriptorBufferHandle(descriptorSet))
    {
        std::unique_lock lock(m_DescriptorBufferSetsMutex);
        m_DescriptorBufferSets.erase(descriptorSet.id);
        return;
    }
    VkDescriptorSet vkSet = reinterpret_cast<VkDescriptorSet>(descriptorSet.id);
    if (vkSet == VK_NULL_HANDLE)
        return;
    // Pool-allocated sets are freed by their owning DescriptorSetAllocator pools
    // (persistent pools at device teardown, transient pools by the per-frame bulk
    // reset) — only the live-set bookkeeping happens here.
    {
        std::lock_guard<std::mutex> lock(m_LiveDescriptorSetsMutex);
        m_LiveDescriptorSets.erase(vkSet);
    }
    // Destroy the associated layout if we created one for this set
    {
        std::lock_guard<std::mutex> lock(m_DescriptorSetLayoutsMutex);
        auto it = m_DescriptorSetLayouts.find(vkSet);
        if (it != m_DescriptorSetLayouts.end())
        {
            VkDescriptorSetLayout layout = it->second;
            m_DescriptorSetLayouts.erase(it);
            m_DescriptorSetLayoutDescs.erase(vkSet);
            if (layout != VK_NULL_HANDLE)
            {
                ReleaseDescriptorSetLayoutCached(layout);
            }
        }
        // Explicitly destroyed transient sets: drop their validation metadata now
        // rather than waiting for the slot's BeginFrame purge.
        for (auto& slotMap : m_TransientDescriptorSetLayoutDescs)
            slotMap.erase(vkSet);
    }
    UnregisterDescriptorSetOwner(descriptorSet);
}

IDevice::DescriptorAllocatorStats VulkanDevice::GetDescriptorAllocatorStats() const
{
    IDevice::DescriptorAllocatorStats out{};
    auto s = m_DsAllocator.GetStats();
    out.TransientPools = s.TransientPools;
    out.TransientAllocated = s.TransientAllocated;
    out.TransientCapacity = s.TransientCapacity;
    out.PersistentPools = s.PersistentPools;
    out.PersistentAllocated = s.PersistentAllocated;
    out.PersistentCapacity = s.PersistentCapacity;
    return out;
}

TextureViewHandle VulkanDevice::CreateTextureView(TextureHandle texture, const TextureViewDesc& desc)
{
    // Reachable from ECS extraction workers (TextureService bindless
    // registration); vkCreateImageView must not overlap a rebuild's teardown.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Create);

    VkImage image = GetVkImage(texture);
    if (image == VK_NULL_HANDLE)
        return TextureViewHandle{};

    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = image;

    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    switch (desc.viewType)
    {
    case TextureViewType::View2D:
        viewType = VK_IMAGE_VIEW_TYPE_2D;
        break;
    case TextureViewType::View2DArray:
        viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        break;
    case TextureViewType::ViewCube:
        viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        break;
    case TextureViewType::ViewCubeArray:
        viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        break;
    case TextureViewType::View3D:
        viewType = VK_IMAGE_VIEW_TYPE_3D;
        break;
    default:
        viewType = VK_IMAGE_VIEW_TYPE_2D;
        break;
    }
    ci.viewType = viewType;
    // Format override if provided (0 means same as image)
    VulkanTexture* tex = GetVulkanTexture(texture);
    VkFormat baseFmt = tex ? tex->format : VK_FORMAT_UNDEFINED;
    ci.format = desc.formatOverride != 0 ? static_cast<VkFormat>(desc.formatOverride) : baseFmt;
    ci.components = {VulkanMappings::TranslateSwizzle(desc.r), VulkanMappings::TranslateSwizzle(desc.g), VulkanMappings::TranslateSwizzle(desc.b), VulkanMappings::TranslateSwizzle(desc.a)};
    // Derive aspect mask from explicit TextureAspect when provided; otherwise
    // fall back to the image format so depth-stencil images don't end up with
    // a COLOR aspect by accident.
    VkImageAspectFlags aspect = VulkanMappings::TranslateAspect(desc.aspect);
    if (aspect == 0)
    {
        switch (baseFmt)
        {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            // Default to DEPTH-only for depth-stencil formats (descriptor-safe).
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        default:
            aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            break;
        }
    }
    ci.subresourceRange.aspectMask = aspect;
    ci.subresourceRange.baseMipLevel = desc.baseMip;
    ci.subresourceRange.levelCount = desc.levelCount == 0 ? VK_REMAINING_MIP_LEVELS : desc.levelCount;
    ci.subresourceRange.baseArrayLayer = desc.baseLayer;
    ci.subresourceRange.layerCount = desc.layerCount == 0 ? VK_REMAINING_ARRAY_LAYERS : desc.layerCount;

    VkImageView view = VK_NULL_HANDLE;
    VkResult vr = vkCreateImageView(m_Device, &ci, nullptr, &view);
    if (vr != VK_SUCCESS)
    {
        Logger::Log::Error("VulkanDevice: vkCreateImageView failed ({})", (int)vr);
        return TextureViewHandle{};
    }
    SetVkObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(view), desc.debugName);
    WatchViewIfOnWatchedImage(ci.image, view, ci.subresourceRange);

    // vkCreateImageView is externally synchronized on nothing here (VkDevice
    // creation entry points are thread-safe), so only the bookkeeping is locked.
    TextureViewHandle h;
    {
        std::unique_lock viewLock(m_TextureViewMutex);
        h = m_TextureViews.Create(view);
        // Metadata that drives the descriptor image layout for this view.
        m_TextureViewAspects[h.id] = ci.subresourceRange.aspectMask;
        m_TextureViewFormats[h.id] = ci.format;
        // Owner, for lifetime hygiene and view refcounting.
        m_TextureViewOwners[static_cast<uint32_t>(h.id)] = texture;
        if (tex)
        {
            ++tex->viewRefCount;
        }
        std::lock_guard<std::mutex> trackingLock(m_ResourceTrackingMutex);
        m_LiveTextureViews.push_back(h);
    }
    return h;
}

void VulkanDevice::DestroyTextureViewImmediate(TextureViewHandle h)
{
    assert((std::this_thread::get_id() == m_DeviceOwnerThread.load(std::memory_order_relaxed) ||
            std::this_thread::get_id() == m_RebuildExclusiveThread.load(std::memory_order_relaxed)) &&
           "immediate texture-view destroy reached from a non-owner thread");
    // Released before m_TextureViewMutex is taken, per the documented lock order.
    if (h.IsValid())
    {
        std::lock_guard<std::mutex> lock(m_DeferredDestroyMutex);
        m_PendingTextureViewDestroyIds.erase(h.id);
    }

    // Held across the vkDestroyImageView so no reader can resolve a handle whose
    // VkImageView has already been destroyed.
    std::unique_lock viewLock(m_TextureViewMutex);

    // Stale handle: already destroyed (e.g. CleanupVulkan's live-view sweep runs
    // before an owning command list retires and re-destroys its cached views).
    // The first destruction handled the refcount and metadata; re-running would
    // assert in the generational store.
    if (!m_TextureViews.Get(h))
        return;

    // Decrement owner's view refcount if tracked
    auto itOwner = m_TextureViewOwners.find(static_cast<uint32_t>(h.id));
    if (itOwner != m_TextureViewOwners.end())
    {
        if (VulkanTexture* tex = GetVulkanTexture(itOwner->second))
        {
            if (tex->viewRefCount > 0)
                --tex->viewRefCount;
        }
        m_TextureViewOwners.erase(itOwner);
    }

    VkImageView* pView = m_TextureViews.Get(h);
    if (pView && *pView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_Device, *pView, nullptr);
        *pView = VK_NULL_HANDLE;
    }
    // Erase cached metadata for this view
    m_TextureViewAspects.erase(h.id);
    m_TextureViewFormats.erase(h.id);
    m_TextureViews.Destroy(h);
    // Untrack live handle. In idle/bulk destroy mode, WaitForIdle() compacts
    // the vectors in one linear pass to avoid O(N^2) remove churn.
    if (!m_IsShutdown && !m_BulkDestroyInProgress)
    {
        std::lock_guard<std::mutex> lock(m_ResourceTrackingMutex);
        auto it = std::remove(m_LiveTextureViews.begin(), m_LiveTextureViews.end(), h);
        if (it != m_LiveTextureViews.end())
            m_LiveTextureViews.erase(it, m_LiveTextureViews.end());
    }
}

VulkanDevice::TextureViewDescriptorInfo
VulkanDevice::ResolveTextureViewForDescriptor(TextureViewHandle view) const
{
    TextureViewDescriptorInfo out;
    // One shared lock covers the interior-pointer dereference and both map
    // lookups, so a concurrent CreateTextureView can neither reallocate the
    // generational store under us nor rehash a map mid-find.
    std::shared_lock viewLock(m_TextureViewMutex);
    if (const VkImageView* pView = m_TextureViews.Get(view))
        out.view = *pView;
    const auto itAspect = m_TextureViewAspects.find(view.id);
    if (itAspect != m_TextureViewAspects.end())
        out.aspect = itAspect->second;
    const auto itOwner = m_TextureViewOwners.find(static_cast<uint32_t>(view.id));
    if (itOwner != m_TextureViewOwners.end())
        if (const VulkanTexture* tex = GetVulkanTextureConst(itOwner->second))
            out.sampledInGeneral = tex->sampledInGeneralLayout;
    return out;
}

void VulkanDevice::DestroyTextureView(TextureViewHandle h)
{
    // Deliberately NOT guarded by DeviceRebuildSharedGuard: this entry runs
    // under a queue's submit mutex on every submit path (RouteSubmittedCommandList
    // -> OnSubmitted -> ReleaseAttachmentViews), and the rebuild mutex is
    // OUTERMOST — acquiring it here would invert the documented lock order. The
    // deferred queueing below is mutex-protected, and every worker-reachable
    // path here sits inside an outer guarded entry on the same thread; the
    // immediate helper asserts owner-thread affinity.
    if (!InTeardown() && h.IsValid())
    {
        QueueDeferredTextureViewDestroy(h);
        return;
    }
    DestroyTextureViewImmediate(h);
}

VkImageView VulkanDevice::GetVkImageView(TextureViewHandle handle)
{
    std::shared_lock viewLock(m_TextureViewMutex);
    VkImageView* pView = m_TextureViews.Get(handle);
    return pView ? *pView : VK_NULL_HANDLE;
}

bool VulkanDevice::TryGetDescriptorLayoutDescForValidation(DescriptorSetHandle handle, DescriptorSetLayoutDesc& out)
{
    if (IsDescriptorBufferHandle(handle))
    {
        std::shared_lock lock(m_DescriptorBufferSetsMutex);
        auto it = m_DescriptorBufferSets.find(handle.id);
        if (it == m_DescriptorBufferSets.end() || it->second.layoutDesc.bindings.empty())
            return false;
        out = it->second.layoutDesc;
        return true;
    }

    VkDescriptorSet vkSet = reinterpret_cast<VkDescriptorSet>(handle.id);
    if (vkSet == VK_NULL_HANDLE)
        return false;
    std::lock_guard<std::mutex> lock(m_DescriptorSetLayoutsMutex);
    auto it = m_DescriptorSetLayoutDescs.find(vkSet);
    if (it != m_DescriptorSetLayoutDescs.end() && !it->second.bindings.empty())
    {
        out = it->second;
        return true;
    }
    // Transient sets live in per-frame-slot maps. Set pointers are partitioned by
    // slot-specific pools, so at most one slot map can contain a given pointer.
    for (const auto& slotMap : m_TransientDescriptorSetLayoutDescs)
    {
        auto itT = slotMap.find(vkSet);
        if (itT != slotMap.end() && !itT->second.bindings.empty())
        {
            out = itT->second;
            return true;
        }
    }
    return false;
}

bool VulkanDevice::ValidateDescriptorUpdateAgainstLayout(const DescriptorSetLayoutDesc& layoutDesc,
                                                         const DescriptorSetUpdate& update) const
{
    auto itB = std::find_if(layoutDesc.bindings.begin(), layoutDesc.bindings.end(), [&](const DescriptorBinding& b)
                            { return b.binding == update.binding; });
    if (itB == layoutDesc.bindings.end())
    {
        const char* layoutName = layoutDesc.debugName ? layoutDesc.debugName : "<unnamed>";
        Logger::Log::Error(
            "VulkanDevice: UpdateDescriptorSet binding not found in layout: {} (layout='{}')",
            update.binding,
            layoutName);
        assert(false && "Descriptor update: binding not found in layout");
        return false;
    }
    if (static_cast<uint32_t>(update.arrayElement) >= itB->count)
    {
        Logger::Log::Error("VulkanDevice: UpdateDescriptorSet arrayElement out of range");
        assert(false && "Descriptor update: arrayElement out of range");
        return false;
    }
    if (itB->type != update.type)
    {
        Logger::Log::Error("VulkanDevice: Descriptor type mismatch for binding {}", update.binding);
        assert(false && "Descriptor update: type mismatch with layout");
        return false;
    }
    // Count check: ensure we're not writing beyond declared array size (for starting at arrayElement)
    uint32_t writeCount = 0;
    if (update.type == DescriptorType::UniformBuffer || update.type == DescriptorType::StorageBuffer)
    {
        writeCount = static_cast<uint32_t>(update.buffers.size());
    }
    else if (update.type == DescriptorType::Sampler)
    {
        writeCount = static_cast<uint32_t>(update.samplers.size());
    }
    else if (update.type == DescriptorType::Texture || update.type == DescriptorType::CombinedImageSampler || update.type == DescriptorType::StorageImage)
    {
        // Prefer textureViews if provided, otherwise textures
        writeCount = static_cast<uint32_t>(!update.textureViews.empty() ? update.textureViews.size() : update.textures.size());
    }
    else if (update.type == DescriptorType::AccelerationStructure)
    {
        writeCount = static_cast<uint32_t>(update.accelerationStructures.size());
    }
    if (writeCount > 0 && (update.arrayElement + writeCount) > itB->count)
    {
        Logger::Log::Error("VulkanDevice: Descriptor write exceeds binding array count");
        assert(false && "Descriptor update: write out of array bounds");
        return false;
    }
    return true;
}

// A descriptor image write must carry every Vulkan handle its type consumes:
// COMBINED_IMAGE_SAMPLER needs both, SAMPLER needs the sampler, the image types
// need the view (VUID-VkWriteDescriptorSet-descriptorType-00646/-02997). A null
// here is an engine handle that outlived its device and resolved to
// VK_NULL_HANDLE — GetVkSampler and GetVulkanTexture both return null rather
// than a live object once the owning VkDevice is gone. Drivers dereference these
// fields directly inside vkUpdateDescriptorSets, so a null is an access
// violation in the ICD, not a validation message.
//
// WriteDescriptorBufferUpdate refuses the same handles on the descriptor-buffer
// path and logs the same way, but at a different granularity: there each element
// is written at its own computed offset, so a bad element is skipped
// individually and the rest of the array still lands. The whole-write rule below
// is forced by pImageInfo being positional, not by the handles.
static bool DescriptorImageInfoIsComplete(const VkDescriptorImageInfo& info, DescriptorType type) noexcept
{
    switch (type)
    {
    case DescriptorType::Sampler:
        return info.sampler != VK_NULL_HANDLE;
    case DescriptorType::CombinedImageSampler:
        return info.sampler != VK_NULL_HANDLE && info.imageView != VK_NULL_HANDLE;
    case DescriptorType::Texture:
    case DescriptorType::StorageImage:
        return info.imageView != VK_NULL_HANDLE;
    default:
        return true;
    }
}

// Whole-write drop, never a per-element skip: pImageInfo is indexed from
// dstArrayElement, so dropping element k in place would silently rebind every
// later element one slot low. Mirrors the batch validator's "any violation drops
// the whole update" rule.
static void LogDroppedIncompleteDescriptorWrite(const VkDescriptorImageInfo& info,
                                                const DescriptorSetUpdate& update,
                                                size_t element)
{
    Logger::Log::Error(
        "VulkanDevice: dropping descriptor write with unresolvable handle (binding={}, arrayElement={}, "
        "type={}, sampler={}, imageView={}) — a handle outlived its device (stale after a device rebuild?); "
        "writing it would fault inside the driver",
        update.binding,
        update.arrayElement + static_cast<uint32_t>(element),
        static_cast<int>(update.type),
        info.sampler != VK_NULL_HANDLE ? "live" : "NULL",
        info.imageView != VK_NULL_HANDLE ? "live" : "NULL");
}

void VulkanDevice::UpdateDescriptorSet(DescriptorSetHandle descriptorSet, const DescriptorSetUpdate& update)
{
    // Reachable from ECS extraction workers (bindless image-binding updates);
    // descriptor writes must not overlap a rebuild's teardown.
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Access);

    // Validate against the recorded layout before either write path runs. This is
    // the single choke point for descriptor writes: the pool path below, and the
    // descriptor-buffer path, which writes raw bytes at binding offsets and would
    // otherwise scribble into the adjacent set's bytes on an array overflow.
    if (DescriptorValidationActive())
    {
        DescriptorSetLayoutDesc layoutDesc{};
        if (TryGetDescriptorLayoutDescForValidation(descriptorSet, layoutDesc) &&
            !ValidateDescriptorUpdateAgainstLayout(layoutDesc, update))
        {
            return; // validated no-op
        }
    }

    // Descriptor-buffer handle? Route to DB write path.
    if (IsDescriptorBufferHandle(descriptorSet))
    {
        WriteDescriptorBufferUpdate(descriptorSet, update);
        return;
    }
    if (!DescriptorSetOwnerMatches(descriptorSet, m_Device))
    {
        assert(false && "UpdateDescriptorSet: cross-device descriptor set handle used");
        return;
    }
    // Convert handle back to VkDescriptorSet
    VkDescriptorSet vkDescriptorSet = reinterpret_cast<VkDescriptorSet>(descriptorSet.id);
    if (vkDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkDescriptorImageInfo> imageInfos;
    // Set by the image cases below when an element's handles do not resolve; the
    // whole write is then abandoned (see LogDroppedIncompleteDescriptorWrite).
    bool dropImageWrite = false;

    // Acceleration structures are chained through pNext rather than carried in
    // a pBufferInfo/pImageInfo array, so both the handle array and the chained
    // struct must stay alive until vkUpdateDescriptorSets at the end.
    std::vector<VkAccelerationStructureKHR> accelStructs;
    VkWriteDescriptorSetAccelerationStructureKHR accelWrite{};
    if (update.type == DescriptorType::AccelerationStructure)
    {
        if (!m_AccelerationStructures)
        {
            Logger::Log::Error(
                "VulkanDevice: acceleration-structure descriptor write on a device with no AS backend "
                "(binding={})",
                update.binding);
            return;
        }
        accelStructs.reserve(update.accelerationStructures.size());
        for (const TlasSlotHandle slot : update.accelerationStructures)
        {
            const VkAccelerationStructureKHR as = m_AccelerationStructures->GetTlasVkHandle(slot);
            if (as == VK_NULL_HANDLE)
            {
                // Dropping the whole write, not the element: the array is
                // positional, and a ray query against a never-written slot is a
                // fault rather than a dark pixel.
                Logger::Log::Error(
                    "VulkanDevice: dropping acceleration-structure descriptor write (binding={}, slot={}) "
                    "— the slot has no TLAS object yet (PrepareTlas not called, or a capacity grow is "
                    "in flight)",
                    update.binding, slot.id);
                return;
            }
            accelStructs.push_back(as);
        }
        if (!accelStructs.empty())
        {
            accelWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            accelWrite.accelerationStructureCount = static_cast<uint32_t>(accelStructs.size());
            accelWrite.pAccelerationStructures = accelStructs.data();

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.pNext = &accelWrite;
            write.dstSet = vkDescriptorSet;
            write.dstBinding = update.binding;
            write.dstArrayElement = update.arrayElement;
            write.descriptorCount = accelWrite.accelerationStructureCount;
            write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            writes.push_back(write);
        }
    }

    // Prepare buffer descriptors
    if (!update.buffers.empty())
    {
        bufferInfos.reserve(update.buffers.size());
        for (size_t i = 0; i < update.buffers.size(); ++i)
        {
            VulkanBuffer* buffer = GetVulkanBuffer(update.buffers[i]);
            if (!buffer)
            {
                Logger::Log::Error("VulkanDevice: Invalid buffer handle in descriptor set update (handle.id={}, binding={}, type={})",
                    static_cast<uint64_t>(update.buffers[i].id), update.binding, static_cast<int>(update.type));
                continue;
            }

#ifndef NDEBUG
            // Debug helper: detect buffers bound as uniform buffers without matching usage flags
            if (update.type == DescriptorType::UniformBuffer)
            {
                const uint32_t usageBits = static_cast<uint32_t>(buffer->usage);
                if ((usageBits & static_cast<uint32_t>(BufferUsage::Uniform)) == 0u)
                {
                    Logger::Log::Warning(
                        "VulkanDevice: DescriptorSet binding buffer as UniformBuffer without Uniform usage. handle={}, "
                        "debugName='{}', usageBits=0x{:X}",
                        (uint64_t)update.buffers[i],
                        buffer->debugName.c_str(),
                        usageBits);
                }
            }
#endif

            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = buffer->buffer;
            bufferInfo.offset = i < update.bufferOffsets.size() ? update.bufferOffsets[i] : 0;
            const VkDeviceSize remaining =
                buffer->size > bufferInfo.offset ? buffer->size - bufferInfo.offset : 0;
            // A descriptor's range must be non-zero
            // (VUID-VkDescriptorBufferInfo-range-00341), so a requested size of 0
            // — the public API's "bind the whole buffer" — becomes VK_WHOLE_SIZE
            // rather than a literal zero range.
            if (i < update.bufferRanges.size() && update.bufferRanges[i] > 0)
                bufferInfo.range = std::min<VkDeviceSize>(update.bufferRanges[i], remaining);
            else
                bufferInfo.range = VK_WHOLE_SIZE;
            bufferInfos.push_back(bufferInfo);
        }

        if (!bufferInfos.empty())
        {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = vkDescriptorSet;
            write.dstBinding = update.binding;
            write.dstArrayElement = update.arrayElement;
            write.descriptorCount = static_cast<uint32_t>(bufferInfos.size());
            write.pBufferInfo = bufferInfos.data();

            // Convert descriptor type
            switch (update.type)
            {
            case DescriptorType::UniformBuffer:
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                break;
            case DescriptorType::StorageBuffer:
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                break;
            default:
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                break;
            }

            writes.push_back(write);
        }
    }

    // Prepare image/sampler descriptors
    // Case A: Sampled image(s) only
    if (update.type == DescriptorType::Texture && (!update.textureViews.empty() || !update.textures.empty()))
    {
        const bool useViews = !update.textureViews.empty();
        const size_t count = useViews ? update.textureViews.size() : update.textures.size();
        imageInfos.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            VkDescriptorImageInfo ii{};
            if (useViews)
            {
                const auto info = ResolveTextureViewForDescriptor(update.textureViews[i]);
                ii.imageView = info.view;
                if (info.sampledInGeneral)
                {
                    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                }
                else if (info.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
                {
                    // Match barrier mapping (TranslateResourceStateToLayout(ResourceState::DepthRead))
                    ii.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                }
                else
                {
                    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
            }
            else
            {
                VulkanTexture* texture = GetVulkanTexture(update.textures[i]);
                if (!texture)
                {
                    Logger::Log::Error(
                        "VulkanDevice: Invalid texture handle in descriptor set update (handle.id={}, binding={}, arrayElement={}, type={})",
                        static_cast<uint64_t>(update.textures[i].id),
                        update.binding,
                        update.arrayElement + static_cast<uint32_t>(i),
                        static_cast<int>(update.type));
                    dropImageWrite = true;
                    break;
                }
                ii.imageView = texture->view;
                // GENERAL-resident sampled texture (View.DepthResolved): claim
                // GENERAL to match the RG's SampledInGeneralLayout constraint (storage co-use),
                // else VUID-vkCmdDraw-None-09600 under async compute.
                const bool isDepthFmt =
                    (texture->format == VK_FORMAT_D16_UNORM ||
                     texture->format == VK_FORMAT_D32_SFLOAT ||
                     texture->format == VK_FORMAT_D24_UNORM_S8_UINT ||
                     texture->format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                     texture->format == VK_FORMAT_X8_D24_UNORM_PACK32);
                ii.imageLayout = texture->sampledInGeneralLayout
                        ? VK_IMAGE_LAYOUT_GENERAL
                        : (isDepthFmt
                        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            ii.sampler = VK_NULL_HANDLE;
            if (!DescriptorImageInfoIsComplete(ii, update.type))
            {
                LogDroppedIncompleteDescriptorWrite(ii, update, i);
                dropImageWrite = true;
                break;
            }
            imageInfos.push_back(ii);
        }
        if (!imageInfos.empty() && !dropImageWrite)
        {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = vkDescriptorSet;
            w.dstBinding = update.binding;
            w.dstArrayElement = update.arrayElement;
            w.descriptorCount = static_cast<uint32_t>(imageInfos.size());
            w.pImageInfo = imageInfos.data();
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes.push_back(w);
        }
    }
    // Case B: Sampler(s) only
    else if (update.type == DescriptorType::Sampler && !update.samplers.empty())
    {
        imageInfos.reserve(update.samplers.size());
        for (size_t i = 0; i < update.samplers.size(); ++i)
        {
            VkDescriptorImageInfo ii{};
            ii.sampler = GetVkSampler(update.samplers[i], m_Device);
            ii.imageView = VK_NULL_HANDLE; // not used for SAMPLER
            ii.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!DescriptorImageInfoIsComplete(ii, update.type))
            {
                LogDroppedIncompleteDescriptorWrite(ii, update, i);
                dropImageWrite = true;
                break;
            }
            imageInfos.push_back(ii);
        }
        if (!imageInfos.empty() && !dropImageWrite)
        {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = vkDescriptorSet;
            w.dstBinding = update.binding;
            w.dstArrayElement = update.arrayElement;
            w.descriptorCount = static_cast<uint32_t>(imageInfos.size());
            w.pImageInfo = imageInfos.data();
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes.push_back(w);
        }
    }
    // Case C: Combined image sampler(s)
    else if (update.type == DescriptorType::CombinedImageSampler && (!update.textureViews.empty() || !update.textures.empty()))
    {
        const bool useViews = !update.textureViews.empty();
        const size_t count = useViews ? update.textureViews.size() : update.textures.size();
        imageInfos.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            VkDescriptorImageInfo ii{};
            if (useViews)
            {
                const auto info = ResolveTextureViewForDescriptor(update.textureViews[i]);
                ii.imageView = info.view;
                if (info.sampledInGeneral)
                {
                    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                }
                else if (info.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
                {
                    // Match barrier mapping (TranslateResourceStateToLayout(ResourceState::DepthRead))
                    ii.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                }
                else
                {
                    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
            }
            else
            {
                VulkanTexture* texture = GetVulkanTexture(update.textures[i]);
                if (!texture)
                {
                    Logger::Log::Error(
                        "VulkanDevice: Invalid texture handle in descriptor set update (handle.id={}, binding={}, arrayElement={}, type={})",
                        static_cast<uint64_t>(update.textures[i].id),
                        update.binding,
                        update.arrayElement + static_cast<uint32_t>(i),
                        static_cast<int>(update.type));
                    dropImageWrite = true;
                    break;
                }
                ii.imageView = texture->view;
                // GENERAL-resident sampled texture (View.DepthResolved): claim
                // GENERAL to match the RG's SampledInGeneralLayout constraint (storage co-use),
                // else VUID-vkCmdDraw-None-09600 under async compute.
                const bool isDepthFmt =
                    (texture->format == VK_FORMAT_D16_UNORM ||
                     texture->format == VK_FORMAT_D32_SFLOAT ||
                     texture->format == VK_FORMAT_D24_UNORM_S8_UINT ||
                     texture->format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                     texture->format == VK_FORMAT_X8_D24_UNORM_PACK32);
                ii.imageLayout = texture->sampledInGeneralLayout
                        ? VK_IMAGE_LAYOUT_GENERAL
                        : (isDepthFmt
                        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            // Assign sampler for COMBINED_IMAGE_SAMPLER
            if (i < update.samplers.size())
            {
                ii.sampler = GetVkSampler(update.samplers[i], m_Device);
            }
            else if (!update.samplers.empty())
            {
                // Fallback: use first sampler if counts differ
                ii.sampler = GetVkSampler(update.samplers[0], m_Device);
            }
            else
            {
                ii.sampler = VK_NULL_HANDLE;
            }
            if (!DescriptorImageInfoIsComplete(ii, update.type))
            {
                LogDroppedIncompleteDescriptorWrite(ii, update, i);
                dropImageWrite = true;
                break;
            }
            imageInfos.push_back(ii);
        }
        if (!imageInfos.empty() && !dropImageWrite)
        {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = vkDescriptorSet;
            w.dstBinding = update.binding;
            w.dstArrayElement = update.arrayElement;
            w.descriptorCount = static_cast<uint32_t>(imageInfos.size());
            w.pImageInfo = imageInfos.data();
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes.push_back(w);
        }
    }
    // Case D: Storage image(s)
    else if (update.type == DescriptorType::StorageImage && (!update.textureViews.empty() || !update.textures.empty()))
    {
        const bool useViews = !update.textureViews.empty();
        const size_t count = useViews ? update.textureViews.size() : update.textures.size();
        imageInfos.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            VkDescriptorImageInfo ii{};
            if (useViews)
            {
                ii.imageView = ResolveTextureViewForDescriptor(update.textureViews[i]).view;
            }
            else
            {
                VulkanTexture* texture = GetVulkanTexture(update.textures[i]);
                if (!texture)
                {
                    Logger::Log::Error("VulkanDevice: Invalid texture handle in storage image update");
                    dropImageWrite = true;
                    break;
                }
                ii.imageView = texture->view;
            }
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL; // storage images are written in GENERAL
            ii.sampler = VK_NULL_HANDLE;
            if (!DescriptorImageInfoIsComplete(ii, update.type))
            {
                LogDroppedIncompleteDescriptorWrite(ii, update, i);
                dropImageWrite = true;
                break;
            }
            imageInfos.push_back(ii);
        }
        if (!imageInfos.empty() && !dropImageWrite)
        {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = vkDescriptorSet;
            w.dstBinding = update.binding;
            w.dstArrayElement = update.arrayElement;
            w.descriptorCount = static_cast<uint32_t>(imageInfos.size());
            w.pImageInfo = imageInfos.data();
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes.push_back(w);
        }
    }

    // Update descriptor sets
    if (!writes.empty())
    {
        WatchCheckDescriptorWrites(writes.data(), static_cast<uint32_t>(writes.size()));
        vkUpdateDescriptorSets(m_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanDevice::UpdateDescriptorSetBatch(DescriptorSetHandle descriptorSet, std::span<const DescriptorSetUpdate> updates)
{
    if (updates.empty())
        return;

    // Same rebuild exclusion as UpdateDescriptorSet (see there).
    DeviceRebuildSharedGuard rebuildGuard(*this, DeviceRebuildSharedGuard::Kind::Access);

    // Same choke point as UpdateDescriptorSet: fetch the layout metadata once and
    // check every update in the batch. Any violation drops the whole batch as a
    // validated no-op — the entries are typically interdependent (one set's
    // bindings), so partially applying them would leave the set inconsistent.
    if (DescriptorValidationActive())
    {
        DescriptorSetLayoutDesc layoutDesc{};
        if (TryGetDescriptorLayoutDescForValidation(descriptorSet, layoutDesc))
        {
            for (const auto& u : updates)
            {
                if (!ValidateDescriptorUpdateAgainstLayout(layoutDesc, u))
                    return; // validated no-op
            }
        }
    }

    if (IsDescriptorBufferHandle(descriptorSet))
    {
        for (const auto& u : updates)
            WriteDescriptorBufferUpdate(descriptorSet, u);
        return;
    }

    VkDescriptorSet vkSet = reinterpret_cast<VkDescriptorSet>(descriptorSet.id);
    if (vkSet == VK_NULL_HANDLE)
        return;

    size_t totalBufEntries = 0;
    size_t totalImgEntries = 0;
    for (const auto& u : updates)
    {
        if (u.type == DescriptorType::UniformBuffer || u.type == DescriptorType::StorageBuffer)
            totalBufEntries += u.buffers.size();
        else if (u.type == DescriptorType::Sampler)
            totalImgEntries += u.samplers.size();
        else
            totalImgEntries += !u.textureViews.empty() ? u.textureViews.size() : u.textures.size();
    }

    std::vector<VkDescriptorBufferInfo> bufferInfos;
    bufferInfos.reserve(totalBufEntries);
    std::vector<VkDescriptorImageInfo> imageInfos;
    imageInfos.reserve(totalImgEntries);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(updates.size());

    for (const auto& update : updates)
    {
        if (update.type == DescriptorType::UniformBuffer || update.type == DescriptorType::StorageBuffer)
        {
            if (update.buffers.empty())
                continue;

            const size_t startIdx = bufferInfos.size();
            uint32_t count = 0;
            for (size_t i = 0; i < update.buffers.size(); ++i)
            {
                VulkanBuffer* buf = GetVulkanBuffer(update.buffers[i]);
                if (!buf)
                    continue;
                VkDescriptorBufferInfo bi{};
                bi.buffer = buf->buffer;
                bi.offset = i < update.bufferOffsets.size() ? update.bufferOffsets[i] : 0;
                const VkDeviceSize remaining =
                    buf->size > bi.offset ? buf->size - bi.offset : 0;
                // Same non-zero range rule as the single-binding path above
                // (VUID-VkDescriptorBufferInfo-range-00341).
                if (i < update.bufferRanges.size() && update.bufferRanges[i] > 0)
                    bi.range = std::min<VkDeviceSize>(update.bufferRanges[i], remaining);
                else
                    bi.range = VK_WHOLE_SIZE;
                bufferInfos.push_back(bi);
                ++count;
            }

            if (count > 0)
            {
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = vkSet;
                w.dstBinding = update.binding;
                w.dstArrayElement = update.arrayElement;
                w.descriptorCount = count;
                w.pBufferInfo = bufferInfos.data() + startIdx;
                w.descriptorType = (update.type == DescriptorType::StorageBuffer)
                                       ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                       : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes.push_back(w);
            }
        }
        else if (update.type == DescriptorType::Sampler)
        {
            if (update.samplers.empty())
                continue;
            const size_t startIdx = imageInfos.size();
            bool dropImageWrite = false;
            for (size_t i = 0; i < update.samplers.size(); ++i)
            {
                VkDescriptorImageInfo ii{};
                ii.sampler = GetVkSampler(update.samplers[i], m_Device);
                ii.imageView = VK_NULL_HANDLE;
                ii.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                if (!DescriptorImageInfoIsComplete(ii, update.type))
                {
                    LogDroppedIncompleteDescriptorWrite(ii, update, i);
                    dropImageWrite = true;
                    break;
                }
                imageInfos.push_back(ii);
            }
            if (dropImageWrite)
            {
                imageInfos.resize(startIdx);
                continue;
            }
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = vkSet;
            w.dstBinding = update.binding;
            w.dstArrayElement = update.arrayElement;
            w.descriptorCount = static_cast<uint32_t>(update.samplers.size());
            w.pImageInfo = imageInfos.data() + startIdx;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes.push_back(w);
        }
        else
        {
            const bool useViews = !update.textureViews.empty();
            const size_t srcCount = useViews ? update.textureViews.size() : update.textures.size();
            if (srcCount == 0)
                continue;
            const size_t startIdx = imageInfos.size();
            uint32_t count = 0;
            bool dropImageWrite = false;
            for (size_t i = 0; i < srcCount; ++i)
            {
                VkDescriptorImageInfo ii{};
                if (useViews)
                {
                    const auto info = ResolveTextureViewForDescriptor(update.textureViews[i]);
                    ii.imageView = info.view;
                    if (update.type != DescriptorType::StorageImage && info.sampledInGeneral)
                        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    else if (info.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
                        ii.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                    else
                        ii.imageLayout = (update.type == DescriptorType::StorageImage)
                                             ? VK_IMAGE_LAYOUT_GENERAL
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
                else
                {
                    VulkanTexture* texture = GetVulkanTexture(update.textures[i]);
                    if (!texture)
                    {
                        dropImageWrite = true;
                        break;
                    }
                    ii.imageView = texture->view;
                    if (update.type == DescriptorType::StorageImage)
                    {
                        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    }
                    else if (texture->sampledInGeneralLayout)
                    {
                        // GENERAL-resident sampled texture (View.DepthResolved):
                        // claim GENERAL to match the RG's SampledInGeneralLayout constraint for
                        // storage co-use, else VUID-09600 at dispatch (DepthMinMax
                        // samples it) / draw on devices WITHOUT descriptor buffers,
                        // which take this legacy batch path. Guarded ahead of the
                        // depth-format test, mirroring sampledLayout() and the
                        // single-update Case A/C edits.
                        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    }
                    else
                    {
                        const bool isDepthFmt =
                            (texture->format == VK_FORMAT_D16_UNORM ||
                             texture->format == VK_FORMAT_D32_SFLOAT ||
                             texture->format == VK_FORMAT_D24_UNORM_S8_UINT ||
                             texture->format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                             texture->format == VK_FORMAT_X8_D24_UNORM_PACK32);
                        ii.imageLayout = isDepthFmt
                                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    }
                }
                ii.sampler = VK_NULL_HANDLE;
                if (update.type == DescriptorType::CombinedImageSampler)
                {
                    if (i < update.samplers.size())
                        ii.sampler = GetVkSampler(update.samplers[i], m_Device);
                    else if (!update.samplers.empty())
                        ii.sampler = GetVkSampler(update.samplers[0], m_Device);
                }
                if (!DescriptorImageInfoIsComplete(ii, update.type))
                {
                    LogDroppedIncompleteDescriptorWrite(ii, update, i);
                    dropImageWrite = true;
                    break;
                }
                imageInfos.push_back(ii);
                ++count;
            }
            if (dropImageWrite)
            {
                imageInfos.resize(startIdx);
                continue;
            }
            if (count > 0)
            {
                VkWriteDescriptorSet w{};
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = vkSet;
                w.dstBinding = update.binding;
                w.dstArrayElement = update.arrayElement;
                w.descriptorCount = count;
                w.pImageInfo = imageInfos.data() + startIdx;
                switch (update.type)
                {
                case DescriptorType::Texture:           w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; break;
                case DescriptorType::CombinedImageSampler: w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
                case DescriptorType::StorageImage:       w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
                default:                                 w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; break;
                }
                writes.push_back(w);
            }
        }
    }

    if (!writes.empty())
    {
        WatchCheckDescriptorWrites(writes.data(), static_cast<uint32_t>(writes.size()));
        vkUpdateDescriptorSets(m_Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

VkDescriptorSet VulkanDevice::GetVkDescriptorSet(DescriptorSetHandle handle)
{
    // DB-backed handles don't have a VkDescriptorSet — callers that need one must
    // check IsDescriptorBufferHandle first.
    if (IsDescriptorBufferHandle(handle))
        return VK_NULL_HANDLE;
    if (!DescriptorSetOwnerMatches(handle, m_Device))
        return VK_NULL_HANDLE;
    // Convert handle back to VkDescriptorSet
    return reinterpret_cast<VkDescriptorSet>(handle.id);
}

VulkanDevice::DescriptorBufferSetLookup
VulkanDevice::LookupDescriptorBufferSet(DescriptorSetHandle handle)
{
    DescriptorBufferSetLookup out;
    if (!IsDescriptorBufferHandle(handle))
        return out;
    // Shared lock: lets parallel-recording workers resolve handles concurrently.
    // Writers (CreateDescriptorSet emplace, BeginFrameReset purge, swapchain-
    // recreate purge) take the exclusive lock. Copying only the POD fields
    // (alloc / layout / frameSlot) under the lock avoids the std::string copy
    // that used to happen per-draw on bind — see the Lookup struct comment.
    std::shared_lock lock(m_DescriptorBufferSetsMutex);
    auto it = m_DescriptorBufferSets.find(handle.id);
    if (it == m_DescriptorBufferSets.end())
        return out;
    out.alloc     = it->second.alloc;
    out.layout    = it->second.layout;
    out.frameSlot = it->second.frameSlot;
    out.Found     = true;
    return out;
}

std::string VulkanDevice::DescriptorBufferSetDebugName(DescriptorSetHandle handle) const
{
    if (!IsDescriptorBufferHandle(handle))
        return "<not-a-descriptor-buffer-set>";
    std::shared_lock lock(m_DescriptorBufferSetsMutex);
    auto it = m_DescriptorBufferSets.find(handle.id);
    if (it == m_DescriptorBufferSets.end())
        return "<unknown>";
    return it->second.debugName.empty() ? "<unnamed>" : it->second.debugName;
}

void VulkanDevice::CleanupVulkan()
{
    // Ensure GPU is completely idle before destroying any objects.
    if (m_Device != VK_NULL_HANDLE)
    {
        EnsureGlobalGpuIdle();
    }
    // Flush descriptor-set ownership entries for this device (including transient
    // sets that are pool-reset and not individually destroyed).
    UnregisterDescriptorSetOwnersForDevice(m_Device);
    // Flush sampler ownership entries for this device as well.
    UnregisterSamplerOwnersForDevice(m_Device);

    TeardownAllWindowTargetsForShutdown();
    FlushDeferredResourcesAndCompactLiveTracking();

    // Cleanup framebuffers first
    for (auto framebuffer : m_SwapchainFramebuffers)
    {
        if (framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(m_Device, framebuffer, nullptr);
        }
    }
    m_SwapchainFramebuffers.clear();

    // Cleanup offscreen framebuffer resources
    if (m_OffscreenImageView != VK_NULL_HANDLE)
    {

        vkDestroyImageView(m_Device, m_OffscreenImageView, nullptr);
        m_OffscreenImageView = VK_NULL_HANDLE;
    }
    if (m_OffscreenImage != VK_NULL_HANDLE)
    {

        vkDestroyImage(m_Device, m_OffscreenImage, nullptr);
        m_OffscreenImage = VK_NULL_HANDLE;
    }
    if (m_OffscreenImageMemory != VK_NULL_HANDLE)
    {
        UntrackDirectMemoryAllocation(m_OffscreenImageMemory);
        vkFreeMemory(m_Device, m_OffscreenImageMemory, nullptr);
        m_OffscreenImageMemory = VK_NULL_HANDLE;
    }

    // Cleanup render pass
    if (m_SwapchainRenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(m_Device, m_SwapchainRenderPass, nullptr);
        m_SwapchainRenderPass = VK_NULL_HANDLE;
    }

    // Pipeline layouts and descriptor set layouts referenced by VulkanPipeline instances
    // are released via DestroyPipeline and the global pipeline manager. VulkanDevice only
    // needs to destroy descriptor set layouts created directly and to flush any remaining
    // cached layouts in the maps below.

    // Destroy descriptor set layouts created via CreateDescriptorSet(desc)
    for (auto& kv : m_DescriptorSetLayouts)
    {
        if (kv.second != VK_NULL_HANDLE)
        {
            // Use cache-aware release to avoid double-destroy; honors ref counts
            ReleaseDescriptorSetLayoutCached(kv.second);
        }
    }
    m_DescriptorSetLayouts.clear();
    // Validation metadata must not outlive the pools that own the sets: a rebuilt
    // device's pools can reuse VkDescriptorSet pointer values, which would resolve
    // to stale layout descs.
    m_DescriptorSetLayoutDescs.clear();
    for (auto& slotMap : m_TransientDescriptorSetLayoutDescs)
        slotMap.clear();

    // Descriptor set allocator already destroyed in Shutdown() before VMA shutdown

    // Cleanup cached descriptor set layouts and pipeline layouts
    // Note: Avoid recursive locking by collecting set layouts to release, then releasing outside the mutex scope.
    {
        std::vector<VkDescriptorSetLayout> toRelease;
        {
            std::lock_guard<std::mutex> lock(m_LayoutCacheMutex);
            for (auto& kv : m_PipelineLayoutCache)
            {
                if (kv.second.layout != VK_NULL_HANDLE)
                {
                    vkDestroyPipelineLayout(m_Device, kv.second.layout, nullptr);
                }
            }
            m_PipelineLayoutCache.clear();
            m_PipelineLayoutReverse.clear();

            toRelease.reserve(m_SetLayoutCache.size());
            for (auto& kv : m_SetLayoutCache)
            {
                if (kv.second.layout != VK_NULL_HANDLE)
                {
                    toRelease.push_back(kv.second.layout);
                }
            }
            // Do not clear m_SetLayoutCache here; ReleaseDescriptorSetLayoutCached() will erase entries.
        }
        for (auto l : toRelease)
        {
            ReleaseDescriptorSetLayoutCached(l);
        }
    }

    // Final safety sweep: if any set layouts remain in cache, destroy them directly
    {
        std::lock_guard<std::mutex> lock(m_LayoutCacheMutex);
        if (!m_SetLayoutCache.empty())
        {
            for (auto& kv : m_SetLayoutCache)
            {
                if (kv.second.layout != VK_NULL_HANDLE)
                {
                    vkDestroyDescriptorSetLayout(m_Device, kv.second.layout, nullptr);
                }
            }
            m_SetLayoutCache.clear();
            m_SetLayoutReverse.clear();
        }
    }

    // Clear the swapchain texture wrapper map before destroying swapchain image views below.
    // NOTE: Don't destroy image views here - they're the same as m_SwapchainImageViews
    // and will be destroyed in the regular swapchain cleanup to prevent double-destruction.
    // Swapchain images don't need VMA cleanup (allocation is nullptr), and the VkImage
    // itself is owned by the swapchain, so we simply drop our cached wrappers.
    m_SwapchainTextures.clear();

    // Cleanup swapchain resources

    for (size_t i = 0; i < m_SwapchainImageViews.size(); i++)
    {

        if (m_SwapchainImageViews[i] != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_Device, m_SwapchainImageViews[i], nullptr);
            m_SwapchainImageViews[i] = VK_NULL_HANDLE;
        }
    }
    m_SwapchainImageViews.clear();

    // Destroy cached framebuffers
    for (auto& kv : m_FramebufferCache)
    {
        if (kv.second != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(m_Device, kv.second, nullptr);
        }
    }
    m_FramebufferCache.clear();

    // Destroy cached render passes
    for (auto& kv : m_RenderPassCache)
    {
        if (kv.second != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(m_Device, kv.second, nullptr);
        }
    }
    m_RenderPassCache.clear();

    m_SwapchainImages.clear();

    if (m_Swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }

    if (m_Surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
    }

    // Resource cleanup is handled by global resource managers
    // No local cleanup needed since resources are managed globally

    // Cleanup any remaining texture views created via CreateTextureView.
    // First pass: destroy everything we know about via m_LiveTextureViews.
    for (auto h : m_LiveTextureViews)
    {
        DestroyTextureViewImmediate(h);
    }
    m_LiveTextureViews.clear();

    // Second pass: authoritative sweep over m_TextureViews itself. Any view
    // still alive here escaped m_LiveTextureViews tracking — typically a
    // per-pass attachment view whose owning VulkanCommandList hasn't been
    // destroyed yet (RenderGraph retirement order). Without this sweep it
    // would leak past vkDestroyDevice and trip VUID-05137. Collect first,
    // then destroy — we can't mutate m_TextureViews while iterating it.
    std::vector<VkImageView> strayViews;
    {
        std::unique_lock viewLock(m_TextureViewMutex);
        m_TextureViews.ForEach([&](Handle /*h*/, VkImageView& v)
        {
            if (v != VK_NULL_HANDLE)
            {
                strayViews.push_back(v);
                v = VK_NULL_HANDLE;
            }
        });
    }
    if (!strayViews.empty())
    {
        Logger::Log::Warning(
            "VulkanDevice::CleanupVulkan: {} image view(s) survived m_LiveTextureViews tracking; "
            "destroying now to avoid VUID-05137. This indicates a CreateTextureView caller whose "
            "owner outlives the live-tracking sweep.",
            strayViews.size());
        for (VkImageView v : strayViews)
            vkDestroyImageView(m_Device, v, nullptr);
    }
    {
        std::unique_lock viewLock(m_TextureViewMutex);
        m_TextureViews.Clear();
        m_TextureViewAspects.clear();
        m_TextureViewFormats.clear();
        m_TextureViewOwners.clear();
    }

    // Descriptor sets are automatically freed when descriptor pool is destroyed

    // Cleanup any remaining direct memory allocations
    CleanupDirectMemoryAllocations();

    // QueryPool cleanup handled in Shutdown() method above

    // Cleanup synchronization objects
    FlushRetiredWindowTargetSemaphores();
    for (VkSemaphore sem : m_ImageAvailableSemaphores)
    {
        DestroySemaphoreTracked(sem);
    }
    m_ImageAvailableSemaphores.clear();

    for (VkSemaphore sem : m_PresentReadySemaphores)
    {
        DestroySemaphoreTracked(sem);
    }
    m_PresentReadySemaphores.clear();

    // Destroy any remaining timeline semaphores registered on the device
    for (auto& kv : m_TimelineSemaphores)
    {
        DestroySemaphoreTracked(kv.second.sem);
    }
    m_TimelineSemaphores.clear();

    // Destroy per-queue frame fences
    for (auto& fr : m_Frames)
    {
        auto destroyFenceIfAny = [&](VkFence& f)
        { if (f != VK_NULL_HANDLE) { vkDestroyFence(m_Device, f, nullptr); f = VK_NULL_HANDLE; } };
        destroyFenceIfAny(fr.graphics.fence);
        destroyFenceIfAny(fr.compute.fence);
        destroyFenceIfAny(fr.transfer.fence);
    }

    // GE_VK_FORCE_GPU_HANG pipeline (raw, untracked). Destroy before the device so
    // the (deliberate) TDR teardown doesn't trip a pipeline-leak VUID. The layout is
    // owned by the pipeline-layout cache; just forget it here.
    if (m_GpuHangPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_Device, m_GpuHangPipeline, nullptr);
        m_GpuHangPipeline = VK_NULL_HANDLE;
    }
    m_GpuHangPipelineLayout = VK_NULL_HANDLE;

    // Destroy all command pools (graphics + optional compute/transfer)
    // Destroy per-thread command pools before the main pools
    DestroyAllThreadPools();

    if (m_CommandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
        m_CommandPool = VK_NULL_HANDLE;
    }
    if (m_ComputeCommandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_Device, m_ComputeCommandPool, nullptr);
        m_ComputeCommandPool = VK_NULL_HANDLE;
    }
    if (m_TransferCommandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_Device, m_TransferCommandPool, nullptr);
        m_TransferCommandPool = VK_NULL_HANDLE;
    }

    if (m_Device != VK_NULL_HANDLE)
    {
        // Resource cleanup is handled by global resource managers

        // Check for any remaining direct allocations
        if (m_OffscreenImageMemory != VK_NULL_HANDLE)
        {
            Logger::Log::Warning("Offscreen image memory still allocated: {}",
                                 (const void*)m_OffscreenImageMemory);
        }

        CloseSemaphoreLedgerForDeviceGeneration();
        vkDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;

        // Device-level VK_EXT_debug_utils entry points die with the device.
        // Cleared here, not just at re-resolution, so the window between
        // teardown and ResolveDebugUtilsEntryPoints holds nulls rather than the
        // dead device's pointers.
        m_SetVkObjectNameFn = nullptr;
        m_DebugUtilsLabelFns = {};
    }

    // Shared-instance refcounted teardown. Skipped during an in-place device
    // rebuild (Q6 slice 2): the instance is instance-scoped and survives a device
    // loss, and the same IDevice object is being reused, so we neither decrement
    // the refcount (design F8) nor destroy the instance/debug messenger here.
    if (m_Instance != VK_NULL_HANDLE && !m_RebuildInProgress)
    {
        if (const auto last = SharedInstanceRegistry::Get().Release(m_Instance))
        {
            if (last->DebugMessenger != VK_NULL_HANDLE)
            {
                auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_Instance, "vkDestroyDebugUtilsMessengerEXT");
                if (func != nullptr)
                {
                    func(m_Instance, last->DebugMessenger, nullptr);
                }
            }
            vkDestroyInstance(m_Instance, nullptr);
        }
        m_DebugMessenger = VK_NULL_HANDLE;
        m_Instance = VK_NULL_HANDLE;
    }
}

// Error handling utilities
bool VulkanDevice::CheckVkResult(VkResult result, const char* operation) const
{
    if (result == VK_SUCCESS)
    {
        return true;
    }

    Logger::Log::Error("VulkanDevice: {} failed with error: {} ({})",
                       operation,
                       VkResultToString(result),
                       (int)result);
    return false;
}

const char* VulkanDevice::VkResultToString(VkResult result) const
{
    switch (result)
    {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN:
        return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY:
        return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
        return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION:
        return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
        return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
        return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT:
        return "VK_ERROR_VALIDATION_FAILED_EXT";
    case VK_ERROR_INVALID_SHADER_NV:
        return "VK_ERROR_INVALID_SHADER_NV";
    default:
        return "UNKNOWN_VK_RESULT";
    }
}

// ===== Layout cache helpers (descriptor set and pipeline layout) =====
VkDescriptorSetLayout VulkanDevice::GetOrCreateDescriptorSetLayoutCached(const DescriptorSetLayoutDesc& layoutDesc, uint64_t& outKey)
{
    // FNV-1a 64-bit hash of the layout description. UAB and non-UAB variants of
    // otherwise-identical bindings hash differently because per-binding flags
    // are mixed in.
    uint64_t key = 1469598103934665603ull;
    auto mix = [&](uint64_t x)
    { key ^= x; key *= 1099511628211ull; };
    mix(static_cast<uint64_t>(layoutDesc.bindings.size()));
    for (const auto& b : layoutDesc.bindings)
    {
        mix(static_cast<uint64_t>(b.binding));
        mix(static_cast<uint64_t>(b.type));
        mix(static_cast<uint64_t>(b.count));
        mix(static_cast<uint64_t>(b.shaderStages));
        mix(static_cast<uint64_t>(b.flags));
    }
    outKey = key;

    // Hold the lock across lookup + create + insert to prevent two threads from
    // concurrently creating the same layout (TOCTOU leak). The Vulkan call is
    // fast (driver bookkeeping, no compilation) so serialization is acceptable.
    std::lock_guard<std::mutex> lock(m_LayoutCacheMutex);
    auto it = m_SetLayoutCache.find(key);
    if (it != m_SetLayoutCache.end())
    {
        it->second.refCount++;
        return it->second.layout;
    }

    VkDescriptorSetLayout layout = CreateVulkanDescriptorSetLayout(layoutDesc);
    if (layout == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    LayoutCacheEntry entry;
    entry.layout = layout;
    entry.refCount = 1;
    entry.descriptorBufferEligible = IsDescriptorBufferEnabled();
    m_SetLayoutCache[key] = entry;
    m_SetLayoutReverse[layout] = key;
    return layout;
}

VkPipelineLayout VulkanDevice::GetOrCreatePipelineLayoutCached(const std::vector<VkDescriptorSetLayout>& setLayouts,
                                                               uint32_t pushConstantSize,
                                                               uint32_t pushConstantStagesMask,
                                                               uint64_t& outKey)
{
    // Hold the lock across the entire lookup + create + insert to prevent
    // concurrent threads from creating duplicate pipeline layouts for the
    // same key. vkCreatePipelineLayout is fast (no compilation).
    std::lock_guard<std::mutex> lock(m_LayoutCacheMutex);

    uint64_t key = 1469598103934665603ull; // FNV-1a 64
    auto mix = [&](uint64_t x)
    { key ^= x; key *= 1099511628211ull; };
    mix(static_cast<uint64_t>(setLayouts.size()));
    for (auto l : setLayouts)
    {
        auto itKey = m_SetLayoutReverse.find(l);
        uint64_t lk = (itKey != m_SetLayoutReverse.end()) ? itKey->second : reinterpret_cast<uint64_t>(l);
        mix(lk);
    }
    mix(static_cast<uint64_t>(pushConstantSize));
    mix(static_cast<uint64_t>(pushConstantStagesMask));
    outKey = key;
    // See note in the vector-overload: descriptor-buffer eligibility is carried by the
    // set layout's DESCRIPTOR_BUFFER_BIT_EXT flag; no pipeline-layout flag exists.

    auto it = m_PipelineLayoutCache.find(key);
    if (it != m_PipelineLayoutCache.end())
    {
        it->second.refCount++;
        return it->second.layout;
    }

    std::vector<VkPushConstantRange> ranges;
    if (pushConstantSize)
    {
        // Explicit stagesMask is required. A zero mask produces a permissive
        // VK_SHADER_STAGE_ALL layout that makes every downstream push-constant
        // emission look valid regardless of pipeline type — it masked the
        // compute-stage VUID-01795 bug for a long time. Warn loudly (assert in
        // debug) and fall back to ALL so existing callers keep working while we
        // migrate them off the default.
        if (!pushConstantStagesMask)
        {
            Logger::Log::Error(
                "GetOrCreatePipelineLayoutCached(scalar): push-constant size={} requested with "
                "stagesMask=0. Caller must specify the shader stages that read the range; "
                "falling back to VK_SHADER_STAGE_ALL, but this risks VUID-01795 for compute "
                "pipelines and hides spec-mismatches for graphics.",
                pushConstantSize);
            assert(false && "GetOrCreatePipelineLayoutCached: explicit stagesMask required");
        }
        VkPushConstantRange r{};
        r.stageFlags = pushConstantStagesMask
                           ? static_cast<VkShaderStageFlags>(pushConstantStagesMask)
                           : static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_ALL);
        r.offset = 0;
        r.size = pushConstantSize;
        if (r.size > m_MaxPushConstantBytes)
        {
            return VK_NULL_HANDLE;
        }
        ranges.push_back(r);
    }

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    ci.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
    ci.pushConstantRangeCount = static_cast<uint32_t>(ranges.size());
    ci.pPushConstantRanges = ranges.empty() ? nullptr : ranges.data();

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(m_Device, &ci, nullptr, &layout) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }

    m_PipelineLayoutCache[key] = {layout, 1};
    m_PipelineLayoutReverse[layout] = key;
    return layout;
}

// Overload for creating pipeline layout with explicit push ranges
VkPipelineLayout VulkanDevice::GetOrCreatePipelineLayoutCached(const std::vector<VkDescriptorSetLayout>& setLayouts,
                                                               const std::vector<VkPushConstantRange>& pushRanges,
                                                               uint64_t& outKey)
{
    // Hold the lock across lookup + create + insert (same rationale as above).
    std::lock_guard<std::mutex> lock(m_LayoutCacheMutex);

    uint64_t key = 1469598103934665603ull; // FNV-1a 64
    auto mix = [&](uint64_t x)
    { key ^= x; key *= 1099511628211ull; };
    mix(static_cast<uint64_t>(setLayouts.size()));
    for (auto l : setLayouts)
    {
        auto itKey = m_SetLayoutReverse.find(l);
        uint64_t lk = (itKey != m_SetLayoutReverse.end()) ? itKey->second : reinterpret_cast<uint64_t>(l);
        mix(lk);
    }
    mix(static_cast<uint64_t>(pushRanges.size()));
    VkShaderStageFlags unionStages = 0;
    uint32_t totalSize = 0;
    for (const auto& r : pushRanges)
    {
        unionStages |= r.stageFlags;
        totalSize = std::max(totalSize, r.offset + r.size);
    }
    mix(static_cast<uint64_t>(unionStages));
    mix(static_cast<uint64_t>(totalSize));
    outKey = key;
    // Descriptor-buffer eligibility is carried on the set layout
    // (VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT, Phase 1c). Because
    // DB-eligible set layouts hash distinctly, DB and non-DB pipeline layouts naturally
    // bucket to different cache keys here. The DB-ness is applied at *pipeline* creation
    // via VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT (see pipeline creation sites +
    // AnySetLayoutIsDescriptorBufferEligible).

    auto it = m_PipelineLayoutCache.find(key);
    if (it != m_PipelineLayoutCache.end())
    {
        it->second.refCount++;
        return it->second.layout;
    }

    VkPipelineLayoutCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    ci.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
    ci.pushConstantRangeCount = static_cast<uint32_t>(pushRanges.size());
    ci.pPushConstantRanges = pushRanges.empty() ? nullptr : pushRanges.data();
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(m_Device, &ci, nullptr, &layout) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }

    m_PipelineLayoutCache[key] = {layout, 1};
    m_PipelineLayoutReverse[layout] = key;
    return layout;
}

VkDescriptorSetLayout VulkanDevice::GetOrCreateSetLayoutHandle(const DescriptorSetLayoutDesc& desc)
{
    uint64_t key = 0;
    return GetOrCreateDescriptorSetLayoutCached(desc, key);
}

DescriptorBufferAllocation VulkanDevice::AllocateDescriptorSetBytes(VkDescriptorSetLayout layout, bool persistent)
{
    if (!m_DescriptorBufferPool || !m_FpGetDescriptorSetLayoutSizeEXT || layout == VK_NULL_HANDLE)
        return {};
    VkDeviceSize layoutSize = 0;
    m_FpGetDescriptorSetLayoutSizeEXT(m_Device, layout, &layoutSize);
    if (layoutSize == 0)
        return {};
    const VkDeviceSize align = m_DescriptorBufferPool->OffsetAlignment();
    return persistent
        ? m_DescriptorBufferPool->AllocatePersistent(layoutSize, align)
        : m_DescriptorBufferPool->Allocate(layoutSize, align);
}

void VulkanDevice::WriteDescriptorAt(const DescriptorBufferAllocation& alloc,
                                     VkDescriptorSetLayout layout,
                                     uint32_t binding,
                                     const VkDescriptorGetInfoEXT& getInfo,
                                     size_t descriptorSize)
{
    if (!alloc.IsValid() || !m_FpGetDescriptorEXT || !m_FpGetDescriptorSetLayoutBindingOffsetEXT)
        return;
    VkDeviceSize bindingOffset = 0;
    m_FpGetDescriptorSetLayoutBindingOffsetEXT(m_Device, layout, binding, &bindingOffset);
    if (bindingOffset + descriptorSize > alloc.Size)
    {
        Logger::Log::Error("VulkanDevice::WriteDescriptorAt: binding {} offset {} + size {} > allocation size {}",
                           binding, bindingOffset, descriptorSize, alloc.Size);
        return;
    }
    uint8_t* dst = static_cast<uint8_t*>(alloc.HostMapped) + bindingOffset;
    WatchCheckDescriptorGetInfo(getInfo, binding, 0, "WriteDescriptorAt");
    m_FpGetDescriptorEXT(m_Device, &getInfo, descriptorSize, dst);
    // VMA_MEMORY_USAGE_AUTO picks coherent memory on all desktop drivers we care about;
    // mobile / non-coherent paths would require vmaFlushAllocation here. We rely on the
    // VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT + MAPPED_BIT combination to choose a
    // coherent heap. See VulkanDescriptorBufferPool::PushBlockLocked.
}

// Translate a DescriptorSetUpdate (the engine's abstract update form) into one or more
// vkGetDescriptorEXT writes into a DB-backed descriptor set's allocation. Supports scalar
// and small arrays for the common descriptor types this engine uses.
void VulkanDevice::WriteDescriptorBufferUpdate(DescriptorSetHandle handle, const DescriptorSetUpdate& update)
{
    const auto lookup = LookupDescriptorBufferSet(handle);
    if (!lookup.Found || !lookup.alloc.IsValid() || lookup.layout == VK_NULL_HANDLE)
        return;
    if (!m_FpGetDescriptorEXT || !m_FpGetDescriptorSetLayoutBindingOffsetEXT)
        return;
    // Local alias matches the former `entry` naming so the writeOne lambda below
    // reads naturally. lookup.alloc / .layout are now POD scalars — safe for CPU
    // writes here; purges are gated on the frame fence (see audit §7.1.2), so the
    // backing bytes can't be recycled mid-write.
    const auto& entry = lookup;

    // An update carrying no resources is a caller no-op, and the pool path
    // treats it as one. Return before the element floor below turns "nothing to
    // write" into one synthetic element and reports it as a dropped write.
    if (update.buffers.empty() && update.textures.empty() &&
        update.samplers.empty() && update.textureViews.empty() &&
        update.accelerationStructures.empty())
        return;

    const uint32_t count = static_cast<uint32_t>(std::max<size_t>({
        update.buffers.size(), update.textures.size(), update.samplers.size(),
        update.textureViews.size(), update.accelerationStructures.size(), size_t{1}}));

    // Helper: build a getInfo for element i of the update (handles arrays).
    // Returns nullptr when the element was written, otherwise a static reason
    // string naming why it was dropped (aggregated into one log after the loop).
    auto writeOne = [&](uint32_t element) -> const char*
    {
        VkDescriptorGetInfoEXT gi{};
        gi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
        VkDescriptorAddressInfoEXT addrInfo{};
        VkDescriptorImageInfo imageInfo{};
        size_t descSize = 0;
        // Resolved once per element: both the image view and the layout claim
        // below read the same view-tracking state, which concurrent registrations
        // mutate.
        const bool hasView =
            element < update.textureViews.size() && update.textureViews[element].IsValid();
        const TextureViewDescriptorInfo viewInfo =
            hasView ? ResolveTextureViewForDescriptor(update.textureViews[element])
                    : TextureViewDescriptorInfo{};
        // Sampled layout claim must be format-aware, matching the legacy
        // UpdateDescriptorSet path and the render graph's transitions
        // (ResourceState::DepthSampled): depth/stencil views are sampled in
        // DEPTH_STENCIL_READ_ONLY_OPTIMAL, everything else in
        // SHADER_READ_ONLY_OPTIMAL. A format-blind claim here mismatches the
        // actual image layout and trips VUID-vkCmdDraw-None-09600.
        auto sampledLayout = [&](uint32_t el) -> VkImageLayout
        {
            if (hasView)
            {
                if (viewInfo.sampledInGeneral)
                    return VK_IMAGE_LAYOUT_GENERAL;
                if (viewInfo.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
                    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            if (el < update.textures.size())
            {
                if (const VulkanTexture* tex = GetVulkanTexture(update.textures[el]))
                {
                    // GENERAL-resident sampled texture (View.DepthResolved): claim
                    // GENERAL so the descriptor matches the layout the RG pins it
                    // to for storage co-use — otherwise VUID-vkCmdDraw-None-09600.
                    if (tex->sampledInGeneralLayout)
                        return VK_IMAGE_LAYOUT_GENERAL;
                    const bool isDepthFmt =
                        (tex->format == VK_FORMAT_D16_UNORM ||
                         tex->format == VK_FORMAT_D32_SFLOAT ||
                         tex->format == VK_FORMAT_D24_UNORM_S8_UINT ||
                         tex->format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                         tex->format == VK_FORMAT_X8_D24_UNORM_PACK32);
                    if (isDepthFmt)
                        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                }
            }
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        };
        switch (update.type)
        {
            case DescriptorType::UniformBuffer:
            case DescriptorType::StorageBuffer:
            {
                if (element >= update.buffers.size()) return "no buffer supplied for this element";
                VkBuffer vkBuf = GetVkBuffer(update.buffers[element]);
                if (vkBuf == VK_NULL_HANDLE) return "buffer handle did not resolve";
                VkBufferDeviceAddressInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
                ai.buffer = vkBuf;
                VkDeviceAddress base = vkGetBufferDeviceAddress(m_Device, &ai);
                VkDeviceSize offset = (element < update.bufferOffsets.size()) ? static_cast<VkDeviceSize>(update.bufferOffsets[element]) : 0;
                VkDeviceSize range  = (element < update.bufferRanges.size())  ? static_cast<VkDeviceSize>(update.bufferRanges[element])  : VK_WHOLE_SIZE;
                // VkDescriptorAddressInfoEXT forbids VK_WHOLE_SIZE (VUID-08939). Resolve it here
                // to the remaining bytes of the source buffer so unspecified-range callers still
                // work on the DB path. Zero-range is explicit "bind no bytes" — leave as-is.
                if (range == VK_WHOLE_SIZE)
                {
                    const VulkanBuffer* vkBufInfo = GetVulkanBufferConst(update.buffers[element]);
                    const VkDeviceSize bufSize = vkBufInfo ? static_cast<VkDeviceSize>(vkBufInfo->size) : 0;
                    range = (bufSize > offset) ? (bufSize - offset) : 0;
                }
                addrInfo.sType   = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT;
                addrInfo.address = base + offset;
                addrInfo.range   = range;
                if (update.type == DescriptorType::UniformBuffer)
                {
                    gi.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    gi.data.pUniformBuffer = &addrInfo;
                    descSize = m_DescBufferDiag.uniformBufferDescriptorSize;
                }
                else
                {
                    gi.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    gi.data.pStorageBuffer = &addrInfo;
                    descSize = m_DescBufferDiag.storageBufferDescriptorSize;
                }
                break;
            }
            case DescriptorType::CombinedImageSampler:
            {
                VkImageView view = VK_NULL_HANDLE;
                if (hasView)
                    view = viewInfo.view;
                else if (element < update.textures.size())
                    view = GetVkImageView(update.textures[element]);
                VkSampler vkSampler = VK_NULL_HANDLE;
                if (element < update.samplers.size())
                    vkSampler = GetVkSampler(update.samplers[element], m_Device);
                if (view == VK_NULL_HANDLE && vkSampler == VK_NULL_HANDLE)
                    return "neither image view nor sampler resolved";
                if (view == VK_NULL_HANDLE) return "image view did not resolve";
                if (vkSampler == VK_NULL_HANDLE) return "sampler did not resolve";
                imageInfo.imageView   = view;
                imageInfo.imageLayout = sampledLayout(element);
                imageInfo.sampler     = vkSampler;
                gi.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gi.data.pCombinedImageSampler = &imageInfo;
                descSize = m_DescBufferDiag.combinedImageSamplerDescriptorSize;
                break;
            }
            case DescriptorType::Texture:
            {
                VkImageView view = VK_NULL_HANDLE;
                if (hasView)
                    view = viewInfo.view;
                else if (element < update.textures.size())
                    view = GetVkImageView(update.textures[element]);
                if (view == VK_NULL_HANDLE) return "image view did not resolve";
                imageInfo.imageView   = view;
                imageInfo.imageLayout = sampledLayout(element);
                gi.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                gi.data.pSampledImage = &imageInfo;
                descSize = m_DescBufferDiag.sampledImageDescriptorSize;
                break;
            }
            case DescriptorType::Sampler:
            {
                if (element >= update.samplers.size()) return "no sampler supplied for this element";
                // Keep the pointed-to handle alive through vkGetDescriptorEXT
                // below; a case-local sampler expires at the end of this block.
                imageInfo.sampler = GetVkSampler(update.samplers[element], m_Device);
                if (imageInfo.sampler == VK_NULL_HANDLE) return "sampler handle did not resolve";
                gi.type = VK_DESCRIPTOR_TYPE_SAMPLER;
                gi.data.pSampler = &imageInfo.sampler;
                descSize = m_DescBufferDiag.samplerDescriptorSize;
                break;
            }
            case DescriptorType::StorageImage:
            {
                VkImageView view = VK_NULL_HANDLE;
                if (hasView)
                    view = viewInfo.view;
                else if (element < update.textures.size())
                    view = GetVkImageView(update.textures[element]);
                if (view == VK_NULL_HANDLE) return "image view did not resolve";
                imageInfo.imageView   = view;
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                gi.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                gi.data.pStorageImage = &imageInfo;
                descSize = m_DescBufferDiag.storageImageDescriptorSize;
                break;
            }
            case DescriptorType::AccelerationStructure:
            {
                // The descriptor-buffer form takes the TLAS DEVICE ADDRESS, not
                // the VkAccelerationStructureKHR the pool path chains through
                // pNext (VkDescriptorGetInfoEXT::data.accelerationStructure).
                if (!m_AccelerationStructures) return "device has no acceleration-structure backend";
                if (element >= update.accelerationStructures.size())
                    return "no acceleration structure supplied for this element";
                const uint64_t address =
                    m_AccelerationStructures->GetTlasDeviceAddress(update.accelerationStructures[element]);
                if (address == 0) return "TLAS slot has no built acceleration structure yet";
                gi.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                gi.data.accelerationStructure = address;
                descSize = m_DescBufferDiag.accelerationStructureDescriptorSize;
                break;
            }
            default:
                return "descriptor type not supported on the descriptor-buffer path";
        }
        if (descSize == 0) return "driver reports a zero descriptor size for this type";

        // Compute per-binding base offset and step by arrayElement * descSize.
        //
        // Concurrent writers to DIFFERENT array elements are safe here, which is
        // what lets bindless registration run on ECS extraction workers. Every
        // input to the write is either immutable after device init
        // (m_FpGetDescriptor*, m_DescBufferDiag sizes), read under its own lock
        // (the set lookup's shared_lock, the view resolve above), or a stack
        // local (gi / addrInfo / imageInfo / descSize / bindingOffset / dst) —
        // there is no shared cursor or scratch buffer. The destinations are
        // [bindingOffset + i*descSize, +descSize) for distinct i, which are
        // disjoint by construction, and the memory is persistently mapped, so
        // there is no map/unmap or flush to serialize either.
        //
        // Not covered by that argument, and still the caller's problem: writing
        // an element the GPU may still be reading. Descriptor-buffer writes are
        // unsynchronized against in-flight command buffers by design, so a slot
        // must be virgin or parked out (BindlessResourceManager's generational
        // free) before it is written.
        VkDeviceSize bindingOffset = 0;
        m_FpGetDescriptorSetLayoutBindingOffsetEXT(m_Device, entry.layout, update.binding, &bindingOffset);
        const VkDeviceSize elementOffset = bindingOffset
            + static_cast<VkDeviceSize>(update.arrayElement + element) * descSize;
        if (elementOffset + descSize > entry.alloc.Size)
            return "write would land past the end of the set's allocation";
        uint8_t* dst = static_cast<uint8_t*>(entry.alloc.HostMapped) + elementOffset;
        WatchCheckDescriptorGetInfo(gi, update.binding, update.arrayElement + element,
                                    "WriteDescriptorBufferUpdate");
        m_FpGetDescriptorEXT(m_Device, &gi, descSize, dst);
        return nullptr;
    };

    // A skipped element is louder here than on the pool path, because descriptor-
    // buffer storage is bump-allocated and never zeroed (VulkanDescriptorBufferPool:
    // BeginFrameReset only rewinds a cursor). The bytes a skipped write leaves
    // behind are therefore whatever the previous owner of that range wrote — a
    // live-looking descriptor the GPU will bind and dereference — not a null.
    uint32_t droppedCount = 0;
    uint32_t firstDroppedElement = 0;
    const char* firstDropReason = nullptr;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (const char* reason = writeOne(i))
        {
            if (droppedCount++ == 0)
            {
                firstDroppedElement = i;
                firstDropReason = reason;
            }
        }
    }
    if (droppedCount > 0)
    {
        // One line per update, not per element: an N-element bindless array whose
        // handles all died would otherwise emit N copies of the same finding and
        // bury the rest of the device-loss log.
        Logger::Log::Error(
            "VulkanDevice: dropped {} of {} descriptor-buffer write(s) (set='{}', binding={}, "
            "type={}, first bad arrayElement={}): {} — a handle outlived its device (stale after "
            "a device rebuild?). Descriptor-buffer memory is recycled and never zeroed, so the "
            "skipped slot keeps the previous owner's descriptor and the GPU will dereference it",
            droppedCount,
            count,
            DescriptorBufferSetDebugName(handle),
            update.binding,
            static_cast<int>(update.type),
            update.arrayElement + firstDroppedElement,
            firstDropReason);
    }
}

bool VulkanDevice::WriteCombinedImageSamplerDescriptor(const DescriptorBufferAllocation& alloc,
                                                       VkDescriptorSetLayout layout,
                                                       uint32_t binding,
                                                       TextureHandle texture,
                                                       SamplerHandle sampler)
{
    VkImageView view = GetVkImageView(texture);
    VkSampler   vkSampler = GetVkSampler(sampler, m_Device);
    if (view == VK_NULL_HANDLE || vkSampler == VK_NULL_HANDLE)
        return false;

    // GENERAL-resident sampled texture (View.DepthResolved) claims GENERAL to
    // match the RG's SampledInGeneralLayout constraint; everything else uses SHADER_READ_ONLY.
    const VulkanTexture* texInfo = GetVulkanTexture(texture);
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler     = vkSampler;
    imageInfo.imageView   = view;
    imageInfo.imageLayout = (texInfo && texInfo->sampledInGeneralLayout)
                                ? VK_IMAGE_LAYOUT_GENERAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorGetInfoEXT gi{};
    gi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
    gi.type  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    gi.data.pCombinedImageSampler = &imageInfo;

    WriteDescriptorAt(alloc, layout, binding, gi, GetCombinedImageSamplerDescriptorSize());
    return true;
}

bool VulkanDevice::AnySetLayoutIsDescriptorBufferEligible(const std::vector<VkDescriptorSetLayout>& setLayouts)
{
    if (!IsDescriptorBufferEnabled())
        return false;
    std::lock_guard<std::mutex> lock(m_LayoutCacheMutex);
    for (auto l : setLayouts)
    {
        auto itKey = m_SetLayoutReverse.find(l);
        if (itKey == m_SetLayoutReverse.end())
            continue;
        auto itEntry = m_SetLayoutCache.find(itKey->second);
        if (itEntry != m_SetLayoutCache.end() && itEntry->second.descriptorBufferEligible)
            return true;
    }
    return false;
}

void VulkanDevice::ReleaseDescriptorSetLayoutCached(VkDescriptorSetLayout layout)
{
    if (layout == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::mutex> lock(m_LayoutCacheMutex);
    auto itRev = m_SetLayoutReverse.find(layout);
    if (itRev != m_SetLayoutReverse.end())
    {
        uint64_t key = itRev->second;
        auto itFwd = m_SetLayoutCache.find(key);
        if (itFwd != m_SetLayoutCache.end() && itFwd->second.layout == layout)
        {
            if (itFwd->second.refCount > 0)
            {
                itFwd->second.refCount--;
                if (itFwd->second.refCount == 0)
                {
                    vkDestroyDescriptorSetLayout(m_Device, layout, nullptr);
                    m_SetLayoutReverse.erase(itRev);
                    m_SetLayoutCache.erase(key);
                }
            }
            return;
        }
    }
    // Not cached or mismatch - destroy directly as fallback
    vkDestroyDescriptorSetLayout(m_Device, layout, nullptr);
}

void VulkanDevice::ReleasePipelineLayoutCached(VkPipelineLayout layout)
{
    if (layout == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::mutex> lock(m_LayoutCacheMutex);
    auto itRev = m_PipelineLayoutReverse.find(layout);
    if (itRev != m_PipelineLayoutReverse.end())
    {
        uint64_t key = itRev->second;
        auto itFwd = m_PipelineLayoutCache.find(key);
        if (itFwd != m_PipelineLayoutCache.end() && itFwd->second.layout == layout)
        {
            if (itFwd->second.refCount > 0)
            {
                itFwd->second.refCount--;
                if (itFwd->second.refCount == 0)
                {
                    vkDestroyPipelineLayout(m_Device, layout, nullptr);
                    m_PipelineLayoutReverse.erase(itRev);
                    m_PipelineLayoutCache.erase(key);
                }
            }
            return;
        }
    }
    // Not cached or mismatch - destroy directly as fallback
    vkDestroyPipelineLayout(m_Device, layout, nullptr);
}

IDevice::DebugBindCounters VulkanDevice::DebugGetBindCounters() const
{
    DebugBindCounters c{};
    c.pipelineBindsGraphics = m_DebugPipelineBindGraphics;
    c.pipelineBindsCompute = m_DebugPipelineBindCompute;
    for (int i = 0; i < 8; ++i)
    {
        c.descriptorBindsGraphics[i] = m_DebugDescBindGraphics[i];
        c.descriptorBindsCompute[i] = m_DebugDescBindCompute[i];
    }
    return c;
}

void VulkanDevice::DebugResetBindCounters()
{
    m_DebugPipelineBindGraphics = 0;
    m_DebugPipelineBindCompute = 0;
    for (int i = 0; i < 8; ++i)
    {
        m_DebugDescBindGraphics[i] = 0;
        m_DebugDescBindCompute[i] = 0;
    }
}

ValidationStats VulkanDevice::GetValidationStats() const
{
    return ValidationStatsStore::Get().Snapshot(m_DebugLayerEnabled);
}

void VulkanDevice::ResetValidationStats()
{
    ValidationStatsStore::Get().Reset();
}

GpuToolingReport VulkanDevice::GetGpuToolingReport() const
{
    GpuToolingReport report;
    // The resolved entry points, not the extension flag: an enabled extension
    // whose commands failed to resolve still leaves labels uncallable, and the
    // callable fact is the one a preflight check is asking about.
    report.DebugLabelsAvailable   = m_DebugUtilsLabelFns.Available();
    // What the engine asked for, not what the loader ended up injecting: a layer
    // forced in from outside through VK_INSTANCE_LAYERS reads false here and
    // shows up in AttachedTools instead.
    report.ValidationLayerEnabled = m_DebugLayerEnabled;
    report.AttachedTools = QueryAttachedTools(m_Instance, m_PhysicalDevice, m_InstanceApiVersion, report.ToolingQueryAvailable);
    return report;
}

void VulkanDevice::SetVkObjectName(VkObjectType objectType, uint64_t objectHandle, const char* name) const
{
    if (!m_SetVkObjectNameFn || !name || name[0] == '\0' || objectHandle == 0)
        return;
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = objectType;
    info.objectHandle = objectHandle;
    info.pObjectName = name;
    m_SetVkObjectNameFn(m_Device, &info);
}

void VulkanDevice::DebugSetDescriptorCaptureEnabled(bool enabled, const std::string& passFilter)
{
    SetDescriptorCaptureEnabled(enabled, passFilter);
}

bool VulkanDevice::DebugIsDescriptorCaptureEnabled() const
{
    return IsDescriptorCaptureEnabled();
}

void VulkanDevice::DebugClearDescriptorCaptures()
{
    ClearDescriptorCaptures();
}

std::vector<IDevice::DebugDescriptorCapture> VulkanDevice::DebugGetDescriptorCaptures() const
{
    const auto& captures = GetDescriptorCaptures();
    std::vector<DebugDescriptorCapture> result;
    result.reserve(captures.size());
    for (const auto& c : captures)
    {
        DebugDescriptorCapture dc;
        dc.PassName = c.PassName;
        dc.DrawIndex = c.DrawIndex;
        dc.DescriptorSetHandle = c.DescriptorSetHandle;
        dc.SetIndex = c.SetIndex;
        dc.Bindings.reserve(c.Bindings.size());
        for (const auto& b : c.Bindings)
        {
            DebugDescriptorBinding db;
            db.Binding = b.Binding;
            db.ResourceType = b.ResourceType;
            db.ResourceHandle = b.ResourceHandle;
            db.DebugName = b.DebugName;
            dc.Bindings.push_back(std::move(db));
        }
        result.push_back(std::move(dc));
    }
    return result;
}

} // namespace Rendering
} // namespace GameEngine
