#pragma once

#include "Physics/Backend/IPhysicsBackend.h"

namespace GameEngine::Physics
{
// Jolt-backed implementation of IPhysicsBackend.
class JoltPhysicsBackend final : public IPhysicsBackend
{
public:
    JoltPhysicsBackend();
    ~JoltPhysicsBackend() override;

    bool Initialize(const PhysicsWorldSettings& settings) override;
    void Shutdown() override;

    void Step(float32 deltaTime, int32 collisionSteps) override;
    void OptimizeBroadphase() override;

    ShapeHandle CreateShape(const ShapeDefinition& definition, const Vector3& bakedScale) override;
    void DestroyShape(ShapeHandle shape) override;
    bool IsShapeValid(ShapeHandle shape) const override;

    bool UpdateHeightFieldRegion(BodyHandle body, ShapeHandle shape,
                                 const float32* allSamples, uint32 gridN,
                                 uint32 x, uint32 z,
                                 uint32 sizeX, uint32 sizeZ) override;
    void ActivateBodiesInAABB(const AABB& worldBox) override;

    BodyHandle CreateBody(const BodySettings& settings) override;
    void DestroyBody(BodyHandle body) override;
    bool IsBodyValid(BodyHandle body) const override;

    Transform GetBodyTransform(BodyHandle body) const override;
    void SetBodyTransform(BodyHandle body, const Transform& transform, ActivationMode activation) override;

    Vector3 GetLinearVelocity(BodyHandle body) const override;
    void SetLinearVelocity(BodyHandle body, const Vector3& velocity) override;

    Vector3 GetAngularVelocity(BodyHandle body) const override;
    void SetAngularVelocity(BodyHandle body, const Vector3& velocity) override;
    Vector3 GetCenterOfMassPosition(BodyHandle body) const override;
    void AddForce(BodyHandle body, const Vector3& force) override;
    void AddForceAtPosition(BodyHandle body, const Vector3& force, const Vector3& worldPosition) override;
    void AddTorque(BodyHandle body, const Vector3& torque) override;
    void AddImpulse(BodyHandle body, const Vector3& impulse) override;
    void AddImpulseAtPosition(BodyHandle body, const Vector3& impulse, const Vector3& worldPosition) override;

    CharacterHandle CreateCharacter(const CharacterSettings& settings) override;
    void DestroyCharacter(CharacterHandle character) override;
    bool IsCharacterValid(CharacterHandle character) const override;

    Transform GetCharacterTransform(CharacterHandle character) const override;
    void SetCharacterPosition(CharacterHandle character, const Vector3& position) override;

    Vector3 GetCharacterLinearVelocity(CharacterHandle character) const override;
    void SetCharacterLinearVelocity(CharacterHandle character, const Vector3& velocity) override;

    bool IsCharacterGrounded(CharacterHandle character) const override;
    Vector3 GetCharacterGroundNormal(CharacterHandle character) const override;

    bool RayCast(const RayCastQuery& query, RayCastResult& outResult) const override;
    bool RayCastAll(const RayCastQuery& query, std::vector<RayCastResult>& outResults, uint32 maxResults) const override;
    bool BoxOverlap(const BoxOverlapQuery& query, std::vector<OverlapResult>& outResults, uint32 maxResults) const override;

    void SetContactCallback(ContactCallback callback) override;
    void SetTriggerCallback(TriggerCallback callback) override;
    void ProcessEvents() override;

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};

} // namespace GameEngine::Physics

