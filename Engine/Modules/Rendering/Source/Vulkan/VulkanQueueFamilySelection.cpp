#include "VulkanQueueFamilySelection.h"

#include <bit>
#include <cstdio>

namespace GameEngine
{
namespace Rendering
{
namespace
{

// Fixed-function media/vision engines. They advertise TRANSFER because copying is
// how a codec is fed, not because they are general copy engines — never schedule
// engine uploads there.
constexpr VkQueueFlags kSpecializedEngineFlags =
    VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_VIDEO_ENCODE_BIT_KHR | VK_QUEUE_OPTICAL_FLOW_BIT_NV;

struct NamedQueueFlag
{
    VkQueueFlags Bit;
    const char* Name;
};

constexpr NamedQueueFlag kNamedQueueFlags[] = {
    {VK_QUEUE_GRAPHICS_BIT, "GRAPHICS"},
    {VK_QUEUE_COMPUTE_BIT, "COMPUTE"},
    {VK_QUEUE_TRANSFER_BIT, "TRANSFER"},
    {VK_QUEUE_SPARSE_BINDING_BIT, "SPARSE"},
    {VK_QUEUE_PROTECTED_BIT, "PROTECTED"},
    {VK_QUEUE_VIDEO_DECODE_BIT_KHR, "VIDEO_DECODE"},
    {VK_QUEUE_VIDEO_ENCODE_BIT_KHR, "VIDEO_ENCODE"},
    {VK_QUEUE_OPTICAL_FLOW_BIT_NV, "OPTICAL_FLOW"},
};

// Lowest-index family advertising every bit of `required`, no GRAPHICS, and no
// fixed-function engine, minimising total capability bits.
uint32_t SelectMostDedicated(std::span<const VkQueueFamilyProperties> families, VkQueueFlags required)
{
    uint32_t best = QueueFamilySelection::kInvalidQueueFamily;
    int bestBitCount = 0;

    for (uint32_t i = 0; i < families.size(); ++i)
    {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & required) != required)
            continue;
        if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0)
            continue;
        if ((flags & kSpecializedEngineFlags) != 0)
            continue;

        const int bitCount = std::popcount(static_cast<uint32_t>(flags));
        if (best == QueueFamilySelection::kInvalidQueueFamily || bitCount < bestBitCount)
        {
            best = i;
            bestBitCount = bitCount;
        }
    }

    return best;
}

} // namespace

QueueFamilySelection SelectQueueFamilies(std::span<const VkQueueFamilyProperties> families)
{
    QueueFamilySelection selection;

    for (uint32_t i = 0; i < families.size(); ++i)
    {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            selection.Graphics = i;
            break;
        }
    }

    selection.Compute = SelectMostDedicated(families, VK_QUEUE_COMPUTE_BIT);
    selection.Transfer = SelectMostDedicated(families, VK_QUEUE_TRANSFER_BIT);

    return selection;
}

std::string DescribeQueueFlags(VkQueueFlags flags)
{
    std::string description;
    VkQueueFlags remaining = flags;

    for (const NamedQueueFlag& named : kNamedQueueFlags)
    {
        if ((flags & named.Bit) == 0)
            continue;
        if (!description.empty())
            description += '|';
        description += named.Name;
        remaining &= ~named.Bit;
    }

    if (remaining != 0)
    {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%X", static_cast<unsigned>(remaining));
        if (!description.empty())
            description += '|';
        description += hex;
    }

    return description.empty() ? std::string("NONE") : description;
}

std::string DescribeQueueFamilySelection(const QueueFamilySelection& selection,
                                         std::span<const VkQueueFamilyProperties> families)
{
    auto describeRole = [&families](const char* role, uint32_t index)
    {
        std::string text = role;
        if (index == QueueFamilySelection::kInvalidQueueFamily || index >= families.size())
            return text + "=none";
        text += '=';
        text += std::to_string(index);
        text += " [" + DescribeQueueFlags(families[index].queueFlags) + ']';
        return text;
    };

    return describeRole("graphics", selection.Graphics) + ", " + describeRole("compute", selection.Compute) + ", "
           + describeRole("transfer", selection.Transfer);
}

} // namespace Rendering
} // namespace GameEngine
