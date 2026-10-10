#pragma once

#include "Pathfinding/PathfindingTypes.h"

#include <mutex>
#include <vector>

namespace GameEngine::Pathfinding
{

class PathBuffer
{
public:
    PathHandle AllocatePath(const PathPoint* points, uint32 count);
    void FreePath(PathHandle handle);

    uint32 GetPointCount(PathHandle handle) const;
    PathPoint GetPoint(PathHandle handle, uint32 index) const;

    // Copy points into caller-owned buffer. Returns number of points copied.
    // Safe alternative to returning a raw pointer across a lock boundary.
    uint32 CopyPoints(PathHandle handle, PathPoint* outBuffer, uint32 bufferCapacity) const;

    void Clear();

private:
    struct Entry
    {
        std::vector<PathPoint> Points;
        uint32 Generation = 0;
        bool Active = false;
    };

    mutable std::mutex m_Mutex;
    std::vector<Entry> m_Entries;
    std::vector<uint32> m_FreeList;
    uint32 m_NextGeneration = 1;
};

} // namespace GameEngine::Pathfinding
