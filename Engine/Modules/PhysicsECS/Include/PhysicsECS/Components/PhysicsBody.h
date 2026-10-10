#pragma once

#include "PhysicsECS/Components/PhysicsWritebackRecord.h"

#include "Physics/PhysicsTypes.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
// ECS component describing a rigid body in the physics world.
// The PhysicsECS systems create/destroy the backend body and keep transforms in sync.
struct PhysicsBody
{
    // Authoring / desired state
    Physics::MotionType motionType = Physics::MotionType::Dynamic;

    float32 mass = 1.0f;
    float32 linearDamping = 0.05f;
    float32 angularDamping = 0.05f;
    float32 gravityScale = 1.0f;
    float32 centerOfMassOffsetX = 0.0f;
    float32 centerOfMassOffsetY = 0.0f;
    float32 centerOfMassOffsetZ = 0.0f;

    // Contact material (used as default for colliders that don't override).
    // Note: The backend applies these at the *body* level for now.
    Physics::PhysicsMaterial material{};

    // Initial velocities (applied when the backend body is created).
    // Useful for demos / gameplay-driven spawning.
    float32 linearVelocityX = 0.0f;
    float32 linearVelocityY = 0.0f;
    float32 linearVelocityZ = 0.0f;
    float32 angularVelocityX = 0.0f;
    float32 angularVelocityY = 0.0f;
    float32 angularVelocityZ = 0.0f;

    bool allowSleep = true;
    bool startAwake = true;
    bool continuousCollision = false;

    // Runtime handles (owned by the physics world)
    Physics::ShapeHandle shape{};
    Physics::BodyHandle body{};
    bool initialized = false;

    // Render interpolation: last physics transform snapshot before the most recent step.
    // This allows PhysicsWritebackSystem to smooth rendering between fixed timesteps.
    Physics::Transform prevPhysicsTransform{};
    bool hasPrevPhysicsTransform = false;

    // What PhysicsWritebackSystem last wrote, so a body whose pose has not changed is skipped.
    PhysicsWritebackRecord WritebackRecord{};

    // If this body was built as a compound, these are the child shapes it owns.
    // (We need to destroy them when rebuilding to avoid leaking shapes.)
    static constexpr uint16 kMaxChildShapes = 16;
    uint16 childShapeCount = 0;
    Physics::ShapeHandle childShapes[kMaxChildShapes]{};
};

// Reduce a PhysicsBody to "owns nothing yet" — the state PhysicsInitSystem
// provisions from. Authoring fields are untouched; only the physics world's own
// bookkeeping is cleared.
//
// Three callers have to agree on this exact set, because each produces an entity
// that names a body it does not own: a scene load, an editor copy, and the undo
// command that frees a deleted entity's body. Clearing a subset is not a smaller
// version of this — PhysicsInitSystem reads `initialized` only to decide whether
// to SKIP, and then destroys whatever the remaining handles name, so a body or
// shape left behind is destroyed on the next tick by whoever inherited it.
//
// childShapes[] needs no scrub: childShapeCount bounds every read of it.
// prevPhysicsTransform likewise — hasPrevPhysicsTransform gates every read — and
// the WritebackRecord's pose and bytes, which its Valid flag gates.
inline void ClearPhysicsBodyRuntimeState(PhysicsBody& body)
{
    body.shape = {};
    body.body = {};
    body.initialized = false;
    body.hasPrevPhysicsTransform = false;
    body.WritebackRecord.Valid = false;
    body.childShapeCount = 0;
}

} // namespace GameEngine::Components
