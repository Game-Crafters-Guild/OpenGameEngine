/**
 * @file VulkanToolingInfo.h
 * @brief Which graphics tools are attached to a device, via VK_EXT_tooling_info.
 */

#pragma once

#include "Rendering/Core/Device.h"

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace GameEngine {
namespace Rendering {

    /// Upper bound on tool entries accepted from the runtime. The count is
    /// driver/layer-reported and unvalidated; sizing an allocation directly from
    /// it is the class that produced a measured multi-second stall and bad_alloc
    /// once before (see kMaxFaultRecords in VulkanDeviceFault.h). Real tool
    /// stacks are single digits.
    inline constexpr uint32_t kMaxAttachedTools = 64;

    /// Spelled-out purposes for one tool. A bit these headers do not know (a tool
    /// built against a newer SDK) renders as its numeric value rather than being
    /// dropped, so an unfamiliar tool still reads as doing something. No bits at
    /// all reads as "NONE", never as an empty string.
    std::string DescribeToolPurposes(VkToolPurposeFlags purposes);

    /**
     * @brief Two-call retrieval of every tool the runtime reports on this device.
     *
     * `outQueryAvailable` is a different fact from an empty return: false says no
     * answer was obtainable, while an empty vector with it true says the runtime
     * was asked and reports nothing attached. Collapsing the two would report a
     * clean device for one that was never queried — and "was a capture layer in
     * the way" is unanswerable after the run.
     *
     * Preconditions are established per the spec, not per what the loader happens
     * to resolve: the core command needs BOTH the instance and the physical
     * device at 1.3+ (hence `instanceApiVersion`, the version the instance was
     * created with), and the EXT alias is physical-device-level functionality of
     * the VK_EXT_tooling_info DEVICE extension — legal once the physical device
     * advertises it, no enablement required or possible (no VkDevice exists when
     * this first runs). A loader-resolved pointer alone establishes neither.
     */
    std::vector<AttachedGraphicsTool> QueryAttachedTools(VkInstance instance,
                                                         VkPhysicalDevice physicalDevice,
                                                         uint32_t instanceApiVersion,
                                                         bool& outQueryAvailable);

    /**
     * @brief One-line rendering of a tool list for a log. Pure; needs no device.
     *
     * Carries name, version and purposes only. Each tool's free-text description
     * is a sentence of driver prose that would swamp a startup line; the debug
     * server reports it in full.
     */
    std::string FormatAttachedTools(const std::vector<AttachedGraphicsTool>& tools, bool queryAvailable);

} // namespace Rendering
} // namespace GameEngine
