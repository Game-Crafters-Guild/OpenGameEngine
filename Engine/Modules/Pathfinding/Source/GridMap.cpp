#include "Pathfinding/GridMap.h"
#include "AStarSolver.h"
#include "HexGridHelpers.h"
#include "Logger/Logger.h"
#include "PathSmoother.h"
#include "Pathfinding/PathBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>

namespace GameEngine::Pathfinding
{

GridMap::GridMap(const GridSettings& settings)
    : m_Settings(settings)
{
    if (m_Settings.Width == 0)
    {
        Logger::Log::Error("[GridMap] Width is 0, defaulting to 1");
        m_Settings.Width = 1;
    }
    if (m_Settings.Depth == 0)
    {
        Logger::Log::Error("[GridMap] Depth is 0, defaulting to 1");
        m_Settings.Depth = 1;
    }
    if (m_Settings.CellSize <= 0.0f)
    {
        Logger::Log::Error("[GridMap] CellSize is {} (must be > 0), defaulting to 1.0", m_Settings.CellSize);
        m_Settings.CellSize = 1.0f;
    }

    // Overflow check
    const uint64 cellCount64 = static_cast<uint64>(m_Settings.Width) * static_cast<uint64>(m_Settings.Depth);
    if (cellCount64 > static_cast<uint64>(UINT32_MAX))
    {
        Logger::Log::Error("[GridMap] Width*Depth overflows uint32 ({}x{}), clamping to 1x1",
                           m_Settings.Width, m_Settings.Depth);
        m_Settings.Width = 1;
        m_Settings.Depth = 1;
    }

    const uint32 cellCount = m_Settings.Width * m_Settings.Depth;
    m_Costs.resize(cellCount, 1.0f);
    m_Heights.resize(cellCount, 0.0f);
    m_Blocked.resize(cellCount, 0);
    m_DynamicBlocked.resize(cellCount, 0);
    m_Reservations.resize(cellCount);
}

const GridSettings& GridMap::GetSettings() const
{
    return m_Settings;
}

uint32 GridMap::GetCellCount() const
{
    return m_Settings.Width * m_Settings.Depth;
}

uint32 GridMap::GetTopologyVersion() const
{
    return m_TopologyVersion;
}

uint32 GridMap::CellIndex(uint32 cellX, uint32 cellZ) const
{
    return cellZ * m_Settings.Width + cellX;
}

void GridMap::CellFromIndex(uint32 index, uint32& outCellX, uint32& outCellZ) const
{
    outCellX = index % m_Settings.Width;
    outCellZ = index / m_Settings.Width;
}

bool GridMap::IsValidCell(uint32 cellX, uint32 cellZ) const
{
    return cellX < m_Settings.Width && cellZ < m_Settings.Depth;
}

float32 GridMap::GetCellCost(uint32 cellX, uint32 cellZ) const
{
    if (!IsValidCell(cellX, cellZ))
        return 1.0f;
    return m_Costs[CellIndex(cellX, cellZ)];
}

void GridMap::SetCellCost(uint32 cellX, uint32 cellZ, float32 cost)
{
    if (!IsValidCell(cellX, cellZ))
        return;
    m_Costs[CellIndex(cellX, cellZ)] = cost;
    ++m_TopologyVersion;
}

bool GridMap::IsCellBlocked(uint32 cellX, uint32 cellZ) const
{
    if (!IsValidCell(cellX, cellZ))
        return true;
    uint32 idx = CellIndex(cellX, cellZ);
    return m_Blocked[idx] != 0 || m_DynamicBlocked[idx] != 0;
}

void GridMap::SetCellBlocked(uint32 cellX, uint32 cellZ, bool blocked)
{
    if (!IsValidCell(cellX, cellZ))
        return;
    m_Blocked[CellIndex(cellX, cellZ)] = blocked ? 1 : 0;
    ++m_TopologyVersion;
}

float32 GridMap::GetCellHeight(uint32 cellX, uint32 cellZ) const
{
    if (!IsValidCell(cellX, cellZ))
        return 0.0f;
    return m_Heights[CellIndex(cellX, cellZ)];
}

void GridMap::SetCellHeight(uint32 cellX, uint32 cellZ, float32 height)
{
    if (!IsValidCell(cellX, cellZ))
        return;
    m_Heights[CellIndex(cellX, cellZ)] = height;
    ++m_TopologyVersion;
}

void GridMap::SetCellDynamicBlocked(uint32 cellX, uint32 cellZ, bool blocked)
{
    if (!IsValidCell(cellX, cellZ))
        return;
    m_DynamicBlocked[CellIndex(cellX, cellZ)] = blocked ? 1 : 0;
}

void GridMap::ClearAllDynamicBlocked()
{
    std::memset(m_DynamicBlocked.data(), 0, m_DynamicBlocked.size());
}

bool GridMap::IsCellDynamicBlocked(uint32 cellX, uint32 cellZ) const
{
    if (!IsValidCell(cellX, cellZ))
        return false;
    return m_DynamicBlocked[CellIndex(cellX, cellZ)] != 0;
}

void GridMap::ReserveCell(uint32 cellX, uint32 cellZ, uint32 agentId, float32 expirationTime,
                          bool occupancy)
{
    if (!IsValidCell(cellX, cellZ))
        return;
    std::unique_lock lock(m_ReservationMutex);
    const uint32 index = CellIndex(cellX, cellZ);
    auto& reservation = m_Reservations[index];
    const bool sameOwner = reservation.AgentId == agentId;
    reservation.AgentId = agentId;
    reservation.ExpirationTime = expirationTime;
    if (sameOwner)
        reservation.Occupancy = (reservation.Occupancy != 0 || occupancy) ? 1 : 0;
    else
        reservation.Occupancy = occupancy ? 1 : 0;
    if (agentId == 0)
        return;

    auto& cells = m_AgentCells[agentId];
    if (std::find(cells.begin(), cells.end(), index) == cells.end())
        cells.push_back(index);
}

void GridMap::ClearCellReservation(uint32 cellX, uint32 cellZ)
{
    if (!IsValidCell(cellX, cellZ))
        return;
    std::unique_lock lock(m_ReservationMutex);
    auto& reservation = m_Reservations[CellIndex(cellX, cellZ)];
    reservation.AgentId = 0;
    reservation.ExpirationTime = 0.0f;
    reservation.Occupancy = 0;
}

CellReservation GridMap::GetCellReservation(uint32 cellX, uint32 cellZ) const
{
    if (!IsValidCell(cellX, cellZ))
        return CellReservation{};
    std::shared_lock lock(m_ReservationMutex);
    return m_Reservations[CellIndex(cellX, cellZ)];
}

void GridMap::ClearReservationsForAgent(uint32 agentId)
{
    if (agentId == 0)
        return;
    std::unique_lock lock(m_ReservationMutex);
    auto it = m_AgentCells.find(agentId);
    if (it == m_AgentCells.end())
        return;
    for (uint32 index : it->second)
    {
        if (index < m_Reservations.size() && m_Reservations[index].AgentId == agentId)
        {
            m_Reservations[index].AgentId = 0;
            m_Reservations[index].ExpirationTime = 0.0f;
            m_Reservations[index].Occupancy = 0;
        }
    }
    m_AgentCells.erase(it);
}

void GridMap::ClearExpiredReservations(float32 currentTime)
{
    std::unique_lock lock(m_ReservationMutex);
    for (auto it = m_AgentCells.begin(); it != m_AgentCells.end();)
    {
        auto& cells = it->second;
        size_t write = 0;
        for (uint32 index : cells)
        {
            if (index >= m_Reservations.size())
                continue;
            auto& reservation = m_Reservations[index];
            if (reservation.AgentId != it->first)
                continue;
            if (reservation.ExpirationTime <= currentTime)
            {
                reservation.AgentId = 0;
                reservation.ExpirationTime = 0.0f;
                reservation.Occupancy = 0;
                continue;
            }
            cells[write++] = index;
        }
        cells.resize(write);
        if (cells.empty())
            it = m_AgentCells.erase(it);
        else
            ++it;
    }

    // Orphans (cleared from a list without zeroing, or stolen then abandoned).
    for (auto& reservation : m_Reservations)
    {
        if (reservation.AgentId != 0 && reservation.ExpirationTime <= currentTime)
        {
            reservation.AgentId = 0;
            reservation.ExpirationTime = 0.0f;
            reservation.Occupancy = 0;
        }
    }
}

bool GridMap::IsOccupiedByOther(uint32 cellX, uint32 cellZ, uint32 excludeAgentId,
                                float32 currentTime) const
{
    if (!IsValidCell(cellX, cellZ))
        return false;
    std::shared_lock lock(m_ReservationMutex);
    const auto& reservation = m_Reservations[CellIndex(cellX, cellZ)];
    return reservation.Occupancy != 0 && reservation.IsActive(currentTime) &&
           reservation.AgentId != excludeAgentId;
}

uint32 GridMap::GetReservedAgentCount() const
{
    std::shared_lock lock(m_ReservationMutex);
    return static_cast<uint32>(m_AgentCells.size());
}

bool GridMap::WorldToCell(float32 worldX, float32 worldZ, uint32& outCellX, uint32& outCellZ) const
{
    if (m_Settings.Type == GridType::Square)
    {
        const float32 localX = worldX - m_Settings.OriginX;
        const float32 localZ = worldZ - m_Settings.OriginZ;

        if (localX < 0.0f || localZ < 0.0f)
            return false;

        const int32 cellX = static_cast<int32>(std::floor(localX / m_Settings.CellSize));
        const int32 cellZ = static_cast<int32>(std::floor(localZ / m_Settings.CellSize));

        if (cellX < 0 || cellX >= static_cast<int32>(m_Settings.Width) ||
            cellZ < 0 || cellZ >= static_cast<int32>(m_Settings.Depth))
        {
            return false;
        }

        outCellX = static_cast<uint32>(cellX);
        outCellZ = static_cast<uint32>(cellZ);
        return true;
    }
    else
    {
        int32 col = 0, row = 0;
        HexGrid::WorldToHex(worldX, worldZ, m_Settings.CellSize,
                            m_Settings.OriginX, m_Settings.OriginZ, col, row);

        if (col < 0 || col >= static_cast<int32>(m_Settings.Width) ||
            row < 0 || row >= static_cast<int32>(m_Settings.Depth))
        {
            return false;
        }

        outCellX = static_cast<uint32>(col);
        outCellZ = static_cast<uint32>(row);
        return true;
    }
}

void GridMap::CellToWorld(uint32 cellX, uint32 cellZ, float32& outWorldX, float32& outWorldZ) const
{
    if (m_Settings.Type == GridType::Square)
    {
        outWorldX = m_Settings.OriginX + (static_cast<float32>(cellX) + 0.5f) * m_Settings.CellSize;
        outWorldZ = m_Settings.OriginZ + (static_cast<float32>(cellZ) + 0.5f) * m_Settings.CellSize;
    }
    else
    {
        HexGrid::HexToWorld(cellX, cellZ, m_Settings.CellSize,
                            m_Settings.OriginX, m_Settings.OriginZ, outWorldX, outWorldZ);
    }
}

uint32 GridMap::FootprintRadiusInCells(float32 agentRadius) const
{
    if (agentRadius <= m_Settings.CellSize * 0.5f)
        return 0;
    return static_cast<uint32>(std::ceil(agentRadius / m_Settings.CellSize));
}

uint32 GridMap::GetNeighborCells(uint32 cellX, uint32 cellZ,
                                 uint32* outCellX, uint32* outCellZ, uint32 maxNeighbors) const
{
    uint32 count = 0;
    if (m_Settings.Type == GridType::Hex)
    {
        int32 dx[6], dz[6];
        uint32 hexCount = 0;
        HexGrid::GetNeighborOffsets(cellZ, dx, dz, hexCount);
        for (uint32 i = 0; i < hexCount && count < maxNeighbors; ++i)
        {
            const int32 nx = static_cast<int32>(cellX) + dx[i];
            const int32 nz = static_cast<int32>(cellZ) + dz[i];
            if (nx >= 0 && nx < static_cast<int32>(m_Settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(m_Settings.Depth))
            {
                outCellX[count] = static_cast<uint32>(nx);
                outCellZ[count] = static_cast<uint32>(nz);
                ++count;
            }
        }
    }
    else
    {
        static constexpr int32 kDx[] = {0, 0, 1, -1, 1, -1, 1, -1};
        static constexpr int32 kDz[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        for (uint32 i = 0; i < 8 && count < maxNeighbors; ++i)
        {
            const int32 nx = static_cast<int32>(cellX) + kDx[i];
            const int32 nz = static_cast<int32>(cellZ) + kDz[i];
            if (nx >= 0 && nx < static_cast<int32>(m_Settings.Width) &&
                nz >= 0 && nz < static_cast<int32>(m_Settings.Depth))
            {
                outCellX[count] = static_cast<uint32>(nx);
                outCellZ[count] = static_cast<uint32>(nz);
                ++count;
            }
        }
    }
    return count;
}

PathStatus GridMap::FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const
{
    uint32 startCellX = 0, startCellZ = 0;
    uint32 endCellX = 0, endCellZ = 0;

    if (!WorldToCell(request.StartX, request.StartZ, startCellX, startCellZ))
        return PathStatus::Failed;
    if (!WorldToCell(request.EndX, request.EndZ, endCellX, endCellZ))
        return PathStatus::Failed;

    const uint32 startIndex = CellIndex(startCellX, startCellZ);
    const uint32 endIndex = CellIndex(endCellX, endCellZ);

    uint32 agentCellRadius = 0;
    if (request.AgentRadius > m_Settings.CellSize * 0.5f)
    {
        agentCellRadius = static_cast<uint32>(std::ceil(request.AgentRadius / m_Settings.CellSize));
    }

    AStarSolver solver;
    std::vector<uint32> cellPath;
    if (!solver.Solve(*this, startIndex, endIndex, request.ExcludeAgentId, request.CurrentTime, agentCellRadius, cellPath))
        return PathStatus::Failed;

    std::vector<PathPoint> worldPoints;
    worldPoints.reserve(cellPath.size());

    for (uint32 cellIndex : cellPath)
    {
        uint32 cx = 0, cz = 0;
        CellFromIndex(cellIndex, cx, cz);

        float32 worldX = 0.0f, worldZ = 0.0f;
        CellToWorld(cx, cz, worldX, worldZ);

        PathPoint point;
        point.X = worldX;
        point.Y = GetCellHeight(cx, cz);
        point.Z = worldZ;
        worldPoints.push_back(point);
    }

    if (request.SmoothPath && worldPoints.size() > 2)
    {
        PathSmoother::Smooth(*this, worldPoints);
    }

    outPath = pathBuffer.AllocatePath(worldPoints.data(), static_cast<uint32>(worldPoints.size()));
    return PathStatus::Complete;
}

bool GridMap::IsPointNavigable(float32 x, float32 /*y*/, float32 z, float32 radius) const
{
    uint32 cellX = 0, cellZ = 0;
    if (!WorldToCell(x, z, cellX, cellZ))
        return false;

    // Single-cell fast path for small/zero radius
    if (radius <= m_Settings.CellSize * 0.5f)
        return !IsCellBlocked(cellX, cellZ);

    // Check the full agent footprint (same logic as AStarSolver)
    const uint32 agentCellRadius = static_cast<uint32>(
        std::ceil(radius / m_Settings.CellSize));
    const int32 r = static_cast<int32>(agentCellRadius);

    for (int32 dz = -r; dz <= r; ++dz)
    {
        for (int32 dx = -r; dx <= r; ++dx)
        {
            const int32 fx = static_cast<int32>(cellX) + dx;
            const int32 fz = static_cast<int32>(cellZ) + dz;

            if (fx < 0 || fx >= static_cast<int32>(m_Settings.Width) ||
                fz < 0 || fz >= static_cast<int32>(m_Settings.Depth))
                return false;

            if (IsCellBlocked(static_cast<uint32>(fx), static_cast<uint32>(fz)))
                return false;
        }
    }

    return true;
}

bool GridMap::GetClosestNavigablePoint(float32 x, float32 /*y*/, float32 z,
                                        float32 searchRadius,
                                        float32& outX, float32& outY, float32& outZ) const
{
    uint32 startCellX = 0, startCellZ = 0;

    // Clamp the query point to grid bounds for the BFS start
    float32 clampedX = std::max(m_Settings.OriginX, std::min(x, m_Settings.OriginX + m_Settings.Width * m_Settings.CellSize - 0.01f));
    float32 clampedZ = std::max(m_Settings.OriginZ, std::min(z, m_Settings.OriginZ + m_Settings.Depth * m_Settings.CellSize - 0.01f));

    if (!WorldToCell(clampedX, clampedZ, startCellX, startCellZ))
        return false;

    // If the start cell is navigable, return its center
    if (!IsCellBlocked(startCellX, startCellZ))
    {
        CellToWorld(startCellX, startCellZ, outX, outZ);
        outY = GetCellHeight(startCellX, startCellZ);
        return true;
    }

    // BFS spiral outward using a bounded visited set (only allocate within search region)
    const uint32 maxRadius = static_cast<uint32>(std::ceil(searchRadius / m_Settings.CellSize));
    const uint32 maxRadiusSq = maxRadius * maxRadius;

    // Compute BFS region bounds clamped to grid
    const uint32 minBfsX = (startCellX > maxRadius) ? (startCellX - maxRadius) : 0;
    const uint32 minBfsZ = (startCellZ > maxRadius) ? (startCellZ - maxRadius) : 0;
    const uint32 maxBfsX = std::min(startCellX + maxRadius, m_Settings.Width - 1);
    const uint32 maxBfsZ = std::min(startCellZ + maxRadius, m_Settings.Depth - 1);
    const uint32 regionW = maxBfsX - minBfsX + 1;
    const uint32 regionD = maxBfsZ - minBfsZ + 1;

    std::vector<uint8> visited(regionW * regionD, 0);
    auto regionIndex = [&](uint32 cx, uint32 cz) { return (cz - minBfsZ) * regionW + (cx - minBfsX); };

    struct BfsEntry
    {
        uint32 CellX;
        uint32 CellZ;
    };

    std::queue<BfsEntry> frontier;
    frontier.push({startCellX, startCellZ});
    visited[regionIndex(startCellX, startCellZ)] = 1;

    while (!frontier.empty())
    {
        BfsEntry current = frontier.front();
        frontier.pop();

        // Use GetNeighborCells to get correct neighbors for both square and hex grids
        uint32 neighX[8], neighZ[8];
        uint32 neighCount = GetNeighborCells(current.CellX, current.CellZ, neighX, neighZ, 8);

        for (uint32 i = 0; i < neighCount; ++i)
        {
            const uint32 unx = neighX[i];
            const uint32 unz = neighZ[i];

            if (unx < minBfsX || unx > maxBfsX || unz < minBfsZ || unz > maxBfsZ)
                continue;

            const uint32 rIdx = regionIndex(unx, unz);

            if (visited[rIdx])
                continue;
            visited[rIdx] = 1;

            // Circular distance check (squared cell distance)
            const uint32 dx = (unx > startCellX) ? (unx - startCellX) : (startCellX - unx);
            const uint32 dz = (unz > startCellZ) ? (unz - startCellZ) : (startCellZ - unz);
            if (dx * dx + dz * dz > maxRadiusSq)
                continue;

            if (!IsCellBlocked(unx, unz))
            {
                CellToWorld(unx, unz, outX, outZ);
                outY = GetCellHeight(unx, unz);
                return true;
            }

            frontier.push({unx, unz});
        }
    }

    return false;
}

bool GridMap::Raycast(float32 startX, float32 /*startY*/, float32 startZ,
                      float32 endX, float32 /*endY*/, float32 endZ,
                      float32& hitX, float32& hitY, float32& hitZ) const
{
    uint32 startCellX = 0, startCellZ = 0;
    uint32 endCellX = 0, endCellZ = 0;

    if (!WorldToCell(startX, startZ, startCellX, startCellZ))
        return false;
    if (!WorldToCell(endX, endZ, endCellX, endCellZ))
        return false;

    // Bresenham line algorithm
    int32 x0 = static_cast<int32>(startCellX);
    int32 z0 = static_cast<int32>(startCellZ);
    const int32 x1 = static_cast<int32>(endCellX);
    const int32 z1 = static_cast<int32>(endCellZ);

    const int32 dx = std::abs(x1 - x0);
    const int32 dz = std::abs(z1 - z0);
    const int32 sx = (x0 < x1) ? 1 : -1;
    const int32 sz = (z0 < z1) ? 1 : -1;
    int32 err = dx - dz;

    while (true)
    {
        if (x0 >= 0 && x0 < static_cast<int32>(m_Settings.Width) &&
            z0 >= 0 && z0 < static_cast<int32>(m_Settings.Depth))
        {
            const uint32 ux = static_cast<uint32>(x0);
            const uint32 uz = static_cast<uint32>(z0);

            if (IsCellBlocked(ux, uz))
            {
                CellToWorld(ux, uz, hitX, hitZ);
                hitY = GetCellHeight(ux, uz);
                return true;
            }
        }

        if (x0 == x1 && z0 == z1)
            break;

        const int32 e2 = 2 * err;
        const bool stepX = (e2 > -dz);
        const bool stepZ = (e2 < dx);

        // If both X and Z step simultaneously (diagonal move), check the two
        // intermediate cardinal cells to prevent the ray cutting through wall corners.
        if (stepX && stepZ)
        {
            const int32 adjX = x0 + sx;
            const int32 adjZ = z0 + sz;
            bool xBlocked = (adjX >= 0 && adjX < static_cast<int32>(m_Settings.Width) &&
                             z0 >= 0 && z0 < static_cast<int32>(m_Settings.Depth) &&
                             IsCellBlocked(static_cast<uint32>(adjX), static_cast<uint32>(z0)));
            bool zBlocked = (x0 >= 0 && x0 < static_cast<int32>(m_Settings.Width) &&
                             adjZ >= 0 && adjZ < static_cast<int32>(m_Settings.Depth) &&
                             IsCellBlocked(static_cast<uint32>(x0), static_cast<uint32>(adjZ)));
            if (xBlocked || zBlocked)
            {
                // Hit a wall corner — report the current cell as the hit point
                if (x0 >= 0 && x0 < static_cast<int32>(m_Settings.Width) &&
                    z0 >= 0 && z0 < static_cast<int32>(m_Settings.Depth))
                {
                    CellToWorld(static_cast<uint32>(x0), static_cast<uint32>(z0), hitX, hitZ);
                    hitY = GetCellHeight(static_cast<uint32>(x0), static_cast<uint32>(z0));
                    return true;
                }
            }
        }

        if (stepX)
        {
            err -= dz;
            x0 += sx;
        }
        if (stepZ)
        {
            err += dx;
            z0 += sz;
        }
    }

    return false;
}

void GridMap::BakeHeights(HeightQueryFn heightQuery)
{
    for (uint32 z = 0; z < m_Settings.Depth; ++z)
    {
        for (uint32 x = 0; x < m_Settings.Width; ++x)
        {
            float32 worldX = 0.0f, worldZ = 0.0f;
            CellToWorld(x, z, worldX, worldZ);

            float32 height = 0.0f;
            if (heightQuery(worldX, worldZ, height))
            {
                m_Heights[CellIndex(x, z)] = height;
            }
        }
    }
    ++m_TopologyVersion;
}

const float32* GridMap::GetCostData() const
{
    return m_Costs.data();
}

const float32* GridMap::GetHeightData() const
{
    return m_Heights.data();
}

const uint8* GridMap::GetBlockedData() const
{
    return m_Blocked.data();
}

} // namespace GameEngine::Pathfinding
