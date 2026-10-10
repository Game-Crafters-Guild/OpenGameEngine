#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"

#include "Source/Vulkan/VulkanDevice.h"
#include "Source/Vulkan/VulkanCommandList.h"

using namespace GameEngine::Rendering;

namespace
{
VkCommandBuffer HandleOf(CommandList* cl)
{
    return static_cast<VulkanCommandList*>(cl)->GetVkCommandBuffer();
}
} // namespace

// A2.4-D5: the per-thread command pool segregates its free/used lists by
// command-buffer level. A retired SECONDARY must recycle into the secondary
// bucket, never the primary bucket — before segregation, a recycled secondary
// could be popped by AcquireThreadCmdBuffer and handed out as a primary, which
// Vulkan rejects (level mismatch). This exercises the real retire + frame-slot
// recycle path and asserts (1) a primary acquire never returns the retired
// secondary and (2) the next secondary acquire reuses it.
TEST(CommandPoolLevelSegregation, RetiredSecondaryNeverReusedAsPrimary)
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        GTEST_SKIP() << "No Vulkan device available";

    // Allocate a secondary from this thread's pool and capture its handle.
    auto secondary = dev->CreateSecondaryCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(secondary);
    const VkCommandBuffer secondaryHandle = HandleOf(secondary.get());
    ASSERT_NE(secondaryHandle, VK_NULL_HANDLE);

    // Retire it (nulls the wrapper's CB so its destructor does not double-recycle),
    // then drop the wrapper.
    CommandList* retire[] = {secondary.get()};
    dev->RetireSecondaryCommandLists(retire, 1);
    secondary.reset();

    // Drive enough frame boundaries to recycle the retire-slot's used list back into
    // the free list. RecycleThreadPoolsForFrame runs once per BeginFrame for the
    // current slot, so a full wrap (+ margin) guarantees the retire slot is visited.
    const uint32_t cycles = dev->GetFramesInFlight() + 3u;
    for (uint32_t i = 0; i < cycles; ++i)
    {
        ASSERT_TRUE(dev->BeginFrame());
        dev->Present();
    }

    // A primary acquire must NOT return the retired secondary (segregation holds).
    auto primary = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(primary);
    EXPECT_NE(HandleOf(primary.get()), secondaryHandle)
        << "Retired secondary was handed out as a primary (level bucket leak)";

    // A secondary acquire SHOULD reuse the recycled secondary (freelist hit).
    auto secondary2 = dev->CreateSecondaryCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(secondary2);
    EXPECT_EQ(HandleOf(secondary2.get()), secondaryHandle)
        << "Recycled secondary was not reused by the secondary freelist";

    dev->WaitForIdle();
}
