#pragma once

namespace GameEngine::Components
{
struct Terrain;
struct TerrainTileCollider;
struct TerrainPlanetFaceCollider;
} // namespace GameEngine::Components

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::TerrainECS
{

/// Releases every external resource a Terrain component owns: its TerrainService
/// slot (single or tiled), the render feature's GPU texture set, and the bindless
/// slots those textures hold. Zeroes the runtime handles so a second call is a
/// no-op.
///
/// Runs from `~World` at process shutdown as well as from scene close, so every
/// service is resolved defensively — a service that is already gone leaves the
/// handles alone rather than being dereferenced.
void ReleaseTerrainOwnedResources(Components::Terrain& terrain);

/// Returns a terrain tile collider's heightfield physics handle to the service.
/// The release is generation-checked, so a stale index frees nothing.
///
/// The collider components live here rather than in PhysicsECS because that is
/// the module that owns them — and because TerrainECS already depends on
/// PhysicsECS, so the reverse would be a cycle. Each hook is self-sufficient
/// from its own component: removal hooks hold the world's non-recursive mutex
/// exclusively, so a sibling lookup would self-deadlock. Both components carry
/// their own handle pair, so none is needed. An entity carrying a collider AND
/// a PhysicsBody fires both hooks, which is how the pair composes.
void ReleaseTerrainTileCollider(Components::TerrainTileCollider& collider);

/// Planet-face counterpart of ReleaseTerrainTileCollider.
void ReleaseTerrainPlanetFaceCollider(Components::TerrainPlanetFaceCollider& collider);

/// Registers the releases above as the world's OnRemove hooks for the terrain
/// component types, which is what makes World::Clear, World::DestroyEntity and
/// a component remove all release through one body per type.
void RegisterTerrainWorldHooks(ECS::World& world);

} // namespace GameEngine::TerrainECS
