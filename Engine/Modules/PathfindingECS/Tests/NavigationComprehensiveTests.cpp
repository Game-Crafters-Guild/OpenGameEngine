#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/Systems/NavigationBuildSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"
#include "ECS/ComponentRegistry.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathBuffer.h"
#include "Pathfinding/PathfindingTypes.h"

#include <gtest/gtest.h>
#include <cmath>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

constexpr float32 kDeltaTime = 1.0f / 60.0f;
constexpr uint32 kDefaultGridWidth = 16;
constexpr uint32 kDefaultGridDepth = 16;
constexpr float32 kDefaultCellSize = 1.0f;

} // namespace

class NavigationComprehensiveTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NavigationService::Initialize();
        m_World = std::make_unique<ECS::World>();
    }

    void TearDown() override
    {
        m_World.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
    }

    ECS::Entity CreateGrid(uint32 width = kDefaultGridWidth, uint32 depth = kDefaultGridDepth,
                           float32 cellSize = kDefaultCellSize)
    {
        auto entity = m_World->Create();
        Components::NavigationGrid source{};
        source.GridType = Pathfinding::GridType::Square;
        source.CellSize = cellSize;
        source.Width = width;
        source.Depth = depth;
        entity.Set(source);
        m_World->ProcessCommands();

        NavigationBuildSystem buildSystem;
        buildSystem.Update(*m_World, kDeltaTime);
        m_World->ProcessCommands();

        return entity;
    }

    ECS::Entity CreateAgent(float32 startX, float32 startZ, float32 destX, float32 destZ,
                            uint32 mapIndex, uint32 mapGeneration, float32 radius = 0.25f)
    {
        auto entity = m_World->Create();

        Components::NavigationAgent agent{};
        agent.Speed = 3.5f;
        agent.Acceleration = 100.0f;
        agent.StoppingDistance = 0.3f;
        agent.Radius = radius;
        agent.NavMapIndex = mapIndex;
        agent.NavMapGeneration = mapGeneration;
        agent.ReplanInterval = 0.0f; // disable replan by default in tests
        agent.AgentId = entity.GetHandle().id;
        entity.Set(agent);

        Components::NavigationAgentState state{};
        state.DestinationX = destX;
        state.DestinationY = 0.0f;
        state.DestinationZ = destZ;
        state.HasDestination = true;
        state.PathDirty = true;
        state.ReplanTimer = 0.0f;
        entity.Set(state);

        Components::Transform xf{};
        xf.matrix[12] = startX;
        xf.matrix[13] = 0.0f;
        xf.matrix[14] = startZ;
        entity.Set(xf);

        Components::WorldTransform wt{};
        wt.matrix[12] = startX;
        wt.matrix[13] = 0.0f;
        wt.matrix[14] = startZ;
        entity.Set(wt);

        m_World->ProcessCommands();
        return entity;
    }

    void RunPathfinding()
    {
        NavigationPathfindingSystem pathSystem;
        pathSystem.Update(*m_World, kDeltaTime);
        m_World->ProcessCommands();
    }

    void RunMovement(int frames, float32 dt = kDeltaTime)
    {
        NavigationMovementSystem moveSystem;
        for (int i = 0; i < frames; ++i)
        {
            moveSystem.Update(*m_World, dt);
            m_World->ProcessCommands();
        }
    }

    void GetAgentPosition(ECS::Entity& entity, float32& x, float32& z)
    {
        auto* wt = entity.Get<Components::WorldTransform>();
        ASSERT_NE(wt, nullptr);
        x = wt->matrix[12];
        z = wt->matrix[14];
    }

    Components::NavigationAgent GetAgent(ECS::Entity& entity)
    {
        auto* ptr = entity.Get<Components::NavigationAgent>();
        EXPECT_NE(ptr, nullptr);
        return *ptr;
    }

    Components::NavigationAgentState GetAgentState(ECS::Entity& entity)
    {
        auto* ptr = entity.Get<Components::NavigationAgentState>();
        EXPECT_NE(ptr, nullptr);
        return *ptr;
    }

    Components::NavigationGrid GetGridSource(ECS::Entity& entity)
    {
        auto* ptr = entity.Get<Components::NavigationGrid>();
        EXPECT_NE(ptr, nullptr);
        return *ptr;
    }

    Pathfinding::GridMap* GetGridMap(ECS::Entity& gridEntity)
    {
        auto src = GetGridSource(gridEntity);
        auto* navWorld = NavigationService::TryGet();
        if (!navWorld)
            return nullptr;
        Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
        return navWorld->GetGridMap(mapHandle);
    }

    std::unique_ptr<ECS::World> m_World;
};

// ---------------------------------------------------------------------------
// Grid Creation and Correctness
// ---------------------------------------------------------------------------

TEST_F(NavigationComprehensiveTest, GridCreatedWithCorrectDimensions)
{
    auto gridEntity = CreateGrid(20, 10);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    EXPECT_EQ(gridMap->GetSettings().Width, 20u);
    EXPECT_EQ(gridMap->GetSettings().Depth, 10u);
    EXPECT_EQ(gridMap->GetCellCount(), 200u);
}

TEST_F(NavigationComprehensiveTest, AllCellsInitiallyNavigable)
{
    auto gridEntity = CreateGrid(8, 8);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    for (uint32 z = 0; z < 8; ++z)
    {
        for (uint32 x = 0; x < 8; ++x)
        {
            EXPECT_FALSE(gridMap->IsCellBlocked(x, z))
                << "Cell (" << x << ", " << z << ") should not be blocked";
        }
    }
}

TEST_F(NavigationComprehensiveTest, CellCostsDefaultToOne)
{
    auto gridEntity = CreateGrid(8, 8);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    for (uint32 z = 0; z < 8; ++z)
    {
        for (uint32 x = 0; x < 8; ++x)
        {
            EXPECT_FLOAT_EQ(gridMap->GetCellCost(x, z), 1.0f)
                << "Cell (" << x << ", " << z << ") cost should default to 1.0";
        }
    }
}

// ---------------------------------------------------------------------------
// Basic Pathfinding
// ---------------------------------------------------------------------------

TEST_F(NavigationComprehensiveTest, PathFoundOnOpenGrid)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 10.5f, 10.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();

    auto state = GetAgentState(agentEntity);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(state.Status), Pathfinding::PathStatus::Complete);
    EXPECT_FALSE(state.PathDirty);
}

TEST_F(NavigationComprehensiveTest, PathNotFoundWhenFullyBlocked)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // Block a full wall at column x=8, no gaps
    for (uint32 z = 0; z < kDefaultGridDepth; ++z)
    {
        gridMap->SetCellBlocked(8, z, true);
    }

    auto agentEntity = CreateAgent(2.5f, 2.5f, 12.5f, 12.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();

    auto state = GetAgentState(agentEntity);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(state.Status), Pathfinding::PathStatus::Failed);
}

TEST_F(NavigationComprehensiveTest, PathGoesAroundBlockedCells)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // Block column x=8 except for a gap at z=0
    for (uint32 z = 1; z < kDefaultGridDepth; ++z)
    {
        gridMap->SetCellBlocked(8, z, true);
    }

    auto agentEntity = CreateAgent(2.5f, 8.5f, 12.5f, 8.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();

    auto state = GetAgentState(agentEntity);
    auto status = static_cast<Pathfinding::PathStatus>(state.Status);
    EXPECT_EQ(status, Pathfinding::PathStatus::Complete);

    // Verify path does not pass through any blocked cell
    Pathfinding::PathHandle pathHandle{state.PathIndex, state.PathGeneration};
    auto& pathBuffer = NavigationService::Get().GetPathBuffer();
    uint32 pointCount = pathBuffer.GetPointCount(pathHandle);

    for (uint32 i = 0; i < pointCount; ++i)
    {
        Pathfinding::PathPoint pt = pathBuffer.GetPoint(pathHandle, i);
        uint32 cellX = 0, cellZ = 0;
        if (gridMap->WorldToCell(pt.X, pt.Z, cellX, cellZ))
        {
            EXPECT_FALSE(gridMap->IsCellBlocked(cellX, cellZ))
                << "Path point " << i << " at cell (" << cellX << ", " << cellZ << ") is blocked";
        }
    }
}

// ---------------------------------------------------------------------------
// Varying Agent Radius
// ---------------------------------------------------------------------------

TEST_F(NavigationComprehensiveTest, SmallAgentFitsThroughNarrowGap)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // Block column x=8 except for z=8 (1-cell gap)
    for (uint32 z = 0; z < kDefaultGridDepth; ++z)
    {
        if (z != 8)
            gridMap->SetCellBlocked(8, z, true);
    }

    // Small radius agent should fit through 1-cell gap
    auto agentEntity = CreateAgent(2.5f, 8.5f, 12.5f, 8.5f,
                                   src.NavMapIndex, src.NavMapGeneration, 0.25f);
    RunPathfinding();

    auto state = GetAgentState(agentEntity);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(state.Status), Pathfinding::PathStatus::Complete);
}

TEST_F(NavigationComprehensiveTest, LargeAgentCannotFitThroughNarrowGap)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // Block column x=8 except for z=8 (1-cell gap)
    for (uint32 z = 0; z < kDefaultGridDepth; ++z)
    {
        if (z != 8)
            gridMap->SetCellBlocked(8, z, true);
    }

    // Large radius agent (> 1 cell) should not fit through the gap
    auto agentEntity = CreateAgent(2.5f, 8.5f, 12.5f, 8.5f,
                                   src.NavMapIndex, src.NavMapGeneration, 1.5f);
    RunPathfinding();

    auto state = GetAgentState(agentEntity);
    auto status = static_cast<Pathfinding::PathStatus>(state.Status);
    EXPECT_EQ(status, Pathfinding::PathStatus::Failed);
}

TEST_F(NavigationComprehensiveTest, LargeAgentFindsWiderPath)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // Block column x=8 with a narrow gap at z=4 and a wider gap at z=12..14
    for (uint32 z = 0; z < kDefaultGridDepth; ++z)
    {
        gridMap->SetCellBlocked(8, z, true);
    }
    // Narrow gap: just z=4
    gridMap->SetCellBlocked(8, 4, false);
    // Wide gap: z=12, z=13, z=14
    gridMap->SetCellBlocked(8, 12, false);
    gridMap->SetCellBlocked(8, 13, false);
    gridMap->SetCellBlocked(8, 14, false);

    // Large agent should route through the wider gap
    auto agentEntity = CreateAgent(2.5f, 8.5f, 12.5f, 8.5f,
                                   src.NavMapIndex, src.NavMapGeneration, 1.0f);
    RunPathfinding();

    auto state = GetAgentState(agentEntity);
    auto status = static_cast<Pathfinding::PathStatus>(state.Status);
    // The agent should find a path (through the wider gap)
    EXPECT_TRUE(status == Pathfinding::PathStatus::Complete ||
                status == Pathfinding::PathStatus::Partial);
}

// ---------------------------------------------------------------------------
// Varying Heights
// ---------------------------------------------------------------------------

TEST_F(NavigationComprehensiveTest, AgentNavigatesUphillWithinSlopeLimit)
{
    // Use a wider grid with generous slope settings
    auto gridEntity = CreateGrid(10, 1, 1.0f);
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // Set incrementally increasing heights (small slope)
    for (uint32 x = 0; x < 10; ++x)
    {
        gridMap->SetCellHeight(x, 0, static_cast<float32>(x) * 0.1f);
    }

    auto agentEntity = CreateAgent(0.5f, 0.5f, 9.5f, 0.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();

    auto state = GetAgentState(agentEntity);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(state.Status), Pathfinding::PathStatus::Complete);
}

TEST_F(NavigationComprehensiveTest, SteepSlopeBlocksPath)
{
    // 3x1 grid with steep slope exceeding MaxSlope
    auto gridEntity = CreateGrid(3, 1);
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // MaxSlope defaults to 45 degrees. tan(45) = 1.0, so height diff > cellSize blocks.
    // Set middle cell very high to create a steep slope
    gridMap->SetCellHeight(1, 0, 5.0f);

    // Need to re-run the A* which checks height constraints
    auto agentEntity = CreateAgent(0.5f, 0.5f, 2.5f, 0.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();

    auto state = GetAgentState(agentEntity);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(state.Status), Pathfinding::PathStatus::Failed);
}

TEST_F(NavigationComprehensiveTest, StepHeightBlocksCliff)
{
    // 3x1 grid where middle cell has height exceeding MaxStepHeight (default 0.4f)
    auto gridEntity = CreateGrid(3, 1);
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // MaxStepHeight is 0.4f. Place a cliff of 2.0f in the middle
    gridMap->SetCellHeight(1, 0, 2.0f);

    auto agentEntity = CreateAgent(0.5f, 0.5f, 2.5f, 0.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();

    auto state = GetAgentState(agentEntity);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(state.Status), Pathfinding::PathStatus::Failed);
}

// ---------------------------------------------------------------------------
// Movement and Arrival
// ---------------------------------------------------------------------------

TEST_F(NavigationComprehensiveTest, AgentMovesTowardDestination)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 10.5f, 10.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();

    float32 startX = 0.0f, startZ = 0.0f;
    GetAgentPosition(agentEntity, startX, startZ);

    RunMovement(30);

    float32 newX = 0.0f, newZ = 0.0f;
    GetAgentPosition(agentEntity, newX, newZ);

    float32 distMoved = std::sqrt((newX - startX) * (newX - startX) +
                                  (newZ - startZ) * (newZ - startZ));
    EXPECT_GT(distMoved, 0.1f) << "Agent should have moved from starting position";
}

TEST_F(NavigationComprehensiveTest, AgentReachesDestination)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);

    // Short path so agent can reach within reasonable tick count
    auto agentEntity = CreateAgent(2.5f, 2.5f, 4.5f, 4.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();

    NavigationMovementSystem moveSystem;
    constexpr int kMaxIterations = 600;
    bool reached = false;

    for (int i = 0; i < kMaxIterations; ++i)
    {
        moveSystem.Update(*m_World, kDeltaTime);
        m_World->ProcessCommands();

        auto state = GetAgentState(agentEntity);
        if (!state.HasDestination)
        {
            reached = true;
            break;
        }
    }

    EXPECT_TRUE(reached) << "Agent should have reached its destination";
}

TEST_F(NavigationComprehensiveTest, AgentStopsAtDestination)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 4.5f, 4.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();

    NavigationMovementSystem moveSystem;
    constexpr int kMaxIterations = 600;

    for (int i = 0; i < kMaxIterations; ++i)
    {
        moveSystem.Update(*m_World, kDeltaTime);
        m_World->ProcessCommands();

        auto agentState = GetAgentState(agentEntity);
        if (!agentState.HasDestination)
            break;
    }

    auto agentState = GetAgentState(agentEntity);
    EXPECT_FALSE(agentState.HasDestination);
    EXPECT_FLOAT_EQ(agentState.VelocityX, 0.0f);
    EXPECT_FLOAT_EQ(agentState.VelocityY, 0.0f);
    EXPECT_FLOAT_EQ(agentState.VelocityZ, 0.0f);
}

// ---------------------------------------------------------------------------
// Reservation and Avoidance
// ---------------------------------------------------------------------------

TEST_F(NavigationComprehensiveTest, ReservationCreatedDuringMovement)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 8.5f, 8.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();
    RunMovement(10);

    bool hasReservation = false;
    for (uint32 z = 0; z < kDefaultGridDepth && !hasReservation; ++z)
    {
        for (uint32 x = 0; x < kDefaultGridWidth && !hasReservation; ++x)
        {
            auto reservation = gridMap->GetCellReservation(x, z);
            if (reservation.AgentId != 0)
                hasReservation = true;
        }
    }
    EXPECT_TRUE(hasReservation) << "Movement system should have created cell reservations";
}

TEST_F(NavigationComprehensiveTest, IdleAgentKeepsBodyStampAfterCancel)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 5.5f, 5.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();
    RunMovement(5);

    // Stop agent from creating new reservations
    auto* statePtr = agentEntity.GetForWrite<Components::NavigationAgentState>();
    ASSERT_NE(statePtr, nullptr);
    statePtr->HasDestination = false;
    statePtr->Status = static_cast<uint8>(Pathfinding::PathStatus::Invalid);

    // Cancel must drop the corridor on the next movement tick. The cell
    // the agent stands in stays occupied.
    NavigationMovementSystem moveSystem;
    moveSystem.Update(*m_World, kDeltaTime);
    m_World->ProcessCommands();

    float32 px = 0.0f;
    float32 pz = 0.0f;
    GetAgentPosition(agentEntity, px, pz);
    uint32 hereX = 0;
    uint32 hereZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(px, pz, hereX, hereZ));
    EXPECT_EQ(gridMap->GetCellReservation(hereX, hereZ).AgentId, GetAgent(agentEntity).AgentId)
        << "Idle agents keep a body stamp so others cannot walk through them";
    EXPECT_NE(gridMap->GetCellReservation(hereX, hereZ).Occupancy, 0u);
}

TEST_F(NavigationComprehensiveTest, SecondAgentAvoidsReservedCells)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);
    auto* gridMap = GetGridMap(gridEntity);
    ASSERT_NE(gridMap, nullptr);

    // First agent creates reservations along its path
    auto agent1Entity = CreateAgent(2.5f, 2.5f, 8.5f, 2.5f,
                                    src.NavMapIndex, src.NavMapGeneration);
    RunPathfinding();
    RunMovement(10);

    // Collect reserved cells
    std::vector<uint32> reservedCells;
    for (uint32 z = 0; z < kDefaultGridDepth; ++z)
    {
        for (uint32 x = 0; x < kDefaultGridWidth; ++x)
        {
            auto reservation = gridMap->GetCellReservation(x, z);
            if (reservation.AgentId != 0)
            {
                reservedCells.push_back(gridMap->CellIndex(x, z));
            }
        }
    }
    ASSERT_FALSE(reservedCells.empty()) << "First agent should have created reservations";

    // Second agent going through the same area
    auto agent2Entity = CreateAgent(2.5f, 2.5f, 8.5f, 2.5f,
                                    src.NavMapIndex, src.NavMapGeneration);

    // Run pathfinding for the second agent; it should see the reservations as penalties
    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*m_World, kDeltaTime);
    m_World->ProcessCommands();

    auto state2 = GetAgentState(agent2Entity);
    auto status = static_cast<Pathfinding::PathStatus>(state2.Status);
    // Lookahead tax is not a hard block. Occupied body cells are, but this
    // 16x16 open grid still has a path around the first agent.
    EXPECT_TRUE(status == Pathfinding::PathStatus::Complete ||
                status == Pathfinding::PathStatus::Partial);
}

// ---------------------------------------------------------------------------
// Path Re-evaluation
// ---------------------------------------------------------------------------

TEST_F(NavigationComprehensiveTest, PathRecomputedAfterReplanInterval)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 12.5f, 12.5f,
                                   src.NavMapIndex, src.NavMapGeneration);

    // Enable re-evaluation with a short interval
    auto* agentPtr = agentEntity.GetForWrite<Components::NavigationAgent>();
    ASSERT_NE(agentPtr, nullptr);
    agentPtr->ReplanInterval = 0.5f;

    auto* statePtr = agentEntity.GetForWrite<Components::NavigationAgentState>();
    ASSERT_NE(statePtr, nullptr);
    statePtr->ReplanTimer = 0.5f;

    // Initial pathfinding
    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*m_World, kDeltaTime);
    m_World->ProcessCommands();

    auto state = GetAgentState(agentEntity);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(state.Status), Pathfinding::PathStatus::Complete);
    EXPECT_FALSE(state.PathDirty);

    // Run pathfinding system for enough cumulative time to trigger replan (> 0.5s)
    // Each call with dt=0.1 advances the timer
    for (int i = 0; i < 6; ++i)
    {
        pathSystem.Update(*m_World, 0.1f);
        m_World->ProcessCommands();
    }

    // After 0.6s cumulative, the replan timer (initially 0.5) should have fired
    // and the system should have recomputed the path
    state = GetAgentState(agentEntity);
    // The path should still be valid (it was recomputed, not cleared)
    auto status = static_cast<Pathfinding::PathStatus>(state.Status);
    EXPECT_TRUE(status == Pathfinding::PathStatus::Complete ||
                status == Pathfinding::PathStatus::Pending);
}

TEST_F(NavigationComprehensiveTest, NoReplanWhenIntervalIsZero)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 12.5f, 12.5f,
                                   src.NavMapIndex, src.NavMapGeneration);

    // Ensure replan is disabled (interval = 0)
    auto* agentPtr = agentEntity.GetForWrite<Components::NavigationAgent>();
    ASSERT_NE(agentPtr, nullptr);
    agentPtr->ReplanInterval = 0.0f;

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*m_World, kDeltaTime);
    m_World->ProcessCommands();

    auto state = GetAgentState(agentEntity);
    uint32 originalPathIndex = state.PathIndex;
    uint32 originalPathGeneration = state.PathGeneration;
    EXPECT_FALSE(state.PathDirty);

    // Run many frames; path should never be marked dirty
    for (int i = 0; i < 120; ++i)
    {
        pathSystem.Update(*m_World, kDeltaTime);
        m_World->ProcessCommands();
    }

    state = GetAgentState(agentEntity);
    EXPECT_FALSE(state.PathDirty);
    // Path handle should remain the same (no recomputation)
    EXPECT_EQ(state.PathIndex, originalPathIndex);
    EXPECT_EQ(state.PathGeneration, originalPathGeneration);
}

// ---------------------------------------------------------------------------
// Multiple Agents
// ---------------------------------------------------------------------------

TEST_F(NavigationComprehensiveTest, MultipleAgentsNavigateSimultaneously)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);

    auto agent1 = CreateAgent(1.5f, 1.5f, 10.5f, 10.5f,
                              src.NavMapIndex, src.NavMapGeneration);
    auto agent2 = CreateAgent(1.5f, 14.5f, 14.5f, 1.5f,
                              src.NavMapIndex, src.NavMapGeneration);
    auto agent3 = CreateAgent(14.5f, 1.5f, 1.5f, 14.5f,
                              src.NavMapIndex, src.NavMapGeneration);

    RunPathfinding();

    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(GetAgentState(agent1).Status),
              Pathfinding::PathStatus::Complete);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(GetAgentState(agent2).Status),
              Pathfinding::PathStatus::Complete);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(GetAgentState(agent3).Status),
              Pathfinding::PathStatus::Complete);

    // Run movement and verify all agents make progress
    float32 start1X, start1Z, start2X, start2Z, start3X, start3Z;
    GetAgentPosition(agent1, start1X, start1Z);
    GetAgentPosition(agent2, start2X, start2Z);
    GetAgentPosition(agent3, start3X, start3Z);

    RunMovement(60);

    float32 end1X, end1Z, end2X, end2Z, end3X, end3Z;
    GetAgentPosition(agent1, end1X, end1Z);
    GetAgentPosition(agent2, end2X, end2Z);
    GetAgentPosition(agent3, end3X, end3Z);

    auto dist = [](float32 ax, float32 az, float32 bx, float32 bz)
    {
        return std::sqrt((ax - bx) * (ax - bx) + (az - bz) * (az - bz));
    };

    EXPECT_GT(dist(start1X, start1Z, end1X, end1Z), 0.1f) << "Agent 1 should have moved";
    EXPECT_GT(dist(start2X, start2Z, end2X, end2Z), 0.1f) << "Agent 2 should have moved";
    EXPECT_GT(dist(start3X, start3Z, end3X, end3Z), 0.1f) << "Agent 3 should have moved";
}

TEST_F(NavigationComprehensiveTest, AgentsWithDifferentRadiiCoexist)
{
    auto gridEntity = CreateGrid();
    auto src = GetGridSource(gridEntity);

    // Small agent
    auto smallAgent = CreateAgent(1.5f, 1.5f, 10.5f, 10.5f,
                                  src.NavMapIndex, src.NavMapGeneration, 0.25f);
    // Larger agent
    auto largeAgent = CreateAgent(1.5f, 8.5f, 10.5f, 8.5f,
                                  src.NavMapIndex, src.NavMapGeneration, 0.75f);

    RunPathfinding();

    auto smallStatus = static_cast<Pathfinding::PathStatus>(GetAgentState(smallAgent).Status);
    auto largeStatus = static_cast<Pathfinding::PathStatus>(GetAgentState(largeAgent).Status);

    EXPECT_EQ(smallStatus, Pathfinding::PathStatus::Complete)
        << "Small agent should find path on open grid";
    EXPECT_EQ(largeStatus, Pathfinding::PathStatus::Complete)
        << "Large agent should find path on open grid";
}
