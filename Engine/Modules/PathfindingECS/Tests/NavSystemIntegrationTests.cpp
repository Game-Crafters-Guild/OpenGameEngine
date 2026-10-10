#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/Components/NavigationDebugSettings.h"
#include "PathfindingECS/Systems/NavigationBuildSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationDebugSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Systems.h"
#include "ECS/ComponentRegistry.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathfindingTypes.h"
#include "Pathfinding/PathBuffer.h"

#include <gtest/gtest.h>
#include <cmath>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{

constexpr float32 kDeltaTime = 1.0f / 60.0f;

// Small grid for tests to keep them fast
constexpr uint32 kTestGridWidth = 16;
constexpr uint32 kTestGridDepth = 16;
constexpr float32 kTestCellSize = 1.0f;

} // namespace

class NavSystemIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
    }

    void TearDown() override
    {
        world.reset();
        ECS::ComponentRegistry::Clear();
        NavigationService::Shutdown();
    }

    // Helper: create a grid source entity and run init system so it gets baked
    ECS::Entity CreateGridSource()
    {
        auto entity = world->Create();
        Components::NavigationGrid source{};
        source.GridType = Pathfinding::GridType::Square;
        source.CellSize = kTestCellSize;
        source.Width = kTestGridWidth;
        source.Depth = kTestGridDepth;
        entity.Set(source);
        world->ProcessCommands();

        NavigationBuildSystem initSystem;
        initSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();

        return entity;
    }

    // Helper: create an agent entity at a given position with a destination
    ECS::Entity CreateAgent(float32 posX, float32 posZ,
                            float32 destX, float32 destZ,
                            uint32 navMapIndex, uint32 navMapGeneration)
    {
        auto entity = world->Create();

        Components::NavigationAgent agent{};
        agent.Speed = 3.5f;
        agent.Acceleration = 100.0f; // High acceleration so velocity converges quickly in tests
        agent.StoppingDistance = 0.3f;
        agent.Radius = 0.25f;
        agent.NavMapIndex = navMapIndex;
        agent.NavMapGeneration = navMapGeneration;
        agent.AgentId = entity.GetHandle().id;
        entity.Set(agent);

        Components::NavigationAgentState state{};
        state.DestinationX = destX;
        state.DestinationY = 0.0f;
        state.DestinationZ = destZ;
        state.HasDestination = true;
        state.PathDirty = true;
        entity.Set(state);

        Components::Transform xf{};
        xf.matrix[12] = posX;
        xf.matrix[13] = 0.0f;
        xf.matrix[14] = posZ;
        entity.Set(xf);

        Components::WorldTransform wt{};
        wt.matrix[12] = posX;
        wt.matrix[13] = 0.0f;
        wt.matrix[14] = posZ;
        entity.Set(wt);

        world->ProcessCommands();
        return entity;
    }

    // Helper: read NavigationGrid back from entity
    Components::NavigationGrid GetGridSource(ECS::Entity& entity)
    {
        auto* ptr = entity.Get<Components::NavigationGrid>();
        EXPECT_NE(ptr, nullptr);
        return *ptr;
    }

    // Helper: read NavigationAgent back from entity
    Components::NavigationAgent GetAgent(ECS::Entity& entity)
    {
        auto* ptr = entity.Get<Components::NavigationAgent>();
        EXPECT_NE(ptr, nullptr);
        return *ptr;
    }

    // Helper: read NavigationAgentState back from entity
    Components::NavigationAgentState GetAgentState(ECS::Entity& entity)
    {
        auto* ptr = entity.Get<Components::NavigationAgentState>();
        EXPECT_NE(ptr, nullptr);
        return *ptr;
    }

    // Helper: read WorldTransform back from entity
    Components::WorldTransform GetTransform(ECS::Entity& entity)
    {
        auto* ptr = entity.Get<Components::WorldTransform>();
        EXPECT_NE(ptr, nullptr);
        return *ptr;
    }

    static bool GridHasReservationFor(const Pathfinding::GridMap& grid, uint32 agentId)
    {
        for (uint32 z = 0; z < kTestGridDepth; ++z)
        {
            for (uint32 x = 0; x < kTestGridWidth; ++x)
            {
                if (grid.GetCellReservation(x, z).AgentId == agentId)
                    return true;
            }
        }
        return false;
    }

    static bool GridHasAnyReservation(const Pathfinding::GridMap& grid)
    {
        for (uint32 z = 0; z < kTestGridDepth; ++z)
        {
            for (uint32 x = 0; x < kTestGridWidth; ++x)
            {
                if (grid.GetCellReservation(x, z).AgentId != 0)
                    return true;
            }
        }
        return false;
    }

    std::unique_ptr<ECS::World> world;
};

// ---------------------------------------------------------------------------
// GridSourceInit
// ---------------------------------------------------------------------------

TEST_F(NavSystemIntegrationTest, GridSourceInit)
{
    auto gridEntity = world->Create();
    Components::NavigationGrid source{};
    source.GridType = Pathfinding::GridType::Square;
    source.CellSize = kTestCellSize;
    source.Width = kTestGridWidth;
    source.Depth = kTestGridDepth;
    gridEntity.Set(source);
    world->ProcessCommands();

    EXPECT_FALSE(GetGridSource(gridEntity).Initialized);

    NavigationBuildSystem initSystem;
    initSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    auto updatedSource = GetGridSource(gridEntity);
    EXPECT_TRUE(updatedSource.Initialized);
    EXPECT_NE(updatedSource.NavMapGeneration, 0u);
}

// ---------------------------------------------------------------------------
// PathfindingOnGrid
// ---------------------------------------------------------------------------

TEST_F(NavSystemIntegrationTest, PathfindingOnGrid)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    // Agent at cell (2,2) heading to cell (10,10)
    auto agentEntity = CreateAgent(2.5f, 2.5f, 10.5f, 10.5f,
                                   src.NavMapIndex, src.NavMapGeneration);

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    auto state = GetAgentState(agentEntity);
    auto status = static_cast<Pathfinding::PathStatus>(state.Status);
    EXPECT_EQ(status, Pathfinding::PathStatus::Complete);
    EXPECT_FALSE(state.PathDirty);
}

// ---------------------------------------------------------------------------
// MovementAlongPath
// ---------------------------------------------------------------------------

TEST_F(NavSystemIntegrationTest, MovementAlongPath)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 10.5f, 10.5f,
                                   src.NavMapIndex, src.NavMapGeneration);

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    float32 startX = GetTransform(agentEntity).matrix[12];
    float32 startZ = GetTransform(agentEntity).matrix[14];

    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 10; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    float32 newX = GetTransform(agentEntity).matrix[12];
    float32 newZ = GetTransform(agentEntity).matrix[14];

    // Agent should have moved from start position
    float32 distMoved = std::sqrt((newX - startX) * (newX - startX) +
                                  (newZ - startZ) * (newZ - startZ));
    EXPECT_GT(distMoved, 0.1f);
}

// ---------------------------------------------------------------------------
// AgentReachesDestination
// ---------------------------------------------------------------------------

TEST_F(NavSystemIntegrationTest, AgentReachesDestination)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    // Short path: (2,2) -> (4,4) so agent can reach it in a reasonable number of ticks
    auto agentEntity = CreateAgent(2.5f, 2.5f, 4.5f, 4.5f,
                                   src.NavMapIndex, src.NavMapGeneration);

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    NavigationMovementSystem moveSystem;
    constexpr int kMaxIterations = 600; // ~10 seconds at 60fps, more than enough
    bool reached = false;

    for (int i = 0; i < kMaxIterations; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();

        auto agentState = GetAgentState(agentEntity);
        if (!agentState.HasDestination)
        {
            reached = true;
            break;
        }
    }

    EXPECT_TRUE(reached) << "Agent should have reached its destination";
}

// ---------------------------------------------------------------------------
// BlockedPathFails
// ---------------------------------------------------------------------------

TEST_F(NavSystemIntegrationTest, BlockedPathFails)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    // Block a wall across the entire grid at column x=5, preventing any path from
    // the left side (x<5) to the right side (x>5)
    for (uint32 z = 0; z < kTestGridDepth; ++z)
    {
        gridMap->SetCellBlocked(5, z, true);
    }

    // Agent at (2,2) trying to reach (10,10) -- wall at x=5 blocks all routes
    auto agentEntity = CreateAgent(2.5f, 2.5f, 10.5f, 10.5f,
                                   src.NavMapIndex, src.NavMapGeneration);

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    auto agentState = GetAgentState(agentEntity);
    auto status = static_cast<Pathfinding::PathStatus>(agentState.Status);
    EXPECT_EQ(status, Pathfinding::PathStatus::Failed);
}

// ---------------------------------------------------------------------------
// DebugDataGenerated
// ---------------------------------------------------------------------------

TEST_F(NavSystemIntegrationTest, DebugDataGenerated)
{
    auto gridEntity = CreateGridSource();

    // Add debug settings entity with ShowGrid enabled
    auto debugEntity = world->Create();
    Components::NavigationDebugSettings debugSettings{};
    debugSettings.ShowGrid = true;
    debugEntity.Set(debugSettings);
    world->ProcessCommands();

    NavigationDebugSystem debugSystem;
    debugSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    const auto& debugData = NavigationService::GetDebugData();
    EXPECT_FALSE(debugData.Lines.empty())
        << "Debug data should contain grid lines when ShowGrid is enabled";
}

// ---------------------------------------------------------------------------
// Reservations drop on cancel, retarget, and destroy — not only after TTL.
// ---------------------------------------------------------------------------

TEST_F(NavSystemIntegrationTest, StandingOccupancyRemainsWhenDestinationCancelled)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 5.5f, 5.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    const uint32 agentId = GetAgent(agentEntity).AgentId;

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 5; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    EXPECT_TRUE(GridHasReservationFor(*gridMap, agentId));

    auto* statePtr = agentEntity.GetForWrite<Components::NavigationAgentState>();
    ASSERT_NE(statePtr, nullptr);
    statePtr->HasDestination = false;
    statePtr->PathDirty = false;

    // One movement tick, not a 1.5 s TTL wait.
    moveSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    const auto wt = GetTransform(agentEntity);
    uint32 hereX = 0;
    uint32 hereZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(wt.matrix[12], wt.matrix[14], hereX, hereZ));
    EXPECT_EQ(gridMap->GetCellReservation(hereX, hereZ).AgentId, agentId);
    EXPECT_NE(gridMap->GetCellReservation(hereX, hereZ).Occupancy, 0u);
    EXPECT_TRUE(GetAgentState(agentEntity).HasCellReservations);
}

TEST_F(NavSystemIntegrationTest, BodyStampRemainsOnPathDirtyRetarget)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 5.5f, 5.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    const uint32 agentId = GetAgent(agentEntity).AgentId;

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 5; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    EXPECT_TRUE(GridHasReservationFor(*gridMap, agentId));

    auto* statePtr = agentEntity.GetForWrite<Components::NavigationAgentState>();
    ASSERT_NE(statePtr, nullptr);
    statePtr->DestinationX = 10.5f;
    statePtr->DestinationZ = 10.5f;
    statePtr->HasDestination = true;
    statePtr->PathDirty = true;

    const auto wt = GetTransform(agentEntity);
    uint32 hereX = 0;
    uint32 hereZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(wt.matrix[12], wt.matrix[14], hereX, hereZ));

    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    EXPECT_EQ(gridMap->GetCellReservation(hereX, hereZ).AgentId, agentId)
        << "Retarget must keep a body stamp so a neighbour cannot step through";
    EXPECT_NE(gridMap->GetCellReservation(hereX, hereZ).Occupancy, 0u);
    EXPECT_TRUE(GetAgentState(agentEntity).HasCellReservations);
}

TEST_F(NavSystemIntegrationTest, ReservationsClearedWhenAgentDestroyed)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 5.5f, 5.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    const uint32 agentId = GetAgent(agentEntity).AgentId;

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 5; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    EXPECT_TRUE(GridHasReservationFor(*gridMap, agentId));

    agentEntity.Destroy();
    world->ProcessCommands();

    EXPECT_FALSE(GridHasReservationFor(*gridMap, agentId));
    EXPECT_FALSE(GridHasAnyReservation(*gridMap));
}

TEST_F(NavSystemIntegrationTest, ReservationsDroppedFromCellsTheAgentLeft)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    auto agentEntity = CreateAgent(1.5f, 1.5f, 14.5f, 1.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    const uint32 agentId = GetAgent(agentEntity).AgentId;

    uint32 startX = 0;
    uint32 startZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(1.5f, 1.5f, startX, startZ));

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 40; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    const auto wt = GetTransform(agentEntity);
    uint32 hereX = 0;
    uint32 hereZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(wt.matrix[12], wt.matrix[14], hereX, hereZ));
    ASSERT_NE(hereX, startX) << "Agent should have left the start cell";

    EXPECT_NE(gridMap->GetCellReservation(startX, startZ).AgentId, agentId)
        << "Start cell must not stay reserved after the agent has left";
    EXPECT_EQ(gridMap->GetCellReservation(hereX, hereZ).AgentId, agentId);
}

TEST_F(NavSystemIntegrationTest, ReservationsDroppedWhenPathDirtyWithoutReplan)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    auto agentEntity = CreateAgent(2.5f, 2.5f, 12.5f, 2.5f,
                                   src.NavMapIndex, src.NavMapGeneration);
    const uint32 agentId = GetAgent(agentEntity).AgentId;
    uint32 startX = 0;
    uint32 startZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(2.5f, 2.5f, startX, startZ));

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 8; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    EXPECT_TRUE(GridHasReservationFor(*gridMap, agentId));

    auto* statePtr = agentEntity.GetForWrite<Components::NavigationAgentState>();
    ASSERT_NE(statePtr, nullptr);
    statePtr->DestinationX = 2.5f;
    statePtr->DestinationZ = 12.5f;
    statePtr->HasDestination = true;
    statePtr->PathDirty = true;

    moveSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    const auto wt = GetTransform(agentEntity);
    uint32 hereX = 0;
    uint32 hereZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(wt.matrix[12], wt.matrix[14], hereX, hereZ));
    EXPECT_NE(gridMap->GetCellReservation(startX, startZ).AgentId, agentId)
        << "A following command must drop cells behind the agent";
    EXPECT_EQ(gridMap->GetCellReservation(hereX, hereZ).AgentId, agentId)
        << "PathDirty drops the corridor but keeps a body stamp";
    EXPECT_NE(gridMap->GetCellReservation(hereX, hereZ).Occupancy, 0u);
}

TEST_F(NavSystemIntegrationTest, IdleAgentKeepsOccupancyOnCurrentCell)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    auto walker = CreateAgent(1.5f, 8.5f, 14.5f, 8.5f, src.NavMapIndex, src.NavMapGeneration);
    auto blocker = CreateAgent(8.5f, 8.5f, 8.5f, 8.5f, src.NavMapIndex, src.NavMapGeneration);
    const uint32 blockerId = GetAgent(blocker).AgentId;

    NavigationPathfindingSystem pathSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    NavigationMovementSystem moveSystem;
    for (int i = 0; i < 4; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    auto* blockerState = blocker.GetForWrite<Components::NavigationAgentState>();
    ASSERT_NE(blockerState, nullptr);
    blockerState->HasDestination = false;

    moveSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    uint32 blockX = 0;
    uint32 blockZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(8.5f, 8.5f, blockX, blockZ));
    EXPECT_EQ(gridMap->GetCellReservation(blockX, blockZ).AgentId, blockerId);
    EXPECT_NE(gridMap->GetCellReservation(blockX, blockZ).Occupancy, 0u);

    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();
    for (int i = 0; i < 80; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    const auto walkerWt = GetTransform(walker);
    uint32 walkerX = 0;
    uint32 walkerZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(walkerWt.matrix[12], walkerWt.matrix[14], walkerX, walkerZ));
    EXPECT_FALSE(walkerX == blockX && walkerZ == blockZ)
        << "Walker must not occupy the blocker's cell";
}

TEST_F(NavSystemIntegrationTest, FailedOccupancyReplansAfterBodyLeaves)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    for (uint32 z = 0; z < kTestGridDepth; ++z)
    {
        if (z == 8)
            continue;
        gridMap->SetCellBlocked(8, z, true);
    }

    auto blocker = CreateAgent(8.5f, 8.5f, 8.5f, 8.5f, src.NavMapIndex, src.NavMapGeneration);
    auto walker = CreateAgent(1.5f, 8.5f, 14.5f, 8.5f, src.NavMapIndex, src.NavMapGeneration);
    if (auto* nav = walker.GetForWrite<Components::NavigationAgent>())
        nav->ReplanInterval = 0.0f;

    NavigationPathfindingSystem pathSystem;
    NavigationMovementSystem moveSystem;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();
    for (int i = 0; i < 4; ++i)
    {
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    auto* blockerState = blocker.GetForWrite<Components::NavigationAgentState>();
    ASSERT_NE(blockerState, nullptr);
    blockerState->HasDestination = false;
    moveSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    uint32 gapX = 0;
    uint32 gapZ = 0;
    ASSERT_TRUE(gridMap->WorldToCell(8.5f, 8.5f, gapX, gapZ));
    EXPECT_NE(gridMap->GetCellReservation(gapX, gapZ).Occupancy, 0u);

    auto* walkerState = walker.GetForWrite<Components::NavigationAgentState>();
    ASSERT_NE(walkerState, nullptr);
    walkerState->PathDirty = true;
    pathSystem.Update(*world, kDeltaTime);
    world->ProcessCommands();

    const auto occupiedStatus = static_cast<Pathfinding::PathStatus>(GetAgentState(walker).Status);
    EXPECT_TRUE(occupiedStatus == Pathfinding::PathStatus::Complete ||
                occupiedStatus == Pathfinding::PathStatus::Partial)
        << "Occupancy on the only corridor cell is a tax, not Failed when dest is walkable";
    {
        const auto walkerWt = GetTransform(walker);
        uint32 walkerX = 0;
        uint32 walkerZ = 0;
        ASSERT_TRUE(gridMap->WorldToCell(walkerWt.matrix[12], walkerWt.matrix[14], walkerX, walkerZ));
        EXPECT_FALSE(walkerX == gapX && walkerZ == gapZ)
            << "Movement must still refuse another agent's body cell";
    }

    blocker.Destroy();
    world->ProcessCommands();

    for (int i = 0; i < 120; ++i)
    {
        pathSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
    }

    const auto status = static_cast<Pathfinding::PathStatus>(GetAgentState(walker).Status);
    EXPECT_TRUE(status == Pathfinding::PathStatus::Complete ||
                status == Pathfinding::PathStatus::Partial)
        << "Walker must recover after the occupying body leaves";
    EXPECT_GT(GetTransform(walker).matrix[12], 8.0f);
}

TEST_F(NavSystemIntegrationTest, HeadOnCorridorDoesNotShareACell)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    for (uint32 z = 0; z < kTestGridDepth; ++z)
    {
        if (z == 8)
            continue;
        gridMap->SetCellBlocked(8, z, true);
    }

    auto left = CreateAgent(1.5f, 8.5f, 14.5f, 8.5f, src.NavMapIndex, src.NavMapGeneration);
    auto right = CreateAgent(14.5f, 8.5f, 1.5f, 8.5f, src.NavMapIndex, src.NavMapGeneration);
    if (auto* nav = left.GetForWrite<Components::NavigationAgent>())
        nav->ReplanInterval = 0.0f;
    if (auto* nav = right.GetForWrite<Components::NavigationAgent>())
        nav->ReplanInterval = 0.0f;

    NavigationPathfindingSystem pathSystem;
    NavigationMovementSystem moveSystem;
    int shared = 0;
    for (int i = 0; i < 300; ++i)
    {
        pathSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();

        const auto l = GetTransform(left);
        const auto r = GetTransform(right);
        uint32 lx = 0, lz = 0, rx = 0, rz = 0;
        ASSERT_TRUE(gridMap->WorldToCell(l.matrix[12], l.matrix[14], lx, lz));
        ASSERT_TRUE(gridMap->WorldToCell(r.matrix[12], r.matrix[14], rx, rz));
        if (lx == rx && lz == rz)
            ++shared;
    }
    EXPECT_EQ(shared, 0) << "Occupancy refuse must keep a body stamp so the other agent cannot step in";
    EXPECT_GT(GetTransform(left).matrix[12], 3.0f)
        << "Must leave spawn; a freeze is not a shared-cell pass";
    EXPECT_LT(GetTransform(right).matrix[12], 13.0f)
        << "Must leave spawn; a freeze is not a shared-cell pass";
}

TEST_F(NavSystemIntegrationTest, DestAllowedPairDoesNotShareACell)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    // Door at x=8. Dests sit on each other's spawn so A* dest-occupancy
    // is the only legal close. A spawn freeze with shared==0 is a false pass.
    for (uint32 z = 0; z < kTestGridDepth; ++z)
    {
        if (z == 8)
            continue;
        gridMap->SetCellBlocked(8, z, true);
    }

    auto left = CreateAgent(2.5f, 8.5f, 9.5f, 8.5f, src.NavMapIndex, src.NavMapGeneration);
    auto right = CreateAgent(9.5f, 8.5f, 2.5f, 8.5f, src.NavMapIndex, src.NavMapGeneration);
    if (auto* nav = left.GetForWrite<Components::NavigationAgent>())
        nav->ReplanInterval = 0.0f;
    if (auto* nav = right.GetForWrite<Components::NavigationAgent>())
        nav->ReplanInterval = 0.0f;

    NavigationPathfindingSystem pathSystem;
    NavigationMovementSystem moveSystem;
    int shared = 0;
    for (int i = 0; i < 300; ++i)
    {
        pathSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();

        const auto l = GetTransform(left);
        const auto r = GetTransform(right);
        uint32 lx = 0, lz = 0, rx = 0, rz = 0;
        ASSERT_TRUE(gridMap->WorldToCell(l.matrix[12], l.matrix[14], lx, lz));
        ASSERT_TRUE(gridMap->WorldToCell(r.matrix[12], r.matrix[14], rx, rz));
        if (lx == rx && lz == rz)
            ++shared;
    }
    EXPECT_EQ(shared, 0)
        << "Dest-allowed occupancy must not let a PathDirty replan drop the body stamp";

    const auto leftEnd = GetTransform(left);
    const auto rightEnd = GetTransform(right);
    const bool leftMoved = leftEnd.matrix[12] > 3.5f;
    const bool rightMoved = rightEnd.matrix[12] < 8.5f;
    const bool leftArrived = !GetAgentState(left).HasDestination;
    const bool rightArrived = !GetAgentState(right).HasDestination;
    EXPECT_TRUE((leftMoved && rightMoved) || (leftArrived && rightArrived))
        << "Dest-allowed pair must close, not freeze on dest occupancy. "
        << "L=" << leftEnd.matrix[12] << " R=" << rightEnd.matrix[12]
        << " leftDest=" << GetAgentState(left).HasDestination
        << " rightDest=" << GetAgentState(right).HasDestination;
}

TEST_F(NavSystemIntegrationTest, CrossingPathsNeverShareACellAndBothArrive)
{
    auto gridEntity = CreateGridSource();
    auto src = GetGridSource(gridEntity);

    auto* navWorld = NavigationService::TryGet();
    ASSERT_NE(navWorld, nullptr);

    Pathfinding::NavMapHandle mapHandle{src.NavMapIndex, src.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    ASSERT_NE(gridMap, nullptr);

    // Open field, perpendicular routes meeting at (8,8) with equal travel
    // distance, so both agents contend for the crossing cell on the same tick.
    // The two halves of the occupancy contract are asserted together: A* taxes
    // the crossing rather than refusing it, so neither agent may end Failed and
    // stall; movement refuses another agent's body cell, so no tick may ever
    // find them in the same cell.
    auto westEast = CreateAgent(2.5f, 8.5f, 13.5f, 8.5f, src.NavMapIndex, src.NavMapGeneration);
    auto northSouth = CreateAgent(8.5f, 2.5f, 8.5f, 13.5f, src.NavMapIndex, src.NavMapGeneration);
    if (auto* nav = westEast.GetForWrite<Components::NavigationAgent>())
        nav->ReplanInterval = 0.0f;
    if (auto* nav = northSouth.GetForWrite<Components::NavigationAgent>())
        nav->ReplanInterval = 0.0f;

    NavigationPathfindingSystem pathSystem;
    NavigationMovementSystem moveSystem;

    int shared = 0;
    bool westEastArrived = false;
    bool northSouthArrived = false;
    constexpr int kMaxIterations = 900; // 15s at 60fps; the direct route is ~3s

    for (int i = 0; i < kMaxIterations; ++i)
    {
        pathSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();
        moveSystem.Update(*world, kDeltaTime);
        world->ProcessCommands();

        const auto we = GetTransform(westEast);
        const auto ns = GetTransform(northSouth);
        uint32 wx = 0, wz = 0, nx = 0, nz = 0;
        ASSERT_TRUE(gridMap->WorldToCell(we.matrix[12], we.matrix[14], wx, wz));
        ASSERT_TRUE(gridMap->WorldToCell(ns.matrix[12], ns.matrix[14], nx, nz));
        if (wx == nx && wz == nz)
            ++shared;

        westEastArrived = westEastArrived || !GetAgentState(westEast).HasDestination;
        northSouthArrived = northSouthArrived || !GetAgentState(northSouth).HasDestination;
        if (westEastArrived && northSouthArrived)
            break;
    }

    EXPECT_EQ(shared, 0) << "Crossing agents must never occupy the same cell on any tick";
    EXPECT_TRUE(westEastArrived)
        << "West-east agent must finish; a taxed crossing must not become a permanent stall. "
        << "x=" << GetTransform(westEast).matrix[12]
        << " status=" << static_cast<int>(GetAgentState(westEast).Status);
    EXPECT_TRUE(northSouthArrived)
        << "North-south agent must finish; a taxed crossing must not become a permanent stall. "
        << "z=" << GetTransform(northSouth).matrix[14]
        << " status=" << static_cast<int>(GetAgentState(northSouth).Status);
}
