#include "PhysicsPoseWriteback.h"

#include "PhysicsECS/Components/PhysicsWritebackRecord.h"

#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "ECS/Entity.h"
#include "Physics/PhysicsTypes.h"

#include <cstring>

namespace GameEngine::PhysicsECS
{
namespace
{
// Bit equality: the skip must hold exactly when composing would see the same inputs.
bool SamePose(const Physics::Transform& a, const Physics::Transform& b)
{
    return std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 &&
           std::memcmp(&a.rotation.GetGLM(), &b.rotation.GetGLM(), sizeof(a.rotation.GetGLM())) == 0;
}

bool HoldsWrittenMatrix(const float32 (&matrix)[16], const Components::PhysicsWritebackRecord& record)
{
    return std::memcmp(matrix, record.WrittenMatrix, sizeof(record.WrittenMatrix)) == 0;
}

bool IsUnchanged(const Physics::Transform& current,
                 const Physics::Transform* interpolateFrom,
                 const Components::Transform& transform,
                 const Components::WorldTransform& worldTransform,
                 const Components::PhysicsWritebackRecord& record)
{
    return record.Valid && SamePose(current, record.AppliedPose) &&
           (interpolateFrom == nullptr || SamePose(*interpolateFrom, record.AppliedPose)) &&
           HoldsWrittenMatrix(transform.matrix, record) && HoldsWrittenMatrix(worldTransform.matrix, record);
}

Physics::Transform BlendPose(const Physics::Transform& from, const Physics::Transform& to, float32 alpha)
{
    Physics::Transform pose = to;
    pose.position.x = from.position.x + (to.position.x - from.position.x) * alpha;
    pose.position.y = from.position.y + (to.position.y - from.position.y) * alpha;
    pose.position.z = from.position.z + (to.position.z - from.position.z) * alpha;
    pose.rotation = Physics::Quaternion::Slerp(from.rotation, to.rotation, alpha);
    return pose;
}
} // namespace

bool WritePhysicsPose(ECS::World& world,
                      ECS::EntityHandle entity,
                      const Physics::Transform& current,
                      const Physics::Transform* interpolateFrom,
                      float32 alpha,
                      Components::Transform& transform,
                      Components::WorldTransform& worldTransform,
                      Components::PhysicsWritebackRecord& record)
{
    if (IsUnchanged(current, interpolateFrom, transform, worldTransform, record))
        return false;

    const Physics::Transform pose = interpolateFrom ? BlendPose(*interpolateFrom, current, alpha) : current;

    const auto scale = transform.GetScale();
    const Mathematics::Vector3 position{pose.position.x, pose.position.y, pose.position.z};
    const auto& rotationGlm = pose.rotation.GetGLM();
    const Mathematics::Quaternion rotation{static_cast<float32>(rotationGlm.w),
                                           static_cast<float32>(rotationGlm.x),
                                           static_cast<float32>(rotationGlm.y),
                                           static_cast<float32>(rotationGlm.z)};
    const Components::Transform composed = Components::Transform::FromTRS(position, rotation, scale);

    // Each target is gated on its own bytes, so a component that already holds the pose keeps its
    // Version and emits no dirty-feed entry.
    if (std::memcmp(transform.matrix, composed.matrix, sizeof(transform.matrix)) != 0)
        transform = composed;

    if (std::memcmp(worldTransform.matrix, composed.matrix, sizeof(worldTransform.matrix)) != 0)
    {
        std::memcpy(worldTransform.matrix, composed.matrix, sizeof(worldTransform.matrix));
        // A direct WorldTransform write: bump Version and emit the dirty-feed entry so this frame's
        // consumers (Scene TLAS, motion vectors, GPUScene upload) see the move; the hierarchy would
        // only re-derive it from the local Transform on the next frame.
        Components::BumpWorldTransform(world, entity, worldTransform);
    }

    record.Valid = interpolateFrom == nullptr || SamePose(*interpolateFrom, current);
    record.AppliedPose = current;
    std::memcpy(record.WrittenMatrix, composed.matrix, sizeof(record.WrittenMatrix));
    return true;
}

} // namespace GameEngine::PhysicsECS
