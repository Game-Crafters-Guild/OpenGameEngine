#include "Source/Vulkan/VulkanQueueFamilySelection.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

constexpr uint32_t kInvalid = QueueFamilySelection::kInvalidQueueFamily;

VkQueueFamilyProperties Family(VkQueueFlags flags, uint32_t queueCount = 1)
{
    VkQueueFamilyProperties props{};
    props.queueFlags = flags;
    props.queueCount = queueCount;
    props.timestampValidBits = 64;
    props.minImageTransferGranularity = {1, 1, 1};
    return props;
}

// vulkaninfo, NVIDIA RTX 5080, driver 580.x. Family 5 is the optical-flow engine:
// it advertises TRANSFER, which is why a last-match-wins scan lands on it.
std::vector<VkQueueFamilyProperties> Rtx5080Layout()
{
    return {
        Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT, 16),
        Family(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT, 2),
        Family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT, 8),
        Family(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_VIDEO_DECODE_BIT_KHR, 2),
        Family(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_VIDEO_ENCODE_BIT_KHR),
        Family(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_OPTICAL_FLOW_BIT_NV),
    };
}

} // namespace

// Regression oracle. Transfer must resolve to family 1 (the dedicated copy engine),
// never family 5 (optical flow) — the family a last-match-wins scan selects.
TEST(VulkanQueueFamilySelection, Rtx5080PicksDedicatedCopyEngineNotOpticalFlow)
{
    const std::vector<VkQueueFamilyProperties> families = Rtx5080Layout();
    const QueueFamilySelection selection = SelectQueueFamilies(families);

    EXPECT_EQ(selection.Graphics, 0u);
    EXPECT_EQ(selection.Compute, 2u);
    EXPECT_EQ(selection.Transfer, 1u);
}

// The chosen families must be general-purpose engines, whatever their index.
TEST(VulkanQueueFamilySelection, Rtx5080NeverSelectsAFixedFunctionEngine)
{
    const std::vector<VkQueueFamilyProperties> families = Rtx5080Layout();
    const QueueFamilySelection selection = SelectQueueFamilies(families);

    constexpr VkQueueFlags kFixedFunction =
        VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_VIDEO_ENCODE_BIT_KHR | VK_QUEUE_OPTICAL_FLOW_BIT_NV;

    ASSERT_NE(selection.Transfer, kInvalid);
    ASSERT_NE(selection.Compute, kInvalid);
    EXPECT_EQ(families[selection.Transfer].queueFlags & kFixedFunction, 0u);
    EXPECT_EQ(families[selection.Compute].queueFlags & kFixedFunction, 0u);
}

// AMD-style discrete layout: one universal family, one async-compute family, one SDMA
// copy family. Both roles have a distinct dedicated home.
TEST(VulkanQueueFamilySelection, AmdStyleLayoutSplitsComputeAndTransfer)
{
    const std::vector<VkQueueFamilyProperties> families = {
        Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT),
        Family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT, 4),
        Family(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT, 2),
    };

    const QueueFamilySelection selection = SelectQueueFamilies(families);

    EXPECT_EQ(selection.Graphics, 0u);
    EXPECT_EQ(selection.Compute, 1u);
    EXPECT_EQ(selection.Transfer, 2u);
}

// Integrated-GPU shape: a single universal family. Both dedicated roles come back
// invalid, and the device substitutes the graphics family.
TEST(VulkanQueueFamilySelection, SingleUniversalFamilyLeavesDedicatedRolesInvalid)
{
    const std::vector<VkQueueFamilyProperties> families = {
        Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT),
    };

    const QueueFamilySelection selection = SelectQueueFamilies(families);

    EXPECT_EQ(selection.Graphics, 0u);
    EXPECT_EQ(selection.Compute, kInvalid);
    EXPECT_EQ(selection.Transfer, kInvalid);
}

// When the only non-graphics family does both jobs, both roles land on it. The device
// then shares one command pool between the two (see AcquireThreadCmdBuffer).
TEST(VulkanQueueFamilySelection, LoneComputeTransferFamilyServesBothRoles)
{
    const std::vector<VkQueueFamilyProperties> families = {
        Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT),
        Family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 2),
    };

    const QueueFamilySelection selection = SelectQueueFamilies(families);

    EXPECT_EQ(selection.Graphics, 0u);
    EXPECT_EQ(selection.Compute, 1u);
    EXPECT_EQ(selection.Transfer, 1u);
}

// Exclusion of fixed-function engines is absolute, not a preference: with only a video
// family to choose from, transfer stays invalid so the caller falls back to the
// general-purpose graphics queue.
TEST(VulkanQueueFamilySelection, VideoOnlyFamilyIsNeverAGeneralTransferQueue)
{
    const std::vector<VkQueueFamilyProperties> families = {
        Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT),
        Family(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_VIDEO_DECODE_BIT_KHR),
    };

    const QueueFamilySelection selection = SelectQueueFamilies(families);

    EXPECT_EQ(selection.Graphics, 0u);
    EXPECT_EQ(selection.Transfer, kInvalid);
    EXPECT_EQ(selection.Compute, kInvalid);
}

// Equal capability counts break on the lowest index, so selection is deterministic.
TEST(VulkanQueueFamilySelection, EqualCapabilityCountsBreakOnLowestIndex)
{
    const std::vector<VkQueueFamilyProperties> families = {
        Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT),
        Family(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT),
        Family(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT),
    };

    EXPECT_EQ(SelectQueueFamilies(families).Transfer, 1u);
}

// A leaner family beats a richer one that appears earlier.
TEST(VulkanQueueFamilySelection, FewerCapabilityBitsBeatsALowerIndex)
{
    const std::vector<VkQueueFamilyProperties> families = {
        Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT),
        Family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT),
        Family(VK_QUEUE_TRANSFER_BIT),
    };

    const QueueFamilySelection selection = SelectQueueFamilies(families);

    EXPECT_EQ(selection.Compute, 1u);
    EXPECT_EQ(selection.Transfer, 2u);
}

// A device with no graphics family at all (compute-only offload part).
TEST(VulkanQueueFamilySelection, ComputeOnlyDeviceHasNoGraphicsFamily)
{
    const std::vector<VkQueueFamilyProperties> families = {
        Family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT),
    };

    const QueueFamilySelection selection = SelectQueueFamilies(families);

    EXPECT_EQ(selection.Graphics, kInvalid);
    EXPECT_EQ(selection.Compute, 0u);
    EXPECT_EQ(selection.Transfer, 0u);
}

// A resolved compute/transfer role is NEVER the graphics family. Callers depend
// on it structurally: the swapchain-recreate drain treats a live m_ComputeQueue /
// m_TransferQueue as proof of a queue the graphics and present waits did not
// cover, and the device only creates those handles for a resolved role.
TEST(VulkanQueueFamilySelection, AResolvedDedicatedRoleIsNeverTheGraphicsFamily)
{
    const std::vector<std::vector<VkQueueFamilyProperties>> layouts = {
        Rtx5080Layout(),
        {Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT),
         Family(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 2)},
        {Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)},
        {Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT),
         Family(VK_QUEUE_COMPUTE_BIT, 4),
         Family(VK_QUEUE_TRANSFER_BIT, 2)},
    };

    for (size_t i = 0; i < layouts.size(); ++i)
    {
        const QueueFamilySelection selection = SelectQueueFamilies(layouts[i]);
        SCOPED_TRACE("layout " + std::to_string(i));
        ASSERT_NE(selection.Graphics, kInvalid) << "every layout here has a graphics family";
        EXPECT_NE(selection.Compute, selection.Graphics);
        EXPECT_NE(selection.Transfer, selection.Graphics);
    }
}

TEST(VulkanQueueFamilySelection, EmptyFamilyListResolvesNothing)
{
    const QueueFamilySelection selection = SelectQueueFamilies({});

    EXPECT_EQ(selection.Graphics, kInvalid);
    EXPECT_EQ(selection.Compute, kInvalid);
    EXPECT_EQ(selection.Transfer, kInvalid);
}

TEST(VulkanQueueFamilySelection, DescribeQueueFlagsNamesEveryKnownBit)
{
    EXPECT_EQ(DescribeQueueFlags(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT
                                 | VK_QUEUE_SPARSE_BINDING_BIT),
              "GRAPHICS|COMPUTE|TRANSFER|SPARSE");
    EXPECT_EQ(DescribeQueueFlags(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_OPTICAL_FLOW_BIT_NV),
              "TRANSFER|SPARSE|OPTICAL_FLOW");
    EXPECT_EQ(DescribeQueueFlags(0), "NONE");
}

// An unrecognised capability must still be visible in the log, not silently dropped.
TEST(VulkanQueueFamilySelection, DescribeQueueFlagsReportsUnnamedBitsAsHex)
{
    constexpr VkQueueFlags kUnknownBit = 0x00001000;
    EXPECT_EQ(DescribeQueueFlags(VK_QUEUE_TRANSFER_BIT | kUnknownBit), "TRANSFER|0x1000");
}

// The init log has to name the engine each role resolved to — the whole reason a
// misrouted transfer queue stayed invisible.
TEST(VulkanQueueFamilySelection, DescribeSelectionNamesTheResolvedEngines)
{
    const std::vector<VkQueueFamilyProperties> families = Rtx5080Layout();

    EXPECT_EQ(DescribeQueueFamilySelection(SelectQueueFamilies(families), families),
              "graphics=0 [GRAPHICS|COMPUTE|TRANSFER|SPARSE], compute=2 [COMPUTE|TRANSFER|SPARSE], "
              "transfer=1 [TRANSFER|SPARSE]");
}

TEST(VulkanQueueFamilySelection, DescribeSelectionReportsUnresolvedRolesAsNone)
{
    const std::vector<VkQueueFamilyProperties> families = {
        Family(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT),
    };

    EXPECT_EQ(DescribeQueueFamilySelection(SelectQueueFamilies(families), families),
              "graphics=0 [GRAPHICS|COMPUTE|TRANSFER], compute=none, transfer=none");
}
