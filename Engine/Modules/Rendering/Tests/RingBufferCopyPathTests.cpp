#include <cstring>
#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Utils/RingBufferHelpers.h"
#include <vector>

using namespace GameEngine::Rendering;

TEST(RingBufferCopyPath, GPUOnlyWriteFlushAndReadback) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));

    // Create a GPUOnly ring buffer for uint32_t with capacity 16 per frame
    auto rb = CreateRingBuffer<uint32_t>(dev.get(), /*frames=*/2, /*capacityPerFrameElements=*/16, "RB_Copy", RingBufferMode::GPUOnly);

    const uint32_t frame = 0;
    ResetRingBufferFrame(rb, frame);

    // Prepare data
    std::vector<uint32_t> values{ 10, 20, 30, 40, 50 };

    // Map and write to staging
    auto alloc = MapRingBuffer<uint32_t>(dev.get(), rb, frame, values.size());
    ASSERT_NE(alloc.ptr, nullptr);
    std::memcpy(alloc.ptr, values.data(), alloc.sizeBytes);
    AdvanceRingBuffer<uint32_t>(rb, frame, values.size());

    // Create readback buffer
    BufferDesc readbackDesc{};
    readbackDesc.size = static_cast<uint32_t>(values.size() * sizeof(uint32_t));
    readbackDesc.usage = static_cast<uint32_t>(BufferUsage::TransferDst);
    readbackDesc.memoryUsage = BufferMemoryUsage::Readback;
    readbackDesc.flags = BufferCreateFlags::PersistentlyMapped;
    readbackDesc.debugName = "RB_Copy_Readback";
    BufferHandle readback = dev->CreateBuffer(readbackDesc);
    ASSERT_TRUE(readback.IsValid());

    // Record commands: flush staging->GPU copy then copy GPU buffer to readback
    auto cmd = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cmd != nullptr);
    cmd->Begin();
    FlushRingBufferWrites<uint32_t>(dev.get(), cmd.get(), rb, frame);
    // Copy from GPU buffer section to readback buffer
    BufferHandle src = GetBufferForBinding<uint32_t>(rb);
    cmd->CopyBuffer(src, readback, alloc.sizeBytes, alloc.byteOffset, 0);
    cmd->End();

    std::vector<CommandList*> lists{ cmd.get() };
    dev->ExecuteCommandLists(lists);
    dev->WaitForIdle();

    // Read back and verify
    void* mapped = dev->MapBuffer(readback);
    ASSERT_NE(mapped, nullptr);
    auto* data = static_cast<uint32_t*>(mapped);
    for (size_t i = 0; i < values.size(); ++i) {
        EXPECT_EQ(data[i], values[i]);
    }
    dev->UnmapBuffer(readback);

    // Cleanup
    dev->DestroyBuffer(readback);
    DestroyRingBuffer<uint32_t>(dev.get(), rb);
}

