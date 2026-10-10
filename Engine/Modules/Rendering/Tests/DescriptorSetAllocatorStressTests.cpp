#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

static DescriptorSetLayoutDesc MakeSimpleLayout(uint32_t stagesBits) {
    DescriptorSetLayoutDesc l{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::UniformBuffer; b.count = 1; b.shaderStages = stagesBits;
    l.bindings.push_back(b);
    return l;
}

TEST(DescriptorSetAllocatorStress, TransientResetAcrossFrames) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));

    // Create a descriptor set layout
    auto layout = MakeSimpleLayout(/*compute*/0x20);

    // For each frame, start BeginFrame and allocate many transient sets
    const int kFrames = 5;
    const int kSetsPerFrame = 2048; // large to force allocator growth

    for (int f = 0; f < kFrames; ++f) {
        ASSERT_TRUE(dev->BeginFrame());

        std::vector<DescriptorSetHandle> tmp;
        tmp.reserve(kSetsPerFrame);
        for (int i = 0; i < kSetsPerFrame; ++i) {
            DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "TransientSet"; ds.transient = true;
            auto h = dev->CreateDescriptorSet(ds);
            ASSERT_TRUE(h.IsValid());
            tmp.push_back(h);
        }
        // End frame without Present to trigger FinalizeFrame path when used
        dev->FinalizeFrame();

        // Note: We do not DestroyDescriptorSet here to simulate transient usage
        // Allocator will reset transient pools on BeginFrame; persistent allocations
        // are not used in this stress path.
    }
}

TEST(DescriptorSetAllocatorStress, PersistentAllocateFreePattern) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));

    auto layout = MakeSimpleLayout(/*compute*/0x20);

    const int kBatches = 4;
    const int kBatchSize = 1024;

    for (int b = 0; b < kBatches; ++b) {
        std::vector<DescriptorSetHandle> batch;
        batch.reserve(kBatchSize);
        for (int i = 0; i < kBatchSize; ++i) {
            DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "PersistentSet";
            auto h = dev->CreateDescriptorSet(ds);
            ASSERT_TRUE(h.IsValid());
            batch.push_back(h);
        }
        // random-ish free order: even then odd
        for (size_t i = 0; i < batch.size(); i += 2) dev->DestroyDescriptorSet(batch[i]);
        for (size_t i = 1; i < batch.size(); i += 2) dev->DestroyDescriptorSet(batch[i]);
    }

    // No explicit validation besides successful shutdown; VMA or Vulkan validation would assert on leaks
}

