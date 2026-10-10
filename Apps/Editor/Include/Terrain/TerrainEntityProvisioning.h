#pragma once

#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
struct EntityHandle;
} // namespace ECS

namespace Components
{
struct Terrain;
struct TerrainPlanetRelief;
} // namespace Components

namespace Editor
{

// The engine's terrain provisioning (TerrainECS::ProvisionTerrainEntity), shared by the create
// menu and the create_terrain / create_entity IPC, with the default surface-rules volume it may
// spawn placed at the bottom of the hierarchy. Returns the refusal, empty when the terrain was
// provisioned.
std::string ProvisionTerrainEntity(ECS::World& world, ECS::EntityHandle entity,
                                   const Components::Terrain& config,
                                   const Components::TerrainPlanetRelief& relief);

} // namespace Editor
} // namespace GameEngine
