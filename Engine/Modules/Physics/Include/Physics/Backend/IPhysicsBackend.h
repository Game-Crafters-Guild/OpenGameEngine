#pragma once

#include "Physics/PhysicsConstraints.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsQuery.h"
#include "Physics/PhysicsShapes.h"
#include "Physics/PhysicsTypes.h"

#include <utility>
#include <vector>

namespace GameEngine::Physics
{
class IPhysicsBackend
{
public:
    virtual ~IPhysicsBackend() = default;

    virtual bool Initialize(const PhysicsWorldSettings& settings) = 0;
    virtual void Shutdown() = 0;

    virtual void Step(float32 deltaTime, int32 collisionSteps) = 0;
    virtual void OptimizeBroadphase() = 0;

    // Shapes
    virtual ShapeHandle CreateShape(const ShapeDefinition& definition, const Vector3& bakedScale) = 0;
    virtual void DestroyShape(ShapeHandle shape) = 0;
    virtual bool IsShapeValid(ShapeHandle shape) const = 0;

    // In-place heightfield region update. allSamples is the FULL sample grid
    // (gridN x gridN, row-major) — the backend widens the region to block
    // boundaries and needs neighboring samples. Region is [x, x+sizeX) x
    // [z, z+sizeZ) in sample space. Takes the body because after mutating the
    // shape in place the backend must refresh the body's broadphase entry —
    // without it, raised terrain is invisible to collision until a full
    // rebuild. Returns false when the backend cannot update in place
    // (encode-range exceeded, unsupported) — caller falls back to
    // destroy+recreate.
    virtual bool UpdateHeightFieldRegion(BodyHandle body, ShapeHandle shape,
                                         const float32* allSamples, uint32 gridN,
                                         uint32 x, uint32 z,
                                         uint32 sizeX, uint32 sizeZ) = 0;

    // Wake all dynamic bodies whose broadphase AABB intersects the box.
    // Required after ANY heightfield geometry change: statics are not island
    // members, so sleeping bodies never notice the ground moving.
    virtual void ActivateBodiesInAABB(const AABB& worldBox) = 0;

    // Bodies
    virtual BodyHandle CreateBody(const BodySettings& settings) = 0;
    virtual void DestroyBody(BodyHandle body) = 0;
    virtual bool IsBodyValid(BodyHandle body) const = 0;

    virtual Transform GetBodyTransform(BodyHandle body) const = 0;
    virtual void SetBodyTransform(BodyHandle body, const Transform& transform, ActivationMode activation) = 0;

    virtual Vector3 GetLinearVelocity(BodyHandle body) const = 0;
    virtual void SetLinearVelocity(BodyHandle body, const Vector3& velocity) = 0;

    virtual Vector3 GetAngularVelocity(BodyHandle body) const = 0;
    virtual void SetAngularVelocity(BodyHandle body, const Vector3& velocity) = 0;

    // Center of mass in world space (reference point for buoyancy torque).
    virtual Vector3 GetCenterOfMassPosition(BodyHandle body) const = 0;

    // Continuous forces/torques accumulate and apply during the next Step;
    // impulses change velocity immediately. The "AtPosition" variants apply at
    // a world-space point, producing torque (used for buoyancy at probe points).
    virtual void AddForce(BodyHandle body, const Vector3& force) = 0;
    virtual void AddForceAtPosition(BodyHandle body, const Vector3& force, const Vector3& worldPosition) = 0;
    virtual void AddTorque(BodyHandle body, const Vector3& torque) = 0;
    virtual void AddImpulse(BodyHandle body, const Vector3& impulse) = 0;
    virtual void AddImpulseAtPosition(BodyHandle body, const Vector3& impulse, const Vector3& worldPosition) = 0;

    // Characters. Capsule is created by the backend from CharacterSettings;
    // there is no separate shape handle. Motor selects kinematic vs dynamic.
    virtual CharacterHandle CreateCharacter(const CharacterSettings& settings) = 0;
    virtual void DestroyCharacter(CharacterHandle character) = 0;
    virtual bool IsCharacterValid(CharacterHandle character) const = 0;

    virtual Transform GetCharacterTransform(CharacterHandle character) const = 0;
    virtual void SetCharacterPosition(CharacterHandle character, const Vector3& position) = 0;

    virtual Vector3 GetCharacterLinearVelocity(CharacterHandle character) const = 0;
    virtual void SetCharacterLinearVelocity(CharacterHandle character, const Vector3& velocity) = 0;

    virtual bool IsCharacterGrounded(CharacterHandle character) const = 0;
    virtual Vector3 GetCharacterGroundNormal(CharacterHandle character) const = 0;

    // Queries
    virtual bool RayCast(const RayCastQuery& query, RayCastResult& outResult) const = 0;
    virtual bool RayCastAll(const RayCastQuery& query, std::vector<RayCastResult>& outResults, uint32 maxResults) const = 0;
    virtual bool BoxOverlap(const BoxOverlapQuery& query, std::vector<OverlapResult>& outResults, uint32 maxResults) const = 0;

    // Events (backend should buffer events and dispatch when asked)
    using ContactCallback = GameEngine::Function<void(const ContactEvent&)>;
    using TriggerCallback = GameEngine::Function<void(const TriggerEvent&)>;

    virtual void SetContactCallback(ContactCallback callback) = 0;
    virtual void SetTriggerCallback(TriggerCallback callback) = 0;
    virtual void ProcessEvents() = 0;
};

} // namespace GameEngine::Physics

