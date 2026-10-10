#pragma once

#include "Pathfinding/PathfindingTypes.h"

namespace GameEngine::ECS { class World; }
namespace GameEngine::Components { struct NavigationGrid; }
namespace GameEngine::Pathfinding { class GridMap; struct GridSettings; }

namespace GameEngine::PathfindingECS
{
// The NavigationWorld map a NavigationGrid names is a runtime resource owned by
// the component that created it, stamped with the creating world. Every reader
// and publisher goes through these functions; nothing else writes the binding.
// Main thread only, and never while path jobs are in flight: NavigationWorld
// maps are not synchronized.

// Creates a map in the current service and binds it to `world`. The new map
// always starts pending rebake; a manual publisher clears NeedsRebake once it
// has filled the map. Returns an invalid handle when the service is absent,
// path jobs are active, or `grid` already resolves to a live map in `world`.
// A stale binding is overwritten without releasing whatever it named.
Pathfinding::NavMapHandle CreateNavigationGridMap(const ECS::World& world,
    Components::NavigationGrid& grid, const Pathfinding::GridSettings& settings);

// The live map `grid` owns in `world`, or nullptr when the binding is absent,
// stamped for another world, or names a map the service no longer holds. Says
// nothing about bake or asset readiness: callers still check NeedsRebake.
Pathfinding::GridMap* ResolveNavigationGridMap(const ECS::World& world,
    const Components::NavigationGrid& grid);

// Releases the map when `grid` resolves in `world`, otherwise only clears the
// binding. Either way the grid is left pending rebake. Safe to repeat.
void ResetNavigationGridMap(const ECS::World& world, Components::NavigationGrid& grid);

// Component teardown (RegisterOnRemove, no world context): releases a binding
// this API created and clears it. A binding authored by hand carries no world
// stamp and is left in the service. A copy of a bound component in another
// world must be reset or forgotten before it is destroyed.
void ReleaseNavigationGridMap(Components::NavigationGrid& grid);

// Clears the binding without touching the map it names. For a copy of a bound
// component made inside the same world (entity duplication), which would
// otherwise share, and on destruction release, the original's map.
void ForgetNavigationGridMap(Components::NavigationGrid& grid);
} // namespace GameEngine::PathfindingECS
