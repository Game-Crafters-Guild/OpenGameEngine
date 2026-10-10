#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include <thread>
#include <vector>

using namespace GameEngine::Rendering;

static DescriptorSetLayoutDesc MakeLayout(uint32_t stagesBits) {
    DescriptorSetLayoutDesc l{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::UniformBuffer; b.count = 1; b.shaderStages = stagesBits;
    l.bindings.push_back(b);
    return l;
}

TEST(DescriptorSetAllocatorThreaded, MultiThreadedAllocatePersistent) {
    DeviceDesc desc{}; desc.preferredAPI = GraphicsAPI::Vulkan;
    auto dev = DeviceFactory::CreateDevice(desc);
    ASSERT_TRUE(dev && dev->Initialize(desc));

    auto layout = MakeLayout(/*compute*/0x20);

    const int kThreads = 8;
    const int kSetsPerThread = 512;

    std::vector<std::thread> threads;
    std::vector<std::vector<DescriptorSetHandle>> results(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            results[t].reserve(kSetsPerThread);
            for (int i = 0; i < kSetsPerThread; ++i) {
                DescriptorSetDesc ds{}; ds.layout = layout; ds.debugName = "ThreadedSet";
                auto h = dev->CreateDescriptorSet(ds);
                ASSERT_TRUE(h.IsValid());
                results[t].push_back(h);
            }
        });
    }

    for (auto& th : threads) th.join();

    // Free all sets
    for (auto& vec : results) for (auto h : vec) dev->DestroyDescriptorSet(h);
}

