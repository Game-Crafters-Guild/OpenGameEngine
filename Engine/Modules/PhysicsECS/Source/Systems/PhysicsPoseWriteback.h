#pragma once

#include "Types/Types.h"

namespace GameEngine::ECS
{
class World;
struct EntityHandle;
} // namespace GameEngine::ECS

namespace GameEngine::Physics
{
struct Transform;
} // namespace GameEngine::Physics

namespace GameEngine::Components
{
struct Transform;
struct WorldTransform;
struct PhysicsWritebackRecord;
} // namespace GameEngine::Components

namespace GameEngine::PhysicsECS
{
/// Writes a backend pose to an entity's Transform and WorldTransform, keeping the Transform's scale
/// (scale is baked into the shapes). The one writeback rule for rigid bodies and characters.
///
/// The decision is made on the inputs: the entity is skipped, before any scale read or matrix compose,
/// when the record is valid, `current` equals the recorded pose, `interpolateFrom` (when given) equals
/// it too, and both Transform and WorldTransform still hold the recorded bytes. Then every alpha
/// composes the same pose, and an edited Transform or WorldTransform fails the byte compare and snaps
/// back to the body pose.
///
/// Otherwise the pose (blended from `interpolateFrom` by `alpha` when given) is composed, each
/// component whose bytes differ is written (WorldTransform with its Version bump and dirty-feed entry),
/// and the record is updated. A blend between two different poses moves with alpha, so it leaves the
/// record invalid and the next frame composes again.
///
/// @param interpolateFrom The backend pose before the last step to blend from, or null for no blend.
/// @param alpha Blend weight of `current` in [0, 1]; read only with `interpolateFrom`.
/// @return true when the pose was composed, false when the entity was skipped as unchanged.
bool WritePhysicsPose(ECS::World& world,
                      ECS::EntityHandle entity,
                      const Physics::Transform& current,
                      const Physics::Transform* interpolateFrom,
                      float32 alpha,
                      Components::Transform& transform,
                      Components::WorldTransform& worldTransform,
                      Components::PhysicsWritebackRecord& record);

} // namespace GameEngine::PhysicsECS
