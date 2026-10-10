#include <gtest/gtest.h>
#include "Rendering/Utils/RingBufferHelpers.h"
#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

struct TestStruct { uint32_t x; uint32_t y; };

TEST(RingBufferHelpers, FrameResetAndOverflow) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));

    auto rb = CreateRingBuffer<TestStruct>(dev.get(), 2, 4, "RBTest", RingBufferMode::GPUOnly); // 4 structs per frame

    ResetRingBufferFrame(rb, 0);
    // Allocate exactly capacity
    auto a1 = MapRingBuffer<TestStruct>(dev.get(), rb, 0, 4);
    ASSERT_NE(a1.ptr, nullptr);
    AdvanceRingBuffer(rb, 0, 4);

    // Next allocation should overflow
    auto a2 = MapRingBuffer<TestStruct>(dev.get(), rb, 0, 1);
    ASSERT_EQ(a2.ptr, nullptr);
    // Overflow flag set
    // Access internal flag directly (header-only test)
    EXPECT_TRUE(rb.overflowed[0]);

    // Next frame reset clears overflow and head
    ResetRingBufferFrame(rb, 1);
    auto a3 = MapRingBuffer<TestStruct>(dev.get(), rb, 1, 2);
    ASSERT_NE(a3.ptr, nullptr);
    AdvanceRingBuffer(rb, 1, 2);
    EXPECT_FALSE(rb.overflowed[1]);

    // Cleanup to avoid VMA assertions on device shutdown
    DestroyRingBuffer(dev.get(), rb);
}

TEST(RingBufferHelpers, DestroyReleasesBuffer) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));

    auto rb = CreateRingBuffer<TestStruct>(dev.get(), 2, 1, "RBTestDestroy", RingBufferMode::GPUOnly);
    ASSERT_TRUE(rb.buffer.IsValid());

    // Explicitly destroy to avoid VMA leak assertion
    DestroyRingBuffer(dev.get(), rb);
    EXPECT_FALSE(rb.buffer.IsValid());
}


