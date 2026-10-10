#pragma once

#include "Physics/PhysicsConstraints.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsQuery.h"
#include "Physics/PhysicsShapes.h"
#include "Physics/PhysicsTypes.h"

#include <memory>
#include <span>
#include <vector>

namespace GameEngine::Physics
{
class IPhysicsBackend;

// Backend-agnostic physics world façade. Implementation is delegated to a backend
// (e.g. Jolt) selected at build time.
class PhysicsWorld
{
public:
    using ListenerId = uint64;

    explicit PhysicsWorld(const PhysicsWorldSettings& settings = {});
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;

    // ---------------------------------------------------------------------
    // Simulation
    // ---------------------------------------------------------------------
    void Step(float32 deltaTime, int32 collisionSteps = 1);
    void OptimizeBroadphase();

    // ---------------------------------------------------------------------
    // Shapes
    // ---------------------------------------------------------------------
    ShapeHandle CreateShape(const ShapeDefinition& definition, const Vector3& bakedScale = Vector3(1, 1, 1));
    void DestroyShape(ShapeHandle shape);
    bool IsShapeValid(ShapeHandle shape) const;

    // In-place heightfield region update; see IPhysicsBackend for contract.
    // Returns false when the backend cannot update in place — caller falls
    // back to the destroy+recreate path.
    bool UpdateHeightFieldRegion(BodyHandle body, ShapeHandle shape,
                                 const float32* allSamples, uint32 gridN,
                                 uint32 x, uint32 z,
                                 uint32 sizeX, uint32 sizeZ);

    // Wake all dynamic bodies whose broadphase AABB intersects the box.
    // Required after any heightfield geometry change so sleeping bodies
    // notice the ground moving.
    void ActivateBodiesInAABB(const AABB& worldBox);

    // ---------------------------------------------------------------------
    // Bodies
    // ---------------------------------------------------------------------
    BodyHandle CreateBody(const BodySettings& settings);
    void DestroyBody(BodyHandle body);
    bool IsBodyValid(BodyHandle body) const;

    Transform GetBodyTransform(BodyHandle body) const;
    void SetBodyTransform(BodyHandle body, const Transform& transform, ActivationMode activation = ActivationMode::Activate);

    Vector3 GetLinearVelocity(BodyHandle body) const;
    void SetLinearVelocity(BodyHandle body, const Vector3& velocity);

    Vector3 GetAngularVelocity(BodyHandle body) const;
    void SetAngularVelocity(BodyHandle body, const Vector3& velocity);

    // World-space center of mass (reference point for buoyancy torque).
    Vector3 GetCenterOfMassPosition(BodyHandle body) const;

    // Forces/torques accumulate and apply during the next Step(); impulses
    // change velocity immediately. The "AtPosition" variants apply at a
    // world-space point (producing torque) — used for buoyancy at probe points.
    void AddForce(BodyHandle body, const Vector3& force);
    void AddForceAtPosition(BodyHandle body, const Vector3& force, const Vector3& worldPosition);
    void AddTorque(BodyHandle body, const Vector3& torque);
    void AddImpulse(BodyHandle body, const Vector3& impulse);
    void AddImpulseAtPosition(BodyHandle body, const Vector3& impulse, const Vector3& worldPosition);

    // ---------------------------------------------------------------------
    // Characters
    // ---------------------------------------------------------------------
    CharacterHandle CreateCharacter(const CharacterSettings& settings);
    void DestroyCharacter(CharacterHandle character);
    bool IsCharacterValid(CharacterHandle character) const;

    Transform GetCharacterTransform(CharacterHandle character) const;
    void SetCharacterPosition(CharacterHandle character, const Vector3& position);

    Vector3 GetCharacterLinearVelocity(CharacterHandle character) const;
    void SetCharacterLinearVelocity(CharacterHandle character, const Vector3& velocity);

    bool IsCharacterGrounded(CharacterHandle character) const;
    Vector3 GetCharacterGroundNormal(CharacterHandle character) const;

    // ---------------------------------------------------------------------
    // Queries
    // ---------------------------------------------------------------------
    bool RayCast(const RayCastQuery& query, RayCastResult& outResult) const;
    bool RayCastAll(const RayCastQuery& query, std::vector<RayCastResult>& outResults, uint32 maxResults = 64) const;
    bool BoxOverlap(const BoxOverlapQuery& query, std::vector<OverlapResult>& outResults, uint32 maxResults = 64) const;

    // ---------------------------------------------------------------------
    // Events
    // ---------------------------------------------------------------------
    using ContactCallback = GameEngine::Function<void(const ContactEvent&)>;
    using TriggerCallback = GameEngine::Function<void(const TriggerEvent&)>;

    // Add/remove listeners. Multiple listeners can be registered; they are invoked in registration order.
    // If no listeners exist, the backend will not generate/dispatch events.
    ListenerId AddContactListener(ContactCallback callback);
    ListenerId AddTriggerListener(TriggerCallback callback);
    void RemoveContactListener(ListenerId id);
    void RemoveTriggerListener(ListenerId id);
    void ClearContactListeners();
    void ClearTriggerListeners();

    // Convenience (back-compat): replaces all listeners with a single callback.
    void SetContactCallback(ContactCallback callback);
    void SetTriggerCallback(TriggerCallback callback);
    void ProcessEvents();

    // Diagnostic counters for the Monitors panel.
    uint32 GetBodyCount() const { return m_BodyCount; }
    uint32 GetContactPairCount() const { return m_LastContactPairCount; }

private:
    void UpdateBackendEventCallbacks();

    std::unique_ptr<IPhysicsBackend> m_Backend;
    std::vector<std::pair<ListenerId, ContactCallback>> m_ContactListeners;
    std::vector<std::pair<ListenerId, TriggerCallback>> m_TriggerListeners;
    ListenerId m_NextListenerId = 1;

    uint32 m_BodyCount = 0;
    uint32 m_LastContactPairCount = 0;
    uint32 m_AccumContactPairs = 0;
};

} // namespace GameEngine::Physics

