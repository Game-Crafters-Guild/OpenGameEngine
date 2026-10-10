#include "AStarSolver.h"
#include "HexGridHelpers.h"
#include "Pathfinding/GridMap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace GameEngine::Pathfinding
{

static constexpr float32 kSqrt2 = 1.41421356f;
static constexpr float32 kPi = 3.14159265358979323846f;
static constexpr float32 kRadToDeg = 180.0f / kPi;
static constexpr float32 kReservationPenalty = 3.0f;
static constexpr float32 kInfinity = std::numeric_limits<float32>::max();

struct AStarNode
{
    uint32 CellIndex;
    float32 FCost;
};

struct NodeCompare
{
    bool operator()(const AStarNode& a, const AStarNode& b) const { return a.FCost > b.FCost; }
};

bool AStarSolver::Solve(const GridMap& grid, uint32 startIndex, uint32 endIndex,
                         uint32 excludeAgentId, float32 currentTime,
                         uint32 agentCellRadius,
                         std::vector<uint32>& outPathCellIndices)
{
    outPathCellIndices.clear();

    if (startIndex == endIndex)
    {
        outPathCellIndices.push_back(startIndex);
        return true;
    }

    const uint32 cellCount = grid.GetCellCount();
    if (startIndex >= cellCount || endIndex >= cellCount)
        return false;

    uint32 startX = 0, startZ = 0, endX = 0, endZ = 0;
    grid.CellFromIndex(startIndex, startX, startZ);
    grid.CellFromIndex(endIndex, endX, endZ);
    if (grid.IsCellBlocked(startX, startZ) || grid.IsCellBlocked(endX, endZ))
        return false;

    const uint32 maxIterations = std::min(cellCount * 2, static_cast<uint32>(65536));

    m_GScore.resize(cellCount);
    m_Parent.resize(cellCount);
    m_Closed.resize(cellCount);

    std::fill_n(m_GScore.data(), cellCount, kInfinity);
    std::fill_n(m_Parent.data(), cellCount, UINT32_MAX);
    std::memset(m_Closed.data(), 0, cellCount);

    std::vector<AStarNode> openList;
    openList.reserve(256);

    m_GScore[startIndex] = 0.0f;
    openList.push_back({startIndex, Heuristic(grid, startIndex, endIndex)});

    uint32 iterations = 0;
    while (!openList.empty() && iterations < maxIterations)
    {
        ++iterations;

        std::pop_heap(openList.begin(), openList.end(), NodeCompare{});
        AStarNode current = openList.back();
        openList.pop_back();

        if (current.CellIndex == endIndex)
        {
            uint32 idx = endIndex;
            while (idx != UINT32_MAX)
            {
                outPathCellIndices.push_back(idx);
                idx = m_Parent[idx];
            }
            std::reverse(outPathCellIndices.begin(), outPathCellIndices.end());
            return true;
        }

        if (m_Closed[current.CellIndex])
            continue;
        m_Closed[current.CellIndex] = 1;

        uint32 neighbors[8];
        float32 baseCosts[8];
        uint32 neighborCount = 0;
        GetNeighbors(grid, current.CellIndex, neighbors, baseCosts, neighborCount);

        uint32 cx = 0, cz = 0;
        grid.CellFromIndex(current.CellIndex, cx, cz);
        const float32 currentHeight = grid.GetCellHeight(cx, cz);

        for (uint32 i = 0; i < neighborCount; ++i)
        {
            const uint32 neighbor = neighbors[i];
            if (m_Closed[neighbor])
                continue;

            uint32 nx = 0, nz = 0;
            grid.CellFromIndex(neighbor, nx, nz);

            // Check if the agent footprint at this cell is fully walkable
            bool footprintClear = true;
            if (agentCellRadius > 0)
            {
                const auto& settings = grid.GetSettings();
                const int32 r = static_cast<int32>(agentCellRadius);
                for (int32 fdz = -r; fdz <= r && footprintClear; ++fdz)
                {
                    for (int32 fdx = -r; fdx <= r && footprintClear; ++fdx)
                    {
                        const int32 fx = static_cast<int32>(nx) + fdx;
                        const int32 fz = static_cast<int32>(nz) + fdz;
                        if (fx < 0 || fx >= static_cast<int32>(settings.Width) ||
                            fz < 0 || fz >= static_cast<int32>(settings.Depth))
                        {
                            footprintClear = false;
                            continue;
                        }
                        if (grid.IsCellBlocked(static_cast<uint32>(fx), static_cast<uint32>(fz)))
                        {
                            footprintClear = false;
                        }
                    }
                }
            }
            else
            {
                footprintClear = !grid.IsCellBlocked(nx, nz);
            }

            if (!footprintClear)
                continue;

            const float32 heightDiff = std::abs(grid.GetCellHeight(nx, nz) - currentHeight);
            const float32 horizontalDist = baseCosts[i] * grid.GetSettings().CellSize;

            if (heightDiff > grid.GetSettings().MaxStepHeight)
                continue;

            if (horizontalDist > 0.0f)
            {
                const float32 slopeDeg = std::atan2(heightDiff, horizontalDist) * kRadToDeg;
                if (slopeDeg > grid.GetSettings().MaxSlope)
                    continue;
            }

            float32 moveCost = baseCosts[i] * grid.GetCellCost(nx, nz);

            const CellReservation& reservation = grid.GetCellReservation(nx, nz);
            if (reservation.IsActive(currentTime) && reservation.AgentId != excludeAgentId)
            {
                // Occupancy is a tax, not a wall. Movement still refuses
                // another agent's body cell. Dest stays searchable so a
                // dest-allowed close can finish.
                moveCost *= (1.0f + kReservationPenalty);
            }

            const float32 tentativeG = m_GScore[current.CellIndex] + moveCost;
            if (tentativeG < m_GScore[neighbor])
            {
                m_GScore[neighbor] = tentativeG;
                m_Parent[neighbor] = current.CellIndex;
                const float32 fCost = tentativeG + Heuristic(grid, neighbor, endIndex);
                openList.push_back({neighbor, fCost});
                std::push_heap(openList.begin(), openList.end(), NodeCompare{});
            }
        }
    }

    return false;
}

void AStarSolver::GetNeighbors(const GridMap& grid, uint32 cellIndex,
                                uint32* outNeighbors, float32* outBaseCosts, uint32& outCount) const
{
    outCount = 0;
    uint32 cx = 0, cz = 0;
    grid.CellFromIndex(cellIndex, cx, cz);
    const auto& settings = grid.GetSettings();

    if (settings.Type == GridType::Square)
    {
        // Cardinals first (indices 0-3), then diagonals (indices 4-7).
        static constexpr int32 dx[] = {0, 0, 1, -1, 1, -1, 1, -1};
        static constexpr int32 dz[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        static constexpr float32 costs[] = {1.0f, 1.0f, 1.0f, 1.0f, kSqrt2, kSqrt2, kSqrt2, kSqrt2};
        // For each diagonal, the two cardinal neighbors that must be walkable
        // to prevent corner-cutting through blocked cells.
        // Diagonal (1,-1) needs (1,0)[idx 2] and (0,-1)[idx 0]
        // Diagonal (-1,-1) needs (-1,0)[idx 3] and (0,-1)[idx 0]
        // Diagonal (1,1) needs (1,0)[idx 2] and (0,1)[idx 1]
        // Diagonal (-1,1) needs (-1,0)[idx 3] and (0,1)[idx 1]
        static constexpr uint32 diagCardinalA[] = {2, 3, 2, 3};
        static constexpr uint32 diagCardinalB[] = {0, 0, 1, 1};

        // Track which cardinal directions are walkable for the corner-cut check.
        bool cardinalWalkable[4] = {false, false, false, false};

        // Add cardinal neighbors (always eligible)
        for (uint32 i = 0; i < 4; ++i)
        {
            const int32 nx = static_cast<int32>(cx) + dx[i];
            const int32 nz = static_cast<int32>(cz) + dz[i];
            if (nx >= 0 && nx < static_cast<int32>(settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(settings.Depth))
            {
                cardinalWalkable[i] = !grid.IsCellBlocked(static_cast<uint32>(nx), static_cast<uint32>(nz));
                outNeighbors[outCount] = grid.CellIndex(static_cast<uint32>(nx), static_cast<uint32>(nz));
                outBaseCosts[outCount] = costs[i];
                ++outCount;
            }
        }

        // Add diagonal neighbors only if both adjacent cardinal cells are walkable.
        for (uint32 i = 4; i < 8; ++i)
        {
            const uint32 diagIdx = i - 4;
            if (!cardinalWalkable[diagCardinalA[diagIdx]] || !cardinalWalkable[diagCardinalB[diagIdx]])
                continue;

            const int32 nx = static_cast<int32>(cx) + dx[i];
            const int32 nz = static_cast<int32>(cz) + dz[i];
            if (nx >= 0 && nx < static_cast<int32>(settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(settings.Depth))
            {
                outNeighbors[outCount] = grid.CellIndex(static_cast<uint32>(nx), static_cast<uint32>(nz));
                outBaseCosts[outCount] = costs[i];
                ++outCount;
            }
        }
    }
    else
    {
        int32 dx[6], dz[6];
        uint32 hexCount = 0;
        HexGrid::GetNeighborOffsets(cz, dx, dz, hexCount);

        for (uint32 i = 0; i < hexCount; ++i)
        {
            const int32 nx = static_cast<int32>(cx) + dx[i];
            const int32 nz = static_cast<int32>(cz) + dz[i];
            if (nx >= 0 && nx < static_cast<int32>(settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(settings.Depth))
            {
                outNeighbors[outCount] = grid.CellIndex(static_cast<uint32>(nx), static_cast<uint32>(nz));
                outBaseCosts[outCount] = 1.0f;
                ++outCount;
            }
        }
    }
}

float32 AStarSolver::Heuristic(const GridMap& grid, uint32 from, uint32 to) const
{
    uint32 fx = 0, fz = 0, tx = 0, tz = 0;
    grid.CellFromIndex(from, fx, fz);
    grid.CellFromIndex(to, tx, tz);

    if (grid.GetSettings().Type == GridType::Square)
    {
        const float32 dx = std::abs(static_cast<float32>(fx) - static_cast<float32>(tx));
        const float32 dz = std::abs(static_cast<float32>(fz) - static_cast<float32>(tz));
        return (dx + dz) + (kSqrt2 - 2.0f) * std::min(dx, dz);
    }
    else
    {
        return HexGrid::Distance(fx, fz, tx, tz);
    }
}

} // namespace GameEngine::Pathfinding
