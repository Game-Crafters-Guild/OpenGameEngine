#pragma once

#include "ECS/RequiredComponents.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "Types/Types.h"

namespace GameEngine::Components
{

// ECS component for heightfield (terrain) collision.
// Add this to any entity that has terrain height data to give it a physics
// heightfield collider — just like adding SphereColliderShape gives a sphere.
//
// The actual height samples are resolved at physics init time via a registered
// HeightFieldDataProvider (typically backed by TerrainService).
struct HeightFieldColliderShape
{
    // Handle into the height data provider (e.g. TerrainService slot index).
    uint32 dataHandle = 0;
    uint32 dataGeneration = 0;

    // Terrain dimensions needed to compute Jolt offset and scale.
    float32 sizeX = 0.0f;
    float32 sizeZ = 0.0f;
    float32 heightScale = 0.0f;

    // Tracks which data version the current physics shape was built from.
    // Also the cursor passed to the HeightFieldDataProvider so it can report
    // the dirty region accumulated since the shape last matched the data.
    uint64 lastBuiltVersion = 0;

    // Tier-2 rebuild throttle state (PhysicsInitSystem): the last provider
    // version observed and how long it has been stable. A full Jolt rebuild
    // only triggers once the version stops changing for the quiescence
    // window, so an edit-drag storm pays one cook, not one per frame.
    uint64 lastSeenVersion = 0;
    float32 rebuildWaitSeconds = 0.0f;

    // Envelope of raw sample heights the built shape has covered (grown by
    // in-place region updates). Used to size the wake AABB conservatively:
    // when terrain is lowered, sleeping bodies rest at the OLD surface
    // height, so the wake box must span old and new heights.
    float32 builtMinHeight = 0.0f;
    float32 builtMaxHeight = 0.0f;

    // Sample count the current shape was built with. A provider grid resize
    // makes region sample strides incompatible with the shape's baked X/Z
    // scale — tier-1 must be refused and the shape rebuilt.
    uint32 builtSampleCount = 0;

    // Version whose in-place attempt failed (encode range exceeded). Skips
    // re-converting the region every frame of the throttle window; retried
    // only when the version moves again.
    uint64 inPlaceFailedVersion = 0;
};

static_assert(std::is_trivially_copyable_v<HeightFieldColliderShape>,
              "HeightFieldColliderShape must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<HeightFieldColliderShape>,
              "HeightFieldColliderShape must be standard layout for ECS storage");

} // namespace GameEngine::Components

namespace GameEngine::ECS::Detail
{
template<>
struct RequiredComponents<GameEngine::Components::HeightFieldColliderShape>
{
    using type = TypeList<GameEngine::Components::PhysicsCollider>;
};
} // namespace GameEngine::ECS::Detail
