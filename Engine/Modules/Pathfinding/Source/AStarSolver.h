#pragma once

#include "Types/Types.h"

#include <vector>

namespace GameEngine::Pathfinding
{

class GridMap;

// A* solver with reusable scratch memory. Each instance owns its own working
// buffers, so concurrent solves on different AStarSolver instances are safe.
// For best performance, reuse the same solver across multiple Solve() calls
// (the buffers grow to high-water mark and are reused without reallocation).
class AStarSolver
{
public:
    // Solves A* on the grid. Working memory is owned by this solver instance.
    // Thread-safe as long as each thread uses its own AStarSolver.
    bool Solve(const GridMap& grid, uint32 startIndex, uint32 endIndex,
               uint32 excludeAgentId, float32 currentTime,
               uint32 agentCellRadius,
               std::vector<uint32>& outPathCellIndices);

private:
    void GetNeighbors(const GridMap& grid, uint32 cellIndex,
                      uint32* outNeighbors, float32* outBaseCosts, uint32& outCount) const;
    float32 Heuristic(const GridMap& grid, uint32 from, uint32 to) const;

    // Reusable working buffers (grow to high-water mark, never shrink).
    // OpenList is stack-local in Solve() since it's cleared each call.
    std::vector<float32> m_GScore;
    std::vector<uint32> m_Parent;
    std::vector<uint8> m_Closed;
};

} // namespace GameEngine::Pathfinding
