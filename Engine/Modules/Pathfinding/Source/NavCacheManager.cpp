#include "Pathfinding/NavCacheManager.h"
#include "AssetCore/GUID.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace GameEngine::Pathfinding {

namespace {

static constexpr char kMagic[4] = {'N', 'C', 'C', 'H'};
static constexpr uint32 kVersion = 1;

// Cache binary format:
// [magic "NCCH"][version uint32][hashSize uint32][sourceHash bytes][payloadSize uint32][payload bytes]
static constexpr size_t kMinHeaderSize = 4 + sizeof(uint32) + sizeof(uint32); // magic + version + hashSize

static_assert(sizeof(float32) == 4, "float32 must be 4 bytes for cache format");

template<typename T>
static void WriteField(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template<typename T>
static bool ReadField(const uint8* data, size_t& offset, size_t maxSize, T& outValue)
{
    if (offset + sizeof(T) > maxSize)
        return false;
    std::memcpy(&outValue, data + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

} // namespace

std::filesystem::path NavCacheManager::s_ProjectRoot;

uint64 NavCacheManager::ComputeFileContentHash(const std::filesystem::path& filePath)
{
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return 0;

    const auto fileSize = static_cast<size_t>(file.tellg());
    if (fileSize == 0)
        return 0;

    file.seekg(0, std::ios::beg);
    std::vector<uint8> buffer(fileSize);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize)))
        return 0;

    // FNV-1a 64-bit
    constexpr uint64 kFnvOffset = 14695981039346656037ull;
    constexpr uint64 kFnvPrime = 1099511628211ull;
    uint64 hash = kFnvOffset;
    for (uint8 byte : buffer)
    {
        hash ^= static_cast<uint64>(byte);
        hash *= kFnvPrime;
    }
    return hash;
}

void NavCacheManager::SetProjectRoot(const std::filesystem::path& root)
{
    s_ProjectRoot = root;
}

std::filesystem::path NavCacheManager::GetCachePath(const GUID& guid, const char* extension)
{
    return s_ProjectRoot / ".Cache" / "Navigation" / (guid.ToString() + extension);
}

bool NavCacheManager::SaveGridCache(const GUID& assetGuid,
                                    const uint8* sourceHash, uint32 hashSize,
                                    const float32* heights, uint32 cellCount)
{
    if (!sourceHash || !heights)
    {
        Logger::Log::Error("NavCacheManager::SaveGridCache: null data pointers");
        return false;
    }

    const auto cachePath = GetCachePath(assetGuid, ".gridcache");
    const auto tmpPath = std::filesystem::path(cachePath).concat(".tmp");

    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);
    if (ec)
    {
        Logger::Log::Error("NavCacheManager: failed to create cache directory: {}", ec.message());
        return false;
    }

    {
        std::ofstream file(tmpPath, std::ios::binary);
        if (!file.is_open())
        {
            Logger::Log::Error("NavCacheManager: failed to open cache file for writing: {}", tmpPath.string());
            return false;
        }

        file.write(kMagic, 4);
        WriteField(file, kVersion);
        WriteField(file, hashSize);
        file.write(reinterpret_cast<const char*>(sourceHash), hashSize);

        const uint32 payloadSize = cellCount * sizeof(float32);
        WriteField(file, payloadSize);
        file.write(reinterpret_cast<const char*>(heights),
                   static_cast<std::streamsize>(payloadSize));

        if (!file.good())
        {
            Logger::Log::Error("NavCacheManager: write error for grid cache: {}", tmpPath.string());
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    } // close file before rename

    return FileSystem::PublishFile(tmpPath, cachePath);
}

bool NavCacheManager::LoadGridCache(const GUID& assetGuid,
                                    const uint8* expectedSourceHash, uint32 hashSize,
                                    std::vector<float32>& outHeights)
{
    if (!expectedSourceHash)
    {
        return false;
    }

    const auto cachePath = GetCachePath(assetGuid, ".gridcache");

    std::ifstream file(cachePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        return false;
    }

    const auto fileSize = static_cast<size_t>(file.tellg());
    if (fileSize < kMinHeaderSize)
    {
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8> data(fileSize);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize)))
    {
        return false;
    }

    // Validate magic
    if (std::memcmp(data.data(), kMagic, 4) != 0)
    {
        return false;
    }

    size_t offset = 4;
    uint32 version = 0;
    if (!ReadField<uint32>(data.data(), offset, fileSize, version) || version != kVersion)
    {
        return false;
    }

    uint32 storedHashSize = 0;
    if (!ReadField<uint32>(data.data(), offset, fileSize, storedHashSize) || storedHashSize != hashSize)
    {
        return false;
    }

    if (offset + storedHashSize > fileSize)
    {
        return false;
    }

    if (std::memcmp(data.data() + offset, expectedSourceHash, hashSize) != 0)
    {
        return false;
    }
    offset += storedHashSize;

    uint32 payloadSize = 0;
    if (!ReadField<uint32>(data.data(), offset, fileSize, payloadSize))
    {
        return false;
    }

    if (offset + payloadSize > fileSize)
    {
        return false;
    }

    if (payloadSize % sizeof(float32) != 0)
    {
        return false;
    }

    const uint32 cellCount = payloadSize / sizeof(float32);
    outHeights.resize(cellCount);
    std::memcpy(outHeights.data(), data.data() + offset, payloadSize);

    return true;
}

bool NavCacheManager::SaveNavMeshCache(const GUID& assetGuid,
                                       const uint8* sourceHash, uint32 hashSize,
                                       const std::vector<uint8>& detourData)
{
    if (!sourceHash)
    {
        Logger::Log::Error("NavCacheManager::SaveNavMeshCache: null source hash");
        return false;
    }

    const auto cachePath = GetCachePath(assetGuid, ".navmeshcache");
    const auto tmpPath = std::filesystem::path(cachePath).concat(".tmp");

    std::error_code ec;
    std::filesystem::create_directories(cachePath.parent_path(), ec);
    if (ec)
    {
        Logger::Log::Error("NavCacheManager: failed to create cache directory: {}", ec.message());
        return false;
    }

    {
        std::ofstream file(tmpPath, std::ios::binary);
        if (!file.is_open())
        {
            Logger::Log::Error("NavCacheManager: failed to open cache file for writing: {}", tmpPath.string());
            return false;
        }

        file.write(kMagic, 4);
        WriteField(file, kVersion);
        WriteField(file, hashSize);
        file.write(reinterpret_cast<const char*>(sourceHash), hashSize);

        const uint32 payloadSize = static_cast<uint32>(detourData.size());
        WriteField(file, payloadSize);
        if (!detourData.empty())
        {
            file.write(reinterpret_cast<const char*>(detourData.data()),
                       static_cast<std::streamsize>(detourData.size()));
        }

        if (!file.good())
        {
            Logger::Log::Error("NavCacheManager: write error for navmesh cache: {}", tmpPath.string());
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    } // close file before rename

    return FileSystem::PublishFile(tmpPath, cachePath);
}

bool NavCacheManager::LoadNavMeshCache(const GUID& assetGuid,
                                       const uint8* expectedSourceHash, uint32 hashSize,
                                       std::vector<uint8>& outDetourData)
{
    if (!expectedSourceHash)
    {
        return false;
    }

    const auto cachePath = GetCachePath(assetGuid, ".navmeshcache");

    std::ifstream file(cachePath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        return false;
    }

    const auto fileSize = static_cast<size_t>(file.tellg());
    if (fileSize < kMinHeaderSize)
    {
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8> data(fileSize);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize)))
    {
        return false;
    }

    // Validate magic
    if (std::memcmp(data.data(), kMagic, 4) != 0)
    {
        return false;
    }

    size_t offset = 4;
    uint32 version = 0;
    if (!ReadField<uint32>(data.data(), offset, fileSize, version) || version != kVersion)
    {
        return false;
    }

    uint32 storedHashSize = 0;
    if (!ReadField<uint32>(data.data(), offset, fileSize, storedHashSize) || storedHashSize != hashSize)
    {
        return false;
    }

    if (offset + storedHashSize > fileSize)
    {
        return false;
    }

    if (std::memcmp(data.data() + offset, expectedSourceHash, hashSize) != 0)
    {
        return false;
    }
    offset += storedHashSize;

    uint32 payloadSize = 0;
    if (!ReadField<uint32>(data.data(), offset, fileSize, payloadSize))
    {
        return false;
    }

    if (offset + payloadSize > fileSize)
    {
        return false;
    }

    outDetourData.resize(payloadSize);
    if (payloadSize > 0)
    {
        std::memcpy(outDetourData.data(), data.data() + offset, payloadSize);
    }

    return true;
}

} // namespace GameEngine::Pathfinding
