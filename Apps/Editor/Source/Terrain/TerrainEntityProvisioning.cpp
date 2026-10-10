#include "Terrain/TerrainEntityProvisioning.h"

#include "Components/Hierarchy.h"
#include "Editor/Hierarchy/HierarchyOrdering.h"
#include "TerrainECS/TerrainEntityProvisioning.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <utility>

namespace GameEngine::Editor
{

std::string ProvisionTerrainEntity(ECS::World& world, ECS::EntityHandle entity,
                                   const Components::Terrain& config,
                                   const Components::TerrainPlanetRelief& relief)
{
    TerrainECS::TerrainProvisioningResult provisioned = TerrainECS::ProvisionTerrainEntity(world, entity, config, relief);
    // Last, so the volume is complete before it takes a position in the hierarchy: below
    // everything already in the scene rather than wherever an unset order happens to sort.
    if (provisioned.SurfaceRulesVolume.IsValid())
        world.AddComponentImmediate(provisioned.SurfaceRulesVolume, NextHierarchyOrderAtBottom(&world));
    return std::move(provisioned.Error);
}

} // namespace GameEngine::Editor
