#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Vulkan/VulkanDevice.h"

using namespace GameEngine::Rendering;

static std::unique_ptr<IDevice> CreateDevice_Headless2(const char* appName) {
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

TEST(BarrierBatchBufferDedupe, IdenticalBufferBarriersMergedInBatch)
{
    auto dev = CreateDevice_Headless2("BarrierBatchBufferDedupe");
    if (!dev) GTEST_SKIP() << "No Vulkan device available";
    auto* vk = dynamic_cast<VulkanDevice*>(dev.get());
    if (!vk) GTEST_SKIP() << "Not running on Vulkan backend";

    // Create a small buffer
    BufferDesc bd{}; bd.size = 256;
    bd.usage = static_cast<uint32_t>(BufferUsage::TransferSrc) | static_cast<uint32_t>(BufferUsage::TransferDst) | static_cast<uint32_t>(BufferUsage::Storage);
    BufferHandle buf = dev->CreateBuffer(bd);

    // Build two identical barriers for the same buffer
    ResourceBarrier b1 = ResourceBarrier::CreateBufferBarrier(buf, ResourceState::CopyDest, ResourceState::CopySource);
    ResourceBarrier b2 = ResourceBarrier::CreateBufferBarrier(buf, ResourceState::CopyDest, ResourceState::CopySource);

    std::unique_ptr<CommandList> cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl != nullptr);
    cl->Begin();
    std::vector<ResourceBarrier> batch{ b1, b2 };
    cl->BarrierBatch(batch);
    cl->End();

    vk->ClearDebugBarriers();
    dev->ExecuteCommandLists(std::vector<CommandList*>{ cl.get() });

    // Single vkCmdPipelineBarrier2 call, and only 1 buffer barrier in that batch
    EXPECT_EQ(vk->GetDebugPipelineBarrier2Calls(), 1u);
    EXPECT_EQ(vk->GetDebugLastDependencyBufferCount(), 1u);
    EXPECT_EQ(vk->GetDebugLastDependencyImageCount(), 0u);
    // Memory barriers not involved here
    EXPECT_EQ(vk->GetDebugLastDependencyMemoryCount(), 0u);

    dev->DestroyBuffer(buf);
}

