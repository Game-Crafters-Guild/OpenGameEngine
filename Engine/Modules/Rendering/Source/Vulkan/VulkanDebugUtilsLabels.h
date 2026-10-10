/**
 * @file VulkanDebugUtilsLabels.h
 * @brief GPU debug labels and object names via VK_EXT_debug_utils.
 */

#pragma once

#include <vulkan/vulkan.h>

namespace GameEngine {
namespace Rendering {

    /// Parse the GE_VK_NO_DEBUG_UTILS escape hatch: truthy drops the extension for
    /// devices on non-debug instances. Same leading-character idiom as the validation toggles, with
    /// the sense inverted because the extension is on by default — labels and
    /// object names are what make a capture or a driver crash report readable, and
    /// nothing pays for them until a tool is listening.
    ///
    /// A debug-layer instance enables the extension whatever this says: the debug
    /// messenger is a VK_EXT_debug_utils object, so dropping it there would silence
    /// validation instead of the labels.
    inline bool ParseDebugUtilsDisabled(const char* envValue) noexcept
    {
        return envValue != nullptr && envValue[0] != '\0' && envValue[0] != '0' &&
               envValue[0] != 'f' && envValue[0] != 'F';
    }

    /**
     * @brief Cached VK_EXT_debug_utils command entry points.
     *
     * Available() requires all three: a half-resolved set opens labels it cannot
     * close, and an unbalanced label tree misleads every downstream tool that
     * reads it.
     */
    struct DebugUtilsLabelFns
    {
        PFN_vkCmdBeginDebugUtilsLabelEXT  Begin  = nullptr;
        PFN_vkCmdEndDebugUtilsLabelEXT    End    = nullptr;
        PFN_vkCmdInsertDebugUtilsLabelEXT Insert = nullptr;

        bool Available() const noexcept { return Begin && End && Insert; }
    };

} // namespace Rendering
} // namespace GameEngine
