#include "PathfindingECS/NavigationGridRuntime.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/NavigationService.h"
#include "ECS/Entity.h"
#include "Pathfinding/NavigationWorld.h"

namespace GameEngine::PathfindingECS
{
Pathfinding::GridMap* ResolveNavigationGridMap(const ECS::World& world,
    const Components::NavigationGrid& grid)
{
    if (!grid.Initialized || grid.OwnerWorldId != world.GetWorldId())
        return nullptr;
    auto* navigation = NavigationService::TryGet();
    return navigation ? navigation->GetGridMap({grid.NavMapIndex, grid.NavMapGeneration}) : nullptr;
}

Pathfinding::NavMapHandle CreateNavigationGridMap(const ECS::World& world,
    Components::NavigationGrid& grid, const Pathfinding::GridSettings& settings)
{
    auto* navigation = NavigationService::TryGet();
    if (!navigation || navigation->HasActivePathJobs() || ResolveNavigationGridMap(world, grid))
        return {};
    const auto handle = navigation->AddGridMap(settings);
    if (!handle.IsValid())
        return {};
    grid.NavMapIndex = handle.Index;
    grid.NavMapGeneration = handle.Generation;
    grid.OwnerWorldId = world.GetWorldId();
    grid.Initialized = true;
    grid.NeedsRebake = true;
    return handle;
}

void ForgetNavigationGridMap(Components::NavigationGrid& grid)
{
    grid.NavMapIndex = 0;
    grid.NavMapGeneration = 0;
    grid.Initialized = false;
    grid.NeedsRebake = true;
    grid.OwnerWorldId = 0;
}

void ReleaseNavigationGridMap(Components::NavigationGrid& grid)
{
    if (grid.Initialized && grid.OwnerWorldId != 0)
    {
        if (auto* navigation = NavigationService::TryGet())
            navigation->RemoveMap({grid.NavMapIndex, grid.NavMapGeneration});
    }
    ForgetNavigationGridMap(grid);
}

void ResetNavigationGridMap(const ECS::World& world, Components::NavigationGrid& grid)
{
    if (ResolveNavigationGridMap(world, grid))
        ReleaseNavigationGridMap(grid);
    else
        ForgetNavigationGridMap(grid);
}
} // namespace GameEngine::PathfindingECS
