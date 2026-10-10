#include "PathfindingECS/Systems/NavigationWorldHooks.h"

#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationAgentState.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/Components/NavigationMesh.h"
#include "PathfindingECS/NavigationService.h"

#include "ECS/Entity.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/NavigationWorld.h"

namespace GameEngine::PathfindingECS
{

namespace
{

// Also runs from ~World at process shutdown, where the navigation service may
// already be gone. Clear the handle either way: leaving it set would hand a
// revived component (undo of a delete) a map index that names nothing.
void ReleaseMap(uint32& mapIndex, uint32& mapGeneration, bool& initialized)
{
    if (initialized)
    {
        if (auto* navWorld = NavigationService::TryGet())
            navWorld->RemoveMap(Pathfinding::NavMapHandle{mapIndex, mapGeneration});
    }

    mapIndex = 0;
    mapGeneration = 0;
    initialized = false;
}

} // namespace

void ReleaseNavigationMeshMap(Components::NavigationMesh& mesh)
{
    ReleaseMap(mesh.NavMapIndex, mesh.NavMapGeneration, mesh.Initialized);
    mesh.NeedsRebuild = true;
}

void ClearNavigationAgentReservations(const Components::NavigationAgent& agent)
{
    if (agent.AgentId == 0)
        return;

    auto* navWorld = NavigationService::TryGet();
    if (!navWorld)
        return;

    Pathfinding::NavMapHandle mapHandle{agent.NavMapIndex, agent.NavMapGeneration};
    if (auto* gridMap = navWorld->GetGridMap(mapHandle))
        gridMap->ClearReservationsForAgent(agent.AgentId);
}

void StampNavigationAgentOccupancy(const Components::NavigationAgent& agent,
                                   Components::NavigationAgentState& state,
                                   float32 posX, float32 posZ, float32 currentTime)
{
    if (agent.AgentId == 0)
        return;
    auto* navWorld = NavigationService::TryGet();
    if (!navWorld)
        return;
    Pathfinding::NavMapHandle mapHandle{agent.NavMapIndex, agent.NavMapGeneration};
    auto* gridMap = navWorld->GetGridMap(mapHandle);
    if (!gridMap)
        return;

    gridMap->ClearReservationsForAgent(agent.AgentId);
    state.HasCellReservations = false;

    uint32 hereX = 0;
    uint32 hereZ = 0;
    if (!gridMap->WorldToCell(posX, posZ, hereX, hereZ))
        return;

    constexpr float32 kReservationLookaheadSeconds = 1.5f;
    const int32 r = static_cast<int32>(gridMap->FootprintRadiusInCells(agent.Radius));
    const float32 expirationTime = currentTime + kReservationLookaheadSeconds;
    for (int32 rdz = -r; rdz <= r; ++rdz)
    {
        for (int32 rdx = -r; rdx <= r; ++rdx)
        {
            const int32 rx = static_cast<int32>(hereX) + rdx;
            const int32 rz = static_cast<int32>(hereZ) + rdz;
            if (rx >= 0 && rz >= 0)
                gridMap->ReserveCell(static_cast<uint32>(rx), static_cast<uint32>(rz),
                                     agent.AgentId, expirationTime, true);
        }
    }
    state.HasCellReservations = true;
}

void ReleaseNavigationAgentReservations(Components::NavigationAgent& agent)
{
    ClearNavigationAgentReservations(agent);
}

void RegisterNavigationWorldHooks(ECS::World& world)
{
    world.RegisterOnRemove<Components::NavigationGrid>(&ReleaseNavigationGridMap);
    world.RegisterOnRemove<Components::NavigationMesh>(&ReleaseNavigationMeshMap);
    world.RegisterOnRemove<Components::NavigationAgent>(&ReleaseNavigationAgentReservations);
}

} // namespace GameEngine::PathfindingECS
