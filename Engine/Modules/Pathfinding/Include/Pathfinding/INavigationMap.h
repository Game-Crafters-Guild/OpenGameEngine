#pragma once

#include "Pathfinding/PathfindingTypes.h"

namespace GameEngine::Pathfinding
{

class PathBuffer;

class INavigationMap
{
public:
    virtual ~INavigationMap() = default;

    virtual PathStatus FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const = 0;

    virtual bool IsPointNavigable(float32 x, float32 y, float32 z, float32 radius) const = 0;

    virtual bool GetClosestNavigablePoint(float32 x, float32 y, float32 z,
                                           float32 searchRadius,
                                           float32& outX, float32& outY, float32& outZ) const = 0;

    virtual bool Raycast(float32 startX, float32 startY, float32 startZ,
                         float32 endX, float32 endY, float32 endZ,
                         float32& hitX, float32& hitY, float32& hitZ) const = 0;
};

} // namespace GameEngine::Pathfinding
