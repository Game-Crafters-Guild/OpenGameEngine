#pragma once

#include "Physics/PhysicsTypes.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
/// What the physics writeback last put on an entity, so it can skip an entity whose inputs have not
/// changed instead of composing a matrix only to find the bytes equal. Runtime state: never serialized.
///
/// Valid only when the last composition did not depend on the interpolation alpha (no interpolation,
/// or the previous and current backend poses were bit-identical); see WritePhysicsPose.
struct PhysicsWritebackRecord
{
    /// The backend pose (current, not interpolated) the writeback composed from.
    Physics::Transform AppliedPose{};
    /// The matrix bytes the writeback wrote to both Transform and WorldTransform.
    float32 WrittenMatrix[16]{};
    bool Valid = false;
};

} // namespace GameEngine::Components
