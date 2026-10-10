#include <gtest/gtest.h>
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DescriptorWriter.h"
#include "Rendering/Core/CommandList.h"

#include <vector>

#include "../Source/Vulkan/VulkanDevice.h"

using namespace GameEngine::Rendering;

namespace {

// Single uniform-buffer binding, the layout both tests allocate. It is
// descriptor-buffer eligible, so which path serves it is decided by the device's
// DescriptorBufferMode and nothing else.
DescriptorSetLayoutDesc UniformBufferLayout() {
    DescriptorSetLayoutDesc layout{};
    DescriptorBinding b{}; b.binding = 0; b.type = DescriptorType::UniformBuffer; b.count = 1; b.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(b);
    return layout;
}

} // namespace

// DescriptorSetAllocator is the legacy descriptor-POOL allocator, so its counters only move
// on a device that runs the pool path. DescriptorBufferMode is captured at Initialize and
// Disabled forces the pool path for every layout — that is what makes these numbers the
// same on every machine. Left at Auto this test measures whether the host happens to
// support descriptor buffers instead of measuring the allocator.
TEST(DescriptorSetAllocator, StatsGrowAndReset) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.descriptorBuffers = DescriptorBufferMode::Disabled;
    auto devBase = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(devBase && devBase->Initialize(dd));

    // Downcast to Vulkan to access stats
    auto* vk = dynamic_cast<GameEngine::Rendering::VulkanDevice*>(devBase.get());
    ASSERT_NE(vk, nullptr);
    ASSERT_FALSE(vk->IsDescriptorBufferEnabled())
        << "DescriptorBufferMode::Disabled must suppress the descriptor-buffer path";

    const DescriptorSetLayoutDesc layout = UniformBufferLayout();

    // Enter a frame before measuring. The allocator partitions transient pools per frame
    // only once the device has set a frame index; sets allocated before that land in the
    // pre-frame fallback list, which GetStats stops reporting as soon as the partition
    // exists. Production always allocates inside a frame.
    vk->FinalizeFrame();
    ASSERT_TRUE(vk->BeginFrame());

    // Run more frames than the device keeps in flight so every slot is re-entered and
    // rewound at least once — that is what makes the bound below a real check.
    constexpr uint32_t kSetsPerFrame = 64;
    constexpr uint32_t kFrames = IDevice::kMaxSupportedFramesInFlight + 2;

    // Allocate transient sets across a few frames
    for (uint32_t f = 0; f < kFrames; ++f) {
        for (uint32_t i = 0; i < kSetsPerFrame; ++i) {
            DescriptorSetDesc ds{}; ds.layout = layout; ds.transient = true; ds.debugName = "StatsTransient";
            auto h = devBase->CreateDescriptorSet(ds);
        // Submit a minimal no-op command list to ensure a signaled fence and trigger transient reset
        {
            auto cl = devBase->CreateCommandList(IDevice::QueueType::Graphics);
            cl->Begin();
            cl->End();
            std::vector<CommandList*> lists{cl.get()};
            devBase->ExecuteCommandLists(lists);
        }
        // Ensure GPU work is complete so transient pools can safely reset next frame
        devBase->WaitForIdle();


            ASSERT_TRUE(h.IsValid());
        }
        // Finalize current offscreen frame to signal the fence, then begin next frame (resets transient pools)
        vk->FinalizeFrame();
        ASSERT_TRUE(vk->BeginFrame());
        auto s = vk->GetDescriptorSetAllocator().GetStats();
        EXPECT_GE(s.TransientPools, 1u);
        // Each slot is rewound by the BeginFrame that re-enters it, so the live transient
        // count stays bounded by one pass per slot no matter how many frames run. An
        // allocator that stopped recycling would instead grow by kSetsPerFrame per frame.
        EXPECT_LE(s.TransientAllocated, kSetsPerFrame * IDevice::kMaxSupportedFramesInFlight);
    }

    // Allocate persistent sets to grow persistent pools
    std::vector<DescriptorSetHandle> persistent;
    for (int i = 0; i < 128; ++i) {
        DescriptorSetDesc ds{}; ds.layout = layout; ds.transient = false; ds.debugName = "StatsPersistent";
        auto h = devBase->CreateDescriptorSet(ds);
        ASSERT_TRUE(h.IsValid());
        persistent.push_back(h);
    }
    auto ps = vk->GetDescriptorSetAllocator().GetStats();
    EXPECT_GE(ps.PersistentPools, 1u);
    EXPECT_GT(ps.PersistentAllocated, 0u);

    for (auto h : persistent) devBase->DestroyDescriptorSet(h);
}

// The same allocations on the other binding model. A descriptor-buffer device serves them
// from VulkanDescriptorBufferPool, a separate allocator, so the pool allocator's counters
// must not move at all. That pool publishes no statistics of its own; the descriptor-buffer
// tag on each returned handle is what evidences the path actually ran, without which
// unchanged counters would also be what "nothing got allocated" looks like.
TEST(DescriptorSetAllocator, DescriptorBufferPathLeavesPoolStatsAlone) {
    DeviceDesc dd{}; dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.descriptorBuffers = DescriptorBufferMode::Auto;
    auto devBase = DeviceFactory::CreateDevice(dd);
    ASSERT_TRUE(devBase && devBase->Initialize(dd));

    auto* vk = dynamic_cast<GameEngine::Rendering::VulkanDevice*>(devBase.get());
    ASSERT_NE(vk, nullptr);
    if (!vk->IsDescriptorBufferEnabled()) {
        GTEST_SKIP() << "device does not run the descriptor-buffer path ("
                     << vk->GetHardwareDescription()
                     << "); the pool path is covered by StatsGrowAndReset";
    }

    const DescriptorSetLayoutDesc layout = UniformBufferLayout();
    const auto before = vk->GetDescriptorSetAllocator().GetStats();

    std::vector<DescriptorSetHandle> persistent;
    for (int i = 0; i < 128; ++i) {
        DescriptorSetDesc ds{}; ds.layout = layout; ds.transient = false; ds.debugName = "StatsPersistentDescriptorBuffer";
        auto h = devBase->CreateDescriptorSet(ds);
        ASSERT_TRUE(h.IsValid());
        ASSERT_TRUE(VulkanDevice::IsDescriptorBufferHandle(h))
            << "a descriptor-buffer device must return descriptor-buffer handles";
        persistent.push_back(h);
    }

    const auto after = vk->GetDescriptorSetAllocator().GetStats();
    EXPECT_EQ(after.PersistentPools, before.PersistentPools);
    EXPECT_EQ(after.PersistentAllocated, before.PersistentAllocated);

    for (auto h : persistent) devBase->DestroyDescriptorSet(h);
}
