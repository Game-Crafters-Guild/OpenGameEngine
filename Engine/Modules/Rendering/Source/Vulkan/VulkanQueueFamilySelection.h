#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string>

namespace GameEngine
{
namespace Rendering
{

// Queue-family indices resolved for one physical device. kInvalidQueueFamily means
// the device exposes no family dedicated to that role; callers substitute the
// graphics family, which always supports transfer operations and in practice also
// supports compute.
struct QueueFamilySelection
{
    static constexpr uint32_t kInvalidQueueFamily = VK_QUEUE_FAMILY_IGNORED;

    uint32_t Graphics = kInvalidQueueFamily;
    uint32_t Compute = kInvalidQueueFamily;
    uint32_t Transfer = kInvalidQueueFamily;
};

/// Resolves the graphics, async-compute and async-transfer families for a device.
///
/// Graphics is the first family advertising GRAPHICS.
///
/// Compute and transfer pick the *most dedicated* family: among families that
/// advertise the required capability and do not advertise GRAPHICS, the one with
/// the fewest capability bits wins, ties breaking on the lowest index. Fewest bits
/// means least shared silicon, so a discrete GPU's copy engine (TRANSFER|SPARSE) is
/// preferred over its compute engine (COMPUTE|TRANSFER|SPARSE), and either is
/// preferred over a family that merely carries TRANSFER as a side effect.
///
/// Fixed-function media and vision engines (VIDEO_DECODE, VIDEO_ENCODE,
/// OPTICAL_FLOW) are excluded outright rather than merely deprioritised: they
/// advertise TRANSFER only to feed their codec, and the general-purpose graphics
/// queue is a better fallback for general uploads than a video engine is.
///
/// The policy ignores minImageTransferGranularity, and may only keep doing so while
/// the transfer queue carries buffer copies alone. That granularity constrains the
/// offsets and extents of IMAGE transfers, and a dedicated copy engine is entitled
/// to advertise a coarse one — or (0,0,0), meaning whole-mip-level copies only.
/// Today the sole transfer-queue command list records a single vkCmdCopyBuffer
/// (VulkanDevice::UpdateBuffer's device-local branch); every image copy in the tree
/// is recorded on a graphics-typed list. Recording an image transfer on a transfer
/// list means honouring the chosen family's granularity, or selecting on it here.
///
/// Pure and free of Vulkan handles so the policy is unit-testable without a GPU
/// (see VulkanQueueFamilySelectionTests).
QueueFamilySelection SelectQueueFamilies(std::span<const VkQueueFamilyProperties> families);

/// Renders one family's capabilities as "GRAPHICS|COMPUTE|TRANSFER|SPARSE", for logs.
/// Bits with no name are appended as hex so an unrecognised capability still shows up.
std::string DescribeQueueFlags(VkQueueFlags flags);

/// Renders a selection as "graphics=0 [GRAPHICS|COMPUTE|TRANSFER|SPARSE], compute=..., transfer=...",
/// so a device reports which engine it actually resolved each role to.
std::string DescribeQueueFamilySelection(const QueueFamilySelection& selection,
                                         std::span<const VkQueueFamilyProperties> families);

} // namespace Rendering
} // namespace GameEngine
