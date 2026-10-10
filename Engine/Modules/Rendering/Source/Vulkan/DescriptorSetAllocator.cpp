#include "DescriptorSetAllocator.h"
#include <cassert>
#include <cstring>

namespace GameEngine { namespace Rendering {

static std::vector<VkDescriptorPoolSize> MakePoolSizes(const DescriptorSetAllocator::Config& cfg) {
    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(6);
    sizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, cfg.Ubos });
    sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cfg.Sbos });
    sizes.push_back({ VK_DESCRIPTOR_TYPE_SAMPLER, cfg.Samplers });
    sizes.push_back({ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, cfg.SampledImages });
    sizes.push_back({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, cfg.CombinedImageSamplers });
    sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, cfg.StorageImages });
    if (cfg.AccelerationStructures > 0)
        sizes.push_back({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, cfg.AccelerationStructures });
    return sizes;
}

VkDescriptorPool DescriptorSetAllocator::CreatePool(uint32_t maxSets, bool updateAfterBind) const {
    std::vector<VkDescriptorPoolSize> sizes = MakePoolSizes(m_Cfg);
    if (updateAfterBind)
    {
        // sizes[] order matches MakePoolSizes: [2]=SAMPLER, [3]=SAMPLED_IMAGE.
        // The only UAB set is the bindless set: a SAMPLED_IMAGE array (binding 0)
        // + a small SAMPLER array (binding 1, covered by the default sizes[2]
        // budget). Reserve SAMPLED_IMAGE at the bindless budget so
        // vkAllocateDescriptorSets can't return OUT_OF_POOL_MEMORY.
        sizes[3].descriptorCount = m_Cfg.UpdateAfterBindSampledImages;
    }

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (updateAfterBind)
        info.flags |= VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info.maxSets = maxSets;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult res = vkCreateDescriptorPool(m_Device, &info, nullptr, &pool);
    if (res != VK_SUCCESS) return VK_NULL_HANDLE;
    return pool;
}

void DescriptorSetAllocator::SetCurrentFrame(uint32_t frameIndex, uint32_t framesInFlight) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_CurrentFrameIndex = frameIndex;
    if (framesInFlight != m_FramesInFlight) {
        m_FramesInFlight = framesInFlight;
        m_TransientPoolsPerFrame.clear();
        m_TransientPoolsPerFrame.resize(m_FramesInFlight);
    } else if (m_TransientPoolsPerFrame.size() != m_FramesInFlight) {
        m_TransientPoolsPerFrame.resize(m_FramesInFlight);
    }
}

void DescriptorSetAllocator::BeginFrameReset(uint32_t frameIndex) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    // Helper: reset all pools in a vector and destroy excess empty ones so that
    // a spike frame that temporarily needed extra pools doesn't leak them forever.
    auto resetAndTrim = [this](std::vector<Pool>& pools) {
        for (auto& p : pools) {
            if (p.Handle != VK_NULL_HANDLE) {
                vkResetDescriptorPool(m_Device, p.Handle, 0);
                p.AllocatedSets = 0;
            }
        }
        // Keep at most 1 pool; destroy extras created during spike frames.
        constexpr size_t kKeepPools = 1;
        while (pools.size() > kKeepPools) {
            Pool& back = pools.back();
            if (back.Handle != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(m_Device, back.Handle, nullptr);
            pools.pop_back();
        }
    };

    if (!m_TransientPoolsPerFrame.empty()) {
        auto& pools = m_TransientPoolsPerFrame[frameIndex % m_TransientPoolsPerFrame.size()];
        resetAndTrim(pools);
    } else {
        resetAndTrim(m_TransientPools);
    }
}

VkDescriptorSet DescriptorSetAllocator::AllocateFromPoolsLocked(std::vector<Pool>& pools, VkDescriptorSetLayout layout, const char* /*debugName*/) {
    return AllocateFromPoolsLockedImpl(pools, layout, false);
}

VkDescriptorSet DescriptorSetAllocator::AllocateFromPoolsLockedImpl(std::vector<Pool>& pools, VkDescriptorSetLayout layout, bool updateAfterBind) {
    // Try existing pools
    for (auto& p : pools) {
        if (p.Handle != VK_NULL_HANDLE && p.AllocatedSets < p.Capacity) {
            VkDescriptorSetAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc.descriptorPool = p.Handle;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &layout;

            VkDescriptorSet set = VK_NULL_HANDLE;
            VkResult res = vkAllocateDescriptorSets(m_Device, &alloc, &set);
            if (res == VK_SUCCESS) { p.AllocatedSets++; return set; }
        }
    }
    // Need a new pool
    Pool np{};
    np.Capacity = updateAfterBind ? m_Cfg.UpdateAfterBindMaxSetsPerPool : m_Cfg.MaxSetsPerPool;
    np.Handle = CreatePool(np.Capacity, updateAfterBind);
    if (np.Handle == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    pools.push_back(np);
    Pool& p = pools.back();

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = p.Handle;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult res = vkAllocateDescriptorSets(m_Device, &alloc, &set);
    if (res == VK_SUCCESS) { p.AllocatedSets++; return set; }
    return VK_NULL_HANDLE;
}

VkDescriptorSet DescriptorSetAllocator::AllocateTransient(VkDescriptorSetLayout layout, const char* debugName) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_TransientPoolsPerFrame.empty())
    {
        auto& pools = m_TransientPoolsPerFrame[m_CurrentFrameIndex % m_TransientPoolsPerFrame.size()];
        return AllocateFromPoolsLocked(pools, layout, debugName);
    }
    return AllocateFromPoolsLocked(m_TransientPools, layout, debugName);
}

VkDescriptorSet DescriptorSetAllocator::AllocatePersistent(VkDescriptorSetLayout layout, const char* debugName) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return AllocateFromPoolsLocked(m_PersistentPools, layout, debugName);
}

VkDescriptorSet DescriptorSetAllocator::AllocatePersistentUpdateAfterBind(VkDescriptorSetLayout layout, const char* debugName) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return AllocateFromPoolsLockedImpl(m_PersistentPoolsUpdateAfterBind, layout, true);
}

void DescriptorSetAllocator::Destroy() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_TransientPoolsPerFrame.empty()) {
        for (auto& vec : m_TransientPoolsPerFrame) {
            for (auto& p : vec) { if (p.Handle != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_Device, p.Handle, nullptr); }
        }
        m_TransientPoolsPerFrame.clear();
    }
    for (auto& p : m_TransientPools) { if (p.Handle != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_Device, p.Handle, nullptr); }
    for (auto& p : m_PersistentPools) { if (p.Handle != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_Device, p.Handle, nullptr); }
    for (auto& p : m_PersistentPoolsUpdateAfterBind) { if (p.Handle != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_Device, p.Handle, nullptr); }
    m_TransientPools.clear(); m_PersistentPools.clear(); m_PersistentPoolsUpdateAfterBind.clear();
}

DescriptorSetAllocator::Stats DescriptorSetAllocator::GetStats() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    Stats s{};
    if (!m_TransientPoolsPerFrame.empty()) {
        s.TransientPools = 0;
        for (const auto& vec : m_TransientPoolsPerFrame) {
            s.TransientPools += static_cast<uint32_t>(vec.size());
            for (const auto& p : vec) { s.TransientAllocated += p.AllocatedSets; s.TransientCapacity += p.Capacity; }
        }
    } else {
        s.TransientPools = static_cast<uint32_t>(m_TransientPools.size());
        for (const auto& p : m_TransientPools) { s.TransientAllocated += p.AllocatedSets; s.TransientCapacity += p.Capacity; }
    }
    s.PersistentPools = static_cast<uint32_t>(m_PersistentPools.size());
    for (const auto& p : m_PersistentPools) { s.PersistentAllocated += p.AllocatedSets; s.PersistentCapacity += p.Capacity; }
    return s;
}


}} // namespace GameEngine::Rendering
