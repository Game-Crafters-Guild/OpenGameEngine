#include <gtest/gtest.h>
#include <cstdlib>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Vulkan/VulkanDevice.h"

using namespace GameEngine::Rendering;

static std::unique_ptr<IDevice> CreateDevice_Headless(const char* appName) {
#ifdef _WIN32
    _putenv_s("GE_HEADLESS_TEST", "1");
#else
    setenv("GE_HEADLESS_TEST", "1", 1);
#endif
    DeviceDesc d{}; d.applicationName = appName; d.preferredAPI = GraphicsAPI::Vulkan; d.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(d);
    if (!dev || !dev->Initialize(d)) return nullptr;
    return dev;
}

TEST(BarrierMemoryBatch, SingleMemoryBarrierIncrementsCounter)
{
    auto dev = CreateDevice_Headless("BarrierMemoryBatch");
    if (!dev) GTEST_SKIP() << "No Vulkan device available";

    auto* vk = dynamic_cast<VulkanDevice*>(dev.get());
    if (!vk) GTEST_SKIP() << "Not running on Vulkan backend";

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_NE(cl, nullptr);

    vk->ClearDebugBarriers();
    ASSERT_TRUE(dev->BeginFrame());
    cl->Begin();

    std::vector<ResourceBarrier> barriers;
    barriers.push_back(ResourceBarrier::CreateMemoryBarrier(
        /*srcStage*/ VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        /*dstStage*/ VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        /*srcAccess*/ VK_ACCESS_SHADER_WRITE_BIT,
        /*dstAccess*/ VK_ACCESS_SHADER_READ_BIT));

    cl->BarrierBatch(barriers);
    cl->End();

    // One vkCmdPipelineBarrier2 call should have been recorded
    EXPECT_EQ(vk->GetDebugPipelineBarrier2Calls(), 1u);

    // The batch holds the one memory barrier and no resource-specific barrier
    EXPECT_EQ(vk->GetDebugLastDependencyImageCount(), 0u);
    EXPECT_EQ(vk->GetDebugLastDependencyBufferCount(), 0u);
    EXPECT_EQ(vk->GetDebugLastDependencyMemoryCount(), 1u);

    dev->Present();
}

