#pragma once

#include "Types/Types.h"
#include <filesystem>
#include <vector>

namespace GameEngine {
class GUID;
}

namespace GameEngine::Pathfinding {

class NavCacheManager {
public:
    // Set the project root (cache stored in <root>/.Cache/Navigation/)
    static void SetProjectRoot(const std::filesystem::path& root);

    // Grid cache: stores baked heights
    static bool SaveGridCache(const GUID& assetGuid,
                              const uint8* sourceHash, uint32 hashSize,
                              const float32* heights, uint32 cellCount);
    static bool LoadGridCache(const GUID& assetGuid,
                              const uint8* expectedSourceHash, uint32 hashSize,
                              std::vector<float32>& outHeights);

    // Compute FNV-1a 64-bit hash of a file's contents. Returns 0 on failure.
    static uint64 ComputeFileContentHash(const std::filesystem::path& filePath);

    // NavMesh cache: stores serialized Detour data
    static bool SaveNavMeshCache(const GUID& assetGuid,
                                 const uint8* sourceHash, uint32 hashSize,
                                 const std::vector<uint8>& detourData);
    static bool LoadNavMeshCache(const GUID& assetGuid,
                                 const uint8* expectedSourceHash, uint32 hashSize,
                                 std::vector<uint8>& outDetourData);

private:
    static std::filesystem::path GetCachePath(const GUID& guid, const char* extension);
    static std::filesystem::path s_ProjectRoot;
};

} // namespace GameEngine::Pathfinding
