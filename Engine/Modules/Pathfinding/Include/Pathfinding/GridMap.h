#pragma once

#include "Pathfinding/INavigationMap.h"
#include "Pathfinding/PathfindingTypes.h"

#include <atomic>
#include <functional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine::Pathfinding
{

class GridMap : public INavigationMap
{
public:
    explicit GridMap(const GridSettings& settings);

    // INavigationMap
    PathStatus FindPath(const PathRequest& request, PathBuffer& pathBuffer, PathHandle& outPath) const override;
    bool IsPointNavigable(float32 x, float32 y, float32 z, float32 radius) const override;
    bool GetClosestNavigablePoint(float32 x, float32 y, float32 z, float32 searchRadius,
                                   float32& outX, float32& outY, float32& outZ) const override;
    bool Raycast(float32 startX, float32 startY, float32 startZ,
                 float32 endX, float32 endY, float32 endZ,
                 float32& hitX, float32& hitY, float32& hitZ) const override;

    // Configuration
    const GridSettings& GetSettings() const;
    uint32 GetCellCount() const;
    uint32 GetTopologyVersion() const;

    // Cell data
    float32 GetCellCost(uint32 cellX, uint32 cellZ) const;
    void SetCellCost(uint32 cellX, uint32 cellZ, float32 cost);
    bool IsCellBlocked(uint32 cellX, uint32 cellZ) const;
    void SetCellBlocked(uint32 cellX, uint32 cellZ, bool blocked);
    float32 GetCellHeight(uint32 cellX, uint32 cellZ) const;
    void SetCellHeight(uint32 cellX, uint32 cellZ, float32 height);

    // Dynamic obstacle blocking (cleared and rebuilt each frame)
    void SetCellDynamicBlocked(uint32 cellX, uint32 cellZ, bool blocked);
    void ClearAllDynamicBlocked();
    bool IsCellDynamicBlocked(uint32 cellX, uint32 cellZ) const;

    // Reservations (for grid-based obstacle avoidance)
    void ReserveCell(uint32 cellX, uint32 cellZ, uint32 agentId, float32 expirationTime,
                     bool occupancy = false);
    void ClearCellReservation(uint32 cellX, uint32 cellZ);
    void ClearReservationsForAgent(uint32 agentId);
    CellReservation GetCellReservation(uint32 cellX, uint32 cellZ) const;
    void ClearExpiredReservations(float32 currentTime);
    bool IsOccupiedByOther(uint32 cellX, uint32 cellZ, uint32 excludeAgentId,
                           float32 currentTime) const;
    // Live agents that currently have at least one stamped cell. Peak
    // concurrent, not lifetime spawned ids.
    uint32 GetReservedAgentCount() const;

    // Coordinate conversion
    bool WorldToCell(float32 worldX, float32 worldZ, uint32& outCellX, uint32& outCellZ) const;
    void CellToWorld(uint32 cellX, uint32 cellZ, float32& outWorldX, float32& outWorldZ) const;

    // Cells covered either side of the centre by an agent of this radius: zero
    // while the agent fits inside one cell, otherwise ceil(radius / cellSize).
    // Stamping a body and testing for one share this single definition, so they
    // cannot disagree about which cells a footprint covers.
    uint32 FootprintRadiusInCells(float32 agentRadius) const;

    // Index helpers
    uint32 CellIndex(uint32 cellX, uint32 cellZ) const;
    void CellFromIndex(uint32 index, uint32& outCellX, uint32& outCellZ) const;
    bool IsValidCell(uint32 cellX, uint32 cellZ) const;

    // Get neighbor cell coordinates for the given cell.
    // Returns the count of valid neighbors written to outCellX/outCellZ arrays.
    // Arrays must be at least 8 elements (max 8 for square, 6 for hex).
    uint32 GetNeighborCells(uint32 cellX, uint32 cellZ,
                            uint32* outCellX, uint32* outCellZ, uint32 maxNeighbors) const;

    // Baking
    using HeightQueryFn = std::function<bool(float32 x, float32 z, float32& outHeight)>;
    void BakeHeights(HeightQueryFn heightQuery);

    // Debug data (raw arrays, size = Width * Depth)
    const float32* GetCostData() const;
    const float32* GetHeightData() const;
    const uint8* GetBlockedData() const;

private:
    GridSettings m_Settings;
    std::vector<float32> m_Costs;
    std::vector<float32> m_Heights;
    std::vector<uint8> m_Blocked;
    std::vector<uint8> m_DynamicBlocked;
    std::vector<CellReservation> m_Reservations;

    // Per-agent cell indices so ClearReservationsForAgent is O(stamped cells),
    // not O(Width*Depth). Keyed by AgentId so ReserveCell is O(1) lookup,
    // not a linear scan of every live agent. Stale indices are skipped if
    // the cell was stolen. Erased on clear so size is live agents.
    std::unordered_map<uint32, std::vector<uint32>> m_AgentCells;

    // Guards m_Reservations and m_AgentCells. Writers (ReserveCell,
    // ClearCellReservation, ClearReservationsForAgent, ClearExpiredReservations)
    // take a unique_lock; readers (GetCellReservation / IsOccupiedByOther)
    // take a shared_lock. FindPath reads reservations through those getters.
    mutable std::shared_mutex m_ReservationMutex;
    std::atomic<uint32> m_TopologyVersion{0};
};

} // namespace GameEngine::Pathfinding
