#include "Physics/PhysicsWorld.h"

#include "Physics/Backend/IPhysicsBackend.h"
#include "Physics/Backend/JoltPhysicsBackend.h"

#include "Logger/Logger.h"

namespace GameEngine::Physics
{
namespace
{
class NullPhysicsBackend final : public IPhysicsBackend
{
public:
    bool Initialize(const PhysicsWorldSettings&) override { return true; }
    void Shutdown() override {}

    void Step(float32, int32) override {}
    void OptimizeBroadphase() override {}

    ShapeHandle CreateShape(const ShapeDefinition&, const Vector3&) override { return {}; }
    void DestroyShape(ShapeHandle) override {}
    bool IsShapeValid(ShapeHandle shape) const override { return shape.IsValid(); }

    // No-op success: a physics-free build must not force callers onto the
    // (equally no-op but more expensive to orchestrate) rebuild path.
    bool UpdateHeightFieldRegion(BodyHandle, ShapeHandle, const float32*, uint32, uint32, uint32, uint32, uint32) override { return true; }
    void ActivateBodiesInAABB(const AABB&) override {}

    BodyHandle CreateBody(const BodySettings&) override { return {}; }
    void DestroyBody(BodyHandle) override {}
    bool IsBodyValid(BodyHandle body) const override { return body.IsValid(); }

    Transform GetBodyTransform(BodyHandle) const override { return {}; }
    void SetBodyTransform(BodyHandle, const Transform&, ActivationMode) override {}

    Vector3 GetLinearVelocity(BodyHandle) const override { return {}; }
    void SetLinearVelocity(BodyHandle, const Vector3&) override {}

    Vector3 GetAngularVelocity(BodyHandle) const override { return {}; }
    void SetAngularVelocity(BodyHandle, const Vector3&) override {}
    Vector3 GetCenterOfMassPosition(BodyHandle) const override { return {}; }
    void AddForce(BodyHandle, const Vector3&) override {}
    void AddForceAtPosition(BodyHandle, const Vector3&, const Vector3&) override {}
    void AddTorque(BodyHandle, const Vector3&) override {}
    void AddImpulse(BodyHandle, const Vector3&) override {}
    void AddImpulseAtPosition(BodyHandle, const Vector3&, const Vector3&) override {}

    CharacterHandle CreateCharacter(const CharacterSettings&) override { return {}; }
    void DestroyCharacter(CharacterHandle) override {}
    bool IsCharacterValid(CharacterHandle character) const override { return character.IsValid(); }

    Transform GetCharacterTransform(CharacterHandle) const override { return {}; }
    void SetCharacterPosition(CharacterHandle, const Vector3&) override {}

    Vector3 GetCharacterLinearVelocity(CharacterHandle) const override { return {}; }
    void SetCharacterLinearVelocity(CharacterHandle, const Vector3&) override {}

    bool IsCharacterGrounded(CharacterHandle) const override { return false; }
    Vector3 GetCharacterGroundNormal(CharacterHandle) const override { return Vector3(0.0f, 1.0f, 0.0f); }

    bool RayCast(const RayCastQuery&, RayCastResult&) const override { return false; }
    bool RayCastAll(const RayCastQuery&, std::vector<RayCastResult>&, uint32) const override { return false; }
    bool BoxOverlap(const BoxOverlapQuery&, std::vector<OverlapResult>&, uint32) const override { return false; }

    void SetContactCallback(IPhysicsBackend::ContactCallback) override {}
    void SetTriggerCallback(IPhysicsBackend::TriggerCallback) override {}
    void ProcessEvents() override {}
};

static std::unique_ptr<IPhysicsBackend> CreateDefaultBackend()
{
#if GE_PHYSICS_BACKEND_JOLT
    return std::make_unique<JoltPhysicsBackend>();
#else
    return std::make_unique<NullPhysicsBackend>();
#endif
}
} // namespace

PhysicsWorld::PhysicsWorld(const PhysicsWorldSettings& settings)
{
    m_Backend = CreateDefaultBackend();
    if (!m_Backend->Initialize(settings))
    {
        Logger::Log::Error("[Physics] Backend init failed; physics disabled");
        m_Backend = std::make_unique<NullPhysicsBackend>();
        (void)m_Backend->Initialize(settings);
    }

    UpdateBackendEventCallbacks();
}

PhysicsWorld::~PhysicsWorld()
{
    if (m_Backend)
    {
        m_Backend->Shutdown();
    }
}

PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

void PhysicsWorld::Step(float32 deltaTime, int32 collisionSteps)
{
    if (m_Backend)
    {
        m_AccumContactPairs = 0;
        m_Backend->Step(deltaTime, collisionSteps);
        m_Backend->ProcessEvents();
        m_LastContactPairCount = m_AccumContactPairs;
    }
}

void PhysicsWorld::OptimizeBroadphase()
{
    if (m_Backend)
        m_Backend->OptimizeBroadphase();
}

ShapeHandle PhysicsWorld::CreateShape(const ShapeDefinition& definition, const Vector3& bakedScale)
{
    return m_Backend ? m_Backend->CreateShape(definition, bakedScale) : ShapeHandle{};
}

void PhysicsWorld::DestroyShape(ShapeHandle shape)
{
    if (m_Backend)
        m_Backend->DestroyShape(shape);
}

bool PhysicsWorld::IsShapeValid(ShapeHandle shape) const
{
    return m_Backend ? m_Backend->IsShapeValid(shape) : false;
}

bool PhysicsWorld::UpdateHeightFieldRegion(BodyHandle body, ShapeHandle shape,
                                           const float32* allSamples, uint32 gridN,
                                           uint32 x, uint32 z,
                                           uint32 sizeX, uint32 sizeZ)
{
    return m_Backend ? m_Backend->UpdateHeightFieldRegion(body, shape, allSamples, gridN, x, z, sizeX, sizeZ) : false;
}

void PhysicsWorld::ActivateBodiesInAABB(const AABB& worldBox)
{
    if (m_Backend)
        m_Backend->ActivateBodiesInAABB(worldBox);
}

BodyHandle PhysicsWorld::CreateBody(const BodySettings& settings)
{
    if (!m_Backend)
        return BodyHandle{};
    BodyHandle h = m_Backend->CreateBody(settings);
    if (h.IsValid())
        ++m_BodyCount;
    return h;
}

void PhysicsWorld::DestroyBody(BodyHandle body)
{
    if (!m_Backend)
        return;
    if (body.IsValid() && m_Backend->IsBodyValid(body) && m_BodyCount > 0)
        --m_BodyCount;
    m_Backend->DestroyBody(body);
}

bool PhysicsWorld::IsBodyValid(BodyHandle body) const
{
    return m_Backend ? m_Backend->IsBodyValid(body) : false;
}

Transform PhysicsWorld::GetBodyTransform(BodyHandle body) const
{
    return m_Backend ? m_Backend->GetBodyTransform(body) : Transform{};
}

void PhysicsWorld::SetBodyTransform(BodyHandle body, const Transform& transform, ActivationMode activation)
{
    if (m_Backend)
        m_Backend->SetBodyTransform(body, transform, activation);
}

Vector3 PhysicsWorld::GetLinearVelocity(BodyHandle body) const
{
    return m_Backend ? m_Backend->GetLinearVelocity(body) : Vector3{};
}

void PhysicsWorld::SetLinearVelocity(BodyHandle body, const Vector3& velocity)
{
    if (m_Backend)
        m_Backend->SetLinearVelocity(body, velocity);
}

Vector3 PhysicsWorld::GetAngularVelocity(BodyHandle body) const
{
    return m_Backend ? m_Backend->GetAngularVelocity(body) : Vector3{};
}

void PhysicsWorld::SetAngularVelocity(BodyHandle body, const Vector3& velocity)
{
    if (m_Backend)
        m_Backend->SetAngularVelocity(body, velocity);
}

Vector3 PhysicsWorld::GetCenterOfMassPosition(BodyHandle body) const
{
    return m_Backend ? m_Backend->GetCenterOfMassPosition(body) : Vector3{};
}

void PhysicsWorld::AddForce(BodyHandle body, const Vector3& force)
{
    if (m_Backend)
        m_Backend->AddForce(body, force);
}

void PhysicsWorld::AddForceAtPosition(BodyHandle body, const Vector3& force, const Vector3& worldPosition)
{
    if (m_Backend)
        m_Backend->AddForceAtPosition(body, force, worldPosition);
}

void PhysicsWorld::AddTorque(BodyHandle body, const Vector3& torque)
{
    if (m_Backend)
        m_Backend->AddTorque(body, torque);
}

void PhysicsWorld::AddImpulse(BodyHandle body, const Vector3& impulse)
{
    if (m_Backend)
        m_Backend->AddImpulse(body, impulse);
}

void PhysicsWorld::AddImpulseAtPosition(BodyHandle body, const Vector3& impulse, const Vector3& worldPosition)
{
    if (m_Backend)
        m_Backend->AddImpulseAtPosition(body, impulse, worldPosition);
}

CharacterHandle PhysicsWorld::CreateCharacter(const CharacterSettings& settings)
{
    return m_Backend ? m_Backend->CreateCharacter(settings) : CharacterHandle{};
}

void PhysicsWorld::DestroyCharacter(CharacterHandle character)
{
    if (m_Backend)
        m_Backend->DestroyCharacter(character);
}

bool PhysicsWorld::IsCharacterValid(CharacterHandle character) const
{
    return m_Backend ? m_Backend->IsCharacterValid(character) : false;
}

Transform PhysicsWorld::GetCharacterTransform(CharacterHandle character) const
{
    return m_Backend ? m_Backend->GetCharacterTransform(character) : Transform{};
}

void PhysicsWorld::SetCharacterPosition(CharacterHandle character, const Vector3& position)
{
    if (m_Backend)
        m_Backend->SetCharacterPosition(character, position);
}

Vector3 PhysicsWorld::GetCharacterLinearVelocity(CharacterHandle character) const
{
    return m_Backend ? m_Backend->GetCharacterLinearVelocity(character) : Vector3{};
}

void PhysicsWorld::SetCharacterLinearVelocity(CharacterHandle character, const Vector3& velocity)
{
    if (m_Backend)
        m_Backend->SetCharacterLinearVelocity(character, velocity);
}

bool PhysicsWorld::IsCharacterGrounded(CharacterHandle character) const
{
    return m_Backend ? m_Backend->IsCharacterGrounded(character) : false;
}

Vector3 PhysicsWorld::GetCharacterGroundNormal(CharacterHandle character) const
{
    return m_Backend ? m_Backend->GetCharacterGroundNormal(character) : Vector3(0.0f, 1.0f, 0.0f);
}

bool PhysicsWorld::RayCast(const RayCastQuery& query, RayCastResult& outResult) const
{
    return m_Backend ? m_Backend->RayCast(query, outResult) : false;
}

bool PhysicsWorld::RayCastAll(const RayCastQuery& query, std::vector<RayCastResult>& outResults, uint32 maxResults) const
{
    return m_Backend ? m_Backend->RayCastAll(query, outResults, maxResults) : false;
}

bool PhysicsWorld::BoxOverlap(const BoxOverlapQuery& query, std::vector<OverlapResult>& outResults, uint32 maxResults) const
{
    if (!m_Backend)
        return false;
    return m_Backend->BoxOverlap(query, outResults, maxResults);
}

void PhysicsWorld::SetContactCallback(ContactCallback callback)
{
    ClearContactListeners();
    if (callback)
        (void)AddContactListener(std::move(callback));
}

void PhysicsWorld::SetTriggerCallback(TriggerCallback callback)
{
    ClearTriggerListeners();
    if (callback)
        (void)AddTriggerListener(std::move(callback));
}

void PhysicsWorld::ProcessEvents()
{
    if (m_Backend)
        m_Backend->ProcessEvents();
}

PhysicsWorld::ListenerId PhysicsWorld::AddContactListener(ContactCallback callback)
{
    if (!callback)
        return 0;

    const ListenerId id = m_NextListenerId++;
    m_ContactListeners.emplace_back(id, std::move(callback));
    UpdateBackendEventCallbacks();
    return id;
}

PhysicsWorld::ListenerId PhysicsWorld::AddTriggerListener(TriggerCallback callback)
{
    if (!callback)
        return 0;

    const ListenerId id = m_NextListenerId++;
    m_TriggerListeners.emplace_back(id, std::move(callback));
    UpdateBackendEventCallbacks();
    return id;
}

void PhysicsWorld::RemoveContactListener(ListenerId id)
{
    if (id == 0)
        return;
    const auto it = std::remove_if(m_ContactListeners.begin(),
                                   m_ContactListeners.end(),
                                   [&](const auto& p)
                                   {
                                       return p.first == id;
                                   });
    if (it != m_ContactListeners.end())
    {
        m_ContactListeners.erase(it, m_ContactListeners.end());
        UpdateBackendEventCallbacks();
    }
}

void PhysicsWorld::RemoveTriggerListener(ListenerId id)
{
    if (id == 0)
        return;
    const auto it = std::remove_if(m_TriggerListeners.begin(),
                                   m_TriggerListeners.end(),
                                   [&](const auto& p)
                                   {
                                       return p.first == id;
                                   });
    if (it != m_TriggerListeners.end())
    {
        m_TriggerListeners.erase(it, m_TriggerListeners.end());
        UpdateBackendEventCallbacks();
    }
}

void PhysicsWorld::ClearContactListeners()
{
    if (!m_ContactListeners.empty())
    {
        m_ContactListeners.clear();
        UpdateBackendEventCallbacks();
    }
}

void PhysicsWorld::ClearTriggerListeners()
{
    if (!m_TriggerListeners.empty())
    {
        m_TriggerListeners.clear();
        UpdateBackendEventCallbacks();
    }
}

void PhysicsWorld::UpdateBackendEventCallbacks()
{
    if (!m_Backend)
        return;

    // Always install a contact callback so we can count contact pairs per step,
    // even when no user listener is registered.
    m_Backend->SetContactCallback([this](const ContactEvent& e)
                                  {
                                      ++m_AccumContactPairs;
                                      for (auto& it : m_ContactListeners)
                                          it.second(e);
                                  });

    if (m_TriggerListeners.empty())
    {
        m_Backend->SetTriggerCallback({});
    }
    else
    {
        m_Backend->SetTriggerCallback([this](const TriggerEvent& e)
                                      {
                                          for (auto& it : m_TriggerListeners)
                                              it.second(e);
                                      });
    }
}

} // namespace GameEngine::Physics

