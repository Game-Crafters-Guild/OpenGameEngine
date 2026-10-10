#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Runtime tag placed by TerrainPhysicsSystem on each of a spherical (planet) terrain's
// six per-face heightfield collider entities (planet-collider slice). A planet is one
// Terrain entity but needs one Jolt heightfield body per cube face, so each face gets its
// own collider entity; this component records which planet + face it serves and the
// planet-face physics handle to release when the terrain is destroyed, switches back to
// Planar, or Play exits. Not authored, not serialized — a pure runtime provisioning link,
// mirroring TerrainTileCollider.
struct TerrainPlanetFaceCollider
{
    // The owning terrain entity's handle (index + version), stored as raw uint32s so the
    // component stays standard-layout for ECS storage. Teardown re-forms the handle to ask
    // whether that entity is still a live spherical terrain.
    uint32 TerrainEntityIndex = 0u;
    uint32 TerrainEntityVersion = 0u;

    // Cube face index (0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z).
    uint32 Face = 0u;

    // HeightFieldColliderShape.dataHandle value (already OR'd with
    // TerrainService::kPlanetFacePhysicsHandleBit); passed to
    // TerrainService::ReleasePlanetFacePhysicsHandle on teardown.
    uint32 PhysicsHandleIndex = 0u;

    // HeightFieldColliderShape.dataGeneration value. Paired with PhysicsHandleIndex so the
    // teardown release is generation-checked — a stale index alone would silently free
    // whichever slot currently owns it.
    uint32 PhysicsHandleGeneration = 0u;
};

static_assert(std::is_trivially_copyable_v<TerrainPlanetFaceCollider>,
              "TerrainPlanetFaceCollider must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<TerrainPlanetFaceCollider>,
              "TerrainPlanetFaceCollider must be standard layout for ECS storage");

} // namespace GameEngine::Components
