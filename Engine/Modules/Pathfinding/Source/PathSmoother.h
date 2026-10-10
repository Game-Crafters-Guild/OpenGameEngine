#pragma once

#include "Pathfinding/PathfindingTypes.h"

#include <vector>

namespace GameEngine::Pathfinding
{

class GridMap;

// Simplifies a grid path by removing intermediate waypoints where line-of-sight
// exists between non-adjacent points. Uses the grid's Raycast to verify visibility.
class PathSmoother
{
public:
    // Smooths the path in-place. Returns the new point count.
    // The input points are world-space coordinates from GridMap::FindPath.
    static uint32 Smooth(const GridMap& grid, std::vector<PathPoint>& points);
};

} // namespace GameEngine::Pathfinding
