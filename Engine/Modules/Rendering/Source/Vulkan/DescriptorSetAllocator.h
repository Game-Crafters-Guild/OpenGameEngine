#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>

namespace GameEngine { namespace Rendering {

class DescriptorSetAllocator {
public:
    struct Config {
        uint32_t MaxSetsPerPool = 1024;
        // Generous defaults for common types
        uint32_t Ubos = 2048;
        uint32_t Sbos = 2048;
        uint32_t Samplers = 1024;
        uint32_t SampledImages = 4096;
        uint32_t CombinedImageSamplers = 4096;
        uint32_t StorageImages = 1024;
        // Ray-query TLAS binds. Zero unless the device actually enabled
        // VK_KHR_acceleration_structure: VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
        // is an extension enum, so naming it in a pool on a device without the
        // extension is invalid usage. VulkanDevice sets this at init.
        uint32_t AccelerationStructures = 0;
        // UPDATE_AFTER_BIND pools (the bindless texture set) must reserve the
        // SAMPLED_IMAGE array (binding 0) >= its per-set count; the small SAMPLER
        // array (binding 1) fits in the default Samplers budget. maxBindlessTextures
        // clamps to UpdateAfterBindSampledImages (single source of truth — see
        // VulkanDevice::QueryDeviceCapabilities). Keep set capacity small since only
        // the one bindless UAB set exists globally.
        uint32_t UpdateAfterBindMaxSetsPerPool = 4;
        uint32_t UpdateAfterBindSampledImages = 16384;
    };

    DescriptorSetAllocator() = default;
    DescriptorSetAllocator(VkDevice device, const Config& cfg) : m_Device(device), m_Cfg(cfg) {}

    void Initialize(VkDevice device, const Config& cfg) { m_Device = device; m_Cfg = cfg; }

    // Per-frame transient pool management
    // Set current frame and total frames-in-flight so transient pools are partitioned per frame index
    void SetCurrentFrame(uint32_t frameIndex, uint32_t framesInFlight);
    // Reset only the transient pools associated with the provided frame index
    void BeginFrameReset(uint32_t frameIndex);

    VkDescriptorSet AllocateTransient(VkDescriptorSetLayout layout, const char* debugName = nullptr);
    VkDescriptorSet AllocatePersistent(VkDescriptorSetLayout layout, const char* debugName = nullptr);
    // Allocate a persistent set from a pool with UPDATE_AFTER_BIND support.
    VkDescriptorSet AllocatePersistentUpdateAfterBind(VkDescriptorSetLayout layout, const char* debugName = nullptr);

    void Destroy();

    // Minimal stats for diagnostics/tests
    struct Stats { uint32_t TransientPools = 0; uint32_t TransientAllocated = 0; uint32_t TransientCapacity = 0; uint32_t PersistentPools = 0; uint32_t PersistentAllocated = 0; uint32_t PersistentCapacity = 0; };
    Stats GetStats() const;

private:
    struct Pool {
        VkDescriptorPool Handle = VK_NULL_HANDLE;
        uint32_t AllocatedSets = 0;
        uint32_t Capacity = 0;
    };

    VkDescriptorPool CreatePool(uint32_t maxSets, bool updateAfterBind = false) const;

    VkDevice m_Device = VK_NULL_HANDLE;
    Config m_Cfg{};

    // Maintain multiple pools; never destroy a pool while sets are alive
    // Legacy single list used as fallback when frames-in-flight not set
    std::vector<Pool> m_TransientPools{}; // reset every frame (legacy)
    // Partitioned per-frame transient pools; only the current frame's pools are reset each BeginFrame
    std::vector<std::vector<Pool>> m_TransientPoolsPerFrame{};
    uint32_t m_CurrentFrameIndex = 0;
    uint32_t m_FramesInFlight = 0;

    std::vector<Pool> m_PersistentPools{};                // long-lived pools
    std::vector<Pool> m_PersistentPoolsUpdateAfterBind{}; // long-lived, UPDATE_AFTER_BIND
    mutable std::mutex m_Mutex; // simple coarse-grained lock for thread-safety during stress tests

    // Allocate from the provided pool vector. Caller must hold m_Mutex.
    VkDescriptorSet AllocateFromPoolsLocked(std::vector<Pool>& pools, VkDescriptorSetLayout layout, const char* debugName);
    VkDescriptorSet AllocateFromPoolsLockedImpl(std::vector<Pool>& pools, VkDescriptorSetLayout layout, bool updateAfterBind);
};

}} // namespace GameEngine::Rendering
