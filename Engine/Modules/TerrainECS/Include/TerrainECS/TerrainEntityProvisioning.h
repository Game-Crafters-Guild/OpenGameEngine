#pragma once

#include "ECS/ECS.h"

#include <string>

namespace GameEngine
{
namespace ECS
{
class World;
} // namespace ECS

namespace Components
{
struct Terrain;
struct TerrainPlanetRelief;
} // namespace Components

namespace TerrainECS
{

/// What ProvisionTerrainEntity did.
struct TerrainProvisioningResult
{
    /// Empty when the terrain was provisioned; otherwise why it was refused and how to fix it, and the entity is untouched.
    std::string Error;
    /// The scene's default "Terrain Surface Rules" volume, when this call spawned it; invalid otherwise.
    ECS::EntityHandle SurfaceRulesVolume;
};

/// Makes `entity` a terrain carrying `config`: the one implementation every creation path shares
/// (the editor's create menu and create_terrain IPC, the web library's scene.terrain.create).
///
/// - Adds Terrain (or overwrites it with `config`) and a default TerrainGrass when absent.
/// - A spherical terrain gets `relief` as its TerrainPlanetRelief; a planar one loses any it had.
/// - A planar terrain that fits one tile gets its noise heightfield now and a static heightfield
///   collider, so it renders and collides at once. Tiled terrains and planets are provisioned by
///   TerrainExtractionSystem on the next tick instead: an eager single heightfield there would be
///   destroyed at the tiling flip and its collider stranded.
/// - A planar terrain in a scene with no global surface rules gets the default rules volume
///   (MakeDefaultTerrainSurfaceRules): an ordinary entity, deletable, never re-created.
///
/// Refuses a planar terrain wider or deeper than MaxTerrainExtentMetres at its sample density.
/// The caller adds the entity's Name and Transform; the editor orders the volume in its hierarchy.
TerrainProvisioningResult ProvisionTerrainEntity(ECS::World& world, ECS::EntityHandle entity,
                                                 const Components::Terrain& config,
                                                 const Components::TerrainPlanetRelief& relief);

} // namespace TerrainECS
} // namespace GameEngine
