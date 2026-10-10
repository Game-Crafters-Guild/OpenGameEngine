#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathBuffer.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include <gtest/gtest.h>

using namespace GameEngine;
using namespace GameEngine::Components;
using namespace GameEngine::PathfindingECS;

namespace
{
constexpr uint32 kPeer = 999;
constexpr float32 kDt = 0.05f;
// Far beyond any time these cases simulate, so a peer body never expires out
// from under an assertion.
constexpr float32 kPeerBodyExpiration = 10000.0f;
// Radius 1.1 over a 1-unit cell is the existing convention's two-cell
// footprint: ceil(1.1 / 1) == 2.
constexpr float32 kTwoCellRadius = 1.1f;

class NavigationOccupiedStartTest : public testing::Test
{
protected:
    ECS::World World{nullptr};
    NavigationPathfindingSystem Paths;
    NavigationMovementSystem Movement;
    Pathfinding::NavMapHandle Map{};
    ECS::EntityHandle Actor{};

    void SetUp() override
    {
        NavigationService::Initialize();
        Pathfinding::GridSettings grid;
        grid.Width = grid.Depth = 32;
        grid.CellSize = 1.0f;
        Map = NavigationService::Get().AddGridMap(grid);
    }
    void TearDown() override
    {
        World.Query<ECS::Read<NavigationAgentState>>().Each([](const NavigationAgentState& state)
        {
            if (state.PathGeneration)
                NavigationService::Get().GetPathBuffer().FreePath({state.PathIndex, state.PathGeneration});
        });
        World.Clear();
        NavigationService::Get().RemoveMap(Map);
        NavigationService::Shutdown();
    }
    Pathfinding::GridMap& Grid() { return *NavigationService::Get().GetGridMap(Map); }
    ECS::Entity Unit() { return {&World, Actor}; }
    const NavigationAgentState& State() { return *Unit().Get<NavigationAgentState>(); }
    Mathematics::Vector3 Position() { return Unit().Get<Transform>()->GetPosition(); }
    void Create(float32 x = 4.25f, float32 z = 4.25f, float32 radius = 0.45f,
                float32 endX = 12.5f, float32 endZ = 4.5f)
    {
        NavigationAgent agent{};
        agent.AgentId = 1;
        agent.Speed = 4.0f;
        agent.Radius = radius;
        agent.StoppingDistance = 0.3f;
        agent.SmoothPaths = false;
        agent.ReplanInterval = 0.0f;
        agent.NavMapIndex = Map.Index;
        agent.NavMapGeneration = Map.Generation;
        NavigationAgentState state{};
        state.DestinationX = endX;
        state.DestinationZ = endZ;
        state.HasDestination = state.PathDirty = true;
        Transform xf{};
        xf.matrix[12] = x;
        xf.matrix[14] = z;
        WorldTransform wt{};
        std::copy(std::begin(xf.matrix), std::end(xf.matrix), std::begin(wt.matrix));
        Actor = World.Create(agent, state, xf, wt).GetHandle();
        World.ProcessCommands();
        Paths.Update(World, kDt);
        ASSERT_EQ(static_cast<Pathfinding::PathStatus>(State().Status), Pathfinding::PathStatus::Complete);
    }
    void Occupy(uint32 x, uint32 z)
    {
        // The ordinary grid API represents an idle peer's body publication.
        // Publish after pathfinding so the movement consumer sees that peer's
        // ownership, just as when the peer runs earlier in its movement query.
        Grid().ReserveCell(x, z, kPeer, kPeerBodyExpiration, true);
    }
    void Step(uint32 x, uint32 z, float32 dt = kDt)
    {
        Paths.Update(World, dt);
        Occupy(x, z);
        Movement.Update(World, dt);
    }
};

TEST_F(NavigationOccupiedStartTest, EscapesAnAlreadyOccupiedStartWithoutReplanningForever)
{
    Create();
    uint32 replans = 0;
    auto generation = State().PathGeneration;
    for (uint32 i = 0; i < 160; ++i)
    {
        Step(4, 4);
        if (generation != State().PathGeneration)
        {
            ++replans;
            generation = State().PathGeneration;
        }
    }
    EXPECT_GT(Position().x, 11.0f);
    EXPECT_FALSE(State().HasDestination);
    EXPECT_LT(replans, 4u);
    EXPECT_EQ(Grid().GetCellReservation(4, 4).AgentId, kPeer);
}

TEST_F(NavigationOccupiedStartTest, BothLookaheadAndSameCellStepPermitDeparture)
{
    Create();
    const auto before = Position();
    Occupy(4, 4);
    Movement.Update(World, kDt);
    EXPECT_FALSE(State().PathDirty);
    EXPECT_GT((Position() - before).Length(), 0.001f);
}

TEST_F(NavigationOccupiedStartTest, LargeAgentCanLeaveOverlapAtItsExistingFootprintEdge)
{
    Create(4.25f, 4.25f, kTwoCellRadius);
    Occupy(6, 4);
    Movement.Update(World, kDt);
    EXPECT_FALSE(State().PathDirty);
    EXPECT_GT(Position().x, 4.25f);
}

TEST_F(NavigationOccupiedStartTest, LargeAgentStillRejectsNewOccupiedFootprintCells)
{
    Create(4.25f, 4.25f, kTwoCellRadius);
    Occupy(7, 4);
    Movement.Update(World, kDt);
    EXPECT_TRUE(State().PathDirty);
}

// Cell (6, 2) lies inside the two-cell starting footprint but off the route's
// cell row, so only the footprint sweep's block check can reach it. Occupying
// it as well is what discriminates: the occupancy is exempt, so a still-dirty
// path can only come from the block check.
TEST_F(NavigationOccupiedStartTest, ExistingFootprintDoesNotExemptStaticBlocks)
{
    Create(4.25f, 4.25f, kTwoCellRadius);
    Grid().SetCellBlocked(6, 2, true);
    Occupy(6, 2);
    Movement.Update(World, kDt);
    EXPECT_TRUE(State().PathDirty);
    EXPECT_TRUE(Grid().IsCellBlocked(6, 2));
}

TEST_F(NavigationOccupiedStartTest, ExistingFootprintDoesNotExemptDynamicBlocks)
{
    Create(4.25f, 4.25f, kTwoCellRadius);
    Grid().SetCellDynamicBlocked(6, 2, true);
    Occupy(6, 2);
    Movement.Update(World, kDt);
    EXPECT_TRUE(State().PathDirty);
    EXPECT_TRUE(Grid().IsCellDynamicBlocked(6, 2));
}

TEST_F(NavigationOccupiedStartTest, NewOccupiedAdjacentCellStillStopsTheStep)
{
    Create(4.5f, 4.5f);
    const auto before = Position();
    Occupy(5, 4);
    Movement.Update(World, 0.25f);
    EXPECT_TRUE(State().PathDirty);
    EXPECT_EQ(Position().x, before.x);
    EXPECT_TRUE(State().HasDestination);
}

TEST_F(NavigationOccupiedStartTest, OccupiedNewDestinationStillStandsOff)
{
    Create(4.5f, 4.5f, 0.45f, 5.5f, 4.5f);
    Occupy(5, 4);
    Movement.Update(World, 0.25f);
    EXPECT_FALSE(State().HasDestination);
    EXPECT_LT(Position().x, 5.0f);
}

TEST_F(NavigationOccupiedStartTest, AnEscapedPeerCellHasNoPersistentReentryPermission)
{
    Create();
    for (uint32 i = 0; i < 100; ++i)
        Step(4, 4);
    ASSERT_GT(Position().x, 8.0f);
    auto* state = Unit().GetForWrite<NavigationAgentState>();
    state->DestinationX = 4.5f;
    state->DestinationZ = 4.5f;
    state->HasDestination = state->PathDirty = true;
    for (uint32 i = 0; i < 120; ++i)
        Step(4, 4);
    EXPECT_FALSE(State().HasDestination);
    EXPECT_GE(Position().x, 5.0f);
    EXPECT_EQ(Grid().GetCellReservation(4, 4).AgentId, kPeer);
}

TEST_F(NavigationOccupiedStartTest, OccupancyOneCellAheadAlongZIsNotExempt)
{
    Create(4.5f, 8.5f, 0.45f, 4.5f, 12.5f);
    Occupy(4, 9);
    const auto before = Position();
    Movement.Update(World, 0.25f);
    EXPECT_TRUE(State().PathDirty);
    EXPECT_EQ(Position().z, before.z);
}
} // namespace
