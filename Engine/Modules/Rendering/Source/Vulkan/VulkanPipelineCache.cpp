/**
 * @file PipelineCache.cpp
 * @brief Vulkan Pipeline Cache implementation
 */

#include "VulkanPipelineCache.h"
#include "Logger/Logger.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <pwd.h>
#include <unistd.h>
#elif defined(__linux__)
#include <pwd.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cstring>

// For compression (optional - can be disabled)
#ifdef RENDERING_HAS_COMPRESSION
#include <zlib.h>
#endif

namespace GameEngine
{
namespace Rendering
{

VulkanPipelineCache::VulkanPipelineCache(VkDevice device)
    : VulkanPipelineCache(device, Config{})
{
}

VulkanPipelineCache::VulkanPipelineCache(VkDevice device, const Config& config)
    : m_Device(device), m_Config(config)
{
}

VulkanPipelineCache::~VulkanPipelineCache()
{
    Shutdown();
}

std::string VulkanPipelineCache::GetPlatformCacheDirectory()
{
#ifdef _WIN32
    // Windows: Use %LOCALAPPDATA%/GameEngine/PipelineCache/
    char* localAppData = nullptr;
    size_t len = 0;
    if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") == 0 && localAppData != nullptr)
    {
        std::string cachePath = std::string(localAppData) + "\\GameEngine\\PipelineCache\\";
        free(localAppData);
        return cachePath;
    }

    // Fallback to %APPDATA%
    if (_dupenv_s(&localAppData, &len, "APPDATA") == 0 && localAppData != nullptr)
    {
        std::string cachePath = std::string(localAppData) + "\\GameEngine\\PipelineCache\\";
        free(localAppData);
        return cachePath;
    }

#elif defined(__APPLE__)
    // macOS: Use ~/Library/Caches/GameEngine/PipelineCache/
    const char* homeDir = getenv("HOME");
    if (homeDir == nullptr)
    {
        struct passwd* pw = getpwuid(getuid());
        if (pw != nullptr)
        {
            homeDir = pw->pw_dir;
        }
    }

    if (homeDir != nullptr)
    {
        return std::string(homeDir) + "/Library/Caches/GameEngine/PipelineCache/";
    }

#elif defined(__linux__)
    // Linux: Use ~/.cache/GameEngine/PipelineCache/
    const char* xdgCacheHome = getenv("XDG_CACHE_HOME");
    if (xdgCacheHome != nullptr)
    {
        return std::string(xdgCacheHome) + "/GameEngine/PipelineCache/";
    }

    const char* homeDir = getenv("HOME");
    if (homeDir == nullptr)
    {
        struct passwd* pw = getpwuid(getuid());
        if (pw != nullptr)
        {
            homeDir = pw->pw_dir;
        }
    }

    if (homeDir != nullptr)
    {
        return std::string(homeDir) + "/.cache/GameEngine/PipelineCache/";
    }
#endif

    // Fallback: Use relative to executable location
    try
    {
        std::filesystem::path exePath = std::filesystem::current_path();
        return (exePath / "cache" / "pipelines").string() + "/";
    }
    catch (const std::exception&)
    {
        // Ultimate fallback
        return "cache/pipelines/";
    }
}

bool VulkanPipelineCache::Initialize()
{
    if (m_Initialized)
    {
        return true;
    }

    // Create cache directory if it doesn't exist
    if (!CreateCacheDirectory())
    {
        std::cerr << "PipelineCache: Failed to create cache directory" << std::endl;
        return false;
    }

    // `vkCreatePipelineCache(VK_NULL_HANDLE, …)` is undefined behavior — some
    // loaders crash, others hang. Bail before touching Vulkan when the device
    // is null (test harness path; production never hits this).
    if (m_Device == VK_NULL_HANDLE)
    {
        return false;
    }

    // Create Vulkan pipeline cache
    VkPipelineCacheCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    // Seed from the persisted cache when a valid file exists. LoadCacheData
    // performs the single read + validation and returns the DECOMPRESSED
    // vkGetPipelineCacheData payload (re-reading the file here raced
    // concurrent saves from other engine processes, and handed the still-
    // compressed bytes to the driver — an ignored blob at best).
    std::optional<std::vector<uint8_t>> cacheData = LoadCacheData();
    if (cacheData)
    {
        createInfo.initialDataSize = cacheData->size();
        createInfo.pInitialData = cacheData->data();
    }

    VkResult result = vkCreatePipelineCache(m_Device, &createInfo, nullptr, &m_PipelineCache);
    if (result != VK_SUCCESS)
    {
        std::cerr << "PipelineCache: Failed to create Vulkan pipeline cache: " << result << std::endl;
        return false;
    }

    m_Initialized = true;

    return true;
}

void VulkanPipelineCache::Shutdown(bool saveToDisk)
{
    if (!m_Initialized)
    {
        return;
    }

    // Save cache before shutdown (skipped during device rebuild — see header).
    if (saveToDisk)
        SaveCache();

    // Destroy Vulkan pipeline cache
    if (m_PipelineCache != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(m_Device, m_PipelineCache, nullptr);
        m_PipelineCache = VK_NULL_HANDLE;
    }

    m_Initialized = false;
}

bool VulkanPipelineCache::SaveCache()
{
    if (m_PipelineCache == VK_NULL_HANDLE)
    {
        return false;
    }

    // Get cache data size
    size_t cacheSize = 0;
    VkResult result = vkGetPipelineCacheData(m_Device, m_PipelineCache, &cacheSize, nullptr);
    if (result != VK_SUCCESS || cacheSize == 0)
    {
        return false;
    }

    // Get cache data
    std::vector<uint8_t> cacheData(cacheSize);
    result = vkGetPipelineCacheData(m_Device, m_PipelineCache, &cacheSize, cacheData.data());
    if (result != VK_SUCCESS)
    {
        std::cerr << "PipelineCache: Failed to get cache data: " << result << std::endl;
        return false;
    }

    // Compress when enabled AND the codec actually ran — the header flag must
    // describe the bytes on disk, not the config: a "compressed" label on
    // uncompressed bytes (zlib-less build) poisons the file for every reader.
    std::vector<uint8_t> compressed;
    if (m_Config.EnableCompression)
        compressed = CompressData(cacheData);
    const bool wasCompressed = !compressed.empty();
    const std::vector<uint8_t>& finalData = wasCompressed ? compressed : cacheData;

    // Create cache header
    CacheHeader header;
    header.version = m_Config.CacheVersion;
    header.dataSize = static_cast<uint32_t>(cacheData.size());
    header.checksum = CalculateChecksum(cacheData);
    header.compressed = wasCompressed ? 1 : 0;

    // The cache file is machine-global: every engine process (editor, tests,
    // player) saves here on device teardown while others may be reading it at
    // device bring-up. Publish atomically — write a per-process temp file,
    // then rename over the destination — so a reader can never observe the
    // truncate-then-append window of an in-place rewrite. That window is what
    // turned a concurrent save into "vector too long" in an unrelated
    // process's device init (order-dependent test flakes, 2026-07).
    auto cachePath = GetCacheFilePath();
    {
        std::error_code ec;
        std::filesystem::create_directories(cachePath.parent_path(), ec);
        if (ec)
            Logger::Log::Warning("PipelineCache: create_directories failed: {} path={}", ec.message(), cachePath.parent_path().string());
    }
    std::filesystem::path tempPath = cachePath;
    tempPath += "." + std::to_string(
#ifdef _WIN32
        static_cast<unsigned long>(GetCurrentProcessId())
#else
        static_cast<unsigned long>(getpid())
#endif
        ) + ".tmp";

    Logger::Log::Info("PipelineCache: Saving {} bytes to {}", cacheData.size(), cachePath.string());
    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            Logger::Log::Warning("PipelineCache: failed to open temp cache file for writing: {}",
                                 tempPath.string());
            return false;
        }
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(finalData.data()), finalData.size());
        if (!file)
        {
            file.close();
            std::error_code ec;
            std::filesystem::remove(tempPath, ec);
            Logger::Log::Warning("PipelineCache: failed writing temp cache file: {}", tempPath.string());
            return false;
        }
    }

    std::error_code renameEc;
    std::filesystem::rename(tempPath, cachePath, renameEc);
    if (renameEc)
    {
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
        Logger::Log::Warning("PipelineCache: atomic publish failed: {} ({} -> {})",
                             renameEc.message(), tempPath.string(), cachePath.string());
        return false;
    }

    m_Statistics.CacheSize = cacheSize;

    return true;
}

bool VulkanPipelineCache::LoadCache()
{
    return LoadCacheData().has_value();
}

std::optional<std::vector<uint8_t>> VulkanPipelineCache::LoadCacheData()
{
    // A concurrent writer (another engine process saving on its device
    // teardown) or a torn file must degrade to a cold cache — never throw
    // and never hand unvalidated sizes to an allocation. try/catch is the
    // final backstop (e.g. bad_alloc); every expected inconsistency is
    // handled explicitly below.
    try
    {
        auto cachePath = GetCacheFilePath();
        std::ifstream file(cachePath, std::ios::binary);
        if (!file.is_open())
        {
            return std::nullopt;
        }

        // Read and validate header
        CacheHeader header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (file.gcount() != sizeof(header))
        {
            return std::nullopt;
        }

        if (header.magic != 0x56504C43 || header.version != m_Config.CacheVersion)
        {
            return std::nullopt;
        }
        if (header.dataSize == 0 || header.dataSize > m_Config.MaxCacheSize)
        {
            Logger::Log::Warning("PipelineCache: implausible dataSize {} in {}; ignoring cache",
                                 header.dataSize, cachePath.string());
            return std::nullopt;
        }

        // Determine payload size from the file end. tellg reports -1 on
        // failure, and a truncated file can be shorter than the header —
        // both previously flowed into `fileSize - sizeof(header)` as size_t
        // and exploded a vector resize ("vector too long" in SetUp of
        // whichever test's device init hit the torn window).
        file.seekg(0, std::ios::end);
        const std::streamoff endPos = file.tellg();
        if (endPos < static_cast<std::streamoff>(sizeof(header)))
        {
            return std::nullopt;
        }
        const size_t payloadSize = static_cast<size_t>(endPos) - sizeof(header);
        if (payloadSize == 0 || payloadSize > m_Config.MaxCacheSize)
        {
            return std::nullopt;
        }

        file.seekg(sizeof(header));
        std::vector<uint8_t> fileData(payloadSize);
        file.read(reinterpret_cast<char*>(fileData.data()), payloadSize);
        if (file.gcount() != static_cast<std::streamsize>(payloadSize))
        {
            return std::nullopt;
        }
        file.close();

        // Decompress if needed. The header carries the exact uncompressed
        // size, so the output buffer is allocated exactly — no estimates.
        std::vector<uint8_t> cacheData;
        if (header.compressed)
        {
            cacheData = DecompressData(fileData, header.dataSize);
            if (cacheData.empty())
            {
                return std::nullopt;
            }
        }
        else
        {
            cacheData = std::move(fileData);
        }

        // Structural validation is unconditional; the (weak) content checksum
        // stays behind EnableValidation as before.
        if (cacheData.size() != header.dataSize)
        {
            return std::nullopt;
        }
        if (m_Config.EnableValidation)
        {
            const uint32_t checksum = CalculateChecksum(cacheData);
            if (checksum != header.checksum)
            {
                Logger::Log::Warning("PipelineCache: checksum mismatch in {}; ignoring cache",
                                     cachePath.string());
                return std::nullopt;
            }
        }

        return cacheData;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("PipelineCache: load failed ({}); starting cold", e.what());
        return std::nullopt;
    }
}

void VulkanPipelineCache::ClearCache()
{
    auto cachePath = GetCacheFilePath();
    if (std::filesystem::exists(cachePath))
    {
        std::filesystem::remove(cachePath);
    }

    // Reset statistics
    m_Statistics = {};
}

void VulkanPipelineCache::RecordPipelineCreation(double creationTimeMs, bool wasCached)
{
    std::lock_guard<std::mutex> lock(m_CreationMutex);
    m_Statistics.TotalPipelines++;

    if (wasCached)
    {
        m_Statistics.CacheHits++;
        m_Statistics.AverageCachedCreationTime =
            (m_Statistics.AverageCachedCreationTime * (m_Statistics.CacheHits - 1) + creationTimeMs) / m_Statistics.CacheHits;
    }
    else
    {
        m_Statistics.CacheMisses++;
        m_Statistics.AverageCreationTime =
            (m_Statistics.AverageCreationTime * (m_Statistics.CacheMisses - 1) + creationTimeMs) / m_Statistics.CacheMisses;
    }
}

std::filesystem::path VulkanPipelineCache::GetCacheFilePath() const
{
    return std::filesystem::path(m_Config.CacheDirectory) / m_Config.CacheFileName;
}

bool VulkanPipelineCache::CreateCacheDirectory()
{
    try
    {
        std::filesystem::create_directories(m_Config.CacheDirectory);
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "PipelineCache: Failed to create directory: " << e.what() << std::endl;
        return false;
    }
}

std::vector<uint8_t> VulkanPipelineCache::CompressData(const std::vector<uint8_t>& data)
{
#ifdef RENDERING_HAS_COMPRESSION
    uLongf compressedSize = compressBound(static_cast<uLong>(data.size()));
    std::vector<uint8_t> compressed(compressedSize);

    int result = compress(compressed.data(), &compressedSize, data.data(),
                          static_cast<uLong>(data.size()));
    if (result == Z_OK)
    {
        compressed.resize(compressedSize);
        return compressed;
    }
#endif
    // Empty signals "not compressed" — the caller stores the original bytes
    // and must mark the header uncompressed.
    return {};
}

std::vector<uint8_t> VulkanPipelineCache::DecompressData(const std::vector<uint8_t>& compressedData,
                                                         uint32_t expectedSize)
{
#ifdef RENDERING_HAS_COMPRESSION
    // The header's dataSize is authoritative (previously a `size * 4`
    // estimate silently failed on any cache with a >4x ratio, discarding the
    // cache every launch). LoadCacheData bounds expectedSize before calling.
    uLongf decompressedSize = expectedSize;
    std::vector<uint8_t> decompressed(decompressedSize);

    int result = uncompress(decompressed.data(), &decompressedSize, compressedData.data(),
                            static_cast<uLong>(compressedData.size()));
    if (result == Z_OK && decompressedSize == expectedSize)
    {
        return decompressed;
    }
#else
    (void)compressedData;
    (void)expectedSize;
#endif
    // Empty signals corruption / unavailability; the caller discards the cache.
    return {};
}

uint32_t VulkanPipelineCache::CalculateChecksum(const std::vector<uint8_t>& data)
{
    // Simple CRC32-like checksum
    uint32_t checksum = 0;
    for (uint8_t byte : data)
    {
        checksum = (checksum << 1) ^ byte;
    }
    return checksum;
}

// Pipeline Creation Timer Implementation
PipelineCreationTimer::PipelineCreationTimer(VulkanPipelineCache* cache, bool wasCached)
    : m_Cache(cache), m_WasCached(wasCached)
{
    m_StartTime = std::chrono::high_resolution_clock::now();
}

PipelineCreationTimer::~PipelineCreationTimer()
{
    if (m_Cache)
    {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - m_StartTime);
        double timeMs = duration.count() / 1000.0;
        m_Cache->RecordPipelineCreation(timeMs, m_WasCached);
    }
}

} // namespace Rendering
} // namespace GameEngine
