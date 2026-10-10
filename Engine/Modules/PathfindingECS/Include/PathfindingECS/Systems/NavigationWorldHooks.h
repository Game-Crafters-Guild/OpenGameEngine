#pragma once

#include "PathfindingECS/NavigationGridRuntime.h"

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

#include "Types/Types.h"

namespace GameEngine::Components
{
struct NavigationAgent;
struct NavigationAgentState;
struct NavigationGrid;
struct NavigationMesh;
} // namespace GameEngine::Components

namespace GameEngine::PathfindingECS
{

// Hands a navmesh's NavigationWorld map back when its component goes away.
void ReleaseNavigationMeshMap(Components::NavigationMesh& mesh);

// Drops every GridMap cell reserved by this agent. No-op if the agent has
// no id, the nav service is gone, or the map handle is not a grid.
void ClearNavigationAgentReservations(const Components::NavigationAgent& agent);

// Drop the corridor and restamp occupancy on the cell the agent stands in.
void StampNavigationAgentOccupancy(const Components::NavigationAgent& agent,
                                   Components::NavigationAgentState& state,
                                   float32 posX, float32 posZ, float32 currentTime);

// Registers the component-teardown hooks that free NavigationWorld maps
// and drop an agent's cell reservations when the agent is removed.
// Covers World::Clear (scene close), World::DestroyEntity and a plain component
// remove alike — the paths a scene open actually takes.
void RegisterNavigationWorldHooks(ECS::World& world);

} // namespace GameEngine::PathfindingECS
