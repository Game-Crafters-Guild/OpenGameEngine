#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/PathBuffer.h"
#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/Systems/NavigationMovementSystem.h"
#include "PathfindingECS/Systems/NavigationPathfindingSystem.h"
#include "PathfindingECS/Systems/NavigationWorldHooks.h"

#include <cmath>
#include <gtest/gtest.h>
#include <memory>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::PathfindingECS;

namespace
{
constexpr float32 kDt = 1.0f / 60.0f;
constexpr uint32 kRequesterId = 101;
constexpr uint32 kPeerId = 202;
constexpr uint32 kWidth = 16;
constexpr uint32 kDepth = 12;
enum class PeerKind
{
    Idle,
    Direct,
    Requesting
};

class InitialOccupancy : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        NavigationService::Initialize();
        world = std::make_unique<ECS::World>();
        RegisterNavigationWorldHooks(*world);
        // This test owns one ordinary map independently of a scene component.
        // Agent removal hooks run before the map is released in TearDown.
        Pathfinding::GridSettings settings{};
        settings.Width = kWidth;
        settings.Depth = kDepth;
        mapHandle = NavigationService::TryGet()->AddGridMap(settings);
        ASSERT_TRUE(mapHandle.IsValid());
        map = NavigationService::TryGet()->GetGridMap(mapHandle);
        ASSERT_NE(map, nullptr);
        for (uint32 z = 0; z < kDepth; ++z)
            for (uint32 x = 0; x < kWidth; ++x)
            {
                map->SetCellBlocked(x, z, false);
                map->SetCellHeight(x, z, 0.0f);
                map->SetCellCost(x, z, 1.0f);
            }
    }

    void TearDown() override
    {
        // No asynchronous navigation is enabled. Actual native hooks retire
        // reservations/maps; service shutdown owns remaining PathBuffer data.
        world.reset();
        map = nullptr;
        NavigationService::TryGet()->RemoveMap(mapHandle);
        NavigationService::Shutdown();
    }

    ECS::Entity Actor(uint32 id, float32 x, float32 z, bool request, float32 goalX = 12.5f,
                      float32 goalZ = 5.5f, float32 radius = 0.25f)
    {
        Components::NavigationAgent agent{};
        agent.AgentId = id; // Explicit unique IDs isolate this native issue.
        agent.NavMapIndex = mapHandle.Index;
        agent.NavMapGeneration = mapHandle.Generation;
        agent.ReplanInterval = 0.0f;
        agent.SmoothPaths = false; // Observe A* cells, not string-pull policy.
        agent.Radius = radius;
        agent.Speed = 3.5f;
        agent.Acceleration = 100.0f;
        Components::NavigationAgentState state{};
        state.HasDestination = request;
        state.PathDirty = request;
        state.DestinationX = goalX;
        state.DestinationZ = goalZ;
        Components::Transform local{};
        Components::WorldTransform global{};
        for (int i : {0, 5, 10, 15})
            local.matrix[i] = global.matrix[i] = 1.0f;
        local.matrix[12] = global.matrix[12] = x;
        local.matrix[14] = global.matrix[14] = z;
        // Identical bundles keep both actors in the same archetype. Query order
        // is still measured explicitly instead of assuming creation order.
        return world->Create(agent, state, local, global);
    }

    ECS::Entity Peer(PeerKind kind)
    {
        auto peer = Actor(kPeerId, 7.5f, 5.5f, kind == PeerKind::Requesting, 7.5f, 9.5f);
        if (kind == PeerKind::Direct)
        {
            auto* state = peer.GetForWrite<Components::NavigationAgentState>();
            state->HasDestination = true;
            state->PathDirty = false;
            // Direct game movement has no native PathBuffer route.
        }
        return peer;
    }

    std::vector<uint32> QueryOrder()
    {
        std::vector<uint32> ids;
        world
            ->Query<ECS::Read<Components::NavigationAgent>, ECS::Write<Components::NavigationAgentState>,
                    ECS::Read<Components::WorldTransform>>()
            .Each([&](ECS::EntityHandle, const Components::NavigationAgent& agent, Components::NavigationAgentState&,
                      const Components::WorldTransform&) { ids.push_back(agent.AgentId); });
        return ids;
    }

    std::vector<Pathfinding::PathPoint> Points(ECS::Entity actor)
    {
        const auto* state = actor.Get<Components::NavigationAgentState>();
        EXPECT_EQ(static_cast<Pathfinding::PathStatus>(state->Status), Pathfinding::PathStatus::Complete);
        EXPECT_NE(state->PathGeneration, 0u);
        const Pathfinding::PathHandle path{state->PathIndex, state->PathGeneration};
        auto& buffer = NavigationService::TryGet()->GetPathBuffer();
        std::vector<Pathfinding::PathPoint> result(buffer.GetPointCount(path));
        buffer.CopyPoints(path, result.data(), static_cast<uint32>(result.size()));
        return result;
    }

    // Carries the solved route in the assertion message so a failure names the
    // cells that were taken, whichever way round the expectation is written.
    ::testing::AssertionResult Crosses(ECS::Entity actor, uint32 cellX, uint32 cellZ)
    {
        const auto points = Points(actor);
        EXPECT_GE(points.size(), 2u);
        bool crosses = false;
        ::testing::Message route;
        for (const auto& p : points)
        {
            uint32 x{}, z{};
            const bool onMap = map->WorldToCell(p.X, p.Z, x, z);
            route << " (" << p.X << "," << p.Z << ")";
            if (onMap && x == cellX && z == cellZ)
                crosses = true;
        }
        ::testing::AssertionResult result =
            crosses ? ::testing::AssertionSuccess() : ::testing::AssertionFailure();
        result << "route cells:" << route;
        return result;
    }

    void ExpectBody(uint32 x, uint32 z, uint32 id, float32 currentTime = kDt)
    {
        const auto cell = map->GetCellReservation(x, z);
        EXPECT_EQ(cell.AgentId, id);
        EXPECT_EQ(cell.Occupancy, 1u);
        EXPECT_TRUE(cell.IsActive(currentTime));
    }

    void PreStamp(ECS::Entity actor)
    {
        const auto* agent = actor.Get<Components::NavigationAgent>();
        const auto* xf = actor.Get<Components::WorldTransform>();
        auto* state = actor.GetForWrite<Components::NavigationAgentState>();
        StampNavigationAgentOccupancy(*agent, *state, xf->matrix[12], xf->matrix[14], 0.0f);
    }

    std::unique_ptr<ECS::World> world;
    Pathfinding::GridMap* map{};
    Pathfinding::NavMapHandle mapHandle{};
    NavigationPathfindingSystem pathfinding;
    NavigationMovementSystem movement;
};

class PeerPermutation : public InitialOccupancy, public ::testing::WithParamInterface<std::tuple<PeerKind, bool>>
{
};

TEST_P(PeerPermutation, FirstSolveSeesPeerBodyAndUsesCheapDetour)
{
    const auto [kind, peerFirst] = GetParam();
    auto requester = [&] {
        if (peerFirst)
            Peer(kind);
        auto actor = Actor(kRequesterId, 2.5f, 5.5f, true);
        if (!peerFirst)
            Peer(kind);
        return actor;
    }();
    const std::vector<uint32> expected =
        peerFirst ? std::vector<uint32>{kPeerId, kRequesterId} : std::vector<uint32>{kRequesterId, kPeerId};
    ASSERT_EQ(QueryOrder(), expected) << "Fixture must actually permute the native query";
    ASSERT_EQ(map->GetReservedAgentCount(), 0u);
    pathfinding.Update(*world, kDt);
    // Deliberately before NMS: a later body stamp cannot repair the first solve.
    ExpectBody(2, 5, kRequesterId);
    ExpectBody(7, 5, kPeerId);
    EXPECT_FALSE(Crosses(requester, 7, 5));
}

INSTANTIATE_TEST_SUITE_P(QueryOrders, PeerPermutation,
                         ::testing::Combine(::testing::Values(PeerKind::Idle, PeerKind::Direct, PeerKind::Requesting),
                                            ::testing::Bool()));

TEST_F(InitialOccupancy, RealPrestampedPeerMakesTheOpenDetourCheaper)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Peer(PeerKind::Idle);
    PreStamp(peer);
    pathfinding.Update(*world, kDt);
    ExpectBody(7, 5, kPeerId);
    EXPECT_FALSE(Crosses(requester, 7, 5));
}

TEST_F(InitialOccupancy, EmptyRouteUsesTheStraightCells)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    pathfinding.Update(*world, kDt);
    ExpectBody(2, 5, kRequesterId);
    EXPECT_TRUE(Crosses(requester, 7, 5));
}

TEST_F(InitialOccupancy, NearbyStaticObstacleStillConstrainsTheRequester)
{
    // Direct next cell blocked; room on both sides. Body must be present but
    // excluded from its owner's request. Occupancy must not relax base blocks.
    map->SetCellBlocked(3, 5, true);
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    pathfinding.Update(*world, kDt);
    ExpectBody(2, 5, kRequesterId);
    EXPECT_FALSE(Crosses(requester, 3, 5));
    for (const auto& point : Points(requester))
    {
        uint32 x{}, z{};
        ASSERT_TRUE(map->WorldToCell(point.X, point.Z, x, z));
        EXPECT_FALSE(map->IsCellBlocked(x, z));
    }
}

TEST_F(InitialOccupancy, RequesterRetainsTheExistingLargeBodyFootprint)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true, 12.5f, 5.5f, 0.75f);
    pathfinding.Update(*world, kDt);
    for (uint32 z = 4; z <= 6; ++z)
        for (uint32 x = 1; x <= 3; ++x)
            ExpectBody(x, z, kRequesterId);
    EXPECT_TRUE(Crosses(requester, 7, 5));
}

TEST_F(InitialOccupancy, StaleTrueFlagDoesNotReplaceAMissingIdleBody)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Peer(PeerKind::Idle);
    PreStamp(peer);
    map->ClearExpiredReservations(2.0f);
    ASSERT_TRUE(peer.Get<Components::NavigationAgentState>()->HasCellReservations);
    ASSERT_EQ(map->GetCellReservation(7, 5).AgentId, 0u);
    pathfinding.Update(*world, 2.0f);
    ExpectBody(7, 5, kPeerId, 2.0f);
    EXPECT_FALSE(Crosses(requester, 7, 5));
}

TEST_F(InitialOccupancy, NativeMovementProducesIdleBodyAfterTheRequestBoundary)
{
    auto peer = Peer(PeerKind::Idle);
    ASSERT_FALSE(peer.Get<Components::NavigationAgentState>()->HasCellReservations);
    movement.Update(*world, kDt);
    ExpectBody(7, 5, kPeerId);
}

TEST_F(InitialOccupancy, NativeMovingCorridorSurvivesAnotherActorsFirstRequest)
{
    auto peer = Actor(kPeerId, 2.5f, 2.5f, true, 12.5f, 2.5f);
    pathfinding.Update(*world, kDt);
    movement.Update(*world, kDt);
    ASSERT_GT(peer.Get<Components::WorldTransform>()->matrix[12], 2.5f);
    ASSERT_TRUE(peer.Get<Components::NavigationAgentState>()->HasDestination);
    ASSERT_FALSE(peer.Get<Components::NavigationAgentState>()->PathDirty);
    struct Cell
    {
        uint32 x, z;
        Pathfinding::CellReservation value;
    };
    std::vector<Cell> corridor;
    for (uint32 z = 0; z < kDepth; ++z)
        for (uint32 x = 0; x < kWidth; ++x)
        {
            const auto reservation = map->GetCellReservation(x, z);
            if (reservation.AgentId == kPeerId && reservation.Occupancy == 0 && reservation.IsActive(2.0f * kDt))
                corridor.push_back({x, z, reservation});
        }
    ASSERT_GE(corridor.size(), 2u) << "Must observe real NMS lookahead, not a hand-written fake";
    auto requester = Actor(kRequesterId, 2.5f, 8.5f, true, 12.5f, 8.5f);
    pathfinding.Update(*world, kDt);
    EXPECT_TRUE(Crosses(requester, 7, 8));
    for (const auto& cell : corridor)
    {
        const auto after = map->GetCellReservation(cell.x, cell.z);
        EXPECT_EQ(after.AgentId, kPeerId);
        EXPECT_EQ(after.Occupancy, cell.value.Occupancy);
        EXPECT_FLOAT_EQ(after.ExpirationTime, cell.value.ExpirationTime);
    }
}
TEST_F(InitialOccupancy, StaticZeroIdDoesNotBecomeAReservedBody)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Actor(0, 7.5f, 5.5f, false);
    peer.GetForWrite<Components::NavigationAgent>()->Speed = 0.0f;
    pathfinding.Update(*world, kDt);
    EXPECT_FALSE(peer.Get<Components::NavigationAgentState>()->HasCellReservations);
    EXPECT_EQ(map->GetCellReservation(7, 5).AgentId, 0u);
    EXPECT_EQ(map->GetCellReservation(7, 5).Occupancy, 0u);
    EXPECT_TRUE(Crosses(requester, 7, 5));
}

TEST_F(InitialOccupancy, InvalidRequesterMapDoesNotPrepareUnrelatedBodies)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Peer(PeerKind::Idle);
    requester.GetForWrite<Components::NavigationAgent>()->NavMapGeneration = 0;
    pathfinding.Update(*world, kDt);
    EXPECT_EQ(map->GetReservedAgentCount(), 0u);
    EXPECT_FALSE(peer.Get<Components::NavigationAgentState>()->HasCellReservations);
    EXPECT_EQ(requester.Get<Components::NavigationAgentState>()->PathGeneration, 0u);
}

TEST_F(InitialOccupancy, StaleRequesterMapDoesNotPrepareUnrelatedBodies)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    Peer(PeerKind::Idle);
    ++requester.GetForWrite<Components::NavigationAgent>()->NavMapGeneration;
    pathfinding.Update(*world, kDt);
    EXPECT_EQ(map->GetReservedAgentCount(), 0u);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(requester.Get<Components::NavigationAgentState>()->Status),
              Pathfinding::PathStatus::Failed);
}

TEST_F(InitialOccupancy, RemovedRequesterMapDoesNotPrepareUnrelatedBodies)
{
    Pathfinding::GridSettings settings{};
    auto removed = NavigationService::TryGet()->AddGridMap(settings);
    NavigationService::TryGet()->RemoveMap(removed);
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    Peer(PeerKind::Idle);
    auto* agent = requester.GetForWrite<Components::NavigationAgent>();
    agent->NavMapIndex = removed.Index;
    agent->NavMapGeneration = removed.Generation;
    pathfinding.Update(*world, kDt);
    EXPECT_EQ(map->GetReservedAgentCount(), 0u);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(requester.Get<Components::NavigationAgentState>()->Status),
              Pathfinding::PathStatus::Failed);
}

TEST_F(InitialOccupancy, InvalidPeerMapDoesNotStampAnotherMap)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Peer(PeerKind::Idle);
    ++peer.GetForWrite<Components::NavigationAgent>()->NavMapGeneration;
    pathfinding.Update(*world, kDt);
    EXPECT_EQ(map->GetCellReservation(7, 5).AgentId, 0u);
    EXPECT_FALSE(peer.Get<Components::NavigationAgentState>()->HasCellReservations);
    EXPECT_TRUE(Crosses(requester, 7, 5));
}

TEST_F(InitialOccupancy, OutsideRequesterDoesNotInventABodyCell)
{
    auto requester = Actor(kRequesterId, -1.5f, 5.5f, true);
    pathfinding.Update(*world, kDt);
    EXPECT_EQ(map->GetReservedAgentCount(), 0u);
    EXPECT_FALSE(requester.Get<Components::NavigationAgentState>()->HasCellReservations);
    EXPECT_EQ(static_cast<Pathfinding::PathStatus>(requester.Get<Components::NavigationAgentState>()->Status),
              Pathfinding::PathStatus::Failed);
}

TEST_F(InitialOccupancy, OutsidePeerDoesNotOccupyAnEdgeCell)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Actor(kPeerId, -1.5f, 5.5f, false);
    pathfinding.Update(*world, kDt);
    EXPECT_EQ(map->GetCellReservation(0, 5).AgentId, 0u);
    EXPECT_FALSE(peer.Get<Components::NavigationAgentState>()->HasCellReservations);
    EXPECT_TRUE(Crosses(requester, 7, 5));
}

TEST_F(InitialOccupancy, WarmBodyWithoutARequestKeepsItsReservationBytes)
{
    auto peer = Peer(PeerKind::Idle);
    PreStamp(peer);
    const auto before = map->GetCellReservation(7, 5);
    pathfinding.Update(*world, kDt);
    const auto after = map->GetCellReservation(7, 5);
    EXPECT_EQ(after.AgentId, before.AgentId);
    EXPECT_EQ(after.Occupancy, before.Occupancy);
    EXPECT_FLOAT_EQ(after.ExpirationTime, before.ExpirationTime);
}

TEST_F(InitialOccupancy, WarmBodyKeepsItsStampDuringAnotherRequest)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Peer(PeerKind::Direct);
    PreStamp(peer);
    const auto before = map->GetCellReservation(7, 5);
    pathfinding.Update(*world, kDt);
    const auto after = map->GetCellReservation(7, 5);
    EXPECT_EQ(after.AgentId, before.AgentId);
    EXPECT_EQ(after.Occupancy, before.Occupancy);
    EXPECT_FLOAT_EQ(after.ExpirationTime, before.ExpirationTime);
    EXPECT_FALSE(Crosses(requester, 7, 5));
}

TEST_F(InitialOccupancy, DirectPeerChangingCellsReplacesItsFormerBody)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Peer(PeerKind::Direct);
    PreStamp(peer);
    peer.GetForWrite<Components::WorldTransform>()->matrix[12] = 9.5f;
    peer.GetForWrite<Components::Transform>()->matrix[12] = 9.5f;
    pathfinding.Update(*world, kDt);
    EXPECT_EQ(map->GetCellReservation(7, 5).AgentId, 0u);
    ExpectBody(9, 5, kPeerId);
    EXPECT_FALSE(Crosses(requester, 9, 5));
}

TEST_F(InitialOccupancy, MissingLargeFootprintCornerIsRepairedDespiteItsWarmCentre)
{
    Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Actor(kPeerId, 7.5f, 5.5f, false, 0.0f, 0.0f, 0.75f);
    PreStamp(peer);
    map->ClearCellReservation(8, 6);
    ASSERT_TRUE(peer.Get<Components::NavigationAgentState>()->HasCellReservations);
    ExpectBody(7, 5, kPeerId);
    pathfinding.Update(*world, kDt);
    for (uint32 z = 4; z <= 6; ++z)
        for (uint32 x = 6; x <= 8; ++x)
            ExpectBody(x, z, kPeerId);
}

TEST_F(InitialOccupancy, WarmLargeBodyClippedAtMapEdgeIsNotRebuilt)
{
    auto requester = Actor(kRequesterId, 2.5f, 5.5f, true);
    auto peer = Actor(kPeerId, 0.5f, 0.5f, false, 0.0f, 0.0f, 0.75f);
    PreStamp(peer);
    const auto before = map->GetCellReservation(0, 0);
    pathfinding.Update(*world, kDt);
    for (uint32 z = 0; z <= 1; ++z)
        for (uint32 x = 0; x <= 1; ++x)
        {
            ExpectBody(x, z, kPeerId);
            EXPECT_FLOAT_EQ(map->GetCellReservation(x, z).ExpirationTime, before.ExpirationTime);
        }
    EXPECT_TRUE(Crosses(requester, 7, 5));
}

TEST_F(InitialOccupancy, DirtyReplannerDropsOnlyItsOwnNativeCorridor)
{
    auto dirty = Actor(kPeerId, 2.5f, 2.5f, true, 12.5f, 2.5f);
    auto steady = Actor(303, 2.5f, 8.5f, true, 12.5f, 8.5f);
    pathfinding.Update(*world, kDt);
    movement.Update(*world, kDt);
    ASSERT_GT(dirty.Get<Components::WorldTransform>()->matrix[12], 2.5f);
    ASSERT_GT(steady.Get<Components::WorldTransform>()->matrix[12], 2.5f);
    ASSERT_EQ(map->GetCellReservation(4, 2).AgentId, kPeerId);
    ASSERT_EQ(map->GetCellReservation(4, 2).Occupancy, 0u);
    const auto other = map->GetCellReservation(4, 8);
    ASSERT_EQ(other.AgentId, 303u);
    ASSERT_EQ(other.Occupancy, 0u);
    auto* state = dirty.GetForWrite<Components::NavigationAgentState>();
    state->DestinationZ = 5.5f;
    state->PathDirty = true;
    pathfinding.Update(*world, kDt);
    EXPECT_EQ(map->GetCellReservation(4, 2).AgentId, 0u);
    ExpectBody(2, 2, kPeerId, 2 * kDt);
    const auto after = map->GetCellReservation(4, 8);
    EXPECT_EQ(after.AgentId, other.AgentId);
    EXPECT_EQ(after.Occupancy, other.Occupancy);
    EXPECT_FLOAT_EQ(after.ExpirationTime, other.ExpirationTime);
}
} // namespace
