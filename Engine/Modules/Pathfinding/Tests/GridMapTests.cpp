#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathBuffer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace GameEngine;
using namespace GameEngine::Pathfinding;

namespace
{

GridSettings MakeDefaultSettings(uint32 width = 16, uint32 depth = 16, float32 cellSize = 1.0f)
{
    GridSettings settings;
    settings.Type = GridType::Square;
    settings.CellSize = cellSize;
    settings.OriginX = 0.0f;
    settings.OriginY = 0.0f;
    settings.OriginZ = 0.0f;
    settings.Width = width;
    settings.Depth = depth;
    settings.MaxSlope = 45.0f;
    settings.MaxStepHeight = 0.4f;
    return settings;
}

} // namespace

class GridMapTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Map = std::make_unique<GridMap>(MakeDefaultSettings());
    }

    std::unique_ptr<GridMap> m_Map;
};

TEST_F(GridMapTest, ConstructionInitializesCorrectSizes)
{
    EXPECT_EQ(m_Map->GetSettings().Width, 16u);
    EXPECT_EQ(m_Map->GetSettings().Depth, 16u);
    EXPECT_EQ(m_Map->GetCellCount(), 256u);
}

TEST_F(GridMapTest, WorldToCellAndCellToWorldRoundTrip)
{
    float32 worldX = 0.0f, worldZ = 0.0f;
    m_Map->CellToWorld(3, 5, worldX, worldZ);

    EXPECT_FLOAT_EQ(worldX, 3.5f);
    EXPECT_FLOAT_EQ(worldZ, 5.5f);

    uint32 cellX = 0, cellZ = 0;
    EXPECT_TRUE(m_Map->WorldToCell(worldX, worldZ, cellX, cellZ));
    EXPECT_EQ(cellX, 3u);
    EXPECT_EQ(cellZ, 5u);
}

TEST_F(GridMapTest, WorldToCellReturnsFalseForOutOfBounds)
{
    uint32 cellX = 0, cellZ = 0;

    EXPECT_FALSE(m_Map->WorldToCell(-1.0f, 0.0f, cellX, cellZ));
    EXPECT_FALSE(m_Map->WorldToCell(0.0f, -1.0f, cellX, cellZ));
    EXPECT_FALSE(m_Map->WorldToCell(100.0f, 0.0f, cellX, cellZ));
    EXPECT_FALSE(m_Map->WorldToCell(0.0f, 100.0f, cellX, cellZ));
}

TEST_F(GridMapTest, SetCellCostAndGetCellCost)
{
    m_Map->SetCellCost(2, 3, 5.0f);
    EXPECT_FLOAT_EQ(m_Map->GetCellCost(2, 3), 5.0f);

    EXPECT_FLOAT_EQ(m_Map->GetCellCost(0, 0), 1.0f);
}

TEST_F(GridMapTest, SetCellBlockedAndIsCellBlocked)
{
    EXPECT_FALSE(m_Map->IsCellBlocked(4, 4));

    m_Map->SetCellBlocked(4, 4, true);
    EXPECT_TRUE(m_Map->IsCellBlocked(4, 4));

    m_Map->SetCellBlocked(4, 4, false);
    EXPECT_FALSE(m_Map->IsCellBlocked(4, 4));
}

TEST_F(GridMapTest, SetCellHeightAndGetCellHeight)
{
    m_Map->SetCellHeight(1, 1, 3.5f);
    EXPECT_FLOAT_EQ(m_Map->GetCellHeight(1, 1), 3.5f);

    EXPECT_FLOAT_EQ(m_Map->GetCellHeight(0, 0), 0.0f);
}

TEST_F(GridMapTest, IsPointNavigableReturnsFalseForBlockedCells)
{
    m_Map->SetCellBlocked(5, 5, true);

    EXPECT_FALSE(m_Map->IsPointNavigable(5.5f, 0.0f, 5.5f, 0.5f));
}

TEST_F(GridMapTest, IsPointNavigableReturnsTrueForOpenCells)
{
    EXPECT_TRUE(m_Map->IsPointNavigable(5.5f, 0.0f, 5.5f, 0.5f));
}

TEST_F(GridMapTest, GetClosestNavigablePointFindsAdjacentCell)
{
    for (int32 dx = -1; dx <= 1; ++dx)
    {
        for (int32 dz = -1; dz <= 1; ++dz)
        {
            m_Map->SetCellBlocked(static_cast<uint32>(5 + dx), static_cast<uint32>(5 + dz), true);
        }
    }
    m_Map->SetCellBlocked(6, 4, false);

    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    bool found = m_Map->GetClosestNavigablePoint(5.5f, 0.0f, 5.5f, 10.0f, outX, outY, outZ);

    EXPECT_TRUE(found);
    EXPECT_FLOAT_EQ(outX, 6.5f);
    EXPECT_FLOAT_EQ(outZ, 4.5f);
}

TEST_F(GridMapTest, GetClosestNavigablePointReturnsStartIfAlreadyNavigable)
{
    float32 outX = 0.0f, outY = 0.0f, outZ = 0.0f;
    bool found = m_Map->GetClosestNavigablePoint(5.5f, 0.0f, 5.5f, 10.0f, outX, outY, outZ);

    EXPECT_TRUE(found);
    EXPECT_FLOAT_EQ(outX, 5.5f);
    EXPECT_FLOAT_EQ(outZ, 5.5f);
}

TEST_F(GridMapTest, RaycastThroughOpenGridReturnsFalse)
{
    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    bool hit = m_Map->Raycast(0.5f, 0.0f, 0.5f, 15.5f, 0.0f, 15.5f, hitX, hitY, hitZ);

    EXPECT_FALSE(hit);
}

TEST_F(GridMapTest, RaycastHittingBlockedCellReturnsTrue)
{
    m_Map->SetCellBlocked(8, 8, true);

    float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    bool hit = m_Map->Raycast(0.5f, 0.0f, 0.5f, 15.5f, 0.0f, 15.5f, hitX, hitY, hitZ);

    EXPECT_TRUE(hit);
    EXPECT_FLOAT_EQ(hitX, 8.5f);
    EXPECT_FLOAT_EQ(hitZ, 8.5f);
}

TEST_F(GridMapTest, FindPathOnOpenGridReturnsComplete)
{
    PathBuffer pathBuffer;
    PathHandle outPath;

    PathRequest request;
    request.StartX = 0.5f;
    request.StartZ = 0.5f;
    request.EndX = 10.5f;
    request.EndZ = 10.5f;

    PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);

    EXPECT_EQ(status, PathStatus::Complete);
    EXPECT_TRUE(outPath.IsValid());
    EXPECT_GT(pathBuffer.GetPointCount(outPath), 0u);
}

TEST_F(GridMapTest, FindPathWhenBlockedReturnsFailed)
{
    for (uint32 z = 0; z < 16; ++z)
    {
        m_Map->SetCellBlocked(8, z, true);
    }

    PathBuffer pathBuffer;
    PathHandle outPath;

    PathRequest request;
    request.StartX = 0.5f;
    request.StartZ = 0.5f;
    request.EndX = 15.5f;
    request.EndZ = 0.5f;

    PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);

    EXPECT_EQ(status, PathStatus::Failed);
}

TEST_F(GridMapTest, FindPathFromAToBProducesValidWaypoints)
{
    PathBuffer pathBuffer;
    PathHandle outPath;

    PathRequest request;
    request.StartX = 0.5f;
    request.StartZ = 0.5f;
    request.EndX = 5.5f;
    request.EndZ = 0.5f;

    PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);

    ASSERT_EQ(status, PathStatus::Complete);

    uint32 count = pathBuffer.GetPointCount(outPath);
    ASSERT_GT(count, 1u);

    PathPoint first = pathBuffer.GetPoint(outPath, 0);
    PathPoint last = pathBuffer.GetPoint(outPath, count - 1);

    EXPECT_NEAR(first.X, 0.5f, 1.0f);
    EXPECT_NEAR(first.Z, 0.5f, 1.0f);
    EXPECT_NEAR(last.X, 5.5f, 1.0f);
    EXPECT_NEAR(last.Z, 0.5f, 1.0f);
}

TEST_F(GridMapTest, BakeHeightsPopulatesHeightData)
{
    auto heightQuery = [](float32 /*x*/, float32 /*z*/, float32& outHeight) -> bool
    {
        outHeight = 10.0f;
        return true;
    };

    m_Map->BakeHeights(heightQuery);

    EXPECT_FLOAT_EQ(m_Map->GetCellHeight(0, 0), 10.0f);
    EXPECT_FLOAT_EQ(m_Map->GetCellHeight(7, 7), 10.0f);
    EXPECT_FLOAT_EQ(m_Map->GetCellHeight(15, 15), 10.0f);
}

TEST_F(GridMapTest, ReserveCellAndGetCellReservation)
{
    m_Map->ReserveCell(3, 4, 42, 100.0f);

    CellReservation reservation = m_Map->GetCellReservation(3, 4);
    EXPECT_EQ(reservation.AgentId, 42u);
    EXPECT_FLOAT_EQ(reservation.ExpirationTime, 100.0f);
    EXPECT_TRUE(reservation.IsActive(50.0f));
    EXPECT_FALSE(reservation.IsActive(200.0f));
}

TEST_F(GridMapTest, ClearReservationsForAgentLeavesOtherAgents)
{
    m_Map->ReserveCell(1, 1, 10, 50.0f);
    m_Map->ReserveCell(2, 2, 20, 50.0f);

    m_Map->ClearReservationsForAgent(10);

    EXPECT_EQ(m_Map->GetCellReservation(1, 1).AgentId, 0u);
    EXPECT_EQ(m_Map->GetCellReservation(2, 2).AgentId, 20u);
}

TEST_F(GridMapTest, ClearThenReserveDropsCellsBehindTheAgent)
{
    m_Map->ReserveCell(1, 1, 10, 50.0f);
    m_Map->ReserveCell(2, 1, 10, 50.0f);
    m_Map->ClearReservationsForAgent(10);
    m_Map->ReserveCell(2, 1, 10, 50.0f);

    EXPECT_EQ(m_Map->GetCellReservation(1, 1).AgentId, 0u);
    EXPECT_EQ(m_Map->GetCellReservation(2, 1).AgentId, 10u);
}

TEST_F(GridMapTest, ClearReservationsForAgentSkipsCellsStolenByAnotherAgent)
{
    m_Map->ReserveCell(3, 3, 10, 50.0f);
    m_Map->ReserveCell(3, 3, 20, 50.0f);
    m_Map->ClearReservationsForAgent(10);

    EXPECT_EQ(m_Map->GetCellReservation(3, 3).AgentId, 20u);
}

TEST_F(GridMapTest, OccupancySticksWhenSameAgentRestampsSoft)
{
    m_Map->ReserveCell(4, 4, 7, 50.0f, true);
    m_Map->ReserveCell(4, 4, 7, 50.0f, false);
    EXPECT_NE(m_Map->GetCellReservation(4, 4).Occupancy, 0u);
}

TEST_F(GridMapTest, ClearedSlotCanBeReusedByAnotherAgent)
{
    m_Map->ReserveCell(1, 1, 10, 50.0f);
    m_Map->ClearReservationsForAgent(10);
    m_Map->ReserveCell(5, 5, 20, 50.0f);

    EXPECT_EQ(m_Map->GetCellReservation(1, 1).AgentId, 0u);
    EXPECT_EQ(m_Map->GetCellReservation(5, 5).AgentId, 20u);
}

TEST_F(GridMapTest, ClearExpiredReservationsRemovesOldReservations)
{
    m_Map->ReserveCell(1, 1, 10, 5.0f);
    m_Map->ReserveCell(2, 2, 20, 15.0f);

    m_Map->ClearExpiredReservations(10.0f);

    CellReservation r1 = m_Map->GetCellReservation(1, 1);
    CellReservation r2 = m_Map->GetCellReservation(2, 2);

    EXPECT_EQ(r1.AgentId, 0u);
    EXPECT_EQ(r2.AgentId, 20u);
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 1u);
}

TEST_F(GridMapTest, ReservedAgentCountTracksLiveAgentsNotLifetime)
{
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 0u);
    for (uint32 i = 1; i <= 200; ++i)
    {
        m_Map->ReserveCell(i % 16, (i / 16) % 16, i, 50.0f);
        m_Map->ClearReservationsForAgent(i);
    }
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 0u);

    m_Map->ReserveCell(3, 3, 7, 50.0f);
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 1u);
}

TEST_F(GridMapTest, ManyAgentsStayAtConcurrentCount)
{
    constexpr uint32 kAgents = 512;
    for (uint32 i = 1; i <= kAgents; ++i)
    {
        const uint32 x = i % 16;
        const uint32 z = (i / 16) % 16;
        for (uint32 k = 0; k < 8; ++k)
            m_Map->ReserveCell((x + k) % 16, z, i, 50.0f);
    }
    EXPECT_EQ(m_Map->GetReservedAgentCount(), kAgents);
    for (uint32 i = 1; i <= kAgents; ++i)
        m_Map->ClearReservationsForAgent(i);
    EXPECT_EQ(m_Map->GetReservedAgentCount(), 0u);
}

TEST_F(GridMapTest, IsOccupiedByOtherIgnoresSelfAndExpired)
{
    m_Map->ReserveCell(2, 2, 9, 10.0f, true);
    EXPECT_TRUE(m_Map->IsOccupiedByOther(2, 2, 0, 0.0f));
    EXPECT_FALSE(m_Map->IsOccupiedByOther(2, 2, 9, 0.0f));
    EXPECT_FALSE(m_Map->IsOccupiedByOther(2, 2, 0, 20.0f));
    m_Map->ReserveCell(3, 3, 9, 10.0f, false);
    EXPECT_FALSE(m_Map->IsOccupiedByOther(3, 3, 0, 0.0f));
}

TEST_F(GridMapTest, DynamicBlockingBlocksAndClears)
{
    // Cell (5,5) should not be dynamically blocked initially
    EXPECT_FALSE(m_Map->IsCellDynamicBlocked(5, 5));
    EXPECT_FALSE(m_Map->IsCellBlocked(5, 5));

    // Set dynamic blocking
    m_Map->SetCellDynamicBlocked(5, 5, true);
    EXPECT_TRUE(m_Map->IsCellDynamicBlocked(5, 5));

    // Block an entire column dynamically to test FindPath avoidance
    for (uint32 z = 0; z < 16; ++z)
        m_Map->SetCellDynamicBlocked(8, z, true);

    // FindPath through column 8 should fail
    {
        PathBuffer pathBuffer;
        PathHandle outPath;
        PathRequest request;
        request.StartX = 0.5f; request.StartZ = 0.5f;
        request.EndX = 15.5f;  request.EndZ = 0.5f;
        PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);
        EXPECT_EQ(status, PathStatus::Failed);
    }

    // Clear all dynamic blocking
    m_Map->ClearAllDynamicBlocked();
    EXPECT_FALSE(m_Map->IsCellDynamicBlocked(5, 5));
    EXPECT_FALSE(m_Map->IsCellDynamicBlocked(8, 0));

    // Path should now succeed
    {
        PathBuffer pathBuffer;
        PathHandle outPath;
        PathRequest request;
        request.StartX = 0.5f; request.StartZ = 0.5f;
        request.EndX = 15.5f;  request.EndZ = 0.5f;
        PathStatus status = m_Map->FindPath(request, pathBuffer, outPath);
        EXPECT_EQ(status, PathStatus::Complete);
    }
}

TEST_F(GridMapTest, TopologyVersionIncrements)
{
    const uint32 v0 = m_Map->GetTopologyVersion();

    m_Map->SetCellCost(1, 1, 3.0f);
    const uint32 v1 = m_Map->GetTopologyVersion();
    EXPECT_GT(v1, v0);

    m_Map->SetCellBlocked(2, 2, true);
    const uint32 v2 = m_Map->GetTopologyVersion();
    EXPECT_GT(v2, v1);

    m_Map->SetCellHeight(3, 3, 5.0f);
    const uint32 v3 = m_Map->GetTopologyVersion();
    EXPECT_GT(v3, v2);

    m_Map->BakeHeights([](float32, float32, float32& h) { h = 1.0f; return true; });
    const uint32 v4 = m_Map->GetTopologyVersion();
    EXPECT_GT(v4, v3);

    EXPECT_EQ(v4, v0 + 4u);
}
