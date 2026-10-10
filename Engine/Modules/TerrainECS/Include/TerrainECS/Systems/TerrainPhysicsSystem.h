#pragma once

#include "ECS/Systems.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::Components
{
struct HeightFieldColliderShape;
struct Terrain;
} // namespace GameEngine::Components

namespace GameEngine::TerrainECS
{

// A planar terrain's collider describes it when it reads the terrain's current
// data handle and was sized from the terrain's current extent and height scale.
bool ColliderMatchesTerrain(const Components::HeightFieldColliderShape& shape, const Components::Terrain& terrain);

// Provisions HeightFieldColliderShape + PhysicsBody + PhysicsCollider on terrain
// entities that have height data ready, and keeps a planar terrain's shape on the
// terrain's current data handle, extent and height scale. The actual Jolt body
// creation is handled by PhysicsInitSystem via its heightfield gather block.
class TerrainPhysicsSystem : public ECS::ISystem
{
public:
    const char* GetName() const override { return "TerrainPhysicsSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // Provision / refresh a spherical terrain's six per-face heightfield colliders
    // (planet-collider slice). Split out so the tiled/single provisioning stays readable.
    void UpdatePlanetColliders(ECS::World& world);

    // Removes the collider of a terrain that lost its planar data with no
    // planar replacement: the Terrain component was removed, or the terrain
    // became tiled or spherical. Its data is gone, so the collider could never
    // be rebuilt and would leave stale collision behind.
    void RemoveOrphanedPlanarColliders(ECS::World& world);

    // Entities carrying a planar terrain collider, for RemoveOrphanedPlanarColliders.
    // Scoped to one world lifetime; cleared when the world or its reset
    // generation changes.
    std::vector<ECS::EntityHandle> m_PlanarColliders;
    uint64 m_TrackedWorld = 0;
    uint64 m_TrackedReset = 0;

    // Latched once a duplicate tiled-handle claim is seen, so the misconfig
    // warning is logged once per system rather than every frame the duplicate
    // persists.
    bool m_WarnedDuplicateTiledClaim = false;

    // Same, for a second enabled spherical terrain (only the first gets face colliders).
    bool m_WarnedDuplicatePlanetClaim = false;

    // The sphere sculpt mirror version the planet face colliders have consumed. A refresh
    // regenerates the dirty face's region only when the pushed mirror version advances past
    // this (idle planets do zero work — the quiescence invariant).
    uint64 m_LastConsumedSculptVersion = 0;

    // The planet's shape inputs (radius + base relief) the six face patches were last cooked
    // against. A shape edit moves every texel of every face, so it has no dirty rect to scope
    // it and must re-cook all six in full — samples AND the shapes' declared grid extent, which
    // the radius parametrizes together. A separate baseline from the sculpt version above,
    // which stays flat across a relief edit because relief feeds neither of its terms.
    // Seeded without re-cooking on the first planet seen (freshly provisioned faces are already
    // generated at the current shape).
    uint64 m_LastPlanetShapeHash = 0;
    bool m_HasPlanetShapeBaseline = false;
};

} // namespace GameEngine::TerrainECS
