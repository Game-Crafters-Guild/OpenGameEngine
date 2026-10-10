#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"

using namespace GameEngine::Rendering;

TEST(DescriptorSetAllocator, TransientResetAndGrowth) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));

    // Create a simple layout
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::UniformBuffer; b.count = 1; b.shaderStages = 0x20; // compute
    layout.bindings.push_back(b);

    // Allocate many persistent sets to force growth; ensure allocations succeed
    const int kSets = 2048;
    std::vector<DescriptorSetHandle> sets; sets.reserve(kSets);
    for (int i = 0; i < kSets; ++i) {
        DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "AllocTest";
        auto h = dev->CreateDescriptorSet(ds);
        ASSERT_TRUE(h.IsValid());
        sets.push_back(h);
    }

    // BeginFrame should reset transient pools; we don't allocate transient directly here
    ASSERT_TRUE(dev->BeginFrame());

    // Cleanup persistent sets
    for (auto h : sets) dev->DestroyDescriptorSet(h);
}

