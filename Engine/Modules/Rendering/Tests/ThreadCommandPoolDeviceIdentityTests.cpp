#include <gtest/gtest.h>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include "Source/Vulkan/VulkanCommandList.h"
#include "Source/Vulkan/VulkanDevice.h"

#include <memory>

using namespace GameEngine::Rendering;

namespace
{

std::unique_ptr<IDevice> MakeHeadlessDevice()
{
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    dd.enableSwapchain = false;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
        return nullptr;
    return dev;
}

const void* PoolOwnerOf(CommandList* cl)
{
    return static_cast<VulkanCommandList*>(cl)->GetThreadPoolOwner();
}

VkCommandBuffer HandleOf(CommandList* cl)
{
    return static_cast<VulkanCommandList*>(cl)->GetVkCommandBuffer();
}

} // namespace

// The per-thread command-pool cache (VulkanDevice::t_CachedPool) is `static
// thread_local`: one slot per THREAD for the whole process, not one per device.
// Two devices alive at once on the same thread must therefore still get their
// own pool entry. Keyed on the rebuild generation alone they did not — both
// start at generation 0 — so the second device recorded into the first
// device's pool and submitted that command buffer to its own queue:
// VUID-VkSubmitInfo-commonparent, and an access violation inside the driver.
//
// The acquisition order is what matters: A is asked first so the cache is warm
// and holding A's entry when B asks.
TEST(ThreadCommandPoolDeviceIdentity, ASecondLiveDeviceGetsItsOwnThreadPool)
{
    auto deviceA = MakeHeadlessDevice();
    if (!deviceA)
        GTEST_SKIP() << "No Vulkan device available";
    auto deviceB = MakeHeadlessDevice();
    ASSERT_TRUE(deviceB) << "second device could not be created";

    auto listA = deviceA->CreateCommandList(IDevice::QueueType::Graphics);
    auto listB = deviceB->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(listA);
    ASSERT_TRUE(listB);

    EXPECT_NE(PoolOwnerOf(listA.get()), PoolOwnerOf(listB.get()))
        << "both devices were handed the same thread command-pool entry";
    EXPECT_NE(HandleOf(listA.get()), HandleOf(listB.get()));

    // Going back to A must return A's entry, not stick to B's.
    auto listA2 = deviceA->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(listA2);
    EXPECT_EQ(PoolOwnerOf(listA2.get()), PoolOwnerOf(listA.get()));

    listA2.reset();
    listA.reset();
    listB.reset();
    deviceB->WaitForIdle();
    deviceA->WaitForIdle();
}

// The end-to-end shape the UI fixtures hit: record and SUBMIT on the second
// device while the first is still alive. With the pools crossed this faults
// inside the driver rather than failing an assertion, so the assertion here is
// only a landmark — surviving the submit is the test.
TEST(ThreadCommandPoolDeviceIdentity, SubmitOnTheSecondDeviceWhileTheFirstIsAlive)
{
    auto deviceA = MakeHeadlessDevice();
    if (!deviceA)
        GTEST_SKIP() << "No Vulkan device available";

    // Warm A's cache on this thread before B exists.
    {
        auto warm = deviceA->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_TRUE(warm);
        warm->Begin();
        warm->End();
        deviceA->ExecuteCommandLists(std::vector<CommandList*>{warm.get()});
    }
    deviceA->WaitForIdle();

    auto deviceB = MakeHeadlessDevice();
    ASSERT_TRUE(deviceB);
    {
        auto listB = deviceB->CreateCommandList(IDevice::QueueType::Graphics);
        ASSERT_TRUE(listB);
        listB->Begin();
        listB->End();
        deviceB->ExecuteCommandLists(std::vector<CommandList*>{listB.get()});
    }
    deviceB->WaitForIdle();

    SUCCEED();
}
