#pragma once

#include "PhysicsECS/Components/PhysicsWritebackRecord.h"

#include "Physics/PhysicsTypes.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
// Capsule motor for a character. The capsule lives on this component — do not
// pair it with PhysicsBody (the systems refuse that combination).
//
// Motor selects the backend object: Kinematic depenetrates and slides;
// Dynamic is a rigid body with mass that other bodies can push.
struct CharacterController
{
    Physics::CharacterMotor motor = Physics::CharacterMotor::Kinematic;

    // Capsule: total height including hemispherical caps. Must be > 2 * radius.
    float32 radius = 0.4f;
    float32 height = 1.8f;

    float32 maxSlopeAngleDegrees = 45.0f;
    float32 maxStepHeight = 0.4f;
    float32 skinWidth = 0.02f;

    // Used by Dynamic motor. Kinematic still stores them for a mode switch.
    float32 mass = 80.0f;
    float32 gravityScale = 1.0f;

    // Gameplay wish: XZ replaced every tick; Y > 0 is consumed as a jump.
    // Used as Speed for Graph params, and as motor velocity unless animation
    // stamped a displacement this tick.
    float32 desiredVelocityX = 0.0f;
    float32 desiredVelocityY = 0.0f;
    float32 desiredVelocityZ = 0.0f;

    // Runtime (physics world). Not serialized.
    Physics::CharacterHandle character{};
    bool initialized = false;
    Physics::CharacterMotor createdMotor = Physics::CharacterMotor::Kinematic;
    bool grounded = false;
    float32 groundNormalX = 0.0f;
    float32 groundNormalY = 1.0f;
    float32 groundNormalZ = 0.0f;
    bool refusedBecausePhysicsBody = false;

    // Animation root-motion displacement this tick, already rotated by the
    // entity's Transform. Applied as XZ velocity = delta / dt, then cleared.
    float32 animationDisplacementX = 0.0f;
    float32 animationDisplacementY = 0.0f;
    float32 animationDisplacementZ = 0.0f;
    bool hasAnimationDisplacement = false;

    // What CharacterControllerWritebackSystem last wrote, so a character whose pose has not changed
    // is skipped. Runtime, not serialized.
    PhysicsWritebackRecord WritebackRecord{};
};

inline void ClearCharacterControllerRuntimeState(CharacterController& c)
{
    c.character = {};
    c.initialized = false;
    c.grounded = false;
    c.groundNormalX = 0.0f;
    c.groundNormalY = 1.0f;
    c.groundNormalZ = 0.0f;
    c.refusedBecausePhysicsBody = false;
    c.animationDisplacementX = 0.0f;
    c.animationDisplacementY = 0.0f;
    c.animationDisplacementZ = 0.0f;
    c.hasAnimationDisplacement = false;
    c.WritebackRecord.Valid = false;
}

} // namespace GameEngine::Components
