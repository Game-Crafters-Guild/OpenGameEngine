/**
 * @file VulkanSharedInstance.h
 * @brief The process's VkInstances, and the contract that decides when two
 *        devices may share one.
 */

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

/**
 * @brief Everything a device asks of vkCreateInstance and of the debug messenger
 *        created alongside it.
 *
 * Instance-level configuration is settled by whichever device creates the
 * instance, and a device that shares one inherits that configuration without
 * being able to change it. So two devices may share an instance only when their
 * instances would have been created identically — otherwise a device silently
 * runs without what it asked for, and a debug-requesting device landed on a
 * non-debug instance reports a clean run it never validated.
 *
 * Every member is an input to the instance or to its debug messenger — both are
 * created once and inherited as they stand. Device-level configuration
 * (physical-device selection, enabled device features, swapchain and HDR policy)
 * is not part of instance identity and stays on the device.
 *
 * The members hold what was REQUESTED, never what the platform granted. A request
 * the loader cannot satisfy is downgraded identically for every device that makes
 * it, so keying on the request is what keeps the lookup and the publish agreeing
 * on one key; what survived the downgrade travels in SharedInstanceState.
 */
struct SharedInstanceKey
{
    /// appInfo.pApplicationName. Drivers key per-application profiles off it and
    /// captures and crash reports are attributed by it, so a mismatch is a real
    /// difference rather than a label.
    std::string ApplicationName;
    /// appInfo.applicationVersion.
    uint32_t ApplicationVersion = 0;
    /// VK_LAYER_KHRONOS_validation and the debug messenger. Not VK_EXT_debug_utils,
    /// which is enabled on its own terms in every config — but DebugLayer does
    /// decide whether the GE_VK_NO_DEBUG_UTILS escape hatch can drop that
    /// extension (the messenger is built on it), so a debug and a non-debug
    /// instance can still differ in debug-utils state, and keying DebugLayer is
    /// what keeps that difference per-key rather than per-device.
    bool DebugLayer = false;
    /// GE_VK_VALIDATION_VERBOSE: whether the messenger also reports the INFO and
    /// VERBOSE severities. The messenger is created once, with the creating device's
    /// severity mask, so a device that asked for verbose output and inherited a
    /// terse messenger would silently get none. Only the environment request is
    /// keyed — the build-configuration part of the mask is identical for every
    /// device in the process and so cannot tell two of them apart.
    bool VerboseMessenger = false;
    /// WSI instance extensions (VK_KHR_surface plus the platform surface
    /// extension). No surface can be created from an instance that lacks them.
    bool SurfaceExtensions = false;
    /// VK_EXT_layer_settings values chained into vkCreateInstance. Each is read
    /// from the environment per device, so two devices in one process can differ.
    bool SyncValidation = false;
    bool GpuAssistedValidation = false;
    bool GpuAvShaderInstrumentation = false;

    bool operator==(const SharedInstanceKey& other) const = default;
};

/// Instance-scoped facts the creating device resolves once and every device
/// sharing that instance reads instead of re-deriving.
struct SharedInstanceState
{
    VkInstance Instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
    /// Whether the debug layer is live, as opposed to requested: a debug request
    /// on a machine with no validation layer installed is downgraded at instance
    /// creation, and a sharing device that assumed otherwise would report
    /// validation it is not getting.
    bool DebugLayerActive = false;
    /// Whether VK_EXT_debug_utils was granted, as opposed to wanted: a loader that
    /// does not advertise it leaves the label and object-name entry points
    /// unresolvable, and a sharing device that assumed otherwise would resolve
    /// entry points for an extension its instance never enabled.
    bool DebugUtilsExtEnabled = false;
    /// The apiVersion the instance was created with. A sharing device judges
    /// core physical-device-query legality against this, not its own request.
    uint32_t InstanceApiVersion = 0;
    bool SwapchainColorSpaceExtEnabled = false;
    bool GpuAvChained = false;
    bool GpuAvShaderInstrumentationRequested = false;
};

/**
 * @brief The process's VkInstances, one per distinct SharedInstanceKey, each
 *        reference-counted by the devices using it.
 *
 * Vulkan instances are independent objects sharing no global state, so a device
 * whose configuration matches no live instance gets its own rather than being
 * refused or quietly degraded. Each instance is destroyed by whichever device
 * drops the last reference to it.
 *
 * Acquire and Publish are separate calls because vkCreateInstance happens between
 * them. Two threads creating devices with the same key can therefore both miss and
 * both create; the outcome is a redundant instance, each entry still correctly
 * reference-counted by the devices holding it — never a leak and never a double
 * destroy.
 */
class SharedInstanceRegistry
{
public:
    /// The live instance for @p key with one more reference taken, or nullopt when
    /// the caller must create an instance and Publish it under @p key.
    std::optional<SharedInstanceState> Acquire(const SharedInstanceKey& key);

    /// Registers a freshly created instance, with the publishing device's own
    /// reference already counted.
    void Publish(const SharedInstanceKey& key, const SharedInstanceState& state);

    /// Drops one reference to @p instance. Returns the state whose messenger and
    /// instance the caller must now destroy, or nullopt while other devices still
    /// hold references.
    std::optional<SharedInstanceState> Release(VkInstance instance);

    static SharedInstanceRegistry& Get();

private:
    /// Entries are erased the moment their count reaches zero, so a registered
    /// entry always carries at least one reference.
    struct Entry
    {
        SharedInstanceKey Key;
        SharedInstanceState State;
        uint32_t RefCount = 0;
    };

    std::mutex m_Mutex;
    std::vector<Entry> m_Entries;
};

} // namespace Rendering
} // namespace GameEngine
