#include "VulkanToolingInfo.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace GameEngine {
namespace Rendering {

namespace
{
// Fixed-size scratch for one formatted numeric field.
constexpr size_t kFormatScratch = 32;

/// Copy a driver-supplied fixed-size text field. Treats the array as bounded
/// rather than NUL-terminated, so a runtime that fills every byte cannot walk
/// the reader off the end.
std::string CopyBoundedField(const char* field, size_t capacity)
{
    if (field == nullptr || capacity == 0)
    {
        return {};
    }
    size_t length = 0;
    while (length < capacity && field[length] != '\0')
    {
        ++length;
    }
    return std::string(field, length);
}
} // namespace

std::string DescribeToolPurposes(VkToolPurposeFlags purposes)
{
    if (purposes == 0)
    {
        return "NONE";
    }

    struct PurposeName
    {
        VkToolPurposeFlags Bit;
        const char*        Name;
    };
    static constexpr PurposeName kPurposeNames[] = {
        {VK_TOOL_PURPOSE_VALIDATION_BIT,          "VALIDATION"},
        {VK_TOOL_PURPOSE_PROFILING_BIT,           "PROFILING"},
        {VK_TOOL_PURPOSE_TRACING_BIT,             "TRACING"},
        {VK_TOOL_PURPOSE_ADDITIONAL_FEATURES_BIT, "ADDITIONAL_FEATURES"},
        {VK_TOOL_PURPOSE_MODIFYING_FEATURES_BIT,  "MODIFYING_FEATURES"},
        {VK_TOOL_PURPOSE_DEBUG_REPORTING_BIT_EXT, "DEBUG_REPORTING"},
        {VK_TOOL_PURPOSE_DEBUG_MARKERS_BIT_EXT,   "DEBUG_MARKERS"},
    };

    std::string        out;
    VkToolPurposeFlags unnamed = purposes;
    for (const PurposeName& purpose : kPurposeNames)
    {
        if ((purposes & purpose.Bit) == 0)
        {
            continue;
        }
        if (!out.empty())
        {
            out += "|";
        }
        out += purpose.Name;
        unnamed &= ~purpose.Bit;
    }

    // Whatever is left is a purpose this SDK cannot name. Printed numerically
    // because a tool that modifies device behaviour must not read as harmless
    // just because the header predates it.
    if (unnamed != 0)
    {
        if (!out.empty())
        {
            out += "|";
        }
        char buffer[kFormatScratch];
        std::snprintf(buffer, sizeof(buffer), "UNKNOWN(0x%x)", static_cast<unsigned>(unnamed));
        out += buffer;
    }
    return out;
}

std::vector<AttachedGraphicsTool> QueryAttachedTools(VkInstance instance,
                                                     VkPhysicalDevice physicalDevice,
                                                     uint32_t instanceApiVersion,
                                                     bool& outQueryAvailable)
{
    outQueryAvailable = false;
    if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
    {
        return {};
    }

    // Establish which spelling is legal before resolving anything: the loader
    // returns pointers for commands the instance never earned, so a non-null
    // resolve proves nothing. Core needs both sides at 1.3.
    VkPhysicalDeviceProperties physicalProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalProperties);
    const bool coreLegal = instanceApiVersion >= VK_API_VERSION_1_3 &&
                           physicalProperties.apiVersion >= VK_API_VERSION_1_3;

    // The EXT alias is physical-device-level functionality of a DEVICE
    // extension: legal once the physical device advertises it, no enablement
    // required (no VkDevice exists yet). The advertisement probe caps its own
    // driver-reported count for the same reason kMaxAttachedTools exists; a
    // truncated probe treats the extension as absent, which fails toward
    // "query unavailable", never toward a wrong answer.
    bool extLegal = false;
    if (!coreLegal)
    {
        constexpr uint32_t kMaxProbedDeviceExtensions = 4096;
        uint32_t extCount = 0;
        if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr) == VK_SUCCESS &&
            extCount > 0)
        {
            extCount = std::min(extCount, kMaxProbedDeviceExtensions);
            std::vector<VkExtensionProperties> extensions(extCount);
            if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, extensions.data()) ==
                VK_SUCCESS)
            {
                for (const VkExtensionProperties& ext : extensions)
                {
                    if (std::strcmp(ext.extensionName, VK_EXT_TOOLING_INFO_EXTENSION_NAME) == 0)
                    {
                        extLegal = true;
                        break;
                    }
                }
            }
        }
    }

    if (!coreLegal && !extLegal)
    {
        return {};
    }

    auto getToolProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceToolProperties>(vkGetInstanceProcAddr(
        instance, coreLegal ? "vkGetPhysicalDeviceToolProperties" : "vkGetPhysicalDeviceToolPropertiesEXT"));
    if (getToolProperties == nullptr)
    {
        return {};
    }

    uint32_t count = 0;
    if (getToolProperties(physicalDevice, &count, nullptr) != VK_SUCCESS)
    {
        // A failed sizing call leaves no answer at all. Reporting availability
        // here would turn "could not ask" into "nothing attached".
        return {};
    }
    outQueryAvailable = true;
    if (count == 0)
    {
        return {};
    }

    // The count is runtime-reported and unvalidated; never size an allocation
    // straight from it (kMaxAttachedTools documents the incident this guards).
    count = std::min(count, kMaxAttachedTools);
    std::vector<VkPhysicalDeviceToolProperties> properties(count);
    for (VkPhysicalDeviceToolProperties& entry : properties)
    {
        entry.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES;
    }

    // VK_INCOMPLETE is a success code: a tool that attached between the two calls
    // means the array holds fewer than the runtime now has, not that the entries
    // it does hold are unusable.
    const VkResult fillResult = getToolProperties(physicalDevice, &count, properties.data());
    if (fillResult != VK_SUCCESS && fillResult != VK_INCOMPLETE)
    {
        outQueryAvailable = false;
        return {};
    }
    // The second call reports what it actually wrote; never read past that.
    properties.resize(std::min(static_cast<size_t>(count), properties.size()));

    std::vector<AttachedGraphicsTool> tools;
    tools.reserve(properties.size());
    for (const VkPhysicalDeviceToolProperties& entry : properties)
    {
        AttachedGraphicsTool tool;
        tool.Name        = CopyBoundedField(entry.name, VK_MAX_EXTENSION_NAME_SIZE);
        tool.Version     = CopyBoundedField(entry.version, VK_MAX_EXTENSION_NAME_SIZE);
        tool.PurposeBits = static_cast<uint32_t>(entry.purposes);
        tool.Purposes    = DescribeToolPurposes(entry.purposes);
        tool.Description = CopyBoundedField(entry.description, VK_MAX_DESCRIPTION_SIZE);
        tool.Layer       = CopyBoundedField(entry.layer, VK_MAX_EXTENSION_NAME_SIZE);
        tools.push_back(std::move(tool));
    }
    return tools;
}

std::string FormatAttachedTools(const std::vector<AttachedGraphicsTool>& tools, bool queryAvailable)
{
    // Two distinct strings on purpose: a reader who cannot tell "asked, nothing
    // there" from "never asked" learns nothing from either.
    if (!queryAvailable)
    {
        return "<tooling query unavailable>";
    }
    if (tools.empty())
    {
        return "<none attached>";
    }

    char countBuffer[kFormatScratch];
    std::snprintf(countBuffer, sizeof(countBuffer), "%zu", tools.size());
    std::string out = countBuffer;
    out += " attached:";
    for (size_t i = 0; i < tools.size(); ++i)
    {
        const AttachedGraphicsTool& tool = tools[i];
        char indexBuffer[kFormatScratch];
        std::snprintf(indexBuffer, sizeof(indexBuffer), " [%zu] '", i);
        out += indexBuffer;
        out += tool.Name;
        out += "'";
        // Labelled, never prefixed: the version is free-form driver text and
        // some tools already spell their own "v" (RenderDoc reports "v1.45"),
        // so synthesizing one renders "vv1.45".
        if (!tool.Version.empty())
        {
            out += " version=" + tool.Version;
        }
        out += " purposes=" + tool.Purposes;
        if (i + 1 < tools.size())
        {
            out += ";";
        }
    }
    return out;
}

} // namespace Rendering
} // namespace GameEngine
