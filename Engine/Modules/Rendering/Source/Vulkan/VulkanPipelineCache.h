#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace GameEngine
{
namespace Rendering
{

// Vulkan-only wrapper around `VkPipelineCache` for disk persistence.
// Distinct from the backend-agnostic `Rendering::PipelineCache` (interning +
// concrete-pipeline cache); name reflects the binding to Vulkan's API type.
class VulkanPipelineCache
{
  public:
    struct Config
    {
        std::string CacheDirectory = "cache/pipelines/";
        std::string CacheFileName = "vulkan_pipeline_cache.bin";
        bool EnableCompression = true;
        bool EnableValidation = true;
        uint32_t MaxCacheSize = 64 * 1024 * 1024;
        uint32_t CacheVersion = 1;
    };

    struct Statistics
    {
        uint32_t TotalPipelines = 0;
        uint32_t CacheHits = 0;
        uint32_t CacheMisses = 0;
        size_t CacheSize = 0;
        double AverageCreationTime = 0.0;
        double AverageCachedCreationTime = 0.0;
    };

    // Provide explicit overloads instead of aggregate default argument to avoid
    // compiler issues with nested Config default member initializers on some toolchains.
    explicit VulkanPipelineCache(VkDevice device);
    VulkanPipelineCache(VkDevice device, const Config& config);
    ~VulkanPipelineCache();

    static std::string GetPlatformCacheDirectory();

    VulkanPipelineCache(const VulkanPipelineCache&) = delete;
    VulkanPipelineCache& operator=(const VulkanPipelineCache&) = delete;
    VulkanPipelineCache(VulkanPipelineCache&&) = delete;
    VulkanPipelineCache& operator=(VulkanPipelineCache&&) = delete;

    bool Initialize();
    // saveToDisk defaults true (the normal shutdown persist point). The Q6
    // in-place device rebuild passes false: the teardown drops this cache object
    // and bringup immediately reconstructs one that reloads the same on-disk
    // bytes, so re-persisting the (up to tens-of-MB) blob mid-recovery is pure
    // latency — the slice-2 smoke measured it dominating recovery wall-time.
    void Shutdown(bool saveToDisk = true);

    VkPipelineCache GetVkPipelineCache() const
    {
        return m_PipelineCache;
    }

    // Lock for external synchronization of VkPipelineCache during concurrent
    // vkCreate*Pipelines calls. The Vulkan spec requires that the application
    // externally synchronize the pipeline cache object when used from multiple
    // threads simultaneously. Callers must hold this lock for the duration of
    // vkCreateGraphicsPipelines / vkCreateComputePipelines calls.
    std::unique_lock<std::mutex> LockForCreation()
    {
        return std::unique_lock<std::mutex>(m_CreationMutex);
    }

    bool SaveCache();
    bool LoadCache();
    void ClearCache();

    const Statistics& GetStatistics() const
    {
        return m_Statistics;
    }
    // Thread-safe: guarded by m_CreationMutex so concurrent pipeline creation
    // from the pre-warm worker thread and the render thread don't race on stats.
    void RecordPipelineCreation(double creationTimeMs, bool wasCached);
    bool IsValid() const
    {
        return m_PipelineCache != VK_NULL_HANDLE;
    }

    std::filesystem::path GetCacheFilePath() const;

  private:
    VkDevice m_Device;
    VkPipelineCache m_PipelineCache = VK_NULL_HANDLE;
    Config m_Config;
    Statistics m_Statistics;
    bool m_Initialized = false;

    std::mutex m_CreationMutex; // See LockForCreation()

    struct CacheHeader
    {
        uint32_t magic = 0x56504C43;
        uint32_t version = 1;
        uint32_t dataSize = 0;
        uint32_t checksum = 0;
        uint32_t compressed = 0;
    };

    bool CreateCacheDirectory();
    // Single-open read + validation of the on-disk cache file, returning the
    // decompressed VkPipelineCache payload. The cache file is machine-global
    // (GetPlatformCacheDirectory) and written by every engine process, so a
    // read can race another process's save: every size here is validated
    // before it reaches an allocation, and any inconsistency (torn write,
    // truncation, failed seek, checksum/size mismatch) yields nullopt — a
    // cold cache, never a throw out of device bring-up.
    std::optional<std::vector<uint8_t>> LoadCacheData();
    // Returns empty when compression is unavailable or fails (caller then
    // stores uncompressed and marks the header accordingly).
    std::vector<uint8_t> CompressData(const std::vector<uint8_t>& data);
    // Requires the exact decompressed size from the header (no estimates);
    // returns empty on unavailability, corruption, or size disagreement.
    std::vector<uint8_t> DecompressData(const std::vector<uint8_t>& compressedData,
                                        uint32_t expectedSize);
    uint32_t CalculateChecksum(const std::vector<uint8_t>& data);
};

class PipelineCreationTimer
{
  public:
    PipelineCreationTimer(VulkanPipelineCache* cache, bool wasCached);
    ~PipelineCreationTimer();

  private:
    VulkanPipelineCache* m_Cache;
    bool m_WasCached;
    std::chrono::high_resolution_clock::time_point m_StartTime;
};

#define PIPELINE_CREATION_TIMER(cache, wasCached) \
    PipelineCreationTimer timer(cache, wasCached)

} // namespace Rendering
} // namespace GameEngine
