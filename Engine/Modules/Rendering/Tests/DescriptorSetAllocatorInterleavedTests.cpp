#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

static DescriptorSetLayoutDesc MakeLayout(uint32_t stagesBits) {
    DescriptorSetLayoutDesc l{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::UniformBuffer; b.count = 1; b.shaderStages = stagesBits;
    l.bindings.push_back(b);
    return l;
}

TEST(DescriptorSetAllocatorInterleaved, MixedTransientPersistent) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));

    auto layout = MakeLayout(/*compute*/0x20);

    const int kFrames = 3;
    const int kTransientPerFrame = 512;
    const int kPersistentPerFrame = 256;

    std::vector<DescriptorSetHandle> persistents;

    for (int f = 0; f < kFrames; ++f) {
        ASSERT_TRUE(dev->BeginFrame());
        // Transient allocations
        for (int i = 0; i < kTransientPerFrame; ++i) {
            DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "InterleavedTransient"; ds.transient = true;
            auto h = dev->CreateDescriptorSet(ds);
            ASSERT_TRUE(h.IsValid());
        }
        // Persistent allocations (kept until end)
        for (int i = 0; i < kPersistentPerFrame; ++i) {
            DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "InterleavedPersistent"; ds.transient = false;
            auto h = dev->CreateDescriptorSet(ds);
            ASSERT_TRUE(h.IsValid());
            persistents.push_back(h);
        }
        dev->FinalizeFrame();
    }

    // Cleanup persistent sets
    for (auto h : persistents) dev->DestroyDescriptorSet(h);
}

